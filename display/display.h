/*
 * Copyright (c) 2014, 2015, 2016, 2022 Jonas 'Sortie' Termansen.
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
 * display.h
 * Display server.
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <sys/ps2mouse.h>

#include <stdbool.h>
#include <stddef.h>

#include "damage-rect.h"
#include "framebuffer.h"

enum mouse_state
{
	MOUSE_STATE_NONE,
	MOUSE_STATE_TITLE_MOVE,
	MOUSE_STATE_RESIZE_BOTTOM,
	MOUSE_STATE_RESIZE_BOTTOM_LEFT,
	MOUSE_STATE_RESIZE_BOTTOM_RIGHT,
	MOUSE_STATE_RESIZE_LEFT,
	MOUSE_STATE_RESIZE_RIGHT,
	MOUSE_STATE_RESIZE_TOP,
	MOUSE_STATE_RESIZE_TOP_LEFT,
	MOUSE_STATE_RESIZE_TOP_RIGHT,
};

struct window;

struct display
{
	struct damage_rect damage_rect;
	struct window* top_window;
	struct window* bottom_window;
	struct window* active_window;
	struct window* tab_candidate;
	size_t screen_width;
	size_t screen_height;
	size_t num_tabs;
	bool key_lctrl;
	bool key_lalt;
	bool key_lsuper;
	bool key_rsuper;
	int pointer_x;
	int pointer_y;
	enum mouse_state mouse_state;
	size_t mouse_byte_count;
	uint8_t mouse_bytes[MOUSE_PACKET_SIZE];
};

void display_initialize(struct display* display);
void assert_is_well_formed_display_list(struct display* display);
void assert_is_well_formed_display(struct display* display);
void display_link_window_at_top(struct display* display, struct window* window);
void display_unlink_window(struct display* display, struct window* window);
void display_unlink_window_removal(struct display* display, struct window* window);
void display_unmark_active_window(struct display* display, struct window* window);
void display_mark_active_window(struct display* display, struct window* window);
void display_update_active_window(struct display* display);
void display_move_window_to_top(struct display* display, struct window* window);
void display_change_active_window(struct display* display, struct window* window);
void display_set_active_window(struct display* display, struct window* window);
void display_add_window(struct display* display, struct window* window);
void display_remove_window(struct display* display, struct window* window);
void display_composit(struct display* display, struct framebuffer fb);
void display_render(struct display* display);
void display_keyboard_event(struct display* display, uint32_t codepoint);
void display_on_resolution_change(struct display* display, size_t width, size_t height);
void display_mouse_event(struct display* display, uint8_t byte);

#endif
