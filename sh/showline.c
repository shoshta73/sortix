/*
 * Copyright (c) 2011-2016, 2025-2026 Jonas 'Sortie' Termansen.
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
 * showline.c
 * Display a line on the terminal.
 */

#include <sys/ioctl.h>

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#include "showline.h"

struct wincurpos predict_cursor(struct cursor_predict* cursor_predict,
                                struct wincurpos* actual_out,
                                struct wincurpos wcp,
                                struct winsize ws,
                                wchar_t c)
{
	// TODO: This function assumes that wcwidth(c) == 1.

	if ( c == L'\0' )
	{
		*actual_out = wcp;
		return wcp;
	}

	// When a line is full, the cursor might be outside the window or on the
	// edge of the window, depending on the terminal. It is not on the next
	// line. If a escape sequence happens, it happens relative to this unknown
	// position. If a newline happens, it goes to the start of the next line.
	// Otherwise if a normal character happens, it is placed at the start of the
	// new line and the cursor goes to the second column (this is the case that
	// is implemented here).
	if ( !cursor_predict->escaped && c != L'\n' &&
	     (size_t) ws.ws_col <= wcp.wcp_col && wcp.wcp_row < (size_t) ws.ws_row )
	{
		wcp.wcp_col = 0;
		wcp.wcp_row = wcp.wcp_row + 1;
	}

	// Tell the caller where the character was actually positioned (which may
	// be different than the cursor position, due to the screen edge special
	// case above).
	*actual_out = wcp;

	// The cursor is not updated normally during escape sequences.
	if ( cursor_predict->escaped )
	{
		if ( (L'a' <= c && c <= L'z') || (L'A' <= c && c <= L'Z') )
			cursor_predict->escaped = false;
		return wcp;
	}

	// Start a escape sequence, which does not affect the cursor normally.
	if ( c == L'\e' )
	{
		cursor_predict->escaped = true;
		return wcp;
	}

	// Go to the new line as needed, otherwise increment the column.
	if ( c == L'\n' || (size_t) ws.ws_col <= wcp.wcp_col )
	{
		wcp.wcp_col = 0;
		if ( wcp.wcp_row + 1 < ws.ws_row )
			wcp.wcp_row++;
	}
	else
		wcp.wcp_col++;

	return wcp;
}

bool predict_will_scroll(struct cursor_predict cursor_predict,
                         struct wincurpos wcp,
                         struct winsize ws,
                         wchar_t c)
{
	if ( c == L'\0' )
		return false;
	if ( cursor_predict.escaped )
		return false;
	return (c == L'\n' || ws.ws_col <= wcp.wcp_col + 1) &&
	       !(wcp.wcp_row + 1 < ws.ws_row);
}

void show_line_begin(struct show_line* show_state, int out_fd)
{
	memset(show_state, 0, sizeof(*show_state));
	show_state->out_fd = out_fd;
	show_state->current_line = NULL;
	show_state->current_cursor = 0;
	ioctl(show_state->out_fd, TIOCGWINSZ, &show_state->ws);
	show_state->wcp_start.wcp_col = 0;
	show_state->wcp_start.wcp_row = 0;
	show_state->wcp_current.wcp_col = 0;
	show_state->wcp_current.wcp_row = 0;
	show_state->max_row = 0;
}

bool show_line_is_weird(const char* line)
{
	for ( size_t i = 0; line[i]; i++ )
	{
		if ( line[i] == '\e' )
		{
			i++;
			if ( line[i] != '[' )
				return true;
			i++;
			while ( ('0' <= line[i] && line[i] <= '9') || line[i] == ';' )
				i++;
			switch ( line[i] )
			{
			case 'm': break;
			default: return true;
			}
			continue;
		}

		switch ( line[i] )
		{
		case '\a': return true;
		case '\b': return true;
		case '\f': return true;
		case '\r': return true;
		case '\t': return true; // TODO: This isn't weird.
		case '\v': return true;
		default: break;
		}
	}

	return false;
}

