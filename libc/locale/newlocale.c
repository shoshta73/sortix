/*
 * Copyright (c) 2025 Jonas 'Sortie' Termansen.
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
 * locale/setlocale.c
 * Create and modify a locale object.
 */

#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>

static const char* const variables[] =
{
	"LC_COLLATE",
	"LC_CTYPE",
	"LC_MESSAGES",
	"LC_MONETARY",
	"LC_NUMERIC",
	"LC_TIME",
	"LC_ALL",
};

locale_t newlocale(int category_mask, const char* locale, locale_t base)
{
	if ( category_mask < 0 || LC_ALL_MASK < category_mask )
		return errno = EINVAL, NULL;
	if ( !strcmp(locale, "") && getenv(variables[LC_ALL]) )
		locale = getenv(variables[LC_ALL]);
	char* new_locales[LC_ALL] = {0};
	for ( int category = 0; category < LC_ALL; category++ )
	{
		if ( !(category_mask & (1 << category)) )
			continue;
		const char* value = locale;
		if ( !strcmp(value, "") && getenv(variables[category]) )
			value = getenv(variables[category]);
		if ( !strcmp(value, "") )
			value = "C";
		if ( (category_mask & (1 << category)) &&
		     !(new_locales[category] = strdup(locale)) )
		{
			for ( int n = 0; n < category; n++ )
				free(new_locales[n]);
			return NULL;
		}
	}
	if ( !base && !(base = calloc(1, sizeof(*base))) )
	{
		for ( int n = 0; n < LC_ALL; n++ )
			free(new_locales[n]);
		return NULL;
	}
	for ( int category = 0; category < LC_ALL; category++ )
	{
		if ( (category_mask & (1 << category)) )
		{
			free(base->current[category]);
			base->current[category] = new_locales[category];
		}
	}
	return base;
}
