/*
 * Copyright (c) 2015, 2016 Jonas 'Sortie' Termansen.
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
 * pty.cpp
 * Pseudoterminals.
 */

#include <sys/types.h>

#include <errno.h>

#include <sortix/fcntl.h>
#include <sortix/ioctl.h>
#include <sortix/poll.h>
#include <sortix/stat.h>
#include <sortix/termmode.h>
#include <sortix/winsize.h>

#include <sortix/kernel/copy.h>
#include <sortix/kernel/descriptor.h>
#include <sortix/kernel/dtable.h>
#include <sortix/kernel/inode.h>
#include <sortix/kernel/ioctx.h>
#include <sortix/kernel/kernel.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/poll.h>
#include <sortix/kernel/process.h>
#include <sortix/kernel/refcount.h>
#include <sortix/kernel/signal.h>
#include <sortix/kernel/syscall.h>
#include <sortix/kernel/vnode.h>

#include "tty.h"

namespace Sortix {

class PTY : public TTY
{
public:
	PTY(mode_t mode, uid_t owner, gid_t group);
	virtual ~PTY();

public:
	virtual int sync(ioctx_t* ctx);
	virtual int ioctl(ioctx_t* ctx, int cmd, uintptr_t arg);

public:
	ssize_t master_read(ioctx_t* ctx, uint8_t* buf, size_t count);
	ssize_t master_write(ioctx_t* ctx, const uint8_t* buf, size_t count);
	int master_poll(ioctx_t* ctx, PollNode* node);
	int master_ioctl(ioctx_t* ctx, int cmd, uintptr_t arg);

protected:
	virtual void tty_output(const unsigned char* buffer, size_t length);

private:
	struct winsize ws;
	kthread_mutex_t input_lock;
	kthread_mutex_t output_lock;
	kthread_cond_t output_ready_cond;
	kthread_cond_t output_possible_cond;
	size_t output_offset;
	size_t output_used;
	static const size_t output_size = 4096;
	uint8_t output[output_size];

};

PTY::PTY(mode_t mode, uid_t owner, gid_t group)
	: TTY(0, mode, owner, group)
{
	tio.c_cflag |= CREAD;
	input_lock = KTHREAD_MUTEX_INITIALIZER;
	output_lock = KTHREAD_MUTEX_INITIALIZER;
	output_ready_cond = KTHREAD_COND_INITIALIZER;
	output_possible_cond = KTHREAD_COND_INITIALIZER;
	output_offset = 0;
	output_used = 0;
	memset(&ws, 0, sizeof(ws));
}

PTY::~PTY()
{
}

ssize_t PTY::master_read(ioctx_t* ctx, uint8_t* buf, size_t count)
{
	ScopedLockSignal lock(&output_lock);
	if ( !lock.IsAcquired() )
		return errno = EINTR, -1;
	while ( !output_used )
	{
		if ( ctx->dflags & O_NONBLOCK )
			return errno = EWOULDBLOCK, -1;
		if ( !kthread_cond_wait_signal(&output_ready_cond, &output_lock) )
			return errno = EINTR, -1;
	}
	size_t sofar = 0;
	while ( output_used && sofar < count )
	{
		size_t limit = output_size - output_offset;
		size_t possible = limit < output_used ? limit : output_used;
		size_t amount = count < possible ? count : possible;
		if ( !ctx->copy_to_dest(buf + sofar, output + output_offset, amount) )
			return sofar ? (ssize_t) sofar : -1;
		output_used -= amount;
		output_offset += amount;
		if ( output_offset == output_size )
			output_offset = 0;
		sofar += amount;
		kthread_cond_signal(&output_possible_cond);
	}
	return (ssize_t) sofar;
}

ssize_t PTY::master_write(ioctx_t* ctx, const uint8_t* buf, size_t count)
{
	ScopedLockSignal lock(&input_lock);
	if ( !lock.IsAcquired() )
		return errno = EINTR, -1;
	size_t sofar = 0;
	while ( sofar < count )
	{
		uint8_t input[1024];
		size_t amount = count < sizeof(input) ? count : sizeof(input);
		if ( !ctx->copy_from_src(input, buf + sofar, amount) )
			return sofar ? (ssize_t) sofar : -1;
		for ( size_t i = 0; i < amount; i++ )
		{
			if ( Signal::IsPending() )
				return sofar ? (ssize_t) sofar : (errno = EINTR, -1);
			ProcessByte(input[i]);
		}
		sofar += amount;
	}
	return (ssize_t) sofar;
}

// TODO: This function can deadlock if data is written using master_write, with
//       tty ECHO on, then it echos the input through this function, but the
//       output buffer is full, so it blocks. But there only was a single thread
//       using the pty, which did a write, and now is waiting for itself to
//       read. Either this is a broken usage of pty's and you must have a
//       dedicated read thread, or need a dedicated kernel thread for each pty
//       that buffers up a large amount of input, then processes it at its own
//       pace.
void PTY::tty_output(const unsigned char* buffer, size_t length)
{
	ScopedLock lock(&output_lock);
	while ( length )
	{
		 while ( output_used == output_size )
			if ( !kthread_cond_wait_signal(&output_possible_cond, &output_lock) )
				return; // TODO: Data loss?
		size_t offset = output_offset + output_used;
		if ( output_size <= offset )
			offset -= output_size;
		size_t left = output_size - output_used;
		size_t end = offset + left;
		if ( output_size < end )
			end = output_size;
		size_t possible = end - offset;
		size_t amount = length < possible ? length : possible;
		memcpy(output + offset, buffer, amount);
		buffer += amount;
		length -= amount;
		output_used += amount;
		kthread_cond_signal(&output_ready_cond);
	}
}

int PTY::master_poll(ioctx_t* ctx, PollNode* node)
{
	(void) ctx;
	(void) node;
	return errno = ENOTSUP, -1; // TODO: Implement this.
}

int PTY::sync(ioctx_t* /*ctx*/)
{
	return 0;
}

int PTY::ioctl(ioctx_t* ctx, int cmd, uintptr_t arg)
{
	if ( cmd == TIOCGWINSZ )
	{
		ScopedLock lock(&termlock);
		struct winsize* user_ws = (struct winsize*) arg;
		if ( !ctx->copy_to_dest(user_ws, &ws, sizeof(ws)) )
			return -1;
		return 0;
	}
	return TTY::ioctl(ctx, cmd, arg);
}

int PTY::master_ioctl(ioctx_t* ctx, int cmd, uintptr_t arg)
{
	if ( cmd == TIOCSWINSZ )
	{
		ScopedLock lock(&termlock);
		const struct winsize* user_ws = (const struct winsize*) arg;
		if ( !ctx->copy_from_src(&ws, user_ws, sizeof(ws)) )
			return -1;
		return 0;
	}
	return ioctl(ctx, cmd, arg);
}

class MasterNode : public AbstractInode
{
public:
	MasterNode(uid_t owner, gid_t group, mode_t mode, Ref<PTY> pty);
	virtual ~MasterNode();
	virtual ssize_t read(ioctx_t* ctx, uint8_t* buf, size_t count);
	virtual ssize_t write(ioctx_t* ctx, const uint8_t* buf, size_t count);
	virtual int poll(ioctx_t* ctx, PollNode* node);
	virtual int ioctl(ioctx_t* ctx, int cmd, uintptr_t arg);

public:
	Ref<PTY> pty;

};

MasterNode::MasterNode(uid_t owner, gid_t group, mode_t mode, Ref<PTY> pty)
	: pty(pty)
{
	inode_type = INODE_TYPE_TTY;
	this->dev = (dev_t) this;
	this->ino = (ino_t) this;
	this->stat_uid = owner;
	this->stat_gid = group;
	this->type = S_IFCHR;
	this->stat_mode = (mode & S_SETABLE) | this->type;
}

MasterNode::~MasterNode()
{
	// TODO: Destrution of master should probably SIGHUP everything that has the
	//       pty as its controlling terminal.
}

ssize_t MasterNode::read(ioctx_t* ctx, uint8_t* buf, size_t count)
{
	return pty->master_read(ctx, buf, count);
}

ssize_t MasterNode::write(ioctx_t* ctx, const uint8_t* buf, size_t count)
{
	return pty->master_write(ctx, buf, count);
}

int MasterNode::poll(ioctx_t* ctx, PollNode* node)
{
	return pty->master_poll(ctx, node);
}

int MasterNode::ioctl(ioctx_t* ctx, int cmd, uintptr_t arg)
{
	return pty->master_ioctl(ctx, cmd, arg);
}

int sys_mkptyline(int* master_fd_user, int* slave_fd_user, int flags)
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

