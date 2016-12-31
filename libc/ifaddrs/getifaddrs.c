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
 * ifaddrs/getifaddrs.c
 * List network interface addresses.
 */

#include <sys/ioctl.h>
#include <sys/socket.h>

#include <endian.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

union ifaddrs_addr
{
	struct sockaddr_in in;
	struct sockaddr_in6 in6;
};

struct ifaddrs_storage
{
	struct ifaddrs pub;
	char name[IF_NAMESIZE];
	union ifaddrs_addr addr;
	union ifaddrs_addr netmask;
	union ifaddrs_addr broadcast;
};

int getifaddrs(struct ifaddrs** ifas_ptr)
{
	struct if_nameindex* ifs = if_nameindex();
	if ( !ifs )
		return -1;
	size_t ifcount = 0;
	while ( ifs[ifcount].if_index )
		ifcount++;
	struct ifaddrs* ifas = NULL;
	size_t i = ifcount;
	while ( i-- )
	{
		struct if_nameindex* netif = &ifs[i];
		char path[5 + IF_NAMESIZE];
		snprintf(path, sizeof(path), "/dev/%s", netif->if_name);
		int fd = open(path, O_RDONLY);
		if ( fd < 0 )
		{
			if_freenameindex(ifs);
			freeifaddrs(ifas);
			return -1;
		}
		struct if_config cfg;
		if ( ioctl(fd, NIOC_GETCONFIG, &cfg) < 0 )
		{
			close(fd);
			if_freenameindex(ifs);
			freeifaddrs(ifas);
			return -1;
		}
		close(fd);
		if ( be32toh(cfg.inet.address.s_addr) != INADDR_ANY )
		{
			struct ifaddrs_storage* ifa =
				calloc(sizeof(struct ifaddrs_storage), 1);
			if ( !ifa )
			{
				if_freenameindex(ifs);
				freeifaddrs(ifas);
				return -1;
			}
			ifa->pub.ifa_next = ifas;
			ifas = &ifa->pub;
			strlcpy(ifa->name, netif->if_name, sizeof(ifa->name));
			ifa->pub.ifa_name = ifa->name;
			ifa->pub.ifa_flags = 0;
			ifa->addr.in.sin_family = AF_INET;
			memcpy(&ifa->addr.in.sin_addr, &cfg.inet.address,
			       sizeof(struct in_addr));
			ifa->pub.ifa_addr = (struct sockaddr*) &ifa->addr.in;
			ifa->netmask.in.sin_family = AF_INET;
			memcpy(&ifa->netmask.in.sin_addr, &cfg.inet.subnet,
			       sizeof(struct in_addr));
			ifa->pub.ifa_netmask = (struct sockaddr*) &ifa->netmask.in;
			ifa->broadcast.in.sin_family = AF_INET;
			ifa->broadcast.in.sin_addr.s_addr =
				ifa->addr.in.sin_addr.s_addr |
				~ifa->netmask.in.sin_addr.s_addr;
			ifa->pub.ifa_dstaddr = (struct sockaddr*) &ifa->broadcast.in;
			ifa->pub.ifa_data = NULL;
			ifa->pub.ifa_size = sizeof(struct sockaddr_in);
		}
		// TODO: IPv6 when struct if_config has IPv6 support.
	}
	if_freenameindex(ifs);
	*ifas_ptr = ifas;
	return 0;
}
