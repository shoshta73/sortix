/*
 * Copyright (c) 2023, 2024, 2025 Jonas 'Sortie' Termansen.
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
 * mount/fat.h
 * File Allocation Table filesystem.
 */

#ifndef INCLUDE_MOUNT_FAT_H
#define INCLUDE_MOUNT_FAT_H

#include <assert.h>
#include <stdint.h>

#include <mount/filesystem.h>

#define BDP_GPT_TYPE_GUID "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
#define ESP_GPT_TYPE_GUID "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"

struct fat_bpb
{
	uint8_t jump[3];
	char oem[8];
	uint8_t bytes_per_sector_low;
	uint8_t bytes_per_sector_high;
	uint8_t sectors_per_cluster;
	uint16_t reserved_sectors;
	uint8_t fat_count;
	uint8_t root_dirent_count_low;
	uint8_t root_dirent_count_high;
	uint8_t total_sectors_low;
	uint8_t total_sectors_high;
	uint8_t media_descriptor_type;
	uint16_t sectors_per_fat;
	uint16_t sectors_per_track;
	uint16_t head_count;
	uint16_t hidden_sectors;
	uint32_t total_sectors_large;
	union
	{
		struct
		{
			uint8_t fat12_drive_number;
			uint8_t fat12_reserved;
			uint8_t fat12_signature;
			uint8_t fat12_volume_id[4];
			uint8_t fat12_volume_label[11];
			uint8_t fat12_system[8];
		};
		struct
		{
			uint32_t fat32_sectors_per_fat;
			uint16_t fat32_flags;
			uint16_t fat32_version;
			uint32_t fat32_root_cluster;
			uint16_t fat32_fsinfo;
			uint16_t fat32_backup_boot;
			uint32_t fat32_reserved1[3];
			uint8_t fat32_drive_number;
			uint8_t fat32_reserved2;
			uint8_t fat32_signature;
			uint8_t fat32_volume_id[4];
			uint8_t fat32_volume_label[11];
			uint8_t fat32_system[8];
		};
		struct
		{
			uint8_t bootloader[510 - 36];
			uint8_t boot_signature[2];
		};
	};
};

static_assert(sizeof(struct fat_bpb) == 512, "sizeof(struct fat_bpb) == 512");

#if defined(__cplusplus)
extern "C" {
#endif

extern const struct filesystem_handler fat_handler;

#if defined(__cplusplus)
} /* extern "C" */
#endif

#endif
