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
 * compat.c
 * Compatibility.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "compat.h"

#ifndef HAVE_EXPLICIT_BZERO
void explicit_bzero(void* buffer, size_t size)
{
	memset(buffer, 0, size);
}
#endif

#ifndef HAVE_REALLOCARRAY
void* reallocarray(void* ptr, size_t nmemb, size_t size)
{
	if ( size && nmemb && SIZE_MAX / size < nmemb )
		return errno = ENOMEM, (void*) NULL;
	return realloc(ptr, nmemb * size);
}
#endif

#ifndef HAVE_STRLCAT
size_t strlcat(char* restrict dest, const char* restrict src, size_t size)
{
	size_t dest_len = strnlen(dest, size);
	if ( size <= dest_len )
		return dest_len + strlen(src);
	return dest_len + strlcpy(dest + dest_len, src, size - dest_len);
}
#endif

#ifndef HAVE_STRLCPY
size_t strlcpy(char* restrict dest, const char* restrict src, size_t size)
{
	if ( !size )
		return strlen(src);
	size_t result;
	for ( result = 0; result < size-1 && src[result]; result++ )
		dest[result] = src[result];
	dest[result] = '\0';
	return result + strlen(src + result);
}
#endif
