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
 * stdlib/getsubopt.c
 * Parse suboptions.
 */

#include <stdlib.h>
#include <string.h>

int getsubopt(char** options_ptr, char* const* keylist_ptr, char** value_ptr)
{
	char* options = *options_ptr;
	size_t comma = strcspn(options, ",");
	if ( options[comma] == ',' )
	{
		*options_ptr = options + comma + 1;
		options[comma] = '\0';
	}
	else
		*options_ptr = options + comma;
	size_t equal = strcspn(options, "=");
	if ( options[equal] == '=' )
	{
		*value_ptr = options + equal + 1;
		options[equal] = '\0';
	}
	else
		*value_ptr = NULL;
	for ( int result = 0; keylist_ptr[result]; result++ )
		if ( !strcmp(options, keylist_ptr[result]) )
			return result;
	return -1;
}
