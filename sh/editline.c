/*
 * Copyright (c) 2011-2016, 2022 Jonas 'Sortie' Termansen.
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
 * editline.c
 * Read a line from the terminal.
 */

#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include "editline.h"
#include "showline.h"

#define CONTROL(x) (((x) - 64) & 127)

void edit_line_show(struct edit_line* edit_state)
{
	size_t line_length = 0;

	mbstate_t ps;
	memset(&ps, 0, sizeof(ps));

	line_length += strlen(edit_state->ps1);

	for ( size_t i = 0; i < edit_state->line_used; i++ )
	{
		char mb[MB_CUR_MAX];
		line_length += wcrtomb(mb, edit_state->line[i], &ps);
		if ( edit_state->line[i] == L'\n' )
			line_length += strlen(edit_state->ps2);
	}

	char* line = (char*) malloc(line_length + 1);
	assert(line);

	size_t cursor = 0;
	size_t line_offset = 0;
	memset(&ps, 0, sizeof(ps));

	strcpy(line + line_offset, edit_state->ps1);
	line_offset += strlen(edit_state->ps1);

	for ( size_t i = 0; i < edit_state->line_used; i++ )
	{
		if ( edit_state->line_offset == i )
			cursor = line_offset;
		line_offset += wcrtomb(line + line_offset, edit_state->line[i], &ps);
		if ( edit_state->line[i] == L'\n' )
		{
			strcpy(line + line_offset, edit_state->ps2);
			line_offset += strlen(edit_state->ps2);
		}
	}

	if ( edit_state->line_offset == edit_state->line_used )
		cursor = line_offset;

	line[line_offset] = '\0';

	show_line(&edit_state->show_state, line, cursor);

	free(line);
}

char* edit_line_result(struct edit_line* edit_state)
{
	size_t result_length = 0;

	mbstate_t ps;
	memset(&ps, 0, sizeof(ps));

	for ( size_t i = 0; i < edit_state->line_used; i++ )
	{
		char mb[MB_CUR_MAX];
		result_length += wcrtomb(mb, edit_state->line[i], &ps);
	}

	char* result = (char*) malloc(result_length + 1);
	if ( !result )
		return NULL;
	size_t result_offset = 0;

	memset(&ps, 0, sizeof(ps));

	for ( size_t i = 0; i < edit_state->line_used; i++ )
		result_offset += wcrtomb(result + result_offset, edit_state->line[i], &ps);

	result[result_offset] = '\0';

	return result;
}

bool edit_line_can_finish(struct edit_line* edit_state)
{
	if ( !edit_state->check_input_incomplete )
		return true;
	char* line = edit_line_result(edit_state);
	assert(line);
	bool result = !edit_state->check_input_incomplete(
		edit_state->check_input_incomplete_context, line);
	free(line);
	return result;
}

void edit_line_append_history(struct edit_line* edit_state, const char* line)
{
	if ( edit_state->history_used == edit_state->history_length )
	{
		size_t new_length = 2 * edit_state->history_length;
		if ( new_length == 0 )
			new_length = 16;
		// TODO: Use reallocarray instead of realloc.
		size_t new_size = sizeof(char*) * new_length;
		char** new_history = (char**) realloc(edit_state->history, new_size);
		assert(new_history);
		edit_state->history = new_history;
		edit_state->history_length = new_length;
	}

	size_t history_index = edit_state->history_used++;
	edit_state->history[history_index] = strdup(line);
	assert(edit_state->history[history_index]);
}

