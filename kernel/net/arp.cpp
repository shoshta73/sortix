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
 * net/arp.cpp
 * Address resolution protocol.
 */

#include <assert.h>
#include <errno.h>
#include <netinet/if_ether.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <timespec.h>

#include <sortix/kernel/clock.h>
#include <sortix/kernel/kernel.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/if.h>
#include <sortix/kernel/packet.h>
#include <sortix/kernel/refcount.h>
#include <sortix/kernel/time.h>
#include <sortix/kernel/timer.h>

#include "arp.h"
#include "ether.h"

// Every network interface has its own ARP table of cached entries. The table
// is a hash map of IP address to an ARP entry. The hash function is the
// bytewise xor of each byte in the IP address. The table can contain up to 256
// entries, which all start out in a linked list of unused entries.
//
// The used entries of an table are in a linked list sorted in order of last
// use. The unused entries are in a linked list in no particular order. The
// entries currently being resolved are in the deadline linked list sorted in
// order of the request deadline. The entries currently resolved and valid are
// in a linked list sorted in order of their expiration.
//
// To evict an entry from the cache, remove the entry from the appropriate
// linked lists, discard the entry's transmission queue, clear it, and add it to
// the table's list of unused entries.
//
// To allocate an entry for an IP address, the hash table is searched for an
// existing entry to return. If an existing entry is found, it is moved to the
// front of the hash table in case of a collision. Otherwise, the first unused
// entry is used. If the table was full, the least recently used entry is
// evicted and then used. The new entry is assigned the IP address and added to
// the hash table.
//
// When a packet is sent to an IP address, an ARP table is made for the network
// interface if it doesn't already have one. If the IP address is outside the
// network interface's IP subnet, or if the network interface has no IP address
// configured, the request fails. An ARP entry for the destination IP address is
// searched for, or if none exists, then a new one is allocated. The entry is
// marked as USED and is moved to the front of the table's list of entries in
// order of last use. If the entry is marked as RESOLVED, the packet is just
// sent to the entry's Ethernet address. Otherwise if the entry has not been
// marked as RESOLVING, an initial request for the IP address is broadcast on
// the local network, the entry is added to the end of the deadline linked list,
// and the deadline timer is set to fire when the request times out. The packet
// is added to the entry's transmission queue unless it is already full.
//
// If the deadline timer fires, the entry is removed from the deadline linked
// list. If too many attempts failed, the entry is evicted. Otherwise, the IP
// address resolution is attempted again and the entry's request attempt counter
// is incremented.
//
// When an ARP message is received, the message is discarded if the source or
// destination IP is outside the network interface's subnet, or if the network
// interface did not have an IP address configured. The entry for the source
// IP address is located in the network interface's table, or if none exists and
// the table is not currently full, an entry is allocated. The entry is removed
// from the deadline linked list if it is RESOLVING. The entry is removed from
// the expiration linked list if it is EXPIRING. The entry is marked as RESOLVED
// and the source Ethernet address is assigned to the entry. The entry is marked
// as EXPIRING and is added to the end of the expiring linked list and the
// expiration is set to fire when the entry expires. Every packet in the entry's
// transmission queue is sent to the source Ethernet address.
//
// If the message is a request, and the destination IP address is that of the
// network interface. Otherwise, an ARP reply message is sent back with the
// Ethernet address of the network interface.
//
// When the expiration timer fires, the entry is removed from the expiration
// linked list. If the entry was not marked as USED, it is evicted. Otherwise
// the entry is marked as RESOLVING, the attempt request attempt counter is
// reset, and the address resolution is attempted again. Until the renewal
// succeeds or times out, the entry remains marked RESOLVED and is used to route
// traffic from its IP address to its Ethernet address.

#define ETHERTYPE_ETHER 1

#define ARP_REQUEST 1
#define ARP_REPLY 2

// The entry contains a valid Ethernet address that has been resolved.
#define ARP_STATUS_RESOLVED (1 << 0)

// The entry is currently being resolved, the deadline timeout has been set and
// the deadline timer will fire when the resolution times out. This status is
// mutually exclusive with the EXPIRING status.
#define ARP_STATUS_RESOLVING (1 << 1)

