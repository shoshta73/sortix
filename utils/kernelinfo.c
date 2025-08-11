/*
 * Copyright (c) 2012, 2016 Jonas 'Sortie' Termansen.
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
 * kernelinfo.c
 * Prints a kernel information string.
 */

#include <sys/kernelinfo.h>

#include <err.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char* argv[])
{
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
			default:
				errx(1, "unknown option -- '%c'", c);
			}
		}
		else
			errx(1, "unknown option: %s", arg);
	}
	size_t size = 32;
	char* buffer = (char*) malloc(size);
	if ( !buffer )
		err(1, "malloc");
	for ( int i = 1; i < argc; i++ )
	{
		while ( true )
		{
			ssize_t needed = kernelinfo(argv[i], buffer, size);
			if ( needed < 0 )
			{
				if ( errno == EINVAL )
					errx(1, "%s: No such kernel information", argv[i]);
				err(1, "kernelinfo: %s", argv[i]);
			}
			if ( 0 < needed )
			{
				size = needed + 1;
				buffer = (char*) realloc(buffer, size);
				if ( !buffer )
					err(1, "malloc");
				continue;
			}
			if ( printf("%s\n", buffer) < 0 )
				err(1, "stdout");

			break;
		}
	}
	if ( ferror(stdout) || fflush(stdout) == EOF )
		err(1, "stdout");
	return 0;
}
