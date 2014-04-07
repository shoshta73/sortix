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
 * display-code.c
 * Display server logic.
 */


#include <sys/display.h>
#include <sys/keycodes.h>
#include <sys/ps2mouse.h>

#include <assert.h>
#include <err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <display-protocol.h>

#include "connection.h"
#include "display.h"
#include "pixel.h"
#include "vgafont.h"
#include "window.h"

extern struct framebuffer arrow_framebuffer;

void display_initialize(struct display* display)
{
	memset(display, 0, sizeof(*display));
}

void assert_is_well_formed_display_list(struct display* display)
{
	struct window* last_window = NULL;
	struct window* iterator = display->bottom_window;
	while ( iterator )
	{
		assert(iterator->below_window == last_window);
		last_window = iterator;
		iterator = iterator->above_window;
	}
	assert(last_window == display->top_window);
}

void assert_is_well_formed_display(struct display* display)
{
	assert_is_well_formed_display_list(display);

	bool found_active_window = display->active_window == NULL;
	bool found_tab_candidate = display->tab_candidate == NULL;
	struct window* iterator = display->bottom_window;
	while ( iterator )
	{
		if ( iterator == display->active_window )
			found_active_window = true;
		if ( iterator == display->tab_candidate )
			found_tab_candidate = true;
		iterator = iterator->above_window;
	}
	assert(found_active_window);
	assert(found_tab_candidate);
}

void display_link_window_at_top(struct display* display, struct window* window)
{
	assert_is_well_formed_display_list(display);

	assert(!window->above_window);
	assert(!window->below_window);
	assert(display->top_window != window);
	assert(display->bottom_window != window);

	if ( (window->below_window = display->top_window) )
		window->below_window->above_window = window;
	window->above_window = NULL;

	display->top_window = window;
	if ( !display->bottom_window )
		display->bottom_window = window;

	assert_is_well_formed_display_list(display);
}

void display_unlink_window(struct display* display, struct window* window)
{
	assert_is_well_formed_display_list(display);

	assert(window->below_window || display->bottom_window == window);
	assert(window->above_window || display->top_window == window);

	if ( window->below_window )
		window->below_window->above_window = window->above_window;
	else
		display->bottom_window = window->above_window;
	if ( window->above_window )
		window->above_window->below_window = window->below_window;
	else
		display->top_window = window->below_window;

	assert(display->bottom_window != window);
	assert(display->top_window != window);

	window->above_window = NULL;
	window->below_window = NULL;

	assert_is_well_formed_display_list(display);
}

void display_unlink_window_removal(struct display* display, struct window* window)
{
	assert_is_well_formed_display_list(display);

	if ( display->tab_candidate == window )
		if ( !(display->tab_candidate = window->below_window) )
			if ( (display->tab_candidate = display->top_window) == window )
				display->tab_candidate = NULL;

	if ( display->active_window == window )
		display->active_window = NULL;

	window->focus = false;

	assert_is_well_formed_display_list(display);

	display_unlink_window(display, window);

	assert_is_well_formed_display_list(display);
}

void display_unmark_active_window(struct display* display, struct window* window)
{
	assert(display->active_window == window);
	window->focus = false;
	display->active_window = NULL;
	window_render_frame(window);
}

void display_mark_active_window(struct display* display, struct window* window)
{
	assert(!display->active_window);
	window->focus = true;
	display->active_window = window;
	window_render_frame(window);
}

void display_update_active_window(struct display* display)
{
	if ( !display->active_window && display->top_window )
		display_mark_active_window(display, display->top_window);
}

void display_move_window_to_top(struct display* display, struct window* window)
{
	display_unlink_window(display, window);
	display_link_window_at_top(display, window);
}

void display_change_active_window(struct display* display, struct window* window)
{
	if ( display->active_window == window )
	{
		display_move_window_to_top(display, window);
		return;
	}

	display_unmark_active_window(display, display->active_window);
	display_mark_active_window(display, window);
}

void display_set_active_window(struct display* display, struct window* window)
{
	display_change_active_window(display, window);
	display_move_window_to_top(display, window);
}

void display_add_window(struct display* display, struct window* window)
{
	display_link_window_at_top(display, window);
	display_update_active_window(display);
}

