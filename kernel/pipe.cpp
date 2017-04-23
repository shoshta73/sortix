/*
 * Copyright (c) 2011, 2012, 2013, 2014, 2015, 2017 Jonas 'Sortie' Termansen.
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

static kthread_mutex_t passing_lock = KTHREAD_MUTEX_INITIALIZER;

struct segment_header
{
	size_t ancillary;
	size_t normal;
};

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
	bool Enqueue(bool (*copy_from_src)(void* dest, const void* src, size_t n),
	             const void* src,
	             size_t amount);
	bool Dequeue(bool (*copy_to_dest)(void* dest, const void* src, size_t n),
	             void* dest,
	             size_t amount,
	             bool peek = false,
	             size_t peek_offset = 0);
	size_t file_pass_capability();
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

private:
	short ReadPollEventStatus();
	short WritePollEventStatus();

private:
	PollChannel read_poll_channel;
	PollChannel write_poll_channel;
	kthread_mutex_t pipelock;
	kthread_cond_t readcond;
	kthread_cond_t writecond;
	struct segment_header first_header;
	struct segment_header last_header;
	dev_t from_dev;
	dev_t to_dev;
	ino_t from_ino;
	ino_t to_ino;
	uint8_t* buffer;
	uintptr_t sender_system_tid;
	uintptr_t receiver_system_tid;
	size_t buffer_offset;
	size_t buffer_used;
	size_t buffer_size;
	size_t pretended_read_buffer_size;
	size_t pledged_read;
	size_t pledged_write;
	size_t enqueued_descriptors_count;
	unsigned long closers;
	bool anyreading;
	bool anywriting;
	bool is_sigpipe_enabled;

};

PipeChannel::PipeChannel(uint8_t* buffer, size_t buffer_size)
{
	pipelock = KTHREAD_MUTEX_INITIALIZER;
	readcond = KTHREAD_COND_INITIALIZER;
	writecond = KTHREAD_COND_INITIALIZER;
	first_header.ancillary = 0;
	first_header.normal = 0;
	last_header.ancillary = 0;
	last_header.normal = 0;
	// TODO: from_dev.
	// TODO: to_dev.
	// TODO: from_ino.
	// TODO: to_ino.
	this->buffer = buffer;
	sender_system_tid = 0;
	receiver_system_tid = 0;
	buffer_offset = 0;
	buffer_used = 0;
	this->buffer_size = buffer_size;
	pretended_read_buffer_size = buffer_size;
	pledged_read = 0;
	pledged_write = 0;
	enqueued_descriptors_count = 0;
	closers = 0;
	anyreading = true;
	anywriting = true;
	is_sigpipe_enabled = true;
}

PipeChannel::~PipeChannel()
{
	// TODO: Dereference all file descriptors in the queue.
	delete[] buffer;
}

void PipeChannel::CloseReading()
{
	kthread_mutex_lock(&pipelock);
	anyreading = false;
	kthread_cond_broadcast(&writecond);
	read_poll_channel.Signal(ReadPollEventStatus());
	write_poll_channel.Signal(WritePollEventStatus());
	kthread_mutex_unlock(&pipelock);
	unsigned long count = InterlockedIncrement(&closers).n;
	if ( count == 2 )
		delete this;
}

void PipeChannel::CloseWriting()
{
	kthread_mutex_lock(&pipelock);
	anywriting = false;
	kthread_cond_broadcast(&readcond);
	read_poll_channel.Signal(ReadPollEventStatus());
	write_poll_channel.Signal(WritePollEventStatus());
	kthread_mutex_unlock(&pipelock);
	unsigned long count = InterlockedIncrement(&closers).n;
	if ( count == 2 )
		delete this;
}

bool PipeChannel::Enqueue(bool (*copy_from_src)(void*, const void*, size_t),
                          const void* src_ptr,
                          size_t amount)
{
	size_t write_offset = (buffer_offset + buffer_used) % buffer_size;
	size_t linear = buffer_size - write_offset;
	size_t first = linear < amount ? linear : amount;
	const unsigned char* src = (const unsigned char*) src_ptr;
	if ( !copy_from_src(buffer + write_offset, src, first) )
		return false;
	if ( first < amount &&
	     !copy_from_src(buffer, src + first, amount - first) )
		return false;
	buffer_used += amount;
	kthread_cond_broadcast(&readcond);
	read_poll_channel.Signal(ReadPollEventStatus());
	write_poll_channel.Signal(WritePollEventStatus());
	return true;
}

bool PipeChannel::Dequeue(bool (*copy_to_dest)(void*, const void*, size_t),
                          void* dest_ptr,
                          size_t amount,
                          bool peek,
                          size_t peek_offset)
{
	size_t offset = buffer_offset;
	if ( peek_offset )
		offset = (buffer_offset + peek_offset) % buffer_size;
	size_t linear = buffer_size - offset;
	size_t first = linear < amount ? linear : amount;
	unsigned char* dest = (unsigned char*) dest_ptr;
	if ( !copy_to_dest(dest, buffer + offset, first) )
		return false;
	if ( first < amount &&
	     !copy_to_dest(dest + first, buffer, amount - first) )
		return false;
	if ( !peek )
	{
		buffer_offset = (offset + amount) % buffer_size;
		buffer_used -= peek_offset + amount;
		kthread_cond_broadcast(&writecond);
		read_poll_channel.Signal(ReadPollEventStatus());
		write_poll_channel.Signal(WritePollEventStatus());
	}
	return true;
}

// Returns 0 if incapable of file description passing, 1 if capable but not
// currently passing any file descriptions, and 2 or higher if any passes are
// in progress.
size_t PipeChannel::file_pass_capability() // passing_lock locked.
{
	return 1 + (0 < enqueued_descriptors_count ? 1 : 0);
}

ssize_t PipeChannel::recv(ioctx_t* ctx, uint8_t* buf, size_t count,
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
	struct msghdr msg;
	if ( !ctx->copy_from_src(&msg, msg_ptr, sizeof(msg)) )
		return -1;
	if ( msg.msg_iovlen < 0 || IOV_MAX < msg.msg_iovlen )
		return errno = EINVAL, -1;
	size_t iov_size = msg.msg_iovlen * sizeof(struct iovec);
	struct iovec* iov = new struct iovec[msg.msg_iovlen];
	if ( !iov )
		return -1;
	if ( !ctx->copy_from_src(iov, msg.msg_iov, iov_size) )
		return delete[] iov, -1;
	msg.msg_iov = iov;
	size_t result = recvmsg_internal(ctx, &msg, flags);
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
	// TODO: Maybe a flag for more useful control data, so it doesn't get
	//       truncated?
	if ( flags & ~(MSG_PEEK | MSG_WAITALL | MSG_CMSG_CLOEXEC |
	               MSG_CMSG_CLOFORK) )
		return errno = EINVAL, -1;
	Thread* this_thread = CurrentThread();
	this_thread->yield_to_tid = sender_system_tid;
	ScopedLockSignal lock(&pipelock);
	if ( !lock.IsAcquired() )
		return errno = EINTR, -1;
	ssize_t so_far = 0;
	size_t peeked = 0;
	// TODO: This is code duplication.
	receiver_system_tid = this_thread->system_tid;
	while ( anywriting && first_header.normal <= peeked )
	{
		if ( first_header.ancillary )
			break;
		if ( first_header.normal < buffer_used )
			break;
		if ( (flags & MSG_PEEK) && so_far )
			return so_far;
		this_thread->yield_to_tid = sender_system_tid;
		if ( pledged_read )
		{
			pledged_write++;
			kthread_mutex_unlock(&pipelock);
			kthread_yield();
			kthread_mutex_lock(&pipelock);
			pledged_write--;
			continue;
		}
		// TODO: Return immediately if ancillary data was read.
		if ( !(flags & MSG_WAITALL) && so_far )
			return so_far;
		if ( ctx->dflags & O_NONBLOCK )
			return errno = EWOULDBLOCK, -1;
		pledged_write++;
		bool interrupted = !kthread_cond_wait_signal(&readcond, &pipelock);
		pledged_write--;
		if ( interrupted )
			return so_far ? so_far : (errno = EINTR, -1);
	}
	unsigned char* control = (unsigned char*) msg->msg_control;
	size_t control_length = msg->msg_controllen;
	bool failed = false;
	// TODO: MSG_PEEK?
	// TODO: Proper error handling.
	while ( 0 < first_header.ancillary )
	{
		struct cmsghdr cmsg;
		assert(sizeof(cmsg) <= first_header.ancillary);
		Dequeue(CopyToKernel, &cmsg, sizeof(cmsg));
		first_header.ancillary -= sizeof(cmsg);
		size_t data = cmsg.cmsg_len - sizeof(struct cmsghdr);
		if ( sizeof(cmsg) <= control_length &&
		     ctx->copy_to_dest(control, &cmsg, sizeof(cmsg)) )
		{
			control += sizeof(cmsg);
			control_length -= sizeof(cmsg);
		}
		else
			failed = true;
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
			// TODO: Properly discard the message if any of these failed.
			// TODO: Preallocate the right number of file descriptors to avoid
			//       error cases. It's OK for fds to get lost, I guess, if the
			//       caller gives us bad buffers. It's also OK if the buffer is
			//       too small and we have to truncate.
			for ( size_t i = 0; i < fds; i++ )
			{
				uintptr_t ptr;
				Dequeue(CopyToKernel, &ptr, sizeof(ptr));
				first_header.ancillary -= sizeof(ptr);
				Ref<Descriptor> desc;
				desc.Import(ptr);
				// TODO: If desc has capacity to pass file descriptors, count
				// down of how many of such we can do.
				if ( failed )
					continue;
				int fd;
				if ( control_length < sizeof(int) )
				{
					failed = true;
					continue;
				}
				if ( (fd = dtable->Allocate(desc, fdflags)) < 0 )
				{
					// TODO: This is what OpenBSD does. But should we use
					//       EMSGSIZE to mean the caller should provide more
					//       control data?
					errno = EMSGSIZE;
					failed = true;
					continue;
				}
				if ( !ctx->copy_to_dest(control, &fd, sizeof(fd)) )
				{
					failed = true;
					continue;
				}
				control += sizeof(fd);
				control_length -= sizeof(fd);
			}
		}
		else
		{
			for ( size_t i = 0; i < data; i++ )
			{
				unsigned char byte;
				Dequeue(CopyToKernel, &byte, 1);
				first_header.ancillary--;
				if ( (failed = failed || control_length < 1) )
					continue;
				control++;
				control_length--;
			}
		}
		if ( !failed )
		{
			// TODO: Any need to force padding after the last message?
			size_t misaligned = CMSG_ALIGN(data) - data;
			if ( control_length <= misaligned &&
			     ctx->zero_dest(control, misaligned) )
			{
				control += misaligned;
				control_length -= misaligned;
			}
			else
				failed = true;
		}
		if ( failed )
		{
			// TODO: Unwind file descriptors copied so far. Complicated, other
			//       threads may already have accessed them, needs a lock. Hmm.
		}
	}
	msg->msg_controllen -= control_length;
	// TODO: If failed where errno is set, return -1?
	if ( failed )
		msg->msg_flags |= MSG_CTRUNC;
	if ( SSIZE_MAX < TruncateIOVec(msg->msg_iov, msg->msg_iovlen, SSIZE_MAX) )
		return errno = EINVAL, -1;
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
		while ( anywriting && first_header.normal <= peeked )
		{
			if ( first_header.ancillary )
				break;
			if ( first_header.normal < buffer_used )
				break;
			if ( (flags & MSG_PEEK) && so_far )
				return so_far;
			this_thread->yield_to_tid = sender_system_tid;
			if ( pledged_read )
			{
				pledged_write++;
				kthread_mutex_unlock(&pipelock);
				kthread_yield();
				kthread_mutex_lock(&pipelock);
				pledged_write--;
				continue;
			}
			// TODO: Return immediately if ancillary data was read.
			if ( !(flags & MSG_WAITALL) && so_far )
				return so_far;
			if ( ctx->dflags & O_NONBLOCK )
				return errno = EWOULDBLOCK, -1;
			pledged_write++;
			bool interrupted = !kthread_cond_wait_signal(&readcond, &pipelock);
			pledged_write--;
			if ( interrupted )
				return so_far ? so_far : (errno = EINTR, -1);
		}
		if ( first_header.ancillary )
			return so_far;
		size_t used = first_header.normal - peeked;
		if ( !used && !anywriting )
			return so_far;
		size_t amount = count;
		if ( used < amount )
			amount = used;
		if ( !Dequeue(ctx->copy_to_dest, buf, amount, flags & MSG_PEEK,
		              peeked) )
			return so_far ? so_far : -1;
		so_far += amount;
		if ( flags & MSG_PEEK )
			peeked += amount;
		iov_offset += amount;
		first_header.normal -= amount;
		if ( first_header.normal == 0 && buffer_used )
		{
			if ( buffer_used == last_header.ancillary + last_header.normal )
			{
				first_header = last_header;
				last_header.ancillary = 0;
				last_header.normal = 0;
			}
			else
			{
				assert(sizeof(first_header) <= buffer_used);
				Dequeue(CopyToKernel, &first_header, sizeof(first_header));
			}
		}
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
	struct msghdr msg;
	if ( !ctx->copy_from_src(&msg, msg_ptr, sizeof(msg)) )
		return -1;
	if ( msg.msg_iovlen < 0 || IOV_MAX < msg.msg_iovlen )
		return errno = EINVAL, -1;
	size_t iov_size = msg.msg_iovlen * sizeof(struct iovec);
	struct iovec* iov = new struct iovec[msg.msg_iovlen];
	if ( !iov )
		return -1;
	if ( !ctx->copy_from_src(iov, msg.msg_iov, iov_size) )
		return delete[] iov, -1;
	msg.msg_iov = iov;
	size_t result = sendmsg_internal(ctx, &msg, flags);
	delete[] iov;
	return result;
}

ssize_t PipeChannel::sendmsg_internal(ioctx_t* ctx,
                                      const struct msghdr* msg,
                                      int flags)
{
	if ( flags & ~(MSG_WAITALL | MSG_NOSIGNAL) )
		return errno = EINVAL, -1;
	Thread* this_thread = CurrentThread();
	this_thread->yield_to_tid = receiver_system_tid;
	ScopedLockSignal lock(&pipelock);
	if ( !lock.IsAcquired() )
		return errno = EINTR, -1;
	sender_system_tid = this_thread->system_tid;
	unsigned char* control_ptr = (unsigned char*) msg->msg_control;
	size_t control_offset = 0;
	// TODO: Undo control stuff queued so far on failure?
	// TODO: Overflows.
	while ( control_offset < msg->msg_controllen )
	{
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
		if ( !(cmsg.cmsg_level == SOL_SOCKET && cmsg.cmsg_type == SCM_RIGHTS) )
			return errno = EINVAL, -1;
		size_t data_size = cmsg.cmsg_len - sizeof(struct cmsghdr);
		size_t needed = sizeof(struct cmsghdr) + data_size;
		if ( cmsg.cmsg_level == SOL_SOCKET && cmsg.cmsg_type == SCM_RIGHTS )
		{
			if ( data_size % sizeof(int) )
				return errno = EINVAL, -1;
			needed = sizeof(struct cmsghdr) +
			         data_size / sizeof(int) * sizeof(uintptr_t);
		}
		// TODO: And segment_header?
		while ( true )
		{
			size_t available = buffer_size - buffer_used;
			size_t actually_needed = needed;
			// TODO: Overflow?
			if ( first_header.normal && last_header.normal )
				actually_needed += sizeof(segment_header);
			if ( actually_needed <= available )
				break;
			// TODO: If the needed size exceeds the pipe capacity, EMSGSIZE.
			// TODO: It will not be possible to know how much ancillary was
			//       transmitted.
			if ( ctx->dflags & O_NONBLOCK )
				return errno = EWOULDBLOCK, -1;
			// TODO: This might interleave ancillary messages. Allow others to
			//       read, but don't allow any more writers right now?
			if ( !kthread_cond_wait_signal(&writecond, &pipelock) )
				return errno = EINTR, -1;
		}
		if ( first_header.normal && last_header.normal )
		{
			size_t available = buffer_size - buffer_used;
			assert(sizeof(last_header) <= available);
			Enqueue(CopyFromKernel, &last_header, sizeof(last_header));
			last_header.ancillary = 0;
			last_header.normal = 0;
		}
		Enqueue(CopyFromKernel, &cmsg, sizeof(cmsg));
		unsigned char* data_ptr = control_ptr + control_offset + sizeof(cmsg);
		if ( cmsg.cmsg_level == SOL_SOCKET && cmsg.cmsg_type == SCM_RIGHTS )
		{
			Process* process = CurrentProcess();
			Ref<DescriptorTable> dtable = process->GetDTable();
			assert(dtable);
			size_t fds = data_size / sizeof(int);
			for ( size_t i = 0; i < fds; i++ )
			{
				unsigned char* buf = data_ptr + sizeof(int) * i;
				int fd;
				if ( !ctx->copy_from_src(&fd, buf, sizeof(fd)) )
				{
					// TODO: Bail out.
					return -1;
				}
				Ref<Descriptor> desc = dtable->Get(fd);
				if ( !desc )
				{
					// TODO: Bail out.
					return -1;
				}
				// TODO: Validate desc isn't a unix socket containing another.
				uintptr_t ptr = desc.Export();
				Enqueue(CopyFromKernel, &ptr, sizeof(ptr));
			}
			size_t increment = sizeof(cmsg) + sizeof(uintptr_t) * fds;
			if ( first_header.normal )
				last_header.ancillary += increment;
			else
				first_header.ancillary += increment;
		}
		else
		{
			if ( !Enqueue(ctx->copy_from_src, data_ptr, data_size) )
			{
				// TODO: Bail out.
				return -1;
			}
			size_t increment = sizeof(cmsg) + data_size;
			if ( first_header.normal )
				last_header.ancillary += increment;
			else
				first_header.ancillary += increment;
		}
		control_offset += CMSG_ALIGN(cmsg.cmsg_len);
	}
	if ( SSIZE_MAX < TruncateIOVec(msg->msg_iov, msg->msg_iovlen, SSIZE_MAX) )
		return errno = EINVAL, -1;
	ssize_t so_far = 0;
	int iov_i = 0;
	size_t iov_offset = 0;
	while ( iov_i < msg->msg_iovlen && so_far < SSIZE_MAX )
	{
		size_t maxcount = SSIZE_MAX - so_far;
		struct iovec* iov = &msg->msg_iov[iov_i];
		const uint8_t* buf = (const uint8_t*) iov->iov_base + iov_offset;
		size_t count = iov->iov_len - iov_offset;
		if ( maxcount < count )
			count = maxcount;
		if ( count == 0 )
		{
			iov_i++;
			iov_offset = 0;
			continue;
		}
		sender_system_tid = this_thread->system_tid;
		while ( anyreading && buffer_used == buffer_size )
		{
			this_thread->yield_to_tid = receiver_system_tid;
			if ( pledged_write )
			{
				pledged_read++;
				kthread_mutex_unlock(&pipelock);
				kthread_yield();
				kthread_mutex_lock(&pipelock);
				pledged_read--;
				continue;
			}
			if ( so_far && !(flags & MSG_WAITALL) )
				return so_far;
			if ( ctx->dflags & O_NONBLOCK )
				return errno = EWOULDBLOCK, -1;
			pledged_read++;
			bool interrupted = !kthread_cond_wait_signal(&writecond, &pipelock);
			pledged_read--;
			if ( interrupted )
				return errno = EINTR, -1;
		}
		if ( !anyreading )
		{
			if ( so_far )
				return so_far;
			if ( is_sigpipe_enabled && !(flags & MSG_NOSIGNAL) )
				CurrentThread()->DeliverSignal(SIGPIPE);
			return errno = EPIPE, -1;
		}
		size_t amount = count;
		if ( buffer_size - buffer_used < amount )
			amount = buffer_size - buffer_used;
		bool use_first_header =
			first_header.ancillary + first_header.normal == buffer_used;
		if ( !Enqueue(ctx->copy_from_src, buf, amount) )
			return so_far ? so_far : -1;
		if ( use_first_header )
			first_header.normal += amount;
		else
			last_header.normal += amount;
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
	if ( !anywriting && !buffer_used )
		status |= POLLHUP;
	if ( buffer_used )
		status |= POLLIN | POLLRDNORM;
	return status;
}

short PipeChannel::WritePollEventStatus()
{
	short status = 0;
	if ( !anyreading )
		status |= POLLERR;
	if ( anyreading && buffer_used != buffer_size )
		status |= POLLOUT | POLLWRNORM;
	return status;
}

int PipeChannel::read_poll(ioctx_t* /*ctx*/, PollNode* node)
{
	ScopedLockSignal lock(&pipelock);
	short ret_status = ReadPollEventStatus() & node->events;
	if ( ret_status )
		return node->master->revents |= ret_status, 0;
	read_poll_channel.Register(node);
	return errno = EAGAIN, -1;
}

