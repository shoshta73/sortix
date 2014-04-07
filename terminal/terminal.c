/*
 * Copyright (c) 2017 Jonas 'Sortie' Termansen.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF7
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * terminal.c
 * Terminal emulator.
 */

#include <sys/ioctl.h>
#include <sys/keycodes.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

#if !defined(TTY_NAME_MAX)
#include <sortix/limits.h>
#endif

#include <display.h>
#include <framebuffer.h>
#include <pixel.h>
#include <vgafont.h>

#include "palette.h"

struct kbkey_sequence
{
	const char* sequence;
	int kbkey;
	int flags;
};

#define MODIFIER_ALT (1 << 0)
#define MODIFIER_LSHIFT (1 << 1)
#define MODIFIER_RSHIFT (1 << 2)
#define MODIFIER_LCONTROL (1 << 3)
#define MODIFIER_RCONTROL (1 << 4)

#define SEQUENCE_1IFMOD (1 << 0)
#define SEQUENCE_OSHORT (1 << 1)

static const struct kbkey_sequence kbkey_sequences[] =
{
	{ "\e[A", KBKEY_UP, SEQUENCE_1IFMOD },
	{ "\e[B", KBKEY_DOWN, SEQUENCE_1IFMOD},
	{ "\e[C", KBKEY_RIGHT, SEQUENCE_1IFMOD },
	{ "\e[D", KBKEY_LEFT, SEQUENCE_1IFMOD },
	{ "\e[F", KBKEY_END, SEQUENCE_1IFMOD },
	{ "\e[H", KBKEY_HOME, SEQUENCE_1IFMOD },
	{ "\e[2~", KBKEY_INSERT, 0 },
	{ "\e[3~", KBKEY_DELETE, 0 },
	{ "\e[5~", KBKEY_PGUP, 0 },
	{ "\e[6~", KBKEY_PGDOWN, 0 },
	{ "\e[1P", KBKEY_F1, SEQUENCE_OSHORT },
	{ "\e[1Q", KBKEY_F2, SEQUENCE_OSHORT },
	{ "\e[1R", KBKEY_F3, SEQUENCE_OSHORT },
	{ "\e[1S", KBKEY_F4, SEQUENCE_OSHORT },
	{ "\e[15~", KBKEY_F5, 0 },
	{ "\e[17~", KBKEY_F6, 0 },
	{ "\e[18~", KBKEY_F7, 0 },
	{ "\e[19~", KBKEY_F8, 0 },
	{ "\e[20~", KBKEY_F9, 0 },
	{ "\e[21~", KBKEY_F10, 0 },
	{ "\e[23~", KBKEY_F11, 0 },
	{ "\e[24~", KBKEY_F12, 0 },
};

static inline const struct kbkey_sequence* lookup_keystroke_sequence(int kbkey)
{
	size_t count = sizeof(kbkey_sequences) / sizeof(kbkey_sequences[0]);
	for ( size_t i = 0; i < count; i++ )
		if ( kbkey_sequences[i].kbkey == kbkey )
			return &kbkey_sequences[i];
	return NULL;
}

static uint32_t WINDOW_ID = 0;
static uint32_t WINDOW_WIDTH = 0;
static uint32_t WINDOW_HEIGHT = 0;

static bool need_redraw = true;
static bool need_show = true;
static bool need_exit = false;
static bool redraw_pipe_written = false;
static int redraw_pipe[2];

struct entry
{
	uint32_t attr;
	uint32_t fgcolor;
	uint32_t bgcolor;
	wchar_t wc;
};

#define ATTR_INVERSE (1 << 0)
#define ATTR_BOLD (1 << 1)
#define ATTR_UNDERLINE (1 << 2)

static struct entry* scrollback;
static size_t column = 0;
static size_t row = 0;
static size_t columns = 0;
static size_t rows = 0;
static int modifiers = 0;
static mbstate_t in_ps;
static mbstate_t out_ps;
static uint32_t default_fgcolor;
static uint32_t default_bgcolor;
static uint32_t current_fgcolor;
static uint32_t current_bgcolor;
static uint32_t attr;
static uint32_t next_attr;
static unsigned ansisavedposx;
static unsigned ansisavedposy;
static enum { NONE = 0, CSI, CHARSET, COMMAND, GREATERTHAN, } ansimode;
#define ANSI_NUM_PARAMS 16
static unsigned ansiusedparams;
static unsigned ansiparams[ANSI_NUM_PARAMS];
static bool ignore_sequence;
static bool draw_cursor = true;

