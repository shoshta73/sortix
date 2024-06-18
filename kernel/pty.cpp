/*
 * Copyright (c) 2015, 2016, 2021, 2022 Jonas 'Sortie' Termansen.
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

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

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
#include <sortix/kernel/ptable.h>
#include <sortix/kernel/refcount.h>
#include <sortix/kernel/signal.h>
#include <sortix/kernel/syscall.h>
#include <sortix/kernel/vnode.h>

#include "pty.h"
#include "tty.h"

#define ULONG_BIT (sizeof(unsigned long) * CHAR_BIT)
#define PTY_LIMIT (1024 * 1024)

namespace Sortix {

struct PTS::Entry
{
	char name[10 + 1];
	ino_t ino;
	Ref<Inode> inode;
};

Ref<PTS> pts;

static kthread_mutex_t ptynum_lock = KTHREAD_MUTEX_INITIALIZER;
static unsigned long* ptynum_bitmap = NULL;
static size_t ptynum_bitmap_words = 0;
static size_t ptynum_none_below = 0;

static int AllocatePTYNumber()
{
	ScopedLock lock(&ptynum_lock);
	for ( size_t i = ptynum_none_below/ULONG_BIT; i < ptynum_bitmap_words; i++ )
	{
		unsigned long word = ptynum_bitmap[i];
		if ( word == ULONG_MAX )
			continue;
		for ( size_t n = 0; n < ULONG_BIT; n++ )
		{
			unsigned long mask = 1UL << n;
			if ( word & mask )
				continue;
			ptynum_bitmap[i] = word | mask;
			size_t result = i * ULONG_MAX + n;
			if ( PTY_LIMIT < result || INT_MAX < result )
			{
				ptynum_none_below = result;
				return errno = EMFILE, -1;
			}
			ptynum_none_below = result + 1;
			return result;
		}
	}
	size_t new_words = 2 * ptynum_bitmap_words;
	if ( !new_words )
		new_words = 4;
	if ( PTY_LIMIT / ULONG_BIT < new_words )
		new_words = PTY_LIMIT / ULONG_BIT;
	if ( new_words <= ptynum_bitmap_words )
		return errno = EMFILE, -1;
	unsigned long* new_bitmap = (unsigned long*)
		reallocarray(ptynum_bitmap, new_words, sizeof(unsigned long));
	if ( !new_bitmap )
		return -1;
	for ( size_t i = ptynum_bitmap_words; i < new_words; i++ )
		new_bitmap[i] = 0;
	size_t result = ptynum_bitmap_words * ULONG_BIT;
	new_bitmap[ptynum_bitmap_words] |= 1UL << 0;
	ptynum_bitmap = new_bitmap;
	ptynum_bitmap_words = new_words;
	ptynum_none_below = result + 1;
	return result;
}

static void FreePTYNumber(int ptynum)
{
	assert(0 <= ptynum);
	ScopedLock lock(&ptynum_lock);
	assert((size_t) ptynum < ptynum_bitmap_words * ULONG_BIT);
	size_t word = ptynum / ULONG_BIT;
	size_t bit = ptynum % ULONG_BIT;
	unsigned long mask = 1UL << bit;
	assert(ptynum_bitmap[word] & mask);
	ptynum_bitmap[word] &= ~mask;
	if ( (size_t) ptynum < ptynum_none_below )
		ptynum_none_below = ptynum;
}

static ssize_t common_tcgetblob(ioctx_t* ctx,
                                const char* name,
                                void* buffer,
                                size_t count)
{
	if ( !name )
	{
		static const char index[] = "device-path\0filesystem-type\0";
		size_t index_size = sizeof(index) - 1;
		if ( buffer && count < index_size )
			return errno = ERANGE, -1;
		if ( buffer && !ctx->copy_to_dest(buffer, &index, index_size) )
			return -1;
		return (ssize_t) index_size;
	}
	else if ( !strcmp(name, "device-path") )
	{
		const char* data = "none";
		size_t size = strlen(data);
		if ( buffer && count < size )
			return errno = ERANGE, -1;
		if ( buffer && !ctx->copy_to_dest(buffer, data, size) )
			return -1;
		return (ssize_t) size;
	}
	else if ( !strcmp(name, "filesystem-type") )
	{
		const char* data = "pts";
		size_t size = strlen(data);
		if ( buffer && count < size )
			return errno = ERANGE, -1;
		if ( buffer && !ctx->copy_to_dest(buffer, data, size) )
			return -1;
		return (ssize_t) size;
	}
	else
		return errno = ENOENT, -1;
}

int common_statvfs(ioctx_t* ctx, struct statvfs* stvfs, dev_t dev)
{
	struct statvfs retstvfs;
	memset(&retstvfs, 0, sizeof(retstvfs));
	retstvfs.f_bsize = 0;
	retstvfs.f_frsize = 0;
	retstvfs.f_blocks = 0;
	retstvfs.f_bfree = 0;
	retstvfs.f_bavail = 0;
	retstvfs.f_files = 0;
	retstvfs.f_ffree = 0;
	retstvfs.f_favail = 0;
	retstvfs.f_fsid = dev;
	retstvfs.f_flag = ST_NOSUID;
	retstvfs.f_namemax = 10; /* ceil(log(INT_MAX)/log(10)) */
	if ( !ctx->copy_to_dest(stvfs, &retstvfs, sizeof(retstvfs)) )
		return -1;
	return 0;
}

