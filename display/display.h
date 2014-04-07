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
 * display.h
 * Display server.
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stddef.h>

#include "damage-rect.h"
#include "framebuffer.h"

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
	int pointer_x;
	int pointer_y;
	size_t mouse_byte_count;
	uint8_t mouse_bytes[3];
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
