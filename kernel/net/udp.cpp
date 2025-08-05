/*
 * Copyright (c) 2016, 2017, 2018, 2022 Jonas 'Sortie' Termansen.
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
 * net/udp.cpp
 * User Datagram Protocol.
 */

#include <sys/socket.h>
#include <sys/stat.h>

#include <assert.h>
#include <errno.h>
#include <endian.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// TODO: After releasing Sortix 1.1, remove this bootstrap compatibility.
#ifndef IOV_MAX
#include <sortix/limits.h>
#endif

#include <sortix/kernel/copy.h>
#include <sortix/kernel/if.h>
#include <sortix/kernel/inode.h>
#include <sortix/kernel/ioctx.h>
#include <sortix/kernel/kernel.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/packet.h>
#include <sortix/kernel/poll.h>
#include <sortix/kernel/process.h>
#include <sortix/kernel/sockopt.h>
#include <sortix/kernel/thread.h>

#include "ip.h"
#include "udp.h"

namespace Sortix {
namespace UDP {

class UDPSocket;

union udp_sockaddr
{
	sa_family_t family;
	struct sockaddr_in in;
	struct sockaddr_in6 in6;
};

// These values are documented in udp(4).
static const size_t DEFAULT_PACKET_LIMIT = 64;
static const size_t MAXIMAL_PACKET_LIMIT = 4096;

static kthread_mutex_t bind_lock = KTHREAD_MUTEX_INITIALIZER;
static UDPSocket** bindings_v4;
static UDPSocket** bindings_v6;

void Init()
{
	if ( !(bindings_v4 = new UDPSocket*[65536]) ||
	     !(bindings_v6 = new UDPSocket*[65536]) )
		Panic("Failed to allocate UDP Socket bindings");
	for ( size_t i = 0; i < 65536; i++ )
	{
		bindings_v4[i] = NULL;
		bindings_v6[i] = NULL;
	}
}

static bool IsSupportedAddressFamily(int af)
{
	return af == AF_INET /* TODO: || af == AF_INET6 */;
}

static size_t AddressFamilySize(int af)
{
	switch ( af )
	{
	case AF_INET: return sizeof(struct sockaddr_in);
	case AF_INET6: return sizeof(struct sockaddr_in6);
	}
	return 0;
}

class UDPSocket : public AbstractInode
{
	friend void HandleIP(Ref<Packet> pkt,
	                     const struct in_addr* src,
	                     const struct in_addr* dst,
	                     bool dst_broadcast);

public:
	UDPSocket(int af);
	virtual ~UDPSocket();
	virtual Ref<Inode> accept4(ioctx_t* ctx, uint8_t* addr, size_t* addrsize,
	                           int flags);
	virtual int bind(ioctx_t* ctx, const uint8_t* addr, size_t addrsize);
	virtual int connect(ioctx_t* ctx, const uint8_t* addr, size_t addrsize);
	virtual int listen(ioctx_t* ctx, int backlog);
	virtual ssize_t readv(ioctx_t* ctx, const struct iovec* iov, int iovcnt);
	virtual ssize_t recv(ioctx_t* ctx, uint8_t* buf, size_t count, int flags);
	virtual ssize_t recvmsg(ioctx_t* ctx, struct msghdr* msg, int flags);
	virtual ssize_t recvmsg_internal(ioctx_t* ctx, struct msghdr* msg,
	                                 int flags);
	virtual ssize_t send(ioctx_t* ctx, const uint8_t* buf, size_t count,
	                     int flags);
	virtual ssize_t sendmsg(ioctx_t* ctx, const struct msghdr* msg, int flags);
	virtual ssize_t sendmsg_internal(ioctx_t* ctx, const struct msghdr* msg,
	                                 int flags);
	virtual ssize_t writev(ioctx_t* ctx, const struct iovec* iov, int iovcnt);
	virtual int poll(ioctx_t* ctx, PollNode* node);
	virtual int getsockopt(ioctx_t* ctx, int level, int option_name,
	                       void* option_value, size_t* option_size_ptr);
	virtual int setsockopt(ioctx_t* ctx, int level, int option_name,
	                       const void* option_value, size_t option_size);
	virtual int shutdown(ioctx_t* ctx, int how);
	virtual int getpeername(ioctx_t* ctx, uint8_t* addr, size_t* addrsize);
	virtual int getsockname(ioctx_t* ctx, uint8_t* addr, size_t* addrsize);

public:
	void ReceivePacket(Ref<Packet> pkt);

private:
	short PollEventStatus();
	bool ImportAddress(ioctx_t* ctx, union udp_sockaddr* dest,
	                   const void* addr, size_t addrsize);
	bool CanBind(union udp_sockaddr new_local);
	bool BindDefault(const union udp_sockaddr* new_local);
	void DropReceiveQueue(void);

private:
	kthread_mutex_t socket_lock;
	kthread_cond_t receive_cond;
	PollChannel poll_channel;
	union udp_sockaddr local;
	union udp_sockaddr remote;
	Ref<Packet> first_packet;
	Ref<Packet> last_packet;
	UDPSocket* prev_socket;
	UDPSocket* next_socket;
	size_t receive_current;
	size_t receive_limit;
	size_t send_limit;
	unsigned int ifindex;
	int af;
	int sockerr;
	int how_shutdown;
	bool bound;
	bool broadcast;
	bool connected;
	bool reuseaddr;

};

// TODO: os-test fstat on a socket.
UDPSocket::UDPSocket(int af)
{
	Process* process = CurrentProcess();
	inode_type = INODE_TYPE_STREAM;
	dev = (dev_t) this;
	ino = (ino_t) this;
	type = S_IFSOCK;
	kthread_mutex_lock(&process->id_lock);
	stat_uid = process->uid;
	stat_gid = process->gid;
	kthread_mutex_unlock(&process->id_lock);
	stat_mode = 0600 | this->type;
	supports_iovec = true;
	socket_lock = KTHREAD_MUTEX_INITIALIZER;
	receive_cond = KTHREAD_COND_INITIALIZER;
	// poll_channel initialized by constructor
	memset(&local, 0, sizeof(local));
	memset(&remote, 0, sizeof(remote));
	if ( af == AF_INET )
	{
		local.in.sin_family = AF_INET;
		local.in.sin_addr.s_addr = htobe32(INADDR_ANY);
		local.in.sin_port = htobe16(0);
		remote.in.sin_family = AF_INET;
		remote.in.sin_addr.s_addr = htobe32(INADDR_ANY);
		remote.in.sin_port = htobe16(0);
	}
	else if ( af == AF_INET6 )
	{
		local.in6.sin6_family = AF_INET6;
		local.in6.sin6_addr = in6addr_any;
		local.in6.sin6_port = htobe16(0);
		remote.in6.sin6_family = AF_INET6;
		remote.in6.sin6_addr = in6addr_any;
		remote.in6.sin6_port = htobe16(0);
	}
	// first_packet initialized by constructor
	// last_packet initialized by constructor
	prev_socket = NULL;
	next_socket = NULL;
	receive_current = 0;
	receive_limit = DEFAULT_PACKET_LIMIT * Page::Size();
	send_limit = DEFAULT_PACKET_LIMIT * Page::Size();
	ifindex = 0;
	this->af = af;
	sockerr = 0;
	how_shutdown = 0;
	bound = false;
	broadcast = false;
	connected = false;
	reuseaddr = false;
}

UDPSocket::~UDPSocket()
{
	if ( bound )
	{
		ScopedLock lock(&bind_lock);
		if ( af == AF_INET )
		{
			uint16_t port = be16toh(local.in.sin_port);
			if ( prev_socket )
				prev_socket->next_socket = next_socket;
			else
				bindings_v4[port] = next_socket;
			if ( next_socket )
				next_socket->prev_socket = prev_socket;
		}
		else if ( af == AF_INET6 )
		{
			uint16_t port = be16toh(local.in6.sin6_port);
			if ( prev_socket )
				prev_socket->next_socket = next_socket;
			else
				bindings_v6[port] = next_socket;
			if ( next_socket )
				next_socket->prev_socket = prev_socket;
		}
		bound = false;
	}
	DropReceiveQueue();
}

Ref<Inode> UDPSocket::accept4(ioctx_t* /*ctx*/, uint8_t* /*addr*/,
                              size_t* /*addrsize*/, int /*flags*/)
{
	return errno = EOPNOTSUPP, Ref<Inode>(NULL);
}

bool UDPSocket::ImportAddress(ioctx_t* ctx,
                              union udp_sockaddr* dest,
                              const void* addr,
                              size_t addrsize)
{
	if ( addrsize != AddressFamilySize(af) )
	{
		sa_family_t family;
		if ( sizeof(family) <= addrsize &&
		    ctx->copy_from_src(&family, addr, sizeof(family)) &&
		    family == AF_UNSPEC )
		{
			union udp_sockaddr unspec;
			memset(&unspec, 0, sizeof(unspec));
			unspec.family = AF_UNSPEC;
			memcpy(dest, &unspec, sizeof(unspec));
			return true;
		}
		return errno = EINVAL, false;
	}
	union udp_sockaddr copy;
	memset(&copy, 0, sizeof(copy));
	if ( !ctx->copy_from_src(&copy, addr, addrsize) )
		return false;
	if ( copy.family != af && copy.family != AF_UNSPEC )
		return errno = EAFNOSUPPORT, false;
	memcpy(dest, &copy, sizeof(copy));
	return true;
}

// bind_lock locked, socket_lock locked (in that order)
bool UDPSocket::CanBind(union udp_sockaddr new_local)
{
	if ( af == AF_INET )
	{
		// Bind to either the any address, the broadcast address, the address of
		// a network interface, or the broadcast address of a network interface.
		if ( new_local.in.sin_addr.s_addr != htobe32(INADDR_ANY) &&
		     new_local.in.sin_addr.s_addr != htobe32(INADDR_BROADCAST) )
		{
			// TODO: What happens to sockets if the network interface changes
			//       its address?
			ScopedLock ifs_lock(&netifs_lock);
			bool found = false;
			for ( unsigned int i = 1; i < netifs_count; i++ )
			{
				NetworkInterface* netif = netifs[i];
				if ( !netif )
					continue;
				ScopedLock cfg_lock(&netif->cfg_lock);
				struct in_addr if_broadcast_ip;
				if_broadcast_ip.s_addr = netif->cfg.inet.address.s_addr |
				                         ~netif->cfg.inet.subnet.s_addr;
				if ( memcmp(&netif->cfg.inet.address, &new_local.in.sin_addr,
				            sizeof(struct in_addr)) == 0 ||
				     memcmp(&if_broadcast_ip, &new_local.in.sin_addr,
				            sizeof(struct in_addr)) == 0 )
				{
					found = true;
					break;
				}
			}
			// No interface had the correct address.
			if ( !found )
				return errno = EADDRNOTAVAIL, false;
		}
		uint16_t port = be16toh(new_local.in.sin_port);
		if ( port == 0 )
			return errno = EINVAL, false;
		for ( UDPSocket* socket = bindings_v4[port];
		      socket;
		      socket = socket->next_socket )
		{
			// Taking the lock of the other socket is safe against deadlocks,
			// despite having the lock of this socket, because bind_lock was
			// locked prior to this socket's lock, and bind_lock must always
			// be taken before the same thread locks two sockets.
			ScopedLock lock(&socket->socket_lock);
			if ( new_local.in.sin_addr.s_addr == htobe32(INADDR_ANY) &&
			     !(reuseaddr && socket->reuseaddr) )
				return errno = EADDRINUSE, false;
			if ( socket->local.in.sin_addr.s_addr == htobe32(INADDR_ANY) &&
			     !(reuseaddr && socket->reuseaddr) )
				return errno = EADDRINUSE, false;
			if ( new_local.in.sin_addr.s_addr ==
			     socket->local.in.sin_addr.s_addr )
				return errno = EADDRINUSE, false;
		}
	}
	else if ( af == AF_INET6 )
	{
		// TODO: IPv6 support for seeing if any interface has the address.
		if ( true )
			return errno = EAFNOSUPPORT, false;
		uint16_t port = be16toh(new_local.in6.sin6_port);
		if ( port == 0 )
			return errno = EINVAL, false;
		for ( UDPSocket* socket = bindings_v6[port];
		      socket;
		      socket = socket->next_socket )
		{
			if ( !memcmp(&new_local.in6.sin6_addr, &in6addr_any,
			             sizeof(in6addr_any)) &&
			     !(reuseaddr && socket->reuseaddr) )
			if ( !memcmp(&socket->local.in6.sin6_addr, &in6addr_any,
			             sizeof(in6addr_any)) &&
			     !(reuseaddr && socket->reuseaddr) )
			if ( !memcmp(&new_local.in6.sin6_addr, &socket->local.in6.sin6_addr,
			             sizeof(new_local.in6.sin6_addr)) )
				return errno = EADDRINUSE, false;
		}
	}
	else
		return errno = EAFNOSUPPORT, false;
	return true;
}

int UDPSocket::bind(ioctx_t* ctx, const uint8_t* addr, size_t addrsize)
{
	ScopedLock lock2(&bind_lock);
	ScopedLock lock(&socket_lock);
	if ( bound )
		return errno = EINVAL, -1;
	union udp_sockaddr new_local;
	if ( !ImportAddress(ctx, &new_local, addr, addrsize) )
		return -1;
	if ( new_local.family == AF_UNSPEC )
		return errno = EAFNOSUPPORT, -1;
	uint16_t port;
	if ( af == AF_INET )
		port = be16toh(new_local.in.sin_port);
	else if ( af == AF_INET6 )
		port = be16toh(new_local.in6.sin6_port);
	else
		return errno = EAFNOSUPPORT, -1;
	if ( port == 0 )
		return BindDefault(&new_local) ? 0 : -1;
	if ( !CanBind(new_local) )
		return -1;
	if ( af == AF_INET )
	{
		uint16_t port = be16toh(new_local.in.sin_port);
		if ( bindings_v4[port] )
			bindings_v4[port]->prev_socket = this;
		next_socket = bindings_v4[port];
		prev_socket = NULL;
		bindings_v4[port] = this;
	}
	else if ( af == AF_INET6 )
	{
		uint16_t port = be16toh(new_local.in6.sin6_port);
		if ( bindings_v6[port] )
			bindings_v6[port]->prev_socket = this;
		next_socket = bindings_v6[port];
		prev_socket = NULL;
		bindings_v6[port] = this;
	}
	else
		return errno = EAFNOSUPPORT, -1;
	memcpy(&local, &new_local, sizeof(new_local));
	bound = true;
	return 0;
}

// bind_lock locked, socket_lock locked (in that order)
bool UDPSocket::BindDefault(const union udp_sockaddr* new_local_ptr)
{
	// TODO: This allocator becomes increasingly biased as more ports are
	//       allocated.
	// TODO: Try not to allocate recently used ports.
	union udp_sockaddr new_local;
	if ( new_local_ptr )
		memcpy(&new_local, new_local_ptr, sizeof(union udp_sockaddr));
	else
	{
		memset(&new_local, 0, sizeof(new_local));
		if ( af == AF_INET )
		{
			new_local.in.sin_family = AF_INET;
			new_local.in.sin_addr.s_addr = htobe32(INADDR_ANY);
		}
		else if ( af == AF_INET6 )
		{
			new_local.in6.sin6_family = AF_INET6;
			new_local.in6.sin6_addr = in6addr_any;
		}
		else
			return errno = EAFNOSUPPORT, false;
	}
	uint16_t start = 32768; // Documented in udp(4).
	uint16_t end = 61000; // Documented in udp(4).
	uint16_t count = end - start;
	uint16_t offset = arc4random_uniform(count);
	for ( uint16_t i = 0; i < count; i++ )
	{
		uint16_t j = offset + i;
		if ( count <= j )
			j -= count;
		uint16_t port = start + j;
		if ( af == AF_INET )
			new_local.in.sin_port = htobe16(port);
		else if ( af == AF_INET6 )
			new_local.in6.sin6_port = htobe16(port);
		else
			return errno = EAFNOSUPPORT, false;
		if ( !CanBind(new_local) )
		{
			if ( errno == EADDRINUSE )
				continue;
			return false;
		}
		if ( af == AF_INET )
		{
			if ( bindings_v4[port] )
				bindings_v4[port]->prev_socket = this;
			next_socket = bindings_v4[port];
			prev_socket = NULL;
			bindings_v4[port] = this;
		}
		else if ( af == AF_INET6 )
		{
			if ( bindings_v6[port] )
				bindings_v6[port]->prev_socket = this;
			next_socket = bindings_v6[port];
			prev_socket = NULL;
			bindings_v6[port] = this;
		}
		else
			return errno = EAFNOSUPPORT, false;
		memcpy(&local, &new_local, sizeof(new_local));
		bound = true;
		return true;
	}
	return errno = EAGAIN, false;
}

int UDPSocket::connect(ioctx_t* ctx, const uint8_t* addr, size_t addrsize)
{
	ScopedLock lock2(&bind_lock);
	ScopedLock lock(&socket_lock);
	union udp_sockaddr new_remote;
	if ( !ImportAddress(ctx, &new_remote, addr, addrsize) )
		return -1;
	if ( new_remote.family == AF_UNSPEC )
	{
		// Disconnect the socket when connecting to the AF_UNSPEC family.
		connected = false;
		return 0;
	}
	else if ( af == AF_INET )
	{
		// Verify the port is non-zero.
		if ( be16toh(new_remote.in.sin_port) == 0 )
			return errno = EADDRNOTAVAIL, -1;
	}
	else
		return errno = EAFNOSUPPORT, -1;
	// If the socket is not bound, find a route to the remote address and bind
	// to the appropriate source address.
	if ( !bound )
	{
		union udp_sockaddr new_local;
		memset(&new_local, 0, sizeof(new_local));
		if ( af == AF_INET )
		{
			struct in_addr any;
			any.s_addr = htobe32(INADDR_ANY);
			new_local.in.sin_family = AF_INET;
			if ( !IP::GetSourceIP(&any, &new_remote.in.sin_addr,
			                      &new_local.in.sin_addr, ifindex, NULL) )
				return -1;
			new_local.in.sin_port = htobe16(0);
		}
		else
			return errno = EAFNOSUPPORT, -1;
		if ( !BindDefault(&new_local) )
			return -1;
	}
	// Test if there is a route from the local address to the remote address.
	if ( af == AF_INET )
	{
		if ( !IP::GetSourceIP(&local.in.sin_addr, &new_remote.in.sin_addr, NULL,
		                      ifindex, NULL) )
		{
			// TODO: Rebind to another interface if reconnecting? Note that this
			//       violates the design that sockets can only be bound once.
			//       DragonFly, FreeBSD, Haiku, macOS, NetBSD, OpenBSD, and
			//       OpenIndiana does this, but Hurd and Linux does not. See
			//       os-test's connect-loopback-reconnect-wan-getsockname. If
			//       so, give preference to the same port if available.
			return -1;
		}
	}
	else
		return errno = EAFNOSUPPORT, -1;
	// Set the remote address and become connected.
	connected = true;
	memcpy(&remote, &new_remote, sizeof(new_remote));
	// Discard datagrams not from the new remote, thus enforcing that all
	// datagrams provided by recvmsg always comes from the address connected to.
	size_t name_size = AddressFamilySize(af);
	Ref<Packet>* packet_ptr = &first_packet;
	while ( *packet_ptr )
	{
		void* name = first_packet->from + first_packet->offset;
		if ( memcmp(name, &remote, name_size) != 0 )
		{
			Ref<Packet> next = (*packet_ptr)->next;
			(*packet_ptr)->next.Reset();
			packet_ptr->Reset();
			*packet_ptr = next;
			continue;
		}
		packet_ptr = &(*packet_ptr)->next;
	}
	if ( !first_packet )
		last_packet.Reset();
	return 0;
}

int UDPSocket::listen(ioctx_t* /*ctx*/, int /*backlog*/)
{
	return errno = EOPNOTSUPP, -1;
}

ssize_t UDPSocket::readv(ioctx_t* ctx, const struct iovec* iov, int iovcnt)
{
	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = (struct iovec*) iov;
	msg.msg_iovlen = iovcnt;
	return recvmsg_internal(ctx, &msg, 0);
}

ssize_t UDPSocket::recv(ioctx_t* ctx, uint8_t* buf, size_t count, int flags)
{
	struct iovec iov;
	memset(&iov, 0, sizeof(iov));
	iov.iov_base = (void*) buf;
	iov.iov_len = count;
	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	return recvmsg_internal(ctx, &msg, flags);
}

ssize_t UDPSocket::recvmsg(ioctx_t* ctx, struct msghdr* msg_ptr, int flags)
{
	struct msghdr msg;
	if ( !ctx->copy_from_src(&msg, msg_ptr, sizeof(msg)) )
		return -1;
	if ( msg.msg_iovlen < 0 || IOV_MAX < msg.msg_iovlen )
		return errno = EINVAL, -1;
	size_t iov_size = msg.msg_iovlen * sizeof(struct iovec);
	struct iovec* iov = new struct iovec[msg.msg_iovlen];
	if ( !iov )
		return -1;
	struct iovec* user_iov = msg.msg_iov;
	if ( !ctx->copy_from_src(iov, user_iov, iov_size) )
		return delete[] iov, -1;
	msg.msg_iov = iov;
	ssize_t result = recvmsg_internal(ctx, &msg, flags);
	msg.msg_iov = user_iov;
	delete[] iov;
	if ( !ctx->copy_to_dest(msg_ptr, &msg, sizeof(msg)) )
		return -1;
	return result;
}

ssize_t UDPSocket::recvmsg_internal(ioctx_t* ctx, struct msghdr* msg, int flags)
{
	if ( flags & ~(MSG_PEEK) )
		return errno = EINVAL, -1;
	ScopedLock lock(&socket_lock);
	if ( sockerr )
	{
		errno = sockerr;
		sockerr = 0;
		return -1;
	}
	if ( how_shutdown & SHUT_RD )
		return 0;
	while ( !first_packet )
	{
		if ( ctx->dflags & O_NONBLOCK )
			return errno = EWOULDBLOCK, -1;
		if ( !kthread_cond_wait_signal(&receive_cond, &socket_lock) )
			return errno = EINTR, -1;
	}
	void* name = first_packet->from + first_packet->offset;
	size_t name_size = AddressFamilySize(af);
	assert(name_size <= first_packet->length - first_packet->offset);
	if ( msg->msg_name )
	{
		if ( name_size < msg->msg_namelen )
			msg->msg_namelen = name_size;
		if ( !ctx->copy_to_dest(msg->msg_name, name, msg->msg_namelen) )
			return -1;
	}
	else
		msg->msg_namelen = 0;
	first_packet->offset += name_size;
	const unsigned char* in = first_packet->from + first_packet->offset;
	size_t in_length = first_packet->length - first_packet->offset;
	msg->msg_controllen = 0;
	msg->msg_flags = 0;
	if ( SSIZE_MAX < TruncateIOVec(msg->msg_iov, msg->msg_iovlen, SSIZE_MAX) )
		return errno = EINVAL, -1;
	size_t sofar = 0;
	for ( int i = 0; i < msg->msg_iovlen && sofar < in_length; i++)
	{
		size_t in_left = in_length - sofar;
		const struct iovec* iov = &msg->msg_iov[i];
		size_t count = in_left < iov->iov_len ? in_left : iov->iov_len;
		if ( !ctx->copy_to_dest(iov->iov_base, in + sofar, count) )
			return -1;
		sofar += count;
	}
	if ( sofar < in_length )
		msg->msg_flags |= MSG_TRUNC;
	if ( !(flags & MSG_PEEK) )
	{
		receive_current -= first_packet->pmap.size;
		Ref<Packet> next = first_packet->next;
		first_packet->next.Reset();
		first_packet = next;
		if ( !first_packet )
			last_packet.Reset();
	}
	return sofar;
}

ssize_t UDPSocket::send(ioctx_t* ctx,
                        const uint8_t* buf,
                        size_t count,
                        int flags)
{
	struct iovec iov;
	memset(&iov, 0, sizeof(iov));
	iov.iov_base = (void*) buf;
	iov.iov_len = count;
	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	return sendmsg_internal(ctx, &msg, flags);
}

ssize_t UDPSocket::sendmsg(ioctx_t* ctx,
                           const struct msghdr* msg_ptr,
                           int flags)
{
	struct msghdr msg;
	if ( !ctx->copy_from_src(&msg, msg_ptr, sizeof(msg)) )
		return -1;
	if ( msg.msg_iovlen < 0 || IOV_MAX < msg.msg_iovlen )
		return errno = EINVAL, -1;
	size_t iov_size = msg.msg_iovlen * sizeof(struct iovec);
	struct iovec* iov = new struct iovec[msg.msg_iovlen];
	if ( !iov )
		return -1;
	if ( !ctx->copy_from_src(iov, msg.msg_iov, iov_size) )
		return delete[] iov, -1;
	msg.msg_iov = iov;
	ssize_t result = sendmsg_internal(ctx, &msg, flags);
	delete[] iov;
	return result;
}

ssize_t UDPSocket::sendmsg_internal(ioctx_t* ctx,
                                    const struct msghdr* msg,
                                    int flags)
{
	if ( flags & ~(MSG_NOSIGNAL) ) // TODO: MSG_DONTROUTE
		return errno = EINVAL, -1;
	ScopedLock lock(&socket_lock);
	if ( how_shutdown & SHUT_WR )
	{
		if ( !(flags & MSG_NOSIGNAL) )
			CurrentThread()->DeliverSignal(SIGPIPE);
		return errno = EPIPE, -1;
	}
	if ( sockerr )
	{
		errno = sockerr;
		sockerr = 0;
		return -1;
	}
	union udp_sockaddr sendto;
	if ( msg->msg_name )
	{
		if ( connected )
			return errno = EISCONN, -1;
		if ( af == AF_INET )
		{
			if ( msg->msg_namelen != sizeof(sendto.in) )
				return errno = EINVAL, -1;
			sendto.family = af;
			if ( !ctx->copy_from_src(&sendto.in, msg->msg_name,
			                         sizeof(sendto.in)) )
				return -1;
		}
		// TODO: IPv6 support.
		else
			return errno = EAFNOSUPPORT, -1;
	}
	else if ( connected )
		sendto = remote;
	else
		return errno = EDESTADDRREQ, -1;
	if ( !bound )
	{
		kthread_mutex_unlock(&socket_lock); // Don't deadlock.
		kthread_mutex_lock(&bind_lock);
		kthread_mutex_lock(&socket_lock);
		bool was_bound = BindDefault(NULL);
		kthread_mutex_unlock(&bind_lock);
		if ( !was_bound )
			return -1;
	}
	// Find a route to the destination and verify the port is non-zero.
	union udp_sockaddr sendfrom;
	if ( af == AF_INET )
	{
		if ( be16toh(sendto.in.sin_port) == 0 )
			return errno = EADDRNOTAVAIL, -1;
		if ( !IP::GetSourceIP(&local.in.sin_addr, &sendto.in.sin_addr,
		                      &sendfrom.in.sin_addr, ifindex) )
			return -1;
	}
	// TODO: IPv6 support.
	else
		return errno = EAFNOSUPPORT, -1;
	Ref<Packet> pkt = GetPacket();
	if ( !pkt )
		return -1;
	size_t mtu = pkt->pmap.size;
	if ( mtu < sizeof(struct udphdr) )
		return errno = EMSGSIZE, -1;
	pkt->length = sizeof(struct udphdr);
	unsigned char* out = pkt->from;
	struct udphdr hdr;
	if ( af == AF_INET )
	{
		hdr.uh_sport = local.in.sin_port;
		hdr.uh_dport = sendto.in.sin_port;
	}
	else if ( af == AF_INET6 )
	{
		hdr.uh_sport = local.in6.sin6_port;
		hdr.uh_dport = sendto.in6.sin6_port;
	}
	else
		return errno = EAFNOSUPPORT, -1;
	if ( SSIZE_MAX < TruncateIOVec(msg->msg_iov, msg->msg_iovlen, SSIZE_MAX) )
		return errno = EINVAL, -1;
	size_t count = 0;
	for ( int i = 0; i < msg->msg_iovlen; i++ )
	{
		const struct iovec* iov = &msg->msg_iov[i];
		if ( mtu - pkt->length < iov->iov_len )
			return errno = EMSGSIZE, -1;
		if ( !ctx->copy_from_src(out + pkt->length, iov->iov_base,
		                         iov->iov_len) )
			return -1;
		pkt->length += iov->iov_len;
		count += iov->iov_len;
	}
	hdr.uh_ulen = htobe16(pkt->length);
	memcpy(out, &hdr, sizeof(hdr));
	uint16_t checksum = 0;
	if ( af == AF_INET )
	{
		checksum = IP::ipsum_buf(checksum, &sendfrom.in.sin_addr,
		                         sizeof(struct in_addr));
		checksum = IP::ipsum_buf(checksum, &sendto.in.sin_addr,
		                         sizeof(struct in_addr));
	}
	else if ( af == AF_INET6 )
	{
		checksum = IP::ipsum_buf(checksum, &sendfrom.in6.sin6_addr,
		                         sizeof(struct in6_addr));
		checksum = IP::ipsum_buf(checksum, &sendto.in6.sin6_addr,
		                         sizeof(struct in6_addr));
	}
	else
		return errno = EAFNOSUPPORT, -1;
	checksum = IP::ipsum_word(checksum, IPPROTO_UDP);
	checksum = IP::ipsum_word(checksum, pkt->length);
	checksum = IP::ipsum_buf(checksum, out, pkt->length);
	checksum = IP::ipsum_finish(checksum);
	if ( checksum == 0x0000 )
		checksum = 0xFFFF;
	hdr.uh_sum = htobe16(checksum);
	memcpy(out, &hdr, sizeof(hdr));
	(void) flags;
	if ( af == AF_INET )
	{
		if ( !IP::Send(pkt, &sendfrom.in.sin_addr, &sendto.in.sin_addr,
		               IPPROTO_UDP, ifindex, broadcast) )
			return -1;
	}
	// TODO: IPv6 support.
	else
		return errno = EAFNOSUPPORT, -1;
	return count;
}

ssize_t UDPSocket::writev(ioctx_t* ctx, const struct iovec* iov, int iovcnt)
{
	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = (struct iovec*) iov;
	msg.msg_iovlen = iovcnt;
	return sendmsg_internal(ctx, &msg, 0);
}

short UDPSocket::PollEventStatus()
{
	short status = 0;
	if ( first_packet || (how_shutdown & SHUT_RD) )
		status |= POLLIN | POLLRDNORM;
	if ( !(how_shutdown & SHUT_WR) )
		status |= POLLOUT | POLLWRNORM;
	else
		status |= POLLHUP;
	if ( sockerr )
		status |= POLLERR;
	return status;
}

int UDPSocket::poll(ioctx_t* /*ctx*/, PollNode* node)
{
	ScopedLock lock(&socket_lock);
	short ret_status = PollEventStatus() & node->events;
	if ( ret_status )
	{
		node->master->revents |= ret_status;
		return 0;
	}
	poll_channel.Register(node);
	return errno = EAGAIN, -1;
}

int UDPSocket::getsockopt(ioctx_t* ctx, int level, int option_name,
                          void* option_value, size_t* option_size_ptr)
{
	ScopedLock lock(&socket_lock);

	if ( level == SOL_SOCKET && option_name == SO_BINDTODEVICE )
	{
		ScopedLock lock(&netifs_lock);
		const char* ifname = "";
		if ( ifindex < netifs_count && netifs[ifindex] )
			ifname = netifs[ifindex]->ifinfo.name;
		size_t option_size;
		if ( !CopyFromUser(&option_size, option_size_ptr, sizeof(option_size)) )
			return -1;
		size_t len = strlen(ifname);
		size_t size = len + 1;
		if ( option_size < size )
			return errno = ERANGE, -1;
		if ( !CopyToUser(option_value, ifname, size) ||
		     !CopyToUser(option_size_ptr, &size, sizeof(size)) )
			return -1;
		return 0;
	}

	uintmax_t result = 0;

	if ( level == IPPROTO_UDP )
	{
		switch ( option_name )
		{
		default: return errno = ENOPROTOOPT, -1;
		}
	}
	else if ( level == SOL_SOCKET )
	{
		switch ( option_name )
		{
		case SO_BINDTOINDEX: result = ifindex; break;
		case SO_BROADCAST: result = broadcast; break;
		case SO_DEBUG: result = 0; break;
		case SO_DOMAIN: result = af; break;
		case SO_DONTROUTE: result = 0; break;
		case SO_ERROR: result = sockerr; sockerr = 0; break;
		case SO_PROTOCOL: result = IPPROTO_UDP; break;
		case SO_RCVBUF: result = receive_limit; break;
		case SO_REUSEADDR: result = reuseaddr; break;
		case SO_SNDBUF: result = send_limit; break;
		case SO_TYPE: result = SOCK_DGRAM; break;
		default: return errno = ENOPROTOOPT, -1;
		}
	}
	else
		return errno = EINVAL, -1;

	if ( !sockopt_return_uintmax(result, ctx, option_value, option_size_ptr) )
		return -1;

	return 0;
}

int UDPSocket::setsockopt(ioctx_t* ctx, int level, int option_name,
                          const void* option_value, size_t option_size)
{
	ScopedLock lock(&socket_lock);

	if ( level == SOL_SOCKET && option_name == SO_BINDTODEVICE )
	{
		char ifname[IF_NAMESIZE];
		if ( sizeof(ifname) < option_size )
			option_size = sizeof(ifname);
		if ( !CopyFromUser(ifname, option_value, option_size) )
			return -1;
		if ( strnlen(ifname, option_size) == sizeof(ifname) )
			return errno = ENODEV, -1;
		ifname[option_size] = '\0';
		ScopedLock lock(&netifs_lock);
		for ( size_t i = 1; i < netifs_count; i++ )
		{
			if ( netifs[i] && !strcmp(ifname, netifs[i]->ifinfo.name) )
			{
				ifindex = i;
				return 0;
			}
		}
		return errno = ENODEV, -1;
	}

	uintmax_t value;
	if ( !sockopt_fetch_uintmax(&value, ctx, option_value, option_size) )
		return -1;

	if ( level == IPPROTO_UDP )
	{
		switch ( option_name )
		{
		default: return errno = ENOPROTOOPT, -1;
		}
	}
	else if ( level == SOL_SOCKET )
	{
		switch ( option_name )
		{
		case SO_BINDTOINDEX:
			if ( UINT_MAX < value )
				return errno = EINVAL, -1;
			ifindex = value;
			break;
		case SO_BROADCAST: broadcast = value; break;
		case SO_DEBUG:
			if ( value != 0 )
				return errno = EPERM, -1;
			break;
		case SO_DONTROUTE:
			if ( value != 0 )
				return errno = EPERM, -1;
			break;
		case SO_RCVBUF:
		{
			size_t hard_limit = MAXIMAL_PACKET_LIMIT * Page::Size();
			if ( hard_limit < value )
				value = hard_limit;
			receive_limit = value;
			// Shrink the receive queue until it fits.
			while ( first_packet && receive_limit < receive_current )
			{
				Ref<Packet> packet = first_packet;
				first_packet->next.Reset();
				first_packet = first_packet->next;
				receive_current -= packet->pmap.size;
			}
			if ( !first_packet )
				last_packet.Reset();
			break;
		}
		case SO_REUSEADDR: reuseaddr = value; break;
		case SO_SNDBUF:
		{
			size_t hard_limit = MAXIMAL_PACKET_LIMIT * Page::Size();
			if ( hard_limit < value )
				value = hard_limit;
			// TODO: This value is unused.
			send_limit = value;
			break;
		}
		default: return errno = ENOPROTOOPT, -1;
		}
	}
	else
		return errno = EINVAL, -1;

	return 0;
}

void UDPSocket::DropReceiveQueue(void)
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

int UDPSocket::shutdown(ioctx_t* ctx, int how)
{
	(void) ctx;
	ScopedLock lock(&socket_lock);
	if ( how & ~(SHUT_RD | SHUT_WR) )
		return errno = EINVAL, -1;
	how_shutdown |= how;
	// Drop the receive queue if shut down for read.
	if ( how & SHUT_RD )
		DropReceiveQueue();

	kthread_cond_broadcast(&receive_cond);
	poll_channel.Signal(PollEventStatus());
	return 0;
}

int UDPSocket::getpeername(ioctx_t* ctx, uint8_t* addr, size_t* addrsize_ptr)
{
	ScopedLock lock(&socket_lock);
	if ( !connected )
		return errno = ENOTCONN, -1;
	size_t addrsize;
	if ( !ctx->copy_from_src(&addrsize, addrsize_ptr, sizeof(addrsize)) )
		return -1;
	if ( af == AF_INET )
	{
		if ( sizeof(remote.in) < addrsize )
			addrsize = sizeof(remote.in);
	}
	else if ( af == AF_INET6 )
	{
		if ( sizeof(remote.in6) < addrsize )
			addrsize = sizeof(remote.in6);
	}
	else
		return errno = EAFNOSUPPORT, -1;
	if ( !ctx->copy_to_dest(addr, &remote, addrsize) )
		return -1;
	if ( !ctx->copy_to_dest(addrsize_ptr, &addrsize, sizeof(addrsize)) )
		return -1;
	return 0;
}

int UDPSocket::getsockname(ioctx_t* ctx, uint8_t* addr, size_t* addrsize_ptr)
{
	ScopedLock lock(&socket_lock);
	size_t addrsize;
	if ( !ctx->copy_from_src(&addrsize, addrsize_ptr, sizeof(addrsize)) )
		return -1;
	if ( af == AF_INET )
	{
		if ( sizeof(local.in) < addrsize )
			addrsize = sizeof(local.in);
	}
	else if ( af == AF_INET6 )
	{
		if ( sizeof(local.in6) < addrsize )
			addrsize = sizeof(local.in6);
	}
	else
		return errno = EAFNOSUPPORT, -1;
	if ( !ctx->copy_to_dest(addr, &local, addrsize) )
		return -1;
	if ( !ctx->copy_to_dest(addrsize_ptr, &addrsize, sizeof(addrsize)) )
		return -1;
	return 0;
}

// socket_lock locked
void UDPSocket::ReceivePacket(Ref<Packet> pkt)
{
	if ( how_shutdown & SHUT_RD )
		return;
	// Drop the packet if the receive queue is full.
	if ( receive_limit < receive_current )
		return;
	size_t available = receive_limit - receive_current;
	if ( available < pkt->pmap.size )
		return;
	// Add the packet to the receive queue.
	receive_current += pkt->pmap.size;
	if ( last_packet )
	{
		last_packet->next = pkt;
		last_packet = pkt;
	}
	else
	{
		first_packet = pkt;
		last_packet = pkt;
	}
	kthread_cond_broadcast(&receive_cond);
	poll_channel.Signal(PollEventStatus());
}

void HandleIP(Ref<Packet> pkt,
              const struct in_addr* src,
              const struct in_addr* dst,
              bool dst_broadcast)
{
	(void) dst_broadcast;
	const unsigned char* in = pkt->from + pkt->offset;
	size_t inlen = pkt->length - pkt->offset;
	struct udphdr hdr;
	if ( inlen < sizeof(hdr) )
		return;
	memcpy(&hdr, in, sizeof(hdr));
	hdr.uh_sport = be16toh(hdr.uh_sport);
	hdr.uh_dport = be16toh(hdr.uh_dport);
	hdr.uh_ulen = be16toh(hdr.uh_ulen);
	hdr.uh_sum = be16toh(hdr.uh_sum);
	if ( hdr.uh_sum )
	{
		uint16_t sum = 0;
		sum = IP::ipsum_buf(sum, src, sizeof(struct in_addr));
		sum = IP::ipsum_buf(sum, dst, sizeof(struct in_addr));
		sum = IP::ipsum_word(sum, IPPROTO_UDP);
		sum = IP::ipsum_word(sum, hdr.uh_ulen);
		sum = IP::ipsum_buf(sum, in, inlen);
		if ( sum != 0 && sum != 0xFFFF )
			return;
	}
	if ( hdr.uh_ulen < sizeof(hdr) )
		return;
	if ( inlen < hdr.uh_ulen )
		return;
	pkt->length = pkt->offset + hdr.uh_ulen;
	pkt->offset += sizeof(hdr);
	// Port 0 is not valid.
	if ( hdr.uh_sport == 0 || hdr.uh_dport == 0 )
		return;
	ScopedLock lock1(&bind_lock);
	// Find the socket that would receive the datagram sent to that address
	// and port, or if no such socket, perhaps a socket bound to the any address
	// and that port.
	UDPSocket* socket = NULL;
	UDPSocket* any_socket = NULL;
	for ( UDPSocket* iter = bindings_v4[hdr.uh_dport];
	      !socket && iter;
	      iter = iter->next_socket )
	{
		// Receive the datagram only if sent to the socket's address.
		if ( !memcmp(&iter->local.in.sin_addr, dst, sizeof(*dst)) )
			socket = iter;
		// Receive the datagram only if the socket's address was the any address
		// (and no other socket is bound to the datagram's destination address
		//  and port).
		if ( iter->local.in.sin_addr.s_addr == htobe32(INADDR_ANY) )
			any_socket = iter;
	}
	// If no socket was bound to the datagram's destination address and port,
	// try to deliver it to a socket bound to the any address and that port.
	if ( !socket )
		socket = any_socket;
	// Drop the datagram is no socket would receive it.
	if ( !socket )
		return;
	// If connected, require the source address is the remote address and the
	// source port is the remote port, otherwise drop the datagram.
	if ( socket->connected &&
	     (memcmp(&socket->remote.in.sin_addr, src, sizeof(*src)) != 0 ||
	      be16toh(socket->remote.in.sin_port) != hdr.uh_sport) )
		return;
	ScopedLock lock2(&socket->socket_lock);
	// If the socket is bound to a network interface, require the datagram to
	// have been received on that network interface.
	if ( socket->ifindex && socket->ifindex != pkt->netif->ifinfo.linkid )
		return;
	// Prepend the source address to the packet.
	struct sockaddr_in from_addr;
	memset(&from_addr, 0, sizeof(from_addr));
	from_addr.sin_family = AF_INET;
	from_addr.sin_port = htobe16(hdr.uh_sport);
	from_addr.sin_addr = *src;
	if ( pkt->offset < sizeof(from_addr) )
		return;
	pkt->offset -= sizeof(from_addr);
	memcpy(pkt->from + pkt->offset, &from_addr, sizeof(from_addr));
	// Receive the datagram on the socket.
	socket->ReceivePacket(pkt);
}

Ref<Inode> Socket(int af)
{
	if ( !IsSupportedAddressFamily(af) )
		return errno = EAFNOSUPPORT, Ref<Inode>(NULL);
	return Ref<Inode>(new UDPSocket(af));
}

} // namespace UDP
} // namespace Sortix