void edit_line_type_use_record(struct edit_line* edit_state, const char* record)
{
	free(edit_state->line);
	edit_state->line_offset = 0;
	edit_state->line_used = 0;
	edit_state->line_length = 0;

	size_t line_length;

	mbstate_t ps;
	memset(&ps, 0, sizeof(ps));

	size_t record_offset = 0;
	for ( line_length = 0; true; line_length++ )
	{
		size_t num_bytes = mbrtowc(NULL, record + record_offset, SIZE_MAX, &ps);
		assert(num_bytes != (size_t) -2);
		assert(num_bytes != (size_t) -1);
		if ( num_bytes == 0 )
			break;
		record_offset += num_bytes;
	}

	wchar_t* line = calloc(line_length, sizeof(wchar_t));
	assert(line);
	size_t line_used;

	memset(&ps, 0, sizeof(ps));

	record_offset = 0;
	for ( line_used = 0; line_used < line_length; line_used++ )
	{
		size_t num_bytes = mbrtowc(&line[line_used], record + record_offset, SIZE_MAX, &ps);
		assert(num_bytes != (size_t) -2);
		assert(num_bytes != (size_t) -1);
		assert(num_bytes != (size_t) 0);
		record_offset += num_bytes;
	}

	edit_state->line = line;
	edit_state->line_offset = line_used;
	edit_state->line_used = line_used;
	edit_state->line_length = line_length;
}

void edit_line_type_history_save_at(struct edit_line* edit_state, size_t index)
{
	assert(index <= edit_state->history_used);

	char* saved_line = edit_line_result(edit_state);
	assert(saved_line);
	if ( index == edit_state->history_used )
	{
		edit_line_append_history(edit_state, saved_line);
		free(saved_line);
	}
	else
	{
		free(edit_state->history[index]);
		edit_state->history[index] = saved_line;
	}
}

void edit_line_type_history_save_current(struct edit_line* edit_state)
{
	edit_line_type_history_save_at(edit_state, edit_state->history_offset);
}

void edit_line_type_history_prev(struct edit_line* edit_state)
{
	if ( edit_state->history_offset == 0 )
		return;

	edit_line_type_history_save_current(edit_state);

	const char* record = edit_state->history[--edit_state->history_offset];
	assert(record);
	edit_line_type_use_record(edit_state, record);
}

void edit_line_type_history_next(struct edit_line* edit_state)
{
	if ( edit_state->history_used - edit_state->history_offset <= 1 )
		return;

	edit_line_type_history_save_current(edit_state);

	const char* record = edit_state->history[++edit_state->history_offset];
	assert(record);
	edit_line_type_use_record(edit_state, record);
}

void edit_line_type_codepoint(struct edit_line* edit_state, wchar_t wc)
{
	if ( wc == L'\n' && edit_line_can_finish(edit_state))
	{
		if ( edit_state->line_used )
			edit_line_type_history_save_at(edit_state, edit_state->history_target);
		edit_state->editing = false;
		return;
	}

	if ( edit_state->line_used == edit_state->line_length )
	{
		size_t new_length = 2 * edit_state->line_length;
		if ( !new_length )
			new_length = 16;
		// TODO: Use reallocarray instead of realloc.
		size_t new_size = sizeof(wchar_t) * new_length;
		wchar_t* new_line = (wchar_t*) realloc(edit_state->line, new_size);
		assert(new_line);
		edit_state->line = new_line;
		edit_state->line_length = new_length;
	}

	assert(edit_state->line_offset <= edit_state->line_used);
	assert(edit_state->line_used <= edit_state->line_length);

	for ( size_t i = edit_state->line_used; i != edit_state->line_offset; i-- )
		edit_state->line[i] = edit_state->line[i-1];

	edit_state->line[edit_state->line_used++, edit_state->line_offset++] = wc;

	assert(edit_state->line_offset <= edit_state->line_used);
	assert(edit_state->line_used <= edit_state->line_length);
}

void edit_line_type_home(struct edit_line* edit_state)
{
	edit_state->line_offset = 0;
}

void edit_line_type_left(struct edit_line* edit_state)
{
	if ( edit_state->line_offset == 0 )
		return;
	edit_state->line_offset--;
}

void edit_line_type_right(struct edit_line* edit_state)
{
	if ( edit_state->line_offset == edit_state->line_used )
		return;
	edit_state->line_offset++;
}

void edit_line_type_end(struct edit_line* edit_state)
{
	edit_state->line_offset = edit_state->line_used;
}

