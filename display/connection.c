/*
 * Copyright (c) 2014, 2015, 2016, 2023, 2024 Jonas 'Sortie' Termansen.
 * Copyright (c) 2023 Juhani 'nortti' Krekelä.
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

#include <sys/display.h>
#include <sys/socket.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "connection.h"
#include "display.h"

void connection_schedule_transmit(struct connection* connection,
                                  const void* buffer,
                                  size_t count)
{
	assert(connection);
	size_t available = connection->outgoing_size - connection->outgoing_used;
	if ( available < count )
	{
		// TODO: Overflow.
		size_t required_size = connection->outgoing_used + count;
		// TODO: Check allocation.
		unsigned char* new_outgoing = malloc(required_size);
		size_t first_available =
			connection->outgoing_size - connection->outgoing_offset;
		size_t first = connection->outgoing_used < first_available ?
		               connection->outgoing_used :
		               first_available;
		if ( connection->outgoing )
		{
			memcpy(new_outgoing, connection->outgoing +
			       connection->outgoing_offset, first);
			size_t second = connection->outgoing_used - first;
			memcpy(new_outgoing + first, connection->outgoing, second);
			free(connection->outgoing);
		}
		connection->outgoing_offset = 0;
		connection->outgoing_size = required_size;
		connection->outgoing = new_outgoing;
	}

	size_t used_offset =
		(connection->outgoing_offset + connection->outgoing_used) %
		connection->outgoing_size;
	size_t first_available = connection->outgoing_size - used_offset;
	size_t first = count < first_available ? count : first_available;
	memcpy(connection->outgoing + used_offset, buffer, first);
	size_t second = count - first;
	memcpy(connection->outgoing, (const unsigned char*) buffer + first, second);
	connection->outgoing_used += count;
}

void connection_schedule_ack_event(struct connection* connection,
                                   uint32_t id,
                                   int32_t error)
{
	struct event_ack event = { .id = id, .error = error };
	struct display_packet_header header = { .id = EVENT_ACK,
	                                        .size = sizeof(event) };
	connection_schedule_transmit(connection, &header, sizeof(header));
	connection_schedule_transmit(connection, &event, sizeof(event));
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
void connection_handler_##message_name( \
	struct connection* connection, \
	struct display_##message_name* msg, \
	void* auxiliary __attribute__((unused)), \
	size_t auxiliary_size __attribute__((unused)), \
	const struct server* server __attribute__((unused)))

#define CONNECTION_MESSAGE_HANDLER(message_name) \
void connection_handler_##message_name( \
	struct connection* connection, \
	struct display_##message_name* msg, \
	unsigned char* auxiliary, \
	size_t auxiliary_size, \
	const struct server* server __attribute__((unused)))

#define CONNECTION_MESSAGE_HANDLER_NO_AUX_SERVER(message_name) \
void connection_handler_##message_name( \
	struct connection* connection, \
	struct display_##message_name* msg, \
	void* auxiliary __attribute__((unused)), \
	size_t auxiliary_size __attribute__((unused)), \
	const struct server* server)

#define CONNECTION_MESSAGE_HANDLER_SERVER(message_name) \
void connection_handler_##message_name( \
	struct connection* connection, \
	struct display_##message_name* msg, \
	unsigned char* auxiliary, \
	size_t auxiliary_size, \
	const struct server* server)

CONNECTION_MESSAGE_HANDLER_NO_AUX(shutdown)
{
	(void) connection;
	if ( msg->code == 0 )
		display_exit(server->display, 0);
	else if ( msg->code == 1 )
		display_exit(server->display, 1);
	else if ( msg->code == 2 )
		display_exit(server->display, 2);
	else if ( msg->code == 3 )
		display_exit(server->display, 3);
	else
		display_exit(server->display, 0);
}

CONNECTION_MESSAGE_HANDLER_NO_AUX(create_window)
{
	struct window* window =
		connection_find_window_raw(connection, msg->window_id);
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
	src.buffer = (uint32_t*) auxiliary;

	if ( auxiliary_size < sizeof(uint32_t) * src.xres * src.yres )
		return;

	struct framebuffer dst =
		framebuffer_crop(window_client_buffer(window),
		                 msg->left, msg->top, msg->width, msg->height);

	framebuffer_copy_to_framebuffer(dst, src);

	window_schedule_redraw(window);
}

CONNECTION_MESSAGE_HANDLER(title_window)
{
	struct window* window = connection_find_window(connection, msg->window_id);
	if ( !window )
		return;

	const char* title = (char*) auxiliary;
	free(window->title);
	window->title = strndup(title, auxiliary_size);

	window_render_frame(window);
}

CONNECTION_MESSAGE_HANDLER_NO_AUX(show_window)
{
	struct window* window = connection_find_window(connection, msg->window_id);
	if ( !window )
		return;
	if ( !window->show )
		display_schedule_redraw(window->display);
	window->show = true;
}

CONNECTION_MESSAGE_HANDLER_NO_AUX(hide_window)
{
	struct window* window = connection_find_window(connection, msg->window_id);
	if ( !window )
		return;
	if ( window->show )
		display_schedule_redraw(window->display);
	window->show = false;
}

CONNECTION_MESSAGE_HANDLER_SERVER(chkblayout)
{
	if ( tcsetblob(server->tty_fd, "kblayout", auxiliary, auxiliary_size) < 0 )
		connection_schedule_ack_event(connection, msg->id, errno);
	else
		connection_schedule_ack_event(connection, msg->id, 0);
}

CONNECTION_MESSAGE_HANDLER_NO_AUX(request_displays)
{
	struct event_displays event;
	event.id = msg->id;
	event.displays = 1; // TODO: Multimonitor support.

	struct display_packet_header header;
	header.id = EVENT_DISPLAYS;
	header.size = sizeof(event);

	connection_schedule_transmit(connection, &header, sizeof(header));
	connection_schedule_transmit(connection, &event, sizeof(event));
}

static struct dispmsg_crtc_mode*
get_available_modes(const struct tiocgdisplay* display,
                    size_t* modes_count_ptr)
{
	struct dispmsg_get_crtc_modes msg;
	msg.msgid = DISPMSG_GET_CRTC_MODES;
	msg.device = display->device;
	msg.connector = display->connector;
	size_t guess = 1;
	while ( true )
	{
		struct dispmsg_crtc_mode* ret =
			calloc(guess, sizeof(struct dispmsg_crtc_mode));
		if ( !ret )
			return NULL;
		msg.modes_length = guess;
		msg.modes = ret;
		if ( dispmsg_issue(&msg, sizeof(msg)) == 0 )
		{
			*modes_count_ptr = guess;
			return ret;
		}
		free(ret);
		if ( errno == ERANGE && guess < msg.modes_length )
		{
			guess = msg.modes_length;
			continue;
		}
		return NULL;
	}
}

CONNECTION_MESSAGE_HANDLER_NO_AUX_SERVER(request_display_modes)
{
	struct event_display_modes event;
	event.id = msg->id;
	event.modes_count = 0;

	struct dispmsg_crtc_mode* modes = NULL;
	// TODO: Multimonitor support.
	if ( msg->display_id != 0 )
	{
		connection_schedule_ack_event(connection, msg->id, EINVAL);
		return;
	}
	else
	{
		size_t modes_count;
		modes = get_available_modes(&server->display->display, &modes_count);
		if ( !modes )
		{
			connection_schedule_ack_event(connection, msg->id, errno);
			return;
		}
		else if ( (uint32_t) modes_count != modes_count )
		{
			free(modes);
			connection_schedule_ack_event(connection, msg->id, EOVERFLOW);
			return;
		}
		event.modes_count = modes_count;
	}

	size_t modes_size = event.modes_count * sizeof(struct dispmsg_crtc_mode);

	struct display_packet_header header;
	header.id = EVENT_DISPLAY_MODES;
	header.size = sizeof(event) + modes_size;

	connection_schedule_transmit(connection, &header, sizeof(header));
	connection_schedule_transmit(connection, &event, sizeof(event));
	connection_schedule_transmit(connection, modes, modes_size);
}

static bool get_current_mode(const struct tiocgdisplay* display,
                             struct dispmsg_crtc_mode* mode)
{
	struct dispmsg_set_crtc_mode msg;
	msg.msgid = DISPMSG_GET_CRTC_MODE;
	msg.device = display->device;
	msg.connector = display->connector;
	if ( dispmsg_issue(&msg, sizeof(msg)) )
		return false;
	*mode = msg.mode;
	return true;
}

CONNECTION_MESSAGE_HANDLER_NO_AUX_SERVER(request_display_mode)
{
	struct event_display_mode event;
	event.id = msg->id;

	// TODO: Multimonitor support.
	if ( msg->display_id != 0 )
	{
		connection_schedule_ack_event(connection, msg->id, EINVAL);
		return;
	}
	else if ( !get_current_mode(&server->display->display, &event.mode) )
	{
		connection_schedule_ack_event(connection, msg->id, EINVAL);
		return;
	}

	struct display_packet_header header;
	header.id = EVENT_DISPLAY_MODE;
	header.size = sizeof(event);

	connection_schedule_transmit(connection, &header, sizeof(header));
	connection_schedule_transmit(connection, &event, sizeof(event));
}

static bool set_current_mode(const struct tiocgdisplay* display,
                             struct dispmsg_crtc_mode mode)
{
	struct dispmsg_set_crtc_mode msg;
	msg.msgid = DISPMSG_SET_CRTC_MODE;
	msg.device = display->device;
	msg.connector = display->connector;
	msg.mode = mode;
	return dispmsg_issue(&msg, sizeof(msg)) == 0;
}

CONNECTION_MESSAGE_HANDLER_NO_AUX_SERVER(set_display_mode)
{
	// TODO: Multimonitor support.
	if ( msg->display_id != 0 )
		connection_schedule_ack_event(connection, msg->id, EINVAL);
	else if ( !set_current_mode(&server->display->display, msg->mode) )
		connection_schedule_ack_event(connection, msg->id, errno);
	else
		connection_schedule_ack_event(connection, msg->id, 0);
}

typedef void (*connection_message_handler)(struct connection* connection,
                                           void* msg,
                                           void* auxiliary,
                                           size_t auxiliary_size,
                                           const struct server* server);

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
	REGISTER_CONNECTION_MESSAGE_HANDLER(chkblayout),
	REGISTER_CONNECTION_MESSAGE_HANDLER(request_displays),
	REGISTER_CONNECTION_MESSAGE_HANDLER(request_display_modes),
	REGISTER_CONNECTION_MESSAGE_HANDLER(request_display_mode),
	REGISTER_CONNECTION_MESSAGE_HANDLER(set_display_mode),
};

size_t num_connection_message_handlers = sizeof(connection_message_handlers) /
                                         sizeof(connection_message_handlers[0]);

short connection_interested_poll_events(struct connection* connection)
{
	return POLLIN | (connection->outgoing_used ? POLLOUT : 0);
}

void connection_can_read(struct connection* connection,
                         const struct server* server)
{
	while ( connection->packet_header_received <
	        sizeof(connection->packet_header) )
	{
		ssize_t amount = read(connection->fd,
		                      (unsigned char*) &connection->packet_header +
		                      connection->packet_header_received,
		                      sizeof(connection->packet_header) -
		                      connection->packet_header_received);
		if ( amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) )
			return;
		if ( amount < 0 || amount == 0 )
			return; // TODO: No longer signal interest in reading + disconnect.
		connection->packet_header_received += amount;
	}

	size_t packet_size = connection->packet_header.size;

	// TODO: Check allocation and protect against too big buffers.
	if ( !connection->packet )
		connection->packet = malloc(packet_size);

	while ( connection->packet_received < packet_size )
	{
		ssize_t amount = read(connection->fd,
		                      connection->packet + connection->packet_received,
		                      packet_size - connection->packet_received);
		if ( amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) )
			return;
		if ( amount < 0 || amount == 0 )
			return; // TODO: No longer signal interest in reading + disconnect.
		connection->packet_received += amount;
	}

	size_t packet_id = connection->packet_header.id;

	if ( packet_id < num_connection_message_handlers )
	{
		struct connection_message_handler_registration* handler =
			&connection_message_handlers[packet_id];
		unsigned char* auxiliary = connection->packet + handler->message_size;
		size_t auxiliary_size = packet_size - handler->message_size;
		handler->handler(connection, connection->packet,
		                 auxiliary, auxiliary_size, server);
	}

	connection->packet_header_received = 0;
	free(connection->packet);
	connection->packet = NULL;
	connection->packet_received = 0;

	// TODO: Check if we can receive another packet, but only if we haven't
	//       done so much work that the display server is starved.
}

void connection_can_write(struct connection* connection)
{
	while ( connection->outgoing_used )
	{
		size_t available =
			connection->outgoing_size - connection->outgoing_offset;
		size_t count = connection->outgoing_used < available ?
		               connection->outgoing_used : available;
		unsigned char* buf = connection->outgoing + connection->outgoing_offset;
		ssize_t amount = write(connection->fd, buf, count);
		if ( amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) )
			return;
		if ( amount < 0 || amount == 0 )
			return; // TODO: No longer signal interest in writing + disconnect.
		connection->outgoing_offset = (connection->outgoing_offset + amount) %
		                              connection->outgoing_size;
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
