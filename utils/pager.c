/*
 * Copyright (c) 2014, 2015, 2016 Jonas 'Sortie' Termansen.
 * Copyright (c) 2021 Juhani 'nortti' Krekelä.
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
 * pager.c
 * Displays files one page at a time.
 */

#include <sys/ioctl.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

#define CONTROL_SEQUENCE_MAX 128

enum control_state
{
	CONTROL_STATE_NONE = 0,
	CONTROL_STATE_CSI,
	CONTROL_STATE_COMMAND,
};

struct line
{
	char* content;
	size_t content_used;
	size_t content_length;
};

static struct termios restore_tio;
static bool restore_scrollback;
static int tty_fd;
static bool stdout_is_tty;
static struct winsize winsize;
static mbstate_t in_ps;
static mbstate_t out_ps;
static const char* input_prompt_name;
static size_t possible_lines;
static size_t allowed_lines;
static bool quiting;
static bool flag_raw_control_chars;
static bool flag_color_sequences;
static enum control_state control_state;
static wchar_t control_sequence[CONTROL_SEQUENCE_MAX];
static size_t control_sequence_length;
static bool input_set_color;
static struct line* lines;
static size_t lines_used;
static size_t lines_length;
static enum control_state incoming_control_state;
static struct line* incoming_line;
static size_t incoming_line_width;
static size_t current_line;
static size_t current_line_offset;
static bool allowance_ever_exhausted;
static bool skipping_to_end;
static bool next_bold;
static bool next_underline;

static void exit_restore_tio(void)
{
	if ( restore_scrollback )
		dprintf(1, "\e[?1049l");
	if ( tcsetattr(tty_fd, TCSADRAIN, &restore_tio) < 0 )
		warn("tcsetattr");
}

static void init(void)
{
	tty_fd = 0;
	if ( !isatty(tty_fd) )
	{
		if ( (tty_fd = open("/dev/tty", O_RDONLY)) < 0 )
			err(1, "/dev/tty");
		if ( !isatty(tty_fd) )
			err(1, "/dev/tty");
	}
	if ( tcgetattr(tty_fd, &restore_tio) < 0 )
		err(1, "tcgetattr");
	if ( atexit(exit_restore_tio) < 0 )
		err(1, "atexit");
	struct termios tio = restore_tio;
	tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
	if ( tcsetattr(tty_fd, TCSADRAIN, &tio) < 0 )
		err(1, "tcsetattr");
	if ( (stdout_is_tty = isatty(1)) )
	{
		if ( ioctl(1, TIOCGWINSZ, &winsize) < 0 )
			err(1, "ioctl: TIOCGWINSZ");
		possible_lines = winsize.ws_row - 1;
		allowed_lines = possible_lines;
		const char* term = getenv("TERM");
		if ( term &&
		     strcmp(term, "sortix") != 0 &&
		     strncmp(term, "sortix-", strlen("sortix-")) != 0 )
		{
			dprintf(1, "\e[?1049h\e[H");
			restore_scrollback = true;
		}
	}

	memset(&in_ps, 0, sizeof(in_ps));
	memset(&out_ps, 0, sizeof(out_ps));
}

static char next_char(void)
{
	char c;
	if ( read(tty_fd, &c, 1) < 1 )
		err(1, "/dev/tty");
	return c;
}

