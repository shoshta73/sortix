/*
 * Copyright (c) 2013, 2014, 2015, 2025 Jonas 'Sortie' Termansen.
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
 * fuse.cpp
 * FUSE frontend.
 */

#if !defined(__sortix__)

#include <sys/stat.h>
#include <sys/types.h>

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FUSE_USE_VERSION 26
#include <fuse.h>

#include "block.h"
#include "device.h"
#include "fat.h"
#include "fatfs.h"
#include "filesystem.h"
#include "fuse.h"
#include "inode.h"

struct fat_fuse_ctx
{
	Device* dev;
	Filesystem* fs;
};

#ifndef S_SETABLE
#define S_SETABLE 02777
#endif

#define FUSE_FS (((struct fat_fuse_ctx*) (fuse_get_context()->private_data))->fs)

void* fat_fuse_init(struct fuse_conn_info* /*conn*/)
{
	return fuse_get_context()->private_data;
}

void fat_fuse_destroy(void* fs_private)
{
	struct fat_fuse_ctx* fat_fuse_ctx = (struct fat_fuse_ctx*) fs_private;
	if ( fat_fuse_ctx->dev->write )
	{
		fat_fuse_ctx->fs->Sync();
		fat_fuse_ctx->fs->MarkUnmounted();
	}
	delete fat_fuse_ctx->fs; fat_fuse_ctx->fs = NULL;
	delete fat_fuse_ctx->dev; fat_fuse_ctx->dev = NULL;
}

Inode* fat_fuse_resolve_path(const char* path)
{
	Filesystem* fs = FUSE_FS;
	Inode* inode = fs->GetInode(fs->root_inode_id);
	if ( !inode )
		return (Inode*) NULL;
	while ( path[0] )
	{
		if ( *path == '/' )
		{
			if ( !S_ISDIR(inode->Mode()) )
				return inode->Unref(), errno = ENOTDIR, (Inode*) NULL;
			path++;
			continue;
		}
		size_t elem_len = strcspn(path, "/");
		char* elem = strndup(path, elem_len);
		if ( !elem )
			return inode->Unref(), errno = ENOTDIR, (Inode*) NULL;
		path += elem_len;
		Inode* next = inode->Open(elem, O_RDONLY, 0);
		free(elem);
		inode->Unref();
		if ( !next )
			return NULL;
		inode = next;
	}
	return inode;
}

// Assumes that the path doesn't end with / unless it's the root directory.
Inode* fat_fuse_parent_dir(const char** path_ptr)
{
	const char* path = *path_ptr;
	Filesystem* fs = FUSE_FS;
	Inode* inode = fs->GetInode(fs->root_inode_id);
	if ( !inode )
		return (Inode*) NULL;
	while ( strchr(path, '/') )
	{
		if ( *path == '/' )
		{
			if ( !S_ISDIR(inode->Mode()) )
				return inode->Unref(), errno = ENOTDIR, (Inode*) NULL;
			path++;
			continue;
		}
		size_t elem_len = strcspn(path, "/");
		char* elem = strndup(path, elem_len);
		if ( !elem )
			return inode->Unref(), errno = ENOTDIR, (Inode*) NULL;
		path += elem_len;
		Inode* next = inode->Open(elem, O_RDONLY, 0);
		free(elem);
		inode->Unref();
		if ( !next )
			return (Inode*) NULL;
		inode = next;
	}
	*path_ptr = *path ? path : ".";
	assert(!strchr(*path_ptr, '/'));
	return inode;
}

int fat_fuse_getattr(const char* path, struct stat* st)
{
	Inode* inode = fat_fuse_resolve_path(path);
	if ( !inode )
		return -errno;
	inode->Stat(st);
	inode->Unref();
	return 0;
}

int fat_fuse_fgetattr(const char* /*path*/, struct stat* st,
                       struct fuse_file_info* fi)
{
	Filesystem* fs = FUSE_FS;
	Inode* inode = fs->GetInode((fat_ino_t) fi->fh);
	if ( !inode )
		return -errno;
	inode->Stat(st);
	inode->Unref();
	return 0;
}

int fat_fuse_readlink(const char* path, char* buf, size_t bufsize)
{
	Inode* inode = fat_fuse_resolve_path(path);
	if ( !inode )
		return -errno;
	if ( !S_ISLNK(inode->Mode()) )
		return inode->Unref(), -(errno = EINVAL);
	if ( !bufsize )
		return inode->Unref(), -(errno = EINVAL);
	ssize_t amount = inode->ReadAt((uint8_t*) buf, bufsize, 0);
	if ( amount < 0 )
		return inode->Unref(), -errno;
	buf[(size_t) amount < bufsize ? (size_t) amount : bufsize - 1] = '\0';
	inode->Unref();
	return 0;
}

