/*
 * Copyright (c) 2014 Jonas 'Sortie' Termansen.
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

#include <err.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <ioleast.h>
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

static void compact_arguments(int* argc, char*** argv)
{
	for ( int i = 0; i < *argc; i++ )
	{
		while ( i < *argc && !(*argv)[i] )
		{
			for ( int n = i; n < *argc; n++ )
				(*argv)[n] = (*argv)[n+1];
			(*argc)--;
		}
	}
}

int main(int argc, char* argv[])
{
	bool list = false;

	setlocale(LC_ALL, "");

	const char* argv0 = argv[0];
	for ( int i = 1; i < argc; i++ )
	{
		const char* arg = argv[i];
		if ( arg[0] != '-' || !arg[1] )
			continue;
		argv[i] = NULL;
		if ( !strcmp(arg, "--") )
			break;
		if ( arg[1] != '-' )
		{
			char c;
			while ( (c = *++arg) ) switch ( c )
			{
			case 'l': list = true; break;
			default:
				fprintf(stderr, "%s: unknown option -- '%c'\n", argv0, c);
				exit(1);
			}
		}
		else if ( !strcmp(arg, "--list") )
			list = true;
		else
		{
			fprintf(stderr, "%s: unknown option: %s\n", argv0, arg);
			exit(1);
		}
	}

	compact_arguments(&argc, &argv);

	if ( list )
	{
		if ( 2 <= argc )
			errx(1, "unexpected extra operand");
		execlp("ls", "ls", "/share/kblayout", (const char*) NULL);
		err(127, "ls");
	}

	const char* tty_path = "/dev/tty";
	int tty_fd = open(tty_path, O_WRONLY);
	if ( tty_fd < 0 )
		error(1, errno, "`%s'", tty_path);
	if ( !isatty(tty_fd) )
		error(1, errno, "`%s'", tty_path);

	if ( argc == 1 )
		error(1, 0, "expected new keyboard layout");

	const char* kblayout_path = argv[1];
	if ( !strchr(kblayout_path, '/') )
	{
		char* new_kblayout_path;
		if ( asprintf(&new_kblayout_path, "/share/kblayout/%s", kblayout_path) < 0 )
			error(1, errno, "asprintf");
		kblayout_path = new_kblayout_path;
	}
	int kblayout_fd = open(kblayout_path, O_RDONLY);
	if ( kblayout_fd < 0 )
		error(1, errno, "`%s'", kblayout_path);

	struct stat kblayout_st;
	if ( fstat(kblayout_fd, &kblayout_st) < 0 )
		error(1, errno, "stat: `%s'", kblayout_path);

	if ( SIZE_MAX < (size_t) kblayout_st.st_size )
		error(1, EFBIG, "`%s'", kblayout_path);

	size_t kblayout_size = (size_t) kblayout_st.st_size;
	unsigned char* kblayout = (unsigned char*) malloc(kblayout_size);
	if ( !kblayout )
		error(1, errno, "malloc");
	if ( readall(kblayout_fd, kblayout, kblayout_size) != kblayout_size )
		error(1, errno, "read: `%s'", kblayout_path);
	close(kblayout_fd);

	if ( tcsetblob(tty_fd, "kblayout", kblayout, kblayout_size) < 0 )
		error(1, errno, "tcsetblob(\"kblayout\", `%s')", kblayout_path);

	free(kblayout);

	close(tty_fd);

	return 0;
}
