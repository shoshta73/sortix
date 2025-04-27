/*
 * Copyright (c) 2013, 2014, 2015, 2022 Jonas 'Sortie' Termansen.
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
#include <ctype.h>
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
#include "filesystem.h"
#include "fuse.h"
#include "inode.h"
#include "iso9660fs.h"

#include <stdio.h> // DEBUG

struct iso9660_fuse_ctx
{
	Device* dev;
	Filesystem* fs;
};

#ifndef S_SETABLE
#define S_SETABLE 02777
#endif

#define FUSE_FS (((struct iso9660_fuse_ctx*) (fuse_get_context()->private_data))->fs)

void* iso9660_fuse_init(struct fuse_conn_info* /*conn*/)
{
	return fuse_get_context()->private_data;
}

void iso9660_fuse_destroy(void* fs_private)
{
	struct iso9660_fuse_ctx* iso9660_fuse_ctx =
		(struct iso9660_fuse_ctx*) fs_private;
	while ( iso9660_fuse_ctx->fs->mru_inode )
	{
		Inode* inode = iso9660_fuse_ctx->fs->mru_inode;
		if ( inode->remote_reference_count )
			inode->RemoteUnref();
		else if ( inode->reference_count )
			inode->Unref();
	}
	delete iso9660_fuse_ctx->fs; iso9660_fuse_ctx->fs = NULL;
	delete iso9660_fuse_ctx->dev; iso9660_fuse_ctx->dev = NULL;
}