int PipeChannel::write_poll(ioctx_t* /*ctx*/, PollNode* node)
{
	ScopedLockSignal lock(&pipelock);
	short ret_status = WritePollEventStatus() & node->events;
	if ( ret_status )
		return node->master->revents |= ret_status, 0;
	write_poll_channel.Register(node);
	return errno = EAGAIN, -1;
}

bool PipeChannel::GetSIGPIPEDelivery()
{
	ScopedLockSignal lock(&pipelock);
	return is_sigpipe_enabled;
}

void PipeChannel::SetSIGPIPEDelivery(bool deliver_sigpipe)
{
	ScopedLockSignal lock(&pipelock);
	is_sigpipe_enabled = deliver_sigpipe;
}

size_t PipeChannel::ReadSize()
{
	ScopedLockSignal lock(&pipelock);
	return pretended_read_buffer_size;
}

size_t PipeChannel::WriteSize()
{
	ScopedLockSignal lock(&pipelock);
	return buffer_size;
}

bool PipeChannel::ReadResize(size_t new_size)
{
	ScopedLockSignal lock(&pipelock);
	if ( !new_size )
		return errno = EINVAL, false;
	// The read and write end share the same buffer, so let the write end decide
	// how big a buffer it wants and pretend the read end can decide too.
	pretended_read_buffer_size = new_size;
	return true;
}

