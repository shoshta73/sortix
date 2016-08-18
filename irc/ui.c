/*
 * Copyright (c) 2016 Jonas 'Sortie' Termansen.
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
 * ui.c
 * User Interface.
 */

#include <sys/ioctl.h>

#include <err.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <wchar.h>

#include "connection.h"
#include "network.h"
#include "scrollback.h"
#include "ui.h"

struct cell
{
	wchar_t c;
	int fgcolor;
	int bgcolor;
};

static struct termios saved_termios;

void tty_show(struct cell* cells, size_t cols, size_t rows)
{
	printf("\e[H");
	int fgcolor = -1;
	int bgcolor = -1;
	mbstate_t ps;
	memset(&ps, 0, sizeof(ps));
	for ( size_t r = 0; r < rows; r++ )
	{
		for ( size_t c = 0; c < cols; c++ )
		{
			struct cell* cell = &cells[r * cols + c];
			if ( fgcolor != cell->fgcolor )
			{
				printf("\e[%im", cell->fgcolor);
				fgcolor = cell->fgcolor;
			}
			if ( bgcolor != cell->bgcolor )
			{
				printf("\e[%im", cell->bgcolor);
				bgcolor = cell->bgcolor;
			}
			char mb[MB_CUR_MAX];
			size_t amount = wcrtomb(mb, cell->c, &ps);
			if ( amount == (size_t) -1 )
				continue;
			fwrite(mb, 1, amount, stdout);
		}
		if ( r + 1 != rows )
			printf("\n");
	}
	fflush(stdout);
}

void on_sigquit(int sig)
{
	// TODO: This is not async signal safe.
	ui_destroy(NULL);
	// TODO: Use sigaction so the handler only runs once.
	//raise(sig);
	(void) sig;
	raise(SIGKILL);
}

void ui_initialize(struct ui* ui, struct network* network)
{
	memset(ui, 0, sizeof(*ui));
	ui->network = network;
	ui->current = find_scrollback_network(network);
	struct winsize ws;
	if ( ioctl(1, TIOCGWINSZ, &ws) < 0 )
		err(1, "stdout: ioctl: TIOCGWINSZ");
	if ( tcgetattr(0, &saved_termios) < 0 )
		err(1, "stdin: tcgetattr");
	struct termios tcattr;
	memcpy(&tcattr, &saved_termios, sizeof(struct termios));
	tcattr.c_lflag &= ~(ECHO | ICANON | IEXTEN);
	tcattr.c_iflag |= ICRNL | ISIG;
	tcattr.c_cc[VMIN] = 1;
	tcattr.c_cc[VTIME] = 0;
	signal(SIGINT, SIG_IGN);
	signal(SIGQUIT, on_sigquit);
	tcsetattr(0, TCSADRAIN, &tcattr);
	if ( getenv("TERM") && strcmp(getenv("TERM"), "sortix") != 0 )
	{
		printf("\e[?1049h");
		fflush(stdout);
	}
}

void ui_destroy(struct ui* ui)
{
	// TODO.
	(void) ui;
	// TODO: This should be done in an atexit handler as well.
	if ( getenv("TERM") && strcmp(getenv("TERM"), "sortix") != 0 )
	{
		printf("\e[?1049l");
		fflush(stdout);
	}
	tcsetattr(0, TCSADRAIN, &saved_termios);
}

void increment_offset(size_t* o_ptr, size_t* line_ptr, size_t cols)
{
	if ( (*o_ptr)++ == cols )
	{
		*o_ptr = 0;
		(*line_ptr)++;
	}
}