class PTMX : public AbstractInode
{
public:
	PTMX(dev_t dev, ino_t ino, mode_t mode, uid_t owner, gid_t group);
	virtual ~PTMX();

public:
	virtual Ref<Inode> factory(ioctx_t* ctx, const char* filename, int flags,
	                           mode_t mode);

};

PTS::PTS(mode_t mode, uid_t owner, gid_t group)
{
	inode_type = INODE_TYPE_DIR;
	dev = (dev_t) this;
	ino = 0;
	stat_gid = owner;
	stat_gid = group;
	type = S_IFDIR;
	stat_mode = (mode & S_SETABLE) | type;
	dirlock = KTHREAD_MUTEX_INITIALIZER;
	entries = NULL;
	entries_count = 0;
	entries_allocated = 0;
}

PTS::~PTS()
{
}

bool PTS::ContainsFile(const char* name) // dirlock held
{
	if ( !strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, "ptmx") )
		return true;
	for ( size_t i = 0; i < entries_count; i++ )
		if ( !strcmp(entries[i].name, name) )
			return true;
	return false;
}

ssize_t PTS::readdirents(ioctx_t* ctx, struct dirent* dirent, size_t size,
                         off_t start)
{
	static const char* const names[3] = { ".", "..", "ptmx" };
	static const ino_t inos[3] = { 0, 0, 1 };
	static const unsigned char dtypes[3] = { DT_DIR, DT_DIR, DT_CHR };
	struct dirent retdirent;
	memset(&retdirent, 0, sizeof(retdirent));
	retdirent.d_dev = dev;
	const char* name;
	ino_t ino;
	unsigned char dtype;
	ScopedLock lock(&dirlock);
	if ( start < 3 )
	{
		name = names[start];
		ino = inos[start];
		dtype = dtypes[start];
	}
	else
	{
		start -= 3;
		if ( (uintmax_t) entries_count <= (uintmax_t) start )
			return 0;
		name = entries[start].name;
		ino = entries[start].ino;
		dtype = DT_CHR;
	}
	size_t namelen = strlen(name);
	retdirent.d_reclen = sizeof(*dirent) + namelen + 1;
	retdirent.d_namlen = namelen;
	retdirent.d_ino = ino;
	retdirent.d_type = dtype;
	if ( !ctx->copy_to_dest(dirent, &retdirent, sizeof(retdirent)) )
		return -1;
	if ( size < retdirent.d_reclen )
		return errno = ERANGE, -1;
	if ( !ctx->copy_to_dest(dirent->d_name, name, namelen+1) )
		return -1;
	return (ssize_t) retdirent.d_reclen;
}

