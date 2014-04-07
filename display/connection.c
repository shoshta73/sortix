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
 * connection.c
 * Display protocol implementation.
 */

#include <sys/socket.h>
#include <sys/termmode.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "connection.h"

void connection_schedule_transmit(struct connection* connection,
                                  const void* buffer,
                                  size_t count)
{
	assert(connection);
	size_t available = connection->outgoing_size - connection->outgoing_used;
	if ( available < count )
	{
		size_t required_size = connection->outgoing_used + count;
		unsigned char* new_outgoing = (unsigned char*) malloc(required_size);
		size_t first_part_available = connection->outgoing_size - connection->outgoing_offset;
		size_t first_part = connection->outgoing_used < first_part_available ?
		                    connection->outgoing_used :
		                    first_part_available;
		if ( connection->outgoing )
		{
			memcpy(new_outgoing, connection->outgoing +
			       connection->outgoing_offset, first_part);
			size_t second_part = connection->outgoing_used - first_part;
			memcpy(new_outgoing + first_part, connection->outgoing, second_part);
			free(connection->outgoing);
		}
		connection->outgoing_offset = 0;
		connection->outgoing_size = required_size;
		connection->outgoing = new_outgoing;
	}

	size_t used_offset = (connection->outgoing_offset + connection->outgoing_used) % connection->outgoing_size;
	size_t first_part_available = connection->outgoing_size - used_offset;
	size_t first_part = count < first_part_available ? count : first_part_available;
	memcpy(connection->outgoing + used_offset, buffer, first_part);
	size_t second_part = count - first_part;
	memcpy(connection->outgoing, (const unsigned char*) buffer + first_part, second_part);
	connection->outgoing_used += count;
}

void connection_initialize(struct connection* connection,
                           struct display* display,
                           int fd)
{
	memset(connection, 0, sizeof(*connection));
	connection->display = display;
	connection->fd = fd;
}

struct window* connection_find_window_raw(struct connection* connection,
                                          uint32_t window_id)
{
	if ( MAX_WINDOWS_PER_CONNECTION <= window_id )
		return NULL;
	return &connection->windows[window_id];
}

struct window* connection_find_window(struct connection* connection,
                                      uint32_t window_id)
{
	struct window* result = connection_find_window_raw(connection, window_id);
	if ( !result->created )
		return NULL;
	return result;
}

#define CONNECTION_MESSAGE_HANDLER_NO_AUX(message_name) \
void connection_handler_##message_name(struct connection* connection, \
                                       struct display_##message_name* msg, \
                                       void* auxilerary __attribute__((unused)), \
                                       size_t auxilerary_size __attribute__((unused)))

#define CONNECTION_MESSAGE_HANDLER(message_name) \
void connection_handler_##message_name(struct connection* connection, \
                                       struct display_##message_name* msg, \
                                       unsigned char* auxilerary, \
                                       size_t auxilerary_size)

CONNECTION_MESSAGE_HANDLER_NO_AUX(shutdown)
{
	(void) connection;
	if ( msg->code == 0 )
		exit(0);
	else if ( msg->code == 1 )
		exit(1);
	else if ( msg->code == 2 )
		exit(2);
	else
		exit(0);
}

CONNECTION_MESSAGE_HANDLER_NO_AUX(create_window)
{
	struct window* window = connection_find_window_raw(connection, msg->window_id);
	if ( !window )
		return;
	if ( window->created )
		return;
	window_initialize(window, connection, connection->display, msg->window_id);
}

CONNECTION_MESSAGE_HANDLER_NO_AUX(destroy_window)
{
	struct window* window = connection_find_window(connection, msg->window_id);
	if ( !window )
		return;
	window_destroy(window);
}

CONNECTION_MESSAGE_HANDLER_NO_AUX(resize_window)
{
	struct window* window = connection_find_window(connection, msg->window_id);
	if ( !window )
		return;
	window_client_resize(window, msg->width, msg->height);
}

CONNECTION_MESSAGE_HANDLER(render_window)
{
	struct window* window = connection_find_window(connection, msg->window_id);
	if ( !window )
		return;

	struct framebuffer src;
	src.xres = msg->width;
	src.yres = msg->height;
	src.pitch = msg->width;
	src.buffer = (uint32_t*) auxilerary;

	if ( auxilerary_size < sizeof(uint32_t) * src.xres * src.yres )
		return;

	struct framebuffer dst =
		framebuffer_crop(window_client_buffer(window),
		                 msg->left, msg->top, msg->width, msg->height);

	framebuffer_copy_to_framebuffer(dst, src);
}

CONNECTION_MESSAGE_HANDLER(title_window)
{
	struct window* window = connection_find_window(connection, msg->window_id);
	if ( !window )
		return;

	const char* title = (char*) auxilerary;
	free(window->title);
	window->title = strndup(title, auxilerary_size);

	window_render_frame(window);
}

