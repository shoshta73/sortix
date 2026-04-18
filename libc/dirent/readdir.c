/*
 * Copyright (c) 2011, 2014, 2015, 2026 Jonas 'Sortie' Termansen.
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
 * dirent/readdir.c
 * Reads a directory entry from a directory stream into a DIR-specific buffer.
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>

struct dirent* readdir(DIR* dir)
{
	int old_errno = errno;
	if ( !dir->buffer )
	{
		size_t size = 32768;
		dir->buffer = malloc(size);
		if ( !dir->buffer )
			return NULL;
		dir->offset = 0;
		dir->used = 0;
		dir->size = size;
	}
	if ( dir->offset == dir->used )
	{
		ssize_t amount = getdents(dir->fd, dir->buffer, dir->size, 0);
		if ( amount < 0 )
			return NULL;
		if ( !amount )
			return errno = old_errno, NULL;
		dir->offset = 0;
		dir->used = (size_t) amount;
		assert(dir->used <= dir->used);
	}
	struct dirent* entry = (struct dirent*) (dir->buffer + dir->offset);
	dir->offset += entry->d_reclen;
	assert(dir->offset <= dir->used);
	return entry;
}