int fat_fuse_mknod(const char* path, mode_t mode, dev_t dev)
{
	(void) path;
	(void) mode;
	(void) dev;
	return -(errno = ENOSYS);
}

int fat_fuse_mkdir(const char* path, mode_t mode)
{
	Inode* inode = fat_fuse_parent_dir(&path);
	if ( !inode )
		return -errno;
	Inode* newdir = inode->CreateDirectory(path, mode);
	inode->Unref();
	if ( !newdir )
		return -errno;
	newdir->Unref();
	return 0;
}

int fat_fuse_unlink(const char* path)
{
	Inode* inode = fat_fuse_parent_dir(&path);
	if ( !inode )
		return -errno;
	bool success = inode->Unlink(path, false);
	inode->Unref();
	return success ? 0 : -errno;
}

int fat_fuse_rmdir(const char* path)
{
	Inode* inode = fat_fuse_parent_dir(&path);
	if ( !inode )
		return -errno;
	bool success = inode->RemoveDirectory(path);
	inode->Unref();
	return success ? 0 : -errno;
}

int fat_fuse_symlink(const char* oldname, const char* newname)
{
	Inode* newdir = fat_fuse_parent_dir(&newname);
	if ( !newdir )
		return -errno;
	bool success = newdir->Symlink(newname, oldname);
	newdir->Unref();
	return success ? 0 : -errno;
}

int fat_fuse_rename(const char* oldname, const char* newname)
{
	Inode* olddir = fat_fuse_parent_dir(&oldname);
	if ( !olddir )
		return -errno;
	Inode* newdir = fat_fuse_parent_dir(&newname);
	if ( !newdir )
		return olddir->Unref(), -errno;
	bool success = newdir->Rename(olddir, oldname, newname);
	newdir->Unref();
	olddir->Unref();
	return success ? 0 : -errno;
}

int fat_fuse_link(const char* oldname, const char* newname)
{
	Inode* inode = fat_fuse_resolve_path(oldname);
	if ( !inode )
		return -errno;
	Inode* newdir = fat_fuse_parent_dir(&newname);
	if ( !newdir )
		return inode->Unref(), -errno;
	bool success = inode->Link(newname, inode, false);
	newdir->Unref();
	inode->Unref();
	return success ? 0 : -errno;
}

int fat_fuse_chmod(const char* path, mode_t mode)
{
	Inode* inode = fat_fuse_resolve_path(path);
	if ( !inode )
		return -errno;
	if ( !FUSE_FS->device->write )
		return inode->Unref(), -(errno = EROFS);
	int result = 0;
	if ( !inode->ChangeMode(mode) )
		result = -errno;
	inode->Unref();
	return result;
}

int fat_fuse_chown(const char* path, uid_t owner, gid_t group)
{
	Inode* inode = fat_fuse_resolve_path(path);
	if ( !inode )
		return -errno;
	if ( !FUSE_FS->device->write )
		return inode->Unref(), -(errno = EROFS);
	int result = 0;
	if ( !inode->ChangeOwner(owner, group) )
		result = -errno;
	inode->Unref();
	return result;
}

int fat_fuse_truncate(const char* path, off_t size)
{
	Inode* inode = fat_fuse_resolve_path(path);
	if ( !inode )
		return -errno;
	if ( !FUSE_FS->device->write )
		return inode->Unref(), -(errno = EROFS);
	inode->Truncate((uint64_t) size);
	inode->Unref();
	return 0;
}

int fat_fuse_ftruncate(const char* /*path*/, off_t size,
                        struct fuse_file_info* fi)
{
	Filesystem* fs = FUSE_FS;
	Inode* inode = fs->GetInode((fat_ino_t) fi->fh);
	if ( !inode )
		return -errno;
	if ( !FUSE_FS->device->write )
		return inode->Unref(), -(errno = EROFS);
	inode->Truncate((uint64_t) size);
	inode->Unref();
	return 0;
}

int fat_fuse_open(const char* path, struct fuse_file_info* fi)
{
	int flags = fi->flags;
	Inode* dir = fat_fuse_parent_dir(&path);
	if ( !dir )
		return -errno;
	Inode* result = dir->Open(path, flags, 0);
	dir->Unref();
	if ( !result )
		return -errno;
	fi->fh = (uint64_t) result->inode_id;
	fi->keep_cache = 1;
	result->RemoteRefer();
	result->Unref();
	return 0;
}