CONNECTION_MESSAGE_HANDLER_NO_AUX(show_window)
{
	struct window* window = connection_find_window(connection, msg->window_id);
	if ( !window )
		return;
	window->show = true;
}

CONNECTION_MESSAGE_HANDLER_NO_AUX(hide_window)
{
	struct window* window = connection_find_window(connection, msg->window_id);
	if ( !window )
		return;
	window->show = false;
}

typedef void (*connection_message_handler)(struct connection* connection,
                                           void* msg,
                                           void* auxilerary,
                                           size_t auxilerary_size);

struct connection_message_handler_registration
{
	connection_message_handler handler;
	size_t message_size;
};

#define REGISTER_CONNECTION_MESSAGE_HANDLER(message_name) \
	{ (connection_message_handler) connection_handler_##message_name, \
	  sizeof(struct display_##message_name) }

struct connection_message_handler_registration connection_message_handlers[] =
{
	REGISTER_CONNECTION_MESSAGE_HANDLER(create_window),
	REGISTER_CONNECTION_MESSAGE_HANDLER(destroy_window),
	REGISTER_CONNECTION_MESSAGE_HANDLER(resize_window),
	REGISTER_CONNECTION_MESSAGE_HANDLER(render_window),
	REGISTER_CONNECTION_MESSAGE_HANDLER(title_window),
	REGISTER_CONNECTION_MESSAGE_HANDLER(show_window),
	REGISTER_CONNECTION_MESSAGE_HANDLER(hide_window),
	REGISTER_CONNECTION_MESSAGE_HANDLER(shutdown),
};

size_t num_connection_message_handlers = sizeof(connection_message_handlers) /
                                         sizeof(connection_message_handlers[0]);

short connection_interested_poll_events(struct connection* connection)
{
	return POLLIN | (connection->outgoing_used ? POLLOUT : 0);
}

void connection_can_read(struct connection* connection)
{
	while ( connection->packet_header_received < sizeof(connection->packet_header) )
	{
		ssize_t amount = read(connection->fd,
		                      (unsigned char*) &connection->packet_header +
		                      connection->packet_header_received,
		                      sizeof(connection->packet_header) -
		                      connection->packet_header_received);
		if ( amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) )
			return;
		if ( amount < 0 || amount == 0 )
			return; // TODO: No longer signal interest in reading and disconnect.
		connection->packet_header_received += amount;
	}

	size_t packet_length = connection->packet_header.message_length;

	if ( !connection->packet )
		connection->packet = (unsigned char*) malloc(packet_length);

	while ( connection->packet_received < packet_length )
	{
		ssize_t amount = read(connection->fd,
		                      connection->packet + connection->packet_received,
		                      packet_length - connection->packet_received);
		if ( amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) )
			return;
		if ( amount < 0 || amount == 0 )
			return; // TODO: No longer signal interest in reading and disconnect.
		connection->packet_received += amount;
	}

	size_t packet_id = connection->packet_header.message_id;

	if ( packet_id < num_connection_message_handlers )
	{
		struct connection_message_handler_registration* handler =
			&connection_message_handlers[packet_id];
		unsigned char* auxilerary = connection->packet + handler->message_size;
		size_t auxilerary_size = packet_length - handler->message_size;
		handler->handler(connection, connection->packet,
		                 auxilerary, auxilerary_size);
	}

	connection->packet_header_received = 0;
	free(connection->packet);
	connection->packet = NULL;
	connection->packet_received = 0;

	// TODO: Check if we can received another packet, but only if we haven't
	//       done so much work that the display server is starved.
}

void connection_can_write(struct connection* connection)
{
	while ( connection->outgoing_used )
	{
		size_t available = connection->outgoing_size - connection->outgoing_offset;
		size_t count = connection->outgoing_used < available ? connection->outgoing_used : available;
		ssize_t amount = write(connection->fd, connection->outgoing + connection->outgoing_offset, count);
		if ( amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) )
			return;
		if ( amount < 0 || amount == 0 )
			return; // TODO: No longer signal interest in writing and disconnect.
		connection->outgoing_offset = (connection->outgoing_offset + amount) % connection->outgoing_size;
		connection->outgoing_used -= amount;
	}

	free(connection->outgoing);
	connection->outgoing = NULL;
	connection->outgoing_used = 0;
	connection->outgoing_size = 0;
}

void connection_destroy(struct connection* connection)
{
	for ( size_t i = 0; i < MAX_WINDOWS_PER_CONNECTION; i++ )
	{
		if ( !connection->windows[i].created )
			continue;
		window_destroy(&connection->windows[i]);
	}
	close(connection->fd);
}
