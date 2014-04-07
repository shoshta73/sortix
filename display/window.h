/*
 * Copyright (c) 2014, 2015, 2016, 2018, 2022 Jonas 'Sortie' Termansen.
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
 * window.h
 * Window abstraction.
 */

#ifndef WINDOW_H
#define WINDOW_H

#include <sys/types.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "framebuffer.h"

struct connection;
struct display;

static const size_t BORDER_WIDTH = 8;
static const size_t TITLE_HEIGHT = 28;
static const size_t RESIZE_GRACE = 16;

enum window_state
{
	WINDOW_STATE_REGULAR,
	WINDOW_STATE_MAXIMIZED,
	WINDOW_STATE_MINIMIZED,
	WINDOW_STATE_TILE_LEFT,
	WINDOW_STATE_TILE_RIGHT,
	WINDOW_STATE_TILE_TOP,
	WINDOW_STATE_TILE_TOP_LEFT,
	WINDOW_STATE_TILE_TOP_RIGHT,
	WINDOW_STATE_TILE_BOTTOM,
	WINDOW_STATE_TILE_BOTTOM_LEFT,
	WINDOW_STATE_TILE_BOTTOM_RIGHT,
};

struct window
{
	struct display* display;
	struct connection* connection;
	struct window* above_window;
	struct window* below_window;
	struct framebuffer buffer;
	char* title;
	ssize_t left;
	ssize_t top;
	size_t width;
	size_t height;
	ssize_t saved_left;
	ssize_t saved_top;
	size_t saved_width;
	size_t saved_height;
	uint32_t window_id;
	enum window_state window_state;
	bool created;
	bool show;
	bool focus;
	bool grab_input;
};

struct framebuffer window_client_buffer(struct window* window);
void window_render_frame(struct window* window);
void window_move(struct window* window, size_t left, size_t top);
void window_resize(struct window* window, size_t width, size_t height);
void window_client_resize(struct window* window, size_t client_width, size_t client_height);
void window_initialize(struct window* window, struct connection* connection, struct display* display, uint32_t window_id);
void window_destroy(struct window* window);
void window_drag_resize(struct window* window, int ld, int td, int wd, int hd);
void window_on_display_resolution_change(struct window* window, struct display* display);
void window_maximize(struct window* window);
void window_restore(struct window* window);
void window_toggle_maximized(struct window* window);
void window_tile(struct window* window, enum window_state state, size_t left, size_t top, size_t width, size_t height);
void window_tile_leftward(struct window* window);
void window_tile_rightward(struct window* window);
void window_tile_up(struct window* window);
void window_tile_down(struct window* window);
void window_tile_left(struct window* window);
void window_tile_right(struct window* window);
void window_tile_top(struct window* window);
void window_tile_top_left(struct window* window);
void window_tile_top_right(struct window* window);
void window_tile_bottom(struct window* window);
void window_tile_bottom_left(struct window* window);
void window_tile_bottom_right(struct window* window);
void window_notify_client_resize(struct window* window);

#endif
