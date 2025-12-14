/*
 * Copyright (c) 2013-2025 Jonas 'Sortie' Termansen.
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
 * net/fs.cpp
 * Filesystem based socket interface.
 */

#include <sys/socket.h>
#include <sys/un.h>

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sortix/fcntl.h>
#include <sortix/poll.h>
#include <sortix/socket.h>
#include <sortix/stat.h>

#include <sortix/kernel/descriptor.h>
#include <sortix/kernel/inode.h>
#include <sortix/kernel/ioctx.h>
#include <sortix/kernel/kernel.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/mtable.h>
#include <sortix/kernel/pipe.h>
#include <sortix/kernel/poll.h>
#include <sortix/kernel/process.h>
#include <sortix/kernel/refcount.h>
#include <sortix/kernel/sockopt.h>

#include "fs.h"

namespace Sortix {
namespace NetFS {

class Manager;
class StreamSocket;

class Manager : public Refcountable
{
public:
	Manager();
	virtual ~Manager();

public:
	bool Bind(StreamSocket* socket, struct sockaddr_un* addr, size_t addrsize);
	bool Listen(StreamSocket* socket);
	void Unbind(StreamSocket* socket);
	void Unlisten(StreamSocket* socket);
	Ref<StreamSocket> Accept(StreamSocket* socket, ioctx_t* ctx, uint8_t* addr,
	                         size_t* addrsize, int flags);
	int AcceptPoll(StreamSocket* socket, ioctx_t* ctx, PollNode* node);
	bool Connect(StreamSocket* socket, struct sockaddr_un* addr,
	             size_t addrsize);

private:
	StreamSocket* LookupServer(struct sockaddr_un* address);

private:
	StreamSocket* first_server;
	StreamSocket* last_server;
	kthread_mutex_t manager_lock;

};

class StreamSocket : public AbstractInode
{
public:
	StreamSocket(uid_t owner, gid_t group, mode_t mode, Ref<Manager> manager);
	virtual ~StreamSocket();
	virtual bool pass();
	virtual void unpass();
	virtual Ref<Inode> accept4(ioctx_t* ctx, uint8_t* addr, size_t* addrsize,
	                           int flags);
	virtual int bind(ioctx_t* ctx, const uint8_t* addr, size_t addrsize);
	virtual int connect(ioctx_t* ctx, const uint8_t* addr, size_t addrsize);
	virtual int listen(ioctx_t* ctx, int backlog);
	virtual ssize_t recv(ioctx_t* ctx, uint8_t* buf, size_t count, int flags);
	virtual ssize_t recvmsg(ioctx_t* ctx, struct msghdr* msg, int flags);
	virtual ssize_t send(ioctx_t* ctx, const uint8_t* buf, size_t count,
	                     int flags);
	virtual ssize_t sendmsg(ioctx_t* ctx, const struct msghdr* msg, int flags);
	virtual ssize_t read(ioctx_t* ctx, uint8_t* buf, size_t count);
	virtual ssize_t readv(ioctx_t* ctx, const struct iovec* iov, int iovcnt);
	virtual ssize_t write(ioctx_t* ctx, const uint8_t* buf, size_t count);
	virtual ssize_t writev(ioctx_t* ctx, const struct iovec* iov, int iovcnt);
	virtual int poll(ioctx_t* ctx, PollNode* node);
	virtual int getsockopt(ioctx_t* ctx, int level, int option_name,
	                       void* option_value, size_t* option_size_ptr);
	virtual int setsockopt(ioctx_t* ctx, int level, int option_name,
	                       const void* option_value, size_t option_size);
	virtual int shutdown(ioctx_t* ctx, int how);
	virtual int getpeername(ioctx_t* ctx, uint8_t* addr, size_t* addrsize);
	virtual int getsockname(ioctx_t* ctx, uint8_t* addr, size_t* addrsize);
	virtual int sockatmark(ioctx_t* ctx);

public: /* For use by Manager. */
	PollChannel accept_poll_channel;
	Ref<Manager> manager;
	Ref<Descriptor> root;
	PipeEndpoint incoming;
	PipeEndpoint outgoing;
	StreamSocket* prev_socket;
	StreamSocket* next_socket;
	StreamSocket* first_pending;
	StreamSocket* last_pending;
	struct sockaddr_un* name;
	struct sockaddr_un* peer;
	size_t name_size;
	size_t peer_size;
	int shutdown_flags;
	bool is_registered;
	bool is_listening;
	bool is_connected;
	bool is_refused;
	kthread_mutex_t socket_lock;
	kthread_cond_t pending_cond;
	kthread_cond_t accepted_cond;

};

static void QueueAppend(StreamSocket** first, StreamSocket** last,
                        StreamSocket* socket)
{
	assert(!socket->prev_socket);
	assert(!socket->next_socket);
	socket->prev_socket = *last;
	socket->next_socket = NULL;
	if ( *last )
		(*last)->next_socket = socket;
	if ( !*first )
		*first = socket;
	*last = socket;
}

static void QueueRemove(StreamSocket** first, StreamSocket** last,
                        StreamSocket* socket)
{
	if ( socket->prev_socket )
		socket->prev_socket->next_socket = socket->next_socket;
	else
		*first = socket->next_socket;
	if ( socket->next_socket )
		socket->next_socket->prev_socket = socket->prev_socket;
	else
		*last = socket->prev_socket;
}

StreamSocket::StreamSocket(uid_t owner, gid_t group, mode_t mode,
                           Ref<Manager> manager)
{
	inode_type = INODE_TYPE_STREAM;
	dev = (dev_t) manager.Get();
	ino = (ino_t) this;
	// Never allow wrapping filesystem sockets as they need to be able to
	// recognize themselves when passing filesystems, to prevent reference
	// cycle loops.
	this->type = S_IFSOCK | S_IFNEVERWRAP;
	this->stat_uid = owner;
	this->stat_gid = group;
	this->stat_mode = (mode & S_SETABLE) | this->type;
	this->prev_socket = NULL;
	this->next_socket = NULL;
	this->first_pending = NULL;
	this->last_pending = NULL;
	this->name = NULL;
	this->peer = NULL;
	this->name_size = 0;
	this->peer_size = 0;
	this->shutdown_flags = 0;
	this->is_registered = false;
	this->is_listening = false;
	this->is_connected = false;
	this->is_refused = false;
	this->manager = manager;
	this->root = CurrentProcess()->GetRoot();
	this->socket_lock = KTHREAD_MUTEX_INITIALIZER;
	this->pending_cond = KTHREAD_COND_INITIALIZER;
	this->accepted_cond = KTHREAD_COND_INITIALIZER;
	this->supports_iovec = true;
}

StreamSocket::~StreamSocket()
{
	if ( is_listening )
		manager->Unlisten(this);
	free(peer);
	if ( name )
		manager->Unbind(this);
}

bool StreamSocket::pass()
{
	if ( outgoing.pass() )
	{
		if ( incoming.pass() )
			return true;
		outgoing.unpass();
	}
	return false;
}

void StreamSocket::unpass()
{
	outgoing.unpass();
	incoming.unpass();
}

Ref<Inode> StreamSocket::accept4(ioctx_t* ctx, uint8_t* addr, size_t* addrsize,
                                 int flags)
{
	ScopedLock lock(&socket_lock);
	if ( !is_listening )
		return errno = EINVAL, Ref<Inode>(NULL);
	return manager->Accept(this, ctx, addr, addrsize, flags);
}

static struct sockaddr_un* import_addr(ioctx_t* ctx,
                                       const uint8_t* user_addr,
                                       size_t addrsize)
{
	size_t path_offset = offsetof(struct sockaddr_un, sun_path);
	if ( addrsize < path_offset )
		return errno = EINVAL, (struct sockaddr_un*) NULL;
	size_t path_len = addrsize - path_offset;
	struct sockaddr_un* addr = (struct sockaddr_un*) malloc(addrsize);
	if ( !addr )
		return NULL;
	if ( !ctx->copy_from_src(addr, user_addr, addrsize) )
		return free(addr), (struct sockaddr_un*) NULL;
	if ( 4096 < path_len )
		return errno = ENAMETOOLONG, (struct sockaddr_un*) NULL;
	if ( addr->sun_family != AF_UNIX )
		return free(addr), errno = EAFNOSUPPORT, (struct sockaddr_un*) NULL;
	const char* path = (const char*) addr + path_offset;
	bool found_nul = false;
	for ( size_t i = 0; !found_nul && i < path_len; i++ )
		if ( path[i] == '\0' )
			found_nul = true;
	if ( !found_nul )
		return free(addr), errno = EINVAL, (struct sockaddr_un*) NULL;
	return addr;
}

int StreamSocket::bind(ioctx_t* ctx, const uint8_t* user_addr, size_t addrsize)
{
	ScopedLock lock(&socket_lock);
	if ( is_connected || is_listening || name )
		return errno = EINVAL, -1;
	struct sockaddr_un* addr = import_addr(ctx, user_addr, addrsize);
	if ( !addr )
		return -1;
	if ( !manager->Bind(this, addr, addrsize) )
		return free(addr), -1;
	return 0;
}

int StreamSocket::connect(ioctx_t* ctx,
                          const uint8_t* user_addr,
                          size_t addrsize)
{
	ScopedLock lock(&socket_lock);
	if ( is_listening )
		return errno = EINVAL, -1;
	if ( is_connected )
		return errno = EISCONN, -1;
	if ( !name )
	{
		// TODO: Actually bind the socket to a unique random name.
		name = import_addr(ctx, user_addr, addrsize);
		if ( !name )
			return -1;
		name_size = addrsize;
		is_registered = false;
	}
	struct sockaddr_un* addr = import_addr(ctx, user_addr, addrsize);
	if ( !addr )
		return -1;
	if ( !manager->Connect(this, addr, addrsize) )
		return free(addr), -1;
	return 0;
}

int StreamSocket::listen(ioctx_t* /*ctx*/, int /*backlog*/)
{
	ScopedLock lock(&socket_lock);
	if ( is_connected || is_listening || !name )
		return errno = EINVAL, -1;
	if ( !manager->Listen(this) )
		return -1;
	return 0;
}

ssize_t StreamSocket::recv(ioctx_t* ctx, uint8_t* buf, size_t count,
                           int flags)
{
	ScopedLock lock(&socket_lock);
	if ( !is_connected )
		return errno = ENOTCONN, -1;
	return incoming.recv(ctx, buf, count, flags);
}

ssize_t StreamSocket::recvmsg(ioctx_t* ctx, struct msghdr* msg, int flags)
{
	ScopedLock lock(&socket_lock);
	if ( !is_connected )
		return errno = ENOTCONN, -1;
	return incoming.recvmsg(ctx, msg, flags);
}

ssize_t StreamSocket::send(ioctx_t* ctx, const uint8_t* buf, size_t count,
                           int flags)
{
	ScopedLock lock(&socket_lock);
	if ( !is_connected )
		return errno = ENOTCONN, -1;
	return outgoing.send(ctx, buf, count, flags);
}

ssize_t StreamSocket::sendmsg(ioctx_t* ctx, const struct msghdr* msg, int flags)
{
	ScopedLock lock(&socket_lock);
	if ( !is_connected )
		return errno = ENOTCONN, -1;
	return outgoing.sendmsg(ctx, msg, flags);
}

ssize_t StreamSocket::read(ioctx_t* ctx, uint8_t* buf, size_t count)
{
	return recv(ctx, buf, count, 0);
}

ssize_t StreamSocket::readv(ioctx_t* ctx, const struct iovec* iov, int iovcnt)
{
	ScopedLock lock(&socket_lock);
	if ( !is_connected )
		return errno = ENOTCONN, -1;
	return incoming.readv(ctx, iov, iovcnt);
}

ssize_t StreamSocket::write(ioctx_t* ctx, const uint8_t* buf, size_t count)
{
	return send(ctx, buf, count, 0);
}

ssize_t StreamSocket::writev(ioctx_t* ctx, const struct iovec* iov, int iovcnt)
{
	ScopedLock lock(&socket_lock);
	if ( !is_connected )
		return errno = ENOTCONN, -1;
	return outgoing.writev(ctx, iov, iovcnt);
}

int StreamSocket::poll(ioctx_t* ctx, PollNode* node)
{
	if ( is_connected )
	{
		PollNode* slave = node->CreateSlave();
		if ( !slave )
			return -1;
		int incoming_result = incoming.poll(ctx, node);
		int outgoing_result = outgoing.poll(ctx, slave);
		return incoming_result == 0 || outgoing_result == 0 ? 0 : -1;
	}
	if ( is_listening )
		return manager->AcceptPoll(this, ctx, node);
	return errno = ENOTCONN, -1;
}

int StreamSocket::getsockopt(ioctx_t* ctx, int level, int option_name,
	                         void* option_value, size_t* option_size_ptr)
{
	if ( level != SOL_SOCKET )
		return errno = EINVAL, -1;

	uintmax_t result = 0;
	switch ( option_name )
	{
	case SO_RCVBUF: result = incoming.Size(); break;
	case SO_SNDBUF: result = outgoing.Size(); break;
	case SO_ERROR: result = 0; break;
	default: return errno = ENOPROTOOPT, -1; break;
	}

	if ( !sockopt_return_uintmax(result, ctx, option_value, option_size_ptr) )
		return -1;

	return 0;
}

int StreamSocket::setsockopt(ioctx_t* ctx, int level, int option_name,
	                         const void* option_value, size_t option_size)
{
	if ( level != SOL_SOCKET )
		return errno = EINVAL, -1;

	uintmax_t value;
	if ( !sockopt_fetch_uintmax(&value, ctx, option_value, option_size) )
		return -1;

	switch ( option_name )
	{
	case SO_RCVBUF:
		if ( SIZE_MAX < value )
			return errno = EINVAL, -1;
		if ( !incoming.Resize((size_t) value) )
			return -1;
		break;
	case SO_SNDBUF:
		if ( SIZE_MAX < value )
			return errno = EINVAL, -1;
		if ( !outgoing.Resize((size_t) value) )
			return -1;
		break;
	default:
		return errno = ENOPROTOOPT, -1;
	}

	return 0;
}

int StreamSocket::shutdown(ioctx_t* /*ctx*/, int how)
{
	ScopedLock lock(&socket_lock);
	if ( how & SHUT_RD )
		incoming.Disconnect();
	if ( how & SHUT_WR )
		outgoing.Disconnect();
	shutdown_flags |= how;
	return 0;
}

int StreamSocket::getpeername(ioctx_t* ctx, uint8_t* addr, size_t* addrsize)
{
	ScopedLock lock(&socket_lock);
	if ( !is_connected )
		return errno = ENOTCONN, -1;
	if ( shutdown_flags & SHUT_WR )
		return errno = EINVAL, -1;
	size_t used_addrsize;
	if ( !ctx->copy_from_src(&used_addrsize, addrsize, sizeof(used_addrsize)) )
		return -1;
	if ( peer_size < used_addrsize )
		used_addrsize = peer_size;
	if ( !ctx->copy_to_dest(addr, peer, used_addrsize) )
		return -1;
	if ( !ctx->copy_to_dest(addrsize, &used_addrsize, sizeof(used_addrsize)) )
		return -1;
	return 0;
}

int StreamSocket::getsockname(ioctx_t* ctx, uint8_t* addr, size_t* addrsize)
{
	ScopedLock lock(&socket_lock);
	size_t used_addrsize;
	if ( !ctx->copy_from_src(&used_addrsize, addrsize, sizeof(used_addrsize)) )
		return -1;
	if ( name_size < used_addrsize )
		used_addrsize = name_size;
	if ( !ctx->copy_to_dest(addr, name, used_addrsize) )
		return -1;
	if ( !ctx->copy_to_dest(addrsize, &used_addrsize, sizeof(used_addrsize)) )
		return -1;
	return 0;
}

int StreamSocket::sockatmark(ioctx_t* ctx)
{
	return incoming.sockatmark(ctx);
}

Manager::Manager()
{
	this->manager_lock = KTHREAD_MUTEX_INITIALIZER;
	this->first_server = NULL;
	this->last_server = NULL;
}

Manager::~Manager()
{
	assert(!first_server);
	assert(!last_server);
}

static int CompareAddress(const struct sockaddr_un* a,
                          const struct sockaddr_un* b)
{
	size_t path_offset = offsetof(struct sockaddr_un, sun_path);
	const char* a_path = (const char*) a + path_offset;
	const char* b_path = (const char*) b + path_offset;
	return strcmp(a_path, b_path);
}

StreamSocket* Manager::LookupServer(struct sockaddr_un* address)
{
	Ref<Descriptor> root = CurrentProcess()->GetRoot();
	for ( StreamSocket* iter = first_server; iter; iter = iter->next_socket )
		if ( CompareAddress(iter->name, address) == 0 &&
		     iter->root->dev == root->dev && iter->root->ino == root->ino )
			return iter;
	return NULL;
}

static StreamSocket* QueuePop(StreamSocket** first, StreamSocket** last)
{
	StreamSocket* ret = *first;
	assert(ret);
	QueueRemove(first, last, ret);
	return ret;
}

bool Manager::Bind(StreamSocket* socket,
                   struct sockaddr_un* addr,
                   size_t addrsize)
{
	ScopedLock lock(&manager_lock);
	if ( LookupServer(addr) )
		return errno = EADDRINUSE, false;
	socket->name = addr;
	socket->name_size = addrsize;
	socket->is_registered = true;
	QueueAppend(&first_server, &last_server, socket);
	return true;
}

bool Manager::Listen(StreamSocket* socket)
{
	ScopedLock lock(&manager_lock);
	socket->is_listening = true;
	return true;
}

void Manager::Unbind(StreamSocket* socket)
{
	ScopedLock lock(&manager_lock);
	while ( socket->first_pending )
	{
		socket->first_pending->is_refused = true;
		kthread_cond_signal(&socket->first_pending->accepted_cond);
		socket->first_pending = socket->first_pending->next_socket;
	}
	socket->last_pending = NULL;
	if ( socket->is_registered )
		QueueRemove(&first_server, &last_server, socket);
	free(socket->name);
	socket->name = NULL;
	socket->name_size = 0;
}

void Manager::Unlisten(StreamSocket* socket)
{
	ScopedLock lock(&manager_lock);
	socket->is_listening = false;
}

int Manager::AcceptPoll(StreamSocket* socket, ioctx_t* /*ctx*/, PollNode* node)
{
	ScopedLock lock(&manager_lock);
	if ( socket->first_pending &&
	    ((POLLIN | POLLRDNORM) & node->events) )
		return node->master->revents |= ((POLLIN | POLLRDNORM) & node->events), 0;
	socket->accept_poll_channel.Register(node);
	return errno = EAGAIN, -1;
}

Ref<StreamSocket> Manager::Accept(StreamSocket* socket, ioctx_t* ctx,
	                              uint8_t* addr, size_t* addrsize, int flags)
{
	if ( flags & ~(0) )
		return errno = EINVAL, Ref<StreamSocket>(NULL);

	ScopedLock lock(&manager_lock);

	while ( !socket->first_pending )
	{
		if ( (ctx->dflags & O_NONBLOCK) || (flags & SOCK_NONBLOCK) )
			return errno = EWOULDBLOCK, Ref<StreamSocket>(NULL);
		if ( !kthread_cond_wait_signal(&socket->pending_cond, &manager_lock) )
			return errno = EINTR, Ref<StreamSocket>(NULL);
	}

	StreamSocket* client = socket->first_pending;

	if ( addr )
	{
		size_t used_addrsize;
		if ( !ctx->copy_from_src(&used_addrsize, addrsize,
		                         sizeof(used_addrsize)) )
			return Ref<StreamSocket>(NULL);
		if ( client->name_size < used_addrsize )
			used_addrsize = client->name_size;
		if ( !ctx->copy_to_dest(addr, client->name, used_addrsize) )
			return Ref<StreamSocket>(NULL);
		if ( !ctx->copy_to_dest(addrsize, &used_addrsize,
		                        sizeof(used_addrsize)) )
			return Ref<StreamSocket>(NULL);
	}

	Ref<StreamSocket> server(new StreamSocket(0, 0, 0666, Ref<Manager>(this)));
	if ( !server )
		return Ref<StreamSocket>(NULL);

	server->name = (struct sockaddr_un*) malloc(socket->name_size);
	if ( !server->name )
		return Ref<StreamSocket>(NULL);
	server->peer = (struct sockaddr_un*) malloc(client->name_size);
	if ( !server->peer )
		return Ref<StreamSocket>(NULL);

	server->name_size = socket->name_size;
	memcpy(server->name, socket->name, socket->name_size);
	server->peer_size = client->name_size;
	memcpy(server->peer, client->name, client->name_size);

	QueuePop(&socket->first_pending, &socket->last_pending);

	if ( !client->outgoing.Connect(&server->incoming) )
		return Ref<StreamSocket>(NULL);
	if ( !server->outgoing.Connect(&client->incoming) )
	{
		client->outgoing.Disconnect();
		server->incoming.Disconnect();
		return Ref<StreamSocket>(NULL);
	}

	client->is_connected = true;
	server->is_connected = true;

	kthread_cond_signal(&client->accepted_cond);

	return server;
}

bool Manager::Connect(StreamSocket* socket,
                      struct sockaddr_un* addr,
                      size_t addrsize)
{
	ScopedLock lock(&manager_lock);
	StreamSocket* server = LookupServer(addr);
	if ( !server || !server->is_listening )
		return errno = ECONNREFUSED, false;

	socket->is_refused = false;

	QueueAppend(&server->first_pending, &server->last_pending, socket);
	kthread_cond_signal(&server->pending_cond);
	server->accept_poll_channel.Signal(POLLIN | POLLRDNORM);

	while ( !(socket->is_connected || socket->is_refused) )
		if ( !kthread_cond_wait_signal(&socket->accepted_cond, &manager_lock) &&
		    !(socket->is_connected || socket->is_refused) )
		{
			QueueRemove(&server->first_pending, &server->last_pending, socket);
			return errno = EINTR, false;
		}

	if ( socket->is_refused )
		return false;
	socket->peer = addr;
	socket->peer_size = addrsize;
	return true;
}

static Ref<Manager> manager;

void Init()
{
	manager = Ref<Manager>(new Manager());
}

Ref<Inode> Socket(int type, int protocol)
{
	if ( protocol != 0 )
		return errno = EPROTONOSUPPORT, Ref<Inode>(NULL);
	switch ( type )
	{
	case SOCK_STREAM: return Ref<Inode>(new StreamSocket(0, 0, 0600, manager));
	default: return errno = ESOCKTNOSUPPORT, Ref<Inode>(NULL);
	}
}

} // namespace NetFS
} // namespace Sortix
