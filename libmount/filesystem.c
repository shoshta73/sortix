/*
 * Copyright (c) 2015, 2025 Jonas 'Sortie' Termansen.
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
 * filesystem.c
 * Filesystem abstraction.
 */

#include <sys/stat.h>
#include <sys/types.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mount/biosboot.h>
#include <mount/blockdevice.h>
#include <mount/ext2.h>
#include <mount/extended.h>
#include <mount/filesystem.h>
#include <mount/harddisk.h>
#include <mount/iso9660.h>
#include <mount/partition.h>

const char* filesystem_error_string(enum filesystem_error error)
{
	switch ( error )
	{
	case FILESYSTEM_ERROR_NONE:
		break;
	case FILESYSTEM_ERROR_ABSENT:
		return "No filesystem found";
	case FILESYSTEM_ERROR_UNRECOGNIZED:
		return "Unrecognized filesystem type";
	case FILESYSTEM_ERROR_ERRNO:
		return (const char*) strerror(errno);
	}
	return "Unknown error condition";
}

static const struct filesystem_handler* filesystem_handlers[] =
{
	&biosboot_handler,
	&extended_handler,
	&ext2_handler,
	// TODO: Applications should search for ISO 9660 filesystems only on the
	//       root block device even if there's a MBR/GPT.
	&iso9660_handler,
	NULL,
};

void filesystem_release(struct filesystem* fs)
{
	if ( !fs || !fs->handler )
		return;
	for ( size_t i = 0; i < fs->identifiers_used; i++ )
		free(fs->identifiers[i]);
	free(fs->identifiers);
	fs->identifiers = NULL;
	fs->identifiers_used = 0;
	fs->identifiers_length = 0;
	fs->handler->release(fs);
}

enum filesystem_error
blockdevice_inspect_filesystem(struct filesystem** fs_ptr,
                               struct blockdevice* bdev)
{
	*fs_ptr = NULL;
	size_t leading_size = 65536;
	for ( size_t i = 0; filesystem_handlers[i]; i++ )
	{
		const struct filesystem_handler* handler = filesystem_handlers[i];
		if ( bdev->pt &&
		     !(handler->flags & FILESYSTEM_HANDLER_FLAG_IGNORE_PARTITIONS) )
			continue;
		size_t amount = filesystem_handlers[i]->probe_amount(bdev);
		if ( leading_size < amount )
			leading_size = amount;
	}
	unsigned char* leading = (unsigned char*) malloc(leading_size);
	if ( !leading )
		return *fs_ptr = NULL, FILESYSTEM_ERROR_ERRNO;
	size_t amount = blockdevice_preadall(bdev, leading, leading_size, 0);
	if ( amount < leading_size && errno != EEOF )
		return free(leading), *fs_ptr = NULL, FILESYSTEM_ERROR_ERRNO;
	for ( size_t i = 0; filesystem_handlers[i]; i++ )
	{
		const struct filesystem_handler* handler = filesystem_handlers[i];
		if ( bdev->pt &&
		     !(handler->flags & FILESYSTEM_HANDLER_FLAG_IGNORE_PARTITIONS) )
			continue;
		if ( !handler->probe(bdev, leading, amount) )
			continue;
		free(leading);
		return handler->inspect(fs_ptr, bdev);
	}
	for ( size_t i = 0; i < amount; i++ )
		if ( !leading[i] )
			return free(leading), FILESYSTEM_ERROR_UNRECOGNIZED;
	return free(leading), FILESYSTEM_ERROR_ABSENT;
}

bool filesystem_add_identifier(struct filesystem* fs, const char* key,
                               const char* value)
{
	if ( !value[0] )
		return true;
	if ( fs->identifiers_used == fs->identifiers_length )
	{
		size_t old_size = fs->identifiers_length ? fs->identifiers_length : 2;
		char** new_identifiers =
			reallocarray(fs->identifiers, old_size, 2 * sizeof(char*));
		if ( !new_identifiers )
			return false;
		fs->identifiers = new_identifiers;
		fs->identifiers_length = 2 * old_size;
	}
	char* identifier;
	if ( asprintf(&identifier, "%s=%s", key, value) < 0 )
		return false;
	fs->identifiers[fs->identifiers_used++] = identifier;
	return true;
}

const char* filesystem_get_identifier(const struct filesystem* fs,
                                      const char* id)
{
	size_t id_len = strlen(id);
	for ( size_t i = 0; i < fs->identifiers_used; i++ )
	{
		if ( !strncmp(fs->identifiers[i], id, id_len) &&
		     fs->identifiers[i][id_len] == '=' )
			return fs->identifiers[i] + id_len + 1;
	}
	return NULL;
}

bool filesystem_match(const struct filesystem* fs, const char* spec)
{
	if ( spec[0] != '/' && strchr(spec, '=') )
	{
		for ( size_t i = 0; i < fs->identifiers_used; i++ )
			if ( !strcmp(fs->identifiers[i], spec) )
				return true;
		return false;
	}
	if ( fs->bdev->p && !strcmp(spec, fs->bdev->p->path) )
		return true;
	else if ( !fs->bdev->p && fs->bdev->hd && !strcmp(spec, fs->bdev->hd->path) )
		return true;
	return false;
}

const char* filesystem_get_mount_spec(const struct filesystem* fs)
{
	const char* prefix = "UUID=";
	size_t prefix_len = strlen(prefix);
	for ( size_t i = 0; i < fs->identifiers_used; i++ )
	{
		if ( !strncmp(fs->identifiers[i], prefix, prefix_len) )
			return fs->identifiers[i];
	}
	if ( fs->bdev->p )
		return fs->bdev->p->path;
	else if ( fs->bdev->hd )
		return fs->bdev->hd->path;
	return NULL;
}