static void scrollback_resize(size_t new_rows, size_t new_columns)
{
	// TODO: Recover gracefully if the scrollback fails.
	// TODO: Overfow.
	struct entry* new_scrollback =
		calloc(sizeof(struct entry), new_rows * new_columns);
	if ( !new_scrollback )
		err(1, "malloc");
	size_t src_y_after_cursor = rows ? row + 1 : 0;
	size_t src_y_count =
		new_rows < src_y_after_cursor ? new_rows : src_y_after_cursor;
	size_t src_y_from = src_y_after_cursor - src_y_count;
	size_t new_row = row;
	size_t new_column = column;
	new_row += src_y_from;
	for ( size_t dst_y = 0; dst_y < new_rows; dst_y++ )
	{
		size_t src_y = src_y_from + dst_y;
		for ( size_t dst_x = 0; dst_x < new_columns; dst_x++ )
		{
			size_t src_x = dst_x;
			struct entry tc;
			if ( src_x < columns && src_y < rows )
				tc = scrollback[src_y * columns + src_x];
			else if ( columns && rows )
			{
				size_t templ_x = src_y < columns ? src_y : src_x - 1;
				size_t templ_y = src_y < rows ? src_y : rows - 1;
				tc = scrollback[templ_y * columns + templ_x];
				tc.wc = 0;
				tc.attr = 0;
			}
			else
				tc = (struct entry) { 0 };
			new_scrollback[dst_y * new_columns + dst_x] = tc;
			if ( src_x == column && src_y == row )
			{
				new_row = dst_y;
				new_column = dst_x;
			}
		}
	}
	if ( new_columns <= new_column )
		new_column = new_columns ? new_column - 1 : 0;
	if ( new_rows <= new_row )
		new_row = new_rows ? new_row - 1 : 0;
	free(scrollback);
	scrollback = new_scrollback;
	rows = new_rows;
	columns = new_columns;
	row = new_row;
	column = new_column;
}

static void fill(size_t from_x, size_t from_y, size_t to_x, size_t to_y,
                 struct entry with)
{
	// TODO: Assert within bounds?
	size_t from = from_y * columns + from_x;
	size_t to = to_y * columns + to_x;
	for ( size_t i = from; i <= to; i++ )
		scrollback[i] = with;
}

static void scroll(ssize_t offsigned, struct entry with)
{
	if ( 0 < offsigned )
	{
		size_t off = offsigned;
		if ( rows < off )
			off = rows;
		size_t dist = off * columns;
		size_t start = 0;
		size_t end = rows * columns - dist;
		for ( size_t i = start; i < end; i++ )
			scrollback[i] = scrollback[i + dist];
		for ( size_t i = end; i < end + dist; i++ )
			scrollback[i] = with;
	}
	else if ( offsigned < 0 )
	{
		size_t off = -offsigned; // TODO: Negation overflow.
		if ( rows < off )
			off = rows;
		size_t dist = off * columns;
		size_t start = dist;
		size_t end = rows * columns;
		for ( size_t i = start; i < end; i++ )
			scrollback[i] = scrollback[i - dist];
		for ( size_t i = 0; i < dist; i++ )
			scrollback[i] = with;
	}
}

static void newline(void)
{
	if ( row + 1 < rows )
	{
		row++;
		return;
	}
	struct entry entry;
	entry.attr = 0;
	entry.fgcolor = current_fgcolor;
	entry.bgcolor = current_bgcolor;
	entry.wc = 0;
	scroll(1, entry);
}

