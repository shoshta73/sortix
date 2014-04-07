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
 * display.h
 * Display client library.
 */

#ifndef INCLUDE_DISPLAY_H
#define INCLUDE_DISPLAY_H

#include <poll.h>
#include <stdint.h>
#include <time.h>

#if defined(__cplusplus)
extern "C" {
#endif

struct display_connection;

int display_spawn(int argc, char** argv);

struct display_connection* display_connect(const char* socket_path);
struct display_connection* display_connect_default(void);
void display_disconnect(struct display_connection* connection);

int display_connection_fd(struct display_connection* connection);

void display_shutdown(struct display_connection* connection, uint32_t code);

void display_create_window(struct display_connection* connection,
                           uint32_t window_id);
void display_destroy_window(struct display_connection* connection,
                            uint32_t window_id);
void display_resize_window(struct display_connection* connection,
                           uint32_t window_id,
                           uint32_t width, uint32_t
                           height);
void display_render_window(struct display_connection* connection,
                           uint32_t window_id,
                           uint32_t left,
                           uint32_t top,
                           uint32_t width,
                           uint32_t height,
                           uint32_t* data);
void display_title_window(struct display_connection* connection,
                           uint32_t window_id,
                           const char* title);
void display_show_window(struct display_connection* connection,
                         uint32_t window_id);
void display_hide_window(struct display_connection* connection,
                           uint32_t window_id);

typedef void (*display_event_disconnect_handler_t)(void*);
typedef void (*display_event_quit_handler_t)(void*, uint32_t);
typedef void (*display_event_resize_handler_t)(void*, uint32_t, uint32_t,
                                               uint32_t);
typedef void (*display_event_keyboard_handler_t)(void*, uint32_t, uint32_t);

struct display_event_handlers
{
	void* context;
	display_event_disconnect_handler_t disconnect_handler;
	display_event_quit_handler_t quit_handler;
	display_event_resize_handler_t resize_handler;
	display_event_keyboard_handler_t keyboard_handler;
};

int display_poll_event(struct display_connection* connection,
                       struct display_event_handlers* handlers);
int display_wait_event(struct display_connection* connection,
                       struct display_event_handlers* handlers);

#if defined(__cplusplus)
} // extern "C"
#endif

#endif
