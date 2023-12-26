/*
 * Copyright (c) 2011, 2012, 2013, 2014, 2015, 2016 Jonas 'Sortie' Termansen.
 * Copyright (c) 2023 Juhani 'nortti' Krekelä.
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
 * textterminal.cpp
 * Translates a character stream to a 2 dimensional array of characters.
 */

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <sortix/vga.h>

#include <sortix/kernel/kernel.h>
#include <sortix/kernel/refcount.h>
#include <sortix/kernel/textbuffer.h>

#include "palette.h"
#include "textterminal.h"

namespace Sortix {

static const uint16_t DEFAULT_VGACOLOR = COLOR8_LIGHT_GREY | COLOR8_BLACK << 4;
static const unsigned int DEFAULT_FOREGROUND = 7;
static const unsigned int DEFAULT_BACKGROUND = 0;

static uint32_t ColorFromRGB(uint8_t r, uint8_t g, uint8_t b)
{
	return (uint32_t) b << 0 | (uint32_t) g << 8 | (uint32_t) r << 16;
}

TextTerminal::TextTerminal(TextBufferHandle* textbufhandle)
{
	memset(&ps, 0, sizeof(ps));
	this->textbufhandle = textbufhandle;
	this->termlock = KTHREAD_MUTEX_INITIALIZER;
	Reset();
}

TextTerminal::~TextTerminal()
{
}

void TextTerminal::Reset()
{
	attr = 0;
	next_attr = 0;
	vgacolor = DEFAULT_VGACOLOR;
	fgcolor = palette[DEFAULT_FOREGROUND];
	bgcolor = palette[DEFAULT_BACKGROUND];
	column = line = 0;
	ansisavedposx = ansisavedposy = 0;
	ansimode = NONE;
	TextBuffer* textbuf = textbufhandle->Acquire();
	TextPos fillfrom(0, 0);
	TextPos fillto(textbuf->Width()-1, textbuf->Height()-1);
	TextChar fillwith(' ', vgacolor, 0, fgcolor, bgcolor);
	textbuf->Fill(fillfrom, fillto, fillwith);
	textbuf->SetCursorEnabled(true);
	UpdateCursor(textbuf);
	textbufhandle->Release(textbuf);
}

size_t TextTerminal::Print(const char* string, size_t stringlen)
{
	ScopedLock lock(&termlock);
	TextBuffer* textbuf = textbufhandle->Acquire();
	for ( size_t i = 0; i < stringlen; i++ )
	{
		if ( string[i] == '\n' )
			PutChar(textbuf, '\r');
		PutChar(textbuf, string[i]);
	}
	UpdateCursor(textbuf);
	textbufhandle->Release(textbuf);
	return stringlen;
}

size_t TextTerminal::PrintRaw(const char* string, size_t stringlen)
{
	ScopedLock lock(&termlock);
	TextBuffer* textbuf = textbufhandle->Acquire();
	for ( size_t i = 0; i < stringlen; i++ )
		PutChar(textbuf, string[i]);
	UpdateCursor(textbuf);
	textbufhandle->Release(textbuf);
	return stringlen;
}

size_t TextTerminal::Width()
{
	ScopedLock lock(&termlock);
	TextBuffer* textbuf = textbufhandle->Acquire();
	size_t width = textbuf->Width();
	textbufhandle->Release(textbuf);
	return width;
}

size_t TextTerminal::Height()
{
	ScopedLock lock(&termlock);
	TextBuffer* textbuf = textbufhandle->Acquire();
	size_t height = textbuf->Height();
	textbufhandle->Release(textbuf);
	return height;
}

void TextTerminal::GetCursor(size_t* column, size_t* row)
{
	ScopedLock lock(&termlock);
	*column = this->column;
	*row = this->line;
}

bool TextTerminal::Sync()
{
	// Reading something from the textbuffer may cause it to block while
	// finishing rendering, effectively synchronizing with it.
	ScopedLock lock(&termlock);
	TextBuffer* textbuf = textbufhandle->Acquire();
	textbuf->GetCursorPos();
	textbufhandle->Release(textbuf);
	return true;
}

bool TextTerminal::Invalidate()
{
	ScopedLock lock(&termlock);
	TextBuffer* textbuf = textbufhandle->Acquire();
	textbuf->Invalidate();
	textbufhandle->Release(textbuf);
	return true;
}

void TextTerminal::BeginReplace()
{
	kthread_mutex_lock(&termlock);
	textbufhandle->BeginReplace();
}

void TextTerminal::CancelReplace()
{
	textbufhandle->CancelReplace();
	kthread_mutex_unlock(&termlock);
}

void TextTerminal::FinishReplace(TextBuffer* new_textbuf)
{
	textbufhandle->FinishReplace(new_textbuf);
	TextBuffer* textbuf = textbufhandle->Acquire();
	size_t new_width = textbuf->Width();
	size_t new_height = textbuf->Height();
	textbufhandle->Release(textbuf);
	if ( new_width < column )
		column = new_width;
	if ( new_height <= line )
		line = new_height ? new_height - 1 : 0;
	kthread_mutex_unlock(&termlock);
}

bool TextTerminal::EmergencyIsImpaired()
{
	// This is during a kernel emergency where preemption has been disabled and
	// this is the only thread running.

	if ( !kthread_mutex_trylock(&termlock) )
		return true;
	kthread_mutex_unlock(&termlock);

	if ( textbufhandle->EmergencyIsImpaired() )
		return true;

	TextBuffer* textbuf = textbufhandle->EmergencyAcquire();
	bool textbuf_was_impaired = textbuf->EmergencyIsImpaired();
	textbufhandle->EmergencyRelease(textbuf);
	if ( textbuf_was_impaired )
		return true;

	return false;
}

bool TextTerminal::EmergencyRecoup()
{
	// This is during a kernel emergency where preemption has been disabled and
	// this is the only thread running.

	if ( !kthread_mutex_trylock(&termlock) )
		return false;
	kthread_mutex_unlock(&termlock);

	if ( textbufhandle->EmergencyIsImpaired() &&
	     !textbufhandle->EmergencyRecoup() )
		return false;

	TextBuffer* textbuf = textbufhandle->EmergencyAcquire();
	bool textbuf_failure = textbuf->EmergencyIsImpaired() &&
	                       !textbuf->EmergencyRecoup();
	textbufhandle->EmergencyRelease(textbuf);

	if ( !textbuf_failure )
		return false;

	return true;
}

void TextTerminal::EmergencyReset()
{
	// This is during a kernel emergency where preemption has been disabled and
	// this is the only thread running.

	textbufhandle->EmergencyReset();

	TextBuffer* textbuf = textbufhandle->EmergencyAcquire();
	textbuf->EmergencyReset();
	textbufhandle->EmergencyRelease(textbuf);

	this->termlock = KTHREAD_MUTEX_INITIALIZER;
	Reset();
}

size_t TextTerminal::EmergencyPrint(const char* string, size_t stringlen)
{
	// This is during a kernel emergency where preemption has been disabled and
	// this is the only thread running. Another thread may have been interrupted
	// while it held the terminal lock. The best case is if the terminal lock is
	// currently unused, which would mean everything is safe.

	TextBuffer* textbuf = textbufhandle->EmergencyAcquire();
	for ( size_t i = 0; i < stringlen; i++ )
	{
		if ( string[i] == '\n' )
			PutChar(textbuf, '\r');
		PutChar(textbuf, string[i]);
	}
	UpdateCursor(textbuf);
	textbufhandle->EmergencyRelease(textbuf);
	return stringlen;
}

size_t TextTerminal::EmergencyPrintRaw(const char* string, size_t stringlen)
{
	// This is during a kernel emergency where preemption has been disabled and
	// this is the only thread running. Another thread may have been interrupted
	// while it held the terminal lock. The best case is if the terminal lock is
	// currently unused, which would mean everything is safe.

	TextBuffer* textbuf = textbufhandle->EmergencyAcquire();
	for ( size_t i = 0; i < stringlen; i++ )
		PutChar(textbuf, string[i]);
	UpdateCursor(textbuf);
	textbufhandle->EmergencyRelease(textbuf);
	return stringlen;
}

size_t TextTerminal::EmergencyWidth()
{
	// This is during a kernel emergency where preemption has been disabled and
	// this is the only thread running. Another thread may have been interrupted
	// while it held the terminal lock. The best case is if the terminal lock is
	// currently unused, which would mean everything is safe.

	TextBuffer* textbuf = textbufhandle->EmergencyAcquire();
	size_t width = textbuf->Width();
	textbufhandle->EmergencyRelease(textbuf);
	return width;
}

size_t TextTerminal::EmergencyHeight()
{
	// This is during a kernel emergency where preemption has been disabled and
	// this is the only thread running. Another thread may have been interrupted
	// while it held the terminal lock. The best case is if the terminal lock is
	// currently unused, which would mean everything is safe.

	TextBuffer* textbuf = textbufhandle->EmergencyAcquire();
	size_t height = textbuf->Height();
	textbufhandle->EmergencyRelease(textbuf);
	return height;
}

void TextTerminal::EmergencyGetCursor(size_t* column, size_t* row)
{
	// This is during a kernel emergency where preemption has been disabled and
	// this is the only thread running. Another thread may have been interrupted
	// while it held the terminal lock. The best case is if the terminal lock is
	// currently unused, which would mean everything is safe.

	*column = this->column;
	*row = this->line;
}

bool TextTerminal::EmergencySync()
{
	// This is during a kernel emergency where preemption has been disabled and
	// this is the only thread running. There is no need to synchronize the
	// text buffer here as there is no background thread rendering the console.

	return true;
}

void TextTerminal::PutChar(TextBuffer* textbuf, char c)
{
	if ( ansimode )
		return PutAnsiEscaped(textbuf, c);

	if ( mbsinit(&ps) )
	{
		switch ( c )
		{
		case '\a': return;
		case '\n': Newline(textbuf); return;
		case '\r': column = 0; return;
		case '\b': Backspace(textbuf); return;
		case '\t': Tab(textbuf); return;
		case '\e': AnsiReset(); return;
		case 127: return;
		default: break;
		}
	}

	wchar_t wc;
	size_t result = mbrtowc(&wc, &c, 1, &ps);
	if ( result == (size_t) -2 )
		return;
	if ( result == (size_t) -1 )
	{
		memset(&ps, 0, sizeof(ps));
		wc = L'�';
	}
	if ( result == (size_t) 0 )
		wc = L' ';

	if ( textbuf->Width() <= column )
	{
		column = 0;
		Newline(textbuf);
	}
	TextPos pos(column++, line);
	uint16_t tcvgacolor;
	uint16_t tcattr = attr | next_attr;
	uint32_t tcfgcolor;
	uint32_t tcbgcolor;
	if ( !(tcattr & ATTR_INVERSE) )
	{
		tcvgacolor = vgacolor;
		tcfgcolor = fgcolor;
		tcbgcolor = bgcolor;
	}
	else
	{
		tcvgacolor = (vgacolor >> 4 & 0xF) << 0 | (vgacolor >> 0 & 0xF) << 4;
		tcfgcolor = bgcolor;
		tcbgcolor = fgcolor;
	}
	TextChar tc(wc, tcvgacolor, tcattr, tcfgcolor, tcbgcolor);
	textbuf->SetChar(pos, tc);
	next_attr = 0;
}

void TextTerminal::UpdateCursor(TextBuffer* textbuf)
{
	textbuf->SetCursorPos(TextPos(column, line));
}

void TextTerminal::Newline(TextBuffer* textbuf)
{
	TextPos pos(column, line);
	if ( line < textbuf->Height()-1 )
		line++;
	else
	{
		uint32_t fillfg = attr & ATTR_INVERSE ? bgcolor : fgcolor;
		uint32_t fillbg = attr & ATTR_INVERSE ? fgcolor : bgcolor;
		textbuf->Scroll(1, TextChar(' ', vgacolor, 0, fillfg, fillbg));
		line = textbuf->Height()-1;
	}
}

#if 0
static TextPos DecrementTextPos(TextBuffer* textbuf, TextPos pos)
{
	if ( !pos.x && !pos.y )
		return pos;
	if ( !pos.x )
		return TextPos(textbuf->Width(), pos.y-1);
	return TextPos(pos.x-1, pos.y);
}
#endif

void TextTerminal::Backspace(TextBuffer* textbuf)
{
	if ( column )
	{
		column--;
		TextPos pos(column, line);
		TextChar tc = textbuf->GetChar(pos);
		next_attr = tc.attr & (ATTR_BOLD | ATTR_UNDERLINE);
		if ( tc.c == L'_' )
			next_attr |= ATTR_UNDERLINE;
		else if ( tc.c == L' ' )
			next_attr &= ~ATTR_BOLD;
		else
			next_attr |= ATTR_BOLD;
	}
}

void TextTerminal::Tab(TextBuffer* textbuf)
{
	if ( column == textbuf->Width() )
	{
		column = 0;
		Newline(textbuf);
	}
	column++;
	column = -(-column & ~0x7U);
	size_t width = textbuf->Width();
	if ( width <= column )
		column = width;
}

// TODO: This implementation of the 'Ansi Escape Codes' is incomplete and hacky.
void TextTerminal::AnsiReset()
{
	next_attr = 0;
	ansiusedparams = 0;
	ansiparams[0] = 0;
	ignoresequence = false;
	ansimode = CSI;
}

void TextTerminal::PutAnsiEscaped(TextBuffer* textbuf, char c)
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
		ignoresequence = true;
	}

	else if ( c == '>' )
	{
		ansimode = GREATERTHAN;
	}

	// Run a command.
	else if ( 64 <= c && c <= 126 )
	{
		if ( !ignoresequence )
		{
			if ( ansimode == COMMAND )
				RunAnsiCommand(textbuf, c);
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
	}

	// TODO: There are some rare things that should be supported here.

	// Ignore unknown input.
	else
	{
		ansimode = NONE;
	}
}