static void run_ansi_command(char c)
{
	switch ( c )
	{
	case 'A': // Cursor up
	{
		unsigned dist = 0 < ansiusedparams ? ansiparams[0] : 1;
		if ( row < dist )
			row = 0;
		else
			row -= dist;
	} break;
	case 'B': // Cursor down
	{
		unsigned dist = 0 < ansiusedparams ? ansiparams[0] : 1;
		if ( rows <= row + dist )
			row = rows-1;
		else
			row += dist;
	} break;
	case 'C': // Cursor forward
	{
		unsigned dist = 0 < ansiusedparams ? ansiparams[0] : 1;
		if ( columns <= column + dist )
			column = columns-1;
		else
			column += dist;
	} break;
	case 'D': // Cursor backward
	{
		unsigned dist = 0 < ansiusedparams ? ansiparams[0] : 1;
		if ( column < dist )
			column = 0;
		else
			column -= dist;
	} break;
	case 'E': // Move to beginning of line N lines down.
	{
		column = 0;
		unsigned dist = 0 < ansiusedparams ? ansiparams[0] : 1;
		if ( rows <= row + dist )
			row = rows-1;
		else
			row += dist;
	} break;
	case 'F': // Move to beginning of line N lines up.
	{
		column = 0;
		unsigned dist = 0 < ansiusedparams ? ansiparams[0] : 1;
		if ( row < dist )
			row = 0;
		else
			row -= dist;
	} break;
	case 'G': // Move the cursor to column N.
	{
		unsigned pos = 0 < ansiusedparams ? ansiparams[0]-1 : 0;
		if ( columns <= pos )
			pos = columns-1;
		column = pos;
	} break;
	case 'H': // Move the cursor to line Y, column X.
	case 'f':
	{
		unsigned posy = 0 < ansiusedparams ? ansiparams[0]-1 : 0;
		unsigned posx = 1 < ansiusedparams ? ansiparams[1]-1 : 0;
		if ( columns <= posx )
			posx = columns-1;
		if ( rows <= posy )
			posy = rows-1;
		column = posx;
		row = posy;
	} break;
	case 'J': // Erase parts of the screen.
	{
		unsigned mode = 0 < ansiusedparams ? ansiparams[0] : 0;
		size_t from_x = 0, from_y = 0;
		size_t to_x = 0, to_y = 0;

		if ( mode == 0 ) // From cursor to end.
		{
			from_x = column;
			from_y = row;
			// TODO: Ensure the number of rows and columns are always non-zero.
			to_x = columns - 1;
			to_y = rows - 1;
		}

		if ( mode == 1 ) // From start to cursor.
		{
			from_x = 0;
			from_y = 0;
			to_x = columns - 1;
			to_y = rows - 1;
		}

		if ( mode == 2 ) // Everything.
		{
			from_x = 0;
			from_y = 0;
			to_x = columns - 1;
			to_y = rows - 1;
		}

		struct entry with;
		with.attr = 0;
		with.fgcolor = attr & ATTR_INVERSE ? current_bgcolor : current_fgcolor;
		with.bgcolor = attr & ATTR_INVERSE ? current_fgcolor : current_bgcolor;
		with.wc = 0;
		fill(from_x, from_y, to_x, to_y, with);
	} break;
	case 'K': // Erase parts of the current line.
	{
		unsigned mode = 0 < ansiusedparams ? ansiparams[0] : 0;
		size_t from_x = 0, from_y = row;
		size_t to_x = 0, to_y = row;

		if ( mode == 0 ) // From cursor to end.
		{
			from_x = column;
			to_x = columns - 1;
		}

		if ( mode == 1 ) // From start to cursor.
		{
			from_x = 0;
			to_x = column;
		}

		if ( mode == 2 ) // Everything.
		{
			from_x = 0;
			to_x = columns - 1;
		}

		struct entry with;
		with.attr = 0;
		with.fgcolor = attr & ATTR_INVERSE ? current_bgcolor : current_fgcolor;
		with.bgcolor = attr & ATTR_INVERSE ? current_fgcolor : current_bgcolor;
		with.wc = 0;
		fill(from_x, from_y, to_x, to_y, with);
	} break;
	// TODO: CSI Ps M  Delete Ps Line(s) (default = 1) (DL).
	//       (delete those lines and move the rest of the lines upwards).
	// TODO: CSI Ps P  Delete Ps Character(s) (default = 1) (DCH).
	//       (delete those characters and move the rest of the line leftward).
	case 'S': // Scroll a line up and place a new line at the buttom.
	{
		struct entry with;
		with.attr = 0;
		with.fgcolor = attr & ATTR_INVERSE ? current_bgcolor : current_fgcolor;
		with.bgcolor = attr & ATTR_INVERSE ? current_fgcolor : current_bgcolor;
		with.wc = 0;
		scroll(1, with);
		row = rows - 1;
	} break;
	case 'T': // Scroll a line up and place a new line at the top.
	{
		struct entry with;
		with.attr = 0;
		with.fgcolor = attr & ATTR_INVERSE ? current_bgcolor : current_fgcolor;
		with.bgcolor = attr & ATTR_INVERSE ? current_fgcolor : current_bgcolor;
		with.wc = 0;
		scroll(-1, with);
		row = 0;
	} break;
	case 'd': // Move the cursor to line N.
	{
		unsigned posy = 0 < ansiusedparams ? ansiparams[0]-1 : 0;
		if ( rows <= posy )
			posy = rows-1;
		row = posy;
	} break;
	case 'm': // Change how the text is rendered.
	{
		if ( ansiusedparams == 0 )
		{
			ansiparams[0] = 0;
			ansiusedparams++;
		}

		for ( size_t i = 0; i < ansiusedparams; i++ )
		{
			unsigned cmd = ansiparams[i];
			// Turn all attributes off.
			if ( cmd == 0 )
			{
				attr = 0;
				current_fgcolor = default_fgcolor;
				current_bgcolor = default_bgcolor;
			}
			// Boldness.
			else if ( cmd == 1 )
				attr |= ATTR_BOLD;
			// TODO: 2, Faint
			// TODO: 3, Italicized
			// Underline.
			else if ( cmd == 4 )
				attr |= ATTR_UNDERLINE;
			// TODO: 5, Blink (appears as Bold)
			// Inverse.
			else if ( cmd == 7 )
				attr |= ATTR_INVERSE;
			// TODO: 8, Invisible
			// TODO: 9, Crossed-out
			// TODO: 21, Doubly-underlined
			// Normal (neither bold nor faint).
			else if ( cmd == 22 )
				attr &= ~ATTR_BOLD;
			// TODO: 23, Not italicized
			// Not underlined.
			else if ( cmd == 24 )
				attr &= ~ATTR_UNDERLINE;
			// TODO: 25, Steady (not blinking)
			// Positive (not inverse).
			else if ( cmd == 27 )
				attr &= ~ATTR_INVERSE;
			// TODO: 28, Visible (not hidden)
			// Set text color.
			else if ( 30 <= cmd && cmd <= 37 )
			{
				unsigned val = cmd - 30;
				current_fgcolor = palette[val] | 0xFF000000;
			}
			// Set text color.
			else if ( cmd == 38 )
			{
				if ( 5 <= ansiusedparams - i && ansiparams[i+1] == 2 )
				{
					uint8_t r = ansiparams[i+2];
					uint8_t g = ansiparams[i+3];
					uint8_t b = ansiparams[i+4];
					i += 5 - 1;
					current_fgcolor = make_color(r, g, b);
				}
				else if ( 3 <= ansiusedparams - i && ansiparams[i+1] == 5 )
				{
					uint8_t index = ansiparams[i+2];
					i += 3 - 1;
					current_fgcolor = palette[index] | 0xFF000000;
				}
			}
			// Set default text color.
			else if ( cmd == 39 )
			{
				current_fgcolor = default_fgcolor;
			}
			// Set background color.
			else if ( 40 <= cmd && cmd <= 47 )
			{
				unsigned val = cmd - 40;
				current_bgcolor = palette[val] | 0xFF000000;
			}
			// Set background color.
			else if ( cmd == 48 )
			{
				if ( 5 <= ansiusedparams - i && ansiparams[i+1] == 2 )
				{
					uint8_t r = ansiparams[i+2];
					uint8_t g = ansiparams[i+3];
					uint8_t b = ansiparams[i+4];
					i += 5 - 1;
					current_bgcolor = make_color(r, g, b);
				}
				else if ( 3 <= ansiusedparams - i && ansiparams[i+1] == 5 )
				{
					uint8_t index = ansiparams[i+2];
					i += 3 - 1;
					current_bgcolor = palette[index] | 0xFF000000;
				}
			}
			// Set default background color.
			else if ( cmd == 49 )
			{
				current_bgcolor = default_bgcolor;
			}
			// Set text color.
			else if ( 90 <= cmd && cmd <= 97 )
			{
				unsigned val = cmd - 90 + 8;
				current_fgcolor = palette[val] | 0xFF000000;
			}
			// Set background color.
			else if ( 100 <= cmd && cmd <= 107 )
			{
				unsigned val = cmd - 100 + 8;
				current_bgcolor = palette[val] | 0xFF000000;
			}
			else
			{
				ansimode = NONE;
			}
		}
	} break;
	case 'n': // Request special information from terminal.
	{
		ansimode = NONE;
		// TODO: Handle this code.
	} break;
	case 's': // Save cursor position.
	{
		ansisavedposx = column;
		ansisavedposy = row;
	} break;
	case 'u': // Restore cursor position.
	{
		column = ansisavedposx;
		row = ansisavedposy;
		if ( columns <= column )
			column = columns-1;
		if ( rows <= row )
			row = rows-1;
	} break;
	case 'l': // Hide cursor.
	{
		// TODO: This is somehow related to the special char '?'.
		if ( 0 < ansiusedparams && ansiparams[0] == 25 )
			draw_cursor = false;
		if ( 0 < ansiusedparams && ansiparams[0] == 1049 )
			{}; // TODO: Save scrollback.
	} break;
	case 'h': // Show cursor.
	{
		// TODO: This is somehow related to the special char '?'.
		if ( 0 < ansiusedparams && ansiparams[0] == 25 )
			draw_cursor = true;
		if ( 0 < ansiusedparams && ansiparams[0] == 1049 )
			{}; // TODO: Restore scrollback.
	} break;
	default:
	{
		ansimode = NONE;
	}
	// TODO: Handle other cases.
	}

	ansimode = NONE;
}

