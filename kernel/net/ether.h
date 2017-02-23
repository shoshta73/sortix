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
 * net/ether.h
 * Ethernet.
 */

#ifndef SORTIX_NET_ETHER_H
#define SORTIX_NET_ETHER_H

#include <netinet/if_ether.h>
#include <stdint.h>

#include <sortix/kernel/packet.h>

namespace Sortix {
class NetworkInterface;
} // namespace Sortix

namespace Sortix {
namespace Ether {

size_t GetMTU(NetworkInterface* netif);
void Handle(Ref<Packet> pkt, bool checksum_offloaded);
bool Send(Ref<Packet> pkt,
          const struct ether_addr* src,
          const struct ether_addr* dst,
          uint16_t ether_type,
          NetworkInterface* netif);

} // namespace Ether
} // namespace Sortix

#endif