void TextTerminal::RunAnsiCommand(TextBuffer* textbuf, char c)
{
	const unsigned width = (unsigned) textbuf->Width();
	const unsigned height = (unsigned) textbuf->Height();

	switch ( c )
	{
	case 'A': // Cursor up
	{
		unsigned dist = 0 < ansiusedparams ? ansiparams[0] : 1;
		if ( line < dist )
			line = 0;
		else
			line -= dist;
	} break;
	case 'B': // Cursor down
	{
		unsigned dist = 0 < ansiusedparams ? ansiparams[0] : 1;
		if ( height <= line + dist )
			line = height-1;
		else
			line += dist;
	} break;
	case 'C': // Cursor forward
	{
		unsigned dist = 0 < ansiusedparams ? ansiparams[0] : 1;
		if ( width <= column + dist )
			column = width-1;
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
		if ( height <= line + dist )
			line = height-1;
		else
			line += dist;
	} break;
	case 'F': // Move to beginning of line N lines up.
	{
		column = 0;
		unsigned dist = 0 < ansiusedparams ? ansiparams[0] : 1;
		if ( line < dist )
			line = 0;
		else
			line -= dist;
	} break;
	case 'G': // Move the cursor to column N.
	{
		unsigned pos = 0 < ansiusedparams ? ansiparams[0]-1 : 0;
		if ( width <= pos )
			pos = width-1;
		column = pos;
	} break;
	case 'H': // Move the cursor to line Y, column X.
	case 'f':
	{
		unsigned posy = 0 < ansiusedparams ? ansiparams[0]-1 : 0;
		unsigned posx = 1 < ansiusedparams ? ansiparams[1]-1 : 0;
		if ( width <= posx )
			posx = width-1;
		if ( height <= posy )
			posy = height-1;
		column = posx;
		line = posy;
	} break;
	case 'J': // Erase parts of the screen.
	{
		unsigned mode = 0 < ansiusedparams ? ansiparams[0] : 0;
		TextPos from(0, 0);
		TextPos to(0, 0);

		if ( mode == 0 ) // From cursor to end.
			from = TextPos{column, line},
			to = TextPos{width-1, height-1};

		if ( mode == 1 ) // From start to cursor.
			from = TextPos{0, 0},
			to = TextPos{column, line};

		if ( mode == 2 ) // Everything.
			from = TextPos{0, 0},
			to = TextPos{width-1, height-1};

		uint32_t fillfg = attr & ATTR_INVERSE ? bgcolor : fgcolor;
		uint32_t fillbg = attr & ATTR_INVERSE ? fgcolor : bgcolor;
		textbuf->Fill(from, to, TextChar(' ', vgacolor, 0, fillfg, fillbg));
	} break;
	case 'K': // Erase parts of the current line.
	{
		unsigned mode = 0 < ansiusedparams ? ansiparams[0] : 0;
		TextPos from(0, 0);
		TextPos to(0, 0);

		if ( mode == 0 ) // From cursor to end.
			from = TextPos{column, line},
			to = TextPos{width-1, line};

		if ( mode == 1 ) // From start to cursor.
			from = TextPos{0, line},
			to = TextPos{column, line};

		if ( mode == 2 ) // Everything.
			from = TextPos{0, line},
			to = TextPos{width-1, line};

		uint32_t fillfg = attr & ATTR_INVERSE ? bgcolor : fgcolor;
		uint32_t fillbg = attr & ATTR_INVERSE ? fgcolor : bgcolor;
		textbuf->Fill(from, to, TextChar(' ', vgacolor, 0, fillfg, fillbg));
	} break;
	case 'L': // Append lines before current line.
	{
		column = 0;
		unsigned count = 0 < ansiusedparams ? ansiparams[0] : 1;
		if ( height - line < count )
			count = height - line;
		TextPos from(0, line);
		TextPos move_to(0, line + count);
		unsigned move_lines = height - (line + count);
		textbuf->Move(move_to, from, move_lines * width);
		if ( 0 < count )
		{
			TextPos fill_to(width - 1, line + count - 1);
			uint32_t fill_fg = attr & ATTR_INVERSE ? bgcolor : fgcolor;
			uint32_t fill_bg = attr & ATTR_INVERSE ? fgcolor : bgcolor;
			TextChar fill_char(' ', vgacolor, 0, fill_fg, fill_bg);
			textbuf->Fill(from, fill_to, fill_char);
		}
	} break;
	case 'M': // Delete lines starting from beginning of current line.
	{
		column = 0;
		unsigned count = 0 < ansiusedparams ? ansiparams[0] : 1;
		if ( height - line < count )
			count = height - line;
		TextPos move_from(0, line + count);
		TextPos move_to(0, line);
		unsigned move_lines = height - (line + count);
		textbuf->Move(move_to, move_from, move_lines * width);
		if ( 0 < count )
		{
			TextPos fill_from(0, height - count);
			TextPos fill_to(width - 1, height - 1);
			uint32_t fill_fg = attr & ATTR_INVERSE ? bgcolor : fgcolor;
			uint32_t fill_bg = attr & ATTR_INVERSE ? fgcolor : bgcolor;
			TextChar fill_char(' ', vgacolor, 0, fill_fg, fill_bg);
			textbuf->Fill(fill_from, fill_to, fill_char);
		}
	} break;
	// TODO: CSI Ps P  Delete Ps Character(s) (default = 1) (DCH).
	//       (delete those characters and move the rest of the line leftward).
	case 'S': // Scroll a line up and place a new line at the buttom.
	{
		uint32_t fillfg = attr & ATTR_INVERSE ? bgcolor : fgcolor;
		uint32_t fillbg = attr & ATTR_INVERSE ? fgcolor : bgcolor;
		textbuf->Scroll(1, TextChar(' ', vgacolor, 0, fillfg, fillbg));
		line = height-1;
	} break;
	case 'T': // Scroll a line up and place a new line at the top.
	{
		uint32_t fillfg = attr & ATTR_INVERSE ? bgcolor : fgcolor;
		uint32_t fillbg = attr & ATTR_INVERSE ? fgcolor : bgcolor;
		textbuf->Scroll(-1, TextChar(' ', vgacolor, 0, fillfg, fillbg));
		line = 0;
	} break;
	case 'd': // Move the cursor to line N.
	{
		unsigned posy = 0 < ansiusedparams ? ansiparams[0]-1 : 0;
		if ( height <= posy )
			posy = height-1;
		line = posy;
	} break;
	case 'm': // Change how the text is rendered.
	{
		if ( ansiusedparams == 0 )
		{
			ansiparams[0] = 0;
			ansiusedparams++;
		}

		// Convert from the ANSI color scheme to the VGA color scheme.
		const unsigned conversion[8] =
		{
#if 0
			0, 4, 2, 6, 1, 5, 3, 7
#else
			COLOR8_BLACK, COLOR8_RED, COLOR8_GREEN, COLOR8_BROWN,
			COLOR8_BLUE, COLOR8_MAGENTA, COLOR8_CYAN, COLOR8_LIGHT_GREY,
#endif
		};

		for ( size_t i = 0; i < ansiusedparams; i++ )
		{
			unsigned cmd = ansiparams[i];
			// Turn all attributes off.
			if ( cmd == 0 )
			{
				vgacolor = DEFAULT_VGACOLOR;
				attr = 0;
				fgcolor = palette[DEFAULT_FOREGROUND];
				bgcolor = palette[DEFAULT_BACKGROUND];
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
				vgacolor &= 0xF0;
				vgacolor |= conversion[val] << 0;
				fgcolor = palette[val];
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
					fgcolor = ColorFromRGB(r, g, b);
					// TODO: Approxpiate vgacolor.
				}
				else if ( 3 <= ansiusedparams - i && ansiparams[i+1] == 5 )
				{
					uint8_t index = ansiparams[i+2];
					i += 3 - 1;
					fgcolor = palette[index];
					// TODO: Approxpiate vgacolor.
				}
			}
			// Set default text color.
			else if ( cmd == 39 )
			{
				vgacolor &= 0xF0;
				vgacolor |= DEFAULT_VGACOLOR & 0x0F;
				fgcolor = palette[DEFAULT_FOREGROUND];
			}
			// Set background color.
			else if ( 40 <= cmd && cmd <= 47 )
			{
				unsigned val = cmd - 40;
				vgacolor &= 0x0F;
				vgacolor |= conversion[val] << 4;
				bgcolor = palette[val];
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
					bgcolor = ColorFromRGB(r, g, b);
					// TODO: Approxpiate vgacolor.
				}
				else if ( 3 <= ansiusedparams - i && ansiparams[i+1] == 5 )
				{
					uint8_t index = ansiparams[i+2];
					i += 3 - 1;
					bgcolor = palette[index];
					// TODO: Approxpiate vgacolor.
				}
			}
			// Set default background color.
			else if ( cmd == 49 )
			{
				vgacolor &= 0x0F;
				vgacolor |= DEFAULT_VGACOLOR & 0xF0;
				bgcolor = palette[DEFAULT_BACKGROUND];
			}
			// Set text color.
			else if ( 90 <= cmd && cmd <= 97 )
			{
				unsigned val = cmd - 90 + 8;
				vgacolor &= 0xF0;
				vgacolor |= (0x8 | conversion[val - 8]) << 0;
				fgcolor = palette[val];
			}
			// Set background color.
			else if ( 100 <= cmd && cmd <= 107 )
			{
				unsigned val = cmd - 100 + 8;
				vgacolor &= 0x0F;
				vgacolor |= (0x8 | conversion[val - 8]) << 4;
				bgcolor = palette[val];
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
		ansisavedposy = line;
	} break;
	case 'u': // Restore cursor position.
	{
		column = ansisavedposx;
		line = ansisavedposy;
		if ( width <= column )
			column = width-1;
		if ( height <= line )
			line = height-1;
	} break;
	case 'l': // Hide cursor.
	{
		// TODO: This is somehow related to the special char '?'.
		if ( 0 < ansiusedparams && ansiparams[0] == 25 )
			textbuf->SetCursorEnabled(false);
		if ( 0 < ansiusedparams && ansiparams[0] == 1049 )
			{}; // TODO: Save scrollback.
	} break;
	case 'h': // Show cursor.
	{
		// TODO: This is somehow related to the special char '?'.
		if ( 0 < ansiusedparams && ansiparams[0] == 25 )
			textbuf->SetCursorEnabled(true);
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

} // namespace Sortix