void display_remove_window(struct display* display, struct window* window)
{
	display_unlink_window_removal(display, window);
	display_update_active_window(display);

	assert(display->top_window != window);
	assert(display->bottom_window != window);
	struct window* last_window = NULL;
	struct window* iterator = display->bottom_window;
	while ( iterator )
	{
		assert(iterator != window);
		assert(iterator->below_window == last_window);
		last_window = iterator;
		iterator = iterator->above_window;
	}
	assert(last_window == display->top_window);

	assert(!display->top_window || display->active_window);
}

void display_composit(struct display* display, struct framebuffer fb)
{
	struct damage_rect damage_rect = display->damage_rect;
	damage_rect.left = 0;
	damage_rect.top = 0;
	damage_rect.width = fb.xres;
	damage_rect.height = fb.yres;
	if ( !damage_rect.width || !damage_rect.height )
		return;

	uint32_t bg_color = make_color(0x89 * 2/3, 0xc7 * 2/3, 0xff * 2/3);
	for ( size_t y = 0; y < damage_rect.height; y++ )
		for ( size_t x = 0; x < damage_rect.width; x++ )
			framebuffer_set_pixel(fb, damage_rect.left + x, damage_rect.top + y, bg_color);

	for ( struct window* window = display->bottom_window;
	      window;
	      window = window->above_window )
	{
		if ( !window->show )
			continue;

		size_t winfb_left;
		size_t winfb_top;
		struct framebuffer winfb = window->buffer;

		if ( window->left < 0 )
		{
			winfb_left = 0;
			winfb = framebuffer_crop(winfb, -window->left, 0, winfb.xres, winfb.yres);
		}
		else
			winfb_left = window->left;

		if ( window->top < 0 )
		{
			winfb_top = 0;
			winfb = framebuffer_crop(winfb, 0, -window->top, winfb.xres, winfb.yres);
		}
		else
			winfb_top = window->top;

		size_t winfb_width = winfb.xres;
		size_t winfb_height = winfb.yres;

#if 0
		if ( winfb_left < damage_rect.left && winfb_width < damage_rect.left - winfb_left )
			continue;
		if ( winfb_left < damage_rect.left  )
			winfb_left = damage_rect.left, winfb_width -= damage_rect.left - winfb_left;
#endif

		struct framebuffer fb_dst =
			framebuffer_crop(fb, winfb_left, winfb_top, winfb_width, winfb_height);

		framebuffer_copy_to_framebuffer_blend(fb_dst, winfb);
	}

	const char* cursor_text = NULL;
	switch ( display->mouse_state )
	{
	case MOUSE_STATE_NONE: break;
	case MOUSE_STATE_TITLE_MOVE: break;
	case MOUSE_STATE_RESIZE_BOTTOM: cursor_text = "↓"; break;
	case MOUSE_STATE_RESIZE_BOTTOM_LEFT: cursor_text = "└"; break;
	case MOUSE_STATE_RESIZE_BOTTOM_RIGHT: cursor_text = "┘"; break;
	case MOUSE_STATE_RESIZE_LEFT: cursor_text = "←"; break;
	case MOUSE_STATE_RESIZE_RIGHT: cursor_text = "→"; break;
	case MOUSE_STATE_RESIZE_TOP: cursor_text = "↑"; break;
	case MOUSE_STATE_RESIZE_TOP_LEFT: cursor_text = "┌"; break;
	case MOUSE_STATE_RESIZE_TOP_RIGHT: cursor_text = "┐"; break;
	}

	int pointer_hwidth = arrow_framebuffer.xres / 2;
	int pointer_hheight = arrow_framebuffer.yres / 2;

	int pointer_x = display->pointer_x - (cursor_text ? 0 : pointer_hwidth);
	int pointer_y = display->pointer_y - (cursor_text ? 0 : pointer_hheight);

	struct framebuffer arrow_render = arrow_framebuffer;
	if ( pointer_x < 0 )
	{
		arrow_render = framebuffer_crop(arrow_render, -pointer_x, 0, arrow_render.xres, arrow_render.yres);
		pointer_x = 0;
	}
	if ( pointer_y < 0 )
	{
		arrow_render = framebuffer_crop(arrow_render, 0, -pointer_y, arrow_render.xres, arrow_render.yres);
		pointer_y = 0;
	}

	struct framebuffer fb_dst =
		framebuffer_crop(fb, pointer_x, pointer_y, fb.xres, fb.yres);

	if ( cursor_text != NULL )
		render_text(fb_dst, cursor_text, make_color(0, 0, 0));
	else
		framebuffer_copy_to_framebuffer_blend(fb_dst, arrow_render);

	memset(&damage_rect, 0, sizeof(damage_rect));
}

