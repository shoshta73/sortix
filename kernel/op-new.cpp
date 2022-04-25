/*
 * Copyright (c) 2011, 2012, 2013, 2022 Jonas 'Sortie' Termansen.
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
 * op-new.cpp
 * C++ allocation operators.
 */

#include <stddef.h>
#include <stdlib.h>

#ifdef __clang__
#warning "security: -fcheck-new might not work on clang"
#endif

#ifdef __TRACE_ALLOCATION_SITES
#undef new

void* operator new(size_t size, struct __allocation_site* allocation_site)
{
	return malloc_trace(allocation_site, size);
}

void* operator new[](size_t size, struct __allocation_site* allocation_site)
{
	return malloc_trace(allocation_site, size);
}
#else
void* operator new(size_t size)
{
	return malloc(size);
}

void* operator new[](size_t size)
{
	return malloc(size);
}
#endif

void operator delete(void* addr)
{
	return free(addr);
}

void operator delete[](void* addr)
{
	return free(addr);
}