static void prompt(bool at_end)
{
	const char* pre = input_set_color ? "" : "\e[47;30m";
	const char* post = input_set_color ? "" : "\e[m";
	if ( at_end )
		dprintf(1, "%s(END)%s\e[J", pre, post);
	else if ( input_prompt_name[0] )
		dprintf(1, "%s%s%s\e[J", pre, input_prompt_name, post);
	else
		dprintf(1, ":");
	while ( true )
	{
		char c;
		char buffer[CONTROL_SEQUENCE_MAX + 1];
		size_t buffer_used = 0;
		buffer[buffer_used++] = c = next_char();
		if ( c == '\e' &&
		     (buffer[buffer_used++] = c = next_char()) == '[' )
		{
			while ( buffer_used < CONTROL_SEQUENCE_MAX )
			{
				char c = next_char();
				buffer[buffer_used++] = c;
				if ( '@' <= c && c <= '~' )
					break;
			}
		}
		buffer[buffer_used] = '\0';

		if ( !strcmp(buffer, "\n") ||
		     !strcmp(buffer, "j") ||
		     !strcmp(buffer, "\x0e") /* ^N */ ||
		     !strcmp(buffer, "\e[B") /* Down Arrow */ )
		{
			dprintf(1, "\r\e[J");
			allowed_lines++;
			return;
		}

		if ( !strcmp(buffer, "k") ||
		     !strcmp(buffer, "\x10") /* ^P */ ||
		     !strcmp(buffer, "\e[A") /* Up Arrow */ )
		{
			if ( current_line <= possible_lines )
				continue;
			dprintf(1, "\e[2J\e[H");
			current_line -= possible_lines + 1;
			current_line_offset = 0;
			allowed_lines = possible_lines;
			return;
		}

		if ( !strcmp(buffer, " ") ||
		     !strcmp(buffer, "f") ||
		     !strcmp(buffer, "\x06") /* ^F */ ||
		     !strcmp(buffer, "\x16") /* ^V */ ||
		     !strcmp(buffer, "\e[6~") /* Page Down */ )
		{
			dprintf(1, "\r\e[J");
			allowed_lines = possible_lines;
			return;
		}

		if ( !strcmp(buffer, "b") ||
		     !strcmp(buffer, "\x02") /* ^B */ ||
		     !strcmp(buffer, "\ev") /* ESC-v */ ||
		     !strcmp(buffer, "\e[5~") /* Page Up */)
		{
			if ( current_line <= possible_lines )
				continue;
			size_t distance = possible_lines;
			if ( current_line - possible_lines < distance )
				distance = current_line - possible_lines;
			dprintf(1, "\e[2J\e[H");
			current_line -= possible_lines + distance;
			current_line_offset = 0;
			allowed_lines = possible_lines;
			return;
		}

		if ( !strcmp(buffer, ">") ||
		     !strcmp(buffer, "G") ||
		     !strcmp(buffer, "\e[F") /* End */ ||
		     !strcmp(buffer, "\e[4~") /* End (Linux console) */ )
		{
			dprintf(1, "\r\e[J");
			skipping_to_end = true;
			allowed_lines = SIZE_MAX;
			return;
		}

		if ( !strcmp(buffer, "<") ||
		     !strcmp(buffer, "g") ||
		     !strcmp(buffer, "\e[H") /* Home */ ||
		     !strcmp(buffer, "\e[1~") /* Home (Linux console) */ )
		{
			if ( current_line <= possible_lines )
				continue;
			dprintf(1, "\e[2J\e[H");
			current_line = 0;
			current_line_offset = 0;
			allowed_lines = possible_lines;
			return;
		}

		if ( !strcmp(buffer, "q") || !strcmp(buffer, "Q") )
		{
			dprintf(1, "\r\e[J");
			quiting = true;
			return;
		}
	}
	err(1, "/dev/tty");
}

static void line_push_char(struct line* line, char c)
{
	if ( line->content_used == line->content_length )
	{
		size_t length = line->content_length;
		if ( !length )
			length = 64;
		char* new = reallocarray(line->content, length, 2);
		if ( !new )
			err(1, "malloc");
		line->content = new;
		line->content_length = length * 2;
	}
	line->content[line->content_used++] = c;
}

static void line_push_string(struct line* line, const char* str)
{
	for ( size_t i = 0; str[i]; i++ )
		line_push_char(line, str[i]);
}

static struct line* continue_line(void)
{
	if ( incoming_line )
		return incoming_line;
	if ( lines_used == lines_length )
	{
		size_t length = lines_length;
		if ( !length )
			length = 64;
		struct line* new = reallocarray(lines, length, sizeof(struct line) * 2);
		if ( !new )
			err(1, "malloc");
		lines = new;
		lines_length = length * 2;
	}
	incoming_line = &lines[lines_used++];
	memset(incoming_line, 0, sizeof(*incoming_line));
	incoming_line_width = 0;
	return incoming_line;
}

