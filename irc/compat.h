/*
 * Copyright (c) 2016 Jonas 'Sortie' Termansen.
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
 * combat.h
 * Compatibility.
 */

#ifndef COMPAT_H
#define COMPAT_H

#include <stddef.h>

#include "config.h"

#ifndef HAVE_EXPLICIT_BZERO
void explicit_bzero(void*, size_t);
#endif
#ifndef HAVE_REALLOCARRAY
void* reallocarray(void*, size_t, size_t);
#endif
#ifndef HAVE_STRLCAT
size_t strlcat(char* restrict, const char* restrict, size_t);
#endif
#ifndef HAVE_STRLCPY
size_t strlcpy(char* restrict, const char* restrict, size_t);
#endif

#endif
