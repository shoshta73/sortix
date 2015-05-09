/*
 * Copyright (c) 2015 Jonas 'Sortie' Termansen.
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
#include <sortix/kernel/syscall.h>
#include <sortix/kernel/vnode.h>

namespace Sortix {

class MasterNode;
class SlaveNode;

class MasterNode : public AbstractInode
{
public:
	MasterNode(uid_t owner, gid_t group, mode_t mode);
	virtual ~MasterNode();
	virtual ssize_t read(ioctx_t* ctx, uint8_t* buf, size_t count);
	virtual ssize_t write(ioctx_t* ctx, const uint8_t* buf, size_t count);
	virtual int poll(ioctx_t* ctx, PollNode* node);

public:
	bool Attach(SlaveNode* destination);

};

class SlaveNode : public AbstractInode
{
public:
	SlaveNode(uid_t owner, gid_t group, mode_t mode);
	virtual ~SlaveNode();
	virtual int sync(ioctx_t* ctx);
	virtual ssize_t read(ioctx_t* ctx, uint8_t* buf, size_t count);
	virtual ssize_t write(ioctx_t* ctx, const uint8_t* buf, size_t count);
	virtual int tcgetwincurpos(ioctx_t* ctx, struct wincurpos* wcp);
	virtual int tcgetwinsize(ioctx_t* ctx, struct winsize* ws);
	virtual int tcsetpgrp(ioctx_t* ctx, pid_t pgid);
	virtual pid_t tcgetpgrp(ioctx_t* ctx);
	virtual int settermmode(ioctx_t* ctx, unsigned termmode);
	virtual int gettermmode(ioctx_t* ctx, unsigned* termmode);
	virtual int poll(ioctx_t* ctx, PollNode* node);
	virtual ssize_t tcgetblob(ioctx_t* ctx, const char* name, void* buffer, size_t count);
	virtual ssize_t tcsetblob(ioctx_t* ctx, const char* name, const void* buffer, size_t count);

};

MasterNode::MasterNode(uid_t owner, gid_t group, mode_t mode)
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
}

ssize_t MasterNode::read(ioctx_t* ctx, uint8_t* buf, size_t count)
{
	(void) ctx;
	(void) buf;
	(void) count;
	return 0;
}

ssize_t MasterNode::write(ioctx_t* ctx, const uint8_t* buf, size_t count)
{
	(void) ctx;
	(void) buf;
	(void) count;
	return 0;
}

int MasterNode::poll(ioctx_t* ctx, PollNode* node)
{
	(void) ctx;
	(void) node;
	return 0;
}

bool MasterNode::Attach(SlaveNode* destination)
{
	(void) destination;
	return true;
}

SlaveNode::SlaveNode(uid_t owner, gid_t group, mode_t mode)
{
	inode_type = INODE_TYPE_TTY;
	this->dev = (dev_t) this;
	this->ino = (ino_t) this;
	this->stat_uid = owner;
	this->stat_gid = group;
	this->type = S_IFCHR;
	this->stat_mode = (mode & S_SETABLE) | this->type;
}

SlaveNode::~SlaveNode()
{
}

int SlaveNode::sync(ioctx_t* /*ctx*/)
{
	return 0;
}

ssize_t SlaveNode::read(ioctx_t* ctx, uint8_t* buf, size_t count)
{
	(void) ctx;
	(void) buf;
	(void) count;
	return 0;
}

ssize_t SlaveNode::write(ioctx_t* ctx, const uint8_t* buf, size_t count)
{
	(void) ctx;
	(void) buf;
	(void) count;
	return 0;
}

int SlaveNode::tcgetwincurpos(ioctx_t* ctx, struct wincurpos* wcp)
{
	(void) ctx;
	(void) wcp;
	return 0;
}

int SlaveNode::tcgetwinsize(ioctx_t* ctx, struct winsize* ws)
{
	(void) ctx;
	(void) ws;
	return 0;
}

int SlaveNode::tcsetpgrp(ioctx_t* ctx, pid_t pgid)
{
	(void) ctx;
	(void) pgid;
	return 0;
}

pid_t SlaveNode::tcgetpgrp(ioctx_t* ctx)
{
	(void) ctx;
	return 0;
}

int SlaveNode::settermmode(ioctx_t* ctx, unsigned termmode)
{
	(void) ctx;
	(void) termmode;
	return 0;
}

int SlaveNode::gettermmode(ioctx_t* ctx, unsigned* termmode)
{
	(void) ctx;
	(void) termmode;
	return 0;
}

int SlaveNode::poll(ioctx_t* ctx, PollNode* node)
{
	(void) ctx;
	(void) node;
	return 0;
}

ssize_t SlaveNode::tcgetblob(ioctx_t* ctx, const char* name, void* buffer, size_t count)
{
	(void) ctx;
	(void) name;
	(void) buffer;
	(void) count;
	return 0;
}

ssize_t SlaveNode::tcsetblob(ioctx_t* ctx, const char* name, const void* buffer, size_t count)
{
	(void) ctx;
	(void) name;
	(void) buffer;
	(void) count;
	return 0;
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

	Ref<MasterNode> master_inode(new MasterNode(uid, gid, mode));
	Ref<SlaveNode> slave_inode(new SlaveNode(uid, gid, mode));
	if ( !master_inode || !slave_inode )
		return -1;
	if ( !master_inode->Attach(slave_inode.Get()) )
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
