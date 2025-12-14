/*
 * Copyright (c) 2012-2017, 2021, 2025 Jonas 'Sortie' Termansen.
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
 * sortix/kernel/descriptor.h
 * A file descriptor.
 */

#ifndef _INCLUDE_SORTIX_KERNEL_DESCRIPTOR_H
#define _INCLUDE_SORTIX_KERNEL_DESCRIPTOR_H

#include <sys/types.h>

#include <stdint.h>

#include <sortix/timespec.h>

#include <sortix/kernel/kthread.h>
#include <sortix/kernel/refcount.h>

struct dirent;
struct iovec;
struct msghdr;
struct stat;
struct statvfs;
struct termios;
struct wincurpos;
struct winsize;

namespace Sortix {

class PollNode;
class Inode;
class Vnode;
struct ioctx_struct;
typedef struct ioctx_struct ioctx_t;

class Descriptor : public Refcountable
{
private:
	Descriptor();
	void LateConstruct(Ref<Vnode> vnode, int dflags);

public:
	Descriptor(Ref<Vnode> vnode, int dflags);
	virtual ~Descriptor();
	Ref<Descriptor> Fork();
	bool SetFlags(int new_dflags);
	int GetFlags();
	bool pass();
	void unpass();
	int sync(ioctx_t* ctx);
	int stat(ioctx_t* ctx, struct stat* st);
	int statvfs(ioctx_t* ctx, struct statvfs* stvfs);
	int chmod(ioctx_t* ctx, mode_t mode);
	int chown(ioctx_t* ctx, uid_t owner, gid_t group);
	int truncate(ioctx_t* ctx, off_t length);
	long pathconf(ioctx_t* ctx, int name);
	off_t lseek(ioctx_t* ctx, off_t offset, int whence);
	ssize_t read(ioctx_t* ctx, uint8_t* buf, size_t count);
	ssize_t readv(ioctx_t* ctx, const struct iovec* iov, int iovcnt);
	ssize_t pread(ioctx_t* ctx, uint8_t* buf, size_t count, off_t off);
	ssize_t preadv(ioctx_t* ctx, const struct iovec* iov, int iovcnt,
	               off_t off);
	ssize_t write(ioctx_t* ctx, const uint8_t* buf, size_t count);
	ssize_t writev(ioctx_t* ctx, const struct iovec* iov, int iovcnt);
	ssize_t pwrite(ioctx_t* ctx, const uint8_t* buf, size_t count, off_t off);
	ssize_t pwritev(ioctx_t* ctx, const struct iovec* iov, int iovcnt,
	                off_t off);
	int utimens(ioctx_t* ctx, const struct timespec* times);
	int isatty(ioctx_t* ctx);
	ssize_t readdirents(ioctx_t* ctx, struct dirent* dirent, size_t size);
	Ref<Descriptor> open(ioctx_t* ctx, const char* filename, int flags,
	                     mode_t mode = 0);
	int mkdir(ioctx_t* ctx, const char* filename, mode_t mode);
	int link(ioctx_t* ctx, const char* filename, Ref<Descriptor> node);
	int unlinkat(ioctx_t* ctx, const char* filename, int flags);
	int symlink(ioctx_t* ctx, const char* oldname, const char* filename);
	ssize_t readlink(ioctx_t* ctx, char* buf, size_t bufsiz);
	int tcgetwincurpos(ioctx_t* ctx, struct wincurpos* wcp);
	int ioctl(ioctx_t* ctx, int cmd, uintptr_t arg);
	int tcsetpgrp(ioctx_t* ctx, pid_t pgid);
	pid_t tcgetpgrp(ioctx_t* ctx);
	int poll(ioctx_t* ctx, PollNode* node);
	int rename_here(ioctx_t* ctx, Ref<Descriptor> from, const char* oldpath,
	                const char* newpath);
	Ref<Descriptor> accept4(ioctx_t* ctx, uint8_t* addr, size_t* addrlen,
	                        int flags);
	int bind(ioctx_t* ctx, const uint8_t* addr, size_t addrlen);
	int connect(ioctx_t* ctx, const uint8_t* addr, size_t addrlen);
	int listen(ioctx_t* ctx, int backlog);
	ssize_t recv(ioctx_t* ctx, uint8_t* buf, size_t count, int flags);
	ssize_t recvmsg(ioctx_t* ctx, struct msghdr* msg, int flags);
	ssize_t send(ioctx_t* ctx, const uint8_t* buf, size_t count, int flags);
	ssize_t sendmsg(ioctx_t* ctx, const struct msghdr* msg, int flags);
	int getsockopt(ioctx_t* ctx, int level, int option_name,
	               void* option_value, size_t* option_size_ptr);
	int setsockopt(ioctx_t* ctx, int level, int option_name,
	               const void* option_value, size_t option_size);
	ssize_t tcgetblob(ioctx_t* ctx, const char* name, void* buffer, size_t count);
	ssize_t tcsetblob(ioctx_t* ctx, const char* name, const void* buffer, size_t count);
	int unmount(ioctx_t* ctx, const char* filename, int flags);
	int fsm_fsbind(ioctx_t* ctx, Ref<Descriptor> target, int flags);
	Ref<Descriptor> fsm_mount(ioctx_t* ctx, const char* filename,
	                          const struct stat* rootst, int flags);
	int tcdrain(ioctx_t* ctx);
	int tcflow(ioctx_t* ctx, int action);
	int tcflush(ioctx_t* ctx, int queue_selector);
	int tcgetattr(ioctx_t* ctx, struct termios* tio);
	pid_t tcgetsid(ioctx_t* ctx);
	int tcsendbreak(ioctx_t* ctx, int duration);
	int tcsetattr(ioctx_t* ctx, int actions, const struct termios* tio);
	int shutdown(ioctx_t* ctx, int how);
	int getpeername(ioctx_t* ctx, uint8_t* addr, size_t* addrsize);
	int getsockname(ioctx_t* ctx, uint8_t* addr, size_t* addrsize);
	int sockatmark(ioctx_t* ctx);

private:
	Ref<Descriptor> open_elem(ioctx_t* ctx, const char* filename, int flags,
	                          mode_t mode);
	bool IsSeekable();

public: /* These must never change after construction. */
	ino_t ino;
	dev_t dev;
	mode_t type; // For use by S_IS* macros.

public:
	Ref<Vnode> vnode;

private:
	kthread_mutex_t current_offset_lock;
	off_t current_offset;
	int dflags;
	bool seekable;
	bool checked_seekable;

};

int LinkInodeInDir(ioctx_t* ctx, Ref<Descriptor> dir, const char* name,
                   Ref<Inode> inode);
Ref<Descriptor> OpenDirContainingPath(ioctx_t* ctx, Ref<Descriptor> from,
                                      const char* path, char** finalp);
size_t TruncateIOVec(struct iovec* iov, int iovcnt, off_t limit);

} // namespace Sortix

#endif
