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
 * uchar/mbrtoc16.c
 * Convert a multibyte sequence to a UTF-16 character.
 */

#include <uchar.h>
#include <wchar.h>

size_t mbrtoc16(char16_t* restrict pc16,
                const char* restrict s,
                size_t n,
                mbstate_t* restrict ps)
{
	static mbstate_t static_ps;
	if ( !ps )
		ps = &static_ps;
	// The second surrogate code unit is delivered on the subsequent call.
	if ( ps->length == 16 )
	{
		if ( s && pc16 )
			*pc16 = (char16_t) ps->wch;
		ps->length = 0;
		return (size_t) -3;
	}
	wchar_t wc;
	size_t result = mbrtowc(&wc, s, n, ps);
	if ( result == (size_t) -1 || result == (size_t) -2 )
		return result;
	// U+010000 through U+10FFFF are encoded as a UTF-16 surrogate pair.
	if ( 0x10000 <= wc )
	{
		ps->length = 16;
		// The lower 10 bits go in a code unit in the 0xDC00-0xDFFF range.
		ps->wch = 0xDC00 + ((wc - 0x10000) & 0x3FF);
		// The upper 10 bits go in a code unit in the 0xD800-0xDBFF range.
		wc = 0xD800 + ((wc - 0x10000) >> 10);
	}
	if ( s && pc16 )
		*pc16 = (char16_t) wc;
	return result;
}
