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
 * iso9660.h
 * Data structures for the ISO 9660 filesystem.
 */

#ifndef ISO9660_H
#define ISO9660_H

#include <stdint.h>

const uint8_t TYPE_BOOT_RECORD = 0x00;
const uint8_t TYPE_PRIMARY_VOLUME_DESCRIPTOR = 0x01;
const uint8_t TYPE_VOLUME_DESCRIPTOR_SET_TERMINATOR = 0xFF;

struct iso9660_pvd /* primary volume descriptor */
{
	uint8_t type;
	char standard_identifier[5];
	uint8_t version;
	uint8_t unused1;
	char system_identifier[32];
	char volume_identifier[32];
	uint8_t unused2[8];
	uint32_t volume_space_size_le;
	uint32_t volume_space_size_be;
	uint8_t unused3[32];
	uint16_t volume_set_size_le;
	uint16_t volume_set_size_be;
	uint16_t volume_sequence_number_le;
	uint16_t volume_sequence_number_be;
	uint16_t logical_block_size_le;
	uint16_t logical_block_size_be;
	uint32_t path_table_size_le;
	uint32_t path_table_size_be;
	uint32_t path_table_lba_le;
	uint32_t path_table_opt_lba_le;
	uint32_t path_table_lba_be;
	uint32_t path_table_opt_lba_be;
	uint8_t root_dirent[34];
	char volume_set_identifier[128];
	char publisher_identifier[128];
	char data_preparer_identifier[128];
	char application_identifier[128];
	char copyright_file_identifier[37];
	char abstract_file_identifier[37];
	char bibliographic_file_identifier[37];
	char creation_datetime[17];
	char modification_datetime[17];
	char expiration_datetime[17];
	char effective_datetime[17];
	uint8_t file_structure_version;
	uint8_t unused4;
	uint8_t application_use[512];
	uint8_t reserved[653];
};

static_assert(sizeof(struct iso9660_pvd) == 2048,
              "sizeof(struct iso9660_pvd) == 2048");

typedef uint64_t iso9660_ino_t;

struct iso9660_dirent
{
	uint8_t unused;
};

#define ISO9660_DIRENT_FLAG_NO_EXIST (1 << 0)
#define ISO9660_DIRENT_FLAG_DIR (1 << 1)
#define ISO9660_DIRENT_FLAG_ASSOCIATED (1 << 2)
#define ISO9660_DIRENT_FLAG_RECORD (1 << 3)
#define ISO9660_DIRENT_FLAG_PROTECTION (1 << 4)
#define ISO9660_DIRENT_FLAG_MULTI_EXTENT (1 << 7)

#define ISO9660_S_IFMT (0170000)
#define ISO9660_S_IFIFO (0010000)
#define ISO9660_S_IFCHR (0020000)
#define ISO9660_S_IFDIR (0040000)
#define ISO9660_S_IFBLK (0060000)
#define ISO9660_S_IFREG (0100000)
#define ISO9660_S_IFLNK (0120000)
#define ISO9660_S_IFSOCK (0140000)

#define ISO9660_S_ISSOCK(mode) (((mode) & ISO9660_S_IFMT) == ISO9660_S_IFSOCK)
#define ISO9660_S_ISLNK(mode) (((mode) & ISO9660_S_IFMT) == ISO9660_S_IFLNK)
#define ISO9660_S_ISREG(mode) (((mode) & ISO9660_S_IFMT) == ISO9660_S_IFREG)
#define ISO9660_S_ISBLK(mode) (((mode) & ISO9660_S_IFMT) == ISO9660_S_IFBLK)
#define ISO9660_S_ISDIR(mode) (((mode) & ISO9660_S_IFMT) == ISO9660_S_IFDIR)
#define ISO9660_S_ISCHR(mode) (((mode) & ISO9660_S_IFMT) == ISO9660_S_IFCHR)
#define ISO9660_S_ISFIFO(mode) (((mode) & ISO9660_S_IFMT) == ISO9660_S_IFIFO)

static const uint8_t ISO9660_FT_UNKNOWN = 0;
static const uint8_t ISO9660_FT_REG_FILE = 1;
static const uint8_t ISO9660_FT_DIR = 2;
static const uint8_t ISO9660_FT_CHRDEV = 3;
static const uint8_t ISO9660_FT_BLKDEV = 4;
static const uint8_t ISO9660_FT_FIFO = 5;
static const uint8_t ISO9660_FT_SOCK = 6;
static const uint8_t ISO9660_FT_SYMLINK = 7;

static inline uint8_t ISO9660_FT_OF_MODE(mode_t mode)
{
	if ( ISO9660_S_ISREG(mode) )
		return ISO9660_FT_REG_FILE;
	if ( ISO9660_S_ISDIR(mode) )
		return ISO9660_FT_DIR;
	if ( ISO9660_S_ISCHR(mode) )
		return ISO9660_FT_CHRDEV;
	if ( ISO9660_S_ISBLK(mode) )
		return ISO9660_FT_BLKDEV;
	if ( ISO9660_S_ISFIFO(mode) )
		return ISO9660_FT_FIFO;
	if ( ISO9660_S_ISSOCK(mode) )
		return ISO9660_FT_SOCK;
	if ( ISO9660_S_ISLNK(mode) )
		return ISO9660_FT_SYMLINK;
	return ISO9660_FT_UNKNOWN;
}

static const uint8_t ISO9660_NM_CONTINUE = 1 << 0;
static const uint8_t ISO9660_NM_CURRENT = 1 << 1;
static const uint8_t ISO9660_NM_PARENT = 1 << 2;

static const uint8_t ISO9660_SL_CONTINUE = 1 << 0;
static const uint8_t ISO9660_SL_CURRENT = 1 << 1;
static const uint8_t ISO9660_SL_PARENT = 1 << 2;
static const uint8_t ISO9660_SL_ROOT = 1 << 3;

static const uint8_t ISO9660_TF_CREATION = 1 << 0;
static const uint8_t ISO9660_TF_MODIFY = 1 << 1;
static const uint8_t ISO9660_TF_ACCESS = 1 << 2;
static const uint8_t ISO9660_TF_ATTRIBUTES = 1 << 3;
static const uint8_t ISO9660_TF_BACKUP = 1 << 4;
static const uint8_t ISO9660_TF_EXPIRATION = 1 << 5;
static const uint8_t ISO9660_TF_EFFECTIVE = 1 << 6;
static const uint8_t ISO9660_TF_LONG_FORM = 1 << 7;

#endif
