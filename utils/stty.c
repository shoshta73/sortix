/*
 * Copyright (c) 2017 Jonas 'Sortie' Termansen.
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
 * stty.c
 * Terminal settings.
 */

// TODO: This program needs a bunch of improvements to conform to POSIX.

#include <fcntl.h>
#include <err.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define ARRAY_SIZE(x) (sizeof((x)) / sizeof((x)[0]))
#define CONTROL(x) (((x) - 64) & 127)
#define M_CONTROL(x) (128 + CONTROL(x))

struct flag
{
	const char* name;
	tcflag_t bit;
};

// TODO: CSIZE, CS5, CS6, CS7, CS8, the baud speed.
static const struct flag cflags[] =
{
	{ "clocal", CLOCAL },
	{ "cread", CREAD },
	{ "cstopb", CSTOPB },
	{ "hupcl", HUPCL },
	{ "parenb", PARENB },
	{ "parodd", PARODD },
};

static const struct flag iflags[] =
{
	{ "brkint", BRKINT },
	{ "icrnl", ICRNL },
	{ "ignbrk", IGNBRK },
	{ "igncr", IGNCR },
	{ "ignpar", IGNPAR },
	{ "inlcr", INLCR },
	{ "inpck", INPCK },
	{ "isig", ISIG },
	{ "istrip", ISTRIP },
	{ "ixany", IXANY },
	{ "ixoff", IXOFF },
	{ "ixon", IXON },
	{ "parmrk", PARMRK },
};

static const struct flag lflags[] =
{
	{ "echo", ECHO },
	{ "echoe", ECHOE },
	{ "echok", ECHOK },
	{ "echonl", ECHONL },
	{ "icanon", ICANON },
	{ "iexten", IEXTEN },
	{ "isig", ISIG },
	{ "isortix_32bit", ISORTIX_32BIT },
	{ "isortix_chars_disable", ISORTIX_CHARS_DISABLE },
	{ "isortix_kbkey", ISORTIX_KBKEY },
	{ "isortix_nonblock", ISORTIX_NONBLOCK },
	{ "isortix_termmode", ISORTIX_TERMMODE },
	{ "noflsh", NOFLSH },
	{ "tostop", TOSTOP },
};

static const struct flag oflags[] =
{
	{ "opost", OPOST },
	{ "onlcr", ONLCR },
	{ "ocrnl", OCRNL },
};

struct control_character
{
	const char* name;
	cc_t value;
};

struct control_character control_characters[] =
{
	{ "eof", VEOF },
	{ "eol", VEOL },
	{ "erase", VERASE },
	{ "intr", VINTR },
	{ "kill", VKILL },
	{ "min", VMIN },
	{ "quit", VQUIT },
	{ "start", VSTART },
	{ "stop", VSTOP },
	{ "time", VTIME },
	{ "usp", VSUSP },
	{ "werase", VWERASE },
};

static void show_flags(const char* kind,
                       tcflag_t value,
                       tcflag_t default_value,
                       const struct flag* flags,
                       size_t flags_count,
                       bool all)
{
	printf("%s:", kind);
	tcflag_t handled = 0;
	for ( size_t i = 0; i < flags_count; i++ )
	{
		const struct flag* flag = &flags[i];
		handled |= flag->bit;
		if ( !all && (value & flag->bit) == (default_value & flag->bit) )
			continue;
		putchar(' ');
		if ( !(value & flag->bit) )
			putchar('-');
		fputs(flag->name, stdout);
	}
	if ( value & ~handled )
		printf(" %#x", value & ~handled);
	putchar('\n');
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
	bool all = false;

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
			case 'a': all = true; break;
			// TODO: -g
			default:
				errx(1, "unknown option -- '%c'", c);
			}
		}
		else
			errx(1, "unknown option: %s", arg);
	}

	compact_arguments(&argc, &argv);

	// Keep this in sync with kernel/tty.cpp.
	struct termios default_tio = { 0 };
	default_tio.c_iflag = BRKINT | ICRNL | IXANY | IXON;
	default_tio.c_oflag = OPOST | ONLCR;
	default_tio.c_cflag = CS8 | CREAD | HUPCL;
	default_tio.c_lflag = ECHO | ECHOE | ECHOK | ICANON | IEXTEN | ISIG;
	default_tio.c_cc[VEOF] = CONTROL('D');
	default_tio.c_cc[VEOL] = M_CONTROL('?');
	default_tio.c_cc[VERASE] = CONTROL('?');
	default_tio.c_cc[VINTR] = CONTROL('C');
	default_tio.c_cc[VKILL] = CONTROL('U');
	default_tio.c_cc[VMIN] = 1;
	default_tio.c_cc[VQUIT] = CONTROL('\\');
	default_tio.c_cc[VSTART] = CONTROL('Q');
	default_tio.c_cc[VSTOP] = CONTROL('S');
	default_tio.c_cc[VSUSP] = CONTROL('Z');
	default_tio.c_cc[VTIME] = 0;
	default_tio.c_cc[VWERASE] = CONTROL('W');
	default_tio.c_ispeed = B38400;
	default_tio.c_ospeed = B38400;

	int tty = 0;
	const char* tty_name = "<stdin>";
	struct termios tio;
	if ( tcgetattr(tty, &tio) < 0 )
		err(1, "tcgetattr: %s", tty_name);

	// TODO: CSIZE, CS5, CS6, CS7, CS8.
	tio.c_cflag &= ~CSIZE;
	default_tio.c_cflag &= ~CSIZE;

	fputs("cc:", stdout);
	for ( size_t i = 0; i < ARRAY_SIZE(control_characters); i++ )
	{
		const struct control_character* cc = &control_characters[i];
		if ( !all && tio.c_cc[cc->value] == default_tio.c_cc[cc->value] )
			continue;
		printf(" %s = ", cc->name);
		// TODO: Only _POSIX_VDISABLE if it is in effect?
		unsigned char value = (unsigned char) tio.c_cc[cc->value];
		if ( value == _POSIX_VDISABLE )
			printf("undef");
		else if ( 128 <= value && (value < 160 || value == 255) )
			printf("M-^%c", (value - 128) ^ 0x40);
		else if ( 128 < value )
			printf("M-%c", value - 128);
		else if ( value < 32 || value == 127 )
			printf("^%c", value ^ 0x40);
		else
			putchar(value);
	}
	putchar('\n');

	show_flags("cflags", tio.c_cflag, default_tio.c_cflag, cflags,
	           ARRAY_SIZE(cflags), all);
	show_flags("iflags", tio.c_iflag, default_tio.c_iflag, iflags,
	           ARRAY_SIZE(iflags), all);
	show_flags("lflags", tio.c_lflag, default_tio.c_lflag, lflags,
	           ARRAY_SIZE(lflags), all);
	show_flags("oflags", tio.c_oflag, default_tio.c_oflag, oflags,
	           ARRAY_SIZE(oflags), all);

	if ( ferror(stdout) || fflush(stdout) == EOF )
		err(1, "stdout");
	return 0;
}
