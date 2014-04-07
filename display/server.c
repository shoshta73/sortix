/*
 * Copyright (c) 2014, 2015, 2016 Jonas 'Sortie' Termansen.
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
 * server.c
 * Display server main loop.
 */

#include <sys/socket.h>
#include <sys/termmode.h>
#include <sys/un.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <display-protocol.h>

#include "connection.h"
#include "display.h"
#include "server.h"
#include "vgafont.h"

static int open_local_server_socket(const char* path, int flags)
{
	size_t path_length = strlen(path);
	size_t addr_size = offsetof(struct sockaddr_un, sun_path) + path_length + 1;
	struct sockaddr_un* sockaddr = (struct sockaddr_un*) malloc(addr_size);
	if ( !sockaddr )
		return -1;
	sockaddr->sun_family = AF_LOCAL;
	strcpy(sockaddr->sun_path, path);
	int fd = socket(AF_LOCAL, SOCK_STREAM | flags, 0);
	if ( fd < 0 )
		return free(sockaddr), -1;
	if ( bind(fd, (const struct sockaddr*) sockaddr, addr_size) < 0 )
		return close(fd), free(sockaddr), -1;
	if ( listen(fd, 5) < 0 )
		return close(fd), free(sockaddr), -1;
	free(sockaddr);
	return fd;
}

void server_initialize(struct server* server, struct display* display)
{
	memset(server, 0, sizeof(*server));

	server->display = display;

	load_font();

	server->tty_fd = 0;
	if ( !isatty(0) )
	{
		server->tty_fd = open("/dev/tty", O_RDONLY);
		if ( server->tty_fd < 0 )
			err(1, "/dev/tty");
	}

	server->mouse_fd = open("/dev/mouse", O_RDONLY | O_CLOEXEC);
	if ( server->mouse_fd < 0 )
		err(1, "%s", "/dev/mouse");

	server->server_path = "/run/display";
	server->server_fd = open_local_server_socket(server->server_path,
	                                             SOCK_NONBLOCK | SOCK_CLOEXEC);
	if ( server->server_fd < 0 )
		err(1, "open_local_server_socket: %s", server->server_path);

	unsigned int termmode =
		TERMMODE_KBKEY | TERMMODE_UNICODE | TERMMODE_NONBLOCK;
	if ( settermmode(0, termmode) < 0 )
		err(1, "settermmode");

	server->pfds_count = server_pfds_count(server);
	server->pfds = (struct pollfd*)
		reallocarray(NULL, sizeof(struct pollfd), server->pfds_count);
	if ( !server->pfds )
		err(1, "malloc");
}

bool server_accept(struct server* server)
{
	int client_fd = accept4(server->server_fd, NULL, NULL, SOCK_NONBLOCK);
	if ( client_fd < 0 )
	{
		warn("accept: %s", server->server_path);
		return false;
	}

	if ( server->connections_used == server->connections_length )
	{
		size_t new_length = server->connections_length * 2;
		if ( !new_length )
			new_length = 16;
		struct connection** new_connections = (struct connection**)
			reallocarray(server->connections, new_length,
			             sizeof(struct connection*));
		if ( !new_connections )
		{
			warn("dropped connection: %s: malloc", server->server_path);
			close(client_fd);
			return false;
		}
		server->connections = new_connections;
		server->connections_length = new_length;
	}

	size_t new_pfds_count = server_pfds_count(server) + 1;
	struct pollfd* new_pfds = (struct pollfd*)
		reallocarray(server->pfds, sizeof(struct pollfd), new_pfds_count);
	if ( !new_pfds )
	{
		warn("dropped connection: %s: malloc", server->server_path);
		close(client_fd);
		return false;
	}
	server->pfds = new_pfds;
	server->pfds_count = new_pfds_count;

	struct connection* connection = (struct connection*)
		malloc(sizeof(struct connection));
	if ( !connection )
	{
		warn("dropped connection: %s: malloc", server->server_path);
		close(client_fd);
		return false;
	}
	server->connections[server->connections_used++] = connection;
	connection_initialize(connection, server->display, client_fd);

	return true;
}

size_t server_pfds_count(const struct server* server)
{
	return 3 + server->connections_used;
}

void server_poll(struct server* server)
{
	struct pollfd* pfds = server->pfds;

	pfds[0].fd = server->server_fd;
	pfds[0].events = POLLIN;
	pfds[0].revents = 0;
	pfds[1].fd = server->tty_fd;
	pfds[1].events = POLLIN;
	pfds[1].revents = 0;
	pfds[2].fd = server->mouse_fd;
	pfds[2].events = POLLIN;
	pfds[2].revents = 0;
	size_t cpfd_off = 3;

	size_t connections_polled = server->connections_used;
	for ( size_t i = 0; i < connections_polled; i++ )
	{
		struct pollfd* pfd = &pfds[cpfd_off + i];
		struct connection* connection = server->connections[i];
		pfd->fd = connection->fd;
		pfd->events = connection_interested_poll_events(connection);
		pfd->revents = 0;
	}
	size_t pfds_used = cpfd_off + connections_polled;

	int num_events = ppoll(pfds, pfds_used, NULL, NULL);
	if ( num_events < 0 )
		err(1, "poll");

	if ( pfds[0].revents )
	{
		// TODO: Handle if this can actually happen.
		assert(!(pfds[0].revents & POLLERR));
		assert(!(pfds[0].revents & POLLHUP));
		assert(!(pfds[0].revents & POLLNVAL));

		server_accept(server);
	}

	if ( pfds[1].revents )
	{
		// TODO: Handle if this can actually happen.
		assert(!(pfds[1].revents & POLLERR));
		assert(!(pfds[1].revents & POLLHUP));
		assert(!(pfds[1].revents & POLLNVAL));

		uint32_t codepoint;
		ssize_t size = sizeof(codepoint);
		while ( read(server->tty_fd, &codepoint, size) == size )
			display_keyboard_event(server->display, codepoint);
	}

	if ( pfds[2].revents )
	{
		// TODO: Handle if this can actually happen.
		assert(!(pfds[2].revents & POLLERR));
		assert(!(pfds[2].revents & POLLHUP));
		assert(!(pfds[2].revents & POLLNVAL));

		unsigned char events[64];
		ssize_t amount = read(server->mouse_fd, events, sizeof(events));
		for ( ssize_t i = 0; i < amount; i++ )
			display_mouse_event(server->display, events[i]);
	}

	bool any_disconnect = false;
	for ( size_t i = 0; i < connections_polled; i++ )
	{
		struct pollfd* pfd = &pfds[cpfd_off + i];
		if ( !pfd->revents )
			continue;
		struct connection* connection = server->connections[i];
		if ( pfd->revents & (POLLERR | POLLHUP | POLLNVAL) )
		{
			connection_destroy(connection);
			free(connection);
			server->connections[i] = NULL;
			any_disconnect = true;
			continue;
		}
		if ( pfd->revents & POLLOUT )
			connection_can_write(connection);
		if ( pfd->revents & POLLIN )
			connection_can_read(connection);
	}

	// Compact the array down here so the pfds match the connections above.
	if ( any_disconnect )
	{
		size_t new_used = 0;
		for ( size_t i = 0; i < server->connections_used; i++ )
		{
			if ( server->connections[i] )
				server->connections[new_used++] = server->connections[i];
		}
		server->connections_used = new_used;
	}
}

void server_mainloop(struct server* server)
{
	while ( true )
	{
		// TODO: Only do this if a redraw is actually needed.
		display_render(server->display);
		server_poll(server);
	}
}
