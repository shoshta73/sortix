/*
 * Copyright (c) 2013, 2015, 2016, 2017, 2023, 2025 Jonas 'Sortie' Termansen.
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
 * tix.c
 * Front end to the Tix package management system.
 */

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char* argv[])
{
	if ( argc == 1 )
		errx(1, "expected collection or command");
	int i = 1;
	const char* collection = NULL;
	if ( strchr(argv[i], '/') )
		collection = argv[i++];

	if ( argc == i )
		errx(1, "expected command");
	const char* command = argv[i++];

	size_t new_argl = 3 + argc - i + 1;
	char** new_argv = calloc(new_argl, sizeof(char*));
	if ( !new_argv )
		err(1, "malloc");

	size_t n = 0;
	if ( asprintf(&new_argv[n++], "tix-%s", command) < 0 )
		err(1, "malloc");
	if ( collection )
	{
		new_argv[n++] = (char*) "-C";
		new_argv[n++] = (char*) collection;
	}
	for ( ; i < argc; i++ )
		new_argv[n++] = argv[i];
	new_argv[n] = NULL;

	execvp(new_argv[0], (char* const*) new_argv);
	err(127, "%s", new_argv[0]);
}
