/*
 * Copyright (c) 2016 Jonas 'Sortie' Termansen.
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
 * dnsconfig.cpp
 * System calls for managing the dns configuration of the current system.
 */

#include <sys/dnsconfig.h>

#include <errno.h>
#include <string.h>

#include <sortix/kernel/copy.h>
#include <sortix/kernel/kernel.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/syscall.h>

namespace Sortix {

static kthread_mutex_t dnsconfig_lock = KTHREAD_MUTEX_INITIALIZER;
static struct dnsconfig dnsconfig;

int sys_getdnsconfig(struct dnsconfig* user_cfg)
{
	ScopedLock lock(&dnsconfig_lock);
	if ( !CopyToUser(user_cfg, &dnsconfig, sizeof(dnsconfig)) )
		return -1;
	return 0;
}

int sys_setdnsconfig(const struct dnsconfig* user_cfg)
{
	ScopedLock lock(&dnsconfig_lock);
	struct dnsconfig newcfg;
	if ( !CopyFromUser(&newcfg, user_cfg, sizeof(dnsconfig)) )
		return -1;
	if ( DNSCONFIG_MAX_SERVERS < newcfg.servers_count )
		return errno = EINVAL, -1;
	for ( size_t i = 0; i < newcfg.servers_count; i++ )
	{
		struct dnsconfig_server* server = &newcfg.servers[i];
		if ( server->family == AF_INET )
		{
			if ( server->addrsize != sizeof(struct in_addr) )
				return errno = EINVAL, -1;
			memset(&server->addr.in + 1, 0,
			       sizeof(server->addr) - sizeof(server->addr.in));
		}
		else if ( server->family == AF_INET6 )
		{
			if ( server->addrsize != sizeof(struct in6_addr) )
				return errno = EINVAL, -1;
			memset(&server->addr.in6 + 1, 0,
			       sizeof(server->addr) - sizeof(server->addr.in6));
		}
		else
			return errno = EAFNOSUPPORT, -1;
	}
	for ( size_t i = newcfg.servers_count; i < DNSCONFIG_MAX_SERVERS; i++ )
		memset(&newcfg.servers[i], 0, sizeof(newcfg.servers[i]));
	memcpy(&dnsconfig, &newcfg, sizeof(dnsconfig));
	return 0;
}

} // namespace Sortix