Inode* iso9660_fuse_resolve_path(const char* path)
{
	Filesystem* fs = FUSE_FS;
	Inode* inode = fs->GetInode(fs->root_ino);
	if ( !inode )
		return (Inode*) NULL;
	while ( path[0] )
	{
		if ( *path == '/' )
		{
			if ( !ISO9660_S_ISDIR(inode->Mode()) )
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
Inode* iso9660_fuse_parent_dir(const char** path_ptr)
{
	const char* path = *path_ptr;
	Filesystem* fs = FUSE_FS;
	Inode* inode = fs->GetInode(fs->root_ino);
	if ( !inode )
		return (Inode*) NULL;
	while ( strchr(path, '/') )
	{
		if ( *path == '/' )
		{
			if ( !ISO9660_S_ISDIR(inode->Mode()) )
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

int iso9660_fuse_getattr(const char* path, struct stat* st)
{
	Inode* inode = iso9660_fuse_resolve_path(path);
	if ( !inode )
		return -errno;
	StatInode(inode, st);
	inode->Unref();
	return 0;
}

int iso9660_fuse_fgetattr(const char* /*path*/, struct stat* st,
                       struct fuse_file_info* fi)
{
	Filesystem* fs = FUSE_FS;
	Inode* inode = fs->GetInode((uint32_t) fi->fh);
	if ( !inode )
		return -errno;
	StatInode(inode, st);
	inode->Unref();
	return 0;
}

int iso9660_fuse_readlink(const char* path, char* buf, size_t bufsize)
{
	Inode* inode = iso9660_fuse_resolve_path(path);
	if ( !inode )
		return -errno;
	if ( !ISO9660_S_ISLNK(inode->Mode()) )
		return inode->Unref(), -(errno = EINVAL);
	if ( !bufsize )
		return inode->Unref(), -(errno = EINVAL);
	ssize_t amount = inode->ReadLink((uint8_t*) buf, bufsize);
	if ( amount < 0 )
		return inode->Unref(), -errno;
	buf[(size_t) amount < bufsize ? (size_t) amount : bufsize - 1] = '\0';
	inode->Unref();
	return 0;
}

int iso9660_fuse_mknod(const char* path, mode_t mode, dev_t dev)
{
	(void) path;
	(void) mode;
	(void) dev;
	return -(errno = ENOSYS);
}

int iso9660_fuse_mkdir(const char* path, mode_t /*mode*/)
{
	Inode* inode = iso9660_fuse_parent_dir(&path);
	if ( !inode )
		return -errno;
	inode->Unref();
	return -(errno = EROFS);
}

int iso9660_fuse_unlink(const char* path)
{
	Inode* inode = iso9660_fuse_parent_dir(&path);
	if ( !inode )
		return -errno;
	bool success = inode->Unlink(path, false);
	inode->Unref();
	return success ? 0 : -errno;
}

int iso9660_fuse_rmdir(const char* path)
{
	Inode* inode = iso9660_fuse_parent_dir(&path);
	if ( !inode )
		return -errno;
	bool success = inode->RemoveDirectory(path);
	inode->Unref();
	return success ? 0 : -errno;
}

int iso9660_fuse_symlink(const char* /*oldname*/, const char* newname)
{
	Inode* newdir = iso9660_fuse_parent_dir(&newname);
	if ( !newdir )
		return -errno;
	newdir->Unref();
	return -(errno = EROFS);
}

int iso9660_fuse_rename(const char* oldname, const char* newname)
{
	Inode* olddir = iso9660_fuse_parent_dir(&oldname);
	if ( !olddir )
		return -errno;
	Inode* newdir = iso9660_fuse_parent_dir(&newname);
	if ( !newdir )
		return olddir->Unref(), -errno;
	bool success = newdir->Rename(olddir, oldname, newname);
	newdir->Unref();
	olddir->Unref();
	return success ? 0 : -errno;
}

int iso9660_fuse_link(const char* oldname, const char* newname)
{
	Inode* inode = iso9660_fuse_resolve_path(oldname);
	if ( !inode )
		return -errno;
	Inode* newdir = iso9660_fuse_parent_dir(&newname);
	if ( !newdir )
		return inode->Unref(), -errno;
	bool success = inode->Link(newname, inode);
	newdir->Unref();
	inode->Unref();
	return success ? 0 : -errno;
}

int iso9660_fuse_chmod(const char* path, mode_t mode)
{
	Inode* inode = iso9660_fuse_resolve_path(path);
	if ( !inode )
		return -errno;
	(void) mode;
	return inode->Unref(), -(errno = EROFS);
}

int iso9660_fuse_chown(const char* path, uid_t owner, gid_t group)
{
	Inode* inode = iso9660_fuse_resolve_path(path);
	if ( !inode )
		return -errno;
	(void) owner;
	(void) group;
	return inode->Unref(), -(errno = EROFS);
}

int iso9660_fuse_truncate(const char* path, off_t size)
{
	Inode* inode = iso9660_fuse_resolve_path(path);
	if ( !inode )
		return -errno;
	(void) size;
	return inode->Unref(), -(errno = EROFS);
}

int iso9660_fuse_ftruncate(const char* /*path*/, off_t size,
                        struct fuse_file_info* fi)
{
	Filesystem* fs = FUSE_FS;
	Inode* inode = fs->GetInode((uint32_t) fi->fh);
	if ( !inode )
		return -errno;
	(void) size;
	return inode->Unref(), -(errno = EROFS);
}

int iso9660_fuse_open(const char* path, struct fuse_file_info* fi)
{
	int flags = fi->flags;
	Inode* dir = iso9660_fuse_parent_dir(&path);
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

int iso9660_fuse_access(const char* path, int mode)
{
	Inode* dir = iso9660_fuse_parent_dir(&path);
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

int iso9660_fuse_create(const char* path, mode_t mode, struct fuse_file_info* fi)
{
	int flags = fi->flags | O_CREAT;
	Inode* inode = iso9660_fuse_parent_dir(&path);
	if ( !inode )
		return -errno;
	Inode* result = inode->Open(path, flags, FsModeFromHostMode(mode));
	inode->Unref();
	if ( !result )
		return -errno;
	fi->fh = (uint64_t) result->inode_id;
	fi->keep_cache = 1;
	result->RemoteRefer();
	result->Unref();
	return 0;
}

int iso9660_fuse_opendir(const char* path, struct fuse_file_info* fi)
{
	return iso9660_fuse_open(path, fi);
}

int iso9660_fuse_read(const char* /*path*/, char* buf, size_t count,
                      off_t offset, struct fuse_file_info* fi)
{
	Filesystem* fs = FUSE_FS;
	if ( INT_MAX < count )
		count = INT_MAX;
	Inode* inode = fs->GetInode((uint32_t) fi->fh);
	if ( !inode )
		return -errno;
	ssize_t result = inode->ReadAt((uint8_t*) buf, count, offset);
	inode->Unref();
	return 0 <= result ? (int) result : -errno;
}

int iso9660_fuse_write(const char* /*path*/, const char* /*buf*/,
                       size_t /*count*/, off_t /*offset*/,
                       struct fuse_file_info* fi)
{
	Filesystem* fs = FUSE_FS;
	Inode* inode = fs->GetInode((uint32_t) fi->fh);
	if ( !inode )
		return -errno;
	inode->Unref();
	return -(errno = EROFS);
}

int iso9660_fuse_statfs(const char* /*path*/, struct statvfs* stvfs)
{
	memset(stvfs, 0, sizeof(*stvfs));
	Filesystem* fs = FUSE_FS;
	stvfs->f_bsize = fs->block_size;
	stvfs->f_frsize = fs->block_size;
	stvfs->f_blocks = fs->device->device_size / fs->block_size;
	stvfs->f_bfree = 0;
	stvfs->f_bavail = 0;
	stvfs->f_files = 0;
	stvfs->f_ffree = 0;
	stvfs->f_favail = 0;
	stvfs->f_ffree = 0;
	stvfs->f_fsid = 0;
	stvfs->f_flag = ST_RDONLY;
	stvfs->f_namemax = 255;
	return 0;
}

int iso9660_fuse_flush(const char* /*path*/, struct fuse_file_info* fi)
{
	Filesystem* fs = FUSE_FS;
	Inode* inode = fs->GetInode((uint32_t) fi->fh);
	if ( !inode )
		return -errno;
	inode->Unref();
	return 0;
}

int iso9660_fuse_release(const char* /*path*/, struct fuse_file_info* fi)
{
	Filesystem* fs = FUSE_FS;
	Inode* inode = fs->GetInode((uint32_t) fi->fh);
	if ( !inode )
		return -errno;
	inode->RemoteUnref();
	inode->Unref();
	return 0;
}

int iso9660_fuse_releasedir(const char* path, struct fuse_file_info* fi)
{
	return iso9660_fuse_release(path, fi);
}

int iso9660_fuse_fsync(const char* /*path*/, int data, struct fuse_file_info* fi)
{
	(void) data;
	Filesystem* fs = FUSE_FS;
	Inode* inode = fs->GetInode((uint32_t) fi->fh);
	if ( !inode )
		return -errno;
	inode->Unref();
	return 0;
}

/*int iso9660_fuse_syncdir(const char* path, int data, struct fuse_file_info* fi)
{
	return iso9660_fuse_sync(path, data, fi);
}*/

/*int iso9660_fuse_setxattr(const char *, const char *, const char *, size_t, int)
{
	return -(errno = ENOSYS);
}*/

/*int iso9660_fuse_getxattr(const char *, const char *, char *, size_t)
{
	return -(errno = ENOSYS);
}*/

/*int iso9660_fuse_listxattr(const char *, char *, size_t)
{
	return -(errno = ENOSYS);
}*/

/*int iso9660_fuse_removexattr(const char *, const char *)
{
	return -(errno = ENOSYS);
}*/

int iso9660_fuse_readdir(const char* /*path*/, void* buf,
                         fuse_fill_dir_t filler, off_t rec_num,
                         struct fuse_file_info* fi)
{
	Filesystem* fs = FUSE_FS;
	Inode* inode = fs->GetInode((iso9660_ino_t) fi->fh);
	if ( !inode )
		return -errno;
	if ( !S_ISDIR(inode->Mode()) )
		return inode->Unref(), -(errno = ENOTDIR);
	uint64_t offset = 0;
	Block* block = NULL;
	uint64_t block_id = 0;
	char name[256];
	uint8_t file_type;
	iso9660_ino_t inode_id;
	while ( inode->ReadDirectory(&offset, &block, &block_id,
	                             rec_num ? NULL : name, &file_type, &inode_id) )
	{
		if ( !rec_num || !rec_num-- )
		{
			if ( filler(buf, name, NULL, 0) )
			{
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

/*int iso9660_fuse_lock(const char*, struct fuse_file_info*, int, struct flock*)
{
	return -(errno = ENOSYS);
}*/

int iso9660_fuse_utimens(const char* path, const struct timespec tv[2])
{
	Inode* inode = iso9660_fuse_resolve_path(path);
	if ( !inode )
		return -errno;
	(void) tv;
	return inode->Unref(), -(errno = EROFS);
}

/*int iso9660_fuse_bmap(const char*, size_t blocksize, uint64_t* idx)
{
	return -(errno = ENOSYS);
}*/

int iso9660_fuse_main(const char* argv0,
                   const char* mount_path,
                   const char* fuse_options,
                   bool foreground,
                   Filesystem* fs,
                   Device* dev)
{
	struct fuse_operations operations;
	memset(&operations, 0, sizeof(operations));

	operations.access = iso9660_fuse_access;
	operations.chmod = iso9660_fuse_chmod;
	operations.chown = iso9660_fuse_chown;
	operations.create = iso9660_fuse_create;
	operations.destroy = iso9660_fuse_destroy;
	operations.fgetattr = iso9660_fuse_fgetattr;
	operations.flush = iso9660_fuse_flush;
	operations.fsync = iso9660_fuse_fsync;
	operations.ftruncate = iso9660_fuse_ftruncate;
	operations.getattr = iso9660_fuse_getattr;
	operations.init = iso9660_fuse_init;
	operations.link = iso9660_fuse_link;
	operations.mkdir = iso9660_fuse_mkdir;
	operations.mknod = iso9660_fuse_mknod;
	operations.opendir = iso9660_fuse_opendir;
	operations.open = iso9660_fuse_open;
	operations.readdir = iso9660_fuse_readdir;
	operations.read = iso9660_fuse_read;
	operations.readlink = iso9660_fuse_readlink;
	operations.releasedir = iso9660_fuse_releasedir;
	operations.release = iso9660_fuse_release;
	operations.rename = iso9660_fuse_rename;
	operations.rmdir = iso9660_fuse_rmdir;
	operations.statfs = iso9660_fuse_statfs;
	operations.symlink = iso9660_fuse_symlink;
	operations.truncate = iso9660_fuse_truncate;
	operations.unlink = iso9660_fuse_unlink;
	operations.utimens = iso9660_fuse_utimens;
	operations.write = iso9660_fuse_write;

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

	struct iso9660_fuse_ctx iso9660_fuse_ctx;
	iso9660_fuse_ctx.fs = fs;
	iso9660_fuse_ctx.dev = dev;

	return fuse_main(argc_fuse, argv_fuse, &operations, &iso9660_fuse_ctx);
}

#endif