static void finish_line(void)
{
	struct line* line = incoming_line;
	assert(line);
	size_t length = line->content_used ? line->content_used : 1;
	char* final_content = realloc(line->content, length);
	if ( final_content )
		line->content = final_content;
	incoming_line = NULL;
	incoming_line_width = 0;
}

static struct line* next_line(void)
{
	finish_line();
	return continue_line();
}

static void push_wchar(wchar_t wc)
{
	bool newline = false;
	int width;
	struct line* line = continue_line();

	if ( incoming_control_state == CONTROL_STATE_CSI )
	{
		incoming_control_state = CONTROL_STATE_NONE;
		if ( wc == '[' )
			incoming_control_state = CONTROL_STATE_COMMAND;
	}
	else if ( incoming_control_state == CONTROL_STATE_COMMAND )
	{
		incoming_control_state = CONTROL_STATE_NONE;
		if ( ('0' <= wc && wc <= '9') ||
		     wc == L';' || wc == L':' || wc == L'?' )
			incoming_control_state = CONTROL_STATE_COMMAND;
	}
	else if ( wc == L'\b' )
	{
		if ( incoming_line_width )
			incoming_line_width--;
		size_t index = line->content_used;
		const char* unbold = "\e[22m";
		if ( strlen(unbold) <= index &&
		     !memcmp(unbold, line->content + index - strlen(unbold), strlen(unbold)) )
			index -= strlen(unbold);
		while ( index && (line->content[index-1] & 0xC0) == 0x80 )
			index--;
		const char* bold = "\e[1m";
		if ( strlen(bold) <= index &&
		     !memcmp(bold, line->content + index - strlen(bold), strlen(bold)) )
		{
			index -= strlen(bold);
			next_bold = true;
		}
		if ( index )
		{
			char c = line->content[index - 1];
			if ( c == '_' )
				next_underline = true;
			else if ( c == ' ' )
				next_bold = false;
			else
				next_bold = true;
			line_push_char(line, '\b');
		}
		return;
	}
	else if ( wc == L'\e' )
	{
		incoming_control_state = CONTROL_STATE_CSI;
	}
	else if ( wc == L'\n' )
	{
		newline = true;
	}
	else if ( wc == L'\t' )
	{
		if ( winsize.ws_col == incoming_line_width )
			line = next_line();
		do
		{
			if ( winsize.ws_col == incoming_line_width )
				break;
			incoming_line_width++;
		} while ( incoming_line_width % 8 != 0 );
	}
	else if ( wc == L'\r' )
	{
		incoming_line_width = 0;
	}
	else if ( wc == 127 )
	{
	}
	else if ( 0 <= (width = wcwidth(wc)) )
	{
		size_t left = winsize.ws_col - incoming_line_width;
		if ( left < (size_t) width )
			line = next_line();
		incoming_line_width += width;
	}
	else
	{
		// TODO: What can cause this and how to handle it?
	}

	char mb[MB_CUR_MAX];
	size_t amount = wcrtomb(mb, wc, &out_ps);
	if ( amount != (size_t) -1 )
	{
		if ( next_bold && next_underline )
			line_push_string(line, "\e[1;4m");
		else if ( next_bold )
			line_push_string(line, "\e[1m");
		else if ( next_underline )
			line_push_string(line, "\e[4m");
		for ( size_t i = 0; i < amount; i++ )
			line_push_char(line, mb[i]);
		if ( next_bold && next_underline )
			line_push_string(line, "\e[22;24m");
		else if ( next_bold )
			line_push_string(line, "\e[22m");
		else if ( next_underline )
			line_push_string(line, "\e[24m");
		next_bold = false;
		next_underline = false;
	}

	if ( newline )
		finish_line();
}

static bool push_wchar_is_escaped(wchar_t wc)
{
	if ( wc == '\b' &&
	    (flag_raw_control_chars || flag_color_sequences) )
		return false;
	return wc < 32 && wc != L'\t' && wc != L'\n';
}

