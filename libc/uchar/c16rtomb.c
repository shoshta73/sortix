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
 * uchar/c16rtomb.c
 * Convert a UTF-16 character to a multibyte sequence.
 */

#include <errno.h>
#include <uchar.h>
#include <wchar.h>

size_t c16rtomb(char* restrict s, char16_t c16, mbstate_t* restrict ps)
{
	static mbstate_t static_ps;
	if ( !ps )
		ps = &static_ps;
	if ( !s )
		c16 = u'\0';
	wchar_t wc = (wchar_t) c16;
	// Expect the next UTF-16 surrogate code unit if we just saw the first.
	if ( ps->length == 16 )
	{
		if ( c16 < 0xDC00 || 0xDFFF < c16 )
			return errno = EILSEQ, (size_t) -1;
		wc = 0x10000 + (ps->wch | (c16 - 0xDC00));
		ps->length = 0;
		ps->wch = 0;
	}
	// Delay decoding if we encounter the first UTF-16 surrogate code unit.
	else if ( 0xD800 <= c16 && c16 <= 0xDBFF )
	{
		ps->length = 16;
		ps->wch = (c16 - 0xD800) << 10;
		return 0;
	}
	return wcrtomb(s, wc, ps);
}
