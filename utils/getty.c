/*
 * Copyright (c) 2023, 2024 Jonas 'Sortie' Termansen.
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
 * getty.c
 * Initialize a terminal session.
 */

#include <sys/ioctl.h>
#include <sys/wait.h>

#include <brand.h>
#include <err.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define CONTROL(x) (((x) - 64) & 127)

int main(int argc, char* argv[])
{
	bool background = false;
	bool logo = true;
	int columns = -1;
	bool force = false;
	int rows = -1;
	int speed = -1;
	int parity = -1;
	int bits = -1;
	bool receive = true;
	const char* term = NULL;
	int opt;
	while ( (opt = getopt(argc, argv, "5678befh:lnoRs:t:w:")) != -1 )
	{
		switch ( opt )
		{
		case '5': bits = CS5; break;
		case '6': bits = CS6; break;
		case '7': bits = CS7; break;
		case '8': bits = CS8; break;
		case 'b': background = true; break;
		case 'e': parity = PARENB; break;
		case 'f': force = true; break;
		case 'h': rows = atoi(optarg); break;
		case 'l': logo = true; break;
		case 'n': parity = 0; break;
		case 'o': parity = PARENB | PARODD; break;
		case 'R': receive = false; break;
		case 's': speed = atoi(optarg); break;
		case 't': term = optarg; break;
		case 'w': columns = atoi(optarg); break;
		default: return 1;
		}
	}
	if ( argc - optind < 1 )
		errx(1, "Expected terminal path");
	if ( argc - optind < 2 )
		errx(1, "Expected program");
	const char* path = argv[optind + 0];
	int tty;
	if ( !strcmp(path, "-") )
		tty = 0;
	else
	{
		if ( (tty = open(path, O_RDWR)) < 0 )
			err(1, "%s", path);
		if ( !isatty(tty) )
			err(1, "%s", path);
	}
	struct termios tio;
	if ( tcgetattr(tty, &tio) < 0 )
		err(1, "tcgetattr: %s", path);
	if ( background || getsid(0) != getpid() )
	{
		pid_t child_pid = fork();
		if ( child_pid < 0 )
			err(1, "fork");
		if ( child_pid )
		{
			if ( background )
				return 0;
			int status;
			waitpid(child_pid, &status, 0);
			return WEXITSTATUS(status);
		}
		if ( setsid() < 0 )
			err(1, "setsid");
	}
	if ( ioctl(tty, TIOCSCTTY, force ? 1 : 0) < 0 )
		err(1, "ioctl: TIOCSCTTY");
	if ( close(0) < 0 || close(1) < 0 || close(2) < 0 )
		err(1, "close");
	if ( dup2(tty, 0) != 0 || dup2(tty, 1) != 1 || dup2(tty, 2) != 2 )
		err(1, "dup");
	if ( closefrom(3) < 0 )
		err(1, "closefrom");
	tty = 0;
	if ( 0 < columns && 0 < rows )
	{
		struct winsize ws;
		if ( tcgetwinsize(tty, &ws) < 0 )
			err(1, "tcgetwinsize");
		if ( 0 < columns )
			ws.ws_col = columns;
		if ( 0 < rows )
			ws.ws_row = rows;
		if ( tcsetwinsize(tty, &ws) < 0 )
			err(1, "tcgetwinsize");
	}
	if ( 0 <= bits )
		tio.c_cflag = (tio.c_cflag & ~CREAD) | bits;
	if ( 0 <= parity )
		tio.c_cflag = (tio.c_cflag & ~(PARENB | PARODD)) | parity;
	if ( 0 <= bits || 0 <= parity )
	{
		tio.c_cflag &= ~(CSTOPB | CLOCAL);
		tio.c_cflag |= HUPCL;
	}
	if ( receive )
		tio.c_cflag |= CREAD;
	tio.c_iflag = BRKINT | ICRNL | IXANY | IXON;
	tio.c_oflag = OPOST | ONLCR;
	tio.c_lflag = ECHO | ECHOE | ECHOK | ICANON | IEXTEN | ISIG;
	tio.c_cc[VEOF] = CONTROL('D');
	tio.c_cc[VEOL] = 0;
	tio.c_cc[VERASE] = CONTROL('?');
	tio.c_cc[VINTR] = CONTROL('C');
	tio.c_cc[VKILL] = CONTROL('U');
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VQUIT] = CONTROL('\\');
	tio.c_cc[VSTART] = CONTROL('Q');
	tio.c_cc[VSTOP] = CONTROL('S');
	tio.c_cc[VSUSP] = CONTROL('Z');
	tio.c_cc[VTIME] = 0;
	tio.c_cc[VWERASE] = CONTROL('W');
	if ( 0 < speed )
		tio.c_ispeed = tio.c_ospeed = speed;
	if ( tcsetattr(tty, TCSANOW, &tio) < 0 )
		err(1, "tcsetattr: %s", path);
	if ( term && setenv("TERM", term, 1) < 0 )
		err(1, "setenv");
	if ( logo )
	{
		struct winsize ws;
		if ( tcgetwinsize(tty, &ws) < 0 )
			err(1, "tcgetwinsize");
		printf("\e[37;41m\e[J");
		const char* string = BRAND_LOGO;
		while ( *string )
		{
			size_t string_width = strcspn(string, "\n");
			size_t leading = string_width <= ws.ws_col ?
				             (ws.ws_col - string_width) / 2 : 0;
			for ( size_t i = 0; i < leading; i++ )
				putchar(' ');
			fwrite(string, string_width, 1, stdout);
			string += string_width;
			if ( *string == '\n' )
			{
				string++;
				if ( !*string )
					printf("\e[m");
				putchar('\n');
			}
		}
		printf("\r\e[m\e[J");
		fflush(stdout);
	}
	execvp(argv[optind + 1], argv + optind + 1);
	err(1, "%s", argv[optind + 1]);
}
