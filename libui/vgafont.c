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
 * vgafont.c
 * VGA font rendering.
 */

#include <err.h>
#include <fcntl.h>
#include <ioleast.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#include "framebuffer.h"
#include "vgafont.h"

static const wchar_t REPLACEMENT_CHARACTER = 0xFFFD;

uint8_t font[FONT_CHARSIZE * FONT_NUMCHARS];

void load_font(void)
{
	int fd = open("/dev/vgafont", O_RDONLY);
	if ( fd < 0 )
		err(1, "/dev/vgafont");
	if ( readall(fd, font, sizeof(font)) != sizeof(font) )
		err(1, "/dev/vgafont");
	close(fd);
}

// https://en.wikipedia.org/wiki/Code_page_437
static inline int map_wide_to_vga_font(wchar_t c)
{
	if ( 32 <= c && c < 127 )
		return (int) c;
	switch ( c )
	{
	case L'☺': return 1;
	case L'☻': return 2;
	case L'♥': return 3;
	case L'♦': return 4;
	case L'♣': return 5;
	case L'♠': return 6;
	case L'•': return 7;
	case L'◘': return 8;
	case L'○': return 9;
	case L'◙': return 10;
	case L'♂': return 11;
	case L'♀': return 12;
	case L'♪': return 13;
	case L'♬': return 14;
	case L'☼': return 15;
	case L'►': return 16;
	case L'◄': return 17;
	case L'↕': return 18;
	case L'‼': return 19;
	case L'¶': return 20;
	case L'§': return 21;
	case L'▬': return 22;
	case L'↨': return 23;
	case L'↑': return 24;
	case L'↓': return 25;
	case L'→': return 26;
	case L'←': return 27;
	case L'∟': return 28;
	case L'↔': return 29;
	case L'▲': return 30;
	case L'▼': return 31;
	case L'⌂': return 127;
	case L'Ç': return 128;
	case L'ü': return 129;
	case L'é': return 130;
	case L'â': return 131;
	case L'ä': return 132;
	case L'à': return 133;
	case L'å': return 134;
	case L'ç': return 135;
	case L'ê': return 136;
	case L'ë': return 137;
	case L'è': return 138;
	case L'ï': return 139;
	case L'î': return 140;
	case L'ì': return 141;
	case L'Ä': return 142;
	case L'Å': return 143;
	case L'É': return 144;
	case L'æ': return 145;
	case L'Æ': return 146;
	case L'ô': return 147;
	case L'ö': return 148;
	case L'ò': return 149;
	case L'û': return 150;
	case L'ù': return 151;
	case L'ÿ': return 152;
	case L'Ö': return 153;
	case L'Ü': return 154;
	case L'¢': return 155;
	case L'£': return 156;
	case L'¥': return 157;
	case L'₧': return 158;
	case L'ƒ': return 159;
	case L'á': return 160;
	case L'í': return 161;
	case L'ó': return 162;
	case L'ú': return 163;
	case L'ñ': return 164;
	case L'Ñ': return 165;
	case L'ª': return 166;
	case L'º': return 167;
	case L'¿': return 168;
	case L'⌐': return 169;
	case L'¬': return 170;
	case L'½': return 171;
	case L'¼': return 172;
	case L'¡': return 173;
	case L'«': return 174;
	case L'»': return 175;
	case L'░': return 176;
	case L'▒': return 177;
	case L'▓': return 178;
	case L'│': return 179;
	case L'┤': return 180;
	case L'╡': return 181;
	case L'╢': return 182;
	case L'╖': return 183;
	case L'╕': return 184;
	case L'╣': return 185;
	case L'║': return 186;
	case L'╗': return 187;
	case L'╝': return 188;
	case L'╜': return 189;
	case L'╛': return 190;
	case L'┐': return 191;
	case L'└': return 192;
	case L'┴': return 193;
	case L'┬': return 194;
	case L'├': return 195;
	case L'─': return 196;
	case L'┼': return 197;
	case L'╞': return 198;
	case L'╟': return 199;
	case L'╚': return 200;
	case L'╔': return 201;
	case L'╩': return 202;
	case L'╦': return 203;
	case L'╠': return 204;
	case L'═': return 205;
	case L'╬': return 206;
	case L'╧': return 207;
	case L'╨': return 208;
	case L'╤': return 209;
	case L'╥': return 210;
	case L'╙': return 211;
	case L'╘': return 212;
	case L'╒': return 213;
	case L'╓': return 214;
	case L'╫': return 215;
	case L'╪': return 216;
	case L'┘': return 217;
	case L'┌': return 218;
	case L'█': return 219;
	case L'▄': return 220;
	case L'▌': return 221;
	case L'▐': return 222;
	case L'▀': return 223;
	case L'α': return 224;
	case L'ß': return 225; /* German sharp S U+00DF */
	case L'β': return 225; /* Greek lowercase beta U+03B2 */
	case L'Γ': return 226;
	case L'π': return 227;
	case L'Σ': return 228; /* Greek uppercase sigma U+03A3 */
	case L'∑': return 228; /* n-ary summation sign U+2211 (replacement) */
	case L'σ': return 229;
	case L'µ': return 230;
	case L'τ': return 231;
	case L'Φ': return 232;
	case L'Θ': return 233;
	case L'Ω': return 234;
	case L'δ': return 235; /* Greek lowercase delta U+03B4 */
	case L'ð': return 235; /* Icelandic lowercase eth U+00F0 (replacement) */
	case L'∂': return 235; /* Partial derivative sign U+2202 (replacement) */
	case L'∞': return 236;
	case L'φ': return 237; /* Greek lowercase phi U+03C6 */
	case L'∅': return 237; /* Empty set sign U+2205 (replacement) */
	case L'ϕ': return 237; /* Greek phi symbol in italics U+03D5 (replacement) */
	case L'⌀': return 237; /* Diameter sign U+2300 (replacement) */
	case L'ø': return 237; /* Latin lowercase O with stroke U+00F8 (replacement) */
	case L'Ø': return 237; /* Latin uppercase O with stroke U+00D8 (replacement) */
	case L'ε': return 238; /* Greek lowercase epsilon U+03B5 */
	case L'∈': return 238; /* Element-of sign U+2208 */
	case L'€': return 238; /* Euro sign U+20AC */
	case L'∩': return 239;
	case L'≡': return 240;
	case L'±': return 241;
	case L'≥': return 242;
	case L'≤': return 243;
	case L'⌠': return 244;
	case L'⌡': return 245;
	case L'÷': return 246;
	case L'≈': return 247;
	case L'°': return 248;
	case L'∙': return 249;
	case L'·': return 250;
	case L'√': return 251;
	case L'ⁿ': return 252;
	case L'²': return 253;
	case L'■': return 254;
	default: return 0 <= c && c < 256 ? c : -1;
	}
}