Ref<Inode> PTS::open(ioctx_t* /*ctx*/, const char* filename, int flags,
                     mode_t /*mode*/)
{
	ScopedLock lock(&dirlock);
	if ( ContainsFile(filename) )
	{
		Ref<Inode> result;
		if ( (flags & O_CREATE) && (flags & O_EXCL) )
			return errno = EEXIST, Ref<Inode>(NULL);
		if ( !strcmp(filename, ".") || !strcmp(filename, "..") )
			result = Ref<Inode>(this);
		else if ( !strcmp(filename, "ptmx") )
			return Ref<Inode>(new PTMX(dev, 1, 0666, 0, 0));
		else
		{
			for ( size_t i = 0; !result && i < entries_count; i++ )
				if ( !strcmp(filename, entries[i].name) )
					result = entries[i].inode;
		}
		if ( result )
		{
			if ( (flags & O_CREATE) && (flags & O_EXCL) )
				return errno = EEXIST, Ref<Inode>(NULL);
			return result;
		}
	}
	if ( !(flags & O_CREATE) )
		return errno = ENOENT, Ref<Inode>(NULL);
	return errno = EPERM, Ref<Inode>(NULL);
}

int PTS::mkdir(ioctx_t* /*ctx*/, const char* filename, mode_t /*mode*/)
{
	ScopedLock lock(&dirlock);
	if ( ContainsFile(filename) )
		return errno = EEXIST, -1;
	return errno = EPERM, -1;
}

int PTS::link(ioctx_t* /*ctx*/, const char* filename, Ref<Inode> /*node*/)
{
	ScopedLock lock(&dirlock);
	if ( ContainsFile(filename) )
		return errno = EEXIST, -1;
	return errno = EPERM, -1;
}

int PTS::link_raw(ioctx_t* /*ctx*/, const char* filename, Ref<Inode> /*node*/)
{
	ScopedLock lock(&dirlock);
	if ( ContainsFile(filename) )
		return errno = EEXIST, -1;
	return errno = EPERM, -1;
}

int PTS::unlink(ioctx_t* /*ctx*/, const char* filename)
{
	ScopedLock lock(&dirlock);
	if ( !ContainsFile(filename) )
		return errno = ENOENT, -1;
	return errno = EPERM, -1;
}

int PTS::unlink_raw(ioctx_t* /*ctx*/, const char* filename)
{
	ScopedLock lock(&dirlock);
	if ( !ContainsFile(filename) )
		return errno = ENOENT, -1;
	return errno = EPERM, -1;
}

int PTS::rmdir(ioctx_t* /*ctx*/, const char* filename)
{
	ScopedLock lock(&dirlock);
	if ( !ContainsFile(filename) )
		return errno = ENOENT, -1;
	return errno = EPERM, -1;
}

int PTS::rmdir_me(ioctx_t* /*ctx*/)
{
	return errno = EPERM, -1;
}

int PTS::symlink(ioctx_t* /*ctx*/, const char* /*oldname*/,
                 const char* filename)
{
	ScopedLock lock(&dirlock);
	if ( ContainsFile(filename) )
		return errno = EEXIST, -1;
	return errno = EPERM, -1;
}

int PTS::rename_here(ioctx_t* /*ctx*/, Ref<Inode> /*from*/,
                     const char* /*oldname*/, const char* /*newname*/)
{
	return errno = EPERM, -1;
}

ssize_t PTS::tcgetblob(ioctx_t* ctx, const char* name, void* buffer,
                       size_t count)
{
	return common_tcgetblob(ctx, name, buffer, count);
}

int PTS::statvfs(ioctx_t* ctx, struct statvfs* stvfs)
{
	return common_statvfs(ctx, stvfs, dev);
}

