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
 * net/tcp.cpp
 * Transmission Control Protocol.
 */

// TODO: Plan:
//
//       - TCP_OFFSET_DECODE.
//       - Implement waiting 2 MSL after socket is closed.
//       - Implement sending back RST and such.
//       - os-test all the things.
//       - Work through the receive/transmit details according to the standards.
//       - PUSH.
//       - URG?
//       - Nagle's algorithm, MSG_MORE, TCP_CORK, TCP_NODELAY, etc.
//       - TCP options.
//       - Buffer and segment sizes (respect TCP_MSS).
//       - Efficient receieve queue when out of order.
//       - Efficient backlog / half-open? Avoid denial of service attacks?
//       - Measure average round trip time for efficient retransmission?
//       - High speed extensions?
//       - Anti-congestion extensions?
//       - Selective acknowledgements.
//       - Documentation.

// TODO: Read RFC 793 and comment where each requirement is implemented.
// TODO: Read RFC 1122's section on TCP.
// TODO: Write tcp(4) documentation.
// TODO: os-test tcp.

#include <sys/socket.h>
#include <sys/stat.h>

#include <arpa/inet.h>
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef IOV_MAX
#include <sortix/limits.h>
#endif

#include <sortix/kernel/clock.h>
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
#include <sortix/kernel/time.h>
#include <sortix/kernel/timer.h>
#include <sortix/kernel/worker.h>

#include "ipv4.h"
#include "tcp.h"

// TODO: Why 2 times the maximum window size? Document this?
#define BUFFER_SIZE (2 * 64 * 1024)

// TODO: IMPLEMENTATION DETAILS the number of retransmissions.
#define NUM_RETRANSMISSIONS 6