void ui_render(struct ui* ui)
{
	mbstate_t ps;
	struct winsize ws;
	if ( ioctl(1, TIOCGWINSZ, &ws) < 0 )
		err(1, "stdout: ioctl: TIOCGWINSZ");
	size_t cols = ws.ws_col;
	size_t rows = ws.ws_row;

	struct cell* cells = calloc(sizeof(struct cell) * cols, rows);
	if ( !cells )
		err(1, "calloc");

	for ( size_t r = 0; r < rows; r++ )
	{
		for ( size_t c = 0; c < cols; c++ )
		{
			struct cell* cell = &cells[r * cols + c];
			cell->c = L' ';
			cell->fgcolor = 0;
			cell->bgcolor = 0;
		}
	}

	// TODO: What if the terminal isn't large enough?
	struct scrollback* sb = ui->current;

	sb->activity = ACTIVITY_NONE;

	size_t title_from = 0;
	size_t when_offset = 0;
	size_t when_width = 2 + 1 + 2 + 1 + 2;
	size_t who_offset = when_offset + when_width + 1;
	size_t who_width = sb->who_width;
	size_t div_offset = who_offset + who_width + 1;
	size_t what_offset = div_offset + 2;
	size_t what_width = cols - what_offset;
	size_t input_width = cols;

	size_t input_num_lines = 1;
	for ( size_t i = 0, o = 0; i < ui->input_used; i++ )
	{
		wchar_t wc = ui->input[i];
		int w = wcwidth(wc);
		if ( w < 0 || w == 0 )
			continue;
		if ( input_width <= o )
		{
			input_num_lines++;
			o = 0;
		}
		o += w;
	}

	char* title;
	if ( asprintf(&title, "%s @ %s / %s", ui->network->nick,
	              ui->network->server_hostname, ui->current->name) < 0 )
		err(1, "asprintf");
	size_t title_len = strlen(title);
	size_t title_how_many = cols < title_len ? cols : title_len;
	size_t title_offset = (cols - title_how_many) / 2;
	for ( size_t i = 0; i < title_how_many; i++ )
	{
		char c = title[i];
		size_t cell_r = title_from;
		size_t cell_c = title_offset + i;
		struct cell* cell = &cells[cell_r * cols + cell_c];
		cell->c = btowc((unsigned char) c);
	}
	free(title);

	size_t scrollbacks_from = title_from + 1;
	size_t scrollbacks_lines = 1;
	size_t scrollbacks_o = 0;
	for ( struct scrollback* iter = ui->network->scrollbacks;
	      iter;
	      iter = iter->scrollback_next )
	{
		if ( iter->scrollback_prev )
		{
			increment_offset(&scrollbacks_o, &scrollbacks_lines, cols);
			increment_offset(&scrollbacks_o, &scrollbacks_lines, cols);
		}
		for ( size_t i = 0; iter->name[i]; i++ )
		{
			char c = iter->name[i];
			size_t cell_r = scrollbacks_from + (scrollbacks_lines - 1);
			size_t cell_c = scrollbacks_o;
			struct cell* cell = &cells[cell_r * cols + cell_c];
			int fgcolor = 0;
			if ( iter == sb )
				fgcolor = 1; // TODO: Boldness should be its own property.
			else if ( iter->activity == ACTIVITY_NONTALK )
				fgcolor = 31;
			else if ( iter->activity == ACTIVITY_TALK )
				fgcolor = 91;
			else if ( iter->activity == ACTIVITY_HIGHLIGHT )
				fgcolor = 94;
			cell->c = btowc((unsigned char) c);
			cell->fgcolor = fgcolor;
			increment_offset(&scrollbacks_o, &scrollbacks_lines, cols);
		}
	}

	size_t horhigh_from = scrollbacks_from + scrollbacks_lines;

	for ( size_t c = 0; c < cols; c++ )
	{
		size_t cell_r = horhigh_from;
		size_t cell_c = c;
		struct cell* cell = &cells[cell_r * cols + cell_c];
		cell->c = c == div_offset ? L'┬' : L'─';
	}

	size_t sb_from = horhigh_from + 1;

	// TODO: What if the input is too big?
	size_t input_bottom = rows - input_num_lines;
	size_t input_offset = 0;
	for ( size_t i = 0, o = 0, line = 0; i < ui->input_used; i++ )
	{
		wchar_t wc = ui->input[i];
		int w = wcwidth(wc);
		if ( w < 0 || w == 0 )
			continue;
		if ( input_width <= o )
		{
			line++;
			o = 0;
		}
		// TODO: If 1 < w.
		size_t cell_r = input_bottom + line;
		size_t cell_c = input_offset + o;
		struct cell* cell = &cells[cell_r * cols + cell_c];
		cell->c = wc;
		o += w;
	}

	size_t horlow_from = input_bottom - 1;

	for ( size_t c = 0; c < cols; c++ )
	{
		size_t cell_r = horlow_from;
		size_t cell_c = c;
		struct cell* cell = &cells[cell_r * cols + cell_c];
		cell->c = c == div_offset ? L'┴' : L'─';
	}

	size_t sb_to = horlow_from;

	for ( size_t r = sb_to - 1; r != sb_from - 1; r-- )
	{
		size_t cell_r = r;
		size_t cell_c = div_offset;
		struct cell* cell = &cells[cell_r * cols + cell_c];
		cell->c = L'│';
	}

	for ( size_t r = sb_to - 1, m = sb->messages_count - 1;
	      r != (sb_from - 1) && m != SIZE_MAX;
	      r--, m-- )
	{
		struct message* msg = &sb->messages[m];
		size_t num_lines = 1;
		size_t max_lines = sb_from - r + 1;
		memset(&ps, 0, sizeof(ps));
		for ( size_t i = 0, o = 0; msg->what[i]; )
		{
			wchar_t wc;
			size_t amount = mbrtowc(&wc, msg->what + i, SIZE_MAX, &ps);
			if ( amount == (size_t) -1 || amount == (size_t) -2 )
			{
				// TODO.
				memset(&ps, 0, sizeof(ps));
				continue;
			}
			i += amount;
			int w = wcwidth(wc);
			if ( w < 0 || w == 0 )
				continue;
			if ( what_width <= o )
			{
				num_lines++;
				o = 0;
			}
			o += w;
		}
		size_t how_many_lines = max_lines < num_lines ? max_lines : num_lines;
		size_t first_line = num_lines - how_many_lines;
		if ( 1 < how_many_lines )
			r -= how_many_lines - 1;
		if ( first_line == 0 )
		{
			char when[2 + 1 + 2 + 1 + 2 + 1 + 1];
			snprintf(when, sizeof(when), "%02i:%02i:%02i ",
			         msg->hour, msg->min, msg->sec);
			for ( size_t i = 0; when[i]; i++ )
			{
				size_t cell_r = r;
				size_t cell_c = when_offset + i;
				struct cell* cell = &cells[cell_r * cols + cell_c];
				cell->c = btowc((unsigned char) when[i]);
			}
			memset(&ps, 0, sizeof(ps));
			size_t msg_who_width = strlen(msg->who);
			size_t msg_who_how_many = who_width < msg_who_width ? who_width : msg_who_width;
			size_t msg_who_first = msg_who_width - msg_who_how_many;
			size_t msg_who_offset = who_width - msg_who_how_many;
			for ( size_t i = 0; i < msg_who_how_many; i++ )
			{
				char c = msg->who[msg_who_first + i];
				size_t cell_r = r;
				size_t cell_c = who_offset + msg_who_offset + i;
				struct cell* cell = &cells[cell_r * cols + cell_c];
				cell->c = btowc((unsigned char) c);
			}
		}
		for ( size_t i = 0, o = 0, line = 0; msg->what[i]; )
		{
			wchar_t wc;
			size_t amount = mbrtowc(&wc, msg->what + i, SIZE_MAX, &ps);
			if ( amount == (size_t) -1 || amount == (size_t) -2 )
			{
				// TODO.
				memset(&ps, 0, sizeof(ps));
				continue;
			}
			i += amount;
			int w = wcwidth(wc);
			if ( w < 0 || w == 0 )
				continue;
			if ( what_width <= o )
			{
				line++;
				o = 0;
			}
			// TODO: If 1 < w.
			if ( first_line <= line )
			{
				size_t cell_r = r + line - first_line;
				size_t cell_c = what_offset + o;
				struct cell* cell = &cells[cell_r * cols + cell_c];
				cell->c = wc;
			}
			o += w;
		}
	}

	(void) ui;

	tty_show(cells, cols, rows);

	free(cells);
}

