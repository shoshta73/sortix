/*
 * Copyright (c) 2014, 2015, 2016, 2017 Jonas 'Sortie' Termansen.
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
 * window.c
 * Window abstraction.
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <display-protocol.h>

#include "connection.h"
#include "display.h"
#include "framebuffer.h"
#include "pixel.h"
#include "vgafont.h"
#include "window.h"

struct framebuffer window_client_buffer(struct window* window)
{
	if ( window->window_state == WINDOW_STATE_MAXIMIZED )
		return framebuffer_crop(window->buffer, 0, TITLE_HEIGHT,
		                        window->width, window->height - TITLE_HEIGHT);
	return framebuffer_crop(window->buffer, BORDER_WIDTH, TITLE_HEIGHT,
	                        window->width - 2 * BORDER_WIDTH,
	                        window->height - TITLE_HEIGHT - BORDER_WIDTH);
}

void window_render_frame(struct window* window)
{
	if ( !window->width || !window->height )
		return;

	bool has_focus = window->display->tab_candidate ?
	                 window->display->tab_candidate == window :
	                 window->display->active_window == window;

	uint32_t glass_color = has_focus ? make_color_a(200, 200, 255, 192)
	                                 : make_color_a(180, 180, 255, 128);
	uint32_t title_color = has_focus ? make_color_a(16, 16, 16, 240)
	                                 : make_color_a(32, 32, 32, 200);

	size_t start_x = 0;
	size_t start_y = 0;
	size_t end_x = window->width - 1;
	size_t end_y = window->height - 1;

	bool maximized = window->window_state == WINDOW_STATE_MAXIMIZED;

	int b0 = 0;
	int b1 = 1;
	int b2 = 2;
	int b3 = BORDER_WIDTH;
	int t0 = TITLE_HEIGHT;

	for ( size_t y = start_y; y <= end_y; y++ )
	{
		for ( size_t x = start_x; x <= end_x; x++ )
		{
			uint32_t color;
			if ( maximized && y <= start_y + t0 )
				color = glass_color;
			else if ( maximized )
				continue;
			else if ( x == start_x + b0 || x == end_x - b0 ||
			          y == start_y + b0 || y == end_y - b0 )
				color = make_color_a(0, 0, 0, 32);
			else if ( x == start_x + b1 || x == end_x - b1 ||
			          y == start_y + b1 || y == end_y - b1 )
				color = make_color_a(0, 0, 0, 64);
			else if ( x == start_x + b2 || x == end_x - b2 ||
			          y == start_y + b2 ||+ y == end_y - b2 )
				color = make_color(240, 240, 250);
			else if ( x < start_x + (b3-1) || x > end_x - (b3-1) ||
			          y < start_y + (t0-1) || y > end_y - (b3-1) )
				color = glass_color;
			else if ( x == start_x + (b3-1) || x == end_x - (b3-1) ||
			          y == start_y + (t0-1) || y == end_y - (b3-1) )
				color = make_color(64, 64, 64);
			else
				continue;
			framebuffer_set_pixel(window->buffer, x, y, color);
		}
	}

	const char* tt = window->title ? window->title : "";
	size_t tt_max_width = window->width - 2 * BORDER_WIDTH;
	size_t tt_desired_width = render_text_width(tt);
	size_t tt_width = tt_desired_width < tt_max_width ? tt_desired_width : tt_max_width;
	size_t tt_height = FONT_HEIGHT;
	size_t tt_pos_x = BORDER_WIDTH + (tt_max_width - tt_width) / 2;
	size_t tt_pos_y = (TITLE_HEIGHT - FONT_HEIGHT) / 2 + 2;
	uint32_t tt_color = title_color;
	render_text(framebuffer_crop(window->buffer, tt_pos_x, tt_pos_y, tt_width, tt_height), tt, tt_color);
}

void window_move(struct window* window, size_t left, size_t top)
{
	window->left = left;
	window->top = top;
}

void window_client_resize(struct window* window,
                          size_t client_width,
                          size_t client_height)
{
	if ( window->window_state == WINDOW_STATE_MAXIMIZED )
		window->window_state = WINDOW_STATE_REGULAR;

	free(window->buffer.buffer);

	window->width = client_width + BORDER_WIDTH + BORDER_WIDTH;
	window->height = client_height + TITLE_HEIGHT + BORDER_WIDTH;

	window->buffer.xres = window->width;
	window->buffer.yres = window->height;
	window->buffer.pitch = window->width;
	window->buffer.buffer = (uint32_t*)
		malloc(sizeof(uint32_t) * window->width * window->height);
	memset(window->buffer.buffer, 0, sizeof(uint32_t) * window->width * window->height);

	window_render_frame(window);
	window_notify_client_resize(window);
}

void window_resize(struct window* window, size_t width, size_t height)
{
	if ( width < BORDER_WIDTH + BORDER_WIDTH )
		width = BORDER_WIDTH + BORDER_WIDTH;
	if ( height < TITLE_HEIGHT + BORDER_WIDTH )
		height = TITLE_HEIGHT + BORDER_WIDTH;
	size_t client_width = width - (BORDER_WIDTH + BORDER_WIDTH);
	size_t client_height = height - (TITLE_HEIGHT + BORDER_WIDTH);
	window_client_resize(window, client_width, client_height);
}

static size_t next_window_position = 25;

void window_initialize(struct window* window,
                       struct connection* connection,
                       struct display* display,
                       uint32_t window_id)
{
	memset(window, 0, sizeof(*window));
	window->created = true;
	window->connection = connection;
	window->display = display;
	window->title = NULL;
	window->window_id = window_id;
	display_add_window(window->display, window);
	window->top = next_window_position;
	window->left = next_window_position;
	next_window_position += 30;
	window_client_resize(window, 0, 0);
}

void window_destroy(struct window* window)
{
	display_remove_window(window->display, window);
	free(window->buffer.buffer);
	free(window->title);
	memset(window, 0, sizeof(*window));
	window->created = false;
}

void window_on_display_resolution_change(struct window* window, struct display* display)
{
	// TODO: Potentially move window back inside screen?
	if ( window->window_state == WINDOW_STATE_MAXIMIZED )
	{
		// TODO: Change size of maximized window.
		(void) display;
	}
}

void window_maximize(struct window* window)
{
	if ( window->window_state == WINDOW_STATE_MAXIMIZED )
		return;
	window->saved_left = window->left;
	window->saved_top = window->top;
	window->saved_width = window->width;
	window->saved_height = window->height;

	free(window->buffer.buffer);

	window->top = 0;
	window->left = 0;
	window->width = window->display->screen_width;
	window->height = window->display->screen_height;

	window->buffer.xres = window->width;
	window->buffer.yres = window->height;
	window->buffer.pitch = window->width;
	window->buffer.buffer = (uint32_t*)
		malloc(sizeof(uint32_t) * window->width * window->height);
	memset(window->buffer.buffer, 0, sizeof(uint32_t) * window->width * window->height);

	window->window_state = WINDOW_STATE_MAXIMIZED;

	window_render_frame(window);
	window_notify_client_resize(window);
}

void window_restore(struct window* window)
{
	if ( window->window_state != WINDOW_STATE_MAXIMIZED )
		return;
	window->top = window->saved_top;
	window->left = window->saved_left;
	window_client_resize(window, window->saved_width - 2 * BORDER_WIDTH, window->saved_height - TITLE_HEIGHT - BORDER_WIDTH);
	window_notify_client_resize(window);
}

void window_toggle_maximized(struct window* window)
{
	if ( window->window_state == WINDOW_STATE_MAXIMIZED )
		window_restore(window);
	else
		window_maximize(window);
}

void window_notify_client_resize(struct window* window)
{
	struct event_resize event;
	event.window_id = window->window_id;
	event.width = window_client_buffer(window).xres;
	event.height = window_client_buffer(window).yres;

	struct display_packet_header header;
	header.message_id = EVENT_RESIZE;
	header.message_length = sizeof(event);

	assert(window->connection);

	connection_schedule_transmit(window->connection, &header, sizeof(header));
	connection_schedule_transmit(window->connection, &event, sizeof(event));
}
