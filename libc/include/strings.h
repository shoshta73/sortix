/*
 * Copyright (c) 2011, 2012, 2013, 2025 Jonas 'Sortie' Termansen.
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
 * strings.h
 * Deprecated header providing previously non-standard functions that have now
 * either been obsoleted or merged into string.h.
 */

#ifndef _INCLUDE_STRINGS_H
#define _INCLUDE_STRINGS_H

#include <sys/cdefs.h>

#ifndef __size_t_defined
#define __size_t_defined
#define __need_size_t
#include <stddef.h>
#endif

#if __USE_SORTIX || 200809L <= __USE_POSIX
#ifndef __locale_t_defined
#define __locale_t_defined
typedef struct __locale* locale_t;
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Functions from early POSIX. */
#if __USE_SORTIX || __USE_POSIX
int strcasecmp(const char*, const char*);
int strncasecmp(const char*, const char*, size_t);
#endif

/* Functions from early X/OPEN. */
#if __USE_SORTIX || __USE_XOPEN
int ffs(int);
#endif

/* Functions from POSIX 2008. */
#if __USE_SORTIX || 200809L <= __USE_POSIX
int strcasecmp_l(const char*, const char*, locale_t);
int strncasecmp_l(const char*, const char*, size_t, locale_t);
#endif

/* Functions from X/OPEN 2024. */
#if __USE_SORTIX || 800 <= __USE_XOPEN
int ffsl(long int);
int ffsll(long long int);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
