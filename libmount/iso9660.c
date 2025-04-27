/*
 * Copyright (c) 2022, 2025 Jonas 'Sortie' Termansen.
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
 * iso9660.c
 * iso9660 filesystem.
 */

#include <endian.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <mount/blockdevice.h>
#include <mount/filesystem.h>
#include <mount/iso9660.h>

#include "util.h"

static size_t isostrnlen(const char* str, size_t size)
{
	size_t result = strnlen(str, size);
	while ( result && str[result - 1] == ' ' )
		result--;
	return result;
}

static bool filesystem_add_iso_identifier(struct filesystem* fs,
                                          const char* key,
                                          const char* field,
                                          size_t field_size)
{
	char* value = strndup(field, isostrnlen(field, field_size));
	if ( !value )
		return false;
	bool result = filesystem_add_identifier(fs, key, value);
	free(value);
	return result;
}

static size_t iso9660_probe_amount(struct blockdevice* bdev)
{
	(void) bdev;
	blksize_t sector_size = 2048;
	blksize_t logical_sector_size = sector_size < 2048 ? 2048 : sector_size;
	return 17 * (size_t) logical_sector_size;
}

static bool iso9660_probe(struct blockdevice* bdev,
                          const unsigned char* leading,
                          size_t amount)
{
	(void) bdev;
	blksize_t sector_size = 2048;
	blksize_t logical_sector_size = sector_size < 2048 ? 2048 : sector_size;
	if ( amount < 17 * (size_t) logical_sector_size )
		return false;
	size_t offset = 16 * (size_t) logical_sector_size;
	return !memcmp(leading + offset + 1, "CD001", 5);
}

static void iso9660_release(struct filesystem* fs)
{
	if ( !fs )
		return;
	free(fs);
}

static enum filesystem_error iso9660_inspect(struct filesystem** fs_ptr,
                                             struct blockdevice* bdev)
{
	*fs_ptr = NULL;
	struct filesystem* fs = CALLOC_TYPE(struct filesystem);
	if ( !fs )
		return FILESYSTEM_ERROR_ERRNO;
	fs->bdev = bdev;
	fs->handler = &iso9660_handler;
	fs->handler_private = NULL;
	fs->fstype_name = "iso9660";
	fs->driver = "iso9660fs";
	blksize_t sector_size = 2048;
	blksize_t logical_sector_size = sector_size < 2048 ? 2048 : sector_size;
	size_t offset = 16 * (size_t) logical_sector_size;
	struct iso9660_pvd pvd;
	if ( blockdevice_preadall(bdev, &pvd, sizeof(pvd), offset) != sizeof(pvd) )
		return iso9660_release(fs), FILESYSTEM_ERROR_ERRNO;
	char uuid[] = "YYYY-MM-DD-HH-MM-SS-CC";
	char* d = pvd.creation_datetime;
	snprintf(uuid, sizeof(uuid), "%c%c%c%c-%c%c-%c%c-%c%c-%c%c-%c%c-%c%c",
	         d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10],
	         d[11], d[12], d[13], d[14], d[15]);
	if ( !filesystem_add_identifier(fs, "TYPE", "iso9660") ||
	     !filesystem_add_identifier(fs, "UUID", uuid) ||
	     !filesystem_add_iso_identifier(fs, "SYSTEM_ID",
	                                    pvd.system_identifier,
	                                    sizeof(pvd.system_identifier)) ||
	     !filesystem_add_iso_identifier(fs, "LABEL",
	                                    pvd.volume_identifier,
	                                    sizeof(pvd.volume_identifier)) ||
	     !filesystem_add_iso_identifier(fs, "VOLUME_SET_ID",
	                                    pvd.volume_set_identifier,
	                                    sizeof(pvd.volume_set_identifier)) ||
	     !filesystem_add_iso_identifier(fs, "PUBLISHER_ID",
	                                    pvd.publisher_identifier,
	                                    sizeof(pvd.publisher_identifier)) ||
	     !filesystem_add_iso_identifier(fs, "DATA_PREPARER_ID",
	                                    pvd.data_preparer_identifier,
	                                    sizeof(pvd.data_preparer_identifier)) ||
	     !filesystem_add_iso_identifier(fs, "APPLICATION_ID",
	                                    pvd.application_identifier,
	                                    sizeof(pvd.application_identifier)) ||
	     !filesystem_add_iso_identifier(fs, "COPYRIGHT_ID",
	                                    pvd.copyright_file_identifier,
	                                    sizeof(pvd.copyright_file_identifier)) ||
	     !filesystem_add_iso_identifier(fs, "ABSTRACT_ID",
	                                    pvd.abstract_file_identifier,
	                                    sizeof(pvd.abstract_file_identifier)) ||
	     !filesystem_add_iso_identifier(fs, "BIBLIOGRAPHIC_ID",
	                                    pvd.bibliographic_file_identifier,
	                                    sizeof(pvd.bibliographic_file_identifier)) )
		return iso9660_release(fs), FILESYSTEM_ERROR_ERRNO;
	return *fs_ptr = fs, FILESYSTEM_ERROR_NONE;
}

const struct filesystem_handler iso9660_handler =
{
	.handler_name = "iso9660",
	.flags = FILESYSTEM_HANDLER_FLAG_IGNORE_PARTITIONS,
	.probe_amount = iso9660_probe_amount,
	.probe = iso9660_probe,
	.inspect = iso9660_inspect,
	.release = iso9660_release,
};
