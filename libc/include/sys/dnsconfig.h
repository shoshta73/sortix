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
 * sys/dnsconfig.h
 * Domain name system configuration.
 */

#ifndef INCLUDE_SYS_DNSCONFIG_H
#define INCLUDE_SYS_DNSCONFIG_H

#include <sys/cdefs.h>

#include <netinet/in.h>
#include <stddef.h>

union dnsconfig_server_union
{
	struct in_addr in;
	struct in6_addr in6;
};

struct dnsconfig_server
{
	sa_family_t family;
	size_t addrsize;
	union dnsconfig_server_union addr;
};

#define DNSCONFIG_MAX_SERVERS 3

struct dnsconfig
{
	size_t servers_count;
	struct dnsconfig_server servers[DNSCONFIG_MAX_SERVERS];
};

#ifdef __cplusplus
extern "C" {
#endif

int getdnsconfig(struct dnsconfig*);
int setdnsconfig(const struct dnsconfig*);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