bool PTS::RegisterPTY(Ref<Inode> pty, int ptynum)
{
	ino_t ino;
	if ( __builtin_add_overflow(ptynum, 2, &ino) )
		return errno = EMFILE, false;
	ScopedLock lock(&dirlock);
	if ( entries_count == entries_allocated )
	{
		size_t new_allocated = 2 * entries_allocated;
		if ( !new_allocated)
			new_allocated = 16;
		struct Entry* new_entries = new Entry[new_allocated];
		if ( !new_entries )
			return false;
		for ( size_t i = 0; i < entries_count; i++ )
		{
			new_entries[i] = entries[i];
			entries[i].inode.Reset();
		}
		delete[] entries;
		entries = new_entries;
		entries_allocated = new_allocated;
	}
	struct Entry* entry = &entries[entries_count++];
	snprintf(entry->name, sizeof(entry->name), "%i", ptynum);
	entry->ino = ino;
	entry->inode = pty;
	return true;
}

void PTS::UnregisterPTY(int ptynum)
{
	ino_t ino = (ino_t) 2 + (ino_t) ptynum;
	ScopedLock lock(&dirlock);
	bool found = false;
	for ( size_t i = 0; i < entries_count; i++ )
	{
		if ( entries[i].ino == ino )
		{
			entries[i].inode.Reset();
			if ( i + 1 != entries_count )
			{
				entries[i] = entries[entries_count-1];
				entries[entries_count-1].inode.Reset();
			}
			entries_count--;
			found = true;
			break;
		}
	}
	assert(found);
	if ( 16 < entries_allocated && entries_count <= entries_allocated / 4 )
	{
		size_t new_allocated = entries_allocated / 2;
		struct Entry* new_entries = new Entry[new_allocated];
		if ( !new_entries )
			return;
		for ( size_t i = 0; i < entries_count; i++ )
		{
			new_entries[i] = entries[i];
			entries[i].inode.Reset();
		}
		delete[] entries;
		entries = new_entries;
		entries_allocated = new_allocated;
	}
}

class PTY : public TTY
{
public:
	PTY(dev_t dev, ino_t ino, mode_t mode, uid_t owner, gid_t group,
	    int ptynum);
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
	short PollMasterEventStatus();

private:
	PollChannel master_poll_channel;
	struct winsize ws;
	kthread_cond_t output_ready_cond;
	kthread_cond_t output_possible_cond;
	size_t output_offset;
	size_t output_used;
	static const size_t output_size = 4096;
	uint8_t output[output_size];
	int ptynum;

};

PTY::PTY(dev_t dev, ino_t ino, mode_t mode, uid_t owner, gid_t group,
         int ptynum) : TTY(dev, ino, mode, owner, group, "")
{
	tio.c_cflag |= CREAD;
	output_ready_cond = KTHREAD_COND_INITIALIZER;
	output_possible_cond = KTHREAD_COND_INITIALIZER;
	output_offset = 0;
	output_used = 0;
	memset(&ws, 0, sizeof(ws));
	this->ptynum = ptynum;
	snprintf(ttyname, sizeof(ttyname), "pts/%i", ptynum);
}

PTY::~PTY()
{
	FreePTYNumber(ptynum);
}

ssize_t PTY::master_read(ioctx_t* ctx, uint8_t* buf, size_t count)
{
	ScopedLockSignal lock(&termlock);
	if ( !lock.IsAcquired() )
		return errno = EINTR, -1;
	while ( !output_used )
	{
		if ( ctx->dflags & O_NONBLOCK )
			return errno = EWOULDBLOCK, -1;
		if ( !kthread_cond_wait_signal(&output_ready_cond, &termlock) )
			return errno = EINTR, -1;
	}
	size_t sofar = 0;
	while ( output_used && sofar < count )
	{
		size_t limit = output_size - output_offset;
		size_t possible = limit < output_used ? limit : output_used;
		size_t left = count - sofar;
		size_t amount = left < possible ? left : possible;
		if ( !ctx->copy_to_dest(buf + sofar, output + output_offset, amount) )
			return sofar ? (ssize_t) sofar : -1;
		output_used -= amount;
		output_offset += amount;
		if ( output_offset == output_size )
			output_offset = 0;
		sofar += amount;
		kthread_cond_broadcast(&output_possible_cond);
	}
	return (ssize_t) sofar;
}

