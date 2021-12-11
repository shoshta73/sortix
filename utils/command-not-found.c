/*
 * Copyright (c) 2013, 2015, 2016, 2021 Jonas 'Sortie' Termansen.
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
 * command-not-found.c
 * Writes a notice that the attempted command wasn't found and suggests
 * possible altetnatives.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ARRAY_LENGTH(array) (sizeof(array) / sizeof((array)[0]))

static void suggest_logout(void)
{
	fprintf(stderr, " Exiting your shell normally to logout.\n");
}

enum category
{
	NONE,
	BROWSER,
	EDITOR,
	LOGOUT,
	MOUNT,
	PAGER,
	POWEROFF,
	RW,
	SHELL,
	UNMOUNT,
};

struct command
{
	enum category category;
	const char* command;
	const char* package;
	void (*suggest)(void);
};

struct command commands[] =
{
	{BROWSER, "chromium", NULL, NULL},
	{BROWSER, "chromium-browser", NULL, NULL},
	{BROWSER, "elinks", NULL, NULL},
	{BROWSER, "firefox", NULL, NULL},
	{BROWSER, "links", "links", NULL},
	{BROWSER, "lynx", NULL, NULL},
	{BROWSER, "w3m", NULL, NULL},
	{BROWSER, "www-browser", NULL, NULL},
	{BROWSER, "x-www-browser", NULL, NULL},

	{EDITOR, "ed", "ed", NULL},
	{EDITOR, "editor", "system", NULL},
	{EDITOR, "emacs", "emacs", NULL},
	{EDITOR, "nano", "nano", NULL},
	{EDITOR, "vim", "vim", NULL},
	{EDITOR, "vi", NULL, NULL},

	{LOGOUT, "logoff", NULL, NULL},
	{LOGOUT, "logout", NULL, suggest_logout},

	{MOUNT, "extfs", "system", NULL},
	{MOUNT, "mount", NULL, NULL},

	{PAGER, "less", NULL, NULL},
	{PAGER, "more", NULL, NULL},
	{PAGER, "pager", "system", NULL},

	{POWEROFF, "poweroff", "system", NULL},
	{POWEROFF, "shutdown", NULL, NULL},

	{RW, "dd", NULL, NULL},
	{RW, "rw", "system", NULL},

	{SHELL, "bash", NULL, NULL},
	{SHELL, "dash", "dash", NULL},
	{SHELL, "ksh", NULL, NULL},
	{SHELL, "sh", "system", NULL},
	{SHELL, "zsh", NULL, NULL},

	{UNMOUNT, "umount", NULL, NULL},
	{UNMOUNT, "unmount", "system", NULL},
};

static enum category find_category(const char* filename)
{
	for ( size_t i = 0; i < ARRAY_LENGTH(commands); i++ )
	{
		if ( commands[i].command && !strcmp(filename, commands[i].command) )
			return commands[i].category;
	}
	return NONE;
}

int main(int argc, char* argv[])
{
	const char* filename = 2 <= argc ? argv[1] : argv[0];

	enum category category = find_category(filename);
	if ( category != NONE )
	{
		fprintf(stderr, "No command '%s' found, did you mean:\n", filename);

		for ( size_t i = 0; i < ARRAY_LENGTH(commands); i++ )
		{
			if ( !commands[i].package && !commands[i].suggest )
				continue;
			else if ( commands[i].category == category )
			{
				if ( commands[i].suggest )
					commands[i].suggest();
				else if ( !strcmp(commands[i].package, "system") )
					fprintf(stderr, " Command '%s' from the base system\n",
					        commands[i].command);
				else
					fprintf(stderr, " Command '%s' from the package '%s'\n",
					        commands[i].command, commands[i].package);
			}
		}
	}

	fprintf(stderr, "%s: command not found\n", filename);
	return 127;
}
