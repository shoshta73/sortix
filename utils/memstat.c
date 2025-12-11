/*
 * Copyright (c) 2011, 2022, 2023 Jonas 'Sortie' Termansen.
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
 * memstat.c
 * System memory statistics.
 */

#include <err.h>
#include <memusage.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BYTES 0
#define KIBI 1
#define MEBI 2
#define GIBI 3
#define TEBI 4
#define PEBI 5
#define EXBI 6

static char* format_bytes_amount(uintmax_t num_bytes, int unit, bool raw)
{
	uintmax_t value = num_bytes;
	uintmax_t value_fraction = 0;
	uintmax_t exponent = 1024;
	char suffixes[] = { 'B', 'K', 'M', 'G', 'T', 'P', 'E', 'Z', 'Y' };
	size_t num_suffixes = sizeof(suffixes) / sizeof(suffixes[0]);
	size_t suffix_index = 0;
	while ( (unit < 0 ? exponent <= value : (int) suffix_index < unit) &&
	        suffix_index + 1 < num_suffixes )
	{
		value_fraction = value % exponent;
		value /= exponent;
		suffix_index++;
	}
	char suffix_char = raw ? 0 : suffixes[suffix_index];
	char value_fraction_char = '0' + (value_fraction / (1024 / 10 + 1)) % 10;
	char decimals[3] = {suffix_index ? '.' : 0, value_fraction_char, 0};
	char* result;
	if ( asprintf(&result, "%ju%s%c", value, decimals, suffix_char) < 0 )
		return NULL;
	return result;
}

struct memusage
{
	size_t counter;
	const char* name;
};

static const struct memusage memusages[] =
{
	{MEMUSAGE_TOTAL, "total"},
	{MEMUSAGE_USED, "used"},
	{MEMUSAGE_PURPOSE_USERSPACE, "userspace"},
	{MEMUSAGE_PURPOSE_KERNEL, "kernel"},
	{MEMUSAGE_PURPOSE_FILESYSTEM, "filesystem"},
	{MEMUSAGE_PURPOSE_NETWORK, "network"},
	{MEMUSAGE_PURPOSE_PAGING, "paging"},
	{MEMUSAGE_PURPOSE_DRIVER, "driver"},
	{MEMUSAGE_PURPOSE_PHYSICAL, "physical"},
	{MEMUSAGE_PURPOSE_EXECVE, "execve"},
};

int main(int argc, char* argv[])
{
	bool all = false;
	bool raw = false;
	int unit = -1;

	int opt;
	while ( (opt = getopt(argc, argv, "abegkmprt")) != -1 )
	{
		switch ( opt )
		{
		case 'a': all = true; break;
		case 'b': unit = BYTES; break;
		case 'e': unit = EXBI; break;
		case 'g': unit = GIBI; break;
		case 'k': unit = KIBI; break;
		case 'm': unit = MEBI; break;
		case 'p': unit = PEBI; break;
		case 'r': raw = true; break;
		case 't': unit = TEBI; break;
		default: return 1;
		}
	}

	const size_t MAX_COUNTERS = sizeof(memusages) / sizeof(memusages[0]);
	const struct memusage* usages[MAX_COUNTERS];
	size_t num_counters;
	size_t start_counter = 0;

	if ( all )
	{
		if ( optind < argc )
			errx(1, "extra operand: %s", argv[optind]);
		num_counters = MAX_COUNTERS;
		for ( size_t i = 0; i < num_counters; i++ )
			usages[i] = &memusages[i];
	}
	else if ( optind < argc )
	{
		num_counters = 1;
		start_counter = 1;
		usages[0] = &memusages[0];
		for ( int i = optind; i < argc; i++ )
		{
			if ( num_counters == MAX_COUNTERS )
				errx(1, "too many counters");
			bool found = false;
			for ( size_t n = 0; n < MAX_COUNTERS; n++ )
			{
				if ( strcmp(argv[i], memusages[n].name) != 0 )
					continue;
				usages[num_counters++] = &memusages[n];
				found = true;
				break;
			}
			if ( !found )
				errx(1, "unknown statistic: %s", argv[i]);
		}
	}
	else
	{
		num_counters = 2;
		usages[0] = &memusages[0];
		usages[1] = &memusages[1];
	}

	size_t counts[MAX_COUNTERS];
	for ( size_t i = 0; i < num_counters; i++ )
		counts[i] = usages[i]->counter;

	size_t values[MAX_COUNTERS];
	if ( memusage(counts, values, num_counters) )
		err(1, "memusage");

	if ( raw && unit == -1 )
		unit = BYTES;

	size_t total = values[0];

	size_t usage_width = 0;
	size_t name_width = 0;
	if ( !raw ) {
		for ( size_t i = start_counter; i < num_counters; i++ )
		{
			size_t count = values[i];
			const char* name = usages[i]->name;
			char* usage = format_bytes_amount(count, unit, raw);
			if ( !usage )
				err(1, "malloc");
			size_t usage_len = strlen(usage);
			if ( usage_width < usage_len )
				usage_width = usage_len;
			size_t name_len = strlen(name);
			if ( name_width < name_len )
				name_width = name_len;
			free(usage);
		}
	}

	for ( size_t i = start_counter; i < num_counters; i++ )
	{
		size_t count = values[i];
		const char* name = usages[i]->name;
		char* usage = format_bytes_amount(count, unit, raw);
		if ( !usage )
			err(1, "malloc");
		if ( raw )
		{
			if ( num_counters - start_counter == 1 )
				printf("%s\n", usage);
			else
				printf("%s %s\n", usage, name);
		}
		else
		{
			printf("%*s", (int) usage_width, usage);
			printf(" %-*s", (int) name_width, name);
			unsigned percent = ((uintmax_t) count * 100) / total;
			printf(" %3u%%\n", percent);
		}
		free(usage);
	}

	if ( ferror(stdout) || fflush(stdout) == EOF )
		err(1, "stdout");

	return 0;
}