// TODO: Have this be non-blocking and have master_poll_channel signal POLLOUT
//       only when it won't block.
ssize_t PTY::master_write(ioctx_t* ctx, const uint8_t* buf, size_t count)
{
	ScopedLockSignal lock(&termlock);
	if ( !lock.IsAcquired() )
		return errno = EINTR, -1;
	size_t sofar = 0;
	while ( sofar < count )
	{
		uint8_t input[1024];
		size_t left = count - sofar;
		size_t amount = left < sizeof(input) ? left : sizeof(input);
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
// TODO: Alternatively the master is supposed to use non-blocking writes and
//       check for pending input as well. This function needs to be changed to
//       allow non-blocking input as well, needs an ioctx and ability to do
//       partial work.
void PTY::tty_output(const unsigned char* buffer, size_t length) // termlock held
{
	while ( length )
	{
		 while ( output_used == output_size )
			if ( !kthread_cond_wait_signal(&output_possible_cond, &termlock) )
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
		kthread_cond_broadcast(&output_ready_cond);
		master_poll_channel.Signal(POLLIN | POLLRDNORM);
	}
}

short PTY::PollMasterEventStatus()
{
	short status = 0;
	if ( output_used )
		status |= POLLIN | POLLRDNORM;
	if ( true /* can always write */ )
		status |= POLLOUT | POLLWRNORM;
	return status;
}

int PTY::master_poll(ioctx_t* /*ctx*/, PollNode* node)
{
	ScopedLock lock(&termlock);
	short ret_status = PollMasterEventStatus() & node->events;
	if ( ret_status )
	{
		node->master->revents |= ret_status;
		return 0;
	}
	master_poll_channel.Register(node);
	return errno = EAGAIN, -1;
}

int PTY::sync(ioctx_t* /*ctx*/)
{
	ScopedLock lock(&termlock);
	if ( hungup )
		return errno = EIO, -1;
	return 0;
}

int PTY::ioctl(ioctx_t* ctx, int cmd, uintptr_t arg)
{
	ScopedLock lock(&termlock);
	if ( hungup )
		return errno = EIO, -1;
	if ( cmd == TIOCGWINSZ )
	{
		struct winsize* user_ws = (struct winsize*) arg;
		if ( !ctx->copy_to_dest(user_ws, &ws, sizeof(ws)) )
			return -1;
		return 0;
	}
	else if ( cmd == TIOCGPTN )
	{
		int* arg_ptr = (int*) arg;
		if ( !ctx->copy_to_dest(arg_ptr, &ptynum, sizeof(ptynum)) )
			return -1;
		return 0;
	}
	lock.Reset();
	return TTY::ioctl(ctx, cmd, arg);
}

int PTY::master_ioctl(ioctx_t* ctx, int cmd, uintptr_t arg)
{
	if ( cmd == TIOCSWINSZ )
	{
		ScopedLock lock1(&termlock);
		const struct winsize* user_ws = (const struct winsize*) arg;
		if ( !ctx->copy_from_src(&ws, user_ws, sizeof(ws)) )
			return -1;

		ScopedLock lock2(&process_family_lock);
		if ( Process* process = CurrentProcess()->GetPTable()->Get(foreground_pgid) )
			process->DeliverGroupSignal(SIGWINCH);
		return 0;
	}
	return ioctl(ctx, cmd, arg);
}

class MasterNode : public AbstractInode
{
public:
	MasterNode(uid_t owner, gid_t group, mode_t mode, Ref<PTY> pty, int ptynum);
	virtual ~MasterNode();
	virtual ssize_t read(ioctx_t* ctx, uint8_t* buf, size_t count);
	virtual ssize_t write(ioctx_t* ctx, const uint8_t* buf, size_t count);
	virtual int poll(ioctx_t* ctx, PollNode* node);
	virtual int ioctl(ioctx_t* ctx, int cmd, uintptr_t arg);

public:
	Ref<PTY> pty;
	int ptynum;

};

MasterNode::MasterNode(uid_t owner, gid_t group, mode_t mode, Ref<PTY> pty,
                       int ptynum) : pty(pty)
{
	inode_type = INODE_TYPE_TTY;
	this->dev = (dev_t) this;
	this->ino = (ino_t) this;
	this->stat_uid = owner;
	this->stat_gid = group;
	this->type = S_IFCHR;
	this->stat_mode = (mode & S_SETABLE) | this->type;
	this->ptynum = ptynum;
}

MasterNode::~MasterNode()
{
	pts->UnregisterPTY(ptynum);
	pty->hup();
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

PTMX::PTMX(dev_t dev, ino_t ino, mode_t mode, uid_t owner, gid_t group)
{
	inode_type = INODE_TYPE_TTY;
	this->dev = dev;
	this->ino = ino;
	this->type = S_IFFACTORY | S_IFFACTORY_NOSTAT;
	this->stat_mode = (mode & S_SETABLE) | S_IFCHR;
	this->stat_uid = owner;
	this->stat_gid = group;
}

PTMX::~PTMX()
{
}

Ref<Inode> PTMX::factory(ioctx_t* ctx, const char* filename, int flags,
	                     mode_t mode)
{
	(void) ctx;
	(void) filename;
	(void) flags;
	(void) mode;
	Process* process = CurrentProcess();
	uid_t uid = process->uid;
	uid_t gid = process->gid;
	mode_t new_mode = 0620;
	int ptynum = AllocatePTYNumber();
	if ( ptynum < 0 )
		return Ref<Inode>(NULL);
	Ref<PTY> slave_inode(new PTY(pts->dev, 2 + (ino_t) ptynum, mode, uid, gid,
	                             ptynum));
	if ( !slave_inode )
		return FreePTYNumber(ptynum), Ref<Inode>(NULL);
	if ( !pts->RegisterPTY(slave_inode, ptynum) )
		return Ref<Inode>(NULL);
	Ref<MasterNode> master_inode(new MasterNode(uid, gid, new_mode, slave_inode,
	                                            ptynum));
	if ( !master_inode )
	{
		pts->UnregisterPTY(ptynum);
		return Ref<Inode>(NULL);
	}
	return master_inode;
}


int sys_mkpty(int* master_fd_user, int* slave_fd_user, int flags)
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
	mode_t mode = 0620;

	int ptynum = AllocatePTYNumber();
	if ( ptynum < 0 )
		return -1;

	Ref<PTY> slave_inode(new PTY(pts->dev, 2 + ptynum, mode, uid, gid, ptynum));
	if ( !slave_inode )
		return FreePTYNumber(ptynum), -1;
	if ( !pts->RegisterPTY(slave_inode, ptynum) )
		return -1;
	Ref<MasterNode> master_inode(new MasterNode(uid, gid, mode, slave_inode,
	                                            ptynum));
	if ( !master_inode )
	{
		pts->UnregisterPTY(ptynum);
		return -1;
	}

	Ref<Vnode> master_vnode(new Vnode(master_inode, Ref<Vnode>(NULL), 0, 0));
	Ref<Vnode> slave_vnode(new Vnode(slave_inode, Ref<Vnode>(NULL), 0, 0));
	master_inode.Reset();
	slave_inode.Reset();
	if ( !master_vnode || !slave_vnode )
		return -1;

	Ref<Descriptor> master_desc(
		new Descriptor(master_vnode, O_READ | O_WRITE | flags));
	Ref<Descriptor> slave_desc(
		new Descriptor(slave_vnode, O_READ | O_WRITE | flags));
	master_vnode.Reset();
	slave_vnode.Reset();
	if ( !master_desc || !slave_desc )
		return -1;

	Ref<DescriptorTable> dtable = process->GetDTable();
	int reservation = 0;
	if ( !dtable->Reserve(2, &reservation) )
		return -1;
	int master_fd = dtable->Allocate(master_desc, fdflags, 0, &reservation);
	int slave_fd = dtable->Allocate(slave_desc, fdflags, 0, &reservation);
	assert(0 <= master_fd);
	assert(0 <= slave_fd);
	master_desc.Reset();
	slave_desc.Reset();
	dtable.Reset();

	if ( !CopyToUser(master_fd_user, &master_fd, sizeof(int)) ||
	     !CopyToUser(slave_fd_user, &slave_fd, sizeof(int)) )
		return -1;

	return 0;
}

} // namespace Sortix
