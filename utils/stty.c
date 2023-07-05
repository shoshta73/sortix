/*
 * Copyright (c) 2017, 2020, 2024 Jonas 'Sortie' Termansen.
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
 * Display and set terminal settings.
 */

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#if !defined(TTY_NAME_MAX)
#include <sortix/limits.h>
#endif

#define ARRAY_SIZE(x) (sizeof((x)) / sizeof((x)[0]))
#define CONTROL(x) (((x) - 64) & 127)
#define M_CONTROL(x) (128 + CONTROL(x))

struct flag
{
	const char* name;
	tcflag_t bit;
};

static const struct flag cflags[] =
{
	{ "clocal", CLOCAL },
	{ "cread", CREAD },
	{ "csize", CSIZE },
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

static const struct control_character control_characters[] =
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
	{ "susp", VSUSP },
	{ "time", VTIME },
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
		if ( !strcmp(flag->name, "csize") )
		{
			if ( (value & CSIZE) == CS5 )
				fputs("cs5", stdout);
			else if ( (value & CSIZE) == CS6 )
				fputs("cs6", stdout);
			else if ( (value & CSIZE) == CS7 )
				fputs("cs7", stdout);
			else if ( (value & CSIZE) == CS8 )
				fputs("cs8", stdout);
		}
		else
		{
			if ( !(value & flag->bit) )
				putchar('-');
			fputs(flag->name, stdout);
		}
	}
	if ( value & ~handled )
		printf(" %#x", value & ~handled);
	putchar('\n');
}

static bool is_unsetable(const char* name)
{
	if ( !strcmp(name, "parity") ||
	     !strcmp(name, "evenp") ||
	     !strcmp(name, "oddp") ||
	     !strcmp(name, "raw") ||
	     !strcmp(name, "nl") )
		return true;
	for ( size_t i = 0; i < ARRAY_SIZE(cflags); i++ )
		if ( !strcmp(name, cflags[i].name) )
			return true;
	for ( size_t i = 0; i < ARRAY_SIZE(iflags); i++ )
		if ( !strcmp(name, iflags[i].name) )
			return true;
	for ( size_t i = 0; i < ARRAY_SIZE(lflags); i++ )
		if ( !strcmp(name, lflags[i].name) )
			return true;
	for ( size_t i = 0; i < ARRAY_SIZE(oflags); i++ )
		if ( !strcmp(name, oflags[i].name) )
			return true;
	return false;
}

static speed_t parse_speed(const char* string)
{
	if ( !isdigit((unsigned char) string[0]) )
		errx(1, "invalid speed: %s", string);
	errno = 0;
	char* endptr;
	uintmax_t value = strtoumax(string, &endptr, 10);
	if ( errno || *endptr || value != (speed_t) value )
		errx(1, "invalid speed: %s", string);
	return (speed_t) value;
}

static cc_t parse_mintime(const char* string)
{
	if ( !isdigit((unsigned char) string[0]) )
		errx(1, "invalid quantity: %s", string);
	errno = 0;
	char* endptr;
	uintmax_t value = strtoumax(string, &endptr, 10);
	if ( errno || *endptr || value != (cc_t) value )
		errx(1, "invalid quantity: %s", string);
	return (cc_t) value;
}

static size_t parse_winsize(const char* string)
{
	if ( !isdigit((unsigned char) string[0]) )
		errx(1, "invalid window size: %s", string);
	errno = 0;
	char* endptr;
	uintmax_t value = strtoumax(string, &endptr, 10);
	if ( errno || *endptr || value != (size_t) value )
		errx(1, "invalid window size: %s", string);
	return (size_t) value;
}

