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
 * autoconf.c
 * Parser for autoinstall.conf(5) and autoupgrade.conf(5).
 */

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "autoconf.h"

// TODO: This file is very unfinished draft stuff.

bool has_autoconf = false;

const char* autoconf_get(const char* name)
{
	if ( !name || !has_autoconf )
		return NULL;
	return getenv(name);
}

void autoconf_load(const char* path)
{
	FILE* fp = fopen(path, "r");
	if ( !fp )
	{
		if ( errno != ENOENT )
			warn("%s", path);
		return;
	}
	char* line = NULL;
	size_t line_size = 0;
	ssize_t line_length;
	while ( 0 < (line_length = getline(&line, &line_size, fp)) )
	{
		if ( line[line_length - 1] == '\n' )
			line[--line_length] = '\0';
		line_length = 0;
		while ( line[line_length] && line[line_length] != '#' )
			line_length++;
		line[line_length] = '\0';
		char* name = line;
		if ( !*name || *name == '=' )
			continue;
		size_t name_length = 1;
		while ( name[name_length] &&
		        name[name_length] != '=' &&
		        name[name_length] != '+' )
			name_length++;
		if ( name[name_length + 0] == '+' &&
		     name[name_length + 1] == '+' &&
		     name[name_length + 2] == '=' )
		{
			name[name_length + 0] = '\0';
			char* value = name + name_length + 3;
			const char* existing = getenv(name);
			if ( existing )
			{
				char* full;
				if ( asprintf(&full, "%s\n%s", existing, value) < 0 )
					err(2, "%s: asprintf", path);
				setenv(name, full, 1);
				free(full);
			}
			else
				setenv(name, value, 1);
		}
		else if ( name[name_length + 0] == '+' &&
		          name[name_length + 1] == '=' )
		{
			name[name_length + 0] = '\0';
			char* value = name + name_length + 2;
			const char* existing = getenv(name);
			if ( existing )
			{
				char* full;
				if ( asprintf(&full, "%s %s", existing, value) < 0 )
					err(2, "%s: asprintf", path);
				setenv(name, full, 1);
				free(full);
			}
			else
				setenv(name, value, 1);
		}
		else if ( name[name_length + 0] == '=' )
		{
			name[name_length + 0] = '\0';
			char* value = name + name_length + 1;
			setenv(name, value, 1);
		}
		else
		{
			// TODO: Graceful.
			errx(2, "%s: Bad line: %s", path, line);
		}
		char* value = name + name_length;
		while ( *value && isblank((unsigned char) *value) )
			value++;
		if ( *value != '=' )
			continue;
		value++;
		while ( *value && isblank((unsigned char) *value) )
			value++;
		name[name_length] = '\0';
	}
	// TODO: Graceful error.
	if ( ferror(fp) )
		err(2, "%s", path);
	free(line);
	fclose(fp);
	has_autoconf = true;
}
