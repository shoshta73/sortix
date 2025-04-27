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
 * mount/iso9660.h
 * ISO 9660 filesystem.
 */

#ifndef INCLUDE_MOUNT_ISO9660_H
#define INCLUDE_MOUNT_ISO9660_H

#include <stdint.h>

#include <mount/filesystem.h>

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

#if defined(__cplusplus)
extern "C" {
#endif

extern const struct filesystem_handler iso9660_handler;

#if defined(__cplusplus)
} /* extern "C" */
#endif

#endif