void display_render(struct display* display)
{
	struct dispmsg_crtc_mode mode;
	{
		struct dispmsg_get_crtc_mode msg;
		memset(&msg, 0, sizeof(msg));
		msg.msgid = DISPMSG_GET_CRTC_MODE;
		msg.device = 0; // TODO: Multi-screen support!
		msg.connector = 0; // TODO: Multi-screen support!

		if ( dispmsg_issue(&msg, sizeof(msg)) != 0 )
			err(1, "dispmsg_issue: dispmsg_get_crtc_mode");

		mode = msg.mode;
	}

	if ( !(mode.control & DISPMSG_CONTROL_VALID) )
		errx(1, "No valid video mode was set");
	if ( mode.control & DISPMSG_CONTROL_VGA )
		errx(1, "A VGA text mode was set");
	if ( mode.fb_format != 32 )
		errx(1, "A 32-bit video mode wasn't set");

	struct framebuffer fb;
	fb.xres = mode.view_xres;
	fb.yres = mode.view_yres;
	fb.pitch = mode.view_xres;
	size_t framebuffer_length = fb.xres * fb.yres;
	fb.buffer = (uint32_t*) malloc(sizeof(uint32_t) * framebuffer_length);
	size_t framebuffer_size = framebuffer_length * sizeof(fb.buffer[0]);

	display_on_resolution_change(display, mode.view_xres, mode.view_yres);

	display_composit(display, fb);

	{
		struct dispmsg_write_memory msg;
		memset(&msg, 0, sizeof(msg));
		msg.msgid = DISPMSG_WRITE_MEMORY;
		msg.device = 0; // TODO: Multi-screen support!
		msg.offset = 0; // TODO: mode.fb_location!
		msg.size = framebuffer_size;
		msg.src = (uint8_t*) fb.buffer;

		if ( dispmsg_issue(&msg, sizeof(msg)) != 0 )
			err(1, "dispmsg_issue: dispmsg_write_memory");
	}

	// TODO: This could be recycled if the resolution did not change.
	free(fb.buffer);
}

void display_keyboard_event(struct display* display, uint32_t codepoint)
{
	struct window* window = display->active_window;

	int kbkey = KBKEY_DECODE(codepoint);
	int abskbkey = kbkey < 0 ? -kbkey : kbkey;

	if ( kbkey && (!window || !window->grab_input) )
	{
		switch ( abskbkey )
		{
		case KBKEY_LCTRL: display->key_lctrl = kbkey > 0; break;
		case KBKEY_LALT: display->key_lalt = kbkey > 0; break;
		case KBKEY_LSUPER: display->key_lsuper = kbkey > 0; break;
		case KBKEY_RSUPER: display->key_rsuper = kbkey > 0; break;
		}
		if ( display->key_lctrl && display->key_lalt && kbkey == KBKEY_DELETE )
			exit(0);
		if ( display->key_lctrl && display->key_lalt && kbkey == KBKEY_T )
		{
			if ( !fork() )
			{
				execlp("terminal", "terminal", (char*) NULL);
				_exit(127);
			}
			return;
		}
		else if ( display->key_lctrl && display->key_lalt && kbkey == -KBKEY_T )
			return;
	}

	if ( kbkey && window && !window->grab_input )
	{
		// TODO: Ctrl+Q when termninal has a way of handling it or not.
		if ( (display->key_lalt && kbkey == KBKEY_F4) /* ||
		     (display->key_lctrl && kbkey == KBKEY_Q)*/  )
		{
			struct event_keyboard event;
			event.window_id = display->active_window->window_id;

			struct display_packet_header header;
			header.message_id = EVENT_QUIT;
			header.message_length = sizeof(event);

			assert(window->connection);

			connection_schedule_transmit(window->connection, &header, sizeof(header));
			connection_schedule_transmit(window->connection, &event, sizeof(event));
			return;
		}

		if ( display->key_lalt && kbkey == KBKEY_F10 )
		{
			window_toggle_maximized(display->active_window);
			return;
		}

		if ( display->key_lalt && kbkey == KBKEY_TAB )
		{
			if ( !display->tab_candidate )
				display->tab_candidate = display->active_window;
			struct window* old_candidate = display->tab_candidate;
			if ( !(display->tab_candidate = old_candidate->below_window) )
				display->tab_candidate = display->top_window;
			window_render_frame(old_candidate);
			window_render_frame(display->tab_candidate);
			return;
		}

		if ( kbkey == -KBKEY_LALT && display->tab_candidate )
		{
			if ( display->tab_candidate != display->active_window )
				display_set_active_window(display, display->tab_candidate);
			display->tab_candidate = NULL;
			window = display->active_window;
			return;
		}
		if ( display->key_lsuper || display->key_lsuper )
		{
			struct window* window = display->active_window;
			if ( kbkey == KBKEY_LEFT )
			{
				window_tile_leftward(window);
				return;
			}
			if ( kbkey == KBKEY_RIGHT )
			{
				window_tile_rightward(window);
				return;
			}
			if ( kbkey == KBKEY_UP )
			{
				window_tile_up(window);
				return;
			}
			if ( kbkey == KBKEY_DOWN )
			{
				window_tile_down(window);
				return;
			}
		}
	}

	const char* grab_inputbed_string = " - Input Grabbed";
	if ( kbkey == KBKEY_F11 && window && !window->grab_input )
	{
		// TODO: window->title can be null.
		char* new_title;
		if ( 0 <= asprintf(&new_title, "%s%s", window->title, grab_inputbed_string) )
		{
			window->grab_input = true;
			free(window->title);
			window->title = new_title;
			window_render_frame(window);
		}
		return;
	}
	if ( kbkey == KBKEY_F12 && window && window->grab_input )
	{
		// TODO: Only remove from title if there.
		size_t grab_inputbed_string_len = strlen(grab_inputbed_string);
		window->title[strlen(window->title) - grab_inputbed_string_len] = '\0';
		window->grab_input = false;
		window_render_frame(window);
		return;
	}

	if ( !window )
		return;

	struct event_keyboard event;
	event.window_id = display->active_window->window_id;
	event.codepoint = codepoint;

	struct display_packet_header header;
	header.message_id = EVENT_KEYBOARD;
	header.message_length = sizeof(event);

	assert(window->connection);

	connection_schedule_transmit(window->connection, &header, sizeof(header));
	connection_schedule_transmit(window->connection, &event, sizeof(event));
}