void edit_line_type_backspace(struct edit_line* edit_state)
{
	if ( edit_state->line_offset == 0 )
		return;
	edit_state->line_used--;
	edit_state->line_offset--;
	for ( size_t i = edit_state->line_offset; i < edit_state->line_used; i++ )
		edit_state->line[i] = edit_state->line[i+1];
}

void edit_line_type_previous_word(struct edit_line* edit_state)
{
	while ( edit_state->line_offset &&
	        iswspace(edit_state->line[edit_state->line_offset-1]) )
		edit_state->line_offset--;
	while ( edit_state->line_offset &&
	        !iswspace(edit_state->line[edit_state->line_offset-1]) )
		edit_state->line_offset--;
}

void edit_line_type_next_word(struct edit_line* edit_state)
{
	while ( edit_state->line_offset != edit_state->line_used &&
	        iswspace(edit_state->line[edit_state->line_offset]) )
		edit_state->line_offset++;
	while ( edit_state->line_offset != edit_state->line_used &&
	        !iswspace(edit_state->line[edit_state->line_offset]) )
		edit_state->line_offset++;
}

void edit_line_type_delete(struct edit_line* edit_state)
{
	if ( edit_state->line_offset == edit_state->line_used )
		return;
	edit_state->line_used--;
	for ( size_t i = edit_state->line_offset; i < edit_state->line_used; i++ )
		edit_state->line[i] = edit_state->line[i+1];
}

void edit_line_type_eof_or_delete(struct edit_line* edit_state)
{
	if ( edit_state->line_used )
		return edit_line_type_delete(edit_state);
	edit_state->editing = false;
	edit_state->eof_condition = true;
	if ( edit_state->trap_eof_opportunity )
		edit_state->trap_eof_opportunity(edit_state->trap_eof_opportunity_context);
}

void edit_line_type_interrupt(struct edit_line* edit_state)
{
	dprintf(edit_state->out_fd, "^C\n");
	edit_state->editing = false;
	edit_state->abort_editing = true;
}

void edit_line_type_kill_after(struct edit_line* edit_state)
{
	while ( edit_state->line_offset < edit_state->line_used )
		edit_line_type_delete(edit_state);
}

void edit_line_type_kill_before(struct edit_line* edit_state)
{
	while ( edit_state->line_offset )
		edit_line_type_backspace(edit_state);
}

void edit_line_type_clear(struct edit_line* edit_state)
{
	show_line_clear(&edit_state->show_state);
}

void edit_line_type_delete_word_before(struct edit_line* edit_state)
{
	while ( edit_state->line_offset &&
	        iswspace(edit_state->line[edit_state->line_offset-1]) )
		edit_line_type_backspace(edit_state);
	while ( edit_state->line_offset &&
	        !iswspace(edit_state->line[edit_state->line_offset-1]) )
		edit_line_type_backspace(edit_state);
}

int edit_line_completion_sort(const void* a_ptr, const void* b_ptr)
{
	const char* a = *(const char**) a_ptr;
	const char* b = *(const char**) b_ptr;
	return strcmp(a, b);
}