static void put_ansi_escaped(char c)
{
	// Check the proper prefixes are used.
	if ( ansimode == CSI )
	{
		if ( c == '[' )
			ansimode = COMMAND;
		else if ( c == '(' || c == ')' || c == '*' || c == '+' ||
		          c == '-' || c == '.' || c == '/' )
			ansimode = CHARSET;
		// TODO: Enter and exit alternatve keypad mode.
		else if ( c == '=' || c == '>' )
			ansimode = NONE;
		else
		{
			ansimode = NONE;
		}
		return;
	}

	if ( ansimode == CHARSET )
	{
		ansimode = NONE;
		return;
	}

	// Read part of a parameter.
	if ( '0' <= c && c <= '9' )
	{
		if ( ansiusedparams == 0 )
			ansiusedparams++;
		unsigned val = c - '0';
		ansiparams[ansiusedparams-1] *= 10;
		ansiparams[ansiusedparams-1] += val;
	}

	// Parameter delimiter.
	else if ( c == ';' )
	{
		if ( ansiusedparams == ANSI_NUM_PARAMS )
		{
			ansimode = NONE;
			return;
		}
		ansiparams[ansiusedparams++] = 0;
	}

	// Left for future standardization, so discard this sequence.
	else if ( c == ':' )
	{
		ignore_sequence = true;
	}

	else if ( c == '>' )
	{
		ansimode = GREATERTHAN;
	}

	// Run a command.
	else if ( 64 <= c && c <= 126 )
	{
		if ( !ignore_sequence )
		{
			if ( ansimode == COMMAND )
				run_ansi_command(c);
			else if ( ansimode == GREATERTHAN )
			{
				// Send Device Attributes
				if ( c == 'c' )
				{
					// TODO: Send an appropriate response through the terminal.
				}
				else
				{
					ansimode = NONE;
					return;
				}
				ansimode = NONE;
			}
		}
		else
			ansimode = NONE;
	}

	// Something I don't understand, and ignore intentionally.
	else if ( c == '?' )
	{
		//ansimode = NONE;
	}

	// TODO: There are some rare things that should be supported here.

	// Ignore unknown input.
	else
	{
		ansimode = NONE;
	}
}

