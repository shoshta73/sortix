/*
 * Copyright (c) 2017 Jonas 'Sortie' Termansen.
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
 * hostname.c
 * Write or set the system hostname.
 */

#include <err.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// TODO: After releasing Sortix 1.1, remove this bootstrap compatibility.
#if !defined(HOST_NAME_MAX) && defined(__sortix__)
#include <sortix/limits.h>
#endif

int main(int argc, char* argv[])
{
	const struct option longopts[] =
	{
		{"short", no_argument, NULL, 's'},
		{0, 0, 0, 0}
	};
	const char* opts = "s";
	bool short_hostname = false;

	int opt;
	while ( (opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1 )
	{
		switch ( opt )
		{
		case 's': short_hostname = true; break;
		default: return 1;
		}
	}
	if ( argc - optind > 1 )
		errx(1, "unexpected extra operand");
	if ( argc - optind == 1 )
	{
		if ( short_hostname )
			errx(1, "the -s option is incompatible with setting hostname");
		char* hostname = argv[optind];
		if ( sethostname(hostname, strlen(hostname)) < 0 )
			err(1, "sethostname: %s", hostname);
		return 0;
	}

	char hostname[HOST_NAME_MAX + 1];
	if ( gethostname(hostname, sizeof(hostname)) < 0 )
		err(1, "gethostname");
	if ( short_hostname )
		hostname[strcspn(hostname, ".")] = '\0';
	puts(hostname);

	if ( ferror(stdout) || fflush(stdout) == EOF )
		err(1, "stdout");
	return 0;
}
