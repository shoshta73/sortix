/*
 * Copyright (c) 2013, 2014, 2024 Jonas 'Sortie' Termansen.
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
 * unistd/sysconf.c
 * Get configuration information at runtime.
 */

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

// TODO: After releasing Sortix 1.1, remove this bootstrap compatibility.
#include <sortix/limits.h>

long sysconf(int name)
{
	switch ( name )
	{
	case _SC_CLK_TCK: return 1000;
	case _SC_GETGR_R_SIZE_MAX: return -1;
	case _SC_GETPW_R_SIZE_MAX: return -1;
	case _SC_IOV_MAX: return IOV_MAX;
	case _SC_MONOTONIC_CLOCK: return _POSIX_MONOTONIC_CLOCK;
	case _SC_NPROCESSORS_CONF: return 1;
	case _SC_NPROCESSORS_ONLN: return 1;
	case _SC_OPEN_MAX: return 0x10000;
	case _SC_PAGESIZE: case _SC_PAGE_SIZE: return getpagesize();
	case _SC_RTSIG_MAX: return (SIGRTMAX+1) - SIGRTMIN;
	default:
		fprintf(stderr, "%s:%u warning: %s(%i) is unsupported\n",
		        __FILE__, __LINE__, __func__, name);
		return errno = EINVAL, -1;
	}
}
