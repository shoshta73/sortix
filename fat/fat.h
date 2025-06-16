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
 * fat.h
 * The File Allocation Table (FAT) filesystem.
 */

#ifndef FAT_H
#define FAT_H

#include <assert.h>
#include <stdint.h>

typedef uint32_t fat_ino_t;
typedef uint32_t fat_off_t;
typedef uint64_t fat_block_t;

#define FAT_OFF_MAX UINT32_MAX
#define FAT_UTF8_NAME_MAX (3 * 255)
#define FAT_UTF8_NAME_MAX_BUF (20 * 13)
#define FAT_UTF16_NAME_MAX 255

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

struct fat_fsinfo
{
	uint32_t signature1;
	uint32_t reserved1[120];
	uint32_t signature2;
	uint32_t free_count;
	uint32_t next_free;
	uint32_t reserved2[3];
	uint32_t signature3;
};

#define FAT_FSINFO_SIGNATURE1 0x41615252
#define FAT_FSINFO_SIGNATURE2 0x61417272
#define FAT_FSINFO_SIGNATURE3 0xAA550000

static_assert(sizeof(struct fat_fsinfo) == 512, "sizeof(struct fat_fsinfo) == 512");

struct fat_dirent
{
	union
	{
		char name[11];
		struct
		{
			uint8_t ord;
			char name1[10];
		};
	};
	uint8_t attributes;
	union
	{
		struct
		{
			uint8_t reserved;
			uint8_t creation_centis;
			uint16_t creation_time;
			uint16_t creation_date;
			uint16_t access_date;
			uint16_t cluster_high;
			uint16_t modified_time;
			uint16_t modified_date;
			uint16_t cluster_low;
			uint32_t size;
		};
		struct
		{
			uint8_t type;
			uint8_t checksum;
			char name2[12];
			uint16_t zero;
			char name3[4];
		};
	};
};

static_assert(sizeof(struct fat_dirent) == 32, "sizeof(struct fat_dirent) == 32");

#define FAT_ATTRIBUTE_READ_ONLY (1 << 0)
#define FAT_ATTRIBUTE_HIDDEN (1 << 1)
#define FAT_ATTRIBUTE_SYSTEM (1 << 2)
#define FAT_ATTRIBUTE_VOLUME_ID (1 << 3)
#define FAT_ATTRIBUTE_DIRECTORY (1 << 4)
#define FAT_ATTRIBUTE_ARCHIVE (1 << 5)

#define FAT_ATTRIBUTE_LONG_NAME 0x0F
#define FAT_ATTRIBUTE_LONG_NAME_MASK 0x3F

#define FAT_RESERVED_LOWER_NAME (1 << 3)
#define FAT_RESERVED_LOWER_EXT (1 << 4)

#define FAT_LONG_NAME_LAST 0x40
#define FAT_LONG_NAME_ORD_MASK 0x3F

#endif
