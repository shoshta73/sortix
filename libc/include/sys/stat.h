/*
 * Copyright (c) 2011-2015, 2018, 2025 Jonas 'Sortie' Termansen.
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
 * sys/stat.h
 * Data returned by the stat() function.
 */

#ifndef _INCLUDE_SYS_STAT_H
#define _INCLUDE_SYS_STAT_H

#include <sys/cdefs.h>

#include <sys/__/types.h>

#include <sortix/timespec.h>
#include <sortix/stat.h>

#ifndef __mode_t_defined
#define __mode_t_defined
typedef __mode_t mode_t;
#endif

/* POSIX mandates that we define these compatibility macros to support programs
   that are yet to embrace struct timespec. */
#define st_atime st_atim.tv_sec
#define st_ctime st_ctim.tv_sec
#define st_mtime st_mtim.tv_sec

#ifdef __cplusplus
extern "C" {
#endif

int chmod(const char*, mode_t);
int fchmod(int, mode_t);
int fchmodat(int, const char*, mode_t, int);
int fstat(int, struct stat*);
int fstatat(int, const char*, struct stat*, int);
int futimens(int, const struct timespec[2]);
int lstat(const char* __restrict, struct stat* __restrict);
#if __USE_SORTIX
mode_t getumask(void);
#endif
int mkdir(const char*, mode_t);
int mkdirat(int, const char*, mode_t);
/* TODO: mkfifo */
/* TODO: mkfifoat */
/* TODO: mknod? */
/* TODO: mknodat? */
int stat(const char* __restrict, struct stat* __restrict);
mode_t umask(mode_t);
#if __USE_SORTIX
int utimens(const char*, const struct timespec[2]);
#endif
int utimensat(int, const char*, const struct timespec[2], int);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
