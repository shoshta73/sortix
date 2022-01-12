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
 * netinet/if_ether.h
 * Ethernet interfaces.
 */

#ifndef _INCLUDE_NETINET_IF_ETHER_H
#define _INCLUDE_NETINET_IF_ETHER_H

#include <sys/cdefs.h>

#include <stdint.h>

#define ETHER_ADDR_LEN 6
#define ETHER_TYPE_LEN 2
#define ETHER_HDR_LEN (ETHER_ADDR_LEN + ETHER_ADDR_LEN + ETHER_TYPE_LEN) /* 14 */
#define ETHER_CRC_LEN 4
#define ETHER_LEN (ETHER_HDR_LEN + ETHER_CRC_LEN) /* 18 */
#define ETHER_MIN_LEN 64
#define ETHER_MAX_LEN 1518

#define ETHERMTU (ETHER_MAX_LEN - ETHER_LEN) /* 1500 */
#define ETHERMIN (ETHER_MIN_LEN - ETHER_LEN) /* 46 */

#define ETHERADDR_BROADCAST_INIT { { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } }

#define ETHERTYPE_IP 0x0800
#define ETHERTYPE_ARP 0x0806
#define ETHERTYPE_IPV6 0x86dd

struct ether_addr
{
	uint8_t ether_addr_octet[ETHER_ADDR_LEN];
};

struct ether_header
{
	uint8_t ether_dhost[ETHER_ADDR_LEN];
	uint8_t ether_shost[ETHER_ADDR_LEN];
	uint16_t ether_type;
};

struct ether_footer
{
	uint32_t ether_crc;
};

#ifdef __cplusplus
extern "C" {
#endif

extern const struct ether_addr etheraddr_broadcast; /* ff:ff:ff:ff:ff:ff */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