bool PipeChannel::WriteResize(size_t new_size)
{
	ScopedLockSignal lock(&pipelock);
	if ( !new_size )
		return errno = EINVAL, false;

	size_t MAX_PIPE_SIZE = 2 * 1024 * 1024;
	if ( MAX_PIPE_SIZE < new_size )
		new_size = MAX_PIPE_SIZE;

	// Refuse to lose data if the the new size would cause truncation.
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
	ScopedLock lock(&passing_lock);
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
	ScopedLock lock(&passing_lock);
	channel = NULL;
}

size_t PipeEndpoint::file_pass_capability() // passing_lock locked.
{
	return channel ? channel->file_pass_capability() : false;
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
	virtual size_t file_pass_capability();
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
	this->type = S_IFCHR;
	this->stat_mode = (mode & S_SETABLE) | this->type;
	supports_iovec = true;
}

PipeNode::~PipeNode()
{
}

size_t PipeNode::file_pass_capability() // passing_lock locked.
{
	return endpoint.file_pass_capability();
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

	int recv_index, send_index;
	if ( 0 <= (recv_index = dtable->Allocate(recv_desc, fdflags)) )
	{
		if ( 0 <= (send_index = dtable->Allocate(send_desc, fdflags)) )
		{
			int ret[2] = { recv_index, send_index };
			if ( CopyToUser(pipefd, ret, sizeof(ret)) )
				return 0;

			dtable->Free(send_index);
		}
		dtable->Free(recv_index);
	}

	return -1;
}

} // namespace Sortix
