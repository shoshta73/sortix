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
 * uchar.h
 * UTF-16 and UTF-32 character support.
 */

#ifndef _INCLUDE_UCHAR_H
#define _INCLUDE_UCHAR_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#if __cplusplus < 201103L
typedef uint_least16_t char16_t;
typedef uint_least32_t char32_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

size_t c16rtomb(char* __restrict, char16_t, mbstate_t* __restrict);
size_t c32rtomb(char* __restrict, char32_t, mbstate_t* __restrict);
size_t mbrtoc16(char16_t* __restrict, const char* __restrict, size_t,
                mbstate_t* __restrict);
size_t mbrtoc32(char32_t* __restrict, const char* __restrict, size_t,
                mbstate_t* __restrict);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