void show_line_change_cursor(struct show_line* show_state, struct wincurpos wcp)
{
	// Keep track of how many lines we know actually exist. We can't use the
	// cursor down command '^[B' beyond this line, since we might hit the bottom
	// of the terminal which won't scroll. We will need to scroll instead.
	if ( show_state->max_row < show_state->wcp_current.wcp_row )
		show_state->max_row = show_state->wcp_current.wcp_row;

	// Do nothing if the cursor is already in the right position.
	if ( wcp.wcp_col == show_state->wcp_current.wcp_col &&
	     wcp.wcp_row == show_state->wcp_current.wcp_row )
		return;

	// Determine how many rows to move the cursor.
	int rel_y = (int) wcp.wcp_row - (int) show_state->wcp_current.wcp_row;
	char mov_y[32];
	mov_y[0] = '\0';

	// We might need to move to a line that might not exist. Use a newline
	// instead of a cursor movement, as that makes sure the terminal scrolls as
	// needed and that we know these lines definitely do exist.
	if ( show_state->max_row < wcp.wcp_row )
	{
		while ( show_state->wcp_current.wcp_row < wcp.wcp_row  )
		{
			write(show_state->out_fd, "\n", 1);
			show_state->wcp_current.wcp_row++;
		}
		show_state->max_row = show_state->wcp_current.wcp_row;
		show_state->wcp_current.wcp_col = 0;
	}
	// Move rows upwards.
	else if ( rel_y < 0 )
	{
		if ( rel_y == -1 )
			snprintf(mov_y, sizeof(mov_y), "\e[A");
		else
			snprintf(mov_y, sizeof(mov_y), "\e[%dA", -rel_y);
	}
	// Move rows downwards.
	else if ( rel_y > 0 )
	{
		if ( rel_y == 1 )
			snprintf(mov_y, sizeof(mov_y), "\e[B");
		else
			snprintf(mov_y, sizeof(mov_y), "\e[%dB", rel_y);
	}

	// Determine how many column to move the cursor.
	int rel_x = (int) wcp.wcp_col - (int) show_state->wcp_current.wcp_col;
	char mov_x[32];
	mov_x[0] = '\0';

	// If we're on the edge of the screen, we might be within the window or
	// outside the window depending on the terminal emulator. We cannot use
	// relative column movement in this case, so use absolute movement instead.
	if ( show_state->ws.ws_col <= show_state->wcp_current.wcp_col )
	{
		if ( !wcp.wcp_col )
			snprintf(mov_x, sizeof(mov_x), "\e[G");
		else
			snprintf(mov_x, sizeof(mov_x), "\e[%zuG", wcp.wcp_col + 1);
	}
	// Move columns backwards.
	else if ( rel_x < 0 )
	{
		if ( rel_x == -1 )
			snprintf(mov_x, sizeof(mov_x), "\e[D");
		else
			snprintf(mov_x, sizeof(mov_x), "\e[%dD", -rel_x);
	}
	// Move columns forward.
	else if ( rel_x > 0 )
	{
		if ( rel_x == 1 )
			snprintf(mov_x, sizeof(mov_x), "\e[C");
		else
			snprintf(mov_x, sizeof(mov_x), "\e[%dC", rel_x);
	}

	// Move the cursor. Combine the strings for some efficiency.
	char mov[64];
	strlcpy(mov, mov_x, sizeof(mov));
	strlcat(mov, mov_y, sizeof(mov));
	write(show_state->out_fd, mov, strlen(mov));

	show_state->wcp_current = wcp;
}

