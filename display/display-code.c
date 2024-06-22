/*
 * Copyright (c) 2014-2016, 2018, 2022-2024 Jonas 'Sortie' Termansen.
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
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <timespec.h>

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
	display->redraw = true;
	display->running = true;
}

static int get_init_exit_plan(void)
{
	FILE* fp = popen("/sbin/service default exit-code", "r");
	if ( !fp )
		return -1;
	int result = -1;
	char buffer[sizeof(int) * 3];
	if ( fgets(buffer, sizeof(buffer), fp) && buffer[0] )
		result = atoi(buffer);
	pclose(fp);
	return result;
}

void display_exit(struct display* display, int exit_code)
{
	display->running = false;
	display->exit_code = exit_code;
	int plan = exit_code == -1 ? get_init_exit_plan() : exit_code;
	display->announcement = "Exiting...";
	if ( plan == 0 )
		display->announcement = "Powering off...";
	else if ( plan == 1 )
		display->announcement = "Rebooting...";
	else if ( plan == 2 )
		display->announcement = "Halting...";
	else if ( plan == 3 )
		display->announcement = "Reinitializing operating system...";
	else if ( getenv("LOGIN_PID") ) // TODO: display -l?
	{
		intmax_t login_pid = strtoimax(getenv("LOGIN_PID"), NULL, 10);
		if ( login_pid == getppid() )
			display->announcement = "Logging out...";
	}
	display->redraw = true;
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

	display_schedule_redraw(display);
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

	display_schedule_redraw(display);
}

void display_unlink_window_removal(struct display* display,
                                   struct window* window)
{
	assert_is_well_formed_display_list(display);

	if ( display->tab_candidate == window )
		if ( !(display->tab_candidate = window->below_window) )
			if ( (display->tab_candidate = display->top_window) == window )
				display->tab_candidate = NULL;

	if ( display->active_window == window )
		display->active_window = NULL;

	window->focus = false;
	display_schedule_redraw(display);

	assert_is_well_formed_display_list(display);

	display_unlink_window(display, window);

	assert_is_well_formed_display_list(display);
}

void display_unmark_active_window(struct display* display,
                                  struct window* window)
{
	assert(display->active_window == window);
	window->focus = false;
	display->active_window = NULL;
	window_render_frame(window);
	window_unsend_keys(window);
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

void display_change_active_window(struct display* display,
                                  struct window* window)
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

union c { struct { uint8_t b; uint8_t g; uint8_t r; }; uint32_t v; };

static void wallpaper(struct framebuffer fb)
{
	static uint32_t s;
	static uint32_t t;
	static bool seeded = false;
	if ( !seeded )
	{
		s = arc4random();
		t = arc4random();
		seeded = true;
	}
	for ( size_t y = 0; y < fb.yres; y++ )
	{
		for ( size_t x = 0; x < fb.xres; x++ )
		{
			uint32_t r = 3793 * x + 6959 * y + 1889 * t + 7901 * s;
			r ^= (5717 * x * 2953 * y) ^ s ^ t;
			r = (r >> 24) ^ (r >> 16) ^ (r >> 8) ^ r;
			union c c;
			if ( x && (r & 0x3) == 2 )
				c.v = framebuffer_get_pixel(fb, x - 1, y);
			else if ( y && (r & 0x3) == 1 )
				c.v = framebuffer_get_pixel(fb, x, y - 1);
			else if ( x && y )
				c.v = framebuffer_get_pixel(fb, x - 1, y - 1);
			else
			{
				c.v = t;
				c.r = (c.r & 0xc0) | (r >> 0 & 0x3f);
				c.g = (c.g & 0xc0) | (r >> 4 & 0x3f);
				c.b = (c.b & 0xc0) | (r >> 8 & 0x3f);
			}
			if ( (r & 0xf0) == 0x10 && c.r ) c.r--;
			if ( (r & 0xf0) == 0x20 && c.g ) c.g--;
			if ( (r & 0xf0) == 0x30 && c.b ) c.b--;
			if ( (r & 0xf0) == 0x40 && c.r != 255 ) c.r++;
			if ( (r & 0xf0) == 0x50 && c.g != 255 ) c.g++;
			if ( (r & 0xf0) == 0x60 && c.b != 255 ) c.b++;
			union c tc = {.v = t};
			if ( c.r && c.r - tc.r > (int8_t) (r >> 0) + 64 ) c.r--;
			if ( c.r != 255 && tc.r - c.r > (int8_t) (r >> 4) + 240 ) c.r++;
			if ( c.g && c.g - tc.g > (int8_t) (r >> 8) + 64) c.g--;
			if ( c.g != 255 && tc.g - c.g > (int8_t) (r >> 12) + 240 ) c.g++;
			if ( c.b && c.b - tc.b > (int8_t) (r >> 16) + 64 ) c.b--;
			if ( c.b != 255 && tc.b - c.b > (int8_t) (r >> 20) + 240 ) c.b++;
			framebuffer_set_pixel(fb, x, y, c.v);
		}
	}
}

static void display_render_exit(struct display* display, struct framebuffer fb)
{
	for ( int yoff = -1; yoff <= 1; yoff++ )
	{
		for ( int xoff = -1; xoff <= 1; xoff++ )
		{
			struct framebuffer msgfb = fb;
			int y = (fb.yres - FONT_HEIGHT) / 2 + yoff;
			msgfb = framebuffer_cut_top_y(msgfb, y);
			int w = strlen(display->announcement) * (FONT_WIDTH+1);
			int x = (fb.xres - w) / 2 + xoff;
			msgfb = framebuffer_cut_left_x(msgfb, x);
			render_text(msgfb, display->announcement, make_color_a(0, 0, 0, 64));
		}
	}

	struct framebuffer msgfb = fb;
	int y = (fb.yres - FONT_HEIGHT) / 2;
	msgfb = framebuffer_cut_top_y(msgfb, y);
	int w = strlen(display->announcement) * (FONT_WIDTH+1);
	int x = (fb.xres - w) / 2;
	msgfb = framebuffer_cut_left_x(msgfb, x);
	render_text(msgfb, display->announcement, make_color(255, 255, 255));
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

#if 0
	uint32_t bg_color = make_color(0x89 * 2/3, 0xc7 * 2/3, 0xff * 2/3);
	for ( size_t y = 0; y < damage_rect.height; y++ )
		for ( size_t x = 0; x < damage_rect.width; x++ )
			framebuffer_set_pixel(fb, damage_rect.left + x, damage_rect.top + y,
			                      bg_color);
#endif

	framebuffer_copy_to_framebuffer(fb, display->wallpaper);

	if ( display->announcement )
	{
		display_render_exit(display, fb);
		return;
	}

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
			winfb = framebuffer_crop(winfb, -window->left, 0,
			                         winfb.xres, winfb.yres);
		}
		else
			winfb_left = window->left;

		if ( window->top < 0 )
		{
			winfb_top = 0;
			winfb = framebuffer_crop(winfb, 0, -window->top,
			                         winfb.xres, winfb.yres);
		}
		else
			winfb_top = window->top;

		size_t winfb_width = winfb.xres;
		size_t winfb_height = winfb.yres;

#if 0
		if ( winfb_left < damage_rect.left &&
		     winfb_width < damage_rect.left - winfb_left )
			continue;
		if ( winfb_left < damage_rect.left  )
		{
			winfb_left = damage_rect.left;
			winfb_width -= damage_rect.left - winfb_left;
		}
#endif

		struct framebuffer fb_dst =
			framebuffer_crop(fb, winfb_left, winfb_top,
			                 winfb_width, winfb_height);

		framebuffer_copy_to_framebuffer_blend(fb_dst, winfb);
	}

	const char* cursor_text = NULL;
	switch ( display->mouse_state )
	{
	case MOUSE_STATE_NONE: break;
	case MOUSE_STATE_IGNORE: break;
	case MOUSE_STATE_BUTTON_PRESS: break;
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
		arrow_render = framebuffer_crop(arrow_render, -pointer_x, 0,
		                                arrow_render.xres, arrow_render.yres);
		pointer_x = 0;
	}
	if ( pointer_y < 0 )
	{
		arrow_render = framebuffer_crop(arrow_render, 0, -pointer_y,
		                                arrow_render.xres, arrow_render.yres);
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

void display_schedule_redraw(struct display* display)
{
	display->redraw = true;
}

void display_render(struct display* display)
{
	if ( !display->redraw )
		return;
	display->redraw = false;

	struct dispmsg_get_crtc_mode get_mode_msg = {0};
	get_mode_msg.msgid = DISPMSG_GET_CRTC_MODE;
	get_mode_msg.device = display->display.device;
	get_mode_msg.connector = display->display.connector;
	if ( dispmsg_issue(&get_mode_msg, sizeof(get_mode_msg)) != 0 )
		err(1, "dispmsg_issue: dispmsg_get_crtc_mode");
	struct dispmsg_crtc_mode mode = get_mode_msg.mode;

	if ( !(mode.control & DISPMSG_CONTROL_VALID) )
		errx(1, "No valid video mode was set");
	if ( mode.control & DISPMSG_CONTROL_VGA )
		errx(1, "A VGA text mode was set");
	if ( mode.fb_format != 32 )
		errx(1, "A 32-bit video mode wasn't set");

	size_t framebuffer_length = mode.view_xres * mode.view_yres;
	size_t framebuffer_size = sizeof(uint32_t) * framebuffer_length;

	if ( display->fb_size != framebuffer_size )
	{
		display->fb.buffer = realloc(display->fb.buffer, framebuffer_size);
		display->fb.xres = mode.view_xres;
		display->fb.yres = mode.view_yres;
		display->fb.pitch = mode.view_xres;
		display->fb_size = framebuffer_size;
	}

	if ( display->wallpaper_size != framebuffer_size )
	{
		display->wallpaper.buffer =
			realloc(display->wallpaper.buffer, framebuffer_size);
		display->wallpaper.xres = mode.view_xres;
		display->wallpaper.yres = mode.view_yres;
		display->wallpaper.pitch = mode.view_xres;
		display->wallpaper_size = framebuffer_size;
	}

	display_on_resolution_change(display, mode.view_xres, mode.view_yres);

	display_composit(display, display->fb);

	struct dispmsg_write_memory write_memory_msg = {0};
	write_memory_msg.msgid = DISPMSG_WRITE_MEMORY;
	write_memory_msg.device = display->display.device;
	write_memory_msg.size = framebuffer_size;
	write_memory_msg.src = (uint8_t*) display->fb.buffer;
	if ( dispmsg_issue(&write_memory_msg, sizeof(write_memory_msg)) != 0 )
		err(1, "dispmsg_issue: dispmsg_write_memory");
}

void display_keyboard_event(struct display* display, uint32_t codepoint)
{
	struct window* window = display->active_window;

	int kbkey = KBKEY_DECODE(codepoint);
	int abskbkey = kbkey < 0 ? -kbkey : kbkey;

	if ( !kbkey && display->codepoint_discard )
		return;
	else if ( kbkey < 0 )
		display->codepoint_discard = false;

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
			display_exit(display, -1);
		if ( display->key_lctrl && display->key_lalt && kbkey == KBKEY_T )
		{
			if ( !fork() )
			{
				sigset_t sigterm;
				sigemptyset(&sigterm);
				sigaddset(&sigterm, SIGTERM);
				sigprocmask(SIG_UNBLOCK, &sigterm, NULL);
				execlp("terminal", "terminal", (char*) NULL);
				_exit(127);
			}
			display->codepoint_discard = true;
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
			display->codepoint_discard = true;
			window_quit(window);
			return;
		}

		if ( display->key_lalt && kbkey == KBKEY_F10 )
		{
			display->codepoint_discard = true;
			window_toggle_maximized(display->active_window);
			return;
		}

		if ( display->key_lalt && kbkey == KBKEY_TAB )
		{
			display->codepoint_discard = true;
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
				display->codepoint_discard = true;
				window_tile_leftward(window);
				return;
			}
			if ( kbkey == KBKEY_RIGHT )
			{
				display->codepoint_discard = true;
				window_tile_rightward(window);
				return;
			}
			if ( kbkey == KBKEY_UP )
			{
				display->codepoint_discard = true;
				window_tile_up(window);
				return;
			}
			if ( kbkey == KBKEY_DOWN )
			{
				display->codepoint_discard = true;
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
		if ( 0 <= asprintf(&new_title, "%s%s", window->title,
		                   grab_inputbed_string) )
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

	window_send_key(display->active_window, codepoint);
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
	if ( xm || ym )
		display_schedule_redraw(display);

	bool clipped_edge = false;
	if ( display->pointer_x < 0 )
	{
		display->pointer_x = 0;
		clipped_edge = true;
	}
	if ( display->pointer_y < 0 )
	{
		display->pointer_y = 0;
		clipped_edge = true;
	}
	if ( display->screen_width < (size_t) display->pointer_x )
	{
		display->pointer_x = display->screen_width;
		clipped_edge = true;
	}
	if ( display->screen_height < (size_t) display->pointer_y )
	{
		display->pointer_y = display->screen_height;
		clipped_edge = true;
	}
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

	if ( display->active_window != window )
	{
		if ( bytes[0] & (MOUSE_BUTTON_LEFT | MOUSE_BUTTON_MIDDLE |
		                 MOUSE_BUTTON_RIGHT) )
		{
			// TODO: Exit mouse from the current window.
			display_set_active_window(display, window);
		}
		else
			return;
	}

	bool maximized = window->window_state != WINDOW_STATE_REGULAR;

	int b2 = 2;
	int t0 = TITLE_HEIGHT;

	size_t border_width = maximized ? 0 : b2 + 1;
	size_t button_area_height = maximized ? t0 : t0 - (b2 + 1);
	size_t button_area_width = button_area_height;
	size_t button_area_top = maximized ? 0 : b2;
	ssize_t buttons_x = window->width - border_width - button_area_width*3 + 1;

	bool mouse_on_title = 0 <= window_pointer_x &&
	                      window_pointer_x < (ssize_t) window->width &&
	                      0 <= window_pointer_y &&
	                      window_pointer_y <= (ssize_t) TITLE_HEIGHT;

	for ( size_t n = 0; n < 3; n++ )
	{
		ssize_t bottom = button_area_top + button_area_height;
		ssize_t left = button_area_width * n;
		ssize_t right = button_area_width * (n + 1);
		if ( (ssize_t) button_area_top <= window_pointer_y &&
		     window_pointer_y <= bottom &&
		     left <= window_pointer_x - buttons_x &&
		     window_pointer_x - buttons_x < right )
		{
			if ( display->mouse_state == MOUSE_STATE_NONE &&
			     (bytes[0] & MOUSE_BUTTON_LEFT) )
			{
				display->mouse_state = MOUSE_STATE_BUTTON_PRESS;
				window->button_states[n] = BUTTON_STATE_PRESSED;
				window_render_frame(window);
			}
			else if ( display->mouse_state == MOUSE_STATE_BUTTON_PRESS &&
			          window->button_states[n] == BUTTON_STATE_PRESSED &&
			          !(bytes[0] & MOUSE_BUTTON_LEFT) )
			{
				window->button_states[n] = BUTTON_STATE_NORMAL;
				window_render_frame(window);
				switch ( n )
				{
				case 0: /* TODO: Minimize. */ break;
				case 1: window_toggle_maximized(window); break;
				case 2: window_quit(window); break;
				}
				return;
			}
			else if ( display->mouse_state == MOUSE_STATE_NONE &&
			          window->button_states[n] != BUTTON_STATE_HOVER )
			{
				window->button_states[n] = BUTTON_STATE_HOVER;
				window_render_frame(window);
			}
		}
		else if ( window->button_states[n] != BUTTON_STATE_NORMAL )
		{
			window->button_states[n] = BUTTON_STATE_NORMAL;
			if ( display->mouse_state != MOUSE_STATE_NONE )
				display->mouse_state = MOUSE_STATE_IGNORE;
			window_render_frame(window);
		}
	}

	struct timespec double_click = timespec_make(0, 500000000);

	if ( bytes[0] & MOUSE_BUTTON_LEFT )
	{
		if ( (display->mouse_state == MOUSE_STATE_NONE) )
		{
			// TODO: Stay in state until mouse release.
			if ( display->key_lalt )
				display->mouse_state = MOUSE_STATE_TITLE_MOVE;
			else if ( mouse_on_title && window_pointer_x < buttons_x )
			{
				struct timespec now;
				clock_gettime(CLOCK_MONOTONIC, &now);
				struct timespec elapsed =
					timespec_sub(now, window->title_click_time);
				if ( 0 <= window->title_click_time.tv_sec &&
				     timespec_le(elapsed, double_click) )
				{
					display->mouse_state = MOUSE_STATE_IGNORE;
					window_toggle_maximized(window);
				}
				else
				{
					// TODO: Reset this if clicked anywhere else or if the
					//       active window changes.
					window->title_click_time = now;
					display->mouse_state = MOUSE_STATE_TITLE_MOVE;
				}
			}
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
			if ( display->mouse_state != MOUSE_STATE_NONE &&
			     display->mouse_state != MOUSE_STATE_IGNORE )
				display_schedule_redraw(display);
		}
		if ( xm || ym )
		{
			bool floating = window->window_state == WINDOW_STATE_REGULAR;
			bool on_edge =
				display->pointer_x == 0 ||
				display->pointer_y == 0 ||
				display->pointer_x == (ssize_t) display->screen_width ||
				display->pointer_y == (ssize_t) display->screen_height;
			switch ( display->mouse_state )
			{
			case MOUSE_STATE_NONE: break;
			case MOUSE_STATE_IGNORE: break;
			case MOUSE_STATE_BUTTON_PRESS: break;
			case MOUSE_STATE_TITLE_MOVE:
				if ( clipped_edge )
				{
					ssize_t x = display->pointer_x;
					ssize_t y = display->pointer_y;
					ssize_t sw = display->screen_width;
					ssize_t sh = display->screen_height;
					ssize_t corner_size = (sw < sh ? sw : sh) / 4;
					if ( x < corner_size && y < corner_size )
						window_tile_top_left(window);
					else if ( sw - x < corner_size && y < corner_size )
						window_tile_top_right(window);
					else if ( x < corner_size && sh - y < corner_size )
						window_tile_bottom_left(window);
					else if ( sw - x < corner_size && sh - y < corner_size )
						window_tile_bottom_right(window);
					else if ( x == 0 )
						window_tile_left(window);
					else if ( x == sw )
						window_tile_right(window);
					else if ( y == 0 )
						window_tile_top(window);
					else if ( y == sh )
						window_tile_bottom(window);
				}
				else if ( floating || !on_edge )
				{
					if ( !floating )
					{
						// The current behaviour of window_restore becomes
						// awkward with tiling gestures. I could change the
						// function itself, especially since this is currently
						// its only callsite, but the old behaviour could be
						// nice for a future untile hotkey. Thus, this hack.
						window_restore(window);
						window->top = display->pointer_y - TITLE_HEIGHT / 2;
						window->left = display->pointer_x - window->width / 2;
					}
					window_move(window, window->left + xm, window->top + ym);
				}
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
	else if ( display->mouse_state != MOUSE_STATE_NONE )
	{
		display->mouse_state = MOUSE_STATE_NONE;
		display_schedule_redraw(display);
	}
}

void display_on_resolution_change(struct display* display, size_t width,
                                  size_t height)
{
	if ( display->screen_width == width && display->screen_height == height )
		return;
	display->screen_width = width;
	display->screen_height = height;
	display->pointer_x = width / 2;
	display->pointer_y = height / 2;
	for ( struct window* window = display->bottom_window;
	      window;
	      window = window->above_window )
		window_on_display_resolution_change(window, display);
	wallpaper(display->wallpaper);
}
