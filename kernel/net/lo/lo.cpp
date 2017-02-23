/*
 * Copyright (c) 2017 Jonas 'Sortie' Termansen.
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
 * net/lo/lo.cpp
 * Loopback device.
 */

#include <sys/socket.h>

#include <arpa/inet.h>
#include <stdio.h>

#include <sortix/kernel/ioctx.h>
#include <sortix/kernel/kernel.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/if.h>
#include <sortix/kernel/worker.h>

#include "../ether.h"

#include "lo.h"

// The loopback device currently communicates through the Ethernet layer and
// pretends to do offload Ethernet checksumming as an optimization.

// The shared worker thread is used for processing. Whenever a packet needs to
// be sent, if the worker thread isn't scheduled, it is scheduled. The worker
// thread transmits all the packets that were in the queue when it begins, but
// not any more than that. If any work remains at the end, it schedules itself
// again to run later (to avoid starving other things using the shared worker
// thread). The packet queue is a singly linked list of packets.

namespace Sortix {
namespace Loopback {

class Loopback : public NetworkInterface
{
	friend void Loopback__Recv(void* ctx);

public:
	Loopback();
	~Loopback();

public:
	bool Send(Ref<Packet> pkt);

private:
	void Recv();

private:
	kthread_mutex_t socket_lock;
	Ref<Packet> first_packet;
	Ref<Packet> last_packet;
	bool worker_scheduled;

};

Loopback::Loopback()
{
	ifinfo.type = IF_TYPE_LOOPBACK;
	ifinfo.features = IF_FEATURE_ETHERNET_CRC_OFFLOAD;
	ifinfo.addrlen = 0;
	ifstatus.flags = IF_STATUS_FLAGS_UP;
	cfg.inet.address.s_addr = htobe32(INADDR_LOOPBACK);
	cfg.inet.router.s_addr = htobe32(INADDR_ANY);
	cfg.inet.subnet.s_addr = htobe32(INADDR_LOOPMASK);
	socket_lock = KTHREAD_MUTEX_INITIALIZER;
	// first_packet initialized by constructor
	// last_packet initialized by constructor
	worker_scheduled = false;
}

Loopback::~Loopback()
{
	// Avoid stack overflow in first_packet recursive destructor.
	while ( first_packet )
	{
		Ref<Packet> next = first_packet->next;
		first_packet->next.Reset();
		first_packet = next;
	}
	last_packet.Reset();
}

void Loopback__Recv(void* ctx)
{
	((Loopback*) ctx)->Recv();
}

void Loopback::Recv()
{
	kthread_mutex_lock(&socket_lock);
	Ref<Packet> next_packet = first_packet;
	first_packet.Reset();
	last_packet.Reset();
	kthread_mutex_unlock(&socket_lock);
	while ( next_packet )
	{
		Ref<Packet> packet = next_packet;
		next_packet = next_packet->next;
		packet->netif = this;
		Ether::Handle(packet, true);
	}
	kthread_mutex_lock(&socket_lock);
	bool should_schedule = first_packet;
	if ( !should_schedule )
		worker_scheduled = false;
	kthread_mutex_unlock(&socket_lock);
	if ( should_schedule )
		Worker::Schedule(Loopback__Recv, this);
}

bool Loopback::Send(Ref<Packet> pkt)
{
	kthread_mutex_lock(&socket_lock);
	if ( last_packet )
		last_packet->next = pkt;
	else
		first_packet = pkt;
	last_packet = pkt;
	bool should_schedule = !worker_scheduled;
	worker_scheduled = true;
	kthread_mutex_unlock(&socket_lock);
	if ( should_schedule )
		Worker::Schedule(Loopback__Recv, this);
	return true;
}

void Init(const char* /*devpath*/, Ref<Descriptor> dev)
{
	Loopback* lo = new Loopback();
	if ( !lo )
		PanicF("Failed to allocate loopback device");
	size_t index = 0;
	snprintf(lo->ifinfo.name, sizeof(lo->ifinfo.name), "lo%zu", index);
	if ( !RegisterNetworkInterface(lo, dev) )
		PanicF("Failed to register %s as network interface", lo->ifinfo.name);
}

} // namespace Loopback
} // namespace Sortix
