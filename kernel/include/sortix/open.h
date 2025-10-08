/*
 * Copyright (c) 2012, 2013, 2014, 2016, 2025 Jonas 'Sortie' Termansen.
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
 * sortix/fcntl.h
 * Declares various constants related to opening files.
 */

#ifndef _INCLUDE_SORTIX_OPEN_H
#define _INCLUDE_SORTIX_OPEN_H

#include <sys/cdefs.h>

/* Remember to update the flag classifications at the top of descriptor.cpp if
   you add new flags here. */
#define O_RDONLY (1<<0)
#define O_WRONLY (1<<1)
#define O_RDWR (O_RDONLY | O_WRONLY)
#define O_EXEC (1<<2)
#define O_APPEND (1<<3)
#define O_CLOEXEC (1<<4)
#define O_CREAT (1<<5)
#define O_DIRECTORY (1<<6)
#define O_EXCL (1<<7)
#define O_TRUNC (1<<8)
#define O_CLOFORK (1<<9)
#define O_SEARCH (1<<10)
#define O_NONBLOCK (1<<11)
#define O_NOFOLLOW (1<<12)
#define O_SYMLINK_NOFOLLOW (1<<13)
#define O_NOCTTY (1<<14)
#define O_TTY_INIT (1<<15)
#ifdef __is_sortix_kernel
#define O_IS_STAT (1<<30)
#endif
#define O_ACCMODE (O_RDONLY | O_WRONLY | O_EXEC | O_SEARCH)

/* The kernel would like to simply deal with one bit for each base access mode,
   but using the traditional names O_RDONLY, O_WRONLY and O_RDWR for this would
   be weird, so it uses O_READ and O_WRITE bits instead.*/
#if __USE_SORTIX
#define O_READ O_RDONLY
#define O_WRITE O_WRONLY
#define O_CREATE O_CREAT 
#endif

#endif
