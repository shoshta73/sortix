/*
 * Copyright (c) 2012 Jonas 'Sortie' Termansen.
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
#include <error.h>
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
				errx(1, "unknown option -- '%c'\n", c);
			}
		}
		else
			errx(1, "unknown option: %s\n", arg);
	}
	size_t bufsize = 32;
	char* buf = (char*) malloc(bufsize);
	if ( !buf )
		error(1, errno, "malloc");
	for ( int i = 1; i < argc; i++ )
	{
		ssize_t ret;
retry:
		ret = kernelinfo(argv[i], buf, bufsize);
		if ( ret < 0 )
		{
			if ( errno == EINVAL )
			{
				error(1, 0, "%s: No such kernel string", argv[i]);
			}
			error(1, errno, "kernelinfo(\"%s\")", argv[i]);
		}
		if ( ret )
		{
			buf = (char*) realloc(buf, ret);
			if ( !buf )
				error(1, errno, "realloc");
			bufsize = ret;
			goto retry;
		}
		printf("%s\n", buf);
	}
	return 0;
}
