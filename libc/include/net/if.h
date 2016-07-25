/*
 * Copyright (c) 2016, 2017 Jonas 'Sortie' Termansen.
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
 * net/if.h
 * Network interface.
 */

#ifndef _INCLUDE_NET_IF_H
#define _INCLUDE_NET_IF_H

#include <sys/cdefs.h>

#define IF_NAMESIZE 32

#if __USE_SORTIX
#include <netinet/if_ether.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

#define IF_HWADDR_MAXSIZE 6

#define IF_TYPE_LOOPBACK 1
#define IF_TYPE_ETHERNET 2

#define IF_FEATURE_ETHERNET_CRC_OFFLOAD (1 << 0)

struct if_info
{
	unsigned int linkid;
	int type;
	int features;
	size_t addrlen;
	char name[IF_NAMESIZE];
	unsigned char addr[IF_HWADDR_MAXSIZE];
};

#define IF_STATUS_FLAGS_UP (1 << 0)

struct if_status
{
	int flags;
	size_t mtu;
};

struct if_config_ether
{
	struct ether_addr address;
};

struct if_config_inet
{
	struct in_addr address;
	struct in_addr router;
	struct in_addr subnet;
};

struct if_config
{
	struct if_config_ether ether;
	struct if_config_inet inet;
};
#endif

struct if_nameindex
{
	unsigned int if_index;
	char* if_name;
};

#ifdef __cplusplus
extern "C" {
#endif

void if_freenameindex(struct if_nameindex*);
char* if_indextoname(unsigned int, char*);
struct if_nameindex* if_nameindex(void);
unsigned int if_nametoindex(const char*);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