bool show_line_optimized(struct show_line* show_state, const char* line,
                         size_t cursor)
{
	struct winsize ws = show_state->ws;

	mbstate_t old_ps;
	mbstate_t new_ps;
	memset(&old_ps, 0, sizeof(old_ps));
	memset(&new_ps, 0, sizeof(new_ps));
	struct wincurpos old_wcp = show_state->wcp_start;
	struct wincurpos new_wcp = show_state->wcp_start;
	struct cursor_predict old_predict;
	struct cursor_predict new_predict;
	memset(&old_predict, 0, sizeof(old_predict));
	memset(&new_predict, 0, sizeof(new_predict));
	size_t old_line_offset = 0;
	size_t new_line_offset = 0;
	const char* old_line = show_state->current_line;
	const char* new_line = line;

	struct wincurpos cursor_wcp = show_state->wcp_start;

	// This is the optimized version of show_line, which attempts to do a quick
	// incremental update of the terminal state. It mutually increments through
	// both the old and the new prompt, looking for differences, and only writes
	// the needed changes to the terminal. If this algorithm fails, then the
	// fallback algorithm is used, which simply rerenders the whole prompt.
	while ( true )
	{
		// If the cursor is in this offset in the new prompt, then remember the
		// this position, and put the cursor here after the update.
		if ( cursor == new_line_offset )
			cursor_wcp = new_wcp;

		// Get the next wide character in both the old and new prompt. If one
		// of these characters is a newline/eof and the other is not, then we
		// may unread one of the characters below. The offsets are not in sync,
		// but the cursor positions are in sync (or correspond to empty space
		// not present in the other prompt).
		wchar_t old_wc;
		wchar_t new_wc;
		size_t old_num_bytes = mbrtowc(&old_wc, old_line + old_line_offset,
		                               SIZE_MAX, &old_ps);
		size_t new_num_bytes = mbrtowc(&new_wc, new_line + new_line_offset,
		                               SIZE_MAX, &new_ps);
		assert(old_num_bytes != (size_t) -2);
		assert(new_num_bytes != (size_t) -2);
		assert(old_num_bytes != (size_t) -1);
		assert(new_num_bytes != (size_t) -1);
		if ( old_num_bytes == 0 && new_num_bytes == 0 )
			break;

		bool will_scroll =
			predict_will_scroll(new_predict, new_wcp, ws, new_wc);
		bool can_scroll = show_state->wcp_start.wcp_row != 0;

		if ( will_scroll && !can_scroll )
		{
			if ( new_line_offset < cursor )
				cursor_wcp = new_wcp;
			break;
		}

		if ( predict_will_scroll(old_predict, old_wcp, ws, old_wc) )
			break;

		// Determine where the character was actually positioned, which may be
		// different from the old cursor (see predict_cursor), and predict the
		// next cursor position.
		struct wincurpos old_actual;
		struct wincurpos new_actual;
		struct wincurpos next_old_wcp =
			predict_cursor(&old_predict, &old_actual, old_wcp, ws, old_wc);
		struct wincurpos next_new_wcp =
			predict_cursor(&new_predict, &new_actual, new_wcp, ws, new_wc);

		// An update is only needed if there is any difference.
		if ( old_wc != new_wc ||
		     old_actual.wcp_row != new_actual.wcp_row ||
		     old_actual.wcp_col != new_actual.wcp_col )
		{
			// Match a newline with a newline, even if the actual columns are
			// difference, as we'll be in sync afterwards.
			if ( old_wc == L'\n' && new_wc == L'\n' )
			{
				// Good enough as newlines are invisible.
			}
			// If the old prompt has a newline, but the new prompt has a normal
			// character, then write the new character, and delay processing the
			// old prompt's newline.
			else if ( old_wc == L'\n' && new_wc != L'\0' )
			{
				show_line_change_cursor(show_state, new_actual);
				write(show_state->out_fd, new_line + new_line_offset,
				      new_num_bytes);
				show_state->wcp_current = next_new_wcp;
				old_num_bytes = 0;
				next_old_wcp = old_wcp;
			}
			// If the old prompt has a normal character, but the new prompt has
			// a newline, then we need to delete that character by overwriting
			// it with a space, and delay processing the new prompt's newline.
			else if ( old_wc != L'\0' && new_wc == '\n' )
			{
				show_line_change_cursor(show_state, old_actual);
				write(show_state->out_fd, " ", 1);
				show_state->wcp_current = next_old_wcp;
				new_num_bytes = 0;
				next_new_wcp = new_wcp;
			}
			// If the old prompt has a newline and the new prompt ends, then
			// we can allow that since the newline is invisible. But we may have
			// to bail out later if the old prompt had more data, but that's not
			// this case's problem.
			else if ( old_wc == L'\n' && new_wc == L'\0' )
			{
				// No need to do anything here as newlines are visible.
			}
			// If the old old ends, and the new prompt has a newline, then
			// simply render it.
			else if ( old_wc == L'\0' && new_wc == L'\n' )
			{
				show_line_change_cursor(show_state, new_actual);
				write(show_state->out_fd, new_line + new_line_offset,
				      new_num_bytes);
				show_state->wcp_current = next_new_wcp;
			}
			// If we somehow got desynchronized and ended up in a tricky case,
			// then stop this algorithm, fail, and simply rerender the whole
			// prompt with the fallback algorithm. We'll return to the optimized
			// algorithm on the next prompt update, which might succeed.
			else if ( old_wc != L'\0' &&
			          (old_actual.wcp_row != new_actual.wcp_row ||
			           old_actual.wcp_col != new_actual.wcp_col) )
				return false;
			// If new prompt does not have a character, and the old does not,
			// then simply erase the character.
			else if ( new_wc == L'\0' && old_wc != L'\0' )
			{
				show_line_change_cursor(show_state, old_actual);
				write(show_state->out_fd, " ", 1);
				show_state->wcp_current = next_old_wcp;
			}
			// Otherwise render the new character in the new prompt.
			else if ( new_wc != L'\0' )
			{
				show_line_change_cursor(show_state, new_actual);
				write(show_state->out_fd, new_line + new_line_offset,
				      new_num_bytes);
				show_state->wcp_current = next_new_wcp;
			}
		}

		if ( will_scroll && can_scroll )
		{
			cursor_wcp.wcp_row--;
			next_old_wcp.wcp_row--;
			show_state->wcp_start.wcp_row--;
		}

		old_wcp = next_old_wcp;
		new_wcp = next_new_wcp;

		old_line_offset += old_num_bytes;
		new_line_offset += new_num_bytes;
	}

	// Move the cursor to the right location now that the prompt is updated.
	show_line_change_cursor(show_state, cursor_wcp);

	// Remember the line for the next update.
	if ( show_state->current_line != line )
	{
		free(show_state->current_line);
		show_state->current_line = strdup(line);
		assert(show_state->current_line);
	}
	show_state->current_cursor = cursor;

	return true;
}

