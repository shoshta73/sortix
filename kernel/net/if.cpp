/*
 * Copyright (c) 2015 Meisaka Yukara.
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
 * net/if.cpp
 * Network Interface.
 */

#include <sys/ioctl.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sortix/stat.h>

#include <sortix/kernel/addralloc.h>
#include <sortix/kernel/copy.h>
#include <sortix/kernel/descriptor.h>
#include <sortix/kernel/if.h>
#include <sortix/kernel/inode.h>
#include <sortix/kernel/ioctx.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/log.h>
#include <sortix/kernel/memorymanagement.h>
#include <sortix/kernel/packet.h>
#include <sortix/kernel/panic.h>
#include <sortix/kernel/pci.h>
#include <sortix/kernel/pci-mmio.h>
#include <sortix/kernel/refcount.h>

#include "arp.h"

namespace Sortix {

kthread_mutex_t netifs_lock = KTHREAD_MUTEX_INITIALIZER;
NetworkInterface** netifs = NULL;
size_t netifs_count = 0;
static size_t netifs_allocated = 0;

class NetworkInterfaceNode : public AbstractInode
{
public:
	NetworkInterfaceNode(dev_t dev, uid_t owner, gid_t group, mode_t mode,
	           NetworkInterface* netif);
	virtual ~NetworkInterfaceNode();

public:
	virtual int ioctl(ioctx_t* ctx, int cmd, uintptr_t ptr);

private:
	NetworkInterface* netif;

};

bool RegisterNetworkInterface(NetworkInterface* netif,
                              Ref<Descriptor> dev)
{
	ScopedLock lock(&netifs_lock);
	if ( netifs_count == netifs_allocated )
	{
		size_t new_length_half = netifs_allocated;
		if ( new_length_half == 0 )
			new_length_half = 8;
		NetworkInterface** new_netifs =
			(NetworkInterface**) reallocarray(netifs, new_length_half,
			                                  2 * sizeof(NetworkInterface*));
		if ( !new_netifs )
			return false;
		netifs = new_netifs;
		netifs_allocated = new_length_half * 2;
	}
	Ref<Inode> node(new NetworkInterfaceNode(dev->dev, 0, 0, 0666, netif));
	if ( !node )
		return false;
	ioctx_t ctx; SetupKernelIOCtx(&ctx);
	if ( LinkInodeInDir(&ctx, dev, netif->ifinfo.name, node) != 0 )
		return false;
	// Interfaces are counted from 1 inclusive up to UINT_MAX exclusive.
	if ( netifs_count == 0 )
		netifs[netifs_count++] = NULL;
	if ( UINT_MAX <= netifs_count + 1 )
		return errno = EOVERFLOW, false;
	unsigned int linkid = netifs_count++;
	netifs[linkid] = netif;
	netif->ifinfo.linkid = linkid;
	return true;
}

NetworkInterface::NetworkInterface()
{
	cfg_lock = KTHREAD_MUTEX_INITIALIZER;
	cfg_cond = KTHREAD_COND_INITIALIZER;
	memset(&ifinfo, 0, sizeof(ifinfo));
	memset(&ifstatus, 0, sizeof(ifstatus));
	memset(&cfg, 0, sizeof(cfg));
	arp_table = NULL;
}

NetworkInterface::~NetworkInterface()
{
}

NetworkInterfaceNode::NetworkInterfaceNode(dev_t dev, uid_t owner, gid_t group,
                                           mode_t mode, NetworkInterface* netif)
{
	inode_type = INODE_TYPE_UNKNOWN;
	if ( !dev )
		dev = (dev_t) this;
	ino = (ino_t) this;
	this->type = S_IFCHR;
	this->dev = dev;
	this->stat_uid = owner;
	this->stat_gid = group;
	this->stat_mode = (mode & S_SETABLE) | this->type;
	this->netif = netif;
}

NetworkInterfaceNode::~NetworkInterfaceNode()
{
}

int NetworkInterfaceNode::ioctl(ioctx_t* ctx, int cmd, uintptr_t arg)
{
	void* ptr = (void*) arg;

	if ( cmd == NIOC_SETCONFIG ||
	     cmd == NIOC_SETCONFIG_ETHER ||
	     cmd == NIOC_SETCONFIG_INET )
	{
		// Avoid deadlock by locking in the right order.
		ScopedLock outer(&ARP::arp_lock);
		ScopedLock inner(&netif->cfg_lock);
		struct if_config new_cfg;
		memcpy(&new_cfg, &netif->cfg, sizeof(new_cfg));
		switch ( cmd )
		{
		case NIOC_SETCONFIG:
			if ( !ctx->copy_from_src(&new_cfg, ptr, sizeof(new_cfg)) )
				return -1;
			break;
		case NIOC_SETCONFIG_ETHER:
			if ( !ctx->copy_from_src(&new_cfg.ether, ptr,
			                         sizeof(new_cfg.ether)) )
				return -1;
			break;
		case NIOC_SETCONFIG_INET:
			if ( !ctx->copy_from_src(&new_cfg.inet, ptr, sizeof(new_cfg.inet)) )
				return -1;
			break;
		}
		// Let the ARP cache know the configuration changed, so it can purge any
		// entries that are no longer valid.
		ARP::OnConfiguration(netif, &netif->cfg, &new_cfg);
		memcpy(&netif->cfg, &new_cfg, sizeof(new_cfg));
		kthread_cond_broadcast(&netif->cfg_cond);
		return 0;
	}

	ScopedLock lock(&netif->cfg_lock);
	switch ( cmd )
	{
	case IOCGETTYPE:
		return IOC_MAKE_TYPE(IOC_TYPE_NETWORK_INTERFACE, 0);
	case NIOC_GETINFO:
		if ( !ctx->copy_to_dest(ptr, &netif->ifinfo, sizeof(netif->ifinfo)) )
			return -1;
		return 0;
	case NIOC_GETSTATUS:
		if ( !ctx->copy_to_dest(ptr, &netif->ifstatus,
		                        sizeof(netif->ifstatus)) )
			return -1;
		return 0;
	case NIOC_GETCONFIG:
		if ( !ctx->copy_to_dest(ptr, &netif->cfg, sizeof(netif->cfg)) )
			return -1;
		return 0;
	case NIOC_GETCONFIG_ETHER:
		if ( !ctx->copy_to_dest(ptr, &netif->cfg.ether,
		                        sizeof(netif->cfg.ether)) )
			return -1;
		return 0;
	case NIOC_GETCONFIG_INET:
		if ( !ctx->copy_to_dest(ptr, &netif->cfg.inet,
		                        sizeof(netif->cfg.inet)) )
			return -1;
		return 0;
	case NIOC_WAITLINKSTATUS:
	{
		int status = (int) (uintptr_t) ptr ? IF_STATUS_FLAGS_UP : 0;
		while ( (netif->ifstatus.flags & IF_STATUS_FLAGS_UP) != status )
			if ( !kthread_cond_wait_signal(&netif->cfg_cond, &netif->cfg_lock) )
				return errno = EINTR, -1;
		return 0;
	}
	default:
		return errno = ENOTTY, -1;
	}
}

} // namespace Sortix