void display_mouse_event(struct display* display, uint8_t byte)
{
	if ( display->mouse_byte_count == 0 && !(byte & MOUSE_ALWAYS_1) )
		return;
	if ( display->mouse_byte_count < MOUSE_PACKET_SIZE )
		display->mouse_bytes[display->mouse_byte_count++] = byte;
	if ( display->mouse_byte_count < MOUSE_PACKET_SIZE )
		return;
	display->mouse_byte_count = 0;
	uint8_t* bytes = display->mouse_bytes;

	int xm = MOUSE_X(bytes);
	int ym = MOUSE_Y(bytes);

	int old_pointer_x = display->pointer_x;
	int old_pointer_y = display->pointer_y;

	if ( xm*xm + ym*ym >= 2*2 )
	{
		xm *= 2;
		ym *= 2;
	}
	else if ( xm*xm + ym*ym >= 5*5 )
	{
		xm *= 3;
		ym *= 3;
	}
	display->pointer_x += xm;
	display->pointer_y += ym;

	if ( display->pointer_x < 0 )
		display->pointer_x = 0;
	if ( display->pointer_y < 0 )
		display->pointer_y = 0;
	if ( display->screen_width <= (size_t) display->pointer_x )
		display->pointer_x = display->screen_width;
	if ( display->screen_height <= (size_t) display->pointer_y )
		display->pointer_y = display->screen_height;
	xm = display->pointer_x - old_pointer_x;
	ym = display->pointer_y - old_pointer_y;

	struct window* window;
	for ( window = display->top_window; window; window = window->below_window )
	{
		if ( display->mouse_state != MOUSE_STATE_NONE )
			break;
		int grace = RESIZE_GRACE;
		if ( window->window_state == WINDOW_STATE_MAXIMIZED )
			grace = 0;
		if ( old_pointer_x < window->left - grace )
			continue;
		if ( old_pointer_y < window->top - grace )
			continue;
		if ( old_pointer_x > window->left + (ssize_t) window->width + grace )
			continue;
		if ( old_pointer_y > window->top + (ssize_t) window->height + grace)
			continue;
		break;
	}
	if ( !window )
		return;

	ssize_t window_pointer_x = display->pointer_x - window->left;
	ssize_t window_pointer_y = display->pointer_y - window->top;

	if ( bytes[0] & MOUSE_BUTTON_LEFT )
	{
		display_set_active_window(display, window);
		if ( display->mouse_state == MOUSE_STATE_NONE )
		{
			// TODO: Stay in state until mouse release.
			if ( display->key_lalt ||
			     (0 <= window_pointer_x &&
			      window_pointer_x < (ssize_t) window->width &&
			      0 <= window_pointer_y &&
			      window_pointer_y <= (ssize_t) TITLE_HEIGHT) )
				display->mouse_state = MOUSE_STATE_TITLE_MOVE;
			else if ( window_pointer_x < 0 && window_pointer_y < 0 )
				display->mouse_state = MOUSE_STATE_RESIZE_TOP_LEFT;
			else if ( window_pointer_x < 0 &&
			          0 <= window_pointer_y &&
			          window_pointer_y < (ssize_t) window->height )
				display->mouse_state = MOUSE_STATE_RESIZE_LEFT;
			else if ( window_pointer_x < 0 &&
			          (ssize_t) window->height <= window_pointer_y )
				display->mouse_state = MOUSE_STATE_RESIZE_BOTTOM_LEFT;
			else if ( 0 <= window_pointer_x &&
			          window_pointer_x < (ssize_t) window->width &&
			          window_pointer_y < 0 )
				display->mouse_state = MOUSE_STATE_RESIZE_TOP;
			else if ( 0 <= window_pointer_x &&
			          window_pointer_x < (ssize_t) window->width &&
			          (ssize_t) window->height < window_pointer_y )
				display->mouse_state = MOUSE_STATE_RESIZE_BOTTOM;
			else if ( (ssize_t) window->width <= window_pointer_x &&
			          window_pointer_y < 0 )
				display->mouse_state = MOUSE_STATE_RESIZE_TOP_RIGHT;
			else if ( (ssize_t) window->width < window_pointer_x &&
			          0 <= window_pointer_y &&
			          window_pointer_y < (ssize_t) window->height )
				display->mouse_state = MOUSE_STATE_RESIZE_RIGHT;
			else if ( (ssize_t) window->width <= window_pointer_x &&
			          (ssize_t) window->height <= window_pointer_y )
				display->mouse_state = MOUSE_STATE_RESIZE_BOTTOM_RIGHT;
		}
		if ( xm || ym )
		{
			switch ( display->mouse_state )
			{
			case MOUSE_STATE_NONE: break;
			case MOUSE_STATE_TITLE_MOVE:
				if ( window->window_state != WINDOW_STATE_REGULAR )
					window_restore(window);
				window_move(window, window->left + xm, window->top + ym);
				break;
			case MOUSE_STATE_RESIZE_TOP_LEFT:
				window_drag_resize(window, xm, ym, -xm, -ym);
				break;
			case MOUSE_STATE_RESIZE_LEFT:
				window_drag_resize(window, xm, 0, -xm, 0);
				break;
			case MOUSE_STATE_RESIZE_BOTTOM_LEFT:
				window_drag_resize(window, xm, 0, -xm, ym);
				break;
			case MOUSE_STATE_RESIZE_TOP:
				window_drag_resize(window, 0, ym, 0, -ym);
				break;
			case MOUSE_STATE_RESIZE_BOTTOM:
				window_drag_resize(window, 0, 0, 0, ym);
				break;
			case MOUSE_STATE_RESIZE_TOP_RIGHT:
				window_drag_resize(window, 0, ym, xm, -ym);
				break;
			case MOUSE_STATE_RESIZE_RIGHT:
				window_drag_resize(window, 0, 0, xm, 0);
				break;
			case MOUSE_STATE_RESIZE_BOTTOM_RIGHT:
				window_drag_resize(window, 0, 0, xm, ym);
				break;
			}
		}

		// TODO: Leave mouse state if the top window closes.
		// TODO: Leave mouse state if the top window is switched.
	}
	else
	{
		display->mouse_state = MOUSE_STATE_NONE;
	}
}

void display_on_resolution_change(struct display* display, size_t width, size_t height)
{
	if ( display->screen_width == width && display->screen_height == height )
		return;
	display->screen_width = width;
	display->screen_height = height;
	display->pointer_x = width / 2;
	display->pointer_y = height / 2;
	for ( struct window* window = display->bottom_window; window; window = window->above_window )
		window_on_display_resolution_change(window, display);
}
