/*
 * Copyright (c) 2016, 2017, 2018 Jonas 'Sortie' Termansen.
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
 * net/ipv4.cpp
 * Internet Protocol Version 4.
 */

#include <sys/socket.h>

#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <netinet/if_ether.h>
#include <netinet/in.h>
#include <stdint.h>
#include <timespec.h>

#include <sortix/kernel/kernel.h>
#include <sortix/kernel/if.h>
#include <sortix/kernel/packet.h>
#include <sortix/kernel/refcount.h>

#include "arp.h"
#include "ether.h"
#include "ping.h"
#include "tcp.h"
#include "udp.h"

namespace Sortix {
namespace IPv4 {

struct ipv4
{
	uint8_t version_ihl;
	uint8_t dscp_ecn;
	uint16_t length;
	uint16_t identification;
	uint16_t fragment;
	uint8_t ttl;
	uint8_t protocol;
	uint16_t checksum;
	uint8_t source[4];
	uint8_t destination[4];
};

#define IPV4_IHL(x) ((x) >> 0 & 0xF)
#define IPV4_VERSION(x) ((x) >> 4 & 0xF)
#define IPV4_IHL_MAKE(x) (((x) & 0xF) << 0)
#define IPV4_VERSION_MAKE(x) (((x) & 0xF) << 4)
#define IPV4_FRAGMENT(x) (((x) >> 0) & 0x1FFF)
#define IPV4_FRAGMENT_MAKE(x) (((x) & 0x1FFF) << 0)
#define IPV4_FRAGMENT_MORE (1 << (13 + 0))
#define IPV4_FRAGMENT_DONT (1 << (13 + 1))
#define IPV4_FRAGMENT_EVIL (1 << (13 + 2))

uint16_t ipsum_word(uint16_t sum, uint16_t word)
{
	uint32_t result = sum + word;
	if ( result & 0x10000 )
		return (result + 1) & 0xFFFF;
	return result;
}

uint16_t ipsum_buf(uint16_t sum, const void* bufptr, size_t size)
{
	const uint8_t* buf = (const uint8_t*) bufptr;
	for ( size_t i = 0; i < (size & ~1UL); i += 2 )
		sum = ipsum_word(sum, buf[i] << 8 | buf[i + 1]);
	// Odd sizes only work correctly if this is the final byte being summed.
	if ( size & 1 )
		sum = ipsum_word(sum, buf[size - 1] << 8);
	return sum;
}

uint16_t ipsum_finish(uint16_t sum)
{
	return ~sum;
}

uint16_t ipsum(const void* bufptr, size_t size)
{
	uint16_t sum = ipsum_buf(0, bufptr, size);
	return ipsum_finish(sum);
}

static NetworkInterface* LocateInterface(const struct in_addr* src,
                                         const struct in_addr* dst,
                                         unsigned int ifindex)
{
	ScopedLock ifs_lock(&netifs_lock);
	in_addr_t any_ip = htobe32(INADDR_ANY);
	in_addr_t broadcast_ip = htobe32(INADDR_BROADCAST);

	// Refuse to route to the any address.
	if ( !memcmp(&any_ip, dst, sizeof(in_addr_t)) )
		return errno = ENETUNREACH, (NetworkInterface*) NULL;
	// If src is set, but ifindex is not set, search for a fitting interface.
	if ( !ifindex && memcmp(&any_ip, src, sizeof(in_addr_t)) != 0 )
	{
		for ( unsigned int i = 1; i < netifs_count; i++ )
		{
			NetworkInterface* netif = netifs[i];
			if ( !netif )
				continue;
			ScopedLock cfg_lock(&netif->cfg_lock);
			if ( memcmp(&netif->cfg.inet.address, src, sizeof(in_addr_t)) != 0 )
				continue;
			ifindex = i;
			break;
		}
		// No interface had the correct address.
		if ( !ifindex )
			return errno = EADDRNOTAVAIL, (NetworkInterface*) NULL;
	}
	// If ifindex is set, route to that interface.
	if ( ifindex )
	{
		// Can't route to non-existent interface.
		if ( netifs_count <= ifindex )
			return errno = EADDRNOTAVAIL, (NetworkInterface*) NULL;
		NetworkInterface* netif = netifs[ifindex];
		if ( !netif )
			return errno = EADDRNOTAVAIL, (NetworkInterface*) NULL;
		ScopedLock cfg_lock(&netif->cfg_lock);
		// Can't route to down interfaces.
		if ( !(netif->ifstatus.flags & IF_STATUS_FLAGS_UP) )
			return errno = ENETDOWN, (NetworkInterface*) NULL;
		// If src is set, it must be the interface's address.
		if ( memcmp(src, &any_ip, sizeof(in_addr_t)) != 0 &&
		     memcmp(src, &netif->cfg.inet.address, sizeof(in_addr_t)) != 0 )
			return errno = EADDRNOTAVAIL, (NetworkInterface*) NULL;
		in_addr_t dstaddr = be32toh(dst->s_addr);
		in_addr_t ifaddr = be32toh(netif->cfg.inet.address.s_addr);
		in_addr_t subnet = be32toh(netif->cfg.inet.subnet.s_addr);
		in_addr_t loopaddr = INADDR_LOOPBACK;
		in_addr_t loopmask = INADDR_LOOPMASK;
		if ( netif->ifinfo.type == IF_TYPE_LOOPBACK )
		{
			// The destination must be on the interface's subnet.
			if ( (dstaddr & subnet) != (ifaddr & subnet) )
				return errno = ENETUNREACH, (NetworkInterface*) NULL;
			return netif;
		}
		else
		{
			// The destination must not be on the loopback network for
			// a non-loopback interface.
			if ( (dstaddr & loopmask) == (loopaddr & loopmask) )
				return errno = ENETUNREACH, (NetworkInterface*) NULL;
			// If the interface does not have a default route, the destination
			// must be broadcast or be on the interface's subnet.
			if ( !memcmp(&netif->cfg.inet.router, &any_ip, sizeof(in_addr_t)) &&
			     memcmp(&dstaddr, &broadcast_ip, sizeof(in_addr_t)) != 0 &&
			     (dstaddr & subnet) != (ifaddr & subnet) )
				return errno = ENETUNREACH, (NetworkInterface*) NULL;
			return netif;
		}
	}
	// If the destination is broadcast, send to the first fitting interface.
	else if ( !memcmp(&broadcast_ip, dst, sizeof(in_addr_t)) )
	{
		for ( unsigned int i = 1; i < netifs_count; i++ )
		{
			NetworkInterface* netif = netifs[i];
			if ( !netif )
				continue;
			ScopedLock cfg_lock(&netif->cfg_lock);
			// Can't route broadcast to loopback interfaces or down interfaces.
			if ( netif->ifinfo.type == IF_TYPE_LOOPBACK ||
			     !(netif->ifstatus.flags & IF_STATUS_FLAGS_UP) )
				continue;
			return netif;
		}
		// No interface was suitable for broadcast.
		return errno = EADDRNOTAVAIL, (NetworkInterface*) NULL;
	}
	// Otherwise, pick the best interface for the destination address.
	else
	{
		NetworkInterface* default_netif = NULL;
		for ( unsigned int i = 1; i < netifs_count; i++ )
		{
			NetworkInterface* netif = netifs[i];
			if ( !netif )
				continue;
			ScopedLock cfg_lock(&netif->cfg_lock);
			in_addr_t dstaddr = be32toh(dst->s_addr);
			in_addr_t ifaddr = be32toh(netif->cfg.inet.address.s_addr);
			in_addr_t subnet = be32toh(netif->cfg.inet.subnet.s_addr);
			// Route to the interface if the destination is on its subnet.
			if ( (dstaddr & subnet) == (ifaddr & subnet) )
			{
				// Can't route to down interfaces.
				if ( !(netif->ifstatus.flags & IF_STATUS_FLAGS_UP) )
					return errno = ENETDOWN, (NetworkInterface*) NULL;
				return netif;
			}
			// If the interface is up, no default route has been found yet, and
			// the interface has a default route, default to that route if no
			// better interface is found.
			else if ( (netif->ifstatus.flags & IF_STATUS_FLAGS_UP) &&
			          !default_netif &&
			          memcmp(&any_ip, &netif->cfg.inet.router,
			                 sizeof(in_addr_t)) != 0 )
				default_netif = netif;
		}
		// If a fitting default route was found, use it.
		if ( default_netif )
			return default_netif;
		// No interface was up that could accept the destination address, hence
		// the network is down.
		return errno = ENETDOWN, (NetworkInterface*) NULL;
	}
}

static bool ShouldHandlePacket(Ref<Packet> pkt,
                               const struct in_addr* src,
                               const struct in_addr* dst,
                               bool dst_broadcast,
                               bool* out_broadcast)
{
	NetworkInterface* netif = pkt->netif;
	ScopedLock cfg_lock(&netif->cfg_lock);
	// The source address must not be broadcast (RFC 1122 3.2.1.3).
	in_addr_t broadcast_ip = htobe32(INADDR_BROADCAST);
	if ( !memcmp(src, &broadcast_ip, sizeof(in_addr_t)) )
		return false;
	// The source address must not be the subnet's broadcast (RFC 1122 3.2.1.3).
	in_addr_t if_broadcast_ip =
		netif->cfg.inet.address.s_addr | ~netif->cfg.inet.subnet.s_addr;
	if ( !memcmp(&if_broadcast_ip, src, sizeof(in_addr_t)) )
		return false;
	if ( netif->ifinfo.type != IF_TYPE_LOOPBACK )
	{
		// 127.0.0.0/8 is only for loopback.
		if ( (be32toh(src->s_addr) & INADDR_LOOPMASK) ==
		     (INADDR_LOOPBACK & INADDR_LOOPMASK) ||
		     (be32toh(dst->s_addr) & INADDR_LOOPMASK) ==
		     (INADDR_LOOPBACK & INADDR_LOOPMASK) )
			return false;
	}
	// Receive packets sent to the broadcast address.
	*out_broadcast = false;
	if ( !memcmp(dst, &broadcast_ip, sizeof(broadcast_ip)) )
		return *out_broadcast = true, true;
	in_addr_t any_ip = htobe32(INADDR_ANY);
	// Only receive non-broadcast packets if the interface is configured.
	if ( memcmp(&netif->cfg.inet.address, &any_ip, sizeof(in_addr_t)) != 0 )
	{
		// Receive packets sent to our address.
		if ( !dst_broadcast &&
			 !memcmp(&netif->cfg.inet.address, dst, sizeof(in_addr_t)) )
			return true;
		// Receive packets sent to the subnet's broadcast address.
		if ( !memcmp(&if_broadcast_ip, dst, sizeof(in_addr_t)) )
			return *out_broadcast = true, true;
	}
	return false;
}

void Handle(Ref<Packet> pkt,
            const struct ether_addr* /*src*/,
            const struct ether_addr* /*dst*/,
            bool dst_broadcast)
{
	struct ipv4 hdr;
	size_t pkt_remain = pkt->length - pkt->offset;
	// The packet has to be large enough to contain a header.
	if ( pkt_remain < sizeof(hdr) )
		return;
	memcpy(&hdr, pkt->from + pkt->offset, sizeof(hdr));
	// Verify the header's checksum is correct.
	if ( ipsum(&hdr, sizeof(hdr)) != 0 )
		return;
	hdr.length = be16toh(hdr.length);
	hdr.identification = be16toh(hdr.identification);
	hdr.fragment = be16toh(hdr.fragment);
	hdr.checksum = be16toh(hdr.checksum);
	// Verify the packet is Internet Protocol Version 4.
	if ( IPV4_VERSION(hdr.version_ihl) != 4 )
		return;
	// Verify the relation:
	//  sizeof(hdr) <= ihl <= hdr.length <= pkt_remain
	size_t ihl = 4 * IPV4_IHL(hdr.version_ihl);
	// Verify the header length isn't smaller than the minimum header.
	if ( ihl < sizeof(hdr) )
		return;
	// Verify total length isn't smaller than the header length.
	if ( hdr.length < ihl )
		return;
	// Verify the packet length isn't smaller than the datagram.
	if ( pkt_remain < hdr.length )
		return;
	// Drop the packet if we shouldn't handle it.
	bool in_dst_broadcast;
	const struct in_addr* in_src = (const struct in_addr*) &hdr.source;
	const struct in_addr* in_dst = (const struct in_addr*) &hdr.destination;
	if ( !ShouldHandlePacket(pkt, in_src, in_dst, dst_broadcast,
	                         &in_dst_broadcast) )
		return;
	// TODO: IP options.
	// TODO: Reassemble fragmented packets.
	if ( IPV4_FRAGMENT(hdr.fragment) )
		return;
	if ( hdr.fragment & IPV4_FRAGMENT_MORE )
		return;
	// Trim the packet to the length according to the header, in case the packet
	// was smaller than the link layer protocol's minimum transmission unit and
	// the packet was padded by zeroes.
	size_t truncated_length = pkt->offset + hdr.length;
	if ( pkt->length < truncated_length )
		return;
	pkt->length = truncated_length;
	pkt->offset += ihl;
	if ( hdr.protocol == IPPROTO_ICMP )
		Ping::HandleIPv4(pkt, in_src, in_dst, in_dst_broadcast);
	else if ( hdr.protocol == IPPROTO_TCP )
		TCP::HandleIPv4(pkt, in_src, in_dst, in_dst_broadcast);
	else if ( hdr.protocol == IPPROTO_UDP )
		UDP::HandleIPv4(pkt, in_src, in_dst, in_dst_broadcast);
}

bool Send(Ref<Packet> pktin,
          const struct in_addr* src,
          const struct in_addr* dst,
          uint8_t protocol,
          unsigned int ifindex,
          bool broadcast)
{
	Ref<Packet> pkt = GetPacket();
	if ( !pkt )
		return -1;
	size_t mtu = pkt->pmap.size;
	if ( mtu < sizeof(struct ipv4) ||
	     mtu - sizeof(struct ipv4) < pktin->length )
		return errno = EMSGSIZE, -1;
	pkt->length = sizeof(struct ipv4) + pktin->length;
	unsigned char* in = pktin->from;
	unsigned char* out = pkt->from;
	struct ipv4 hdr;
	hdr.version_ihl = IPV4_VERSION_MAKE(4) | IPV4_IHL_MAKE(5);
	hdr.dscp_ecn = 0;
	hdr.length = htobe16(pkt->length);
	hdr.identification = htobe16(0); // TODO: Assign identification to packets.
	hdr.fragment = htobe16(0);
	hdr.ttl = 0x40; // TODO: This should be configurable.
	hdr.protocol = protocol;
	hdr.checksum = 0;
	memcpy(hdr.source, src, sizeof(struct in_addr));
	memcpy(hdr.destination, dst, sizeof(struct in_addr));
	hdr.checksum = htobe16(ipsum(&hdr, sizeof(hdr)));
	memcpy(out, &hdr, sizeof(hdr));
	memcpy(out + sizeof(struct ipv4), in, pktin->length);

	NetworkInterface* netif = LocateInterface(src, dst, ifindex);
	if ( !netif )
		return false;

	if ( netif->ifinfo.type == IF_TYPE_LOOPBACK )
	{
		struct ether_addr localaddr;
		memset(&localaddr, 0, sizeof(localaddr));
		return Ether::Send(pkt, &localaddr, &localaddr, ETHERTYPE_IP, netif);
	}

	if ( netif->ifinfo.type != IF_TYPE_ETHERNET )
		return errno = EAFNOSUPPORT, false;

	kthread_mutex_lock(&netif->cfg_lock);
	in_addr_t dst_ip = dst->s_addr;
	in_addr_t address_ip = netif->cfg.inet.address.s_addr;
	in_addr_t router_ip = netif->cfg.inet.router.s_addr;
	in_addr_t subnet_ip = netif->cfg.inet.subnet.s_addr;
	in_addr_t broadcast_ip =
		netif->cfg.inet.address.s_addr | ~netif->cfg.inet.subnet.s_addr;
	struct ether_addr ether_src = netif->cfg.ether.address;
	kthread_mutex_unlock(&netif->cfg_lock);

	struct in_addr route;
	// Route directly to the destination if the destination is broadcast.
	if ( dst_ip == htobe32(INADDR_BROADCAST) || dst_ip == broadcast_ip )
		memcpy(&route, &dst_ip, sizeof(route));
	// Route directly to the destination if the destination is on the subnet-
	else if ( (dst_ip & subnet_ip) == (address_ip & subnet_ip) &&
	          dst_ip != address_ip )
		memcpy(&route, dst, sizeof(route));
	// Route to the the default route if any.
	else if ( router_ip != htobe32(INADDR_ANY) )
		memcpy(&route, &router_ip, sizeof(route));
	// Otherwise the network is unreachable.
	else
		return errno = ENETUNREACH, false;

	// If the destination is broadcast, send an ethernet broadcast.
	if ( dst_ip == htobe32(INADDR_BROADCAST) || dst_ip == broadcast_ip )
	{
		if ( !broadcast )
			return errno = EACCES, false;
		return Ether::Send(pkt, &ether_src, &etheraddr_broadcast, ETHERTYPE_IP,
		                   netif);
	}
	return ARP::RouteIPv4Ethernet(netif, pkt, &route);
}

bool GetSourceIP(const struct in_addr* src,
                 const struct in_addr* dst,
                 struct in_addr* sendfrom,
                 unsigned int ifindex,
                 size_t* mtu)
{
	NetworkInterface* netif = LocateInterface(src, dst, ifindex);
	if ( !netif )
		return false;
	ScopedLock cfg_lock(&netif->cfg_lock);
	if ( sendfrom )
		memcpy(sendfrom, &netif->cfg.inet.address, sizeof(struct in_addr));
	if ( mtu )
		*mtu = Ether::GetMTU(netif) - sizeof(struct ipv4);
	return true;
}

Ref<Inode> Socket(int type, int protocol)
{
	switch ( type )
	{
	case SOCK_DGRAM:
		if ( protocol == 0 || protocol == IPPROTO_UDP )
			return UDP::Socket(AF_INET);
		if ( protocol == IPPROTO_PING )
			return Ping::Socket(AF_INET);
		return errno = EPROTONOSUPPORT, Ref<Inode>(NULL);
	case SOCK_STREAM:
		if ( protocol == 0 || protocol == IPPROTO_TCP )
			return TCP::Socket(AF_INET);
		return errno = EPROTONOSUPPORT, Ref<Inode>(NULL);
	default: return errno = EPROTOTYPE, Ref<Inode>(NULL);
	}
}

} // namespace IPv4
} // namespace Sortix
