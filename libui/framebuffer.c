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
 * framebuffer.c
 * Framebuffer functions.
 */

#include <stddef.h>
#include <stdint.h>

#include "framebuffer.h"
#include "pixel.h"

struct framebuffer framebuffer_crop(struct framebuffer fb,
                                    size_t left,
                                    size_t top,
                                    size_t width,
                                    size_t height)
{
	// Crop the framebuffer horizontally.
	if ( fb.xres < left )
		left = fb.xres;
	fb.buffer += left;
	fb.xres -= left;
	if ( width < fb.xres )
		fb.xres = width;

	// Crop the framebuffer vertically.
	if ( fb.yres < top )
		top = fb.yres;
	fb.buffer += top * fb.pitch;
	fb.yres -= top;
	if ( height < fb.yres )
		fb.yres = height;

	return fb;
}

void framebuffer_copy_to_framebuffer(const struct framebuffer dst,
                                     const struct framebuffer src)
{
	for ( size_t y = 0; y < src.yres; y++ )
		for ( size_t x = 0; x < src.xres; x++ )
			framebuffer_set_pixel(dst, x, y, framebuffer_get_pixel(src, x, y));
}

void framebuffer_copy_to_framebuffer_blend(const struct framebuffer dst,
                                           const struct framebuffer src)
{
	for ( size_t y = 0; y < src.yres; y++ )
	{
		for ( size_t x = 0; x < src.xres; x++ )
		{
			uint32_t bg = framebuffer_get_pixel(dst, x, y);
			uint32_t fg = framebuffer_get_pixel(src, x, y);
			framebuffer_set_pixel(dst, x, y, blend_pixel(bg, fg));
		}
	}
}
