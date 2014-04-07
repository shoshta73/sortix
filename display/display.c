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
 * display.c
 * Display server.
 */

#include <err.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "arrow.inc"

#include "display.h"
#include "framebuffer.h"
#include "server.h"

uint32_t arrow_buffer[48 * 48];
struct framebuffer arrow_framebuffer = { 48, arrow_buffer, 48, 48 };

int main(int argc, char* argv[])
{
	(void) argc;
	(void) argv;

	memcpy(arrow_buffer, arrow, sizeof(arrow));

	setlocale(LC_ALL, "");
	setvbuf(stdout, NULL, _IOLBF, 0);

	if ( getpgid(0) != getpid() )
		errx(1, "This program must be run in its own process group");

	struct display display;
	display_initialize(&display);

	struct server server;
	server_initialize(&server, &display);

	// TODO: Add a proper session abstraction.
	if ( !fork() ) { execlp("asteroids", "asteroids", (char*) NULL); _exit(127); }
	if ( !fork() ) { execlp("program", "program", "0", (char*) NULL); _exit(127); }
	if ( !fork() ) { execlp("program", "program", "1", (char*) NULL); _exit(127); }
	if ( !fork() ) { execlp("program", "program", "2", (char*) NULL); _exit(127); }
	if ( !fork() ) { execlp("gears", "gears", (char*) NULL); _exit(127); }
	if ( !fork() ) { execlp("nyan", "nyan", (char*) NULL); _exit(127); }
	if ( !fork() ) { execlp("video-player", "video-player", "/root/charlie-the-unicorn.mp4", (char*) NULL); _exit(127); }

	server_mainloop(&server);

	return 0;
}
