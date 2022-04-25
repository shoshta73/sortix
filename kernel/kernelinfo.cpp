/*
 * Copyright (c) 2012, 2015, 2022 Jonas 'Sortie' Termansen.
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
 * kernelinfo.cpp
 * Lets user-space query information about the kernel.
 */

#include <brand.h>
#include <errno.h>

#include <sortix/kernel/copy.h>
#include <sortix/kernel/kernel.h>
#include <sortix/kernel/syscall.h>

#ifndef VERSIONSTR
#define VERSIONSTR "unknown"
#endif

namespace Sortix {

static const char* KernelInfo(const char* req)
{
	if ( strcmp(req, "name") == 0 ) { return BRAND_KERNEL_NAME; }
	if ( strcmp(req, "version") == 0 ) { return VERSIONSTR; }
	if ( strcmp(req, "tagline") == 0 ) { return BRAND_RELEASE_TAGLINE; }
	if ( strcmp(req, "builddate") == 0 ) { return __DATE__; }
	if ( strcmp(req, "buildtime") == 0 ) { return __TIME__; }
#if defined(__i386__) || defined(__x86_64__)
	if ( strcmp(req, "firmware") == 0 ) { return "bios"; }
#else
	#warning "Name your system firmware here"
#endif
	return NULL;
}

ssize_t sys_kernelinfo(const char* user_req, char* user_resp, size_t resplen)
{
	char* req = GetStringFromUser(user_req);
	if ( !req )
		return -1;
#ifdef __TRACE_ALLOCATION_SITES
	if ( !strcmp(req, "allocations") )
	{
		delete[] req;
		bool exhausted = false;
		size_t total_needed = 0;
		for ( struct kernel_allocation_site* site = first_kernel_allocation_site;
			  site;
			  site = site->next )
		{
			char str[256];
			snprintf(str, sizeof(str), "%20zu B %zu allocations %s:%zu %s %c",
			         site->allocation_site.current_size,
			         site->allocation_site.allocations,
			         site->allocation_site.file,
			         site->allocation_site.line,
			         site->allocation_site.function,
			         site->next ? '\n' : '\0');
			size_t stringlen = strlen(str);
			total_needed += stringlen;
			if ( exhausted )
				continue;
			if ( resplen < stringlen )
			{
				exhausted = true;
				continue;
			}
			if ( !CopyToUser(user_resp, str, sizeof(char) * stringlen) )
				return -1;
			user_resp += stringlen;
			resplen -= stringlen;
		}
		if ( !exhausted && !resplen )
			exhausted = true;
		if ( !exhausted )
		{
			char zero = '\0';
			if ( !CopyToUser(user_resp, &zero, 1) )
				return -1;
		}
		if ( exhausted )
			return errno = ERANGE, (ssize_t) total_needed;
		return 0;
	}
#endif
	const char* str = KernelInfo(req);
	delete[] req;
	if ( !str )
		return errno = EINVAL, -1;
	size_t stringlen = strlen(str);
	if ( resplen < stringlen + 1 )
		return errno = ERANGE, (ssize_t) stringlen;
	if ( !CopyToUser(user_resp, str, sizeof(char) * (stringlen + 1)) )
		return -1;
	return 0;
}

} // namespace Sortix
