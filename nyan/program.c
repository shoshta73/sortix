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
 * program.c
 * A colored window.
 */

#include <sys/socket.h>
#include <sys/keycodes.h>
#include <sys/un.h>

#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <error.h>
#include <ioleast.h>
#include <locale.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <display.h>

#include "pixel.h"

uint32_t WINDOW_ID = 0;
uint32_t WINDOW_WIDTH = 0;
uint32_t WINDOW_HEIGHT = 0;

bool need_redraw = true;
bool need_show = true;
bool need_exit = false;

void on_disconnect(void* ctx)
{
	(void) ctx;
	need_exit = true;
}

void on_quit(void* ctx, uint32_t window_id)
{
	(void) ctx;
	if ( window_id != WINDOW_ID )
		return;
	need_exit = true;
}

void on_resize(void* ctx, uint32_t window_id, uint32_t width, uint32_t height)
{
	(void) ctx;
	if ( window_id != WINDOW_ID )
		return;
	need_redraw = true;
	WINDOW_WIDTH = width;
	WINDOW_HEIGHT = height;
}

void on_keyboard(void* ctx, uint32_t window_id, uint32_t codepoint)
{
	(void) ctx;
	if ( window_id != WINDOW_ID )
		return;
	(void) codepoint;
}

int main(int argc, char* argv[])
{
	struct window_description
	{
		const char* title;
		uint32_t color;
		uint32_t width;
		uint32_t height;
	};

	struct window_description descs[] =
	{
		{ "Red Window", make_color(255, 0, 0), 384, 192 },
		{ "Green Window", make_color(0, 255, 0), 400, 200 },
		{ "Blue Window", make_color(0, 0, 255), 384, 256 },
		{ "Transparent Window", make_color_a(255, 255, 255, 128), 600, 384 },
	};

	size_t num_descs = sizeof(descs) / sizeof(descs[0]);
	size_t desc_index = 0;

	if ( 2 <= argc )
		desc_index = strtoul(argv[1], NULL, 10) % num_descs;

	struct window_description* desc = &descs[desc_index];

	setlocale(LC_ALL, "");
	setvbuf(stdout, NULL, _IOLBF, 0);

	struct display_connection* connection = display_connect_default();
	if ( !connection && errno == ECONNREFUSED )
		display_spawn(argc, argv);
	if ( !connection )
		error(1, errno, "Could not connect to display server");

	WINDOW_WIDTH = desc->width;
	WINDOW_HEIGHT = desc->height;

	display_create_window(connection, WINDOW_ID);
	display_resize_window(connection, WINDOW_ID, WINDOW_WIDTH, WINDOW_HEIGHT);
	display_title_window(connection, WINDOW_ID, desc->title);

	while ( !need_exit )
	{
		if ( need_redraw )
		{
			uint32_t* framebuffer = (uint32_t*) malloc(sizeof(uint32_t) * WINDOW_WIDTH * WINDOW_HEIGHT);

			uint32_t color = desc->color;
			for ( size_t y = 0; y < WINDOW_HEIGHT; y++ )
				for ( size_t x = 0; x < WINDOW_WIDTH; x++ )
					framebuffer[y * WINDOW_WIDTH + x] = color;

			display_render_window(connection, WINDOW_ID, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, framebuffer);

			free(framebuffer);

			need_redraw = false;
		}

		if ( need_show )
		{
			display_show_window(connection, WINDOW_ID);
			need_show = false;
		}

		struct display_event_handlers handlers;
		memset(&handlers, 0, sizeof(handlers));
		handlers.disconnect_handler = on_disconnect;
		handlers.quit_handler = on_quit;
		handlers.resize_handler = on_resize;
		handlers.keyboard_handler = on_keyboard;
		display_wait_event(connection, &handlers);
	}

	display_disconnect(connection);

	return 0;
}
