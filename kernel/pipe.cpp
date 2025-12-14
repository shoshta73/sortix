/*
 * Copyright (c) 2011-2017, 2021, 2025 Jonas 'Sortie' Termansen.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * pipe.cpp
 * A device with a writing end and a reading end.
 */

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include <sortix/fcntl.h>
// TODO: After releasing Sortix 1.1, remove this bootstrap compatibility.
#ifndef IOV_MAX
#include <sortix/limits.h>
#endif
#include <sortix/poll.h>
#include <sortix/signal.h>
#include <sortix/stat.h>

#include <sortix/kernel/copy.h>
#include <sortix/kernel/descriptor.h>
#include <sortix/kernel/dtable.h>
#include <sortix/kernel/inode.h>
#include <sortix/kernel/interlock.h>
#include <sortix/kernel/ioctx.h>
#include <sortix/kernel/kernel.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/pipe.h>
#include <sortix/kernel/poll.h>
#include <sortix/kernel/process.h>
#include <sortix/kernel/refcount.h>
#include <sortix/kernel/scheduler.h>
#include <sortix/kernel/signal.h>
#include <sortix/kernel/syscall.h>
#include <sortix/kernel/thread.h>
#include <sortix/kernel/vnode.h>

namespace Sortix {

// A segment contains an optional leading ancillary data buffer and then a
// normal data buffer.
struct segment_header
{
	size_t ancillary;
	size_t normal;
};

// A pipe communication channel in one direction.
//
// The pipe uses a ring buffer containing segments. Each segment is stored as
// its header, followed by an optional ancillary data buffer (cmsg(3)) and a
// normal data buffer. Writing data will append to the last segment, if any, if
// it has a compatible type and isn't finished.
class PipeChannel
{
public:
	PipeChannel(uint8_t* buffer, size_t buffer_size);
	~PipeChannel();
	void CloseReading();
	void CloseWriting();
	bool GetSIGPIPEDelivery();
	void SetSIGPIPEDelivery(bool deliver_sigpipe);
	size_t ReadSize();
	size_t WriteSize();
	bool ReadResize(size_t new_size);
	bool WriteResize(size_t new_size);
	bool WriteBuffer(bool (*copy_from_src)(void*, const void*, size_t),
	                 const void* src_ptr,
	                 size_t amount,
	                 size_t position);
	bool ReadBuffer(bool (*copy_to_dest)(void*, const void*, size_t),
	                void* dest_ptr,
	                size_t amount,
	                size_t position);
	bool Enqueue(bool (*copy_from_src)(void* dest, const void* src, size_t n),
	             const void* src,
	             size_t amount,
	             bool is_ancillary);
	void Unenqueue(bool (*copy_to_dest)(void*, const void*, size_t),
		           void* dest_ptr,
		           size_t amount,
		           size_t saved_last_header_position);
	bool Dequeue(bool (*copy_to_dest)(void* dest, const void* src, size_t n),
	             void* dest,
	             size_t amount,
	             bool peek,
	             size_t peek_offset);
	bool pass();
	void unpass();
	ssize_t readv(ioctx_t* ctx, const struct iovec* iov, int iovcnt);
	ssize_t recv(ioctx_t* ctx, uint8_t* buf, size_t count, int flags);
	ssize_t recvmsg(ioctx_t* ctx, struct msghdr* msg, int flags);
	ssize_t recvmsg_internal(ioctx_t* ctx, struct msghdr* msg, int flags);
	ssize_t send(ioctx_t* ctx, const uint8_t* buf, size_t count, int flags);
	ssize_t sendmsg(ioctx_t* ctx, const struct msghdr* msg, int flags);
	ssize_t sendmsg_internal(ioctx_t* ctx, const struct msghdr* msg, int flags);
	ssize_t writev(ioctx_t* ctx, const struct iovec* iov, int iovcnt);
	int read_poll(ioctx_t* ctx, PollNode* node);
	int write_poll(ioctx_t* ctx, PollNode* node);
	int sockatmark(ioctx_t* ctx);

private:
	short ReadPollEventStatus();
	short WritePollEventStatus();

private:
	PollChannel read_poll_channel;
	PollChannel write_poll_channel;
	// Lock protecting the pipe's data structrues.
	kthread_mutex_t pipe_lock;
	// Lock protecting passing_count and passing_count only.
	kthread_mutex_t pass_lock;
	// Condition for when readers can wake up.
	kthread_cond_t read_cond;
	// Condition for when writers can wake up.
	kthread_cond_t write_cond;
	// The id of a thread that has plegded to write and can be yielded to.
	uintptr_t sender_system_tid;
	// The id of a thread that has plegded to receive and can be yielded to.
	uintptr_t receiver_system_tid;
	// The ring buffer containing segments.
	uint8_t* buffer;
	// The offset in the ring buffer where data begins.
	size_t buffer_offset;
	// The amount of data used after buffer_offset in the ring buffer.
	size_t buffer_used;
	// The size of the ring buffer.
	size_t buffer_size;
	// The position of the last header in the ring buffer, a new header is
	// required if last_header_position == buffer_used.
	size_t last_header_position;
	// The buffer size as far as the reader is concerned (the writer decides).
	size_t pretended_read_buffer_size;
	// This many readers have pledged to write.
	size_t pledged_write;
	// This many writers have pledged to read.
	size_t pledged_read;
	// How many file descriptors are passed on this socket.
	size_t passing_count;
	// How many times this socket itself is being passed.
	size_t passed_count;
	// Atomically incremented on read/write close, number two does the delete.
	unsigned long closers;
	// Whether anyone still has this channel open for reading.
	bool any_reading;
	// Whether anyone still has this channel open for writing.
	bool any_writing;
	// Whether writing with no readers will send SIGPIPE.
	bool is_sigpipe_enabled;

};

PipeChannel::PipeChannel(uint8_t* buffer, size_t buffer_size)
{
	pipe_lock = KTHREAD_MUTEX_INITIALIZER;
	pass_lock = KTHREAD_MUTEX_INITIALIZER;
	read_cond = KTHREAD_COND_INITIALIZER;
	write_cond = KTHREAD_COND_INITIALIZER;
	sender_system_tid = 0;
	receiver_system_tid = 0;
	this->buffer = buffer;
	buffer_offset = 0;
	buffer_used = 0;
	this->buffer_size = buffer_size;
	last_header_position = 0;
	pretended_read_buffer_size = buffer_size;
	pledged_write = 0;
	pledged_read = 0;
	passing_count = 0;
	passed_count = 0;
	closers = 0;
	any_reading = true;
	any_writing = true;
	is_sigpipe_enabled = true;
}

PipeChannel::~PipeChannel()
{
	// Drain the ring buffer contents and deference passed file descriptors.
	while ( buffer_used )
	{
		struct segment_header header;
		assert(sizeof(header) <= buffer_used);
		ReadBuffer(CopyToKernel, &header, sizeof(header), 0);
		while ( 0 < header.ancillary )
		{
			struct cmsghdr cmsg;
			assert(sizeof(cmsg) <= header.ancillary);
			assert(sizeof(header) <= buffer_used);
			Dequeue(CopyToKernel, &cmsg, sizeof(cmsg), false, 0);
			header.ancillary -= sizeof(cmsg);
			size_t data = cmsg.cmsg_len - sizeof(struct cmsghdr);
			if ( cmsg.cmsg_level == SOL_SOCKET && cmsg.cmsg_type == SCM_RIGHTS )
			{
				size_t fds = data / sizeof(int);
				for ( size_t i = 0; i < fds; i++ )
				{
					uintptr_t ptr;
					assert(sizeof(header) <= buffer_used);
					Dequeue(CopyToKernel, &ptr, sizeof(ptr), false, 0);
					header.ancillary -= sizeof(ptr);
					Ref<Descriptor> desc;
					desc.Import(ptr);
					passing_count--;
				}
			}
			else
			{
				assert(sizeof(header) <= buffer_used);
				Dequeue(NULL, NULL, data, false, 0);
				header.ancillary -= data;
			}
		}
		if ( header.normal )
		{
			assert(sizeof(header) <= buffer_used);
			Dequeue(NULL, NULL, header.normal, false, 0);
			header.normal -= header.normal;
		}
	}
	assert(!passed_count && !passing_count);
	delete[] buffer;
}

void PipeChannel::CloseReading()
{
	kthread_mutex_lock(&pipe_lock);
	any_reading = false;
	kthread_cond_broadcast(&write_cond);
	read_poll_channel.Signal(ReadPollEventStatus());
	write_poll_channel.Signal(WritePollEventStatus());
	kthread_mutex_unlock(&pipe_lock);
	unsigned long count = InterlockedIncrement(&closers).n;
	if ( count == 2 )
		delete this;
}

void PipeChannel::CloseWriting()
{
	kthread_mutex_lock(&pipe_lock);
	any_writing = false;
	kthread_cond_broadcast(&read_cond);
	read_poll_channel.Signal(ReadPollEventStatus());
	write_poll_channel.Signal(WritePollEventStatus());
	kthread_mutex_unlock(&pipe_lock);
	unsigned long count = InterlockedIncrement(&closers).n;
	if ( count == 2 )
		delete this;
}

bool PipeChannel::WriteBuffer(bool (*copy_from_src)(void*, const void*, size_t),
                              const void* src_ptr,
                              size_t amount,
                              size_t position)
{
	size_t write_offset = (buffer_offset + position) % buffer_size;
	size_t linear = buffer_size - write_offset;
	size_t first = linear < amount ? linear : amount;
	const unsigned char* src = (const unsigned char*) src_ptr;
	if ( !copy_from_src(buffer + write_offset, src, first) )
		return false;
	if ( first < amount &&
	     !copy_from_src(buffer, src + first, amount - first) )
		return false;
	return true;
}

bool PipeChannel::ReadBuffer(bool (*copy_to_dest)(void*, const void*, size_t),
                             void* dest_ptr,
                             size_t amount,
                             size_t position)
{
	size_t offset = (buffer_offset + position) % buffer_size;
	size_t linear = buffer_size - offset;
	size_t first = linear < amount ? linear : amount;
	unsigned char* dest = (unsigned char*) dest_ptr;
	if ( !copy_to_dest(dest, buffer + offset, first) )
		return false;
	if ( first < amount &&
	     !copy_to_dest(dest + first, buffer, amount - first) )
		return false;
	return true;
}

bool PipeChannel::Enqueue(bool (*copy_from_src)(void*, const void*, size_t),
                          const void* src_ptr,
                          size_t amount,
                          bool is_ancillary)
{
	struct segment_header header = { 0, 0 };
	assert(last_header_position <= buffer_used);
	// Try to unify with a previous segment header if any or make a new one.
	bool has_header = buffer_used - last_header_position;
	if ( has_header )
		ReadBuffer(CopyToKernel, &header, sizeof(header), last_header_position);
	if ( is_ancillary )
	{
		// Ancillary data must be at the start of the segment.
		if ( header.normal )
		{
			last_header_position = buffer_used;
			assert(last_header_position <= buffer_used);
			has_header = false;
			memset(&header, 0, sizeof(header));
		}
		header.ancillary += amount;
	}
	else
		header.normal += amount;
	assert(has_header || sizeof(header) <= buffer_size - buffer_used);
	size_t header_size = has_header ? 0 : sizeof(header);
	size_t position = buffer_used + header_size;
	assert(amount <= buffer_size - position);
	if ( !WriteBuffer(copy_from_src, src_ptr, amount, position) )
		return false;
	WriteBuffer(CopyFromKernel, &header, sizeof(header), last_header_position);
	buffer_used += amount + header_size;
	kthread_cond_broadcast(&read_cond);
	read_poll_channel.Signal(ReadPollEventStatus());
	write_poll_channel.Signal(WritePollEventStatus());
	return true;
}

void PipeChannel::Unenqueue(bool (*copy_to_dest)(void*, const void*, size_t),
                            void* dest_ptr,
                            size_t amount,
                            size_t saved_last_header_position)
{
	struct segment_header header;
	assert(sizeof(header) <= buffer_used);
	assert(last_header_position <= buffer_used);
	// Remove the data from the last segment header.
	ReadBuffer(CopyToKernel, &header, sizeof(header), last_header_position);
	if ( header.ancillary )
	{
		assert(amount <= header.ancillary);
		header.ancillary -= amount;
	}
	else
	{
		assert(amount <= header.normal);
		header.normal -= amount;
	}
	assert(amount <= buffer_used - sizeof(header));
	if ( copy_to_dest )
		ReadBuffer(CopyToKernel, dest_ptr, amount, buffer_used - amount);
	buffer_used -= amount;
	// Remove the last segment header if it becomes empty.
	if ( header.ancillary || header.normal )
		WriteBuffer(CopyFromKernel, &header, sizeof(header),
		            last_header_position);
	else
	{
		buffer_used -= sizeof(header);
		last_header_position = saved_last_header_position;
	}
	kthread_cond_broadcast(&write_cond);
	read_poll_channel.Signal(ReadPollEventStatus());
	write_poll_channel.Signal(WritePollEventStatus());
}

bool PipeChannel::Dequeue(bool (*copy_to_dest)(void*, const void*, size_t),
                          void* dest_ptr,
                          size_t amount,
                          bool peek,
                          size_t peek_offset)
{
	assert(peek || !peek_offset);
	struct segment_header header;
	assert(sizeof(header) <= buffer_used);
	assert(last_header_position <= buffer_used);
	assert(peek_offset <= buffer_used - sizeof(header));
	if ( copy_to_dest &&
	     !ReadBuffer(copy_to_dest, dest_ptr, amount,
	                 sizeof(header) + peek_offset) )
		return false;
	if ( !peek )
	{
		// Remove the data from the segment.
		ReadBuffer(CopyToKernel, &header, sizeof(header), 0);
		if ( header.ancillary )
		{
			assert(amount <= header.ancillary);
			header.ancillary -= amount;
		}
		else
		{
			assert(amount <= header.normal);
			header.normal -= amount;
		}
		// Shift the segment header.
		if ( header.ancillary || header.normal )
			WriteBuffer(CopyFromKernel, &header, sizeof(header), amount);
		else
			amount += sizeof(header);
		buffer_offset = (buffer_offset + amount) % buffer_size;
		buffer_used -= amount;
		if ( amount <= last_header_position )
			last_header_position -= amount;
		else
			last_header_position = 0;
		kthread_cond_broadcast(&write_cond);
		read_poll_channel.Signal(ReadPollEventStatus());
		write_poll_channel.Signal(WritePollEventStatus());
		// Realign the buffer if it becomes empty.
		if ( !buffer_used )
		{
			buffer_offset = 0;
			last_header_position = 0;
		}
	}
	return true;
}

bool PipeChannel::pass()
{
	// The Unix socket is being passed on another socket.
	ScopedLock lock(&pass_lock);
	// If this socket has descriptors passed on it, then refuse to pass it over
	// another socket to avoid reference count cycles.
	if ( passing_count )
		return false;
	passed_count++;
	return true;
}

void PipeChannel::unpass()
{
	// The Unix socket is no longer being passed on another socket.
	ScopedLock lock(&pass_lock);
	assert(passed_count);
	assert(!passing_count);
	passed_count--;
}

ssize_t PipeChannel::recv(ioctx_t* ctx, uint8_t* buf, size_t count, int flags)
{
	struct iovec iov;
	memset(&iov, 0, sizeof(iov));
	iov.iov_base = (void*) buf;
	iov.iov_len = count;
	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	return recvmsg_internal(ctx, &msg, flags);
}

ssize_t PipeChannel::readv(ioctx_t* ctx, const struct iovec* iov, int iovcnt)
{
	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = (struct iovec*) iov;
	msg.msg_iovlen = iovcnt;
	return recvmsg_internal(ctx, &msg, 0);
}

ssize_t PipeChannel::recvmsg(ioctx_t* ctx, struct msghdr* msg_ptr, int flags)
{
	// recvmsg can only be called through Unix sockets and not regular pipes.
	struct msghdr msg;
	if ( !ctx->copy_from_src(&msg, msg_ptr, sizeof(msg)) )
		return -1;
	if ( msg.msg_iovlen < 0 || IOV_MAX < msg.msg_iovlen )
		return errno = EINVAL, -1;
	size_t iov_size = msg.msg_iovlen * sizeof(struct iovec);
	struct iovec* iov = new struct iovec[msg.msg_iovlen];
	if ( !iov )
		return -1;
	struct iovec* user_iov = msg.msg_iov;
	if ( !ctx->copy_from_src(iov, user_iov, iov_size) )
		return delete[] iov, -1;
	msg.msg_iov = iov;
	ssize_t result = recvmsg_internal(ctx, &msg, flags);
	msg.msg_iov = user_iov;
	delete[] iov;
	if ( !ctx->copy_to_dest(msg_ptr, &msg, sizeof(msg)) )
		return -1;
	return result;
}

ssize_t PipeChannel::recvmsg_internal(ioctx_t* ctx,
                                      struct msghdr* msg,
                                      int flags)
{
	msg->msg_flags = 0;
	if ( flags & ~(MSG_PEEK | MSG_WAITALL | MSG_CMSG_CLOEXEC |
	               MSG_CMSG_CLOFORK) )
		return errno = EINVAL, -1;
	if ( SSIZE_MAX < TruncateIOVec(msg->msg_iov, msg->msg_iovlen, SSIZE_MAX) )
		return errno = EINVAL, -1;
	Thread* this_thread = CurrentThread();
	this_thread->yield_to_tid = sender_system_tid;
	ScopedLockSignal lock(&pipe_lock);
	if ( !lock.IsAcquired() )
		return errno = EINTR, -1;
	receiver_system_tid = this_thread->system_tid;
	// Receive the first segment only in the ring buffer and wait for it.
	while ( !buffer_used )
	{
		// EOF if there are no writers left.
		if ( !any_writing )
		{
			msg->msg_controllen = 0;
			return 0;
		}
		// If a thread has plegded to send, yield to it.
		this_thread->yield_to_tid = sender_system_tid;
		if ( pledged_write )
		{
			pledged_read++;
			kthread_mutex_unlock(&pipe_lock);
			kthread_yield();
			kthread_mutex_lock(&pipe_lock);
			pledged_read--;
			continue;
		}
		// Wait for data to arrive in the ring buffer.
		if ( ctx->dflags & O_NONBLOCK )
			return errno = EWOULDBLOCK, -1;
		pledged_read++;
		bool interrupted = !kthread_cond_wait_signal(&read_cond, &pipe_lock);
		pledged_read--;
		if ( interrupted )
			return errno = EINTR, -1;
	}
	// Peeking iterates through the segment without removing data.
	bool peek = flags & MSG_PEEK;
	size_t peeked = 0;
	// The remaining user-space control data (if any), incremented as it is
	// being written to, and the final values are used to tell the caller how
	// much control data was written.
	unsigned char* control = (unsigned char*) msg->msg_control;
	size_t control_length = msg->msg_controllen;
	// Whether reading the control data failed, and whether it was due to a
	// harmless truncation (in which case the system call doesn't fail).
	bool failed = false;
	bool truncated = false;
	// Read the ancillary data, if any, and discard it if the caller didn't
	// expect control data or if reading the control the data failed.
	struct segment_header header;
	assert(sizeof(header) <= buffer_used);
	ReadBuffer(CopyToKernel, &header, sizeof(header), 0);
	while ( 0 < header.ancillary )
	{
		// Read the nested cmsg header and find the control message type.
		struct cmsghdr cmsg;
		assert(sizeof(cmsg) <= header.ancillary);
		Dequeue(CopyToKernel, &cmsg, sizeof(cmsg), peek, peeked);
		if ( peek )
			peeked += sizeof(cmsg);
		header.ancillary -= sizeof(cmsg);
		// Determine how much data the caller gets after any truncation and
		// correct the control message header given to the caller.
		size_t data = cmsg.cmsg_len - sizeof(struct cmsghdr);
		size_t truncated_data = data;
		if ( cmsg.cmsg_level == SOL_SOCKET && cmsg.cmsg_type == SCM_RIGHTS )
			truncated_data = (data / sizeof(int)) * sizeof(int);
		size_t available_data =
			sizeof(cmsg) <= control_length ? control_length - sizeof(cmsg) : 0;
		if ( available_data < truncated_data )
			truncated_data = available_data;
		cmsg.cmsg_len = sizeof(struct cmsghdr) + truncated_data;
		// Copy the control message header to the caller.
		if ( sizeof(cmsg) <= control_length &&
		     ctx->copy_to_dest(control, &cmsg, sizeof(cmsg)) )
		{
			control += sizeof(cmsg);
			control_length -= sizeof(cmsg);
		}
		else if ( !failed )
			truncated = failed = true;
		// Passed file descriptors needs to be unserialized and allocated in the
		// process file descriptor table.
		if ( cmsg.cmsg_level == SOL_SOCKET && cmsg.cmsg_type == SCM_RIGHTS )
		{
			int fdflags = 0;
			if ( flags & MSG_CMSG_CLOEXEC )
				fdflags |= FD_CLOEXEC;
			if ( flags & MSG_CMSG_CLOFORK )
				fdflags |= FD_CLOFORK;
			Process* process = CurrentProcess();
			Ref<DescriptorTable> dtable = process->GetDTable();
			size_t fds = data / sizeof(int);
			// Preallocate the needed file descriptor slots so it doesn't fail
			// later and it becomes possible to do an all-or-nothing operation.
			int reservation = 0;
			if ( !failed &&
			     (INT_MAX < fds || !dtable->Reserve(fds, &reservation)) )
			{
				errno = EMSGSIZE;
				failed = true;
			}
			for ( size_t i = 0; i < fds; i++ )
			{
				uintptr_t ptr;
				Dequeue(CopyToKernel, &ptr, sizeof(ptr), peek, peeked);
				if ( peek )
					peeked += sizeof(ptr);
				header.ancillary -= sizeof(ptr);
				Ref<Descriptor> desc;
				desc.Import(ptr);
				if ( peek )
					desc.Export();
				else
				{
					desc->unpass();
					kthread_mutex_lock(&pass_lock);
					passing_count--;
					kthread_mutex_unlock(&pass_lock);
				}
				if ( failed )
					continue;
				if ( control_length < sizeof(int) )
				{
					truncated = failed = true;
					continue;
				}
				int fd = dtable->Allocate(desc, fdflags, 0, &reservation);
				assert(0 <= fd);
				if ( !ctx->copy_to_dest(control, &fd, sizeof(fd)) )
				{
					// The file descriptor leaks because the caller was faulty.
					failed = true;
					continue;
				}
				control += sizeof(fd);
				control_length -= sizeof(fd);
			}
			dtable->Unreserve(&reservation);
		}
		else
		{
			// Transfer the control data directly to the caller and truncate it
			// if it was too long.
			size_t amount = control_length < data ? control_length : data;
			if ( failed ||
			     !Dequeue(ctx->copy_to_dest, control, amount, peek, peeked) )
			{
				failed = true;
				amount = data;
				Dequeue(NULL, NULL, amount, peek, peeked);
			}
			if ( !failed && amount < data )
				failed = truncated = true;
			if ( peek )
				peeked += data;
			header.ancillary -= data;
		}
		if ( !failed )
		{
			// Zero the alignment padding between the caller's control messages.
			size_t misaligned = CMSG_ALIGN(data) - data;
			if ( control_length <= misaligned &&
			     ctx->zero_dest(control, misaligned) )
			{
				control += misaligned;
				control_length -= misaligned;
			}
			else if ( header.ancillary )
				truncated = failed = true;
		}
	}
	// Fail if the control data couldn't be read and was discarded. It's not an
	// error if the control data was simply truncated.
	if ( !truncated && failed )
		return -1;
	msg->msg_controllen -= control_length;
	if ( truncated )
		msg->msg_flags |= MSG_CTRUNC;
	// Read the regular data in the segment.
	ssize_t so_far = 0;
	int iov_i = 0;
	size_t iov_offset = 0;
	while ( iov_i < msg->msg_iovlen && so_far < SSIZE_MAX )
	{
		size_t maxcount = SSIZE_MAX - so_far;
		struct iovec* iov = &msg->msg_iov[iov_i];
		uint8_t* buf = (uint8_t*) iov->iov_base + iov_offset;
		size_t count = iov->iov_len - iov_offset;
		if ( maxcount < count )
			count = maxcount;
		if ( count == 0 )
		{
			iov_i++;
			iov_offset = 0;
			continue;
		}
		receiver_system_tid = this_thread->system_tid;
		// If we depleted the segment, try to wait for additional data if a
		// writer has committed to writing more data or if MSG_WAITALL wants
		// all its data.
		while ( !header.normal && any_writing )
		{
			if ( peek )
				return so_far;
			// If the segment is empty but the buffer is non-empty, then there
			// already is another segment which shouldn't be received yet.
			if ( buffer_used )
				return so_far;
			this_thread->yield_to_tid = sender_system_tid;
			if ( pledged_write )
			{
				// Yield to the thread that has pledged to write more data.
				pledged_read++;
				kthread_mutex_unlock(&pipe_lock);
				kthread_yield();
				kthread_mutex_lock(&pipe_lock);
				pledged_read--;
			}
			else
			{
				// Wait for the remaining data to arrive if MSG_WAITALL.
				if ( !(flags & MSG_WAITALL) && so_far )
					return so_far;
				if ( ctx->dflags & O_NONBLOCK )
					return errno = EWOULDBLOCK, -1;
				pledged_read++;
				bool interrupted =
					!kthread_cond_wait_signal(&read_cond, &pipe_lock);
				pledged_read--;
				if ( interrupted )
					return so_far ? so_far : (errno = EINTR, -1);
			}
			if ( !buffer_used )
				continue;
			// Reread the header as another thread has updated it.
			assert(sizeof(header) <= buffer_used);
			ReadBuffer(CopyToKernel, &header, sizeof(header), 0);
			// Don't cross into another segment with ancillary data.
			if ( header.ancillary )
				return so_far;
		}
		// EOF if there are no writers left.
		if ( !header.normal && !any_writing )
			break;
		// Transfer the normal data to the caller.
		size_t amount = count;
		if ( header.normal < amount )
			amount = header.normal;
		if ( !Dequeue(ctx->copy_to_dest, buf, amount, peek, peeked) )
			return so_far ? so_far : -1;
		so_far += amount;
		if ( peek )
			peeked += amount;
		iov_offset += amount;
		header.normal -= amount;
		if ( iov_offset == iov->iov_len )
		{
			iov_i++;
			iov_offset = 0;
		}
	}
	return so_far;
}

ssize_t PipeChannel::send(ioctx_t* ctx, const uint8_t* buf, size_t count,
                          int flags)
{
	struct iovec iov;
	memset(&iov, 0, sizeof(iov));
	iov.iov_base = (void*) buf;
	iov.iov_len = count;
	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	return sendmsg_internal(ctx, &msg, flags);
}

ssize_t PipeChannel::writev(ioctx_t* ctx, const struct iovec* iov, int iovcnt)
{
	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = (struct iovec*) iov;
	msg.msg_iovlen = iovcnt;
	return sendmsg_internal(ctx, &msg, 0);
}

ssize_t PipeChannel::sendmsg(ioctx_t* ctx,
                             const struct msghdr* msg_ptr,
                             int flags)
{
	// sendmsg can only be called through Unix sockets and not regular pipes.
	struct msghdr msg;
	if ( !ctx->copy_from_src(&msg, msg_ptr, sizeof(msg)) )
		return -1;
	if ( msg.msg_iovlen < 0 || IOV_MAX < msg.msg_iovlen )
		return errno = EINVAL, -1;
	if ( msg.msg_name )
		return errno = EISCONN, -1;
	size_t iov_size = msg.msg_iovlen * sizeof(struct iovec);
	struct iovec* iov = new struct iovec[msg.msg_iovlen];
	if ( !iov )
		return -1;
	if ( !ctx->copy_from_src(iov, msg.msg_iov, iov_size) )
		return delete[] iov, -1;
	msg.msg_iov = iov;
	ssize_t result = sendmsg_internal(ctx, &msg, flags);
	delete[] iov;
	return result;
}

ssize_t PipeChannel::sendmsg_internal(ioctx_t* ctx,
                                      const struct msghdr* msg,
                                      int flags)
{
	if ( flags & ~(MSG_NOSIGNAL) )
		return errno = EINVAL, -1;
	if ( SSIZE_MAX < TruncateIOVec(msg->msg_iov, msg->msg_iovlen, SSIZE_MAX) )
		return errno = EINVAL, -1;
	// Measure how much control data buffer space is required to make sure it
	// can be sent in a single attempt without unnecessary truncation or
	// fragmentation. This is only used as an estimate, if the control data
	// changes between the two reads of it, then the caller loses the ability
	// to know how much control data was sent.
	unsigned char* control_ptr = (unsigned char*) msg->msg_control;
	size_t required = 1; // At least one free byte, plus control below.
	for ( size_t control_offset = 0; control_offset < msg->msg_controllen; )
	{
		// Read the next control message header.
		size_t control_left = msg->msg_controllen - control_offset;
		struct cmsghdr cmsg;
		if ( control_left < sizeof(cmsg) )
			return errno = EINVAL, -1;
		unsigned char* cmsg_ptr = control_ptr + control_offset;
		if ( !ctx->copy_from_src(&cmsg, cmsg_ptr, sizeof(cmsg)) )
			return -1;
		if ( cmsg.cmsg_len < sizeof(struct cmsghdr) ||
		     control_left < cmsg.cmsg_len )
			return errno = EINVAL, -1;
		// Determine how much space is needed for the message.
		size_t needed = cmsg.cmsg_len;
		size_t data_size = cmsg.cmsg_len - sizeof(struct cmsghdr);
		if ( cmsg.cmsg_level == SOL_SOCKET && cmsg.cmsg_type == SCM_RIGHTS )
		{
			size_t pointers_size;
			if ( __builtin_mul_overflow(data_size / sizeof(int),
			                            sizeof(uintptr_t), &pointers_size) ||
			     __builtin_add_overflow(sizeof(struct cmsghdr), pointers_size,
			                            &needed) )
				return errno = EMSGSIZE, -1;
		}
		if ( __builtin_add_overflow(required, needed, &required) )
				return errno = EMSGSIZE, -1;
		control_offset += CMSG_ALIGN(cmsg.cmsg_len);
	}
	Thread* this_thread = CurrentThread();
	this_thread->yield_to_tid = receiver_system_tid;
	ScopedLockSignal lock(&pipe_lock);
	if ( !lock.IsAcquired() )
		return errno = EINTR, -1;
	sender_system_tid = this_thread->system_tid;
	bool need_header;
	struct segment_header header;
	// Wait until we can send all the control data in one attempt plus one byte.
	while ( true )
	{
		// Send SIGPIPE or fail with EPIPE if there are no readers left.
		if ( !any_reading )
		{
			if ( is_sigpipe_enabled && !(flags & MSG_NOSIGNAL) )
				CurrentThread()->DeliverSignal(SIGPIPE);
			return errno = EPIPE, -1;
		}
		if ( buffer_size < sizeof(header) )
			return errno = EMSGSIZE, -1;
		if ( buffer_size - sizeof(header) < required )
			return errno = EMSGSIZE, -1;
		// Check whether there is an existing segment header we can combine.
		need_header = true;
		if ( last_header_position < buffer_used )
		{
			assert(sizeof(header) <= buffer_used - last_header_position);
			ReadBuffer(CopyToKernel, &header, sizeof(header),
			           last_header_position);
			need_header = msg->msg_controllen && header.normal;
		}
		// Check if there's enough buffer space for the request now.
		size_t available = buffer_size - buffer_used;
		if ( need_header &&
		     sizeof(header) <= available &&
		     required <= available - sizeof(header) )
			break;
		if ( !need_header && required <= available )
			break;
		// Wait for more ring buffer space to be available.
		if ( ctx->dflags & O_NONBLOCK )
			return errno = EWOULDBLOCK, -1;
		this_thread->yield_to_tid = receiver_system_tid;
		if ( !kthread_cond_wait_signal(&write_cond, &pipe_lock) )
			return errno = EINTR, -1;
	}
	// Write the control data now that we've taken steps avoid truncation.
	for ( size_t control_offset = 0; control_offset < msg->msg_controllen; )
	{
		// Read the next control message header.
		size_t control_left = msg->msg_controllen - control_offset;
		struct cmsghdr cmsg;
		if ( control_left < sizeof(cmsg) )
			return errno = EINVAL, -1;
		unsigned char* cmsg_ptr = control_ptr + control_offset;
		if ( !ctx->copy_from_src(&cmsg, cmsg_ptr, sizeof(cmsg)) )
			return -1;
		if ( cmsg.cmsg_len < sizeof(struct cmsghdr) ||
		     control_left < cmsg.cmsg_len )
			return errno = EINVAL, -1;
		// Determine how much space is needed for the message.
		size_t needed = cmsg.cmsg_len;
		size_t data_size = cmsg.cmsg_len - sizeof(struct cmsghdr);
		if ( cmsg.cmsg_level == SOL_SOCKET && cmsg.cmsg_type == SCM_RIGHTS )
		{
			size_t pointers_size;
			if ( __builtin_mul_overflow(data_size / sizeof(int),
			                            sizeof(uintptr_t), &pointers_size) ||
			     __builtin_add_overflow(sizeof(struct cmsghdr), pointers_size,
			                            &needed) )
				return errno = EMSGSIZE, -1;
		}
		else
			return errno = EINVAL, -1;
		// Reject the control message if the ancillary data changed midway and
		// more buffer space is required than was provisioned above.
		if ( need_header &&
		     __builtin_add_overflow(needed, sizeof(header), &needed) )
			return errno = EMSGSIZE, -1;
		if ( buffer_size - buffer_used < needed )
			return errno = EMSGSIZE, -1;
		// Take into account potentially needing a segment header.
		size_t saved_last_header_position = last_header_position;
		Enqueue(CopyFromKernel, &cmsg, sizeof(cmsg), true);
		need_header = false;
		unsigned char* data_ptr = control_ptr + control_offset + sizeof(cmsg);
		bool failed = false;
		// File descriptors needs to be serialized as a raw pointer to the
		// underlying object which may be larger than an int.
		if ( cmsg.cmsg_level == SOL_SOCKET && cmsg.cmsg_type == SCM_RIGHTS )
		{
			Process* process = CurrentProcess();
			Ref<DescriptorTable> dtable = process->GetDTable();
			size_t fds = data_size / sizeof(int);
			for ( size_t i = 0; i < fds; i++ )
			{
				unsigned char* buf = data_ptr + sizeof(int) * i;
				int fd;
				if ( !(failed = !ctx->copy_from_src(&fd, buf, sizeof(fd))) )
				{
					Ref<Descriptor> desc = dtable->Get(fd);
					if ( !(failed = !desc) )
					{
						// File descriptors are reference counted and there must
						// never any reference count cycles, which can happen if
						// a socket is sent on a socket. Additionally a tall
						// tower of reference counted objects containing
						// reference counted objects can overflow the stack in
						// the destructors. Other operating systems have a full
						// garbage collection system to avoid these problems,
						// but here these properties are protected with three
						// strict rules.
						kthread_mutex_lock(&pass_lock);
						// 1. Sockets themselves being sent cannot be sent on.
						if ( passed_count )
							failed = true;
						// 2. Sockets cannot be sent on themselves (either
						//    endpoint). Prevent it by marking themselves as
						//    being sent on before asking if the descriptor is
						// not being sent on.
						passing_count++;
						kthread_mutex_unlock(&pass_lock);
						// 3. Sockets cannot send another socket being sent on.
						if ( failed || (failed = !desc->pass()) )
						{
							errno = EPERM;
							kthread_mutex_lock(&pass_lock);
							passing_count--;
							kthread_mutex_unlock(&pass_lock);
						}
						else
						{
							// Pass file descriptors as a leaked reference to
							// the underlying reference counted descriptor.
							uintptr_t ptr = desc.Export();
							Enqueue(CopyFromKernel, &ptr, sizeof(ptr), true);
						}
					}
				}
				// If any file descriptors couldn't be sent, undo the entire
				// control message, so the caller knows that either none of them
				// got sent or all of them.
				if ( failed )
				{
					for ( ; i; i-- )
					{
						uintptr_t ptr;
						Unenqueue(CopyToKernel, &ptr, sizeof(ptr),
						          saved_last_header_position);
						Ref<Descriptor> desc;
						desc.Import(ptr);
						desc->unpass();
						kthread_mutex_lock(&pass_lock);
						passing_count--;
						kthread_mutex_unlock(&pass_lock);
					}
					break;
				}
			}
		}
		else
		{
			if ( !Enqueue(ctx->copy_from_src, data_ptr, data_size, true) )
				failed = true;
		}
		// Undo the control message header if the control message data couldn't
		// be sent.
		if ( failed )
		{
			Unenqueue(NULL, NULL, sizeof(cmsg), saved_last_header_position);
			return -1;
		}
		control_offset += CMSG_ALIGN(cmsg.cmsg_len);
	}
	// Write the normal data to the ring buffer.
	ssize_t so_far = 0;
	int iov_i = 0;
	size_t iov_offset = 0;
	while ( iov_i < msg->msg_iovlen && so_far < SSIZE_MAX )
	{
		size_t max_count = SSIZE_MAX - so_far;
		struct iovec* iov = &msg->msg_iov[iov_i];
		const uint8_t* buf = (const uint8_t*) iov->iov_base + iov_offset;
		size_t count = iov->iov_len - iov_offset;
		if ( max_count < count )
			count = max_count;
		if ( count == 0 )
		{
			iov_i++;
			iov_offset = 0;
			continue;
		}
		// Handle when the buffer space is exhausted.
		sender_system_tid = this_thread->system_tid;
		size_t overhead = need_header ? sizeof(header) : 0;
		while ( any_reading && buffer_size - buffer_used <= overhead )
		{
			// Yield to the thread that pledged to read more data.
			this_thread->yield_to_tid = receiver_system_tid;
			if ( pledged_read )
			{
				pledged_write++;
				kthread_mutex_unlock(&pipe_lock);
				kthread_yield();
				kthread_mutex_lock(&pipe_lock);
				pledged_write--;
			}
			else
			{
				// If the estimated of the required buffer space was accurate,
				// it will always have been possible to write at least one byte
				// of data.
				if ( so_far )
					return so_far;
				// Wait for more buffer space to become available.
				if ( ctx->dflags & O_NONBLOCK )
					return so_far ? so_far : errno = EWOULDBLOCK, -1;
				pledged_write++;
				bool interrupted =
					!kthread_cond_wait_signal(&write_cond, &pipe_lock);
				pledged_write--;
				if ( interrupted )
					return so_far ? so_far : errno = EINTR, -1;
			}
			need_header = last_header_position == buffer_used;
			overhead = need_header ? sizeof(header) : 0;
		}
		// Send SIGPIPE or fail with EPIPE if there are no readers left.
		if ( !any_reading )
		{
			if ( so_far )
				return so_far;
			if ( is_sigpipe_enabled && !(flags & MSG_NOSIGNAL) )
				CurrentThread()->DeliverSignal(SIGPIPE);
			return errno = EPIPE, -1;
		}
		// Write the normal data to the ring buffer.
		size_t available = buffer_size - buffer_used - overhead;
		size_t amount = count;
		if ( available < amount )
			amount = available;
		if ( !Enqueue(ctx->copy_from_src, buf, amount, false) )
			return so_far ? so_far : -1;
		so_far += amount;
		iov_offset += amount;
		if ( iov_offset == iov->iov_len )
		{
			iov_i++;
			iov_offset = 0;
		}
	}
	return so_far;
}

short PipeChannel::ReadPollEventStatus()
{
	short status = 0;
	if ( !any_writing && !buffer_used )
		status |= POLLHUP;
	if ( buffer_used )
		status |= POLLIN | POLLRDNORM;
	return status;
}

short PipeChannel::WritePollEventStatus()
{
	short status = 0;
	if ( !any_reading )
		status |= POLLERR;
	bool need_header = last_header_position == buffer_size;
	size_t needed_space = need_header ? sizeof(segment_header) : 0;
	if ( any_reading && needed_space < buffer_size - buffer_used )
		status |= POLLOUT | POLLWRNORM;
	return status;
}

int PipeChannel::read_poll(ioctx_t* /*ctx*/, PollNode* node)
{
	ScopedLock lock(&pipe_lock);
	short ret_status = ReadPollEventStatus() & node->events;
	if ( ret_status )
		return node->master->revents |= ret_status, 0;
	read_poll_channel.Register(node);
	return errno = EAGAIN, -1;
}

int PipeChannel::write_poll(ioctx_t* /*ctx*/, PollNode* node)
{
	ScopedLock lock(&pipe_lock);
	short ret_status = WritePollEventStatus() & node->events;
	if ( ret_status )
		return node->master->revents |= ret_status, 0;
	write_poll_channel.Register(node);
	return errno = EAGAIN, -1;
}

int PipeChannel::sockatmark(ioctx_t* /*ctx*/)
{
	if ( !buffer_used )
		return 0;
	struct segment_header header;
	assert(sizeof(header) <= buffer_used);
	ReadBuffer(CopyToKernel, &header, sizeof(header), 0);
	return 0 < header.ancillary;
}

bool PipeChannel::GetSIGPIPEDelivery()
{
	ScopedLock lock(&pipe_lock);
	return is_sigpipe_enabled;
}

void PipeChannel::SetSIGPIPEDelivery(bool deliver_sigpipe)
{
	ScopedLock lock(&pipe_lock);
	is_sigpipe_enabled = deliver_sigpipe;
}

size_t PipeChannel::ReadSize()
{
	ScopedLock lock(&pipe_lock);
	return pretended_read_buffer_size;
}

size_t PipeChannel::WriteSize()
{
	ScopedLock lock(&pipe_lock);
	return buffer_size;
}

bool PipeChannel::ReadResize(size_t new_size)
{
	ScopedLock lock(&pipe_lock);
	if ( !new_size )
		return errno = EINVAL, false;
	// The reading and writing ends share the same buffer, so let the writing
	// end decide how big a buffer it wants and pretend the reading end can
	// decide too.
	pretended_read_buffer_size = new_size;
	return true;
}

bool PipeChannel::WriteResize(size_t new_size)
{
	ScopedLock lock(&pipe_lock);
	if ( !new_size )
		return errno = EINVAL, false;

	size_t max_pipe_size = 2 * 1024 * 1024;
	if ( max_pipe_size < new_size )
		new_size = max_pipe_size;

	size_t min_pipe_size = sizeof(segment_header) + 1;
	if ( new_size < min_pipe_size )
		new_size = min_pipe_size;

	// Refuse to lose data if the new size would cause truncation.
	if ( new_size < buffer_used )
		new_size = buffer_used;

	uint8_t* new_buffer = new uint8_t[new_size];
	if ( !new_buffer )
		return false;

	for ( size_t i = 0; i < buffer_used; i++ )
		new_buffer[i] = buffer[(buffer_offset + i) % buffer_size];
	delete[] buffer;
	buffer = new_buffer;
	buffer_size = new_size;
	buffer_offset = 0;

	return true;
}

PipeEndpoint::PipeEndpoint()
{
	channel = NULL;
	reading = false;
}

PipeEndpoint::~PipeEndpoint()
{
	if ( channel )
		Disconnect();
}

bool PipeEndpoint::Connect(PipeEndpoint* destination)
{
	assert(!channel);
	assert(!destination->channel);
	const size_t BUFFER_SIZE = 64 * 1024;
	size_t size = BUFFER_SIZE;
	uint8_t* buffer = new uint8_t[size];
	if ( !buffer )
		return false;
	destination->reading = !(reading = false);
	if ( !(destination->channel = channel = new PipeChannel(buffer, size)) )
	{
		delete[] buffer;
		return false;
	}
	return true;
}

void PipeEndpoint::Disconnect()
{
	if ( !channel )
		return;
	if ( reading )
		channel->CloseReading();
	else
		channel->CloseWriting();
	channel = NULL;
}

bool PipeEndpoint::pass()
{
	return channel ? channel->pass() : false;
}

void PipeEndpoint::unpass()
{
	if ( channel )
		channel->unpass();
}

ssize_t PipeEndpoint::recv(ioctx_t* ctx, uint8_t* buf, size_t count, int flags)
{
	if ( !reading )
		return errno = EBADF, -1;
	if ( !channel )
		return 0;
	ssize_t result = channel->recv(ctx, buf, count, flags);
	CurrentThread()->yield_to_tid = 0;
	Scheduler::ScheduleTrueThread();
	return result;
}

ssize_t PipeEndpoint::recvmsg(ioctx_t* ctx, struct msghdr* msg, int flags)
{
	if ( !reading )
		return errno = EBADF, -1;
	if ( !channel )
		return 0;
	ssize_t result = channel->recvmsg(ctx, msg, flags);
	CurrentThread()->yield_to_tid = 0;
	Scheduler::ScheduleTrueThread();
	return result;
}

ssize_t PipeEndpoint::send(ioctx_t* ctx, const uint8_t* buf, size_t count,
                           int flags)
{
	if ( reading )
		return errno = EBADF, -1;
	if ( !channel )
	{
		if ( !(flags & MSG_NOSIGNAL) )
			CurrentThread()->DeliverSignal(SIGPIPE);
		return errno = EPIPE, -1;
	}
	ssize_t result = channel->send(ctx, buf, count, flags);
	CurrentThread()->yield_to_tid = 0;
	Scheduler::ScheduleTrueThread();
	return result;
}

ssize_t PipeEndpoint::sendmsg(ioctx_t* ctx, const struct msghdr* msg, int flags)
{
	if ( reading )
		return errno = EBADF, -1;
	if ( !channel )
	{
		if ( !(flags & MSG_NOSIGNAL) )
			CurrentThread()->DeliverSignal(SIGPIPE);
		return errno = EPIPE, -1;
	}
	ssize_t result = channel->sendmsg(ctx, msg, flags);
	CurrentThread()->yield_to_tid = 0;
	Scheduler::ScheduleTrueThread();
	return result;
}

ssize_t PipeEndpoint::readv(ioctx_t* ctx, const struct iovec* iov, int iovcnt)
{
	if ( !reading )
		return errno = EBADF, -1;
	ssize_t result = channel->readv(ctx, iov, iovcnt);
	if ( !channel )
		return 0;
	CurrentThread()->yield_to_tid = 0;
	Scheduler::ScheduleTrueThread();
	return result;
}

ssize_t PipeEndpoint::writev(ioctx_t* ctx, const struct iovec* iov, int iovcnt)
{
	if ( reading )
		return errno = EBADF, -1;
	if ( !channel )
	{
		CurrentThread()->DeliverSignal(SIGPIPE);
		return errno = EPIPE, -1;
	}
	ssize_t result = channel->writev(ctx, iov, iovcnt);
	CurrentThread()->yield_to_tid = 0;
	Scheduler::ScheduleTrueThread();
	return result;
}

int PipeEndpoint::poll(ioctx_t* ctx, PollNode* node)
{
	if ( !channel )
		return 0;
	return reading ? channel->read_poll(ctx, node)
	               : channel->write_poll(ctx, node);
}

int PipeEndpoint::sockatmark(ioctx_t* ctx)
{
	return reading ? channel->sockatmark(ctx) : 0;
}

bool PipeEndpoint::GetSIGPIPEDelivery()
{
	if ( !channel )
		return errno = EINVAL, true;
	return !reading ? channel->GetSIGPIPEDelivery() : false;
}

bool PipeEndpoint::SetSIGPIPEDelivery(bool deliver_sigpipe)
{
	if ( !channel )
		return errno = EINVAL, false;
	if ( !reading )
		channel->SetSIGPIPEDelivery(deliver_sigpipe);
	else if ( reading && deliver_sigpipe != false )
		return errno = EINVAL, false;
	return true;
}

size_t PipeEndpoint::Size()
{
	if ( !channel )
		return errno = EINVAL, 0;
	return reading ? channel->ReadSize()
	               : channel->WriteSize();
}

bool PipeEndpoint::Resize(size_t new_size)
{
	if ( !channel )
		return errno = EINVAL, false;
	return reading ? channel->ReadResize(new_size)
	               : channel->WriteResize(new_size);
}

class PipeNode : public AbstractInode
{
public:
	PipeNode(dev_t dev, uid_t owner, gid_t group, mode_t mode);
	virtual ~PipeNode();
	virtual bool pass();
	virtual void unpass();
	virtual ssize_t readv(ioctx_t* ctx, const struct iovec* iov, int iovcnt);
	virtual ssize_t writev(ioctx_t* ctx, const struct iovec* iov, int iovcnt);
	virtual int poll(ioctx_t* ctx, PollNode* node);
	// Pipes must not provide sendmsg/recvmsg that can do file descriptor
	// passing. S_IFNEVERWRAP in type must be set if this was to be supported,
	// and the kernel would need to be audited for the assumption that only
	// filesystem sockets can do file descriptor passing.

public:
	bool Connect(PipeNode* destination);

private:
	PipeEndpoint endpoint;

};

bool PipeNode::Connect(PipeNode* destination)
{
	return endpoint.Connect(&destination->endpoint);
}

PipeNode::PipeNode(dev_t dev, uid_t owner, gid_t group, mode_t mode)
{
	inode_type = INODE_TYPE_STREAM;
	this->dev = dev;
	this->ino = (ino_t) this;
	this->stat_uid = owner;
	this->stat_gid = group;
	this->type = S_IFIFO;
	this->stat_mode = (mode & S_SETABLE) | this->type;
	supports_iovec = true;
}

PipeNode::~PipeNode()
{
}

bool PipeNode::pass()
{
	return endpoint.pass();
}

void PipeNode::unpass()
{
	endpoint.unpass();
}

ssize_t PipeNode::readv(ioctx_t* ctx, const struct iovec* iov, int iovcnt)
{
	return endpoint.readv(ctx, iov, iovcnt);
}

ssize_t PipeNode::writev(ioctx_t* ctx, const struct iovec* iov, int iovcnt)
{
	return endpoint.writev(ctx, iov, iovcnt);
}

int PipeNode::poll(ioctx_t* ctx, PollNode* node)
{
	return endpoint.poll(ctx, node);
}

int sys_pipe2(int* pipefd, int flags)
{
	int fdflags = 0;
	if ( flags & O_CLOEXEC ) fdflags |= FD_CLOEXEC;
	if ( flags & O_CLOFORK ) fdflags |= FD_CLOFORK;
	flags &= ~(O_CLOEXEC | O_CLOFORK);

	if ( flags & ~(O_NONBLOCK) )
		return errno = EINVAL, -1;

	Process* process = CurrentProcess();
	uid_t uid = process->uid;
	uid_t gid = process->gid;
	mode_t mode = 0600;

	Ref<PipeNode> recv_inode(new PipeNode(0, uid, gid, mode));
	if ( !recv_inode ) return -1;
	Ref<PipeNode> send_inode(new PipeNode(0, uid, gid, mode));
	if ( !send_inode ) return -1;

	if ( !send_inode->Connect(recv_inode.Get()) )
		return -1;

	Ref<Vnode> recv_vnode(new Vnode(recv_inode, Ref<Vnode>(NULL), 0, 0));
	Ref<Vnode> send_vnode(new Vnode(send_inode, Ref<Vnode>(NULL), 0, 0));
	if ( !recv_vnode || !send_vnode ) return -1;

	Ref<Descriptor> recv_desc(new Descriptor(recv_vnode, O_READ | flags));
	Ref<Descriptor> send_desc(new Descriptor(send_vnode, O_WRITE | flags));
	if ( !recv_desc || !send_desc ) return -1;

	Ref<DescriptorTable> dtable = process->GetDTable();
	int reservation;
	if ( !dtable->Reserve(2, &reservation) )
		return -1;
	int recv_index = dtable->Allocate(recv_desc, fdflags, 0, &reservation);
	int send_index = dtable->Allocate(send_desc, fdflags, 0, &reservation);
	assert(0 <= recv_index);
	assert(0 <= send_index);
	int ret[2] = { recv_index, send_index };
	if ( !CopyToUser(pipefd, ret, sizeof(ret)) )
		return -1;

	return 0;
}

} // namespace Sortix
