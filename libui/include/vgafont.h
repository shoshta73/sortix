/*
 * Copyright (c) 2014, 2015, 2016, 2017 Jonas 'Sortie' Termansen.
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
 * vgafont.h
 * VGA font rendering.
 */

#ifndef VGAFONT_H
#define VGAFONT_H

#include <stdint.h>

#include "framebuffer.h"

#define FONT_REALWIDTH 8
#define FONT_WIDTH 9
#define FONT_HEIGHT 16
#define FONT_CHARSIZE (FONT_REALWIDTH * FONT_HEIGHT / 8)
#define FONT_NUMCHARS 256

extern uint8_t font[FONT_CHARSIZE * FONT_NUMCHARS];

void load_font(void);
void render_char(struct framebuffer fb, wchar_t c, uint32_t color);
void render_text(struct framebuffer fb, const char* str, uint32_t color);
size_t render_text_columns(const char* str);
size_t render_text_width(const char* str);

#endif
