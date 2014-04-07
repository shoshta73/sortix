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
 * display-protocol.h
 * Display protocol.
 */

#ifndef DISPLAY_PROTOCOL_H
#define DISPLAY_PROTOCOL_H

#include <stdint.h>

struct display_packet_header
{
	uint32_t message_id;
	uint32_t message_length;
};

#define DISPLAY_CREATE_WINDOW 0
struct display_create_window
{
	uint32_t window_id;
};

#define DISPLAY_DESTROY_WINDOW 1
struct display_destroy_window
{
	uint32_t window_id;
};

#define DISPLAY_RESIZE_WINDOW 2
struct display_resize_window
{
	uint32_t window_id;
	uint32_t width;
	uint32_t height;
};

#define DISPLAY_RENDER_WINDOW 3
struct display_render_window
{
	uint32_t window_id;
	uint32_t left;
	uint32_t top;
	uint32_t width;
	uint32_t height;
	/* width * height * sizeof(uint32_t) image bytes follows */
};

#define DISPLAY_TITLE_WINDOW 4
struct display_title_window
{
	uint32_t window_id;
	/* An UTF-8 string follows */
};

#define DISPLAY_SHOW_WINDOW 5
struct display_show_window
{
	uint32_t window_id;
};

#define DISPLAY_HIDE_WINDOW 6
struct display_hide_window
{
	uint32_t window_id;
};

#define DISPLAY_SHUTDOWN 7
struct display_shutdown
{
	uint32_t code;
};

#define EVENT_DISCONNECT 0
struct event_disconnect
{
};

#define EVENT_QUIT 1
struct event_quit
{
	uint32_t window_id;
};

#define EVENT_RESIZE 2
struct event_resize
{
	uint32_t window_id;
	uint32_t width;
	uint32_t height;
};

#define EVENT_KEYBOARD 3
struct event_keyboard
{
	uint32_t window_id;
	uint32_t codepoint;
};

#endif
