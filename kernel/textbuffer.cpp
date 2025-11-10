/*
 * Copyright (c) 2012, 2013, 2015, 2016 Jonas 'Sortie' Termansen.
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
 * textbuffer.cpp
 * Provides a indexable text buffer for used by text mode terminals.
 */

#include <assert.h>
#include <errno.h>

#include <sortix/kernel/kernel.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/refcount.h>
#include <sortix/kernel/textbuffer.h>

namespace Sortix {

TextBufferHandle::TextBufferHandle(TextBuffer* textbuf)
{
	this->textbuf = textbuf;
	this->numused = 0;
	this->mutex = KTHREAD_MUTEX_INITIALIZER;
	this->unusedcond = KTHREAD_COND_INITIALIZER;
}

TextBufferHandle::~TextBufferHandle()
{
	delete textbuf;
}

TextBuffer* TextBufferHandle::Acquire()
{
	ScopedLock lock(&mutex);
	if ( !textbuf )
		return errno = EINIT, (TextBuffer*) NULL;
	numused++;
	return textbuf;
}

void TextBufferHandle::Release(TextBuffer* textbuf)
{
	assert(textbuf);
	ScopedLock lock(&mutex);
	assert(numused);
	numused--;
	if ( numused == 0 )
		kthread_cond_signal(&unusedcond);
}

bool TextBufferHandle::EmergencyIsImpaired()
{
	if ( !kthread_mutex_trylock(&mutex) )
		return true;
	kthread_mutex_unlock(&mutex);
	return false;
}

bool TextBufferHandle::EmergencyRecoup()
{
	if ( !EmergencyIsImpaired() )
		return true;
	mutex = KTHREAD_MUTEX_INITIALIZER;
	return true;
}

void TextBufferHandle::EmergencyReset()
{
}

TextBuffer* TextBufferHandle::EmergencyAcquire()
{
	// This is during a kernel emergency where preemption has been disabled and
	// this is the only thread running.
	return textbuf;
}

void TextBufferHandle::EmergencyRelease(TextBuffer* textbuf)
{
	// This is during a kernel emergency where preemption has been disabled and
	// this is the only thread running. We don't maintain the reference count
	// during this state, so this is a no-operation.
	(void) textbuf;
}

void TextBufferHandle::BeginReplace()
{
	kthread_mutex_lock(&mutex);
	while ( 0 < numused )
		kthread_cond_wait(&unusedcond, &mutex);
	if ( textbuf )
		textbuf->Pause();
}

void TextBufferHandle::CancelReplace()
{
	if ( textbuf )
		textbuf->Resume();
	kthread_mutex_unlock(&mutex);
}

void TextBufferHandle::FinishReplace(TextBuffer* newtextbuf)
{
	// TODO: This shouldn't redraw when a graphical app is in control of the
	//       screen. Might even leak information from the console.
	newtextbuf->Resume();
	if ( textbuf )
	{
		size_t src_width = textbuf->Width();
		size_t src_height = textbuf->Height();
		size_t dst_width = newtextbuf->Width();
		size_t dst_height = newtextbuf->Height();
		bool cursor_enabled = textbuf->GetCursorEnabled();
		TextPos src_cursor = textbuf->GetCursorPos();
		size_t src_y_after_cursor = src_height ? src_cursor.y + 1 : 0;
		size_t src_y_count = dst_height < src_y_after_cursor ? dst_height : src_y_after_cursor;
		size_t src_y_from = src_y_after_cursor - src_y_count;
		TextPos dst_cursor = src_cursor;
		dst_cursor.y += src_y_from;
		newtextbuf->SetCursorEnabled(false);
		for ( size_t dst_y = 0; dst_y < dst_height; dst_y++ )
		{
			// TODO: Ability to center the boot cat.
			size_t src_y = src_y_from + dst_y;
			for ( size_t dst_x = 0; dst_x < dst_width; dst_x++ )
			{
				size_t src_x = dst_x;
				TextPos src_pos{src_x, src_y};
				TextPos dst_pos{dst_x, dst_y};
				TextChar tc{0, 0, 0, 0, 0};
				if ( src_x < src_width && src_y < src_height )
					tc = textbuf->GetChar(src_pos);
				else if ( src_width && src_height )
				{
					TextPos templ_pos;
					templ_pos.x = src_x < src_width ? src_x : src_width - 1;
					templ_pos.y = src_y < src_height ? src_y : src_height - 1;
					tc = textbuf->GetChar(templ_pos);
					tc.c = 0;
					tc.attr = 0;
				}
				newtextbuf->SetChar(dst_pos, tc);
				if ( src_x == src_cursor.x && src_y == src_cursor.y )
					dst_cursor = dst_pos;
			}
		}
		if ( dst_width <= dst_cursor.x )
			dst_cursor.x = dst_width ? dst_cursor.x - 1 : 0;
		if ( dst_height <= dst_cursor.y )
			dst_cursor.y = dst_height ? dst_cursor.y - 1 : 0;
		newtextbuf->SetCursorPos(dst_cursor);
		newtextbuf->SetCursorEnabled(cursor_enabled);
	}
	delete textbuf;
	textbuf = newtextbuf;
	kthread_mutex_unlock(&mutex);
}

} // namespace Sortix