namespace Sortix {
namespace TCP {

class TCPSocket;

// TODO: Implement PUSH.
// TODO: Implement URG?

union tcp_sockaddr
{
	sa_family_t family;
	struct sockaddr_in in;
	struct sockaddr_in6 in6;
};

// The TCP states per STD 7 (RFC 793).
enum tcp_state
{
	TCP_STATE_CLOSED,
	TCP_STATE_LISTEN,
	TCP_STATE_SYN_SENT,
	TCP_STATE_SYN_RECV,
	TCP_STATE_ESTAB,
	TCP_STATE_FIN_WAIT_1,
	TCP_STATE_CLOSE_WAIT,
	TCP_STATE_FIN_WAIT_2,
	TCP_STATE_CLOSING,
	TCP_STATE_LAST_ACK,
	TCP_STATE_TIME_WAIT,
};

enum tcp_special
{
	TCP_SPECIAL_NOT,
	TCP_SPECIAL_PENDING,
	TCP_SPECIAL_WINDOW,
	TCP_SPECIAL_ACKED,
};

// Global lock protecting all TCP sockets as they need to access each other.
static kthread_mutex_t tcp_lock = KTHREAD_MUTEX_INITIALIZER;

static TCPSocket** bindings_v4;
static TCPSocket** bindings_v6;

static TCPSocket* all_first_socket;
static TCPSocket* all_last_socket;

void Init()
{
	if ( !(bindings_v4 = new TCPSocket*[65536]) ||
	     !(bindings_v6 = new TCPSocket*[65536]) )
		Panic("Failed to allocate TCP Socket bindings");
	for ( size_t i = 0; i < 65536; i++ )
	{
		bindings_v4[i] = NULL;
		bindings_v6[i] = NULL;
	}
}

static inline bool mod32_le(tcp_seq a, tcp_seq b)
{
	return (int32_t) (a - b) <= 0;
}

static inline bool mod32_lt(tcp_seq a, tcp_seq b)
{
	return (int32_t) (a - b) < 0;
}

static inline bool mod32_ge(tcp_seq a, tcp_seq b)
{
	return (int32_t) (a - b) >= 0;
}

static inline bool mod32_gt(tcp_seq a, tcp_seq b)
{
	return (int32_t) (a - b) > 0;
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

// The TCP socket implementation. It is separate from the class TCPSocketNode
// as that class is reference counted, but this class manages its own lifetime
// so the socket is properly shut down after all references are closed.
//
// Bound sockets are in a double linked list starting from the appropriate
// bindings array indexed by the port, and then the sockets on that port are
// doubly linked using prev_socket and next_socket.
//
// Half-open sockets are in a doubly linked list starting from connecting_half
// in the listening socket, and then doubly linked with connecting_prev and
// connecting_next (with connecting_parent going back to the listening socket).
//
// Ready sockets that have not yet been accepted are in a doubly linked list
// starting from connecting_ready in the listening socket, and then doubly
// linked with connecting_prev and connecting_next (with connecting_parent going
// back to the listening socket).
//
// A socket wants to be deleted when it's in the CLOSED state and is not
// referenced by its TCPSocketNode anymore. Deletion is possible when the timer
// and transmission worker threads are not pending.
class TCPSocket
{
	friend void HandleIPv4(Ref<Packet> pkt,
	                       const struct in_addr* src,
	                       const struct in_addr* dst,
	                       bool dst_broadcast);

public:
	TCPSocket(int af);
	~TCPSocket();
	Ref<Inode> accept4(ioctx_t* ctx, uint8_t* addr, size_t* addrsize,
	                   int flags);
	int bind(ioctx_t* ctx, const uint8_t* addr, size_t addrsize);
	int connect(ioctx_t* ctx, const uint8_t* addr, size_t addrsize);
	int listen(ioctx_t* ctx, int backlog);
	ssize_t recv(ioctx_t* ctx, uint8_t* buf, size_t count, int flags);
	ssize_t send(ioctx_t* ctx, const uint8_t* buf, size_t count, int flags);
	ssize_t sendmsg(ioctx_t* ctx, const struct msghdr* msg_ptr, int flags);
	ssize_t read(ioctx_t* ctx, uint8_t* buf, size_t count);
	ssize_t recvmsg(ioctx_t* ctx, struct msghdr* msg, int flags);
	ssize_t write(ioctx_t* ctx, const uint8_t* buf, size_t count);
	int poll(ioctx_t* ctx, PollNode* node);
	int getsockopt(ioctx_t* ctx, int level, int option_name, void* option_value,
	                size_t* option_size_ptr);
	int setsockopt(ioctx_t* ctx, int level, int option_name,
	               const void* option_value, size_t option_size);
	int shutdown(ioctx_t* ctx, int how);
	int getpeername(ioctx_t* ctx, uint8_t* addr, size_t* addrsize);
	int getsockname(ioctx_t* ctx, uint8_t* addr, size_t* addrsize);

public:
	size_t Describe(char* buf, size_t buflen);
	void Unreference();
	void ProcessPacket(Ref<Packet> pkt, union tcp_sockaddr* pkt_src,
	                   union tcp_sockaddr* pkt_dst);
	void ReceivePacket(Ref<Packet> pkt, union tcp_sockaddr* pkt_src,
	                   union tcp_sockaddr* pkt_dst);
	void TransmitWork();
	void OnTimer();

	inline bool want_destruction()
	{
		return state == TCP_STATE_CLOSED && !is_referenced;
	}

	inline bool can_destroy()
	{
		return want_destruction() && !transmit_scheduled && !timer_armed;
	}

private:
	short PollEventStatus();
	bool ImportAddress(ioctx_t* ctx, union tcp_sockaddr* dest, const void* addr,
	                   size_t addrsize);
	bool CanBind(union tcp_sockaddr new_local);
	bool BindDefault(const union tcp_sockaddr* new_local_ptr);
	void UpdateWindow(uint16_t new_window);
	void TransmitLoop();
	bool Transmit();
	void ScheduleTransmit();
	void DoScheduleTransmit();
	void SetTimer();
	void Close();
	void Destroy();
	void Disconnect();
	void Fail(int error);
	ssize_t recv_unlocked(ioctx_t* ctx, uint8_t* buf, size_t count, int flags);
	ssize_t send_unlocked(ioctx_t* ctx, const uint8_t* buf, size_t count,
	                      int flags);
	int shutdown_unlocked(int how);

public:
	// The previous socket bound on the same port in the address family.
	TCPSocket* prev_socket;

	// The next socket bound on the same port in the address family.
	TCPSocket* next_socket;

	// The first half-connected socket in our listening queue.
	TCPSocket* connecting_half;

	// The first ready socket in our listening queue.
	TCPSocket* connecting_ready;

	// The previous half-connected or ready socket in our listening queue.
	TCPSocket* connecting_prev;

	// The next half-connected or ready socket in our listening queue.
	TCPSocket* connecting_next;

	// The listening socket this socket is in the listening queue for.
	TCPSocket* connecting_parent;

	// DEBUG: The previous socket of all sockets.
	TCPSocket* all_prev_socket;

	// DEBUG: The next socket of all sockets.
	TCPSocket* all_next_socket;

	// Condition variable that is signaled when new data can be received.
	kthread_cond_t receive_cond;

	// Condition variable that is signaled when new data can be transmitted.
	kthread_cond_t transmit_cond;

	// The local socket name, or the any address port 0 if not set.
	union tcp_sockaddr local;

	// The remote socket name, or the any address port 0 if not set.
	union tcp_sockaddr remote;

	// The network interface the socket is bound to, or 0 if none.
	unsigned int ifindex;

	// Whether the socket has been bound to a port.
	bool bound;

	// Whether the socket is receiving datagrams.
	bool remoted;

	// Whether SO_REUSEADDR is set.
	bool reuseaddr;

	// Whether the socket is referenced from anywhere and must not deallocate.
	bool is_referenced;

private:
	// The timer used for retransmissions and timing out the connection.
	Timer timer;

	// The poll channel to publish poll bit changes on.
	PollChannel poll_channel;

	// The queue of incoming packets whose sequence numbers are too high to
	// process right now, sorted by increasing sequence number.
	Ref<Packet> receive_queue; // TODO: Not a good way to keep track of this.

	// The offset at which data begins in the incoming ring buffer.
	size_t incoming_offset;

	// The amount of bytes in the incoming ring buffer.
	size_t incoming_used;

	// The offset at which data begins in the outgoing ring buffer.
	size_t outgoing_offset;

	// The amount of bytes in the outgoing ring buffer.
	size_t outgoing_used;

	// Send unacknowledged (STD 7, RFC 793).
	tcp_seq send_una;

	// Send next (STD 7, RFC 793).
	tcp_seq send_nxt;

	// Send window (STD 7, RFC 793).
	tcp_seq send_wnd;

	// Send urgent pointer (STD 7, RFC 793).
	tcp_seq send_up;

	// Segment sequence number used for last window update (STD 7, RFC 793).
	tcp_seq send_wl1;

	// Segment acknowledgment number used for last window update (STD 7, RFC
	// 793).
	tcp_seq send_wl2;

	// Next sequence to send (STD 7, RFC 793).
	tcp_seq send_pos;

	// Initial send sequence number (STD 7, RFC 793).
	tcp_seq iss;

	// Receive next (STD 7, RFC 793).
	tcp_seq recv_nxt;

	// Receive window (STD 7, RFC 793).
	tcp_seq recv_wnd;

	// Receive urgent pointer (STD 7, RFC 793).
	tcp_seq recv_up;

	// Last sequence acked (STD 7, RFC 793).
	tcp_seq recv_acked;

	// Last window size advertised (STD 7, RFC 793).
	tcp_seq recv_wndlast;

	// Initial receive sequence number (STD 7, RFC 793).
	tcp_seq irs;

	// The address family to which this socket belongs.
	int af;

	// Set to an errno value if a socket error has occured, or 0 otherwise.
	int sockerr;

	// The number of sockets in the listening queue.
	int backlog_used;

	// The maximum number of sockets in the listening queue.
	int backlog_max;

	// The number of retransmissions that have occured since the last
	// acknowledgement from the remote socket.
	unsigned int retransmissions;

	// The current TCP state.
	enum tcp_state state;

	// The state of the outgoing SYN.
	enum tcp_special outgoing_syn;

	// The state of the outgoing FIN.
	enum tcp_special outgoing_fin;

	// Whether SYN has been received from the remote socket.
	bool has_syn;

	// Whether FIN has been received from the remote socket.
	bool has_fin;

	// Whether a transmission has been scheduled.
	bool transmit_scheduled;

	// Whether the current owner of the tcp_lock should schedule a transmission
	// after unlocking the lock.
	bool transmit_do_schedule_worker;

	// Whether the timer is pending.
	bool timer_armed;

	// The incoming ring buffer.
	unsigned char incoming[BUFFER_SIZE];

	// The outgoing ring buffer.
	unsigned char outgoing[BUFFER_SIZE];
};

// The TCP socket Inode with a reference counted lifetime. The backend class
// TCPSocket is separate as it may stay alive for a little while after all
// references to it has been lost.
class TCPSocketNode : public AbstractInode
{
public:
	TCPSocketNode(TCPSocket* socket);
	virtual ~TCPSocketNode();
	virtual Ref<Inode> accept4(ioctx_t* ctx, uint8_t* addr, size_t* addrsize,
	                           int flags);
	virtual int bind(ioctx_t* ctx, const uint8_t* addr, size_t addrsize);
	virtual int connect(ioctx_t* ctx, const uint8_t* addr, size_t addrsize);
	virtual int listen(ioctx_t* ctx, int backlog);
	virtual ssize_t recv(ioctx_t* ctx, uint8_t* buf, size_t count, int flags);
	virtual ssize_t recvmsg(ioctx_t* ctx, struct msghdr* msg, int flags);
	virtual ssize_t send(ioctx_t* ctx, const uint8_t* buf, size_t count,
	                     int flags);
	virtual ssize_t sendmsg(ioctx_t* ctx, const struct msghdr* msg_ptr,
	                        int flags);
	virtual ssize_t read(ioctx_t* ctx, uint8_t* buf, size_t count);
	virtual ssize_t write(ioctx_t* ctx, const uint8_t* buf, size_t count);
	virtual int poll(ioctx_t* ctx, PollNode* node);
	virtual int getsockopt(ioctx_t* ctx, int level, int option_name,
	                       void* option_value, size_t* option_size_ptr);
	virtual int setsockopt(ioctx_t* ctx, int level, int option_name,
	                       const void* option_value, size_t option_size);
	virtual int shutdown(ioctx_t* ctx, int how);
	virtual int getpeername(ioctx_t* ctx, uint8_t* addr, size_t* addrsize);
	virtual int getsockname(ioctx_t* ctx, uint8_t* addr, size_t* addrsize);

private:
	TCPSocket* socket;

};

void TCPSocket__OnTimer(Clock* /*clock*/, Timer* /*timer*/, void* user)
{
	((TCPSocket*) user)->OnTimer();
}

TCPSocket::TCPSocket(int af) // DEBUG: tcp_lock taken
{
	prev_socket = NULL;
	next_socket = NULL;
	connecting_half = NULL;
	connecting_ready = NULL;
	connecting_prev = NULL;
	connecting_next = NULL;
	connecting_parent = NULL;
	receive_cond = KTHREAD_COND_INITIALIZER;
	transmit_cond = KTHREAD_COND_INITIALIZER;
	memset(&local, 0, sizeof(local));
	memset(&remote, 0, sizeof(remote));
	ifindex = 0;
	bound = false;
	remoted = false;
	reuseaddr = false;
	// timer is initialized by its constructor.
	timer.Attach(Time::GetClock(CLOCK_MONOTONIC));
	// poll_channel is initialized by its constructor.
	// receive_queue is initialized by its constructor.
	incoming_offset = 0;
	incoming_used = 0;
	outgoing_offset = 0;
	outgoing_used = 0;
	send_una = 0;
	send_nxt = 0;
	send_wnd = 0;
	send_up = 0;
	send_wl1 = 0;
	send_wl2 = 0;
	send_pos = 0;
	iss = 0;
	recv_nxt = 0;
	recv_wnd = 0;
	recv_up = 0;
	recv_acked = 0;
	recv_wndlast = 0;
	irs = 0;
	this->af = af;
	sockerr = 0;
	backlog_used = 0;
	backlog_max = 0;
	retransmissions = 0;
	state = TCP_STATE_CLOSED;
	outgoing_syn = TCP_SPECIAL_NOT;
	outgoing_fin = TCP_SPECIAL_NOT;
	has_syn = false;
	has_fin = false;
	transmit_scheduled = false;
	transmit_do_schedule_worker = false;
	is_referenced = false;
	timer_armed = false;
	memset(incoming, 0, sizeof(incoming));
	memset(outgoing, 0, sizeof(outgoing));
	// DEBUG
	all_prev_socket = all_last_socket;
	all_next_socket = NULL;
	(all_last_socket ?
	 all_last_socket->all_next_socket : all_first_socket) = this;
	all_last_socket = this;
}

TCPSocket::~TCPSocket() // DEBUG: tcp_lock taken
{
	assert(state == TCP_STATE_CLOSED || state == TCP_STATE_LISTEN);
	assert(!bound);
	assert(!prev_socket);
	assert(!next_socket);
	assert(!connecting_half);
	assert(!connecting_half);
	assert(!connecting_ready);
	assert(!connecting_prev);
	assert(!connecting_next);
	assert(!connecting_parent);
	assert(!is_referenced);
	// DEBUG
	(all_prev_socket ?
	 all_prev_socket->all_next_socket : all_first_socket) = all_next_socket;
	(all_next_socket ?
	 all_next_socket->all_prev_socket : all_last_socket) = all_prev_socket;
	all_prev_socket = NULL;
	all_next_socket = NULL;
}

// DEBUG
size_t TCPSocket::Describe(char* buf, size_t buflen) // tcp_lock taken
{
	const char* const STATE_NAMES[] =
	{
		"CLOSED",
		"LISTEN",
		"SYN_SENT",
		"SYN_RECV",
		"ESTAB",
		"FIN_WAIT_1",
		"CLOSE_WAIT",
		"FIN_WAIT_2",
		"CLOSING",
		"LAST_ACK",
		"TIME_WAIT",
	};
	const char* state_name = STATE_NAMES[state];
	char local_str[INET_ADDRSTRLEN];
	char remote_str[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &local.in.sin_addr, local_str, sizeof(local_str));
	inet_ntop(AF_INET, &remote.in.sin_addr, remote_str, sizeof(remote_str));
	char timeout[64] = "none";
	if ( timer_armed )
	{
		struct itimerspec its;
		timer.Get(&its);
		snprintf(timeout, sizeof(timeout), "%ji.%09li",
		         (intmax_t) its.it_value.tv_sec, its.it_value.tv_nsec);
	}
	return snprintf(buf, buflen,
	                "%s %s %u -> %s %u"
	                " timeout=%s resends=%u sockerr=%i transmit=%i refed=%i\n",
	                state_name, local_str, be16toh(local.in.sin_port),
	                remote_str, be16toh(remote.in.sin_port), timeout,
	                retransmissions, sockerr, transmit_scheduled,
	                is_referenced);
}

void TCPSocket::Unreference()
{
	kthread_mutex_lock(&tcp_lock);
	is_referenced = false;
	Disconnect();
	bool do_delete = can_destroy();
	bool do_schedule_worker = transmit_do_schedule_worker;
	transmit_do_schedule_worker = false;
	kthread_mutex_unlock(&tcp_lock);
	if ( do_delete )
		delete this;
	else if ( do_schedule_worker )
		DoScheduleTransmit();
}

void TCPSocket::Close() // tcp_lock taken
{
	if ( timer_armed && timer.TryCancel() )
		timer_armed = false;
	Destroy();
	state = TCP_STATE_CLOSED;
	// TODO: Except when called from Unreference.
	SetTimer();
}

void TCPSocket::Disconnect() // tcp_lock taken
{
	if ( state != TCP_STATE_CLOSED &&
	     state != TCP_STATE_LISTEN &&
	     state != TCP_STATE_SYN_SENT &&
	     state != TCP_STATE_SYN_RECV )
	{
		shutdown_unlocked(SHUT_RDWR);
		return;
	}
	Close();
}

void TCPSocket::Fail(int error)
{
	sockerr = error;
	Destroy();
	state = TCP_STATE_CLOSED;
	kthread_cond_broadcast(&transmit_cond);
	kthread_cond_broadcast(&receive_cond);
	poll_channel.Signal(PollEventStatus());
	SetTimer();
}

void TCPSocket::Destroy() // tcp_lock taken
{
	if ( bound )
	{
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
		prev_socket = NULL;
		next_socket = NULL;
		bound = false;
	}
	while ( connecting_half || connecting_ready )
	{
		TCPSocket* socket;
		if ( connecting_half )
		{
			socket = connecting_half;
			connecting_half = socket->connecting_next;
			if ( connecting_half )
				connecting_half->connecting_prev = NULL;
		}
		else
		{
			socket = connecting_ready;
			connecting_ready = socket->connecting_next;
			if ( connecting_ready )
				connecting_ready->connecting_prev = NULL;
		}
		socket->connecting_prev = NULL;
		socket->connecting_next = NULL;
		socket->connecting_parent = NULL;
		backlog_used--;
		// TODO: Transmission can't be scheduled here since tcp_lock is taken,
		//       use a timer instead.
		socket->Disconnect();
	}
	if ( connecting_parent )
	{
		if ( connecting_prev )
			connecting_prev->connecting_next = connecting_next;
		else if ( state == TCP_STATE_SYN_RECV )
			connecting_parent->connecting_half = connecting_next;
		else
			connecting_parent->connecting_ready = connecting_next;
		if ( connecting_next )
			connecting_next->connecting_prev = connecting_prev;
		connecting_prev = NULL;
		connecting_next = NULL;
		// TODO: Review backlog_used is accounted correctly.
		connecting_parent->backlog_used--;
		connecting_parent = NULL;
	}
}

Ref<Inode> TCPSocket::accept4(ioctx_t* ctx, uint8_t* addr, size_t* addrsize_ptr,
                              int flags)
{
	if ( flags & ~(0) )
		return errno = EINVAL, Ref<Inode>(NULL);
	if ( addr && !addrsize_ptr )
		return errno = EINVAL, Ref<Inode>(NULL);
	ScopedLock lock(&tcp_lock);
	if ( state != TCP_STATE_LISTEN )
		return errno = EINVAL, Ref<Inode>(NULL);
	while ( !connecting_ready )
	{
		if ( ctx->dflags & O_NONBLOCK )
			return errno = EWOULDBLOCK, Ref<Inode>(NULL);
		if ( !kthread_cond_wait_signal(&receive_cond, &tcp_lock) )
			return errno = EINTR, Ref<Inode>(NULL);
	}
	TCPSocket* socket = connecting_ready;
	if ( addr )
	{
		size_t addrsize;
		if ( !ctx->copy_from_src(&addrsize, addrsize_ptr, sizeof(addrsize)) )
			return Ref<Inode>(NULL);
		size_t af_addrsize = AddressFamilySize(af);
		if ( af_addrsize < addrsize )
			addrsize = af_addrsize;
		if ( !ctx->copy_to_dest(addr, &socket->remote, addrsize) )
			return Ref<Inode>(NULL);
		if ( !ctx->copy_to_dest(addrsize_ptr, &addrsize, sizeof(addrsize)) )
			return Ref<Inode>(NULL);
	}
	Ref<TCPSocketNode> result(new TCPSocketNode(socket));
	if ( !result )
		return Ref<Inode>(NULL);
	connecting_ready = socket->connecting_next;
	if ( connecting_ready )
		connecting_ready->connecting_prev = NULL;
	socket->connecting_prev = NULL;
	socket->connecting_next = NULL;
	socket->connecting_parent = NULL;
	backlog_used--;
	return result;
}

bool TCPSocket::ImportAddress(ioctx_t* ctx,
                              union tcp_sockaddr* dest,
                              const void* addr,
                              size_t addrsize)
{
	// TODO: os-test whether AF_UNSPEC can disconnect.
	if ( addrsize != AddressFamilySize(af) )
		return errno = EINVAL, -1;
	union tcp_sockaddr copy;
	memset(&copy, 0, sizeof(copy));
	if ( !ctx->copy_from_src(&copy, addr, addrsize) )
		return false;
	if ( copy.family != af )
		return errno = EAFNOSUPPORT, false;
	memcpy(dest, &copy, sizeof(copy));
	return true;
}

bool TCPSocket::CanBind(union tcp_sockaddr new_local) // tcp_lock taken
{
	if ( af == AF_INET )
	{
		// TODO: os-test binding to broadcast addresses.
		// Bind to either the any address or the address of a network interface.
		if ( new_local.in.sin_addr.s_addr != htobe32(INADDR_ANY) )
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
				if ( memcmp(&netif->cfg.inet.address, &new_local.in.sin_addr,
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
		for ( TCPSocket* socket = bindings_v4[port];
		      socket;
		      socket = socket->next_socket )
		{
			// TODO: os-test how SO_REUSEADDR works for TCP.
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
		if ( bindings_v6[port] )
			return errno = EADDRINUSE, false;
		for ( TCPSocket* socket = bindings_v6[port];
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

int TCPSocket::bind(ioctx_t* ctx, const uint8_t* addr, size_t addrsize)
{
	ScopedLock lock(&tcp_lock);
	if ( bound )
		return errno = EINVAL, -1;
	union tcp_sockaddr new_local;
	if ( !ImportAddress(ctx, &new_local, addr, addrsize) )
		return -1;
	uint16_t port;
	if ( af == AF_INET )
		port = be16toh(new_local.in.sin_port);
	else if ( af == AF_INET6 )
		port = be16toh(new_local.in6.sin6_port);
	else
		return errno = EAFNOSUPPORT, -1;
	// TODO: Binding to the any address needs to pick the appropriate source
	//       interface and bind to its address. (Or really? udp doesn't?
	//       os-test?)
	// TODO: os-test a server listening on any, and then getsockname a
	//       connection received on that port.
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
			return errno = EADDRINUSE, -1;
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

// tcp_lock locked
bool TCPSocket::BindDefault(const union tcp_sockaddr* new_local_ptr)
{
	// TODO: This allocator becomes increasingly biased as more ports are
	//       allocated.
	// TODO: Try not to allocate recently used ports.
	union tcp_sockaddr new_local;
	if ( new_local_ptr )
		memcpy(&new_local, new_local_ptr, sizeof(union tcp_sockaddr));
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
	uint16_t start = 32768; // TODO: Document in tcp(4).
	uint16_t end = 61000; // TODO: Document in tcp(4).
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

void TCPSocket::TransmitLoop()
{
	if ( state == TCP_STATE_CLOSED )
		return;
	if ( NUM_RETRANSMISSIONS <= retransmissions )
	{
		Fail(ETIMEDOUT);
		return;
	}
	if ( !Transmit() && NUM_RETRANSMISSIONS - 1 <= retransmissions )
	{
		Fail(errno);
		return;
	}
	SetTimer();
}

bool TCPSocket::Transmit()
{
	if ( state == TCP_STATE_CLOSED )
		return (errno = sockerr ? sockerr : ENOTCONN), false;

	// Move new outgoing data into the transmission window if there is room.
	tcp_seq window_available = (tcp_seq) (send_una + send_wnd - send_nxt);
	if ( window_available && outgoing_syn == TCP_SPECIAL_PENDING )
	{
		send_nxt++;
		outgoing_syn = TCP_SPECIAL_WINDOW;
		window_available--;
	}
	if ( window_available )
	{
		tcp_seq window_data = (tcp_seq)(send_nxt - send_una);
		if ( outgoing_syn == TCP_SPECIAL_WINDOW )
			window_data--;
		if ( outgoing_fin == TCP_SPECIAL_WINDOW )
			window_data--;
		assert(window_data <= outgoing_used);
		size_t outgoing_new = outgoing_used - window_data;
		tcp_seq amount = window_available;
		if ( outgoing_new < amount )
			amount = outgoing_new;
		send_nxt += amount;
		window_available -= amount;
	}
	if ( window_available && outgoing_fin == TCP_SPECIAL_PENDING )
	{
		send_nxt++;
		outgoing_fin = TCP_SPECIAL_WINDOW;
		window_available--;
	}

	// Transmit packets.
	while ( mod32_lt(send_pos, send_nxt) ||
	        (has_syn && mod32_lt(recv_acked, recv_nxt)) ||
	        recv_wnd != recv_wndlast )
	{
		size_t mtu;
		union tcp_sockaddr sendfrom;
		if ( af == AF_INET )
		{
			if ( !IPv4::GetSourceIP(&local.in.sin_addr, &remote.in.sin_addr,
				                    &sendfrom.in.sin_addr, ifindex, &mtu) )
				return false;
		}
		// TODO: IPv6 support.
		else
			return errno = EAFNOSUPPORT, false;
		if ( mtu < sizeof(struct tcphdr) )
			return errno = EINVAL, false;
		mtu -= sizeof(struct tcphdr);
		Ref<Packet> pkt = GetPacket();
		if ( !pkt )
			return false;
		pkt->length = sizeof(struct tcphdr);
		unsigned char* out = pkt->from;
		struct tcphdr hdr;
		if ( af == AF_INET )
		{
			hdr.th_sport = local.in.sin_port;
			hdr.th_dport = remote.in.sin_port;
		}
		else if ( af == AF_INET6 )
		{
			hdr.th_sport = local.in6.sin6_port;
			hdr.th_dport = remote.in6.sin6_port;
		}
		else
			return errno = EAFNOSUPPORT, false;
		hdr.th_seq = htobe32(send_pos);
		hdr.th_offset = TCP_OFFSET_ENCODE(sizeof(struct tcphdr) / 4);
		hdr.th_flags = 0;
		tcp_seq send_nxtpos = send_pos;
		assert(mod32_le(send_nxtpos, send_nxt));
		if ( outgoing_syn == TCP_SPECIAL_WINDOW && send_nxtpos == send_una )
		{
			hdr.th_flags |= TH_SYN;
			send_nxtpos++;
		}
		assert(mod32_le(send_nxtpos, send_nxt));
		if ( has_syn )
		{
			// TODO: RFC 1122 4.2.2.6:
			//       "TCP SHOULD send an MSS (Maximum Segment Size) option in
			//        every SYN segment when its receive MSS differs from the
			//        default 536, and MAY send it always."
			//       "If an MSS option is not received at connection setup, TCP
			//        MUST assume a default send MSS of 536 (576-40)."
			hdr.th_flags |= TH_ACK;
			hdr.th_ack = htobe32(recv_nxt);
		}
		else
			hdr.th_ack = htobe32(0);
		hdr.th_win = htobe16(recv_wnd);
		hdr.th_urp = htobe16(0);
		hdr.th_sum = htobe16(0);
		tcp_seq window_data = (tcp_seq)(send_nxt - send_pos);
		if ( send_pos == send_una && outgoing_syn == TCP_SPECIAL_WINDOW )
			window_data--;
		if ( mod32_lt(send_pos, send_nxt) &&
		     outgoing_fin == TCP_SPECIAL_WINDOW )
			window_data--;
		if ( window_data )
		{
			size_t amount = mtu < window_data ? mtu : window_data;
			assert(outgoing_offset <= sizeof(outgoing));
			tcp_seq window_length = (tcp_seq) (send_nxtpos - send_una);
			if ( outgoing_syn == TCP_SPECIAL_WINDOW )
				window_length--;
			assert(window_length <= sizeof(outgoing));
			size_t outgoing_end = outgoing_offset + window_length;
			if ( sizeof(outgoing) <= outgoing_end )
				outgoing_end -= sizeof(outgoing);
			assert(outgoing_end < sizeof(outgoing));
			size_t until_end = sizeof(outgoing) - outgoing_end;
			size_t first = until_end < amount ? until_end : amount;
			assert(first <= sizeof(outgoing));
			assert(first <= sizeof(outgoing) - outgoing_end);
			size_t second = amount - first;
			assert(second <= sizeof(outgoing));
			memcpy(out + sizeof(hdr), outgoing + outgoing_end, first);
			if ( second )
				memcpy(out + sizeof(hdr) + first, outgoing, second);
			pkt->length += amount;
			send_nxtpos += amount;
		}
		assert(mod32_le(send_nxtpos, send_nxt));
		if ( outgoing_fin == TCP_SPECIAL_WINDOW &&
		     send_nxtpos + 1 == send_nxt )
		{
			hdr.th_flags |= TH_FIN;
			send_nxtpos++;
		}
		assert(mod32_le(send_nxtpos, send_nxt));
		memcpy(out, &hdr, sizeof(hdr));
		uint16_t checksum = 0;
		if ( af == AF_INET )
		{
			checksum = IPv4::ipsum_buf(checksum, &sendfrom.in.sin_addr,
			                           sizeof(struct in_addr));
			checksum = IPv4::ipsum_buf(checksum, &remote.in.sin_addr,
			                           sizeof(struct in_addr));
		}
		else if ( af == AF_INET6 )
		{
			checksum = IPv4::ipsum_buf(checksum, &sendfrom.in6.sin6_addr,
			                           sizeof(struct in6_addr));
			checksum = IPv4::ipsum_buf(checksum, &remote.in6.sin6_addr,
			                           sizeof(struct in6_addr));
		}
		else
			return errno = EAFNOSUPPORT, false;
		checksum = IPv4::ipsum_word(checksum, IPPROTO_TCP);
		checksum = IPv4::ipsum_word(checksum, pkt->length);
		checksum = IPv4::ipsum_buf(checksum, out, pkt->length);
		hdr.th_sum = htobe16(IPv4::ipsum_finish(checksum));
		memcpy(out, &hdr, sizeof(hdr));
		if ( af == AF_INET )
		{
			if ( !IPv4::Send(pkt, &sendfrom.in.sin_addr, &remote.in.sin_addr,
				             IPPROTO_TCP, ifindex, false) )
				return false;
		}
		// TODO: IPv6 support.
		else
			return errno = EAFNOSUPPORT, false;
		if ( has_syn )
			recv_acked = recv_nxt;
		recv_wndlast = recv_wnd;
		assert(mod32_le(send_nxtpos, send_nxt));
		send_pos = send_nxtpos;
	}
	return true;
}

void TCPSocket::TransmitWork()
{
	ScopedLock lock(&tcp_lock);
	transmit_scheduled = false;
	TransmitLoop();
	if ( can_destroy() )
		delete this;
}

void TCPSocket::OnTimer()
{
	ScopedLock lock(&tcp_lock);
	timer_armed = false;
	if ( mod32_lt(send_una, send_pos) )
	{
		retransmissions++;
		send_pos = send_una;
		TransmitLoop();
	}
	if ( can_destroy() )
		delete this;
}

static void TCPSocket__Transmit(void* ctx)
{
	((TCPSocket*) ctx)->TransmitWork();
}

void TCPSocket::ScheduleTransmit() // tcp_lock locked
{
	if ( transmit_scheduled )
		return;
	transmit_scheduled = true;
	transmit_do_schedule_worker = true;
}

void TCPSocket::DoScheduleTransmit() // tcp_lock unlocked
{
	// TODO: Merge with timer instead of worker thread as that will never block.
	Worker::Schedule(TCPSocket__Transmit, this);
}

void TCPSocket::SetTimer() // tcp_lock locked
{
	if ( timer_armed )
	{
		if ( !timer.TryCancel() )
			return;
		timer_armed = false;
	}
	bool destruction_is_wanted = want_destruction();
	if ( destruction_is_wanted && transmit_scheduled )
		return;
	if ( mod32_le(send_una, send_pos) || destruction_is_wanted )
	{
		struct itimerspec timeout;
		memset(&timeout, 0, sizeof(timeout));
		// TODO: IMPLEMENTATION DETAILS the timeout algorithm.
		// TODO: BUGS measure latency and adapt to it for first retransmission.
		if ( !destruction_is_wanted )
			timeout.it_value.tv_sec = 1 + 1 * retransmissions;
		// TODO: Review timer lifetime.
		timer.Set(&timeout, NULL, TIMER_FUNC_MAY_DEALLOCATE_TIMER,
		          TCPSocket__OnTimer, this);
		timer_armed = true;
	}
}

void TCPSocket::ProcessPacket(Ref<Packet> pkt,
                              union tcp_sockaddr* pkt_src,
                              union tcp_sockaddr* pkt_dst) // tcp_lock locked
{
	const unsigned char* in = pkt->from + pkt->offset;
	size_t inlen = pkt->length - pkt->offset;
	struct tcphdr hdr;
	memcpy(&hdr, in, sizeof(hdr));
	hdr.th_sport = be16toh(hdr.th_sport);
	hdr.th_dport = be16toh(hdr.th_dport);
	hdr.th_seq = be32toh(hdr.th_seq);
	hdr.th_ack = be32toh(hdr.th_ack);
	hdr.th_win = be16toh(hdr.th_win);
	hdr.th_urp = be16toh(hdr.th_urp);
	in += sizeof(hdr);
	inlen -= sizeof(hdr);
	if ( state == TCP_STATE_CLOSED )
	{
		if ( hdr.th_flags & TH_RST )
			return;
		// TODO: ACK the RST.
		// TODO: Also do this if there isn't any bound socket.
		return;
	}
	else if ( state == TCP_STATE_LISTEN )
	{
		if ( hdr.th_flags & TH_RST )
			return;
		if ( hdr.th_flags & TH_ACK )
		{
			// TODO: Send <SEQ=SEG.ACK><CTL=RST>.
			return;
		}
		if ( !(hdr.th_flags & TH_SYN) )
			return;
		if ( !hdr.th_win )
			return;
		// TODO: BUGS how this leads to denial of service.
		if ( backlog_used == backlog_max )
			return;
		// TODO: backlog_used is never incremented!
		// TODO: Use SYN cache to mitigate SYN flood attack.
		TCPSocket* socket = new TCPSocket(af);
		if ( !socket )
			return;
		assert(pkt_src);
		assert(pkt_dst);
		socket->remote = *pkt_src;
		socket->local = *pkt_dst;
		socket->remoted = true;
		socket->bound = true;
		// TODO: Do we know for sure that such a connection doesn't already
		//       exist? I don't think that's possible but a check seems prudent
		//       to avoid duplicate connections.
		if ( af == AF_INET )
		{
			uint16_t port = be16toh(socket->local.in.sin_port);
			socket->prev_socket = NULL;
			socket->next_socket = bindings_v4[port];
			if ( socket->next_socket )
				socket->next_socket->prev_socket = socket;
			bindings_v4[port] = socket;
		}
		else if ( af == AF_INET6 )
		{
			uint16_t port = be16toh(socket->local.in6.sin6_port);
			socket->prev_socket = NULL;
			socket->next_socket = bindings_v6[port];
			if ( socket->next_socket )
				socket->next_socket->prev_socket = socket;
			bindings_v6[port] = socket;
		}
		socket->iss = arc4random();
		socket->send_una = socket->iss;
		socket->send_nxt = socket->iss;
		socket->send_wnd = 1;
		socket->send_pos = socket->iss;
		socket->outgoing_syn = TCP_SPECIAL_PENDING;
		socket->recv_wnd = TCP_MAXWIN;
		socket->recv_acked = hdr.th_seq;
		socket->recv_nxt = hdr.th_seq + 1;
		socket->irs = hdr.th_seq;
		socket->has_syn = true;
		socket->state = TCP_STATE_SYN_RECV;
		socket->UpdateWindow(hdr.th_win);
		socket->connecting_parent = this;
		socket->connecting_prev = NULL;
		socket->connecting_next = connecting_half;
		if ( socket->connecting_next )
			socket->connecting_next->connecting_prev = socket;
		connecting_half = socket;
		socket->TransmitLoop();
		return;
	}
	else if ( state == TCP_STATE_SYN_SENT )
	{
		if ( hdr.th_flags & TH_ACK )
		{
			if ( mod32_le(hdr.th_ack, iss) || mod32_gt(hdr.th_ack, send_nxt) )
			{
				if ( hdr.th_flags & TH_RST )
					return;
				// TODO: Send RST.
			}
			if ( !(mod32_le(send_una, hdr.th_ack) &&
			     mod32_le(hdr.th_ack, send_nxt)) )
				return;
		}
		if ( hdr.th_flags & TH_RST )
		{
			Fail(ECONNREFUSED);
			return;
		}
		if ( !(hdr.th_flags & TH_SYN) )
			return;
		recv_acked = hdr.th_seq;
		recv_nxt = hdr.th_seq + 1;
		irs = hdr.th_seq;
		has_syn = true;
		UpdateWindow(hdr.th_win);
		// TODO: Drop packet if the packet contains data/FIN beyond the SYN?
		if ( hdr.th_flags & TH_ACK )
		{
			send_una = hdr.th_ack;
			retransmissions = 0;
			SetTimer();
			if ( mod32_le(iss, send_una) ) // TODO: Or lt?
			{
				outgoing_syn = TCP_SPECIAL_ACKED;
				state = TCP_STATE_ESTAB;
				kthread_cond_broadcast(&receive_cond); // Wake up connect.
			}
		}
		else
		{
			state = TCP_STATE_SYN_RECV;
		}
		return;
	}
	bool acceptable = false;
	if ( inlen == 0 && recv_wnd == 0 )
		acceptable = hdr.th_seq == recv_nxt;
	else if ( inlen == 0 && 0 < recv_wnd )
		acceptable = mod32_le(recv_nxt, hdr.th_seq) &&
		             mod32_lt(hdr.th_seq, recv_nxt + recv_wnd);
	else if ( 0 < inlen && 0 < recv_wnd )
	{
		tcp_seq seg_end = (tcp_seq) (hdr.th_seq + inlen - 1);
		acceptable = (mod32_le(recv_nxt, hdr.th_seq) &&
		              mod32_lt(hdr.th_seq, recv_nxt + recv_wnd)) ||
	                 (mod32_le(recv_nxt, seg_end) &&
		              mod32_lt(seg_end, recv_nxt + recv_wnd));
	}
	if ( !acceptable )
	{
		if ( hdr.th_flags & TH_RST )
			return;
		// Send <SEQ=SND.NXT><ACK=RCV.NXT><CTL=ACK>.
		recv_acked = recv_nxt - 1;
		return;
	}
	uint16_t real_seq = hdr.th_seq;
	if ( mod32_lt(hdr.th_seq, recv_nxt) && hdr.th_flags & TH_SYN )
	{
		hdr.th_flags &= ~TH_SYN;
		hdr.th_seq++;
	}
	if ( mod32_lt(hdr.th_seq, recv_nxt) )
	{
		tcp_seq skip = recv_nxt - hdr.th_seq;
		if ( inlen < skip )
			skip = inlen;
		hdr.th_seq += skip;
		in += skip;
		inlen -= skip;
	}
	if ( mod32_lt(hdr.th_seq, recv_nxt) && hdr.th_flags & TH_FIN )
	{
		hdr.th_flags &= ~TH_FIN;
		hdr.th_seq++;
	}
	if ( mod32_lt(hdr.th_seq, recv_nxt) )
		return;
	if ( mod32_gt(hdr.th_seq, recv_nxt) )
	{
		Ref<Packet> prev;
		Ref<Packet> iter = receive_queue;
		// TODO: For n packets in the worst order, this scales O(n^2).
		// TODO: This wastes a packet per byte in the worst case.
		while ( iter )
		{
			const unsigned char* iter_in = iter->from + iter->offset;
			const unsigned char* iter_in_seq =
				iter_in + offsetof(struct tcphdr, th_seq);
			tcp_seq iter_seq;
			memcpy(&iter_seq, iter_in_seq, sizeof(iter_seq));
			iter_seq = be32toh(iter_seq);
			if ( mod32_le(real_seq, iter_seq) )
				break;
			// TODO: Handle duplicate and overlapping segments.
			prev = iter;
			iter = iter->next;
		}
		if ( prev )
		{
			pkt->next = prev->next;
			prev->next = pkt;
		}
		else
		{
			pkt->next = receive_queue;
			receive_queue = pkt;
		}
		return;
	}
	// TODO: Potentially trim the end if it exceeds the receive window.
	if ( hdr.th_flags & TH_RST )
	{
		if ( state == TCP_STATE_SYN_RECV )
		{
			// TODO: If passive open (listening), then return to the LISTEN
			//       state and return.
		}
		else if ( state == TCP_STATE_ESTAB ||
		          state == TCP_STATE_FIN_WAIT_1 ||
		          state == TCP_STATE_FIN_WAIT_2 ||
		          state == TCP_STATE_CLOSE_WAIT )
		{
		}
		else // CLOSING, LAST_ACK, TIME_WAIT.
		{
		}
		Fail(ECONNRESET);
		return;
	}
	if ( hdr.th_flags & TH_SYN )
	{
		// TODO: Send RST.
		Fail(ECONNRESET);
		return;
	}
	if ( !(hdr.th_flags & TH_ACK) )
		return;
	if ( state == TCP_STATE_SYN_RECV )
	{
		if ( mod32_le(send_una, hdr.th_ack) && mod32_le(hdr.th_ack, send_nxt) )
		{
			state = TCP_STATE_ESTAB;
			if ( connecting_parent )
			{
				if ( connecting_prev )
					connecting_prev->connecting_next = connecting_next;
				else
					connecting_parent->connecting_half = connecting_next;
				if ( connecting_next )
					connecting_next->connecting_prev = connecting_prev;
				// TODO: This inserts the connection to the front of the
				//       accept queue, rather than the end, which is unfair to
				//       connections that have been waiting longer.
				connecting_prev = NULL;
				connecting_next = connecting_parent->connecting_ready;
				if ( connecting_next )
					connecting_next->connecting_prev = this;
				connecting_parent->connecting_ready = this;
				kthread_cond_broadcast(&connecting_parent->receive_cond);
				uint16_t status = connecting_parent->PollEventStatus();
				connecting_parent->poll_channel.Signal(status);
			}
		}
		else
		{
			// TODO: Send <SEQ=SEG.ACK><CTL=RST>.
			TransmitLoop();
			return;
		}
	}
	// TODO: This is only supposed to happen if state == TCP_STATE_ESTAB (or
	//       when TCP_STATE_RECV transitions to TCP_STATE_ESTAB) but I want it
	//       in other cases as well (FIN_WAIT_1 at least). Check RFC 793.
	if ( mod32_lt(send_una, hdr.th_ack) && mod32_le(hdr.th_ack, send_nxt) )
	{
		// TODO: If in CLOSING state and FIN is not ACKed, drop segment.
		// TODO: If in LAST-ACK state only ACK of our FIN can arrive.
		// TODO: If in TIME-WAIT state, only a retransmission of the remote
		//       FIN can arrive. If so, ACK it and restart the 2 MSL timeout.
		tcp_seq old_send_una = send_una;
		tcp_seq acked = (tcp_seq) (hdr.th_ack - send_una);
		if ( outgoing_syn == TCP_SPECIAL_WINDOW && 0 < acked )
		{
			outgoing_syn = TCP_SPECIAL_ACKED;
			acked--;
			send_una++;
		}
		tcp_seq window_data = (tcp_seq)(send_nxt - send_una);
		if ( outgoing_fin == TCP_SPECIAL_WINDOW )
			window_data--;
		if ( window_data && acked )
		{
			size_t amount = window_data < acked ? window_data : acked;
			assert(outgoing_offset < sizeof(outgoing));
			outgoing_offset += amount;
			if ( sizeof(outgoing) <= outgoing_offset )
				outgoing_offset -= sizeof(outgoing);
			assert(outgoing_offset < sizeof(outgoing));
			assert(amount <= outgoing_used);
			outgoing_used -= amount;
			kthread_cond_broadcast(&transmit_cond);
			poll_channel.Signal(PollEventStatus());
			acked -= amount;
			send_una += amount;
		}
		bool fin_was_acked = false;
		if ( outgoing_fin == TCP_SPECIAL_WINDOW && 0 < acked )
		{
			outgoing_fin = TCP_SPECIAL_ACKED;
			acked--;
			send_una++;
			fin_was_acked = true;
		}
		if ( send_una != old_send_una )
		{
			// TODO: Possibly recalculate the average time to contact remote.
			retransmissions = 0;
			SetTimer();
		}
		if ( fin_was_acked )
		{
			if ( state == TCP_STATE_FIN_WAIT_1 )
			{
				state = TCP_STATE_FIN_WAIT_2;
				// TODO: I had a couple sockets leak in this state, just a quick
				//       workaround until I read up on what should be done.
				Close();
			}
			else if ( state == TCP_STATE_CLOSING )
			{
				state = TCP_STATE_CLOSING;
				// TODO: Wait 2 MSL and then close:
				Close();
				return;
			}
			else if ( state == TCP_STATE_LAST_ACK )
			{
				Close();
				return;
			}
		}
	}
	// TODO: If the ACK is a duplicate (SEG.ACK < SND.UNA), it can be
	//       ignored.  If the ACK acks something not yet sent (SEG.ACK >
	//       SND.NXT) then send an ACK, drop the segment, and return.
	if ( mod32_lt(send_wl1, hdr.th_seq) ||
	     (send_wl1 == hdr.th_seq && mod32_le(send_wl2, hdr.th_ack)) )
	{
		UpdateWindow(hdr.th_win);
		send_wl1 = hdr.th_seq;
		send_wl2 = hdr.th_ack;
	}
	if ( state == TCP_STATE_ESTAB ||
	     state == TCP_STATE_FIN_WAIT_1 ||
	     state == TCP_STATE_FIN_WAIT_2 )
	{
		assert(incoming_offset < sizeof(incoming));
		assert(incoming_used <= sizeof(incoming));
		size_t available = sizeof(incoming) - incoming_used;
		size_t amount = available < inlen ? available : inlen;
		assert(amount <= sizeof(incoming));
		assert(amount <= available);
		size_t newat = incoming_offset + incoming_used;
		if ( sizeof(incoming) <= newat )
			newat -= sizeof(incoming);
		assert(newat < sizeof(incoming));
		size_t until_end = sizeof(incoming) - newat;
		assert(until_end <= sizeof(incoming));
		size_t first = until_end < amount ? until_end : amount;
		assert(first <= amount);
		assert(first <= sizeof(incoming));
		size_t second = amount - first;
		assert(second <= amount);
		assert(second <= sizeof(incoming));
		assert(first + second == amount);
		assert(first + second <= sizeof(incoming));
		assert(first + second <= available);
		memcpy(incoming + newat, in, first);
		if ( second )
			memcpy(incoming, in + first, second);
		incoming_used += amount;
		available = sizeof(incoming) - incoming_used;
		if ( available < recv_wnd )
			recv_wnd = available;
		recv_nxt = hdr.th_seq + amount;
		if ( amount == inlen && (hdr.th_flags & TH_FIN) )
		{
			recv_nxt++;
			has_fin = true;
		}
		// TODO: Is it possible that amount == 0?
		kthread_cond_broadcast(&receive_cond);
		poll_channel.Signal(PollEventStatus());
	}
	if ( hdr.th_flags & TH_FIN )
	{
		// TODO: Are any of these states even possible here? I suppose it must
		//       be, at least for SYN-SENT, as the remote can close at any time.
		if ( state == TCP_STATE_CLOSED ||
			 state == TCP_STATE_LISTEN ||
			 state == TCP_STATE_SYN_SENT )
			return;
		// TODO: HACK: ACK the FIN. This would be done if we didn't enter CLOSED
		//       immediately, it also doesn't retransmit the ACK in case the
		//       ACK gets lost and the remote sends the FIN again.
		Transmit();
		if ( state == TCP_STATE_SYN_RECV || state == TCP_STATE_ESTAB )
		{
			state = TCP_STATE_CLOSE_WAIT;
			kthread_cond_broadcast(&receive_cond);
			poll_channel.Signal(PollEventStatus());
		}
		else if ( state == TCP_STATE_FIN_WAIT_1 )
		{
			if ( outgoing_fin == TCP_SPECIAL_ACKED )
			{
				// TODO: Is this case possible? Wouldn't we be in FIN-WAIT-2?
				state = TCP_STATE_TIME_WAIT;
				// TODO: Start the TIME-WAIT timer and turn off other timers.
				Close();
			}
			else
			{
				state = TCP_STATE_CLOSING;
				// TODO: Are we supposed to wait 2 MSL and then close?
				Close();
			}
		}
		else if ( state == TCP_STATE_FIN_WAIT_2 )
		{
			state = TCP_STATE_TIME_WAIT;
			// TODO: Start the TIME-WAIT timer and turn off other timers.
			Close();
		}
		else if ( state == TCP_STATE_TIME_WAIT )
		{
			// TODO: Restart the 2 MSL TIME-WAIT timeout.
		}
	}
}

void TCPSocket::ReceivePacket(Ref<Packet> pktnew,
                              union tcp_sockaddr* pkt_src,
                              union tcp_sockaddr* pkt_dst) // tcp_lock locked
{
	if ( pktnew )
		ProcessPacket(pktnew, pkt_src, pkt_dst);
	while ( receive_queue )
	{
		Ref<Packet> pkt = receive_queue;
		const unsigned char* in = pkt->from + pkt->offset;
		const unsigned char* in_seq = in + offsetof(struct tcphdr, th_seq);
		tcp_seq seq;
		memcpy(&seq, in_seq, sizeof(seq));
		seq = be32toh(seq);
		if ( mod32_gt(seq, recv_nxt) )
			break;
		receive_queue = pkt->next;
		if ( seq == recv_nxt )
			ProcessPacket(pkt, pkt_src, pkt_dst);
	}
	ScheduleTransmit();
}

void TCPSocket::UpdateWindow(uint16_t new_window)
{
	tcp_seq pending = (tcp_seq) (send_nxt - send_una);
	if ( new_window < pending )
		send_nxt = (tcp_seq) (send_una + pending);
	send_wnd = new_window;
}

int TCPSocket::connect(ioctx_t* ctx, const uint8_t* addr, size_t addrsize)
{
	ScopedLock lock(&tcp_lock);
	// TODO: os-test listen + connect, what errno?
	if ( state == TCP_STATE_SYN_SENT || state == TCP_STATE_SYN_RECV )
		return errno = EALREADY, -1;
	if ( state != TCP_STATE_CLOSED )
		return errno = EISCONN, -1; // TODO: Another errno if listening?
	union tcp_sockaddr new_remote;
	if ( !ImportAddress(ctx, &new_remote, addr, addrsize) )
		return -1;
	if ( af == AF_INET )
	{
		// Verify the port is non-zero.
		if ( be16toh(new_remote.in.sin_port) == 0 )
			return errno = EADDRNOTAVAIL, -1;
	}
	else
		return errno = EAFNOSUPPORT, -1;
	// TODO: os-test AF_UNSPEC
	// If the socket is not bound, find a route to the remote address and bind
	// to the appropriate source address.
	if ( !bound )
	{
		union tcp_sockaddr new_local;
		memset(&new_local, 0, sizeof(new_local));
		if ( af == AF_INET )
		{
			struct in_addr any;
			any.s_addr = htobe32(INADDR_ANY);
			new_local.in.sin_family = AF_INET;
			if ( !IPv4::GetSourceIP(&any, &new_remote.in.sin_addr,
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
	// TODO: Does TCP also do this? Note that connecting to the any address
	//       should be forbidden, right?
	if ( af == AF_INET )
	{
		if ( !IPv4::GetSourceIP(&local.in.sin_addr, &new_remote.in.sin_addr,
		                        NULL, ifindex, NULL) )
			return -1;
	}
	else
		return errno = EAFNOSUPPORT, -1;
	memcpy(&remote, &new_remote, sizeof(new_remote));
	remoted = true;
	iss = arc4random();
	//recv_wnd = UINT16_MAX; // TODO?
	recv_wnd = TCP_MAXWIN;
	send_una = iss;
	send_nxt = iss;
	send_wnd = 1;
	send_pos = iss;
	outgoing_syn = TCP_SPECIAL_PENDING;
	state = TCP_STATE_SYN_SENT;
	TransmitLoop();
	while ( !sockerr &&
	        (state == TCP_STATE_SYN_SENT || state == TCP_STATE_SYN_RECV) )
	{
		// TODO: os-test non-blocking connect.
		if ( ctx->dflags & O_NONBLOCK )
			return errno = EINPROGRESS, -1;
		if ( !kthread_cond_wait_signal(&receive_cond, &tcp_lock) )
			return errno = EINTR, -1;
	}
	if ( sockerr )
	{
		// TODO: This is not recoverable. Is that correct?
		// TODO: os-test whether reconnect is possible after failed connect?
		return errno = sockerr, -1;
	}
	return 0;
}

int TCPSocket::listen(ioctx_t* /*ctx*/, int backlog)
{
	if ( backlog < 0 )
		return errno = EINVAL, -1;
	if ( backlog == 0 || SOMAXCONN < backlog )
		backlog = SOMAXCONN;
	ScopedLock lock(&tcp_lock);
	if ( !bound )
		return errno = EDESTADDRREQ, -1;
	// TODO: Does this allow listening on a closed normal socket?
	// TODO: os-test a regular connection, close, and then try to listen.
	if ( state != TCP_STATE_CLOSED )
		return errno = EINVAL, -1;
	backlog_used = 0;
	backlog_max = backlog;
	memset(&remote, 0, sizeof(remote));
	if ( af == AF_INET )
	{
		remote.in.sin_family = AF_INET;
		remote.in.sin_addr.s_addr = htobe32(INADDR_ANY);
	}
	else if ( af == AF_INET6 )
	{
		remote.in6.sin6_family = AF_INET6;
		remote.in6.sin6_addr = in6addr_any;
	}
	else
		return errno = EAFNOSUPPORT, -1;
	remoted = true;
	state = TCP_STATE_LISTEN;
	return 0;
}

ssize_t TCPSocket::recv(ioctx_t* ctx,
                        uint8_t* buf,
                        size_t count,
                        int flags)
{
	if ( flags & ~(MSG_PEEK | MSG_WAITALL) ) // TODO: MSG_OOB.
		return errno = EINVAL, -1;
	kthread_mutex_lock(&tcp_lock);
	ssize_t result = recv_unlocked(ctx, buf, count, flags);
	bool do_schedule_worker = transmit_do_schedule_worker;
	transmit_do_schedule_worker = false;
	kthread_mutex_unlock(&tcp_lock);
	if ( do_schedule_worker )
		DoScheduleTransmit();
	return result;
}

ssize_t TCPSocket::recvmsg(ioctx_t* ctx, struct msghdr* msg_ptr, int flags)
{
	struct msghdr msg;
	if ( !ctx->copy_from_src(&msg, msg_ptr, sizeof(msg)) )
		return -1;
	if ( msg.msg_iovlen < 0 || IOV_MAX < msg.msg_iovlen )
		return errno = EINVAL, -1;
	// TODO: Reject if non-null msg_name, msg_control?
	size_t iov_size = msg.msg_iovlen * sizeof(struct iovec);
	struct iovec* iov = new struct iovec[msg.msg_iovlen];
	if ( !iov )
		return -1;
	if ( !ctx->copy_from_src(iov, msg.msg_iov, iov_size) )
		return delete[] iov, -1;
	msg.msg_iov = iov;
	kthread_mutex_lock(&tcp_lock);
	ssize_t result = 0;
	for ( int i = 0; i < msg.msg_iovlen && result < SSIZE_MAX; i++ )
	{
		size_t maximum = SSIZE_MAX - (size_t) result;
		uint8_t* buf = (uint8_t*) iov[i].iov_base;
		size_t count = iov[i].iov_len < maximum ? iov[i].iov_len : maximum;
		// TODO: What about an empty iov_len vs. EOF?
		ssize_t amount = recv_unlocked(ctx, buf, count, flags);
		if ( amount < 0 )
		{
			if ( result == 0 )
				result = -1;
			break;
		}
		result += amount;
		if ( (size_t) amount != count )
			break;
	}
	bool do_schedule_worker = transmit_do_schedule_worker;
	transmit_do_schedule_worker = false;
	kthread_mutex_unlock(&tcp_lock);
	if ( do_schedule_worker )
		DoScheduleTransmit();
	delete[] iov;
	if ( !ctx->copy_to_dest(msg_ptr, &msg, sizeof(msg)) )
		return -1;
	return result;
}

ssize_t TCPSocket::recv_unlocked(ioctx_t* ctx,
                                 uint8_t* buf,
                                 size_t count,
                                 int flags)
{
	if ( flags & ~(MSG_PEEK | MSG_WAITALL) ) // TODO: MSG_OOB.
		return errno = EINVAL, -1;
	if ( sockerr )
		return errno = sockerr, -1;
	// TODO: os-test non-blocking connect + immediate recv.
	// TODO: Proper state check.
	if ( state != TCP_STATE_ESTAB &&
	     state != TCP_STATE_CLOSE_WAIT &&
	     state != TCP_STATE_FIN_WAIT_1 &&
	     state != TCP_STATE_FIN_WAIT_2)
		return errno = ENOTCONN, -1;
	size_t sofar = 0;
	while ( sofar < count )
	{
		while ( !(incoming_used || has_fin) )
		{
			// TODO: Would has_fin be true in TCP_STATE_CLOSE_WAIT?
			if ( state == TCP_STATE_CLOSE_WAIT )
				return sofar;
			if ( sofar && !(flags & MSG_WAITALL) )
				return sofar;
			// TODO: Is this the right precedence for errors?
			if ( sockerr )
				return sofar ? sofar : (errno = sockerr, -1);
			if ( ctx->dflags & O_NONBLOCK )
				return sofar ? sofar : (errno = EWOULDBLOCK, -1);
			if ( !kthread_cond_wait_signal(&receive_cond, &tcp_lock) )
				return sofar ? sofar : (errno = EINTR, -1);
			if ( sockerr )
				return sofar ? sofar : (errno = sockerr, -1);
		}
		if ( incoming_used == 0 && has_fin )
			return sofar;
		uint8_t* data = buf + sofar;
		size_t left = count - sofar;
		assert(incoming_used <= sizeof(incoming));
		size_t amount = incoming_used < left ? incoming_used : left;
		assert(incoming_offset < sizeof(incoming));
		size_t until_end = sizeof(incoming) - incoming_offset;
		size_t first = until_end < amount ? until_end : amount;
		size_t second = amount - first;
		if ( !ctx->copy_to_dest(data, incoming + incoming_offset, first) )
			return sofar ? sofar : -1;
		if ( second && !ctx->copy_to_dest(data + first, incoming, second) )
			return sofar ? sofar : -1;
		sofar += amount;
		if ( flags & MSG_PEEK )
			return sofar;
		incoming_offset += amount;
		if ( sizeof(incoming) <= incoming_offset )
			incoming_offset -= sizeof(incoming);
		assert(incoming_offset < sizeof(incoming));
		incoming_used -= amount;
		size_t window_possible = sizeof(incoming) - incoming_used;
		if ( UINT16_MAX < window_possible )
			window_possible = UINT16_MAX;
		if ( TCP_MAXWIN < window_possible )
			window_possible = TCP_MAXWIN;
		// TODO: Should this be done outside of the established state?
		if ( !sockerr && recv_wnd != window_possible )
		{
			recv_wnd = window_possible;
			ScheduleTransmit();
		}
	}
	return sofar;
}

ssize_t TCPSocket::send(ioctx_t* ctx,
                        const uint8_t* buf,
                        size_t count,
                        int flags)
{
	// TODO: MSG_MORE (and implement TCP_CORK).
	if ( flags & ~(MSG_NOSIGNAL) ) // TODO: MSG_OOB, MSG_DONTROUTE.
		return errno = EINVAL, -1;
	kthread_mutex_lock(&tcp_lock);
	ssize_t result = send_unlocked(ctx, buf, count, flags);
	bool do_schedule_worker = transmit_do_schedule_worker;
	transmit_do_schedule_worker = false;
	kthread_mutex_unlock(&tcp_lock);
	if ( do_schedule_worker )
		DoScheduleTransmit();
	return result;
}

ssize_t TCPSocket::sendmsg(ioctx_t* ctx,
                           const struct msghdr* msg_ptr,
                           int flags)
{
	// TODO: MSG_MORE (and implement TCP_CORK).
	if ( flags & ~(MSG_NOSIGNAL) ) // TODO: MSG_OOB, MSG_DONTROUTE.
		return errno = EINVAL, -1;
	struct msghdr msg;
	if ( !ctx->copy_from_src(&msg, msg_ptr, sizeof(msg)) )
		return -1;
	if ( msg.msg_iovlen < 0 || IOV_MAX < msg.msg_iovlen )
		return errno = EINVAL, -1;
	// TODO: Reject if non-null msg_name, msg_control?
	size_t iov_size = msg.msg_iovlen * sizeof(struct iovec);
	struct iovec* iov = new struct iovec[msg.msg_iovlen];
	if ( !iov )
		return -1;
	if ( !ctx->copy_from_src(iov, msg.msg_iov, iov_size) )
		return delete[] iov, -1;
	msg.msg_iov = iov;
	kthread_mutex_lock(&tcp_lock);
	ssize_t result = 0;
	for ( int i = 0; i < msg.msg_iovlen && result < SSIZE_MAX; i++ )
	{
		size_t maximum = SSIZE_MAX - (size_t) result;
		const uint8_t* buf = (const uint8_t*) iov[i].iov_base;
		size_t count = iov[i].iov_len < maximum ? iov[i].iov_len : maximum;
		ssize_t amount = send_unlocked(ctx, buf, count, flags);
		if ( amount < 0 )
		{
			if ( result == 0 )
				result = -1;
			break;
		}
		result += amount;
		if ( (size_t) amount != count )
			break;
	}
	bool do_schedule_worker = transmit_do_schedule_worker;
	transmit_do_schedule_worker = false;
	kthread_mutex_unlock(&tcp_lock);
	if ( do_schedule_worker )
		DoScheduleTransmit();
	delete[] iov;
	return result;
}

ssize_t TCPSocket::send_unlocked(ioctx_t* ctx,
                                 const uint8_t* buf,
                                 size_t count,
                                 int flags)
{
	// TODO: Implement SIGPIPE/EPIPE if !MSG_NOSIGNAL.
	// TODO: os-test SIGPIPE/EPIPE if !MSG_NOSIGNAL.
	(void) flags;
	if ( sockerr )
		return errno = sockerr, -1;
	// TODO: Proper state check.
	// TODO: FIN-WAIT-1, FIN-WAIT-2, CLOSING, LAST-ACK, TIME-WAIT.
	if ( state != TCP_STATE_ESTAB &&
	     state != TCP_STATE_CLOSE_WAIT )
		return errno = ENOTCONN, -1;
	size_t sofar = 0;
	while ( sofar < count )
	{
		// TODO: Also wake on connection termination.
		while ( outgoing_used == sizeof(outgoing) )
		{
			if ( sofar )
				return sofar;
			if ( sockerr )
				return errno = sockerr, -1;
			if ( ctx->dflags & O_NONBLOCK )
				return errno = EWOULDBLOCK;
			if ( !kthread_cond_wait_signal(&transmit_cond, &tcp_lock) )
				return errno = EINTR, -1;
		}
		const uint8_t* data = buf + sofar;
		size_t left = count - sofar;
		assert(outgoing_offset < sizeof(outgoing));
		assert(outgoing_used <= sizeof(outgoing));
		size_t available = sizeof(outgoing) - outgoing_used;
		size_t amount = available < left ? available : left;
		size_t newat = outgoing_offset + outgoing_used;
		if ( sizeof(outgoing) <= newat )
			newat -= sizeof(outgoing);
		assert(newat < sizeof(outgoing));
		size_t until_end = sizeof(outgoing) - newat;
		size_t first = until_end < amount ? until_end : amount;
		size_t second = amount - first;
		if ( !ctx->copy_from_src(outgoing + newat, data, first) )
			return sofar ? sofar : -1;
		if ( second && !ctx->copy_from_src(outgoing, data + first, second) )
			return sofar ? sofar : -1;
		outgoing_used += amount;
		assert(outgoing_used <= sizeof(outgoing));
		sofar += amount;
		// TODO: If there's a sent packet that hasn't been acknowledged, and
		//       there isn't a full packet yet, then just buffer and don't
		//       transmit yet.
		// TODO: TCP_NODELAY, TCP_NOPUSH, MSG_MORE.
		// TODO: Set PUSH appropriately.
		ScheduleTransmit();
	}
	return sofar;
}

ssize_t TCPSocket::read(ioctx_t* ctx, uint8_t* buf, size_t count)
{
	return recv(ctx, buf, count, 0);
}

ssize_t TCPSocket::write(ioctx_t* ctx, const uint8_t* buf, size_t count)
{
	return send(ctx, buf, count, 0);
}

short TCPSocket::PollEventStatus()
{
	// TODO: os-test the poll bits.
	// TODO: OOB poll bits.
	short status = 0;
	if ( connecting_ready )
		status |= POLLIN | POLLRDNORM;
	if ( incoming_used )
		status |= POLLIN | POLLRDNORM;
	if ( (state == TCP_STATE_ESTAB || state == TCP_STATE_CLOSE_WAIT) &&
	     outgoing_used < sizeof(outgoing) )
		status |= POLLOUT | POLLWRNORM;
	if ( state == TCP_STATE_CLOSE_WAIT ||
	     state == TCP_STATE_LAST_ACK ||
	     state == TCP_STATE_TIME_WAIT ||
	     state == TCP_STATE_CLOSED )
		status |= POLLHUP;
	if ( sockerr )
		status |= POLLERR;
	return status;
}

int TCPSocket::poll(ioctx_t* /*ctx*/, PollNode* node)
{
	ScopedLock lock(&tcp_lock);
	short ret_status = PollEventStatus() & node->events;
	if ( ret_status )
	{
		node->master->revents |= ret_status;
		return 0;
	}
	poll_channel.Register(node);
	return errno = EAGAIN, -1;
}

int TCPSocket::getsockopt(ioctx_t* ctx, int level, int option_name,
                          void* option_value, size_t* option_size_ptr)
{
	ScopedLock lock(&tcp_lock);

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
			return errno = ERANGE;
		if ( !CopyToUser(option_value, ifname, size) ||
			 !CopyToUser(option_size_ptr, &size, sizeof(size)) )
			return -1;
		return 0;
	}

	uintmax_t result = 0;

	if ( level == IPPROTO_TCP )
	{
		switch ( option_name )
		{
		//case TCP_NODELAY: break; // TODO: Transmit if turned on?
		//case TCP_MAXSEG: break; // TODO: Do I want this initially?
		//case TCP_NOPUSH: break; // TODO: Do I want this initially?
		// TODO: TCP_CORK?
		default: return errno = ENOPROTOOPT, -1;
		}
	}
	else if ( level == SOL_SOCKET )
	{
		switch ( option_name )
		{
		case SO_BINDTOINDEX: result = ifindex; break;
		case SO_DEBUG: result = 0; break;
		case SO_DOMAIN: result = af; break;
		case SO_ERROR: result = sockerr; break;
		case SO_PROTOCOL: result = IPPROTO_TCP; break;
		case SO_RCVBUF: result = sizeof(incoming); break;
		case SO_REUSEADDR: result = reuseaddr; break;
		case SO_SNDBUF: result = sizeof(outgoing); break;
		case SO_TYPE: result = SOCK_STREAM; break;
		// TODO: And document these:
		// TODO: SO_ACCEPTCONN
		// TODO: SO_LINGER
		// TODO: SO_OOBINLINE
		// TODO: SO_RCVLOWAT
		// TODO: SO_RCVTIMEO
		// TODO: SO_SNDLOWAT
		// TODO: SO_SNDTIMEO
		// TODO: SO_DONTROUTE
		// TODO: SO_BROADCAST
		default: return errno = ENOPROTOOPT, -1;
		}
	}
	else
		return errno = EINVAL, -1;

	if ( !sockopt_return_uintmax(result, ctx, option_value, option_size_ptr) )
		return -1;

	return 0;
}

// TODO: os-test socket options on shut down sockets. POSIX says EINVAL.
// TODO: os-test the errno for an invalid protocol.
// TODO: os-test the errno for an invalid option at a protocol level.

int TCPSocket::setsockopt(ioctx_t* ctx, int level, int option_name,
                          const void* option_value, size_t option_size)
{
	ScopedLock lock(&tcp_lock);

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

	if ( level == IPPROTO_TCP )
	{
		switch ( option_name )
		{
		case TCP_NODELAY: break; // TODO: Transmit if turned on?
		case TCP_MAXSEG: break; // TODO: Do I want this initially?
		case TCP_NOPUSH: break; // TODO: Do I want this initially?
		// TODO: TCP_CORK?
		default:
			return errno = ENOPROTOOPT, -1;
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
		case SO_DEBUG:
			if ( value != 0 )
				return errno = EPERM, -1;
			break;
		case SO_KEEPALIVE: break; // TODO: Implement this.
		case SO_REUSEADDR: reuseaddr = value; break;
		case SO_LINGER: break; // TODO: Implement this.
		case SO_RCVBUF: break; // TODO: Implement this.
		case SO_SNDBUF: break; // TODO: Implement this.
		// TODO: And document these:
		// TODO: SO_BROADCAST
		// TODO: SO_DONTROUTE
		// TODO: SO_LINGER
		// TODO: SO_RCVLOWAT
		// TODO: SO_RCVTIMEO
		// TODO: SO_SNDLOWAT
		// TODO: SO_SNDTIMEO
		default: return errno = ENOPROTOOPT, -1;
		}
	}
	else
		return errno = EINVAL, -1;

	return 0;
}

int TCPSocket::shutdown(ioctx_t* /*ctx*/, int how)
{
	kthread_mutex_lock(&tcp_lock);
	int result = shutdown_unlocked(how);
	bool do_schedule_worker = transmit_do_schedule_worker;
	transmit_do_schedule_worker = false;
	kthread_mutex_unlock(&tcp_lock);
	if ( do_schedule_worker )
		DoScheduleTransmit();
	return result;
}

int TCPSocket::shutdown_unlocked(int how) // tcp_lock taken
{
	// TODO: Proper state check.
	if ( state != TCP_STATE_ESTAB && state != TCP_STATE_CLOSE_WAIT )
		return errno = ENOTCONN, -1;
	// TODO: I don't think this this leads to deletion in every state.
	// TODO: SHUT_RD.
	if ( how & SHUT_WR && outgoing_fin == TCP_SPECIAL_NOT )
	{
		if ( state == TCP_STATE_ESTAB || state == TCP_STATE_CLOSE_WAIT )
		{
			outgoing_fin = TCP_SPECIAL_PENDING;
			if ( state == TCP_STATE_ESTAB )
				state = TCP_STATE_FIN_WAIT_1;
			else
				state = TCP_STATE_LAST_ACK;
			ScheduleTransmit();
		}
		else
		{
			// TODO: Other states.
		}
	}
	return 0;
}

int TCPSocket::getpeername(ioctx_t* ctx, uint8_t* addr, size_t* addrsize_ptr)
{
	ScopedLock lock(&tcp_lock);
	if ( !remoted || state == TCP_STATE_LISTEN )
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

int TCPSocket::getsockname(ioctx_t* ctx, uint8_t* addr, size_t* addrsize_ptr)
{
	ScopedLock lock(&tcp_lock);
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

// TODO: os-test fstat on a socket.
TCPSocketNode::TCPSocketNode(TCPSocket* socket)
{
	this->socket = socket;
	socket->is_referenced = true;
	Process* process = CurrentProcess();
	inode_type = INODE_TYPE_STREAM;
	dev = (dev_t) this;
	ino = (ino_t) this;
	type = S_IFSOCK;
	kthread_mutex_lock(&process->idlock);
	stat_uid = process->uid;
	stat_gid = process->gid;
	kthread_mutex_unlock(&process->idlock);
	stat_mode = 0600 | this->type;
}

TCPSocketNode::~TCPSocketNode()
{
	socket->Unreference();
}

Ref<Inode> TCPSocketNode::accept4(ioctx_t* ctx, uint8_t* addr, size_t* addrsize,
                                  int flags)
{
	return socket->accept4(ctx, addr, addrsize, flags);
}

int TCPSocketNode::bind(ioctx_t* ctx, const uint8_t* addr, size_t addrsize)
{
	return socket->bind(ctx, addr, addrsize);
}

int TCPSocketNode::connect(ioctx_t* ctx, const uint8_t* addr, size_t addrsize)
{
	return socket->connect(ctx, addr, addrsize);
}

int TCPSocketNode::listen(ioctx_t* ctx, int backlog)
{
	return socket->listen(ctx, backlog);
}

ssize_t TCPSocketNode::recv(ioctx_t* ctx, uint8_t* buf, size_t count, int flags)
{
	return socket->recv(ctx, buf, count, flags);
}

ssize_t TCPSocketNode::recvmsg(ioctx_t* ctx, struct msghdr* msg, int flags)
{
	return socket->recvmsg(ctx, msg, flags);
}

ssize_t TCPSocketNode::send(ioctx_t* ctx, const uint8_t* buf, size_t count,
                            int flags)
{
	return socket->send(ctx, buf, count, flags);
}

ssize_t TCPSocketNode::sendmsg(ioctx_t* ctx, const struct msghdr* msg,
                               int flags)
{
	return socket->sendmsg(ctx, msg, flags);
}

ssize_t TCPSocketNode::read(ioctx_t* ctx, uint8_t* buf, size_t count)
{
	return socket->read(ctx, buf, count);
}

ssize_t TCPSocketNode::write(ioctx_t* ctx, const uint8_t* buf, size_t count)
{
	return socket->write(ctx, buf, count);
}

int TCPSocketNode::poll(ioctx_t* ctx, PollNode* node)
{
	return socket->poll(ctx, node);
}

int TCPSocketNode::getsockopt(ioctx_t* ctx, int level, int option_name,
                              void* option_value, size_t* option_size_ptr)
{
	return socket->getsockopt(ctx, level, option_name, option_value,
	                          option_size_ptr);
}

int TCPSocketNode::setsockopt(ioctx_t* ctx, int level, int option_name,
                              const void* option_value, size_t option_size)
{
	return socket->setsockopt(ctx, level, option_name, option_value,
	                          option_size);
}

int TCPSocketNode::shutdown(ioctx_t* ctx, int how)
{
	return socket->shutdown(ctx, how);
}

int TCPSocketNode::getpeername(ioctx_t* ctx, uint8_t* addr, size_t* addrsize)
{
	return socket->getpeername(ctx, addr, addrsize);
}

int TCPSocketNode::getsockname(ioctx_t* ctx, uint8_t* addr, size_t* addrsize)
{
	return socket->getsockname(ctx, addr, addrsize);
}

void HandleIPv4(Ref<Packet> pkt,
                const struct in_addr* src,
                const struct in_addr* dst,
                bool dst_broadcast)
{
	// TODO: What if src is the any address?
	if ( dst_broadcast )
		return;
	const unsigned char* in = pkt->from + pkt->offset;
	size_t inlen = pkt->length - pkt->offset;
	struct tcphdr hdr;
	if ( inlen < sizeof(hdr) )
		return;
	if ( UINT16_MAX < inlen )
		return;
	memcpy(&hdr, in, sizeof(hdr));
	hdr.th_sport = be16toh(hdr.th_sport);
	hdr.th_dport = be16toh(hdr.th_dport);
	hdr.th_sum = be16toh(hdr.th_sum);
	uint16_t sum = 0;
	sum = IPv4::ipsum_buf(sum, src, sizeof(struct in_addr));
	sum = IPv4::ipsum_buf(sum, dst, sizeof(struct in_addr));
	sum = IPv4::ipsum_word(sum, IPPROTO_TCP);
	sum = IPv4::ipsum_word(sum, inlen);
	sum = IPv4::ipsum_buf(sum, in, inlen);
	if ( sum != 0 && sum != 0xFFFF )
		return;
	if ( TCP_OFFSET_DECODE(hdr.th_offset) < sizeof(hdr) / 4 )
		return;
	// TODO: This is the only use of TCP_OFFSET_DECODE. That seems wrong.
	// TODO: Reject if src is the any address. Right? Is this the best place to
	//       handle it? What about udp and ping? They'll want to receive from
	//       any, but they don't want to respond except by broadcast, right?
	// Port 0 is not valid.
	if ( hdr.th_sport == 0 || hdr.th_dport == 0 )
		return;
	// TODO: TCP options. Respect TCPOPT_MAXSEG.
	TCPSocket* socket = NULL;
	TCPSocket* socket_listener = NULL;
	TCPSocket* any_socket = NULL;
	TCPSocket* any_socket_listener = NULL;
	ScopedLock lock(&tcp_lock);
	for ( TCPSocket* iter = bindings_v4[hdr.th_dport];
	      !socket && iter;
	      iter = iter->next_socket )
	{
		// TODO: If a TCP socket is bound, and then connected to, what happens?
		//       What if the TCP socket then connects to the other side?
		if ( !iter->remoted )
			continue;
		// The datagram was sent to the socket's local address.
		if ( !memcmp(&iter->local.in.sin_addr, dst, sizeof(*dst)) )
		{
			// The first priority is to receive on a socket with the correct
			// local address and the correct remote address.
			if ( !memcmp(&iter->remote.in.sin_addr, src, sizeof(*src)) &&
				 be16toh(iter->remote.in.sin_port) == hdr.th_sport )
				socket = iter;
			// The second priority is to receive on a socket with the correct
			// local address and listening for connections from any address.
			else if ( iter->remote.in.sin_addr.s_addr == htobe32(INADDR_ANY) )
				socket_listener = iter;
		}
		// The socket is bound to the any address.
		if ( iter->local.in.sin_addr.s_addr == htobe32(INADDR_ANY) )
		{
			// The third priority is to receive on a socket bound to the any
			// address and the correct remote address.
			// local address and correct remote address.
			// TODO: Can a socket bound to the any address be connected? Is the
			//       any_socket case possible, and desirable or harmful?
			if ( !memcmp(&iter->remote.in.sin_addr, src, sizeof(*src)) &&
				 be16toh(iter->remote.in.sin_port) == hdr.th_sport )
				any_socket = iter;
			// The fourth priority is to receive on a socket bound to the any
			// address and listening for connections from any address.
			else if ( iter->remote.in.sin_addr.s_addr == htobe32(INADDR_ANY) )
				any_socket_listener = iter;
		}
	}
	if ( !socket )
		socket = socket_listener;
	if ( !socket )
		socket = any_socket;
	if ( !socket )
		socket = any_socket_listener;
	// No socket wanted to receive the packet.
	if ( !socket )
	{
		// TODO: Send RST.
		return;
	}
	// If the socket is bound to a network interface, require the packet to
	// have been received on that network interface.
	if ( socket->ifindex && socket->ifindex != pkt->netif->ifinfo.linkid )
	{
		// TODO: Send RST.
		return;
	}
	union tcp_sockaddr pkt_src;
	pkt_src.in.sin_family = AF_INET;
	pkt_src.in.sin_addr = *src;
	pkt_src.in.sin_port = htobe16(hdr.th_sport);
	union tcp_sockaddr pkt_dst;
	pkt_dst.in.sin_family = AF_INET;
	pkt_dst.in.sin_addr = *dst;
	pkt_dst.in.sin_port = htobe16(hdr.th_dport);
	// Receive the packet on the socket.
	socket->ReceivePacket(pkt, &pkt_src, &pkt_dst);
	// Delete the socket if needed or schedule a transmit if needed.
	if ( socket->can_destroy() )
		delete socket;
	else if ( socket->transmit_do_schedule_worker )
	{
		socket->transmit_do_schedule_worker = false;
		lock.Reset();
		socket->DoScheduleTransmit();
	}
}

Ref<Inode> Socket(int af)
{
	if ( !IsSupportedAddressFamily(af) )
		return errno = EAFNOSUPPORT, Ref<Inode>(NULL);
	ScopedLock lock(&tcp_lock); // DEBUG
	TCPSocket* socket = new TCPSocket(af);
	if ( !socket )
		return Ref<Inode>();
	Ref<TCPSocketNode> result(new TCPSocketNode(socket));
	if ( !result )
		return delete socket, Ref<Inode>();
	return result;
}

// DEBUG
ssize_t Info(char* user_resp, size_t resplen)
{
	ScopedLock lock(&tcp_lock); // DEBUG
	bool exhausted = false;
	size_t total_needed = 0;
	for ( TCPSocket* socket = all_first_socket;
	      socket;
	      socket = socket->all_next_socket )
	{
		char str[256];
		size_t stringlen = socket->Describe(str, sizeof(str));
		if ( !socket->all_next_socket && stringlen )
			stringlen--;
		total_needed += stringlen;
		if ( exhausted )
			continue;
		if ( resplen < stringlen )
		{
			exhausted = true;
			continue;
		}
		if ( !CopyToUser(user_resp, str, sizeof(char) * stringlen) )
			return -1;
		user_resp += stringlen;
		resplen -= stringlen;
	}
	if ( !exhausted && !resplen )
		exhausted = true;
	if ( !exhausted )
	{
		char zero = '\0';
		if ( !CopyToUser(user_resp, &zero, 1) )
			return -1;
	}
	if ( exhausted )
		return errno = ERANGE, (ssize_t) total_needed;
	return 0;
}

} // namespace TCP
} // namespace Sortix