// The entry has been resolved and is currently waiting until it expires, the
// expiration timeout has been set and the expiration timer will fire when the
// entry expires. This status is mutually exclusive with the RESOLVING status.
#define ARP_STATUS_EXPIRING (1 << 2)

// The entry has been used to route a packet and should be renewed on expiry.
#define ARP_STATUS_USED (1 << 3)

// The number of entries in an ARP table, this value is documented in arp(4).
#define ARP_TABLE_LENGTH 256

// The number of entries in the ARP table hashmap, this value is documented
// in arp(4).
#define ARP_HASHTABLE_LENGTH 256

// Attempt to resolve an address this many times before giving up, this value is
// documented in arp(4).
#define ARP_MAX_ATTEMPTS 3

// The maximum number of packets in an ARP entry's transmission queue, this
// value is documented in arp(4).
#define ARP_MAX_PENDING 16

namespace Sortix {
namespace ARP {

// The duration to wait before giving up on an attempt to resolve an address,
// this value is documented in arp(4).
static const struct timespec REQUEST_TIMEOUT = { .tv_sec = 1, .tv_nsec = 0 };

// The duration before the entry expires and renewal begins, this value is
// documented in arp(4).
static const struct timespec ENTRY_TIMEOUT = { .tv_sec = 60, .tv_nsec = 0 };

struct arp
{
	uint16_t hrd; /* Hardware address space */
	uint16_t pro; /* Protocol address space */
	uint8_t hln; /* Byte length of each hardware address */
	uint8_t pln; /* Byte length of each protocol address */
	uint16_t op; /* opcode */
	uint8_t sha[6]; /* Hardware address of sender */
	uint8_t spa[4]; /* Protocol address of sender */
	uint8_t tha[6]; /* Hardware address of target */
	uint8_t tpa[4]; /* Protocol address of target */
};

struct arp_entry
{
	struct arp_table* table;
	struct arp_entry* prev_by_table;
	struct arp_entry* next_by_table;
	struct arp_entry* prev_by_hash;
	struct arp_entry* next_by_hash;
	struct arp_entry* prev_by_timer;
	struct arp_entry* next_by_timer;
	struct timespec timeout;
	struct in_addr addr;
	struct ether_addr ether;
	uint16_t status;
	uint16_t attempts;
	uint16_t pending;
	Ref<Packet> pending_first;
	Ref<Packet> pending_last;
};

struct arp_table
{
	NetworkInterface* netif;
	struct arp_entry* first_unused;
	struct arp_entry* first_used;
	struct arp_entry* last_unused;
	struct arp_entry* last_used;
	struct arp_entry* hashtable[ARP_HASHTABLE_LENGTH];
	struct arp_entry entries[ARP_TABLE_LENGTH];
};

static void OnDeadline(Clock* clock, Timer* timer, void* context);
static void OnExpiration(Clock* clock, Timer* timer, void* context);

kthread_mutex_t arp_lock = KTHREAD_MUTEX_INITIALIZER;
static struct arp_entry* first_by_deadline;
static struct arp_entry* last_by_deadline;
static struct arp_entry* first_by_expiration;
static struct arp_entry* last_by_expiration;
static Timer* deadline_timer;
static Timer* expiration_timer;
static bool deadline_timer_armed;
static bool expiration_timer_armed;

// This hash function is perfect if the subnet is at least /24, with no more
// than remaining bits for the address of the machine on the subnet.
static inline uint8_t HashAddress(const struct in_addr* addr)
{
	uint32_t value = be32toh(addr->s_addr);
	return (value <<  0 & 0xFF) ^ (value <<  8 & 0xFF) ^
	       (value << 16 & 0xFF) ^ (value << 24 & 0xFF);
}

// arp_lock locked
static struct arp_table* GetTable(NetworkInterface* netif)
{
	if ( netif->arp_table )
		return netif->arp_table;
	struct arp_table* table =
		(struct arp_table*) calloc(1, sizeof(struct arp_table));
	if ( !table )
		return NULL;
	netif->arp_table = table;
	table->netif = netif;
	// Enter every entry into the table's unused linked list.
	table->first_unused = &table->entries[0];
	for ( size_t i = 0; i < ARP_TABLE_LENGTH; i++ )
	{
		table->entries[i].table = table;
		if ( i )
			table->entries[i].prev_by_table = &table->entries[i-1];
		if ( i + 1 < ARP_TABLE_LENGTH )
			table->entries[i].next_by_table = &table->entries[i+1];
	}
	table->last_unused = &table->entries[ARP_TABLE_LENGTH-1];
	return table;
}

// arp_lock locked
static void EvictEntry(struct arp_table* table, struct arp_entry* entry)
{
	unsigned char hash = HashAddress(&entry->addr);

	// Remove from the table's used linked list.
	(entry->next_by_table ?
	 entry->next_by_table->prev_by_table :
	 table->last_used) = entry->prev_by_table;
	(entry->prev_by_table ?
	 entry->prev_by_table->next_by_table :
	 table->first_used) = entry->next_by_table;
	entry->prev_by_table = NULL;
	entry->next_by_table = NULL;

	// Remove from the hash table.
	if ( entry->next_by_hash )
		entry->next_by_hash->prev_by_hash = entry->prev_by_hash;
	(entry->prev_by_hash ?
	 entry->prev_by_hash->next_by_hash :
	 table->hashtable[hash]) = entry->next_by_hash;
	entry->prev_by_hash = NULL;
	entry->next_by_hash = NULL;

	// Remove from deadline linked list.
	if ( entry->status & ARP_STATUS_RESOLVING )
	{
		(entry->next_by_timer ?
		 entry->next_by_timer->prev_by_timer :
		 last_by_deadline) = entry->prev_by_timer;
		(entry->prev_by_timer ?
		 entry->prev_by_timer->next_by_timer :
		 first_by_deadline) = entry->next_by_timer;
		entry->prev_by_timer = NULL;
		entry->next_by_timer = NULL;
	}

	// Remove from expiration linked list.
	else if ( entry->status & ARP_STATUS_EXPIRING )
	{
		(entry->next_by_timer ?
		 entry->next_by_timer->prev_by_timer :
		 last_by_expiration) = entry->prev_by_timer;
		(entry->prev_by_timer ?
		 entry->prev_by_timer->next_by_timer :
		 first_by_expiration) = entry->next_by_timer;
		entry->prev_by_timer = NULL;
		entry->next_by_timer = NULL;
	}

	// Drain the transmission queue while avoiding a stack overflow in packet
	// recursive destructor.
	while ( entry->pending_first )
	{
		Ref<Packet> next = entry->pending_first->next;
		entry->pending_first->next.Reset();
		entry->pending_first = next;
		entry->pending--;
		if ( !entry->pending_first )
			entry->pending_last.Reset();
	}

	// Clear the entry.
	assert(!entry->pending_first);
	assert(!entry->pending_last);
	memset((char*) entry, 0, sizeof(*entry));
	entry->table = table;

	// Insert the entry into the table's unused linked list.
	(table->first_unused ?
	 table->first_unused->prev_by_table :
	 table->last_unused) = entry;
	entry->prev_by_table = NULL;
	entry->next_by_table = table->first_unused;
	table->first_unused = entry;
}

// arp_lock locked
static struct arp_entry* AllocateEntry(struct arp_table* table,
                                       const struct in_addr* addr,
                                       bool evict)
{
	// Search for an existing entry.
	unsigned char hash = HashAddress(addr);
	for ( struct arp_entry* entry = table->hashtable[hash];
	      entry;
	      entry = entry->next_by_hash )
	{
		if ( be32toh(addr->s_addr) == be32toh(entry->addr.s_addr) )
		{
			// Move to the front of the hash table if not already.
			if ( entry->prev_by_hash )
			{
				if ( entry->next_by_hash )
					entry->next_by_hash->prev_by_hash = entry->prev_by_hash;
				entry->prev_by_hash->next_by_hash = entry->next_by_hash;
				entry->prev_by_hash = NULL;
				entry->next_by_hash = table->hashtable[hash];
				table->hashtable[hash]->prev_by_hash = entry;
				table->hashtable[hash] = entry;
			}
			return entry;
		}
	}

	// Allocate a new entry, potentially evicting the least recently used entry.
	struct arp_entry* entry = table->first_unused;
	if ( !table->first_unused )
	{
		if ( !evict )
			return NULL;
		EvictEntry(table, table->last_used);
		assert(table->first_unused);
		entry = table->first_unused;
	}

	// Remove from the table's unused list.
	table->first_unused = entry->next_by_table;
	(table->first_unused ?
	 table->first_unused->prev_by_table :
	 table->last_unused) = NULL;

	// Initialize the entry.
	entry->addr.s_addr = addr->s_addr;

	// Insert into the table's used list.
	(table->last_used ?
	 table->last_used->next_by_table :
	 table->first_used) = entry;
	entry->prev_by_table = table->last_used;
	entry->next_by_table = NULL;
	table->last_used = entry;

	// Add to the front of the hash table.
	if ( table->hashtable[hash] )
		table->hashtable[hash]->prev_by_hash = entry;
	entry->prev_by_hash = NULL;
	entry->next_by_hash = table->hashtable[hash];
	table->hashtable[hash] = entry;

	return entry;
}

// arp_lock locked
static bool Resolve(NetworkInterface* netif, struct arp_entry* entry)
{
	if ( !deadline_timer )
	{
		if ( !(deadline_timer = new Timer()) )
			return false;
		deadline_timer->Attach(Time::GetClock(CLOCK_MONOTONIC));
	}
	struct ether_addr src_ether_addr;
	struct in_addr src_in_addr;
	kthread_mutex_lock(&netif->cfg_lock);
	struct if_config* cfg = &netif->cfg;
	memcpy(&src_ether_addr, &cfg->ether.address, sizeof(struct ether_addr));
	memcpy(&src_in_addr, &cfg->inet.address, sizeof(struct in_addr));
	kthread_mutex_unlock(&netif->cfg_lock);
	struct arp arp;
	arp.hrd = htobe16(ETHERTYPE_ETHER);
	arp.pro = htobe16(ETHERTYPE_IP);
	arp.hln = sizeof(struct ether_addr);
	arp.pln = sizeof(struct in_addr);
	arp.op = htobe16(ARP_REQUEST);
	memcpy(arp.sha, &src_ether_addr, sizeof(struct ether_addr));
	memcpy(arp.spa, &src_in_addr, sizeof(struct in_addr));
	memcpy(arp.tha, &etheraddr_broadcast, sizeof(struct ether_addr));
	memcpy(arp.tpa, &entry->addr, sizeof(struct in_addr));
	Ref<Packet> pkt = GetPacket();
	if ( !pkt )
		return false;
	if ( pkt->pmap.size < sizeof(arp) )
		return errno = EMSGSIZE, false;
	pkt->length = sizeof(arp);
	memcpy(pkt->from, &arp, sizeof(arp));
	if ( !Ether::Send(pkt, &src_ether_addr, &etheraddr_broadcast, ETHERTYPE_ARP,
	                  netif) )
		return false;
	entry->status |= ARP_STATUS_RESOLVING;
	entry->attempts++;
	struct timespec now = Time::Get(CLOCK_MONOTONIC);
	entry->timeout = timespec_add(now, REQUEST_TIMEOUT);
	// Add entry to end of deadline linked list.
	(last_by_deadline ?
	 last_by_deadline->next_by_timer :
	 first_by_deadline) = entry;
	entry->prev_by_timer = last_by_deadline;
	entry->next_by_timer = NULL;
	last_by_deadline = entry;
	if ( !deadline_timer_armed )
	{
		struct itimerspec its;
		its.it_value = REQUEST_TIMEOUT;
		its.it_interval = timespec_nul();
		deadline_timer->Set(&its, NULL, 0, OnDeadline, NULL);
		deadline_timer_armed = true;
	}
	return true;
}

static void OnDeadline(Clock* clock, Timer* timer, void* /*context*/)
{
	ScopedLock lock(&arp_lock);
	struct timespec now;
	clock->Get(&now, NULL);
	struct arp_entry* entry;
	while ( (entry = first_by_deadline) )
	{
		if ( timespec_lt(now, entry->timeout) )
		{
			struct itimerspec its;
			its.it_value = timespec_sub(entry->timeout, now);
			its.it_interval = timespec_nul();
			timer->Set(&its, NULL, 0, OnDeadline, NULL);
			return;
		}
		struct arp_table* table = entry->table;
		// Remove from the deadline linked list.
		entry->status &= ~ARP_STATUS_RESOLVING;
		first_by_deadline = entry->next_by_timer;
		(first_by_deadline ?
		 first_by_deadline->prev_by_timer :
		 last_by_deadline) = NULL;
		entry->prev_by_timer = NULL;
		entry->next_by_timer = NULL;
		if ( entry->attempts < ARP_MAX_ATTEMPTS )
			Resolve(table->netif, entry);
		else
			EvictEntry(table, entry);
	}
	deadline_timer_armed = false;
}

static void OnExpiration(Clock* clock, Timer* timer, void* /*context*/)
{
	ScopedLock lock(&arp_lock);
	struct timespec now;
	clock->Get(&now, NULL);
	struct arp_entry* entry;
	while ( (entry = first_by_expiration) )
	{
		if ( timespec_lt(now, entry->timeout) )
		{
			struct itimerspec its;
			its.it_value = timespec_sub(entry->timeout, now);
			its.it_interval = timespec_nul();
			timer->Set(&its, NULL, 0, OnExpiration, NULL);
			return;
		}
		struct arp_table* table = entry->table;
		// Remove the entry from the expiration linked list.
		entry->status &= ~ARP_STATUS_EXPIRING;
		first_by_expiration = entry->next_by_timer;
		(first_by_expiration ?
		 first_by_expiration->prev_by_timer :
		 last_by_expiration) = NULL;
		entry->prev_by_timer = NULL;
		entry->next_by_timer = NULL;
		if ( entry->status & ARP_STATUS_USED )
		{
			entry->status &= ~ARP_STATUS_USED;
			entry->attempts = 0;
			Resolve(table->netif, entry);
		}
		else
			EvictEntry(table, entry);
	}
	expiration_timer_armed = false;
}

bool RouteIPEthernet(NetworkInterface* netif,
                     Ref<Packet> pkt,
                     const struct in_addr* dst)
{
	struct ether_addr local_ether;
	struct in_addr local_in;
	struct in_addr local_router;
	struct in_addr local_subnet;
	kthread_mutex_lock(&netif->cfg_lock);
	memcpy(&local_ether, &netif->cfg.ether.address, sizeof(struct ether_addr));
	memcpy(&local_in, &netif->cfg.inet.address, sizeof(struct in_addr));
	memcpy(&local_router, &netif->cfg.inet.router, sizeof(struct in_addr));
	memcpy(&local_subnet, &netif->cfg.inet.subnet, sizeof(struct in_addr));
	kthread_mutex_unlock(&netif->cfg_lock);
	if ( be32toh(local_in.s_addr) == INADDR_ANY )
		return errno = ENETUNREACH, false;
	if ( (local_in.s_addr & local_subnet.s_addr) !=
	     (dst->s_addr &local_subnet.s_addr) )
		return errno = ENETUNREACH, false;
	ScopedLock lock(&arp_lock);
	struct arp_table* table = GetTable(netif);
	if ( !table )
		return false;
	struct arp_entry* entry = AllocateEntry(table, dst, true);
	assert(entry);
	// Mark as USED and move the entry to the front of table's used linked list.
	entry->status |= ARP_STATUS_USED;
	if ( entry->prev_by_table )
	{
		(entry->next_by_table ?
		 entry->next_by_table->prev_by_table :
		 table->last_used) = entry->prev_by_table;
		entry->prev_by_table->next_by_table = entry->next_by_table;
		entry->prev_by_table = NULL;
		entry->next_by_table = table->first_used;
		table->first_used->prev_by_table = entry;
		table->first_used = entry;
	}
	if ( entry->status & ARP_STATUS_RESOLVED )
	{
		struct ether_addr dst_ether = entry->ether;
		lock.Reset();
		return Ether::Send(pkt, &local_ether, &dst_ether, ETHERTYPE_IP, netif);
	}
	assert(!pkt->next);
	if ( !(entry->status & ARP_STATUS_RESOLVING) && !Resolve(netif, entry) )
		return false;
	// If the address isn't resolved, try send to the router instead.
	if ( dst->s_addr != local_router.s_addr &&
	     local_router.s_addr != INADDR_ANY )
	{
		lock.Reset();
		return RouteIPEthernet(netif, pkt, &local_router);
	}
	// Drop the packet if the transmission queue is full.
	if ( ARP_MAX_PENDING <= entry->pending )
		return true;
	(entry->pending_last ?
	 entry->pending_last->next :
	 entry->pending_first) = pkt;
	entry->pending_last = pkt;
	entry->pending++;
	return true;
}

void Handle(Ref<Packet> pkt,
            const struct ether_addr* src_ether_of_packet,
            const struct ether_addr* /*dst_ether*/,
            bool /*dst_ether*/)
{
	const unsigned char* in = pkt->from + pkt->offset;
	size_t inlen = pkt->length - pkt->offset;
	NetworkInterface* netif = pkt->netif;
	struct arp hdr;
	if ( inlen < sizeof(hdr) )
		return;
	memcpy(&hdr, in, sizeof(hdr));
	hdr.hrd = be16toh(hdr.hrd);
	hdr.pro = be16toh(hdr.pro);
	hdr.op = be16toh(hdr.op);

	// Drop unsupported or invalid packets.
	if ( !(hdr.hrd == ETHERTYPE_ETHER && hdr.hln == 6) )
		return;
	if ( !(hdr.pro == ETHERTYPE_IP && hdr.pln == 4) )
		return;
	if ( !(hdr.op == ARP_REQUEST || hdr.op == ARP_REPLY) )
		return;

	struct in_addr src;
	struct in_addr dst;
	memcpy(&src, hdr.spa, sizeof(src));
	memcpy(&dst, hdr.tpa, sizeof(dst));
	struct ether_addr src_ether;
	memcpy(&src_ether, hdr.sha, sizeof(src_ether));
	struct ether_addr local_eth;
	struct in_addr local_in;
	struct in_addr local_subnet;
	kthread_mutex_lock(&netif->cfg_lock);
	memcpy(&local_eth, &netif->cfg.ether.address, sizeof(struct ether_addr));
	memcpy(&local_in, &netif->cfg.inet.address, sizeof(struct in_addr));
	memcpy(&local_subnet, &netif->cfg.inet.subnet, sizeof(struct in_addr));
	kthread_mutex_unlock(&netif->cfg_lock);

	// Drop packets if the network interface does not have an IP address
	// configured, or if the source or destination IP address are outside of the
	// network interface's IP subnet.
	if ( be32toh(local_in.s_addr) == INADDR_ANY )
		return;
	if ( (local_in.s_addr & local_subnet.s_addr) !=
	     (src.s_addr & local_subnet.s_addr))
		return;
	if ( (local_in.s_addr & local_subnet.s_addr) !=
	     (dst.s_addr & local_subnet.s_addr) )
		return;

	ScopedLock lock(&arp_lock);

	if ( !expiration_timer )
	{
		if ( !(expiration_timer = new Timer()) )
			return;
		expiration_timer->Attach(Time::GetClock(CLOCK_MONOTONIC));
	}

	struct arp_table* table = GetTable(netif);
	if ( !table )
		return;
	struct arp_entry* entry = AllocateEntry(table, &src, false);

	if ( entry )
	{
		// Remove from pending request linked list.
		if ( entry->status & ARP_STATUS_RESOLVING )
		{
			entry->status &= ~ARP_STATUS_RESOLVING;
			(entry->next_by_timer ?
			 entry->next_by_timer->prev_by_timer :
			 last_by_deadline) = entry->prev_by_timer;
			(entry->prev_by_timer ?
			 entry->prev_by_timer->next_by_timer :
			 first_by_deadline) = entry->next_by_timer;
			entry->prev_by_timer = NULL;
			entry->next_by_timer = NULL;
		}

		// Remove from expiration linked list.
		else if ( entry->status & ARP_STATUS_EXPIRING )
		{
			entry->status &= ~ARP_STATUS_EXPIRING;
			(entry->next_by_timer ?
			 entry->next_by_timer->prev_by_timer :
			 last_by_expiration) = entry->prev_by_timer;
			(entry->prev_by_timer ?
			 entry->prev_by_timer->next_by_timer :
			 first_by_expiration) = entry->next_by_timer;
			entry->prev_by_timer = NULL;
			entry->next_by_timer = NULL;
		}

		// Mark entry as RESOLVED.
		entry->status |= ARP_STATUS_RESOLVED;
		memcpy(&entry->ether, &src_ether, sizeof(src_ether));

		// Mark entry as EXPIRING and add to end of the expiration linked list.
		entry->status |= ARP_STATUS_EXPIRING;
		(last_by_expiration ?
		 last_by_expiration->next_by_timer :
		 first_by_expiration) = entry;
		entry->prev_by_timer = last_by_expiration;
		entry->next_by_timer = NULL;
		last_by_expiration = entry;
		struct timespec now = Time::Get(CLOCK_MONOTONIC);
		entry->timeout = timespec_add(now, ENTRY_TIMEOUT);
		if ( !expiration_timer_armed )
		{
			struct itimerspec its;
			its.it_value = ENTRY_TIMEOUT;
			its.it_interval = timespec_nul();
			expiration_timer->Set(&its, NULL, 0, OnExpiration, NULL);
			expiration_timer_armed = true;
		}

		// Transmit the transission queue.
		while ( entry->pending_first )
		{
			Ref<Packet> pending = entry->pending_first;
			entry->pending_first = pending->next;
			pending->next.Reset();
			Ether::Send(pending, &local_eth, &src_ether, ETHERTYPE_IP, netif);
			if ( !entry->pending_first )
				entry->pending_last.Reset();
		}
	}

	// Send an ARP reply if our local address was requested.
	if ( hdr.op == ARP_REQUEST &&
	     !memcmp(&local_in, &dst, sizeof(struct in_addr)) )
	{
		Ref<Packet> packet = GetPacket();
		if ( !packet )
			return;
		struct arp arp;
		arp.hrd = htobe16(ETHERTYPE_ETHER);
		arp.pro = htobe16(ETHERTYPE_IP);
		arp.hln = sizeof(struct ether_addr);
		arp.pln = sizeof(struct in_addr);
		arp.op = htobe16(ARP_REPLY);
		memcpy(arp.sha, &local_eth, sizeof(struct ether_addr));
		memcpy(arp.spa, &local_in, sizeof(struct in_addr));
		memcpy(arp.tha, &src_ether, sizeof(struct ether_addr));
		memcpy(arp.tpa, &src, sizeof(struct in_addr));
		if ( packet->pmap.size < sizeof(arp) )
			return;
		packet->length = sizeof(arp);
		unsigned char* out = packet->from;
		memcpy(out, &arp, sizeof(arp));
		Ether::Send(packet, &local_eth, src_ether_of_packet, ETHERTYPE_ARP,
		            netif);
	}
}

// arp_lock locked, netif->cfg_lock locked.
void OnConfiguration(NetworkInterface* netif,
                     const struct if_config* old_cfg,
                     const struct if_config* new_cfg)
{
	// Purge the ARP cache if the ether or inet configuration changed.
	if ( !memcmp(&old_cfg->ether, &new_cfg->ether, sizeof(new_cfg->ether)) &&
	     !memcmp(&old_cfg->inet, &new_cfg->inet, sizeof(new_cfg->inet)) )
		return;
	struct arp_table* table = GetTable(netif);
	if ( !table )
		return;
	while ( table->first_used )
		EvictEntry(table, table->first_used);
}

} // namespace ARP
} // namespace Sortix
