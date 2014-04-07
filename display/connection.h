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
 * connection.h
 * Display protocol implementation.
 */

#ifndef CONNECTION_H
#define CONNECTION_H

#include <stddef.h>
#include <stdint.h>

#include <display-protocol.h>

#include "window.h"

struct display;
struct window;

#define MAX_WINDOWS_PER_CONNECTION 256

struct connection
{
	struct display* display;
	struct window windows[MAX_WINDOWS_PER_CONNECTION];
	struct display_packet_header packet_header;
	size_t packet_header_received;
	unsigned char* packet;
	size_t packet_received;
	unsigned char* outgoing;
	size_t outgoing_offset;
	size_t outgoing_used;
	size_t outgoing_size;
	int fd;
};

void connection_schedule_transmit(struct connection* connection,
                                  const void* buffer,
                                  size_t count);
void connection_initialize(struct connection* connection,
                           struct display* display,
                           int fd);
struct window* connection_find_window_raw(struct connection* connection,
                                          uint32_t window_id);
struct window* connection_find_window(struct connection* connection,
                                      uint32_t window_id);
short connection_interested_poll_events(struct connection* connection);
void connection_can_read(struct connection* connection);
void connection_can_write(struct connection* connection);
void connection_destroy(struct connection* connection);

#endif
