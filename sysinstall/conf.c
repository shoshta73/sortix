/*
 * Copyright (c) 2015, 2016, 2017, 2021 Jonas 'Sortie' Termansen.
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
 * conf.c
 * Utility functions to handle upgrade.conf(5).
 */

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "conf.h"

void conf_init(struct conf* conf)
{
	memset(conf, 0, sizeof(*conf));
	conf->ports = true;
	conf->system = true;
}

void conf_free(struct conf* conf)
{
	free(conf->channel);
	free(conf->mirror);
	free(conf->release_key);
	free(conf->release_sig_url);
	conf_init(conf);
}

static bool conf_boolean(const char* name,
                         const char* value,
                         const char* path,
                         off_t line_number)
{
	if ( !strcmp(value, "yes") )
		return true;
	if ( !strcmp(value, "no") )
		return false;
	printf("%s:%ji: %s: Expected yes or no instead of unsupported value\n",
	       path, (intmax_t) line_number, name);
	return false;
}

static bool conf_assign(struct conf* conf,
                        const char* name,
                        const char* value,
                        const char* path,
                        off_t line_number)
{
	char* new_value;
	if ( !strcmp(name, "channel") )
	{
		if ( !(new_value = strdup(value)) )
			return false;
		free(conf->channel);
		conf->channel = new_value;
	}
	else if ( !strcmp(name, "force_mirror") )
		conf->force_mirror = conf_boolean(name, value, path, line_number);
	else if ( !strcmp(name, "grub") )
		conf->grub = conf_boolean(name, value, path, line_number);
	else if ( !strcmp(name, "mirror") )
	{
		if ( !(new_value = strdup(value)) )
			return false;
		free(conf->mirror);
		conf->mirror = new_value;
	}
	else if ( !strcmp(name, "newsrc") )
		conf->newsrc = conf_boolean(name, value, path, line_number);
	else if ( !strcmp(name, "ports") )
		conf->ports = conf_boolean(name, value, path, line_number);
	else if ( !strcmp(name, "release_key") )
	{
		if ( !(new_value = strdup(value)) )
			return false;
		free(conf->release_key);
		conf->release_key = new_value;
	}
	else if ( !strcmp(name, "release_sig_url") )
	{
		if ( !(new_value = strdup(value)) )
			return false;
		free(conf->release_sig_url);
		conf->release_sig_url = new_value;
	}
	else if ( !strcmp(name, "src") )
		conf->src = conf_boolean(name, value, path, line_number);
	else if ( !strcmp(name, "system") )
		conf->system = conf_boolean(name, value, path, line_number);
	else
		printf("%s:%ji: Unsupported variable: %s\n", path,
		       (intmax_t) line_number, name);
	return true;
}

bool conf_load(struct conf* conf, const char* path)
{
	FILE* fp = fopen(path, "r");
	if ( !fp )
		return false;
	char* line = NULL;
	size_t line_size = 0;
	ssize_t line_length;
	intmax_t line_number = 0;
	bool success = true;
	while ( 0 < (line_length = getline(&line, &line_size, fp)) )
	{
		line_number++;
		if ( line[line_length - 1] == '\n' )
			line[--line_length] = '\0';
		line_length = 0;
		while ( line[line_length] && line[line_length] != '#' )
			line_length++;
		line[line_length] = '\0';
		while ( line_length && isblank((unsigned char) line[line_length - 1]) )
			line[--line_length] = '\0';
		char* name = line;
		while ( *name && isblank((unsigned char) *name) )
			name++;
		if ( !*name )
			continue;
		if ( *name == '=' )
		{
			printf("%s:%ji: Ignoring malformed line\n", path, line_number);
			continue;
		}
		size_t name_length = 1;
		while ( name[name_length] &&
		        !isblank((unsigned char) name[name_length]) &&
		        name[name_length] != '=' )
			name_length++;
		char* value = name + name_length;
		while ( *value && isblank((unsigned char) *value) )
			value++;
		if ( *value != '=' )
		{
			printf("%s:%ji: Ignoring malformed line\n", path, line_number);
			continue;
		}
		value++;
		while ( *value && isblank((unsigned char) *value) )
			value++;
		name[name_length] = '\0';
		if ( !conf_assign(conf, name, value, path, line_number) )
		{
			success = false;
			break;
		}
	}
	if ( ferror(fp) )
		success = false;
	free(line);
	fclose(fp);
	return success;
}
