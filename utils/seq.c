/*
 * Copyright (c) 2026 Jonas 'Sortie' Termansen.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF7
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * seq.c
 * Write number sequence-
 */

#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

intmax_t parse(const char* string)
{
	char* end;
	errno = 0;
	int base = 10;
	if ( string[0] == '0' && (string[1] == 'x' || string[1] == 'X') )
		base = 16;
	intmax_t result = strtoimax(string, &end, base);
	if ( errno || *end )
		errx(1, "invalid integer: %s", string);
	return result;
}

int main(int argc, char* argv[])
{
	const char* separator = "\n";
	const char* terminator = "\n";
	bool width = false;

	int opt;
	while ( (opt = getopt(argc, argv, "s:t:w")) != -1 )
	{
		switch ( opt )
		{
		case 's': separator = optarg; break;
		case 't': terminator = optarg; break;
		case 'w': width = true; break;
		default: return 1;
		}
	}

	intmax_t first = 1;
	intmax_t last;
	intmax_t increment = 1;

	if ( argc - optind < 1 )
		errx(1, "expected operand");
	else if ( argc - optind == 1 )
		last = parse(argv[optind]);
	else if ( argc - optind == 2 )
	{
		first = parse(argv[optind + 0]);
		last = parse(argv[optind + 1]);
	}
	else if ( argc - optind == 3 )
	{
		first = parse(argv[optind + 0]);
		increment = parse(argv[optind + 1]);
		last = parse(argv[optind + 2]);
	}
	else
		errx(1, "unexpected extra operand: %s", argv[optind + 3]);

	if ( !increment )
		errx(1, "increment cannot be zero");

	if ( (0 < increment && last < first) ||
	     (0 > increment && last > first) )
		return 0;

	int field_width = 0;
	if ( width )
	{
		int first_width = snprintf(NULL, 0, "%jd", first);
		int last_width = snprintf(NULL, 0, "%jd", last);
		field_width = first_width > last_width ? first_width : last_width;
	}

	intmax_t value = first;
	bool any = false;
	do
	{
		if ( any && fputs(separator, stdout) == EOF )
			break;
		if ( printf("%0*jd", field_width, value) < 0 )
			break;
		if ( __builtin_add_overflow(value, increment, &value) )
			break;
		if ( value != last &&
		     ((0 < increment && last <= value) ||
		      (0 > increment && last >= value)) )
			break;
		any = true;
	} while ( true );

	fputs(terminator, stdout);

	if ( ferror(stdout) || fflush(stdout) == EOF )
		err(1, "stdout");

	return 0;
}