static bool is_command(const char* input,
                       const char* cmd,
                       const char** param)
{
	size_t cmdlen = strlen(cmd);
	if ( strncmp(input, cmd, cmdlen) != 0 )
		return false;
	if ( !input[cmdlen] )
	{
		if ( param )
			*param = NULL;
		return true;
	}
	if ( input[cmdlen] != ' ' )
		return false;
	if ( !param )
		return false;
	*param = input + cmdlen + 1;
	return true;
}

static bool is_command_param(const char* input,
                             const char* cmd,
                             const char** param)
{
	if ( !is_command(input, cmd, param) )
		return false;
	if ( !*param )
		return false; // TODO: Help message in scrollback.
	return true;
}

void ui_input_char(struct ui* ui, char c)
{
	wchar_t wc;
	size_t amount = mbrtowc(&wc, &c, 1, &ui->input_ps);
	if ( amount == (size_t) -2 )
		return;
	if ( amount == (size_t) -1 )
	{
		// TODO.
		memset(&ui->input_ps, 0, sizeof(ui->input_ps));
		return;
	}
	if ( wc == L'\b' || wc == 127 )
	{
		if ( 0 < ui->input_used )
			ui->input_used--;
	}
	else if ( wc == L'\f' /* ^L */ )
	{
		scrollback_clear(ui->current);
		// TODO: Schedule full redraw?
	}
	else if ( wc == L'\n' )
	{
		char input[4 * sizeof(ui->input) / sizeof(ui->input[0])];
		mbstate_t ps;
		memset(&ps, 0, sizeof(ps));
		const wchar_t* wcs = ui->input;
		size_t amount = wcsnrtombs(input, &wcs, ui->input_used, sizeof(input), &ps);
		ui->input_used = 0;
		if ( amount == (size_t) -1 )
			return;
		input[amount < sizeof(input) ? amount : amount - 1] = '\0';
		struct irc_connection* conn = ui->network->irc_connection;
		const char* who = ui->network->nick;
		const char* where = ui->current->name;
		const char* param;
		if ( input[0] == '/' && input[1] != '/' )
		{
			if ( !input[1] )
				return;
			if ( is_command_param(input, "/w", &param) ||
			     is_command_param(input, "/window", &param) )
			{
				struct scrollback* sb = find_scrollback(ui->network, param);
				if ( sb )
					ui->current = sb;
			}
			else if ( is_command_param(input, "/query", &param) )
			{
				if ( param[0] == '#' )
					return; // TODO: Help in scrollback.
				struct scrollback* sb = get_scrollback(ui->network, param);
				if ( sb )
					ui->current = sb;
			}
			else if ( is_command_param(input, "/join", &param) )
			{
				irc_command_join(conn, param);
				struct scrollback* sb = get_scrollback(ui->network, param);
				if ( sb )
					ui->current = sb;
			}
			// TODO: Make it default to the current channel if any.
			else if ( is_command_param(input, "/part", &param) )
			{
				irc_command_part(conn, param);
			}
			else if ( is_command(input, "/quit", &param) )
			{
				irc_command_quit(conn, param ? param : "Quiting");
			}
			else if ( is_command_param(input, "/nick", &param) )
			{
				irc_command_nick(conn, param);
			}
			else if ( is_command_param(input, "/raw", &param) )
			{
				irc_transmit_string(conn, param);
			}
			else if ( is_command_param(input, "/me", &param) )
			{
				scrollback_printf(ui->current, ACTIVITY_NONE, "*", "%s %s",
				                  who, param);
				irc_command_privmsgf(conn, where, "\x01""ACTION %s""\x01",
				                     param);
			}
			else if ( is_command(input, "/clear", &param) )
			{
				scrollback_clear(ui->current);
			}
			// TODO: /ban
			// TODO: /ctcp
			// TODO: /deop
			// TODO: /devoice
			// TODO: /kick
			// TODO: /mode
			// TODO: /op
			// TODO: /quiet
			// TODO: /topic
			// TODO: /voice
			else
			{
				scrollback_printf(ui->current, ACTIVITY_NONE, "*",
				                  "%s :Unknown command", input + 1);
			}
		}
		else
		{
			const char* what = input;
			if ( what[0] == '/' )
				what++;
			scrollback_print(ui->current, ACTIVITY_NONE, who, what);
			irc_command_privmsg(conn, where, what);
		}
	}
	else
	{
		if ( ui->input_used < sizeof(ui->input) / sizeof(ui->input[0]) )
			ui->input[ui->input_used++] = wc;
	}
}