static bool is_gfmt1_name(const char* str, const char* name)
{
	size_t name_length = strlen(name);
	return !strncmp(str, name, name_length) && str[name_length] == '=';
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
	bool save = false;

	for ( int i = 1; i < argc; i++ )
	{
		const char* arg = argv[i];
		if ( arg[0] != '-' || !arg[1] )
			continue;
		if ( is_unsetable(arg + 1) )
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
			case 'g': save = true; break;
			default:
				errx(1, "unknown option -- '%c'", c);
			}
		}
		else
			errx(1, "unknown option: %s", arg);
	}

	compact_arguments(&argc, &argv);

	if ( all && save )
		errx(1, "Incompatible output modes");

	if ( (all || save) && argc != 1 )
		errx(1, "Cannot both change and display terminal modes");

	// Keep this in sync with kernel/tty.cpp, utils/getty.c.
	struct termios default_tio = { 0 };
	default_tio.c_iflag = BRKINT | ICRNL | IXANY | IXON;
	default_tio.c_oflag = OPOST | ONLCR;
	default_tio.c_cflag = CS8 | CREAD | HUPCL;
	default_tio.c_lflag = ECHO | ECHOE | ECHOK | ICANON | IEXTEN | ISIG;
	default_tio.c_cc[VEOF] = CONTROL('D');
	default_tio.c_cc[VEOL] = 0;
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

	if ( !isatty(tty) )
		err(1, "<stdin>");

	char tty_name[TTY_NAME_MAX+1] = "<stdin>";
	ttyname_r(tty, tty_name, sizeof(tty_name));

	struct winsize ws = {0};
	bool got_ws = tcgetwinsize(tty, &ws) == 0;
	bool set_ws = false;

	struct termios tio;
	if ( tcgetattr(tty, &tio) < 0 )
		err(1, "tcgetattr: %s", tty_name);

	if ( save )
	{
		printf("gfmt1:cflag=%x:iflag=%x:lflag=%x:oflag=%x",
		       tio.c_cflag, tio.c_iflag, tio.c_lflag, tio.c_oflag);
		for ( size_t i = 0; i < ARRAY_SIZE(control_characters); i++ )
			printf(":%s=%x", control_characters[i].name,
			       tio.c_cc[control_characters[i].value]);
		printf(":ispeed=%u:ospeed=%u\n", tio.c_ispeed, tio.c_ospeed);
		if ( ferror(stdout) || fflush(stdout) == EOF )
			err(1, "stdout");
		return 0;
	}

	if ( argc == 1 )
	{
		if ( tio.c_ispeed == tio.c_ospeed )
			printf("speed %u baud;", tio.c_ispeed);
		else
			printf("ispeed %u baud; ospeed %u baud;", tio.c_ispeed,
			       tio.c_ospeed);

		if ( all && got_ws )
			printf(" %zu rows; %zu columns;", ws.ws_row, ws.ws_col);

		putchar('\n');

		fputs("cc:", stdout);
		for ( size_t i = 0; i < ARRAY_SIZE(control_characters); i++ )
		{
			const struct control_character* cc = &control_characters[i];
			if ( !all && tio.c_cc[cc->value] == default_tio.c_cc[cc->value] )
				continue;
			printf(" %s = ", cc->name);
			unsigned char value = (unsigned char) tio.c_cc[cc->value];
			if ( cc->value == VMIN || cc->value == VTIME )
				printf("%i", value);
			else if ( value == _POSIX_VDISABLE )
				printf("undef");
			else if ( 128 <= value && (value < 160 || value == 255) )
				printf("M-^%c", (value - 128) ^ 0x40);
			else if ( 128 < value )
				printf("M-%c", value - 128);
			else if ( value < 32 || value == 127 )
				printf("^%c", value ^ 0x40);
			else
				putchar(value);
			putchar(';');
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

	for ( int i = 1; i < argc; i++ )
	{
		const char* arg = argv[i];
		if ( !strncmp(arg, "gfmt1:", strlen("gfmt1:")) )
		{
			const char* str = arg + strlen("gfmt1:");
			while ( str[0] )
			{
				const char* name = str;
				size_t name_length = strcspn(str, "=");
				if ( str[name_length] != '=' || !str[name_length + 1] )
					errx(1, "invalid saved state: %s", arg);
				str += name_length + 1;
				int base = is_gfmt1_name(name, "ispeed") ||
				           is_gfmt1_name(name, "ospeed") ? 10 : 16;
				errno = 0;
				char* endptr;
				uintmax_t value = strtoumax(str, &endptr, base);
				size_t value_length = endptr - str;
				if ( errno || !value_length )
					errx(1, "invalid saved state: %s", arg);
				str += value_length;
				if ( is_gfmt1_name(name, "cflag") )
					tio.c_cflag = value;
				else if ( is_gfmt1_name(name, "iflag") )
					tio.c_iflag = value;
				else if ( is_gfmt1_name(name, "lflag") )
					tio.c_lflag = value;
				else if ( is_gfmt1_name(name, "oflag") )
					tio.c_oflag = value;
				else if ( is_gfmt1_name(name, "ispeed") )
					tio.c_ispeed = value;
				else if ( is_gfmt1_name(name, "ospeed") )
					tio.c_ospeed = value;
				else
				{
					bool found = false;
					for ( size_t n = 0;
					      n < ARRAY_SIZE(control_characters);
					      n++ )
					{
						if ( is_gfmt1_name(name, control_characters[n].name) )
						{
							tio.c_cc[control_characters[n].value] = value;
							found = true;
							break;
						}
					}
					if ( !found )
						errx(1, "invalid saved state: %s", arg);
				}
				if ( str[0] == ':' )
					str++;
				else if ( str[0] )
					errx(1, "invalid saved state: %s", arg);
			}
		}
		else if ( !strcmp(arg, "cs5") )
			tio.c_cflag = (tio.c_cflag & ~CSIZE) | CS5;
		else if ( !strcmp(arg, "cs6") )
			tio.c_cflag = (tio.c_cflag & ~CSIZE) | CS6;
		else if ( !strcmp(arg, "cs7") )
			tio.c_cflag = (tio.c_cflag & ~CSIZE) | CS7;
		else if ( !strcmp(arg, "cs8") )
			tio.c_cflag = (tio.c_cflag & ~CSIZE) | CS8;
		else if ( !strcmp(arg, "csize") )
			errx(1, "unknown operand: %s", arg);
		else if ( arg[0] && !arg[strspn(arg, "0123456789")] )
			tio.c_ispeed = tio.c_ospeed = parse_speed(arg);
		else if ( !strcmp(arg, "speed") )
		{
			printf("%u\n", tio.c_ospeed);
			if ( i + 1 < argc && !argv[i+1][strspn(argv[i+1], "0123456789")] )
			{
				const char* parameter = argv[++i];
				tio.c_ispeed = tio.c_ospeed = parse_speed(parameter);
			}
		}
		else if ( !strcmp(arg, "ispeed") )
		{
			if ( i + 1 == argc )
				errx(1, "missing argument to %s", arg);
			const char* parameter = argv[++i];
			tio.c_ispeed = parse_speed(parameter);
		}
		else if ( !strcmp(arg, "ospeed") )
		{
			if ( i + 1 == argc )
				errx(1, "missing argument to %s", arg);
			const char* parameter = argv[++i];
			tio.c_ospeed = parse_speed(parameter);
		}
		else if ( !strcmp(arg, "size") )
			printf("%zu %zu\n", ws.ws_row, ws.ws_col);
		else if ( !strcmp(arg, "columns") || !strcmp(arg, "cols") )
		{
			if ( i + 1 == argc )
				errx(1, "missing argument to %s", arg);
			const char* parameter = argv[++i];
			ws.ws_col = parse_winsize(parameter);
			set_ws = true;
		}
		else if ( !strcmp(arg, "rows") )
		{
			if ( i + 1 == argc )
				errx(1, "missing argument to %s", arg);
			const char* parameter = argv[++i];
			ws.ws_row = parse_winsize(parameter);
			set_ws = true;
		}
		else if ( !strcmp(arg, "min") )
		{
			if ( i + 1 == argc )
				errx(1, "missing argument to %s", arg);
			const char* parameter = argv[++i];
			tio.c_cc[VMIN] = parse_mintime(parameter);
		}
		else if ( !strcmp(arg, "time") )
		{
			if ( i + 1 == argc )
				errx(1, "missing argument to %s", arg);
			const char* parameter = argv[++i];
			tio.c_cc[VTIME] = parse_mintime(parameter);
		}
		else if ( !strcmp(arg, "evenp") || !strcmp(arg, "parity") )
			tio.c_cflag = (tio.c_cflag & ~(CSIZE | PARODD)) | PARENB | CS7;
		else if ( !strcmp(arg, "oddp") )
			tio.c_cflag = (tio.c_cflag & ~CSIZE) | PARENB | PARODD | CS7;
		else if ( !strcmp(arg, "-parity") ||
		          !strcmp(arg, "-evenp") ||
		          !strcmp(arg, "-oddp") )
			tio.c_cflag = (tio.c_cflag & ~(CSIZE | PARENB)) | CS8;
		else if ( !strcmp(arg, "raw") || !strcmp(arg, "-cooked") )
		{
			tio.c_iflag &= ~(BRKINT | ICRNL | IGNBRK | IGNCR | INLCR | ISTRIP |
			                 IXON | PARMRK);
			tio.c_oflag &= ~(OPOST);
			tio.c_cflag &= ~(CSIZE | CSTOPB | PARENB | PARODD);
			tio.c_cflag |= CS8;
			tio.c_lflag &= ~(ECHO | ECHONL | ICANON | IEXTEN | ISIG |
			                 ISORTIX_TERMMODE | ISORTIX_CHARS_DISABLE |
			                 ISORTIX_KBKEY | ISORTIX_32BIT | ISORTIX_NONBLOCK);
			tio.c_cc[VMIN] = 1;
			tio.c_cc[VTIME] = 0;
		}
		else if ( !strcmp(arg, "nl") )
			tio.c_iflag = (tio.c_iflag & ~ICRNL);
		else if ( !strcmp(arg, "-nl") )
			tio.c_iflag = (tio.c_iflag & ~(INLCR | IGNCR)) | ICRNL;
		else if ( !strcmp(arg, "ek") )
		{
			tio.c_cc[VERASE] = default_tio.c_cc[VERASE];
			tio.c_cc[VKILL] = default_tio.c_cc[VKILL];
		}
		else if ( !strcmp(arg, "sane") ||
		          !strcmp(arg, "cooked") ||
		          !strcmp(arg, "-raw") )
		{
			tio.c_iflag = default_tio.c_iflag;
			tio.c_oflag = default_tio.c_oflag;
			tio.c_cflag = default_tio.c_cflag;
			tio.c_lflag = default_tio.c_lflag;
			memcpy(&tio.c_cc, &default_tio.c_cc, sizeof(tio.c_cc));
		}
		else
		{
			if ( !strcmp(arg, "hup") )
				arg = "hupcl";
			else if ( !strcmp(arg, "-hup") )
				arg = "-hupcl";
			bool negated = arg[0] == '-';
			const char* name = negated ? arg + 1 : arg;
			for ( size_t n = 0; n < ARRAY_SIZE(cflags); n++ )
			{
				if ( strcmp(name, cflags[n].name) != 0 )
					continue;
				tio.c_cflag = (tio.c_cflag & ~cflags[n].bit) |
				              (negated ? 0 : cflags[n].bit);
				goto found;
			}
			for ( size_t n = 0; n < ARRAY_SIZE(iflags); n++ )
			{
				if ( strcmp(name, iflags[n].name) != 0 )
					continue;
				tio.c_iflag = (tio.c_iflag & ~iflags[n].bit) |
				              (negated ? 0 : iflags[n].bit);
				goto found;
			}
			for ( size_t n = 0; n < ARRAY_SIZE(lflags); n++ )
			{
				if ( strcmp(name, lflags[n].name) != 0 )
					continue;
				tio.c_lflag = (tio.c_lflag & ~lflags[n].bit) |
				              (negated ? 0 : lflags[n].bit);
				goto found;
			}
			for ( size_t n = 0; n < ARRAY_SIZE(oflags); n++ )
			{
				if ( strcmp(name, oflags[n].name) != 0 )
					continue;
				tio.c_oflag = (tio.c_oflag & ~oflags[n].bit) |
				              (negated ? 0 : oflags[n].bit);
				goto found;
			}
			for ( size_t n = 0; n < ARRAY_SIZE(control_characters); n++ )
			{
				if ( strcmp(name, control_characters[n].name) != 0 )
					continue;
				if ( i + 1 == argc )
					errx(1, "missing argument to %s", name);
				const char* parameter = argv[++i];
				if ( !parameter[0] || !parameter[1] )
					tio.c_cc[control_characters[n].value] = parameter[0];
				else if ( !strcmp(parameter, "undef") ||
				          !strcmp(parameter, "^-") )
					tio.c_cc[control_characters[n].value] = _POSIX_VDISABLE;
				else if ( parameter[0] == '^' &&
				          (('@' <= parameter[1] && parameter[1] <= '_') ||
				           ('a' <= parameter[1] && parameter[1] <= 'z') ||
				           parameter[1] == '?') &&
				          !parameter[2] )
					tio.c_cc[control_characters[n].value] =
						CONTROL(toupper((unsigned char) parameter[1]));
				else if ( parameter[0] == 'M' &&
				          parameter[1] == '-' &&
				          32 <= parameter[2] && parameter[2] <= 126 &&
				          !parameter[3] )
					tio.c_cc[control_characters[n].value] =
						(unsigned char) parameter[2] + 128;
				else if ( parameter[0] == 'M' &&
				          parameter[1] == '-' &&
				          parameter[2] == '^' &&
				          (('@' <= parameter[3] && parameter[3] <= '_') ||
				           ('a' <= parameter[3] && parameter[3] <= 'z') ||
				           parameter[3] == '?') &&
				          !parameter[4] )
					tio.c_cc[control_characters[n].value] =
						M_CONTROL(toupper((unsigned char) parameter[3]));
				else if ( isdigit((unsigned char) parameter[0]) &&
				          isdigit((unsigned char) parameter[1]) &&
				          (!parameter[2] ||
				           (isdigit((unsigned char) parameter[2]) &&
				            !parameter[3])) &&
				          atoi(parameter) <= 255)
					tio.c_cc[control_characters[n].value] = atoi(parameter);
				else
					errx(1, "invalid control character: %s", parameter);
				goto found;
			}
			errx(1, "unknown operand: %s", arg);
			found:;
		}
	}

	if ( tcsetattr(tty, TCSANOW, &tio) < 0 )
		err(1, "tcsetattr: %s", tty_name);

	if ( set_ws && tcsetwinsize(tty, &ws) < 0 )
		err(1, "tcsetwinsize: %s", tty_name);

	return 0;
}