static void push_wchar_escape(wchar_t wc)
{
	if ( push_wchar_is_escaped(wc) )
	{
		push_wchar(L'^');
		//push_wchar(L'\b');
		//push_wchar(L'^');
		push_wchar(L'@' + wc);
		//push_wchar(L'\b');
		//push_wchar(L'@' + wc);
	}
	else
	{
		push_wchar(wc);
	}
}

static void control_sequence_begin(void)
{
	control_sequence_length = 0;
}

static void control_sequence_accept(void)
{
	for ( size_t i = 0; i < control_sequence_length; i++ )
		push_wchar(control_sequence[i]);
	control_sequence_length = 0;
	control_state = CONTROL_STATE_NONE;
}

static void control_sequence_reject(void)
{
	for ( size_t i = 0; i < control_sequence_length; i++ )
		push_wchar_escape(control_sequence[i]);
	control_sequence_length = 0;
	control_state = CONTROL_STATE_NONE;
}

static void control_sequence_push(wchar_t wc)
{
	if ( flag_raw_control_chars )
		return push_wchar(wc);
	if ( CONTROL_SEQUENCE_MAX <= control_sequence_length )
	{
		control_sequence_reject();
		push_wchar_escape(wc);
		return;
	}
	control_sequence[control_sequence_length++] = wc;
}

static void control_sequence_finish(wchar_t wc)
{
	control_sequence_push(wc);
	if ( control_state == CONTROL_STATE_NONE )
		return;
	if ( wc == L'm' )
	{
		input_set_color = true;
		return control_sequence_accept();
	}
	if ( !flag_raw_control_chars )
		return control_sequence_reject();
	control_sequence_accept();
}

static void push_wchar_filter(wchar_t wc)
{
	if ( wc == L'\e' &&
	     (flag_raw_control_chars || flag_color_sequences) &&
	     control_state == CONTROL_STATE_NONE )
	{
		control_sequence_begin();
		control_sequence_push(wc);
		control_state = CONTROL_STATE_CSI;
		return;
	}
	else if ( control_state == CONTROL_STATE_CSI )
	{
		if ( wc == L'[' )
		{
			control_sequence_push(wc);
			control_state = CONTROL_STATE_COMMAND;
			return;
		}
		control_sequence_reject();
	}
	else if ( control_state == CONTROL_STATE_COMMAND )
	{
		if ( ('0' <= wc && wc <= '9') ||
		     wc == L';' || wc == L':' || wc == L'?' )
		{
			control_sequence_push(wc);
			return;
		}
		control_sequence_finish(wc);
		return;
	}
	push_wchar_escape(wc);
}

static void push_byte(unsigned char byte)
{
	if ( quiting )
		return;

	wchar_t wc;
	size_t amount = mbrtowc(&wc, (const char*) &byte, 1, &in_ps);
	if ( amount == (size_t) -2 )
		return;
	if ( amount == (size_t) -1 )
	{
		wc = 0xFFFD /* REPLACEMENT CHARACTER */;
		memset(&in_ps, 0, sizeof(in_ps));
	}
	push_wchar_filter(wc);
}

static bool read_fd(int fd, const char* fdpath)
{
	unsigned char buffer[4096];
	ssize_t amount = read(fd, buffer, sizeof(buffer));
	if ( amount < 0 )
		err(1, "%s", fdpath);
	for ( ssize_t i = 0; i < amount; i++ )
		push_byte(buffer[i]);
	return amount != 0;
}

static void simple_fd(int fd, const char* fdpath)
{
	unsigned char buffer[4096];
	ssize_t amount = 0;
	while ( 0 < (amount = read(fd, buffer, sizeof(buffer))) )
	{
		ssize_t sofar = 0;
		while ( sofar < amount )
		{
			ssize_t done = write(1, buffer + sofar, amount - sofar);
			if ( done < 0 )
				err(1, "<stdout>");
			sofar += done;
		}
	}
	if ( amount < 0 )
		err(1, "%s", fdpath);
}

static bool can_page(void)
{
	if ( current_line + 1 == lines_used )
	{
		struct line* line = &lines[current_line];
		return current_line_offset < line->content_used;
	}
	return current_line + 1 < lines_used;
}