static uint32_t boldify(uint32_t color)
{
	int b = color >>  0 & 0xFF;
	int g = color >>  8 & 0xFF;
	int r = color >> 16 & 0xFF;
	int a = color >> 24 & 0xFF;
	b += 63;
	if ( 255 < b )
		b = 255;
	g += 63;
	if ( 255 < g )
		g = 255;
	r += 63;
	if ( 255 < r )
		r = 255;
	return make_color_a(r, g, b, a);
}

static void outwc(wchar_t wc)
{
	if ( wc == L'\a' )
	{
	}
	else if ( wc == L'\n' )
	{
		newline();
	}
	else if ( wc == L'\r' )
	{
		column = 0;
	}
	else if ( wc == L'\b' )
	{
		if ( column )
		{
			column--;
			struct entry* entry = &scrollback[row * columns + column];
			next_attr = entry->attr & (ATTR_BOLD | ATTR_UNDERLINE);
			if ( entry->wc == L'_' )
				next_attr |= ATTR_UNDERLINE;
			else if ( entry->wc == L' ' )
				next_attr &= ~ATTR_BOLD;
			else
				next_attr |= ATTR_BOLD;
		}
	}
	else if ( wc == L'\t' )
	{
		if ( column == columns )
		{
			newline();
			column = 0;
		}
		column++;
		column = -(-column & ~((size_t)0x7));
		if ( columns <= column )
			column = columns;
	}
	else if ( wc == L'\e' )
	{
		next_attr = 0;
		ansiusedparams = 0;
		ansiparams[0] = 0;
		ignore_sequence = false;
		ansimode = CSI;
	}
	else
	{
		if ( column == columns )
		{
			newline();
			column = 0;
		}
		struct entry* entry = &scrollback[row * columns + column++];
		entry->attr = attr | next_attr;
		if ( !(entry->attr & ATTR_INVERSE) )
		{
			entry->fgcolor = current_fgcolor;
			entry->bgcolor = current_bgcolor;
		}
		else
		{
			entry->fgcolor = current_bgcolor;
			entry->bgcolor = current_fgcolor;
		}
		if ( entry->attr & ATTR_BOLD )
			entry->fgcolor = boldify(entry->fgcolor);
		entry->wc = wc;
		next_attr = 0;
	}
}

static void outc(char c)
{
	if ( ansimode != NONE )
	{
		put_ansi_escaped(c);
		return;
	}
	wchar_t wc;
	size_t amount = mbrtowc(&wc, &c, 1, &out_ps);
	if ( amount == (size_t) -2 )
		return;
	if ( amount == (size_t) -1 )
	{
		memset(&out_ps, 0, sizeof(out_ps));
		wc = 0xFFFD; /* REPLACEMENT CHARACTER */;
	}
	if ( amount == (size_t) 0 )
		wc = L' ';
	outwc(wc);
}