void edit_line_type_complete(struct edit_line* edit_state)
{
	if ( !edit_state->complete )
		return;

	char* partial = edit_line_result(edit_state);
	if ( !partial )
		return;

	mbstate_t ps;
	memset(&ps, 0, sizeof(ps));

	size_t complete_at = 0;
	for ( size_t i = 0; i < edit_state->line_offset; i++ )
	{
		char mb[MB_CUR_MAX];
		size_t num_bytes = wcrtomb(mb, edit_state->line[i], &ps);
		assert(num_bytes != (size_t) -1);
		assert(num_bytes != (size_t)  0);
		complete_at += num_bytes;
	}

	char** completions;
	size_t used_before;
	size_t used_after;
	size_t num_completions = edit_state->complete(
		&completions,
		&used_before,
		&used_after,
		edit_state->complete_context,
		partial,
		complete_at);

	qsort(completions, num_completions, sizeof(char*), edit_line_completion_sort);

	size_t lcp = 0;
	bool similar = true;
	while ( num_completions && similar )
	{
		char c = completions[0][lcp];
		if ( c == '\0' )
			break;
		for ( size_t i = 1; similar && i < num_completions; i++ )
		{
			if ( completions[i][lcp] != c )
				similar = false;
		}
		if ( similar )
			lcp++;
	}

	bool prefix_ends_with_slash = false;
	memset(&ps, 0, sizeof(ps));
	for ( size_t i = 0; i < lcp; )
	{
		const char* completion = completions[0];
		wchar_t wc;
		size_t num_bytes = mbrtowc(&wc, completion + i, lcp - i, &ps);
		if ( num_bytes == (size_t) -2 )
			break;
		assert(num_bytes != (size_t) -1);
		assert(num_bytes != (size_t)  0);
		edit_line_type_codepoint(edit_state, wc);
		prefix_ends_with_slash = wc == L'/';
		i += num_bytes;
	}

	if ( num_completions == 1 && !prefix_ends_with_slash )
	{
		edit_line_type_codepoint(edit_state, ' ');
	}

	if ( 2 <= num_completions && lcp == 0 && edit_state->double_tab )
	{
		bool first = true;
		for ( size_t i = 0; i < num_completions; i++ )
		{
			const char* completion = completions[i];
			size_t length = used_before + strlen(completion) + used_after;
			if ( !length )
				continue;
			if ( first )
				show_line_finish(&edit_state->show_state);
			// TODO: Use a reliable write.
			if ( !first )
				write(edit_state->out_fd, " ", 1);
			write(edit_state->out_fd, partial + complete_at - used_before, used_before);
			write(edit_state->out_fd, completion, strlen(completion));
			write(edit_state->out_fd, partial + complete_at, used_after);
			first = false;
		}
		if ( !first)
		{
			write(edit_state->out_fd, "\n", 1);
			show_line_begin(&edit_state->show_state, edit_state->out_fd);
			edit_line_show(edit_state);
		}
	}

	edit_state->double_tab = true;

	(void) used_before;
	(void) used_after;

	for ( size_t i = 0; i < num_completions; i++ )
		free(completions[i]);
	free(completions);

	free(partial);
}

static size_t get_histsize(void)
{
	const char* histfile = getenv("HISTSIZE");
	if ( histfile && isdigit(*histfile) )
	{
		errno = 0;
		char* end;
		size_t value = strtoul(histfile, &end, 10);
		// Enforce a reasonable upper limit to avoid OOM on misconfigurations,
		// when users try to have unlimited history size, but the below saving
		// will allocate an array of this size.
		if ( 1048576 <= value )
			value = 1048576;
		if ( !errno && !*end )
			return value;
	}
	return 500;
}

bool edit_line_history_load(struct edit_line* edit_state, const char* path)
{
	if ( !path )
		return true;
	FILE* fp = fopen(path, "r");
	if ( !fp )
	{
		if ( errno == ENOENT )
			return true;
		warn("%s", path);
		return false;
	}
	char* line = NULL;
	size_t line_size;
	ssize_t line_length;
	while ( 0 < (line_length = getline(&line, &line_size, fp)) )
	{
		if ( line[line_length - 1] == '\n' )
			line[--line_length] = '\0';
		edit_line_append_history(edit_state, line);
	}
	edit_state->history_begun = edit_state->history_used;
	if ( ferror(fp) )
		warn("read: %s", path);
	free(line);
	fclose(fp);
	return true;
}

