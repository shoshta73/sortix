/*
 * Copyright (c) 2014 Jonas 'Sortie' Termansen.
 * Copyright (c) 2023 Juhani 'nortti' Krekelä.
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
 * chkblayout.c
 * Changes the current keyboard layout.
 */

#include <sys/stat.h>

#include <display.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <ioleast.h>
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

#define CHKBLAYOUT_ID 0

static bool chkblayout_ack_received = false;
static int chkblayout_error;

static void on_ack(void* ctx, uint32_t id, int32_t error)
{
	(void) ctx;
	if ( id != CHKBLAYOUT_ID )
		return;
	chkblayout_error = error;
	chkblayout_ack_received = true;
}

int main(int argc, char* argv[])
{
	bool list = false;

	setlocale(LC_ALL, "");

	const struct option longopts[] =
	{
		{"list", no_argument, NULL, 'l'},
		{0, 0, 0, 0}
	};
	const char* opts = "l";
	int opt;
	while ( (opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1 )
	{
		switch ( opt )
		{
		case 'l': list = true; break;
		default: return 1;
		}
	}

	if ( list )
	{
		if ( 0 < argc - optind )
			errx(1, "unexpected extra operand");
		execlp("ls", "ls", "/share/kblayout", (const char*) NULL);
		err(127, "ls");
	}

	const char* tty_path = "/dev/tty";
	int tty_fd = open(tty_path, O_WRONLY);
	if ( tty_fd < 0 )
		err(1, "%s", tty_path);
	if ( !isatty(tty_fd) )
		err(1, "%s", tty_path);

	if ( argc - optind == 0 )
		errx(1, "expected new keyboard layout");
	if ( 1 < argc - optind )
		errx(1, "unexpected extra operand");

	const char* kblayout_path = argv[optind];
	if ( !strchr(kblayout_path, '/') )
	{
		char* new_kblayout_path;
		if ( asprintf(&new_kblayout_path, "/share/kblayout/%s", kblayout_path) < 0 )
			err(1, "asprintf");
		kblayout_path = new_kblayout_path;
	}
	int kblayout_fd = open(kblayout_path, O_RDONLY);
	if ( kblayout_fd < 0 )
		err(1, "%s", kblayout_path);

	struct stat kblayout_st;
	if ( fstat(kblayout_fd, &kblayout_st) < 0 )
		err(1, "stat: %s", kblayout_path);

	if ( (size_t) kblayout_st.st_size != (uintmax_t) kblayout_st.st_size )
		errc(1, EFBIG, "%s", kblayout_path);

	size_t kblayout_size = (size_t) kblayout_st.st_size;
	unsigned char* kblayout = (unsigned char*) malloc(kblayout_size);
	if ( !kblayout )
		err(1, "malloc");
	if ( readall(kblayout_fd, kblayout, kblayout_size) != kblayout_size )
		err(1, "read: %s", kblayout_path);
	close(kblayout_fd);

	if ( getenv("DISPLAY_SOCKET") )
	{
		struct display_connection* connection = display_connect_default();
		if ( !connection )
			err(1, "Could not connect to display server");

		display_chkblayout(connection, CHKBLAYOUT_ID, kblayout, kblayout_size);

		struct display_event_handlers handlers = {0};
		handlers.ack_handler = on_ack;
		while ( !chkblayout_ack_received )
			display_wait_event(connection, &handlers);

		if ( chkblayout_error )
		{
			errno = chkblayout_error;
			err(1, "tcsetblob: kblayout: %s", kblayout_path);
		}

		display_disconnect(connection);
	}
	else if ( tcsetblob(tty_fd, "kblayout", kblayout, kblayout_size) < 0 )
		err(1, "tcsetblob: kblayout: %s", kblayout_path);

	free(kblayout);

	close(tty_fd);

	return 0;
}
