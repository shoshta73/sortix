/*
 * Copyright (c) 2012-2018 Jonas 'Sortie' Termansen.
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
 * sortix/stat.h
 * Defines the struct stat used for file meta-information and other useful
 * macros and values relating to values stored in it.
 */

#ifndef _INCLUDE_SORTIX_STAT_H
#define _INCLUDE_SORTIX_STAT_H

#include <sys/cdefs.h>

#include <sys/__/types.h>

#include <sortix/mode.h>
#include <sortix/timespec.h>

#ifndef __dev_t_defined
#define __dev_t_defined
typedef __dev_t dev_t;
#endif

#ifndef __ino_t_defined
#define __ino_t_defined
typedef __ino_t ino_t;
#endif

#ifndef __mode_t_defined
#define __mode_t_defined
typedef __mode_t mode_t;
#endif

#ifndef __nlink_t_defined
#define __nlink_t_defined
typedef __nlink_t nlink_t;
#endif

#ifndef __uid_t_defined
#define __uid_t_defined
typedef __uid_t uid_t;
#endif

#ifndef __gid_t_defined
#define __gid_t_defined
typedef __gid_t gid_t;
#endif

#ifndef __off_t_defined
#define __off_t_defined
typedef __off_t off_t;
#endif

#ifndef __blksize_t_defined
#define __blksize_t_defined
typedef __blksize_t blksize_t;
#endif

#ifndef __blkcnt_t_defined
#define __blkcnt_t_defined
typedef __blkcnt_t blkcnt_t;
#endif

struct stat
{
	dev_t st_dev;
	dev_t st_rdev;
	ino_t st_ino;
	mode_t st_mode;
	nlink_t st_nlink;
	uid_t st_uid;
	gid_t st_gid;
	off_t st_size;
	struct timespec st_atim;
	struct timespec st_mtim;
	struct timespec st_ctim;
	blksize_t st_blksize;
	/* TODO: After releasing Sortix 1.1, bump the major ABI with an incompatible
	         change and remove this padding since blksize_t used to be
	         intmax_t. */
#ifdef __i386__
	long __stat_blksize_padding;
#endif
	blkcnt_t st_blocks;
};

#define UTIME_NOW  0x3FFFFFFF
#define UTIME_OMIT 0x3FFFFFFE

#endif