static pthread_mutex_t scrollback_mutex = PTHREAD_MUTEX_INITIALIZER;
static int master_fd;

static void* outgoing_thread(void* ctx)
{
	(void) ctx;
	const char* getcursor = "\e[6n";
	size_t i = 0;
	char c;
	ssize_t amount = 0;
	while ( 0 < (amount = read(master_fd, &c, 1)) )
	{
		// TODO: Do escape code handling in the escape code parsing.
		if ( c == getcursor[i] )
		{
			i++;
			if ( !getcursor[i] )
			{
				i = 0;
				char buf[64];
				snprintf(buf, sizeof(buf), "\e[%zu;%zuR",
				         (size_t) (row + 1), (size_t) (column + 1));
				for ( size_t n = 0; buf[n]; n++ )
				{
					if ( write(master_fd, &buf[n], 1) <= 0 )
					{
						warn("incoming write");
						break; // TODO: This break is incorrect.
					}
				}
			}
			continue;
		}
		pthread_mutex_lock(&scrollback_mutex);
		for ( size_t j = 0; j < i; j++ )
			outc(getcursor[j]);
		i = 0;
		outc(c);
		if ( !redraw_pipe_written )
		{
			char c = 'X';
			if ( write(redraw_pipe[1], &c, 1) < 0 )
				warn("write: redraw_pipe");
			else
				redraw_pipe_written = true;
		}
		pthread_mutex_unlock(&scrollback_mutex);
	}
	if ( amount < 0 )
		warn("outgoing read");
	return NULL;
}

void on_disconnect(void* ctx)
{
	(void) ctx;
	need_exit = true;
}

void on_quit(void* ctx, uint32_t window_id)
{
	(void) ctx;
	if ( window_id != WINDOW_ID )
		return;
	need_exit = true;
}

void on_resize(void* ctx, uint32_t window_id, uint32_t width, uint32_t height)
{
	(void) ctx;
	if ( window_id != WINDOW_ID )
		return;
	// TODO: The resolution 0x0 gets sent for newly created windows that hasn't
	//       been shown yet, fix the compositor to not do this.
	if ( width == 0 && height == 0 )
		return;
	pthread_mutex_lock(&scrollback_mutex);
	size_t new_rows = height / FONT_HEIGHT;
	size_t new_columns = width / FONT_WIDTH;
	scrollback_resize(new_rows, new_columns);
	struct winsize ws;
	ws.ws_row = rows;
	ws.ws_col = columns;
	if ( ioctl(master_fd, TIOCSWINSZ, &ws) < 0 )
		warn("TIOCSWINSZ");
	pthread_mutex_unlock(&scrollback_mutex);
	need_redraw = true;
	WINDOW_WIDTH = width;
	WINDOW_HEIGHT = height;
}

void inuc(unsigned char uc)
{
	write(master_fd, &uc, 1);
}

void on_keycode(int kbkey)
{
	if ( kbkey < 0 )
		return;

	if ( kbkey == KBKEY_ESC )
	{
		inuc('\e');
		return;
	}

	const struct kbkey_sequence* seq = lookup_keystroke_sequence(kbkey);
	if ( !seq )
		return;

	const char* str = seq->sequence;
	size_t len = strlen(str);

	int mods = 0;
	if ( modifiers & (MODIFIER_LSHIFT | MODIFIER_RSHIFT) )
		mods |= 1;
	if ( modifiers & MODIFIER_ALT )
		mods |= 2;
	if ( modifiers & (MODIFIER_LCONTROL | MODIFIER_RCONTROL) )
		mods |= 4;

	if ( (seq->flags & SEQUENCE_OSHORT) && mods == 0 )
	{
		inuc('\e');
		inuc('O');
		inuc((unsigned char) str[len-1]);
		return;
	}

	for ( size_t i = 0; i < len - 1; i++ )
		inuc((unsigned char) str[i]);
	if ( seq->flags & SEQUENCE_1IFMOD && mods != 0 )
		inuc('1');
	if ( mods )
	{
		inuc(';');
		inuc('1' + mods);
	}
	inuc(str[len-1]);
}

