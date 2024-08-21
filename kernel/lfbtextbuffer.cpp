/*
 * Copyright (c) 2012, 2013, 2014, 2015, 2016, 2021 Jonas 'Sortie' Termansen.
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
 * lfbtextbuffer.cpp
 * An indexable text buffer rendered to a graphical linear frame buffer.
 */

#include <string.h>

#include <sortix/vga.h>

#include <sortix/kernel/kernel.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/refcount.h>
#include <sortix/kernel/scheduler.h>
#include <sortix/kernel/textbuffer.h>
#include <sortix/kernel/thread.h>

#include "vga.h"
#include "lfbtextbuffer.h"

namespace Sortix {

static uint32_t boldify(uint32_t color)
{
	int b = color >>  0 & 0xFF;
	int g = color >>  8 & 0xFF;
	int r = color >> 16 & 0xFF;
	b += 63;
	if ( 255 < b )
		b = 255;
	g += 63;
	if ( 255 < g )
		g = 255;
	r += 63;
	if ( 255 < r )
		r = 255;
	return (uint32_t) b << 0 | (uint32_t) g << 8 | (uint32_t) r << 16;
}

static void LFBTextBuffer__RenderThread(void* user)
{
	((LFBTextBuffer*) user)->RenderThread();
}

LFBTextBuffer* CreateLFBTextBuffer(uint8_t* lfb, uint32_t lfbformat,
                                   size_t xres, size_t yres, size_t scansize)
{
	const size_t QUEUE_LENGTH = 1024;
	size_t columns = xres / (VGA_FONT_WIDTH + 1);
	size_t rows = yres / VGA_FONT_HEIGHT;
	size_t fontsize = VGA_FONT_CHARSIZE * VGA_FONT_NUMCHARS;
	uint8_t* backbuf;
	uint8_t* font;
	TextChar* chars;
	TextBufferCmd* queue;
	LFBTextBuffer* ret;

	Process* kernel_process = Scheduler::GetKernelProcess();

	if ( !(backbuf = new uint8_t[yres * scansize]) )
		goto cleanup_done;
	if ( !(font = new uint8_t[fontsize]) )
		goto cleanup_backbuf;
	if ( !(chars = new TextChar[columns * rows]) )
		goto cleanup_font;
	if ( !(queue = new TextBufferCmd[QUEUE_LENGTH]) )
		goto cleanup_chars;
	if ( !(ret = new LFBTextBuffer) )
		goto cleanup_queue;

	memcpy(font, VGA::GetFont(), fontsize);
	ret->execute_lock = KTHREAD_MUTEX_INITIALIZER;
	ret->queue_lock = KTHREAD_MUTEX_INITIALIZER;
	ret->queue_not_full = KTHREAD_COND_INITIALIZER;
	ret->queue_not_empty = KTHREAD_COND_INITIALIZER;
	ret->queue_exit = KTHREAD_COND_INITIALIZER;
	ret->queue_sync = KTHREAD_COND_INITIALIZER;
	ret->queue_paused = KTHREAD_COND_INITIALIZER;
	ret->queue_resume = KTHREAD_COND_INITIALIZER;
	ret->queue = queue;
	ret->queue_length = QUEUE_LENGTH;
	ret->queue_offset = 0;
	ret->queue_used = 0;
	ret->queue_is_paused = false;
	ret->queue_thread = false;
	ret->lfb = lfb;
	ret->backbuf = backbuf;
	ret->lfbformat = lfbformat;
	ret->bytes_per_pixel = (lfbformat + 7) / 8;
	ret->pixelsx = xres;
	ret->pixelsy = yres;
	ret->scansize = scansize;
	ret->columns = columns;
	ret->rows = rows;
	ret->font = font;
	ret->chars = chars;
	ret->cursorenabled = true;
	ret->cursorpos = TextPos(0, 0);
	ret->emergency_state = false;
	ret->invalidated = false;
	ret->need_clear = true;
	ret->exit_after_pause = false;

	if ( !kernel_process )
		return ret;

	ret->queue_thread = true; // Visible to new thread.
	if ( !RunKernelThread(kernel_process, LFBTextBuffer__RenderThread, ret,
	                      "console") )
		ret->queue_thread = false;

	return ret;

cleanup_queue:
	delete[] queue;
cleanup_chars:
	delete[] chars;
cleanup_font:
	delete[] font;
cleanup_backbuf:
	delete[] backbuf;
cleanup_done:
	return NULL;
}

void LFBTextBuffer::SpawnThreads()
{
	if ( queue_thread )
		return;
	Process* kernel_process = Scheduler::GetKernelProcess();
	queue_thread = true; // Visible to new thread.
	if ( !RunKernelThread(kernel_process, LFBTextBuffer__RenderThread, this,
	                      "console") )
		queue_thread = false;
}

LFBTextBuffer::LFBTextBuffer()
{
}

LFBTextBuffer::~LFBTextBuffer()
{
	if ( queue_thread )
	{
		kthread_mutex_lock(&queue_lock);
		if ( queue_is_paused )
		{
			queue_is_paused = false;
			exit_after_pause = true;
			kthread_cond_signal(&queue_resume);
		}
		else
		{
			TextBufferCmd cmd;
			cmd.type = TEXTBUFCMD_EXIT;
			kthread_mutex_unlock(&queue_lock);
			IssueCommand(&cmd);
			kthread_mutex_lock(&queue_lock);
		}
		while ( queue_thread )
			kthread_cond_wait(&queue_exit, &queue_lock);
		kthread_mutex_unlock(&queue_lock);
	}
	delete[] backbuf;
	delete[] font;
	delete[] chars;
	delete[] queue;
}

size_t LFBTextBuffer::Width()
{
	return columns;
}

size_t LFBTextBuffer::Height()
{
	return rows;
}

bool LFBTextBuffer::UsablePosition(TextPos pos) const
{
	return pos.x < columns && pos.y < rows;
}

TextPos LFBTextBuffer::CropPosition(TextPos pos) const
{
	if ( columns <= pos.x )
		pos.x = columns - 1;
	if ( rows <= pos.y )
		pos.y = rows -1;
	return pos;
}

TextPos LFBTextBuffer::AddToPosition(TextPos pos, size_t count)
{
	size_t index = pos.y * columns + pos.x + count;
	TextPos ret(index % columns, index / columns);
	return CropPosition(ret);
}

void LFBTextBuffer::RenderChar(TextChar textchar, size_t posx, size_t posy)
{
	if ( columns <= posx || rows <= posy )
		return;
	// TODO: Support other resolutions.
	if ( lfbformat != 24 && lfbformat != 32 )
		return;
	// TODO: Support other font sizes.
	if ( VGA_FONT_WIDTH != 8UL )
		return;
	bool drawcursor = cursorenabled && posx == cursorpos.x && posy == cursorpos.y;
	uint32_t fgcolor = textchar.fg;
	uint32_t bgcolor = textchar.bg;
	if ( textchar.attr & ATTR_BOLD )
		fgcolor = boldify(fgcolor);
	int remap = VGA::MapWideToVGAFont(textchar.c);
	const uint8_t* charfont = VGA::GetCharacterFont(font, remap);
	size_t pixelyoff = rows * VGA_FONT_HEIGHT;
	size_t pixelxoff = posx * (VGA_FONT_WIDTH+1);
	for ( size_t y = 0; y < VGA_FONT_HEIGHT; y++ )
	{
		size_t pixely = posy * VGA_FONT_HEIGHT + y;
		uint8_t linebitmap = charfont[y];
		uint8_t* line = (uint8_t*) (lfb + pixely * scansize);
		size_t bytesxoff = bytes_per_pixel * pixelxoff;
		for ( size_t x = 0; x < VGA_FONT_WIDTH; x++ )
		{
			uint32_t color = linebitmap & 1 << (7-x) ? fgcolor : bgcolor;
			line[bytesxoff++] = color >> 0;
			line[bytesxoff++] = color >> 8;
			line[bytesxoff++] = color >> 16;
			if ( lfbformat == 32 )
				line[bytesxoff++] = color >> 24;
		}
		uint32_t lastcolor = bgcolor;
		if ( 0xB0 <= remap && remap <= 0xDF && (linebitmap & 1) )
			lastcolor = fgcolor;
		line[bytesxoff++] = lastcolor >> 0;
		line[bytesxoff++] = lastcolor >> 8;
		line[bytesxoff++] = lastcolor >> 16;
		if ( lfbformat == 32 )
			line[bytesxoff++] = lastcolor >> 24;
		if ( unlikely(posx + 1 == columns) )
		{
			for ( size_t x = pixelxoff + VGA_FONT_WIDTH + 1; x < pixelsx; x++ )
			{
				line[bytesxoff++] = bgcolor >> 0;
				line[bytesxoff++] = bgcolor >> 8;
				line[bytesxoff++] = bgcolor >> 16;
				if ( lfbformat == 32 )
					line[bytesxoff++] = bgcolor >> 24;
			}
		}
	}
	if ( unlikely(posy + 1 == rows) )
	{
		size_t width = VGA_FONT_WIDTH + 1;
		if ( unlikely(posx + 1 == columns) )
			width = pixelsx - pixelxoff;
		for ( size_t y = pixelyoff; y < pixelsy; y++ )
		{
			size_t bytesxoff = bytes_per_pixel * pixelxoff;
			uint8_t* line = (uint8_t*) (lfb + y * scansize);
			for ( size_t x = 0; x < width; x++ )
			{
				line[bytesxoff++] = bgcolor >> 0;
				line[bytesxoff++] = bgcolor >> 8;
				line[bytesxoff++] = bgcolor >> 16;
				if ( lfbformat == 32 )
					line[bytesxoff++] = bgcolor >> 24;
			}
		}
	}
	if ( likely(!drawcursor) && !(textchar.attr & ATTR_UNDERLINE) )
		return;
	size_t underlines = VGA_FONT_HEIGHT - (!drawcursor ? 1 : 0);
	for ( size_t y = VGA_FONT_HEIGHT - 2; y < underlines; y++ )
	{
		size_t pixely = posy * VGA_FONT_HEIGHT + y;
		uint8_t* line = (uint8_t*) (lfb + pixely * scansize);
		size_t bytesxoff = bytes_per_pixel * pixelxoff;
		for ( size_t x = 0; x < VGA_FONT_WIDTH+1; x++ )
		{
			line[bytesxoff++] = fgcolor >> 0;
			line[bytesxoff++] = fgcolor >> 8;
			line[bytesxoff++] = fgcolor >> 16;
			if ( lfbformat == 32 )
				line[bytesxoff++] = fgcolor >> 24;
		}
	}
}

void LFBTextBuffer::RenderCharAt(TextPos pos)
{
	RenderChar(chars[pos.y * columns + pos.x], pos.x, pos.y);
}

void LFBTextBuffer::RenderRegion(size_t c1, size_t r1, size_t c2, size_t r2)
{
	for ( size_t y = r1; y <= r2; y++ )
		for ( size_t x = c1; x <= c2; x++ )
			RenderChar(chars[y * columns + x], x, y);
}

void LFBTextBuffer::RenderRange(TextPos from, TextPos to)
{
	from = CropPosition(from);
	to = CropPosition(to);
	uint8_t* orig_lfb = lfb;
	bool backbuffered = from.y != to.y;
	if ( backbuffered )
	{
		lfb = backbuf;
		from.x = 0;
		to.x = columns - 1;
	}
	TextPos i = from;
	RenderChar(chars[i.y * columns + i.x], i.x, i.y);
	while ( !(i.x==to.x && i.y==to.y) )
	{
		i = AddToPosition(i, 1);
		RenderChar(chars[i.y * columns + i.x], i.x, i.y);
	}
	if ( backbuffered )
	{
		lfb = orig_lfb;
		size_t scanline_start = from.y * VGA_FONT_HEIGHT;
		size_t scanline_end = ((to.y+1) * VGA_FONT_HEIGHT) - 1;
		if ( to.y + 1 == rows )
			scanline_end = pixelsy - 1;
		for ( size_t sc = scanline_start; sc <= scanline_end; sc++ )
		{
			size_t offset = sc * scansize;
			memcpy(lfb + offset, backbuf + offset, pixelsx * bytes_per_pixel);
		}
	}
}

void LFBTextBuffer::IssueCommand(TextBufferCmd* cmd, bool already_locked)
{
	if ( invalidated )
	{
		invalidated = false;
		TextBufferCmd newcmd;
		newcmd.type = TEXTBUFCMD_REDRAW;
		IssueCommand(&newcmd, already_locked);
	}
	if ( !queue_thread || emergency_state )
	{
		bool exit_requested = false;
		bool sync_requested = false;
		bool pause_requested = false;
		TextPos render_from(columns - 1, rows - 1);
		TextPos render_to(0, 0);
		ExecuteCommand(cmd, exit_requested, sync_requested, pause_requested, render_from, render_to);
		if ( !IsTextPosBeforeTextPos(render_to, render_from) )
			RenderRange(render_from, render_to);
		return;
	}
	if ( !already_locked )
		kthread_mutex_lock(&queue_lock);
	while ( queue_used == queue_length )
		kthread_cond_wait(&queue_not_full, &queue_lock);
	if ( !queue_used )
		kthread_cond_signal(&queue_not_empty);
	queue[(queue_offset + queue_used++) % queue_length] = *cmd;
	if ( !already_locked )
		kthread_mutex_unlock(&queue_lock);
}

bool LFBTextBuffer::StopRendering()
{
	if ( !queue_thread || emergency_state )
		return false;
	TextBufferCmd cmd;
	cmd.type = TEXTBUFCMD_PAUSE;
	ScopedLock lock(&queue_lock);
	if ( queue_is_paused )
		return false;
	IssueCommand(&cmd, true);
	while ( !queue_is_paused )
		kthread_cond_wait(&queue_paused, &queue_lock);
	return true;
}

void LFBTextBuffer::ResumeRendering()
{
	if ( !queue_thread || emergency_state )
		return;
	ScopedLock lock(&queue_lock);
	if ( !queue_is_paused )
		return;
	queue_is_paused = false;
	kthread_cond_signal(&queue_resume);
}

TextChar LFBTextBuffer::GetChar(TextPos pos)
{
	if ( UsablePosition(pos) )
	{
		bool was_rendering = StopRendering();
		TextChar ret = chars[pos.y * columns + pos.x];
		if ( was_rendering )
			ResumeRendering();
		return ret;
	}
	return {0, 0, 0, 0, 0};
}

void LFBTextBuffer::SetChar(TextPos pos, TextChar c)
{
	if ( !UsablePosition(pos) )
		return;
	TextBufferCmd cmd;
	cmd.type = TEXTBUFCMD_CHAR;
	cmd.x = pos.x;
	cmd.y = pos.y;
	cmd.c = c;
	IssueCommand(&cmd);
}

bool LFBTextBuffer::GetCursorEnabled()
{
	bool was_rendering = StopRendering();
	bool ret = cursorenabled;
	if ( was_rendering )
		ResumeRendering();
	return ret;
}

void LFBTextBuffer::SetCursorEnabled(bool enablecursor)
{
	TextBufferCmd cmd;
	cmd.type = TEXTBUFCMD_CURSOR_SET_ENABLED;
	cmd.b = enablecursor;
	IssueCommand(&cmd);
}

TextPos LFBTextBuffer::GetCursorPos()
{
	bool was_rendering = StopRendering();
	TextPos ret = cursorpos;
	if ( was_rendering )
		ResumeRendering();
	return ret;
}

void LFBTextBuffer::SetCursorPos(TextPos newcursorpos)
{
	TextBufferCmd cmd;
	cmd.type = TEXTBUFCMD_CURSOR_MOVE;
	cmd.x = newcursorpos.x;
	cmd.y = newcursorpos.y;
	IssueCommand(&cmd);
}

void LFBTextBuffer::Invalidate()
{
	invalidated = true;
}

size_t LFBTextBuffer::OffsetOfPos(TextPos pos) const
{
	return pos.y * columns + pos.x;
}

void LFBTextBuffer::Scroll(ssize_t off, TextChar fillwith)
{
	if ( !off )
		return;
	TextBufferCmd cmd;
	cmd.type = TEXTBUFCMD_SCROLL;
	cmd.scroll_offset = off;
	cmd.c = fillwith;
	IssueCommand(&cmd);
}

void LFBTextBuffer::Move(TextPos to, TextPos from, size_t numchars)
{
	to = CropPosition(to);
	from = CropPosition(from);
	TextBufferCmd cmd;
	cmd.type = TEXTBUFCMD_MOVE;
	cmd.to_x = to.x;
	cmd.to_y = to.y;
	cmd.from_x = from.x;
	cmd.from_y = from.y;
	cmd.val = numchars;
	IssueCommand(&cmd);
}

void LFBTextBuffer::Fill(TextPos from, TextPos to, TextChar fillwith)
{
	from = CropPosition(from);
	to = CropPosition(to);
	TextBufferCmd cmd;
	cmd.type = TEXTBUFCMD_FILL;
	cmd.from_x = from.x;
	cmd.from_y = from.y;
	cmd.to_x = to.x;
	cmd.to_y = to.y;
	cmd.c = fillwith;
	IssueCommand(&cmd);
}

void LFBTextBuffer::DoScroll(ssize_t off, TextChar entry)
{
	bool neg = 0 < off;
	size_t absoff = off < 0 ? -off : off;
	if ( rows < absoff )
		absoff = rows;
	TextPos scrollfrom = neg ? TextPos{0, absoff} : TextPos{0, 0};
	TextPos scrollto = neg ? TextPos{0, 0} : TextPos{0, absoff};
	TextPos fillfrom = neg ? TextPos{0, rows-absoff} : TextPos{0, 0};
	TextPos fillto = neg ? TextPos{columns-1, rows-1} : TextPos{columns-1, absoff-1};
	size_t scrollchars = columns * (rows-absoff);
	DoMove(scrollto, scrollfrom, scrollchars);
	DoFill(fillfrom, fillto, entry);
}

void LFBTextBuffer::DoMove(TextPos to, TextPos from, size_t numchars)
{
	size_t dest = OffsetOfPos(to);
	size_t src = OffsetOfPos(from);
	if ( dest < src )
		for ( size_t i = 0; i < numchars; i++ )
			chars[dest + i] = chars[src + i];
	else if ( src < dest )
		for ( size_t i = 0; i < numchars; i++ )
			chars[dest + numchars-1 - i] = chars[src + numchars-1 - i];
}

void LFBTextBuffer::DoFill(TextPos from, TextPos to, TextChar fillwith)
{
	size_t start = OffsetOfPos(from);
	size_t end = OffsetOfPos(to);
	for ( size_t i = start; i <= end; i++ )
		chars[i] = fillwith;
}

bool LFBTextBuffer::IsCommandIdempotent(const TextBufferCmd* cmd) const
{
	switch ( cmd->type )
	{
		case TEXTBUFCMD_EXIT: return true;
		case TEXTBUFCMD_SYNC: return true;
		case TEXTBUFCMD_PAUSE: return true;
		case TEXTBUFCMD_CHAR: return true;
		case TEXTBUFCMD_CURSOR_SET_ENABLED: return true;
		case TEXTBUFCMD_CURSOR_MOVE: return true;
		case TEXTBUFCMD_MOVE: return false;
		case TEXTBUFCMD_FILL: return true;
		case TEXTBUFCMD_SCROLL: return false;
		case TEXTBUFCMD_REDRAW: return true;
		default: return false;
	}
}

void LFBTextBuffer::ExecuteCommand(TextBufferCmd* cmd,
                                   bool& exit_requested,
                                   bool& sync_requested,
                                   bool& pause_requested,
                                   TextPos& render_from,
                                   TextPos& render_to)
{
	switch ( cmd->type )
	{
		case TEXTBUFCMD_EXIT:
			exit_requested = true;
			break;
		case TEXTBUFCMD_SYNC:
			sync_requested = true;
			break;
		case TEXTBUFCMD_PAUSE:
			pause_requested = true;
			break;
		case TEXTBUFCMD_CHAR:
		{
			TextPos pos(cmd->x, cmd->y);
			chars[pos.y * columns + pos.x] = cmd->c;
			if ( IsTextPosBeforeTextPos(pos, render_from) )
				render_from = pos;
			if ( IsTextPosAfterTextPos(pos, render_to) )
				render_to = pos;
		} break;
		case TEXTBUFCMD_CURSOR_SET_ENABLED:
			if ( cmd->b != cursorenabled )
			{
				cursorenabled = cmd->b;
				if ( IsTextPosBeforeTextPos(cursorpos, render_from) )
					render_from = cursorpos;
				if ( IsTextPosAfterTextPos(cursorpos, render_to) )
					render_to = cursorpos;
			}
			break;
		case TEXTBUFCMD_CURSOR_MOVE:
		{
			TextPos pos(cmd->x, cmd->y);
			if ( cursorpos.x != pos.x || cursorpos.y != pos.y )
			{
				if ( IsTextPosBeforeTextPos(cursorpos, render_from) )
					render_from = cursorpos;
				if ( IsTextPosAfterTextPos(cursorpos, render_to) )
					render_to = cursorpos;
				cursorpos = pos;
				if ( IsTextPosBeforeTextPos(cursorpos, render_from) )
					render_from = cursorpos;
				if ( IsTextPosAfterTextPos(cursorpos, render_to) )
					render_to = cursorpos;
			}
		} break;
		case TEXTBUFCMD_MOVE:
		{
			TextPos to(cmd->to_x, cmd->to_y);
			TextPos from(cmd->from_x, cmd->from_y);
			size_t numchars = cmd->val;
			DoMove(to, from, numchars);
			TextPos toend = AddToPosition(to, numchars);
			if ( IsTextPosBeforeTextPos(to, render_from) )
				render_from = to;
			if ( IsTextPosAfterTextPos(toend, render_to) )
				render_to = toend;
		} break;
		case TEXTBUFCMD_FILL:
		{
			TextPos from(cmd->from_x, cmd->from_y);
			TextPos to(cmd->to_x, cmd->to_y);
			DoFill(from, to, cmd->c);
			if ( IsTextPosBeforeTextPos(from, render_from) )
				render_from = from;
			if ( IsTextPosAfterTextPos(to, render_to) )
				render_to = to;
		} break;
		case TEXTBUFCMD_SCROLL:
		{
			ssize_t off = cmd->scroll_offset;
			DoScroll(off, cmd->c);
			render_from = {0, 0};
			render_to = {columns-1, rows-1};
		} break;
		case TEXTBUFCMD_REDRAW:
		{
			render_from = {0, 0};
			render_to = {columns-1, rows-1};
		} break;
	}
}

void LFBTextBuffer::RenderThread()
{
	queue_is_paused = false;
	size_t offset = 0;
	size_t amount = 0;
	bool exit_requested = false;
	bool sync_requested = false;
	bool pause_requested = false;

	while ( true )
	{
		kthread_mutex_lock(&queue_lock);

		if ( queue_used == queue_length && amount )
			kthread_cond_signal(&queue_not_full);

		queue_used -= amount;
		queue_offset = (queue_offset + amount) % queue_length;

		if ( !queue_used )
		{
			if ( exit_requested )
			{
				queue_thread = false;
				kthread_cond_signal(&queue_exit);
				kthread_mutex_unlock(&queue_lock);
				return;
			}

			if ( sync_requested )
			{
				kthread_cond_signal(&queue_sync);
				sync_requested = false;
			}

			if ( pause_requested )
			{
				queue_is_paused = true;
				kthread_cond_signal(&queue_paused);
				while ( queue_is_paused )
					kthread_cond_wait(&queue_resume, &queue_lock);
				pause_requested = false;
				if ( exit_after_pause )
				{
					queue_thread = false;
					kthread_cond_signal(&queue_exit);
					kthread_mutex_unlock(&queue_lock);
					return;
				}
			}
		}

		while ( !queue_used )
			kthread_cond_wait(&queue_not_empty, &queue_lock);

		amount = queue_used;
		offset = queue_offset;

		kthread_mutex_unlock(&queue_lock);

		execute_amount = amount;

		kthread_mutex_lock(&execute_lock);

		TextPos render_from(columns - 1, rows - 1);
		TextPos render_to(0, 0);

		for ( size_t i = 0; i < amount; i++ )
		{
			TextBufferCmd* cmd = &queue[(offset + i) % queue_length];
			ExecuteCommand(cmd, exit_requested, sync_requested, pause_requested, render_from, render_to);
		}

		kthread_mutex_unlock(&execute_lock);

		if ( !IsTextPosBeforeTextPos(render_to, render_from) )
			RenderRange(render_from, render_to);
	}
}

bool LFBTextBuffer::EmergencyIsImpaired()
{
	return !emergency_state;
}

bool LFBTextBuffer::EmergencyRecoup()
{
	if ( !emergency_state )
		emergency_state = true;

	if ( !kthread_mutex_trylock(&queue_lock) )
		return false;
	kthread_mutex_unlock(&queue_lock);

	if ( !kthread_mutex_trylock(&execute_lock) )
	{
		for ( size_t i = 0; i < execute_amount; i++ )
		{
			TextBufferCmd* cmd = &queue[(queue_offset + i) % queue_length];
			if ( !IsCommandIdempotent(cmd) )
				return false;
		}
	}
	else
		kthread_mutex_unlock(&execute_lock);

	TextPos render_from(0, 0);
	TextPos render_to(columns - 1, rows - 1);

	for ( size_t i = 0; i < queue_used; i++ )
	{
		bool exit_requested = false;
		bool sync_requested = false;
		bool pause_requested = false;
		TextBufferCmd* cmd = &queue[(queue_offset + i) % queue_length];
		ExecuteCommand(cmd, exit_requested, sync_requested, pause_requested,
		               render_from, render_to);
	}

	queue_used = 0;
	queue_offset = 0;

	RenderRange(render_from, render_to);

	return true;
}

void LFBTextBuffer::EmergencyReset()
{
	// TODO: Reset everything here!

	Fill(TextPos{0, 0}, TextPos{columns-1, rows-1}, TextChar{0, 0, 0, 0, 0});
	SetCursorPos(TextPos{0, 0});
}

void LFBTextBuffer::Resume()
{
	if ( need_clear )
	{
		for ( size_t y = 0; y < pixelsy; y++ )
			memset(lfb + scansize * y, 0, bytes_per_pixel * pixelsx);
		need_clear = false;
	}
	ResumeRendering();
}

void LFBTextBuffer::Pause()
{
	StopRendering();
}

} // namespace Sortix
