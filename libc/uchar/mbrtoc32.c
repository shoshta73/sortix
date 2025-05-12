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
 * uchar/mbrtoc32.c
 * Convert a multibyte sequence to a UTF-32 character.
 */

#include <uchar.h>
#include <wchar.h>

size_t mbrtoc32(char32_t* restrict pc32,
                const char* restrict s,
                size_t n,
                mbstate_t* restrict ps)
{
	static mbstate_t static_ps;
	if ( !ps )
		ps = &static_ps;
	wchar_t wc;
	size_t result = mbrtowc(&wc, s, n, ps);
	if ( result == (size_t) -1 || result == (size_t) -2 )
		return result;
	if ( s && pc32 )
		*pc32 = (char32_t) wc;
	return result;
}