void on_keyboard(void* ctx, uint32_t window_id, uint32_t codepoint)
{
	(void) ctx;
	if ( window_id != WINDOW_ID )
		return;
	int kbkey = KBKEY_DECODE(codepoint);
	if ( kbkey != 0 )
	{
		// TODO: Don't do this here, let the compositor do this.
		if ( kbkey == KBKEY_LALT )
			modifiers |= MODIFIER_ALT;
		else if ( kbkey == -KBKEY_LALT )
			modifiers &= ~MODIFIER_ALT;
		else if ( kbkey == KBKEY_LSHIFT )
			modifiers |= MODIFIER_LSHIFT;
		else if ( kbkey == -KBKEY_LSHIFT )
			modifiers &= ~MODIFIER_LSHIFT;
		else if ( kbkey == KBKEY_RSHIFT )
			modifiers |= MODIFIER_RSHIFT;
		else if ( kbkey == -KBKEY_RSHIFT )
			modifiers &= ~MODIFIER_RSHIFT;
		else if ( kbkey == KBKEY_LCTRL )
			modifiers |= MODIFIER_LCONTROL;
		else if ( kbkey == -KBKEY_LCTRL )
			modifiers &= ~MODIFIER_LCONTROL;
		else if ( kbkey == KBKEY_RCTRL )
			modifiers |= MODIFIER_RCONTROL;
		else if ( kbkey == -KBKEY_RCTRL )
			modifiers &= ~MODIFIER_RCONTROL;
		on_keycode(kbkey);
		return;
	}
	if ( codepoint == '\n' )
		codepoint = '\r';
	bool control = modifiers & (MODIFIER_LCONTROL | MODIFIER_RCONTROL);
	if ( codepoint == '\b' )
		codepoint = 127;
	if ( modifiers & MODIFIER_ALT )
		inuc('\e');
	if ( control && codepoint == L' ' )
		inuc(0);
	else if ( control && (L'`' <= codepoint && codepoint <= L'}') )
		inuc(codepoint - L'`');
	else if ( control && (L'@' <= codepoint && codepoint <= L'_') )
		inuc(codepoint - L'@');
	else if ( control && codepoint == L'?' )
		inuc(127);
	else
	{
		wchar_t wc = codepoint;
		char mb[MB_CUR_MAX];
		size_t amount = wcrtomb(mb, wc, &in_ps);
		if ( amount == (size_t) -1 )
			memset(&in_ps, 0, sizeof(in_ps));
		else
		{
			for ( size_t i = 0; i < amount; i++ )
				inuc((unsigned char) mb[i]);
		}
	}
}

void draw(struct display_connection* connection)
{
	uint32_t* framebuffer = (uint32_t*)
		calloc(sizeof(uint32_t), WINDOW_WIDTH * WINDOW_HEIGHT);
	assert(framebuffer);

	struct framebuffer fb;
	fb.pitch = WINDOW_WIDTH;
	fb.buffer = framebuffer;
	fb.xres = WINDOW_WIDTH;
	fb.yres = WINDOW_HEIGHT;

	pthread_mutex_lock(&scrollback_mutex);
	size_t draw_rows = WINDOW_HEIGHT / FONT_HEIGHT;
	size_t draw_columns = WINDOW_WIDTH / FONT_WIDTH;
	if ( rows < draw_rows )
		draw_rows = rows;
	if ( columns < draw_columns )
		draw_rows = columns;
	for ( size_t y = 0; y < rows; y++ )
	{
		size_t yoff = FONT_HEIGHT * y;
		for ( size_t x = 0; x < columns; x++ )
		{
			size_t xoff = FONT_WIDTH * x;
			size_t cell_width = FONT_WIDTH;
			size_t cell_height = FONT_HEIGHT;
			if ( x + 1 == columns )
				cell_width = WINDOW_WIDTH - xoff;
			if ( y + 1 == rows )
				cell_height = WINDOW_HEIGHT - yoff;
			struct framebuffer charfb =
				framebuffer_crop(fb, xoff, yoff, cell_width, cell_height);
			struct entry* entry = &scrollback[y * columns + x];
			for ( size_t py = 0; py < charfb.yres; py++ )
			{
				uint32_t* line = charfb.buffer + py * charfb.pitch;
				for ( size_t px = 0; px < charfb.xres; px++ )
					line[px] = entry->bgcolor;
			}
			render_char(charfb, entry->wc, entry->fgcolor);
			size_t entry_width = FONT_WIDTH;
			size_t entry_height = FONT_HEIGHT;
			if ( charfb.xres < entry_width )
				entry_width = charfb.xres;
			if ( charfb.yres < entry_height )
				entry_height = charfb.yres;
			size_t underlines = 0;
			if ( draw_cursor && y == row && x == column )
				underlines = 2;
			else if ( entry->attr & ATTR_UNDERLINE )
				underlines = 1;
			size_t start_underlines = FONT_HEIGHT - underlines;
			for ( size_t py = start_underlines; py < entry_height; py++ )
			{
				uint32_t* line = charfb.buffer + py * charfb.pitch;
				for ( size_t x = 0; x < entry_width; x++ )
					line[x] = blend_pixel(line[x], entry->fgcolor);
			}
		}
	}
	pthread_mutex_unlock(&scrollback_mutex);

	display_render_window(connection, WINDOW_ID, 0, 0, WINDOW_WIDTH,
	                      WINDOW_HEIGHT, framebuffer);

	free(framebuffer);
}