static void page(void)
{
	struct line* line = &lines[current_line];
	if ( current_line_offset < line->content_used )
	{
		const char* buffer = line->content + current_line_offset;
		size_t amount = line->content_used - current_line_offset;
		size_t sofar = 0;
		while ( sofar < amount )
		{
			ssize_t done = write(1, buffer + sofar, amount - sofar);
			if ( done < 0 )
				err(1, "<stdout>");
			sofar += done;
		}
		current_line_offset = line->content_used;
	}
	if ( current_line + 1 < lines_used )
	{
		if ( allowed_lines != SIZE_MAX )
			allowed_lines--;
		current_line++;
		current_line_offset = 0;
	}
}

static void push_fd(int fd, const char* fdpath)
{
	if ( quiting )
		return;
	if ( !strcmp(fdpath, "<stdin>") )
		input_prompt_name = "";
	else
		input_prompt_name = fdpath;
	// TODO: In this case, we should disable echoing and read from the terminal
	//       anyways. Remember to enable it again.
	if ( isatty(fd) )
		errx(1, "/dev/tty: Is a terminal");
	if ( !stdout_is_tty )
		return simple_fd(fd, fdpath);
	bool eof = false;
	while ( !quiting )
	{
		if ( !skipping_to_end )
		{
			if ( allowed_lines == 0 )
			{
				allowance_ever_exhausted = true;
				prompt(false);
				continue;
			}
			if ( can_page() )
			{
				page();
				continue;
			}
		}
		if ( eof )
			break;
		if ( !read_fd(fd, fdpath) )
			eof = true;
	}
}

static void push_path(const char* path)
{
	if ( quiting )
		return;
	if ( !strcmp(path, "-") )
		return push_fd(0, "<stdin>");
	int fd = open(path, O_RDONLY);
	if ( fd < 0 )
		err(1, "%s", path);
	push_fd(fd, path);
	close(fd);
}

static void compact_arguments(int* argc, char*** argv)
{
	for ( int i = 0; i < *argc; i++ )
	{
		while ( i < *argc && !(*argv)[i] )
		{
			for ( int n = i; n < *argc; n++ )
				(*argv)[n] = (*argv)[n+1];
			(*argc)--;
		}
	}
}

int main(int argc, char* argv[])
{
	setlocale(LC_ALL, "");

	if ( getenv("LESS") )
	{
		const char* options = getenv("LESS");
		char c;
		while ( (c = *options++) )
		{
			switch ( c )
			{
			case '-': break;
			case 'r': flag_raw_control_chars = true; break;
			case 'R': flag_color_sequences = true; break;
			default: break;
			}
		}
	}

	for ( int i = 1; i < argc; i++ )
	{
		const char* arg = argv[i];
		if ( arg[0] != '-' || !arg[1] )
			continue;
		argv[i] = NULL;
		if ( !strcmp(arg, "--") )
			break;
		if ( arg[1] != '-' )
		{
			char c;
			while ( (c = *++arg) ) switch ( c )
			{
			case 'r': flag_raw_control_chars = true; break;
			case 'R': flag_color_sequences = true; break;
			default:
				errx(1, "unknown option -- '%c'", c);
			}
		}
		else
			errx(1, "unknown option: %s", arg);
	}

	compact_arguments(&argc, &argv);

	init();

	if ( argc == 1 )
	{
		if ( tty_fd == 0 )
			errx(1, "missing file operand");
		push_fd(0, "<stdin>");
	}
	else for ( int i = 1; i < argc; i++ )
	{
		push_path(argv[i]);
	}

	while ( stdout_is_tty &&
	        (allowance_ever_exhausted || restore_scrollback) &&
	        !quiting )
	{
		if ( skipping_to_end )
		{
			dprintf(1, "\e[2J\e[H");
			size_t line = 0;
			if ( possible_lines <= lines_used )
				line = lines_used - possible_lines;
			current_line = line;
			current_line_offset = 0;
			allowed_lines = possible_lines;
			skipping_to_end = false;
		}
		bool cant_page = !can_page();
		if ( cant_page || allowed_lines == 0 )
		{
			prompt(cant_page);
			continue;
		}
		page();
	}

	return 0;
}
