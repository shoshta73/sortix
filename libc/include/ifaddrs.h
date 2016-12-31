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
 * ifaddrs.h
 * Network interface addresses.
 */

#ifndef INCLUDE_IFADDRS_H
#define INCLUDE_IFADDRS_H

#include <sys/cdefs.h>

#include <sys/__/types.h>

#ifndef __socklen_t_defined
#define __socklen_t_defined
typedef __socklen_t socklen_t;
#endif

struct ifaddrs
{
	struct ifaddrs* ifa_next;
	char* ifa_name;
	unsigned int ifa_flags;
	struct sockaddr* ifa_addr;
	struct sockaddr* ifa_netmask;
	struct sockaddr* ifa_dstaddr;
	void* ifa_data;
	socklen_t ifa_size;
};

#ifndef	ifa_broadaddr
#define	ifa_broadaddr ifa_dstaddr
#endif

#ifdef __cplusplus
extern "C" {
#endif

int getifaddrs(struct ifaddrs**);
void freeifaddrs(struct ifaddrs*);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
