/*
 * Copyright (c) 2014, 2022 Jonas 'Sortie' Termansen.
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
 * stdlib/reallocarray.c
 * Reallocates a chunk of memory from the dynamic memory heap.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __TRACE_ALLOCATION_SITES
void* reallocarray_trace(struct __allocation_site* allocation_site,
                         void* ptr, size_t nmemb, size_t size)
#else
void* reallocarray(void* ptr, size_t nmemb, size_t size)
#endif
{
	if ( size && nmemb && SIZE_MAX / size < nmemb )
		return errno = ENOMEM, (void*) NULL;
#ifdef __TRACE_ALLOCATION_SITES
	return realloc_trace(allocation_site, ptr, nmemb * size);
#else
	return realloc(ptr, nmemb * size);
#endif
}
