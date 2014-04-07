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
 * nyan.c
 * Window with animated nyancat.
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
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <timespec.h>
#include <unistd.h>

#include <display.h>
#include <nyan.h>

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
	(void) argc;
	(void) argv;

	setlocale(LC_ALL, "");
	setvbuf(stdout, NULL, _IOLBF, 0);

	struct display_connection* connection = display_connect_default();
	if ( !connection && errno == ECONNREFUSED )
		display_spawn(argc, argv);
	if ( !connection )
		error(1, errno, "Could not connect to display server");

	WINDOW_WIDTH = 600;
	WINDOW_HEIGHT = 600;

	display_create_window(connection, WINDOW_ID);
	display_resize_window(connection, WINDOW_ID, WINDOW_WIDTH, WINDOW_HEIGHT);
	display_title_window(connection, WINDOW_ID, "Nyanyanyanyanyanyanya...");

	struct timespec frame_duration = timespec_make(0, 90 * 1000 * 1000);

	struct timespec last_frame;
	clock_gettime(CLOCK_MONOTONIC, &last_frame);

	int frame_num = 0;

	while ( !need_exit )
	{
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		struct timespec since_last_frame = timespec_sub(now, last_frame);

		if ( !need_redraw && timespec_lt(since_last_frame, frame_duration) )
		{
			struct timespec remainder = timespec_sub(frame_duration, since_last_frame);
			if ( timespec_lt(remainder, timespec_make(0, 10 * 1000 * 1000)) )
				remainder = timespec_make(0, 10 * 1000 * 1000);
			nanosleep(&remainder, NULL);
			continue;
		}

		while ( timespec_le(frame_duration, since_last_frame) )
		{
			if ( !nyan_frames[++frame_num] )
				frame_num = 0;
			need_redraw = true;
			since_last_frame = timespec_sub(since_last_frame, frame_duration);
		}

		if ( need_redraw )
		{
			last_frame = now;

			uint32_t* framebuffer = (uint32_t*) malloc(sizeof(uint32_t) * WINDOW_WIDTH * WINDOW_HEIGHT);

			const char* const* frame = nyan_frames[frame_num];
			for ( size_t y = 0; y < WINDOW_HEIGHT; y++ )
			{
				int yi = y * NYAN_FRAME_HEIGHT / WINDOW_HEIGHT;
				const char* line = frame[yi];
				for ( size_t x = 0; x < WINDOW_WIDTH; x++ )
				{
					int xi = x * NYAN_FRAME_WIDTH / WINDOW_WIDTH;
					char elem = line[xi];
					const uint8_t* cc = nyan_palette[nyan_palette_of_char(elem)];
					framebuffer[y * WINDOW_WIDTH + x] = make_color_a(cc[0], cc[1], cc[2], cc[3]);;
				}
			}

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
		while ( display_poll_event(connection, &handlers) == 0 );
	}

	display_disconnect(connection);

	return 0;
}
