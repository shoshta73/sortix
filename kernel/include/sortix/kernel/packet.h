/*
 * Copyright (c) 2016 Meisaka Yukara.
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
 * sortix/kernel/packet.h
 * Reference counted network packets.
 */

#ifndef _INCLUDE_SORTIX_KERNEL_PACKET_H
#define _INCLUDE_SORTIX_KERNEL_PACKET_H

#include <endian.h>
#include <stdint.h>

#include <sortix/kernel/addralloc.h>
#include <sortix/kernel/pci-mmio.h>
#include <sortix/kernel/refcount.h>

namespace Sortix {

class NetworkInterface;

class Packet : public Refcountable
{
public:
	Packet(paddrmapped_t pmap);
	virtual ~Packet();

public:
	paddrmapped_t pmap;
	unsigned char* from;
	size_t length;
	size_t offset;
	NetworkInterface* netif;
	Ref<Packet> next;

};

Ref<Packet> GetPacket();

} // namespace Sortix

#endif