static const uint8_t font_replacement_character[16] =
{
	0b00000000,
	0b00010000,
	0b00111000,
	0b01000100,
	0b10111010,
	0b10111010,
	0b11110110,
	0b11101110,
	0b11101110,
	0b11111110,
	0b01101100,
	0b00101000,
	0b00010000,
	0b00000000,
	0b00000000,
	0b00000000,
};

static inline const uint8_t* get_character_font(const uint8_t* font, int remap)
{
	if ( remap < 0 )
		return font_replacement_character;
	return font + 16 * remap;
}

void render_char(struct framebuffer fb, wchar_t wc, uint32_t color)
{
	// TODO: Special case the rendering of some block drawing characters like in
	//       the kernel so pstree looks nice.
	int remap = map_wide_to_vga_font(wc);
	const uint8_t* charfont = get_character_font(font, remap);
	uint32_t buffer[FONT_HEIGHT * (FONT_REALWIDTH+1)];
	for ( size_t y = 0; y < FONT_HEIGHT; y++ )
	{
		uint8_t line_bitmap = charfont[y];
		for ( size_t x = 0; x < FONT_REALWIDTH; x++ )
			buffer[y * (FONT_REALWIDTH+1) + x] = line_bitmap & 1U << (7 - x) ? color : 0;
		uint32_t last_color = 0;
		if ( 0xB0 <= remap && remap <= 0xDF && (line_bitmap & 1) )
			last_color = color;
		buffer[y * (FONT_REALWIDTH+1) + 8] = last_color;
	}

	struct framebuffer character_fb;
	character_fb.xres = FONT_WIDTH;
	character_fb.yres = FONT_HEIGHT;
	character_fb.pitch = character_fb.xres;
	character_fb.buffer = buffer;

	framebuffer_copy_to_framebuffer_blend(fb, character_fb);
}

void render_text(struct framebuffer fb, const char* str, uint32_t color)
{
	mbstate_t ps;
	memset(&ps, 0, sizeof(ps));
	size_t column = 0;
	for ( size_t i = 0; true; i++ )
	{
		wchar_t wc;
		size_t amount = mbrtowc(&wc, str + i, 1, &ps);
		if ( amount == (size_t) -2 )
			continue;
		if ( amount == (size_t) -1 )
		{
			wc = REPLACEMENT_CHARACTER;
			memset(&ps, 0, sizeof(ps));
		}
		if ( amount == (size_t) 0 )
			break;
		int width = wcwidth(wc);
		if ( 0 < width )
		{
			render_char(framebuffer_crop(fb, FONT_REALWIDTH * column, 0,
			            fb.xres, fb.yres), wc, color);
			column += width; // TODO: Overflow.
		}
		if ( amount == (size_t) -1 && str[i] == '\0' )
			break;
	}
}

size_t render_text_columns(const char* str)
{
	mbstate_t ps;
	memset(&ps, 0, sizeof(ps));
	size_t column = 0;
	for ( size_t i = 0; true; i++ )
	{
		wchar_t wc;
		size_t amount = mbrtowc(&wc, str + i, 1, &ps);
		if ( amount == (size_t) -2 )
			continue;
		if ( amount == (size_t) -1 )
		{
			wc = REPLACEMENT_CHARACTER;
			memset(&ps, 0, sizeof(ps));
		}
		if ( amount == (size_t) 0 )
			break;
		int width = wcwidth(wc);
		if ( 0 < width )
			column += width; // TODO: Overflow.
		if ( amount == (size_t) -1 && str[i] == '\0' )
			break;
	}
	return column;
}

size_t render_text_width(const char* str)
{
	// TODO: Overflow.
	return FONT_WIDTH * render_text_columns(str);
}
