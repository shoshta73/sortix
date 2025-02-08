/*
 * Copyright (c) 2013, 2021, 2023, 2025 Jonas 'Sortie' Termansen.
 * Copyright (c) 2022 Juhani 'nortti' Krekelä.
 * Copyright (c) 2022 Dennis Wölfing.
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
 * date.c
 * Print or set system date and time.
 */

#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Note: There's no way to discern whether the format string was too long for
// the output buffer or if the output was simply empty, so don't call this
// function with a format that could produce an empty string. E.g. use a prefix
// like '+' and skip it when using the string.
static char* astrftime(const char* format, const struct tm* tm)
{
	size_t format_size = strlen(format) + 1;
	size_t buffer_size = format_size;
	while ( true )
	{
		char* buffer = calloc(buffer_size, 2);
		if ( !buffer )
			return NULL;
		buffer_size *= 2;
		if ( strftime(buffer, buffer_size, format, tm) )
			return buffer;
		free(buffer);
	}
}

int main(int argc, char* argv[])
{
	const char* date = NULL;
	bool set = false;
	const char* reference = NULL;

	int opt;
	while ( (opt = getopt(argc, argv, "d:r:s:u")) != -1 )
	{
		switch ( opt )
		{
		case 'd': date = optarg; break;
		case 'r': reference = optarg; break;
		case 's': date = optarg; set = true; break;
		case 'u':
			if ( setenv("TZ", "UTC0", 1) )
				err(1, "setenv");
			break;
		default: return 1;
		}
	}

	if ( date && reference )
		errx(1, "the -d and -r options are mutually incompatible");
	if ( set && reference )
		errx(1, "the -s and -r options are mutually incompatible");

	const char* format = "+%a %b %e %H:%M:%S %Z %Y";
	if ( 1 <= argc - optind )
	{
		if ( argv[optind][0] != '+' )
			errx(1, "the format specifier must start with a +");
		format = argv[optind];
	}
	if ( 2 <= argc - optind )
		errx(1, "unexpected extra operand: %s", argv[optind + 1]);

	struct timespec moment = {0};
	struct tm tm = {0};

	if ( date )
	{
		char* e;
		if ( ((e = strptime(date, format + 1, &tm)) && !*e) ||
		     ((e = strptime(date, "%a %b %e %H:%M:%S %Z %Y", &tm)) && !*e) ||
		     ((e = strptime(date, "%Y-%m-%dT%H:%M:%SZ", &tm)) && !*e) ||
		     ((e = strptime(date, "%Y-%m-%dT%H:%M:%S%z", &tm)) && !*e) ||
		     ((e = strptime(date, "%Y-%m-%dT%H:%M:%S%Z", &tm)) && !*e) ||
		     ((e = strptime(date, "%Y-%m-%d %H:%M:%S", &tm)) && !*e) ||
		     ((e = strptime(date, "%Y-%m-%d %H:%M:%S %z", &tm)) && !*e) ||
		     ((e = strptime(date, "%Y-%m-%d %H:%M:%S %Z", &tm)) && !*e) )
			moment.tv_sec = timegm(&tm); // TODO: timelocal
		else if ( date[0] == '@' && date[1] )
		{
			errno = 0;
			intmax_t time = strtoimax(date + 1, &e, 10);
			if ( *e || errno || (time_t) time != time )
				errx(1, "invalid datetime: %s", date);
			moment.tv_sec = (time_t) time;
			if ( !gmtime_r(&moment.tv_sec, &tm) )
				err(1, "localtime_r(%ji)", (intmax_t) moment.tv_sec);
		}
		else
			errx(1, "invalid datetime: %s", date);
	}
	else
	{
		if ( reference )
		{
			struct stat st;
			if ( stat(reference, &st) < 0 )
				err(1, "%s", reference);
			moment = st.st_mtim;
		}
		else
			clock_gettime(CLOCK_REALTIME, &moment);
		if ( !localtime_r(&moment.tv_sec, &tm) )
			err(1, "localtime_r(%ji)", (intmax_t) moment.tv_sec);
	}

	if ( set )
	{
		if ( clock_settime(CLOCK_REALTIME, &moment) < 0 )
			err(1, "clock_settime: CLOCK_REALTIME");
		return 0;
	}

	char* string = astrftime(format, &tm);
	if ( !string )
		err(1, "malloc");
	if ( printf("%s\n", string + 1) == EOF )
		err(1, "stdout");
	free(string);
	if ( ferror(stdout) || fflush(stdout) == EOF )
		err(1, "stdout");
	return 0;
}