bool edit_line_history_save(struct edit_line* edit_state, const char* path)
{
	size_t histsize = get_histsize();
	if ( !path || !histsize )
		return true;
	// Avoid replacing the null device if used to disable the history.
	if ( !strcmp(path, "/dev/null") )
		return true;
	// TODO: File locking is a better alternative to replacing the actual file.
	//       A temporary file rename is used to atomically replace the contents
	//       but may lose the race with other processes.
	char* tmp;
	if ( asprintf(&tmp, "%s.XXXXXXXXX", path) < 0 )
	{
		warn("malloc");
		return false;
	}
	int fd = mkstemp(tmp);
	if ( fd < 0 )
	{
		if ( errno == EROFS )
			return true;
		warn("%s", path);
		return false;
	}
	FILE* fpout = fdopen(fd, "w");
	if ( !fpout )
	{
		warn("%s", path);
		close(fd);
		unlink(tmp);
		free(tmp);
		return false;
	}
	// Merge with any updated history.
	bool success = true;
	char** history = calloc(histsize, sizeof(char*));
	if ( !history )
		warn("malloc"), success = false;
	size_t first = 0;
	size_t used = 0;
	FILE* fpin;
	if ( success && (fpin = fopen(path, "r")) )
	{
		char* line = NULL;
		size_t line_size;
		ssize_t line_length;
		while ( 0 < (line_length = getline(&line, &line_size, fpin)) )
		{
			if ( line[line_length - 1] == '\n' )
				line[--line_length] = '\0';
			size_t n = (first + used) % histsize;
			if ( history[n] )
				free(history[n]);
			history[n] = line;
			if ( used == histsize )
				first = (first + 1) % histsize;
			else
				used++;
			line = NULL;
		}
		if ( ferror(fpin) )
			warn("read: %s", path), success = false;
		fclose(fpin);
	}
	else if ( errno != ENOENT )
		warn("%s", path);
	for ( size_t i = edit_state->history_begun;
	      success && i < edit_state->history_used;
	      i++ )
	{
		char* line = strdup(edit_state->history[i]);
		if ( !line )
		{
			warn("malloc");
			success = false;
			break;
		}
		size_t n = (first + used) % histsize;
		if ( history[n] )
			free(history[n]);
		history[n] = line;
		if ( used == histsize )
			first = (first + 1) % histsize;
		else
			used++;
		line = NULL;
	}
	for ( size_t i = 0; i < used; i++ )
	{
		size_t n = (first + i) % histsize;
		char* line = history[n];
		if ( success && fprintf(fpout, "%s\n", line) < 0 )
			warn("%s", tmp), success = false;
		free(line);
	}
	int ret = fclose(fpout);
	if ( success && ret == EOF )
		warn("%s", path), success = false;
	if ( success && rename(tmp, path) < 0 )
		warn("rename: %s -> %s", tmp, path), success = false;
	if ( !success )
		unlink(tmp);
	free(tmp);
	return success;
}

#define SORTIX_LFLAGS (ISORTIX_KBKEY | ISORTIX_32BIT)

