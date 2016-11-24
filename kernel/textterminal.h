/*
 * Copyright (c) 2011, 2012, 2013, 2014, 2015, 2016 Jonas 'Sortie' Termansen.
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
 * textterminal.h
 * Translates a character stream to a 2 dimensional array of characters.
 */

#ifndef SORTIX_TEXTTERMINAL_H
#define SORTIX_TEXTTERMINAL_H

#include <wchar.h>

#include <sortix/kernel/kthread.h>
#include <sortix/kernel/refcount.h>

namespace Sortix {

class TextBufferHandle;

class TextTerminal
{
public:
	TextTerminal(TextBufferHandle* textbufhandle);
	~TextTerminal();
	size_t Print(const char* string, size_t stringlen);
	size_t PrintRaw(const char* string, size_t stringlen);
	size_t Width();
	size_t Height();
	void GetCursor(size_t* column, size_t* row);
	bool Sync();
	bool Invalidate();
	void BeginReplace();
	void CancelReplace();
	void FinishReplace(TextBuffer* textbuf);
	bool EmergencyIsImpaired();
	bool EmergencyRecoup();
	void EmergencyReset();
	size_t EmergencyPrint(const char* string, size_t stringlen);
	size_t EmergencyPrintRaw(const char* string, size_t stringlen);
	size_t EmergencyWidth();
	size_t EmergencyHeight();
	void EmergencyGetCursor(size_t* column, size_t* row);
	bool EmergencySync();

private:
	void PutChar(TextBuffer* textbuf, char c);
	void UpdateCursor(TextBuffer* textbuf);
	void Newline(TextBuffer* textbuf);
	void Backspace(TextBuffer* textbuf);
	void Tab(TextBuffer* textbuf);
	void PutAnsiEscaped(TextBuffer* textbuf, char c);
	void RunAnsiCommand(TextBuffer* textbuf, char c);
	void AnsiReset();
	void Reset();
	void UnhandledDebug(TextBuffer* textbuf, const char* str);

private:
	mbstate_t ps;
	TextBufferHandle* textbufhandle;
	kthread_mutex_t termlock;
	uint16_t next_attr;
	uint32_t attr;
	uint32_t fgcolor;
	uint32_t bgcolor;
	uint8_t vgacolor;
	unsigned column;
	unsigned line;
	unsigned ansisavedposx;
	unsigned ansisavedposy;
	enum { NONE = 0, CSI, CHARSET, COMMAND, GREATERTHAN, } ansimode;
	static const size_t ANSI_NUM_PARAMS = 16;
	unsigned ansiusedparams;
	unsigned ansiparams[ANSI_NUM_PARAMS];
	bool ignoresequence;

};

} // namespace Sortix

#endif
