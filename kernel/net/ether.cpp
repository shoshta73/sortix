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
 * net/ether.cpp
 * Ethernet.
 */

#include <assert.h>
#include <errno.h>
#include <endian.h>
#include <netinet/if_ether.h>
#include <stdint.h>
#include <string.h>

#include <sortix/kernel/crc32.h>
#include <sortix/kernel/kernel.h>
#include <sortix/kernel/if.h>
#include <sortix/kernel/packet.h>

#include "arp.h"
#include "ether.h"
#include "ipv4.h"

namespace Sortix {
namespace Ether {

size_t GetMTU(NetworkInterface* netif)
{
	(void) netif;
	return ETHERMTU;
}

void Handle(Ref<Packet> pkt, bool checksum_offloaded)
{
	assert(pkt->netif);
	assert(pkt->offset <= pkt->length);
	const unsigned char* in = pkt->from + pkt->offset;
	size_t inlen = pkt->length - pkt->offset;
	struct ether_header hdr;
	if ( !checksum_offloaded )
	{
		struct ether_footer ftr;
		if ( inlen < sizeof(hdr) + sizeof(ftr) /* ETHER_LEN */ )
			return;
		size_t ftr_offset = inlen - sizeof(ftr);
		memcpy(&ftr, in + ftr_offset, sizeof(ftr));
		pkt->length -= sizeof(ftr);
		inlen -= sizeof(ftr);
		ftr.ether_crc = le32toh(ftr.ether_crc);
		if ( ftr.ether_crc != crc32(0, in, inlen) )
			return;
	}
	else if ( inlen < sizeof(hdr) /* ETHER_HDR_LEN */ )
		return;
	memcpy(&hdr, in, sizeof(hdr));
	hdr.ether_type = be16toh(hdr.ether_type);
	pkt->offset += sizeof(hdr);
	const struct ether_addr* src = (const struct ether_addr*) &hdr.ether_shost;
	const struct ether_addr* dst = (const struct ether_addr*) &hdr.ether_dhost;
	// Drop invalid frames with broadcast source.
	if ( !memcmp(src, &etheraddr_broadcast, sizeof(struct ether_addr)) )
		return;
	// Accept only frames with destination being broadcast or our address.
	bool dst_broadcast =
		!memcmp(dst, &etheraddr_broadcast, sizeof(struct ether_addr));
	if ( !dst_broadcast )
	{
		ScopedLock(&pkt->netif->cfg_lock);
		const struct ether_addr* local = &pkt->netif->cfg.ether.address;
		if ( pkt->netif->ifinfo.type != IF_TYPE_LOOPBACK &&
		     memcmp(dst, local, sizeof(struct ether_addr)) != 0 )
			return;
	}
	switch ( hdr.ether_type )
	{
	case ETHERTYPE_IP: // IPv4
		IPv4::Handle(pkt, src, dst, dst_broadcast);
		break;
	case ETHERTYPE_ARP: // Address Resolution Protocol
		ARP::Handle(pkt, src, dst, dst_broadcast);
		break;
	case ETHERTYPE_IPV6: // IPv6
		break;
	default:
		break;
	}
}

bool Send(Ref<Packet> pktin,
          const struct ether_addr* src,
          const struct ether_addr* dst,
          uint16_t ether_type,
          NetworkInterface* netif)
{
	if ( ETHERMTU < pktin->length )
		return errno = EMSGSIZE, false;
	Ref<Packet> pkt = GetPacket();
	if ( !pkt )
		return false;
	const unsigned char* in = pktin->from;
	size_t inlen = pktin->length;
	size_t padding = inlen < ETHERMIN ? ETHERMIN - inlen : 0;
	unsigned char* out = pkt->from;
	struct ether_header hdr;
	struct ether_footer ftr;
	size_t outlen = sizeof(hdr) /* ETHER_HDR_LEN */ + inlen + padding;
	if ( !(netif->ifinfo.features & IF_FEATURE_ETHERNET_CRC_OFFLOAD) )
		outlen += sizeof(ftr) /* ETHER_CRC_LEN */;
	if ( pkt->pmap.size < outlen )
		return errno = EMSGSIZE, false;
	pkt->length = outlen;
	memcpy(&hdr.ether_dhost, dst, sizeof(struct ether_addr));
	memcpy(&hdr.ether_shost, src, sizeof(struct ether_addr));
	hdr.ether_type = htobe16(ether_type);
	memcpy(out, &hdr, sizeof(hdr));
	memcpy(out + sizeof(hdr), in, inlen);
	memset(out + sizeof(hdr) + inlen, 0, padding);
	if ( !(netif->ifinfo.features & IF_FEATURE_ETHERNET_CRC_OFFLOAD) )
	{
		ftr.ether_crc = htole32(crc32(0, out, pkt->length));
		memcpy(out + sizeof(hdr) + inlen + padding, &ftr, sizeof(ftr));
	}
	return netif->Send(pkt);
}

} // namespace Ether
} // namespace Sortix