void edit_line(struct edit_line* edit_state)
{
	edit_state->editing = true;
	edit_state->abort_editing = false;
	edit_state->eof_condition = false;
	edit_state->double_tab = false;

	free(edit_state->line);
	edit_state->line = NULL;
	edit_state->line_offset = 0;
	edit_state->line_used = 0;
	edit_state->line_length = 0;
	edit_state->history_offset = edit_state->history_used;
	edit_state->history_target = edit_state->history_used;

	struct termios old_tio, tio;
	tcgetattr(edit_state->in_fd, &old_tio);

	// TODO: There's no good way in Sortix for processes to restore the terminal
	//       attributes on exit, even if that exit is a crash. Restore default
	//       terminal attributes here to ensure programs run in the expected
	//       terminal environment, if transitional Sortix extensions are
	//       enabled. This ensures programs are run in a conforming environment
	//       even if a process using Sortix extensions don't exit cleanly.
	if ( old_tio.c_lflag & SORTIX_LFLAGS )
	{
		old_tio.c_lflag &= ~SORTIX_LFLAGS;
		old_tio.c_lflag |= ECHO | ECHOE | ECHOK | ICANON | IEXTEN | ISIG;
		old_tio.c_iflag |= ICRNL;
		old_tio.c_oflag &= ~(OCRNL);
		old_tio.c_oflag |= OPOST | ONLCR;
	}

	memcpy(&tio, &old_tio, sizeof(tio));
	tio.c_lflag &= ~(ISORTIX_KBKEY | ISORTIX_32BIT);
	tio.c_lflag &= ~(ISIG | ICANON | ECHO | IEXTEN);

	tcsetattr(edit_state->in_fd, TCSANOW, &tio);

	show_line_begin(&edit_state->show_state, edit_state->out_fd);

	int escape = 0;
	unsigned int params[16];
	size_t param_index = 0;

	mbstate_t ps = { 0 };
	while ( edit_state->editing )
	{
		edit_line_show(edit_state);

		char c;
		if ( read(0, &c, sizeof(c)) != sizeof(c) )
		{
			edit_state->error_condition = true;
			break;
		}

		if ( c != '\t' )
			edit_state->double_tab = false;

		if ( escape )
		{
			if ( c == '[' )
			{
				escape = 2;
			}
			else if ( escape == 1 && c == 'O' )
			{
				escape = 3;
			}
			else if ( '0' <= c && c <= '9' )
			{
				params[param_index] *= 10;
				params[param_index] += c - '0';
			}
			else if ( c == ';' )
			{
				if ( param_index < 16 )
					++param_index;
			}
			else if ( 64 <= c && c <= 126 )
			{
				for ( size_t i = 0; i < 16; i++ )
					if ( params[i] == 0 )
						params[i] = 1;
				switch ( c )
				{
				case 'A': edit_line_type_history_prev(edit_state); break;
				case 'B': edit_line_type_history_next(edit_state); break;
				case 'C':
					if ( (params[1] - 1) & (1 << 2) ) /* control */
						edit_line_type_next_word(edit_state);
					else
						edit_line_type_right(edit_state);
					break;
				case 'D':
					if ( (params[1] - 1) & (1 << 2) ) /* control */
						edit_line_type_previous_word(edit_state);
					else
						edit_line_type_left(edit_state);
					break;
				case 'F': edit_line_type_end(edit_state); break;
				case 'H': edit_line_type_home(edit_state); break;
				case 'R':
				{
					unsigned int r = params[0] - 1;
					unsigned int c = params[1] - 1;
					show_line_wincurpos(&edit_state->show_state, r, c);
					edit_line_show(edit_state);
				} break;
				case '~':
					if ( params[0] == 1 )
						edit_line_type_home(edit_state);
					if ( params[0] == 3 )
						edit_line_type_delete(edit_state);
					if ( params[0] == 4 )
						edit_line_type_end(edit_state);
					break;
				}
				escape = 0;
			}
		}
		else if ( c == CONTROL('A') )
			edit_line_type_home(edit_state);
		else if ( c == CONTROL('B') )
			edit_line_type_left(edit_state);
		else if ( c == CONTROL('C') )
			edit_line_type_interrupt(edit_state);
		else if ( c == CONTROL('D') )
			edit_line_type_eof_or_delete(edit_state);
		else if ( c == CONTROL('E') )
			edit_line_type_end(edit_state);
		else if ( c == CONTROL('F') )
			edit_line_type_right(edit_state);
		else if ( c == CONTROL('I') )
			edit_line_type_complete(edit_state);
		else if ( c == CONTROL('K') )
			edit_line_type_kill_after(edit_state);
		else if ( c == CONTROL('L') )
			show_line_clear(&edit_state->show_state);
		else if ( c == CONTROL('U') )
			edit_line_type_kill_before(edit_state);
		else if ( c == CONTROL('W') )
			edit_line_type_delete_word_before(edit_state);
		else if ( c == CONTROL('[') )
		{
			param_index = 0;
			memset(params, 0, sizeof(params));
			escape = 1;
		}
		else if ( c == 127 )
			edit_line_type_backspace(edit_state);
		else
		{
			wchar_t wc;
			size_t amount = mbrtowc(&wc, &c, 1, &ps);
			if ( amount == (size_t) -2 )
				continue;
			if ( amount == (size_t) -1 )
				wc = 0xFFFD; /* REPLACEMENT CHARACTER */
			if ( amount == 0 )
				continue;
			edit_line_type_codepoint(edit_state, wc);
		}
	}

	if ( edit_state->abort_editing )
		show_line_abort(&edit_state->show_state);
	else
	{
		edit_line_show(edit_state);
		show_line_finish(&edit_state->show_state);
	}

	tcsetattr(edit_state->in_fd, TCSANOW, &old_tio);
}
