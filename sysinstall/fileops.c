/*
 * Copyright (c) 2015, 2016, 2017, 2020, 2021 Jonas 'Sortie' Termansen.
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
 * fileops.c
 * File operation utility functions.
 */

#include <sys/kernelinfo.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <ioleast.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fileops.h"
#include "string_array.h"

char* join_paths(const char* a, const char* b)
{
	size_t a_len = strlen(a);
	bool has_slash = (a_len && a[a_len-1] == '/') || b[0] == '/';
	char* result;
	if ( (has_slash && asprintf(&result, "%s%s", a, b) < 0) ||
	     (!has_slash && asprintf(&result, "%s/%s", a, b) < 0) )
		return NULL;
	return result;
}

int mkdir_p(const char* path, mode_t mode)
{
	int saved_errno = errno;
	if ( !mkdir(path, mode) )
		return 0;
	if ( errno == ENOENT )
	{
		char* prev = strdup(path);
		if ( !prev )
			return -1;
		int status = mkdir_p(dirname(prev), mode | 0500);
		free(prev);
		if ( status < 0 )
			return -1;
		errno = saved_errno;
		if ( !mkdir(path, mode) )
			return 0;
	}
	if ( errno == EEXIST )
		return errno = saved_errno, 0;
	return -1;
}

int access_or_die(const char* path, int mode)
{
	if ( access(path, mode) < 0 )
	{
		if ( errno == EACCES ||
		     errno == ENOENT ||
             errno == ELOOP ||
             errno == ENAMETOOLONG ||
             errno == ENOTDIR )
			return -1;
		warn("%s", path);
		_exit(2);
	}
	return 0;
}

int access_join_or_die(const char* a, const char* b, int mode)
{
	char* path = join_paths(a, b);
	if ( !path )
	{
		warn("malloc");
		_exit(2);
	}
	int result = access_or_die(path, mode);
	free(path);
	return result;
}

void mkdir_or_chmod_or_die(const char* path, mode_t mode)
{
	if ( mkdir(path, mode) == 0 )
		return;
	if ( errno == EEXIST )
	{
		if ( chmod(path, mode) == 0 )
			return;
		warn("chmod: %s", path);
		_exit(2);
	}
	warn("mkdir: %s", path);
	_exit(2);
}

void write_random_seed(const char* path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
	if ( fd < 0 )
	{
		warn("%s", path);
		_exit(2);
	}
	if ( fchown(fd, 0, 0) < 0 )
	{
		warn("chown: %s", path);
		_exit(2);
	}
	if ( fchmod(fd, 0600) < 0 )
	{
		warn("chmod: %s", path);
		_exit(2);
	}
	// Write out randomness, but mix in some fresh kernel randomness in case the
	// randomness used to seed arc4random didn't have enough entropy, there may
	// be more now.
	unsigned char buf[256];
	arc4random_buf(buf, sizeof(buf));
	unsigned char newbuf[256];
	getentropy(newbuf, sizeof(newbuf));
	size_t done = writeall(fd, buf, sizeof(buf));
	explicit_bzero(buf, sizeof(buf));
	for ( size_t i = 0; i < 256; i++ )
		buf[i] ^= newbuf[i];
	if ( done < sizeof(buf) )
	{
		warn("write: %s", path);
		_exit(2);
	}
	if ( ftruncate(fd, sizeof(buf)) < 0  )
	{
		warn("truncate: %s", path);
		_exit(2);
	}
	close(fd);
}

char* read_string_file(const char* path)
{
	FILE* fp = fopen(path, "r");
	if ( !fp )
		return NULL;
	struct stat st;
	if ( fstat(fileno(fp), &st) < 0 )
	{
		fclose(fp);
		return NULL;
	}
	size_t content_length;
	if ( __builtin_add_overflow(1, st.st_size, &content_length) )
	{
		fclose(fp);
		errno = EOVERFLOW;
		return NULL;
	}
	char* content = malloc(content_length);
	if ( !content )
	{
		fclose(fp);
		return NULL;
	}
	size_t amount = fread(content, 1, content_length - 1, fp);
	if ( ferror(fp) )
	{
		fclose(fp);
		free(content);
		return NULL;
	}
	fclose(fp);
	if ( 0 < amount && content[amount - 1] == '\n' )
		content[amount - 1] = '\0';
	else
		content[amount] = '\0';
	return content;
}

char** read_lines_file(const char* path, size_t* out_count)
{
	FILE* fp = fopen(path, "r");
	if ( !fp )
		return NULL;
	size_t count;
	size_t length;
	char** lines;
	if ( !string_array_init(&lines, &count, &length) )
	{
		fclose(fp);
		return NULL;
	}
	char* line = NULL;
	size_t line_size = 0;
	ssize_t line_length;
	while ( 0 < (errno = 0, line_length = getline(&line, &line_size, fp)) )
	{
		if ( line[line_length-1] == '\n' )
			line[--line_length] = '\0';
		if ( !string_array_append_nodup(&lines, &count, &length, line) )
		{
			free(line);
			string_array_free(&lines, &count, &length);
			fclose(fp);
			return false;
		}
		line = NULL;
		line_size = 0;
	}
	free(line);
	if ( ferror(fp) )
	{
		string_array_free(&lines, &count, &length);
		fclose(fp);
		return NULL;
	}
	fclose(fp);
	*out_count = count;
	return lines;
}

char* akernelinfo(const char* request)
{
	char* buffer = NULL;
	size_t size = 0;
	while ( true )
	{
		errno = 0;
		ssize_t needed = kernelinfo(request, buffer, size);
		if ( needed < 0 )
			return free(buffer), NULL;
		if ( errno != ERANGE )
			return buffer;
		size = (size_t) needed + 1;
		free(buffer);
		if ( !(buffer = malloc(size)) )
			return NULL;
	}
}