int fat_fuse_access(const char* path, int mode)
{
	Inode* dir = fat_fuse_parent_dir(&path);
	if ( !dir )
		return -errno;
	Inode* result = dir->Open(path, O_RDONLY, 0);
	dir->Unref();
	if ( !result )
		return -errno;
	(void) mode;
	result->Unref();
	return 0;
}

int fat_fuse_create(const char* path, mode_t mode, struct fuse_file_info* fi)
{
	int flags = fi->flags | O_CREAT;
	Inode* inode = fat_fuse_parent_dir(&path);
	if ( !inode )
		return -errno;
	Inode* result = inode->Open(path, flags, mode);
	inode->Unref();
	if ( !result )
		return -errno;
	fi->fh = (uint64_t) result->inode_id;
	fi->keep_cache = 1;
	result->RemoteRefer();
	result->Unref();
	return 0;
}

int fat_fuse_opendir(const char* path, struct fuse_file_info* fi)
{
	return fat_fuse_open(path, fi);
}

int fat_fuse_read(const char* /*path*/, char* buf, size_t count, off_t offset,
                   struct fuse_file_info* fi)
{
	Filesystem* fs = FUSE_FS;
	if ( INT_MAX < count )
		count = INT_MAX;
	Inode* inode = fs->GetInode((fat_ino_t) fi->fh);
	if ( !inode )
		return -errno;
	ssize_t result = inode->ReadAt((uint8_t*) buf, count, offset);
	inode->Unref();
	return 0 <= result ? (int) result : -errno;
}

int fat_fuse_write(const char* /*path*/, const char* buf, size_t count,
                    off_t offset, struct fuse_file_info* fi)
{
	Filesystem* fs = FUSE_FS;
	if ( INT_MAX < count )
		count = INT_MAX;
	Inode* inode = fs->GetInode((fat_ino_t) fi->fh);
	if ( !inode )
		return -errno;
	ssize_t result = inode->WriteAt((const uint8_t*) buf, count, offset);
	inode->Unref();
	return 0 <= result ? (int) result : -errno;
}

int fat_fuse_statfs(const char* /*path*/, struct statvfs* stvfs)
{
	memset(stvfs, 0, sizeof(*stvfs));
	Filesystem* fs = FUSE_FS;
	stvfs->f_bsize = fs->cluster_size;
	stvfs->f_frsize = fs->cluster_size;
	stvfs->f_blocks = fs->cluster_count;
	stvfs->f_bfree = fs->CalculateFreeCount();
	stvfs->f_bavail = stvfs->f_bfree;
	stvfs->f_files = stvfs->f_blocks;
	stvfs->f_ffree = stvfs->f_bfree;
	stvfs->f_favail = stvfs->f_files;
	stvfs->f_fsid = 0;
	stvfs->f_flag = 0;
	if ( !fs->device->write )
		stvfs->f_flag |= ST_RDONLY;
	stvfs->f_namemax = FAT_UTF16_NAME_MAX;
	return 0;
}

int fat_fuse_flush(const char* /*path*/, struct fuse_file_info* fi)
{
	Filesystem* fs = FUSE_FS;
	Inode* inode = fs->GetInode((fat_ino_t) fi->fh);
	if ( !inode )
		return -errno;
	inode->Sync();
	inode->Unref();
	return 0;
}

int fat_fuse_release(const char* /*path*/, struct fuse_file_info* fi)
{
	Filesystem* fs = FUSE_FS;
	Inode* inode = fs->GetInode((fat_ino_t) fi->fh);
	if ( !inode )
		return -errno;
	inode->RemoteUnref();
	inode->Unref();
	return 0;
}

int fat_fuse_releasedir(const char* path, struct fuse_file_info* fi)
{
	return fat_fuse_release(path, fi);
}

int fat_fuse_fsync(const char* /*path*/, int data, struct fuse_file_info* fi)
{
	(void) data;
	Filesystem* fs = FUSE_FS;
	Inode* inode = fs->GetInode((fat_ino_t) fi->fh);
	if ( !inode )
		return -errno;
	inode->Sync();
	inode->Unref();
	return 0;
}

/*int fat_fuse_syncdir(const char* path, int data, struct fuse_file_info* fi)
{
	return fat_fuse_sync(path, data, fi);
}*/

/*int fat_fuse_setxattr(const char *, const char *, const char *, size_t, int)
{
	return -(errno = ENOSYS);
}*/

/*int fat_fuse_getxattr(const char *, const char *, char *, size_t)
{
	return -(errno = ENOSYS);
}*/

/*int fat_fuse_listxattr(const char *, char *, size_t)
{
	return -(errno = ENOSYS);
}*/

