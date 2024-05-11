/*
 * Copyright (c) 2014-2017, 2022-2024 Jonas 'Sortie' Termansen.
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

#include <sys/keycodes.h>

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <timespec.h>

#include <display-protocol.h>

#include "connection.h"
#include "display.h"
#include "framebuffer.h"
#include "pixel.h"
#include "vgafont.h"
#include "window.h"

struct framebuffer window_client_buffer(struct window* window)
{
	if ( window->window_state != WINDOW_STATE_REGULAR )
		return framebuffer_crop(window->buffer, 0, TITLE_HEIGHT,
		                        window->width, window->height - TITLE_HEIGHT);
	return framebuffer_crop(window->buffer, BORDER_WIDTH, TITLE_HEIGHT,
	                        window->width - 2 * BORDER_WIDTH,
	                        window->height - TITLE_HEIGHT - BORDER_WIDTH);
}

void window_schedule_redraw(struct window* window)
{
	if ( window->show )
		display_schedule_redraw(window->display);
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
	uint32_t button_hover_glass = make_color_a(220, 220, 255, 255);
	uint32_t button_press_glass = make_color_a(180, 180, 255, 255);

	size_t start_x = 0;
	size_t start_y = 0;
	size_t end_x = window->width - 1;
	size_t end_y = window->height - 1;

	bool maximized = window->window_state != WINDOW_STATE_REGULAR;

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
			if ( maximized && y < start_y + t0 )
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
			          y == start_y + b2 || y == end_y - b2 )
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
	ssize_t tt_width = render_text_width(tt); // Potentially adjusted later.
	size_t tt_height = FONT_HEIGHT;
	size_t tt_pos_y = (TITLE_HEIGHT - FONT_HEIGHT) / 2 + 2;
	uint32_t tt_color = title_color;

	size_t border_width = maximized ? 0 : b2 + 1;
	size_t button_area_height = maximized ? t0 : t0 - (b2 + 1);
	size_t button_area_width = button_area_height;
	size_t button_area_top = maximized ? 0 : b2;
	size_t button_size = FONT_WIDTH - 1;
	size_t button_top = (button_area_height - button_size + 1) / 2;
	size_t button_left = (button_area_width - button_size + 1) / 2;
	ssize_t buttons_x = window->width - border_width - button_area_width*3 + 1;
	struct framebuffer buttons_fb =
		framebuffer_crop(window->buffer, buttons_x, button_area_top,
		                 button_area_width * 3, button_area_height);
	for ( size_t n = 0; n < 3; n++ )
	{
		uint32_t color = glass_color;
		switch ( window->button_states[n] )
		{
		case BUTTON_STATE_NORMAL: continue;
		case BUTTON_STATE_HOVER: color = button_hover_glass; break;
		case BUTTON_STATE_PRESSED: color = button_press_glass; break;
		}
		size_t bx = button_area_width * n;
		size_t by = 0;
		for ( size_t y = 0; y < button_area_height; y++ )
			for ( size_t x = 0; x < button_area_width; x++ )
				framebuffer_set_pixel(buttons_fb, bx + x, by + y, color);
	}
	for ( size_t i = 0; i < button_size; i++ )
	{
		size_t bx = button_area_width * 0 + button_left;
		size_t by = button_top;
		framebuffer_set_pixel(buttons_fb, bx + i,
		                      by + button_size - 1, tt_color);
		framebuffer_set_pixel(buttons_fb, bx + i,
		                      by + button_size - 2, tt_color);
	}
	for ( size_t i = 0; i < button_size; i++ )
	{
		size_t bx = button_area_width * 1 + button_left;
		size_t by = button_top;
		framebuffer_set_pixel(buttons_fb, bx + i,
		                      by, tt_color);
		framebuffer_set_pixel(buttons_fb, bx + i,
		                      by + button_size - 1 , tt_color);
		framebuffer_set_pixel(buttons_fb, bx,
		                      by + i, tt_color);
		framebuffer_set_pixel(buttons_fb, bx + button_size - 1,
		                      by + i, tt_color);

		framebuffer_set_pixel(buttons_fb, bx + i,
		                      by + 1, tt_color);
		framebuffer_set_pixel(buttons_fb, bx + i,
		                      by + button_size - 2 , tt_color);
		framebuffer_set_pixel(buttons_fb, bx + 1,
		                      by + i, tt_color);
		framebuffer_set_pixel(buttons_fb, bx + button_size - 2,
		                      by + i, tt_color);
	}
	for ( size_t i = 0; i < button_size; i++ )
	{
		size_t bx = button_area_width * 2 + button_left;
		size_t by = button_top;
		framebuffer_set_pixel(buttons_fb, bx + i,
		                      by + i, tt_color);
		framebuffer_set_pixel(buttons_fb, bx + i,
		                      by + button_size - 1 - i, tt_color);

		framebuffer_set_pixel(buttons_fb, bx + i - 1,
		                      by + i, tt_color);
		framebuffer_set_pixel(buttons_fb, bx + i - 1,
		                      by + button_size - 1 - i, tt_color);
	}

	ssize_t q = 500 - window->width;
	ssize_t q_width = 200;
	q = q < q_width ? q : q_width;
	q = 0 < q ? q : 0;
	ssize_t center_over = window->width - (button_area_width * 3 * q / q_width);
	ssize_t tt_pos_x = (center_over - tt_width) / 2;
	if ( tt_pos_x < (ssize_t)border_width )
	{
		tt_pos_x = border_width;
		tt_width = buttons_x - border_width;
		tt_width = 0 < tt_width ? tt_width : 0;
	}
	render_text(framebuffer_crop(window->buffer, tt_pos_x, tt_pos_y,
	                             tt_width, tt_height), tt, tt_color);
	window_schedule_redraw(window);
}

void window_move(struct window* window, size_t left, size_t top)
{
	window->left = left;
	window->top = top;
	window_schedule_redraw(window);
}

void window_client_resize(struct window* window,
                          size_t client_width,
                          size_t client_height)
{
	if ( window->window_state != WINDOW_STATE_MINIMIZED )
		window->window_state = WINDOW_STATE_REGULAR;

	struct framebuffer old_fb = window->buffer;

	window->width = client_width + BORDER_WIDTH + BORDER_WIDTH;
	window->height = client_height + TITLE_HEIGHT + BORDER_WIDTH;

	window->buffer.xres = window->width;
	window->buffer.yres = window->height;
	window->buffer.pitch = window->width;
	// TODO: Check malloc.
	window->buffer.buffer =
		malloc(sizeof(uint32_t) * window->width * window->height);
	for ( size_t y = 0; y < window->height; y++ )
		for ( size_t x = 0; x < window->width; x++ )
			framebuffer_set_pixel(window->buffer, x, y,
			                      framebuffer_get_pixel(old_fb, x, y));

	free(old_fb.buffer);

	window_render_frame(window);
	window_notify_client_resize(window);
	window_schedule_redraw(window);
}

void window_resize(struct window* window, size_t width, size_t height)
{
	if ( width < BORDER_WIDTH + BORDER_WIDTH )
		width = BORDER_WIDTH + BORDER_WIDTH;
	if ( height < TITLE_HEIGHT + BORDER_WIDTH )
		height = TITLE_HEIGHT + BORDER_WIDTH;
	// TODO: Keep proper track of this for each state.
	size_t client_width = width - (BORDER_WIDTH + BORDER_WIDTH);
	size_t client_height = height - (TITLE_HEIGHT + BORDER_WIDTH);
	window_client_resize(window, client_width, client_height);
}

void window_drag_resize(struct window* window, int ld, int td, int wd, int hd)
{
	// TODO: Keep proper track of this for each state.
	size_t client_width = window->width - (BORDER_WIDTH + BORDER_WIDTH);
	size_t client_height = window->height - (TITLE_HEIGHT + BORDER_WIDTH);
	if ( ld || td )
		window_move(window, window->left + ld, window->top + td);
	if ( wd || hd )
	{
		ssize_t new_width = (ssize_t) client_width + wd;
		ssize_t new_height = (ssize_t) client_height + hd;
		if ( new_width < 1 )
			new_width = 1;
		if ( new_height < 1 )
			new_height = 1;
		window_client_resize(window, new_width, new_height);
	}
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
	window->title_click_time = timespec_make(-1, 0);
	window->window_id = window_id;
	display_add_window(window->display, window);
	window->top = next_window_position;
	window->left = next_window_position;
	size_t max_position = display->screen_width < display->screen_height ?
	                      display->screen_width : display->screen_height;
	max_position = (max_position * 6) / 10;
	next_window_position += 30;
	next_window_position %= max_position;
	window_client_resize(window, 0, 0);
}

void window_quit(struct window* window)
{
	struct event_quit event;
	event.window_id = window->window_id;

	struct display_packet_header header;
	header.id = EVENT_QUIT;
	header.size = sizeof(event);

	assert(window->connection);

	connection_schedule_transmit(window->connection, &header, sizeof(header));
	connection_schedule_transmit(window->connection, &event, sizeof(event));
}

void window_destroy(struct window* window)
{
	display_remove_window(window->display, window);
	free(window->buffer.buffer);
	free(window->title);
	memset(window, 0, sizeof(*window));
	window->created = false;
}

void window_on_display_resolution_change(struct window* window,
                                         struct display* display)
{
	switch ( window->window_state )
	{
	case WINDOW_STATE_REGULAR:
	{
		ssize_t left = window->left, top = window->top;
		if ( (ssize_t) display->screen_width <= left )
			left = 0;
		if ( (ssize_t) display->screen_height <= top )
			top = 0;
		window_move(window, left, top);
		break;
	}
	case WINDOW_STATE_MAXIMIZED: window_maximize(window); break;
	case WINDOW_STATE_MINIMIZED: break;
	case WINDOW_STATE_TILE_LEFT: window_tile_left(window); break;
	case WINDOW_STATE_TILE_RIGHT: window_tile_right(window); break;
	case WINDOW_STATE_TILE_TOP: window_tile_top(window); break;
	case WINDOW_STATE_TILE_TOP_LEFT: window_tile_top_left(window); break;
	case WINDOW_STATE_TILE_TOP_RIGHT: window_tile_top_right(window); break;
	case WINDOW_STATE_TILE_BOTTOM: window_tile_bottom(window); break;
	case WINDOW_STATE_TILE_BOTTOM_LEFT: window_tile_bottom_left(window); break;
	case WINDOW_STATE_TILE_BOTTOM_RIGHT:
		window_tile_bottom_right(window);
		break;
	}
}

void window_tile(struct window* window, enum window_state state, size_t left,
                 size_t top, size_t width, size_t height)
{
	if ( window->window_state == state &&
	     window->left == (ssize_t) left && window->top == (ssize_t) top &&
	     window->width == width && window->height == height )
		return;

	if ( window->window_state == WINDOW_STATE_REGULAR )
	{
		window->saved_left = window->left;
		window->saved_top = window->top;
		window->saved_width = window->width;
		window->saved_height = window->height;
	}

	free(window->buffer.buffer);

	window->left = left;
	window->top = top;
	window->width = width;
	window->height = height;

	// TODO: Share logic with window_client_resize.
	window->buffer.xres = window->width;
	window->buffer.yres = window->height;
	window->buffer.pitch = window->width;
	// TODO: Check malloc.
	window->buffer.buffer =
		calloc(1, sizeof(uint32_t) * window->width * window->height);

	window->window_state = state;

	window_render_frame(window);
	window_notify_client_resize(window);
}

void window_maximize(struct window* window)
{
	window_tile(window, WINDOW_STATE_MAXIMIZED,
	            0, 0,
	            window->display->screen_width, window->display->screen_height);
}

void window_restore(struct window* window)
{
	if ( window->window_state == WINDOW_STATE_REGULAR )
		return;
	window->top = window->saved_top;
	window->left = window->saved_left;
	window_client_resize(window, window->saved_width - 2 * BORDER_WIDTH,
	                     window->saved_height - TITLE_HEIGHT - BORDER_WIDTH);
	window_notify_client_resize(window);
}

void window_toggle_maximized(struct window* window)
{
	if ( window->window_state == WINDOW_STATE_MAXIMIZED )
		window_restore(window);
	else
		window_maximize(window);
}

void window_tile_leftward(struct window* window)
{
	switch ( window->window_state )
	{
	case WINDOW_STATE_REGULAR: window_tile_left(window); break;
	case WINDOW_STATE_MAXIMIZED: window_tile_left(window); break;
	case WINDOW_STATE_MINIMIZED: window_tile_right(window); break;
	case WINDOW_STATE_TILE_LEFT: break;
	case WINDOW_STATE_TILE_RIGHT: window_restore(window); break;
	case WINDOW_STATE_TILE_TOP: window_tile_top_left(window); break;
	case WINDOW_STATE_TILE_TOP_LEFT: break;
	case WINDOW_STATE_TILE_TOP_RIGHT: window_tile_top(window); break;
	case WINDOW_STATE_TILE_BOTTOM: window_tile_bottom_left(window); break;
	case WINDOW_STATE_TILE_BOTTOM_LEFT: break;
	case WINDOW_STATE_TILE_BOTTOM_RIGHT: window_tile_bottom(window); break;
	}
}

void window_tile_rightward(struct window* window)
{
	switch ( window->window_state )
	{
	case WINDOW_STATE_REGULAR: window_tile_right(window); break;
	case WINDOW_STATE_MAXIMIZED: window_tile_right(window); break;
	case WINDOW_STATE_MINIMIZED: window_tile_right(window); break;
	case WINDOW_STATE_TILE_LEFT: window_restore(window); break;
	case WINDOW_STATE_TILE_RIGHT: break;
	case WINDOW_STATE_TILE_TOP: window_tile_top_right(window); break;
	case WINDOW_STATE_TILE_TOP_LEFT: window_tile_top(window); break;
	case WINDOW_STATE_TILE_TOP_RIGHT: break;
	case WINDOW_STATE_TILE_BOTTOM: window_tile_bottom_right(window); break;
	case WINDOW_STATE_TILE_BOTTOM_LEFT: window_tile_bottom(window); break;
	case WINDOW_STATE_TILE_BOTTOM_RIGHT: break;
	}
}

void window_tile_up(struct window* window)
{
	switch ( window->window_state )
	{
	case WINDOW_STATE_REGULAR: window_tile_top(window); break;
	case WINDOW_STATE_MAXIMIZED: window_restore(window); break;
	case WINDOW_STATE_MINIMIZED: window_tile_top(window); break;
	case WINDOW_STATE_TILE_LEFT: window_tile_top_left(window); break;
	case WINDOW_STATE_TILE_RIGHT: window_tile_top_right(window); break;
	case WINDOW_STATE_TILE_TOP: window_maximize(window); break;
	case WINDOW_STATE_TILE_TOP_LEFT: break;
	case WINDOW_STATE_TILE_TOP_RIGHT: break;
	case WINDOW_STATE_TILE_BOTTOM: window_restore(window); break;
	case WINDOW_STATE_TILE_BOTTOM_LEFT: window_tile_left(window); break;
	case WINDOW_STATE_TILE_BOTTOM_RIGHT: window_tile_right(window); break;
	}
}

void window_tile_down(struct window* window)
{
	switch ( window->window_state )
	{
	case WINDOW_STATE_REGULAR: window_tile_bottom(window); break;
	case WINDOW_STATE_MAXIMIZED: window_tile_top(window); break;
	case WINDOW_STATE_MINIMIZED: window_tile_bottom(window); break;
	case WINDOW_STATE_TILE_LEFT: window_tile_bottom_left(window); break;
	case WINDOW_STATE_TILE_RIGHT: window_tile_bottom_right(window); break;
	case WINDOW_STATE_TILE_TOP: window_restore(window); break;
	case WINDOW_STATE_TILE_TOP_LEFT: window_tile_left(window); break;
	case WINDOW_STATE_TILE_TOP_RIGHT: window_tile_right(window); break;
	case WINDOW_STATE_TILE_BOTTOM: break;
	case WINDOW_STATE_TILE_BOTTOM_LEFT: break;
	case WINDOW_STATE_TILE_BOTTOM_RIGHT: break;
	}
}

void window_tile_left(struct window* window)
{
	window_tile(window, WINDOW_STATE_TILE_LEFT,
	            0,
	            0,
	            window->display->screen_width / 2,
	            window->display->screen_height);
}

void window_tile_right(struct window* window)
{
	window_tile(window, WINDOW_STATE_TILE_RIGHT,
	            (window->display->screen_width + 1) / 2,
	            0,
	            (window->display->screen_width + 1) / 2,
	            window->display->screen_height);
}

void window_tile_top(struct window* window)
{
	window_tile(window, WINDOW_STATE_TILE_TOP,
	            0,
	            0,
	            window->display->screen_width,
	            window->display->screen_height / 2);
}

void window_tile_top_left(struct window* window)
{
	window_tile(window, WINDOW_STATE_TILE_TOP_LEFT,
	            0,
	            0,
	            window->display->screen_width / 2,
	            window->display->screen_height / 2);
}

void window_tile_top_right(struct window* window)
{
	window_tile(window, WINDOW_STATE_TILE_TOP_RIGHT,
	            (window->display->screen_width + 1) / 2,
	            0,
	            (window->display->screen_width + 1) / 2,
	            window->display->screen_height / 2);
}

void window_tile_bottom(struct window* window)
{
	window_tile(window, WINDOW_STATE_TILE_BOTTOM,
	            0,
	            (window->display->screen_height + 1) / 2,
	            window->display->screen_width,
	            (window->display->screen_height + 1) / 2);
}

void window_tile_bottom_left(struct window* window)
{
	window_tile(window, WINDOW_STATE_TILE_BOTTOM_LEFT,
	            0,
	            (window->display->screen_height + 1) / 2,
	            window->display->screen_width / 2,
	            (window->display->screen_height + 1) / 2);
}

void window_tile_bottom_right(struct window* window)
{
	window_tile(window, WINDOW_STATE_TILE_BOTTOM_RIGHT,
	            (window->display->screen_width + 1) / 2,
	            (window->display->screen_height + 1) / 2,
	            (window->display->screen_width + 1) / 2,
	            (window->display->screen_height + 1) / 2);
}

void window_notify_client_resize(struct window* window)
{
	struct event_resize event;
	event.window_id = window->window_id;
	event.width = window_client_buffer(window).xres;
	event.height = window_client_buffer(window).yres;

	struct display_packet_header header;
	header.id = EVENT_RESIZE;
	header.size = sizeof(event);

	assert(window->connection);

	connection_schedule_transmit(window->connection, &header, sizeof(header));
	connection_schedule_transmit(window->connection, &event, sizeof(event));
}

void window_send_key(struct window* window, uint32_t codepoint)
{
	int kbkey = KBKEY_DECODE(codepoint);
	unsigned int abskbkey = kbkey < 0 ? -kbkey : kbkey;
	if ( 0 < abskbkey && abskbkey < 512 )
	{
		size_t index = abskbkey / (8 * sizeof(size_t));
		size_t bit = abskbkey % (8 * sizeof(size_t));
		size_t mask = 1ULL << bit;
		if ( kbkey < 0 )
			window->key_bitmap[index] &= ~mask;
		else
			window->key_bitmap[index] |= mask;
	}

	struct event_keyboard event;
	event.window_id = window->window_id;
	event.codepoint = codepoint;

	struct display_packet_header header;
	header.id = EVENT_KEYBOARD;
	header.size = sizeof(event);

	assert(window->connection);

	connection_schedule_transmit(window->connection, &header, sizeof(header));
	connection_schedule_transmit(window->connection, &event, sizeof(event));
}

void window_unsend_keys(struct window* window)
{
	struct event_keyboard event;
	event.window_id = window->window_id;
	event.codepoint = 0;

	struct display_packet_header header;
	header.id = EVENT_KEYBOARD;
	header.size = sizeof(event);

	assert(window->connection);

	for ( int kbkey = 1; kbkey < 512; kbkey++ )
	{
		size_t index = kbkey / (8 * sizeof(size_t));
		size_t bit = kbkey % (8 * sizeof(size_t));
		size_t mask = 1ULL << bit;
		if ( window->key_bitmap[index] & mask )
		{
			event.codepoint = KBKEY_ENCODE(-kbkey);
			connection_schedule_transmit(window->connection, &header,
			                             sizeof(header));
			connection_schedule_transmit(window->connection, &event,
			                             sizeof(event));
		}
	}
	memset(window->key_bitmap, 0, sizeof(window->key_bitmap));
}