	Ref<PTY> slave_inode(new PTY(uid, gid, mode));
	if ( !slave_inode )
		return -1;
	Ref<MasterNode> master_inode(new MasterNode(uid, gid, mode, slave_inode));
	if ( !master_inode )
		return -1;

	Ref<Vnode> master_vnode(new Vnode(master_inode, Ref<Vnode>(NULL), 0, 0));
	Ref<Vnode> slave_vnode(new Vnode(slave_inode, Ref<Vnode>(NULL), 0, 0));
	master_inode.Reset();
	slave_inode.Reset();
	if ( !master_vnode || !slave_vnode )
		return -1;

	Ref<Descriptor> master_desc(new Descriptor(master_vnode, O_READ | O_WRITE | flags));
	Ref<Descriptor> slave_desc(new Descriptor(slave_vnode, O_READ | O_WRITE | flags));
	master_vnode.Reset();
	slave_vnode.Reset();
	if ( !master_desc || !slave_desc )
		return -1;

	Ref<DescriptorTable> dtable = process->GetDTable();
	int master_fd = dtable->Allocate(master_desc, fdflags);
	int slave_fd = dtable->Allocate(slave_desc, fdflags);
	master_desc.Reset();
	slave_desc.Reset();
	if ( master_fd < 0 || slave_fd < 0 )
	{
		if ( 0 < master_fd )
			dtable->Free(master_fd);
		if ( 0 < master_fd )
			dtable->Free(slave_fd);
		return -1;
	}
	dtable.Reset();

	if ( !CopyToUser(master_fd_user, &master_fd, sizeof(int)) ||
	     !CopyToUser(slave_fd_user, &slave_fd, sizeof(int)) )
		return -1;

	return 0;
}

} // namespace Sortix