/*int fat_fuse_removexattr(const char *, const char *)
{
	return -(errno = ENOSYS);
}*/

int fat_fuse_readdir(const char* /*path*/, void* buf, fuse_fill_dir_t filler,
                      off_t rec_num, struct fuse_file_info* fi)
{
	Filesystem* fs = FUSE_FS;
	Inode* inode = fs->GetInode((fat_ino_t) fi->fh);
	if ( !inode )
		return -errno;
	if ( !S_ISDIR(inode->Mode()) )
		return inode->Unref(), -(errno = ENOTDIR);
	struct position next_position;
	next_position.cluster = inode->first_cluster;
	next_position.sector = 0;
	next_position.offset = 0;
	Block* block = NULL;
	char name[FAT_UTF8_NAME_MAX + 1];
	uint8_t file_type;
	fat_ino_t inode_id;
	struct fat_dirent* entry;
	while ( inode->ReadDirectory(&block, &next_position, rec_num ? NULL : name,
	                             &file_type, &inode_id, &entry, NULL, NULL,
	                             NULL) )
	{
		if ( !rec_num || !rec_num-- )
		{
			if ( filler(buf, name, NULL, 0) )
			{
				if ( block )
					block->Unref();
				inode->Unref();
				return 0;
			}
		}
	}
	int errnum = errno;
	if ( block )
		block->Unref();
	inode->Unref();
	if ( errnum )
		return -errnum;
	return 0;
}

/*int fat_fuse_lock(const char*, struct fuse_file_info*, int, struct flock*)
{
	return -(errno = ENOSYS);
}*/

int fat_fuse_utimens(const char* path, const struct timespec tv[2])
{
	Inode* inode = fat_fuse_resolve_path(path);
	if ( !inode )
		return -errno;
	if ( !FUSE_FS->device->write )
		return inode->Unref(), -(errno = EROFS);
	inode->UTimens(tv);
	inode->Unref();
	return 0;
}

/*int fat_fuse_bmap(const char*, size_t blocksize, uint64_t* idx)
{
	return -(errno = ENOSYS);
}*/

int fat_fuse_main(const char* argv0,
                   const char* mount_path,
                   const char* fuse_options,
                   bool foreground,
                   Filesystem* fs,
                   Device* dev)
{
	struct fuse_operations operations;
	memset(&operations, 0, sizeof(operations));

	operations.access = fat_fuse_access;
	operations.chmod = fat_fuse_chmod;
	operations.chown = fat_fuse_chown;
	operations.create = fat_fuse_create;
	operations.destroy = fat_fuse_destroy;
	operations.fgetattr = fat_fuse_fgetattr;
	operations.flush = fat_fuse_flush;
	operations.fsync = fat_fuse_fsync;
	operations.ftruncate = fat_fuse_ftruncate;
	operations.getattr = fat_fuse_getattr;
	operations.init = fat_fuse_init;
	operations.link = fat_fuse_link;
	operations.mkdir = fat_fuse_mkdir;
	operations.mknod = fat_fuse_mknod;
	operations.opendir = fat_fuse_opendir;
	operations.open = fat_fuse_open;
	operations.readdir = fat_fuse_readdir;
	operations.read = fat_fuse_read;
	operations.readlink = fat_fuse_readlink;
	operations.releasedir = fat_fuse_releasedir;
	operations.release = fat_fuse_release;
	operations.rename = fat_fuse_rename;
	operations.rmdir = fat_fuse_rmdir;
	operations.statfs = fat_fuse_statfs;
	operations.symlink = fat_fuse_symlink;
	operations.truncate = fat_fuse_truncate;
	operations.unlink = fat_fuse_unlink;
	operations.utimens = fat_fuse_utimens;
	operations.write = fat_fuse_write;

	operations.flag_nullpath_ok = 1;
	operations.flag_nopath = 1;

	char* argv_fuse[] =
	{
		(char*) argv0,
		(char*) "-ouse_ino",
		(char*) "-o",
		(char*) (fuse_options ? fuse_options : "use_ino"),
		(char*) "-s",
		(char*) (foreground ? "-f" : mount_path),
		(char*) (foreground ? mount_path : NULL),
		(char*) NULL,
	};

	int argc_fuse = 0;
	while ( argv_fuse[argc_fuse] )
		argc_fuse++;

	struct fat_fuse_ctx fat_fuse_ctx;
	fat_fuse_ctx.fs = fs;
	fat_fuse_ctx.dev = dev;

	(void) foreground;

	return fuse_main(argc_fuse, argv_fuse, &operations, &fat_fuse_ctx);
}

#endif
