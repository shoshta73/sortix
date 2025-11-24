/*
 * Copyright (c) 2025 Jonas 'Sortie' Termansen.
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
 * limits.h
 * System limits.
 */

#ifndef _INCLUDE_LIMITS_H
#define _INCLUDE_LIMITS_H

#include <sys/cdefs.h>

#include <sortix/limits.h>

#define CHAR_BIT __CHAR_BIT__

#define SCHAR_MAX __SCHAR_MAX__
#define SCHAR_MIN (-SCHAR_MAX - 1)
#define UCHAR_MAX (SCHAR_MAX * 2 + 1)
#ifdef __CHAR_UNSIGNED__
#define CHAR_MAX UCHAR_MAX
#define CHAR_MIN 0
#else
#define CHAR_MAX SCHAR_MAX
#define CHAR_MIN SCHAR_MIN
#endif

#define SHRT_MAX __SHRT_MAX__
#define SHRT_MIN (-SHRT_MAX - 1)
#define USHRT_MAX (SHRT_MAX * 2 + 1)

#define INT_MAX __INT_MAX__
#define INT_MIN (-INT_MAX - 1)
#define UINT_MAX (INT_MAX * 2U + 1)

#define LONG_MAX __LONG_MAX__
#define LONG_MIN (-LONG_MAX - 1L)
#define ULONG_MAX (LONG_MAX * 2UL + 1UL)

#define LLONG_MAX __LONG_LONG_MAX__
#define LLONG_MIN (-LLONG_MAX - 1LL)
#define ULLONG_MAX (LLONG_MAX * 2ULL + 1ULL)

#define MB_LEN_MAX 4

#if defined(__STDC_WANT_IEC_60559_BFP_EXT__) || (defined (__STDC_VERSION__) && 201710L < __STDC_VERSION__)
#define CHAR_WIDTH __SCHAR_WIDTH__
#define SCHAR_WIDTH __SCHAR_WIDTH__
#define UCHAR_WIDTH __SCHAR_WIDTH__
#define SHRT_WIDTH __SHRT_WIDTH__
#define USHRT_WIDTH __SHRT_WIDTH__
#define INT_WIDTH __INT_WIDTH__
#define UINT_WIDTH __INT_WIDTH__
#define LONG_WIDTH __LONG_WIDTH__
#define ULONG_WIDTH __LONG_WIDTH__
#define LLONG_WIDTH __LONG_LONG_WIDTH__
#define ULLONG_WIDTH __LONG_LONG_WIDTH__
#endif

#if defined(__STDC_VERSION__) && 201710L < __STDC_VERSION__
#ifndef __GNUC__ /* TODO: Conflict with gcc that doesn't undef the value. */
#define __STDC_VERSION_LIMITS_H__ 202311L
#endif
#define BOOL_MAX 1
#undef BOOL_WIDTH
#define BOOL_WIDTH 1
#ifdef __BITINT_MAXWIDTH__
#define BITINT_MAXWIDTH __BITINT_MAXWIDTH__
#endif
#endif

#endif
