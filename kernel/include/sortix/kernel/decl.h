/*
 * Copyright (c) 2011, 2012, 2013, 2014, 2022 Jonas 'Sortie' Termansen.
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
 * sortix/kernel/decl.h
 * Various declarations. These should likely be replaced with better names from
 * standard headers or at least one with a less generic name than decl.h.
 */

#ifndef _INCLUDE_SORTIX_KERNEL_DECL_H
#define _INCLUDE_SORTIX_KERNEL_DECL_H

#include <stdint.h>

typedef uintptr_t addr_t;

#define likely(x) __builtin_expect(!!(x),1)
#define unlikely(x) __builtin_expect(!!(x),0)

#if !defined(CPU) && defined(__i386__)
	#define CPU X86
#endif

#if !defined(CPU) && defined(__x86_64__)
	#define CPU X64
#endif

#ifdef __TRACE_ALLOCATION_SITES

struct kernel_allocation_site
{
	struct __allocation_site allocation_site;
	struct kernel_allocation_site* next;
	bool registered;
};

extern struct kernel_allocation_site* first_kernel_allocation_site;

#undef ALLOCATION_SITE
#define ALLOCATION_SITE \
({ \
	static struct kernel_allocation_site site = \
		{ { __FILE__, __LINE__, __func__, 0, 0 }, NULL, false }; \
	if ( !site.registered ) \
	{ \
		site.registered = true; \
		site.next = first_kernel_allocation_site; \
		first_kernel_allocation_site = &site; \
	} \
	&site.allocation_site; \
})

#include <stddef.h>
void* operator new(size_t size, struct __allocation_site* allocation_site);
void* operator new[](size_t size, struct __allocation_site* allocation_site);
#define new new (ALLOCATION_SITE)
#endif

#endif
