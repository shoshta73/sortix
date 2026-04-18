/*
 * Copyright (c) 2016, 2026 Jonas 'Sortie' Termansen.
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
 * pty.h
 * Pseudoterminals.
 */

#ifndef SORTIX_PTY_H
#define SORTIX_PTY_H

#include <sortix/kernel/kthread.h>
#include <sortix/kernel/inode.h>

namespace Sortix {

class PTS : public AbstractInode
{
	struct Entry;

public:
	PTS(mode_t mode, uid_t owner, gid_t group);
	virtual ~PTS();

public:
	virtual ssize_t getdents(ioctx_t* ctx, void* buf, size_t size, int flags,
	                         off_t* offset);
	virtual Ref<Inode> open(ioctx_t* ctx, const char* filename, int flags,
	                        mode_t mode);
	virtual int mkdir(ioctx_t* ctx, const char* filename, mode_t mode);
	virtual int link(ioctx_t* ctx, const char* filename, Ref<Inode> node);
	virtual int link_raw(ioctx_t* ctx, const char* filename, Ref<Inode> node);
	virtual int unlink(ioctx_t* ctx, const char* filename);
	virtual int unlink_raw(ioctx_t* ctx, const char* filename);
	virtual int rmdir(ioctx_t* ctx, const char* filename);
	virtual int rmdir_me(ioctx_t* ctx);
	virtual int symlink(ioctx_t* ctx, const char* oldname,
	                    const char* filename);
	virtual int rename_here(ioctx_t* ctx, Ref<Inode> from, const char* oldname,
	                        const char* newname);
	virtual ssize_t tcgetblob(ioctx_t* ctx, const char* name, void* buffer,
	                          size_t count);
	virtual int statvfs(ioctx_t* ctx, struct statvfs* stvfs);

public:
	bool RegisterPTY(Ref<Inode> pty, int ptynum);
	void UnregisterPTY(int ptynum);

private:
	bool ContainsFile(const char* name);

private:
	kthread_mutex_t dirlock;
	struct Entry* entries;
	size_t entries_count;
	size_t entries_allocated;

};

extern Ref<PTS> pts;

} // namespace Sortix

#endif