int main(int argc, char* argv[])
{
	(void) argc;
	(void) argv;

	struct display_connection* connection = display_connect_default();
	if ( !connection && errno == ECONNREFUSED )
		display_spawn(argc, argv);
	if ( !connection )
		err(1, "Could not connect to display server");

	load_font();

	if ( pipe(redraw_pipe) < 0 )
		err(1, "pipe");

	rows = 25;
	columns = 80;

	// TODO: Overflow.
	scrollback = calloc(sizeof(struct entry), rows * columns);
	if ( !scrollback )
		err(1, "malloc");

	WINDOW_WIDTH = columns * FONT_WIDTH;
	WINDOW_HEIGHT = rows * FONT_HEIGHT;

	default_bgcolor = make_color_a(0, 0, 0, 220);
	default_fgcolor = palette[7] | 0xFF000000;
	current_bgcolor = default_bgcolor;
	current_fgcolor = default_fgcolor;
	for ( size_t y = 0; y < rows; y++ )
	{
		for ( size_t x = 0; x < columns; x++ )
		{
			struct entry* entry = &scrollback[y * columns + x];
			entry->attr = 0;
			entry->fgcolor = current_fgcolor;
			entry->bgcolor = current_bgcolor;
			entry->wc = 0;
		}
	}

	display_create_window(connection, WINDOW_ID);
	display_resize_window(connection, WINDOW_ID, WINDOW_WIDTH, WINDOW_HEIGHT);
	display_title_window(connection, WINDOW_ID, "Terminal");

	struct winsize ws;
	ws.ws_row = rows;
	ws.ws_col = columns;
	char path[TTY_NAME_MAX + 1];
	int slave_fd;
	if ( openpty(&master_fd, &slave_fd, path, NULL, &ws) < 0 )
		err(1, "openpty");

	pid_t child_pid = fork();
	if ( child_pid < 0 )
		err(1, "fork");

	if ( !child_pid )
	{
		if ( setsid() < 0 )
		{
			warn("setsid");
			_exit(1);
		}
		if ( ioctl(slave_fd, TIOCSCTTY) < 0 )
		{
			warn("ioctl: TIOCSCTTY");
			_exit(1);
		}
		if ( close(0) < 0 || close(1) < 0 || close(2) < 0 )
		{
			warn("close");
			_exit(1);
		}
		if ( dup2(slave_fd, 0) != 0 ||
		     dup2(slave_fd, 1) != 1 ||
		     dup2(slave_fd, 2) != 2 )
		{
			warn("dup");
			_exit(1);
		}
		if ( closefrom(3) < 0 )
		{
			warn("closefrom");
			_exit(1);
		}
		if ( argc <= 1 )
		{
			const char* program = "sh";
			execlp(program, program, (const char*) NULL);
		}
		else
		{
			execvp(argv[1], argv + 1);
		}
		_exit(127);
	}

	int errnum;
	pthread_t outthread;
	if ( (errnum = pthread_create(&outthread, NULL, outgoing_thread, NULL)) )
	{
		errno = errnum;
		err(1, "pthread_create");
	}

	struct display_event_handlers handlers;
	memset(&handlers, 0, sizeof(handlers));
	handlers.disconnect_handler = on_disconnect;
	handlers.quit_handler = on_quit;
	handlers.resize_handler = on_resize;
	handlers.keyboard_handler = on_keyboard;

	const nfds_t nfds = 2;
	struct pollfd pfds[nfds];
	pfds[0].fd = redraw_pipe[0];
	pfds[0].events = POLLIN;
	pfds[0].revents = 0;
	pfds[1].fd = display_connection_fd(connection);
	pfds[1].events = POLLIN;
	pfds[1].revents = 0;

	while ( !need_exit )
	{
		if ( need_redraw )
		{
			draw(connection);
			need_redraw = false;
		}

		if ( need_show )
		{
			display_show_window(connection, WINDOW_ID);
			need_show = false;
		}

		if ( poll(pfds, nfds, -1) < 0 )
			err(1, "poll");

		if ( pfds[0].revents )
		{
			pthread_mutex_lock(&scrollback_mutex);
			char c;
			ssize_t amount = read(redraw_pipe[0], &c, 1);
			if ( amount < 0 )
				err(1, "read: redraw_pipe");
			if ( 0 < amount )
			{
				redraw_pipe_written = false;
				need_redraw = true;
			}
			pthread_mutex_unlock(&scrollback_mutex);
		}

		if ( pfds[1].revents )
		{
			while ( display_poll_event(connection, &handlers) == 0 );
		}
	}

	display_disconnect(connection);

	return 0;
}