void show_line(struct show_line* show_state, const char* line, size_t cursor)
{
	// TODO: We don't currently invalidate on SIGWINCH.
	struct winsize ws;
	ioctl(show_state->out_fd, TIOCGWINSZ, &ws);
	if ( ws.ws_col != show_state->ws.ws_col ||
	     ws.ws_row != show_state->ws.ws_row )
	{
		show_state->invalidated = true;
		show_state->ws = ws;
	}

	// Attempt to do an optimized line re-rendering reusing the characters
	// already present on the console. Bail out if this turns out to be harder
	// than expected and re-render everything from scratch instead.
	if ( !show_state->invalidated &&
	     show_state->current_line &&
	     !show_line_is_weird(show_state->current_line) &&
	     !show_line_is_weird(line) )
	{
		if ( show_line_optimized(show_state, line, cursor) )
			return;
		show_state->invalidated = true;
	}

	// Move the cursor to the where the prompt begins.
	show_line_change_cursor(show_state, show_state->wcp_start);

	// Reset font attributes.
	dprintf(show_state->out_fd, "\e[m");

	// If we've been invalidated, clear the screen from here to the bottom.
	if ( show_state->invalidated || show_state->current_line )
		dprintf(show_state->out_fd, "\e[0J");

	struct cursor_predict cursor_predict;
	memset(&cursor_predict, 0, sizeof(cursor_predict));
	struct wincurpos wcp = show_state->wcp_start;
	struct wincurpos cursor_wcp = wcp;

	// Iterate the prompt one wide character at a time, and keep track of where
	// the cursor is located while so doing so.
	mbstate_t ps;
	memset(&ps, 0, sizeof(ps));
	for ( size_t i = 0; true; )
	{
		if ( cursor == i )
			cursor_wcp = wcp;
		wchar_t wc;
		size_t num_bytes = mbrtowc(&wc, line + i, SIZE_MAX, &ps);
		assert(num_bytes != (size_t) -2);
		assert(num_bytes != (size_t) -1);
		if ( num_bytes == 0 )
			break;
		bool will_scroll = predict_will_scroll(cursor_predict, wcp, ws, wc);
		bool can_scroll = show_state->wcp_start.wcp_row != 0;
		if ( will_scroll && !can_scroll )
		{
			if ( i < cursor )
				cursor_wcp = wcp;
			break;
		}
		write(show_state->out_fd, line + i, num_bytes);
		if ( will_scroll && can_scroll )
		{
			cursor_wcp.wcp_row--;
			show_state->wcp_start.wcp_row--;
		}
		struct wincurpos actual;
		wcp = predict_cursor(&cursor_predict, &actual, wcp, ws, wc);
		i += num_bytes;
	}

	// Move the cursor to the correct location.
	show_state->wcp_current = wcp;
	show_line_change_cursor(show_state, cursor_wcp);

	if ( show_state->current_line != line )
	{
		free(show_state->current_line);
		show_state->current_line = strdup(line);
		assert(show_state->current_line);
	}
	show_state->current_cursor = cursor;

	// Everything is good now.
	show_state->invalidated = false;
}

void show_line_end(struct show_line* show_state, const char* line)
{
	size_t cursor = line ? strlen(line) : 0;
	show_line(show_state, line, cursor);
}

void show_line_clear(struct show_line* show_state)
{
	dprintf(show_state->out_fd, "\e[H\e[2J");

	show_state->wcp_start.wcp_row = 0;
	show_state->wcp_start.wcp_col = 0;
	show_state->invalidated = true;
	show_state->max_row = 0;

	show_line_end(show_state, show_state->current_line);
}

void show_line_abort(struct show_line* show_state)
{
	free(show_state->current_line);
	show_state->current_line = NULL;
	show_state->current_cursor = 0;
}

void show_line_finish(struct show_line* show_state)
{
	show_line_end(show_state, show_state->current_line);
	dprintf(show_state->out_fd, "\n");

	show_line_abort(show_state);
}
