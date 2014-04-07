/*
 * Copyright (c) 2014, 2015, 2016 Jonas 'Sortie' Termansen.
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
 * damage-rect.c
 * Damage rectangles.
 */

#include <stddef.h>

#include "damage-rect.h"

struct damage_rect damage_rect_add(struct damage_rect a, struct damage_rect b)
{
	if ( !a.width || !a.height )
		return b;
	if ( !b.width || !b.height )
		return a;
	if ( a.left < b.left )
		b.width += b.left - a.left, b.left = a.left;
	if ( b.width < a.width )
		b.width = a.width;
	if ( a.top < b.top )
		b.height += b.top - a.top, b.top = a.top;
	if ( b.height < a.height )
		b.height = a.height;
	return b;
}
