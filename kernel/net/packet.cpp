/*
 * Copyright (c) 2015 Meisaka Yukara.
 * Copyright (c) 2016, 2017, 2022 Jonas 'Sortie' Termansen.
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
 * net/packet.cpp
 * Reference counted network packets.
 */

#include <errno.h>

#include <sortix/kernel/kernel.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/memorymanagement.h>
#include <sortix/kernel/packet.h>
#include <sortix/kernel/pci-mmio.h>

namespace Sortix {

// Limit the memory usage of network packages to this fraction of total memory.
// This constant is documented in if(4) under NOTES.
static const size_t MAX_PACKET_FRACTION = 16;

// Keep this amount of virtually mapped buffers around at all times.
// This constant is documented in if(4) under NOTES.
static const size_t PACKET_CACHE_TARGET_SIZE = 384;

// A cache of physical memory allocations for quick allocation to new packets.
static kthread_mutex_t packet_cache_lock = KTHREAD_MUTEX_INITIALIZER;
static paddrmapped_t* packet_cache = NULL;
static size_t packet_cache_used = 0;
static size_t packet_cache_allocated = 0;
static size_t packet_count = 0;

Packet::Packet(paddrmapped_t _pmap) : pmap(_pmap)
{
	from = (unsigned char*) pmap.from;
	length = 0;
	offset = 0;
	netif = NULL;
	packet_count++;
}

Packet::~Packet()
{
	ScopedLock lock(&packet_cache_lock);
	if ( packet_cache_used < packet_cache_allocated )
		packet_cache[packet_cache_used++] = pmap;
	else
		FreeAllocatedAndMappedPage(&pmap);
	packet_count--;
}

Ref<Packet> GetPacket()
{
	ScopedLock lock(&packet_cache_lock);
	if ( packet_cache == NULL )
	{
		size_t new_allocated = PACKET_CACHE_TARGET_SIZE;
		packet_cache = new paddrmapped_t[new_allocated];
		if ( !packet_cache )
			return errno = ENOBUFS, Ref<Packet>(NULL);
		packet_cache_allocated = new_allocated;
	}
	paddrmapped_t pmap;
	if ( 0 < packet_cache_used )
		pmap = packet_cache[--packet_cache_used];
	else
	{
		size_t total_memory;
		Memory::Statistics(NULL, &total_memory, NULL);
		size_t total_pages = total_memory / Page::Size();
		size_t max_packets = total_pages / MAX_PACKET_FRACTION;
		if ( max_packets <= packet_count )
			return errno = ENOBUFS, Ref<Packet>(NULL);
		if ( !AllocateAndMapPage(&pmap, PAGE_USAGE_NETWORK_PACKET) )
			return errno = ENOBUFS, Ref<Packet>(NULL);
	}
	Ref<Packet> pkt = Ref<Packet>(new Packet(pmap));
	if ( !pkt )
	{
		FreeAllocatedAndMappedPage(&pmap);
		return errno = ENOBUFS, Ref<Packet>(NULL);
	}
	return pkt;
}

} // namespace Sortix
