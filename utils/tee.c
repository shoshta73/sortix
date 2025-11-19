/*
 * Copyright (c) 2016 Nicholas De Nova.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
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
 * tee.c
 * Write stdin to stdout and the specified output files.
 */

#include <err.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
	bool append = false;
	bool ignore_interrupts = false;

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
			while ( (c = *++arg) )
				switch ( c )
				{
				case 'a': append = true; break;
				case 'i': ignore_interrupts = true; break;
				default:
					errx(1, "unknown option -- '%c'", c);
				}
		}
		else if ( !strcmp(arg, "--append") )
			append = true;
		else if ( !strcmp(arg, "--ignore-interrupts") )
			ignore_interrupts = true;
		else
			errx(1, "unknown option: %s", arg);
	}

	compact_arguments(&argc, &argv);

	if ( ignore_interrupts )
		signal(SIGINT, SIG_IGN);

	int files_count = argc - 1;
	char buffer[65536];
	bool ok = true;
	bool stdout_ok = true;

	#if defined(__GNUC__) && 7 <= __GNUC__
	#pragma GCC diagnostic ignored "-Walloc-size-larger-than="
	#endif
	int* fds = calloc(files_count, sizeof(*fds));
	if ( !fds )
		err(1, "malloc");

	for ( int i = 0; i < files_count; i++ )
	{
		int oflags = O_CREAT | O_WRONLY | (append ? O_APPEND : O_TRUNC);
		fds[i] = open(argv[i + 1], oflags, 0666);
		if ( fds[i] < 0 )
		{
			warn("%s", argv[i + 1]);
			ok = false;
		}
	}

	while ( true )
	{
		ssize_t bytes_read = read(0, buffer, sizeof(buffer));
		if ( bytes_read < 0 )
			err(1, "stdin");
		if ( bytes_read == 0 )
			break;
		ssize_t stdout_written = 0;
		while ( stdout_written < bytes_read )
		{
			ssize_t stdout_left = bytes_read - stdout_written;
			ssize_t stdout_amount;

			if ( stdout_ok )
			{
				stdout_amount = write(1, buffer + stdout_written, stdout_left);
				if ( stdout_amount < 0 )
				{
					warn("stdout");
					stdout_ok = false;
					ok = false;
				}
			}

			if ( !stdout_ok )
				stdout_amount = stdout_left;

			for ( int i = 0; i < files_count; i++ )
			{
				if ( fds[i] < 0 )
					continue;

				ssize_t bytes_written = 0;
				while ( bytes_written < stdout_amount )
				{
					char* outgoing = buffer + stdout_written + bytes_written;
					size_t left = stdout_amount - bytes_written;
					ssize_t amount = write(fds[i], outgoing, left);
					if ( amount < 0 )
					{
						warn("%s", argv[i + 1]);
						close(fds[i]);
						fds[i] = -1;
						ok = false;
						break;
					}
					bytes_written += amount;
				}
			}
			stdout_written += stdout_amount;
		}
	}

	return ok ? 0 : 1;
}
