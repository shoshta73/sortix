/*
 * Copyright (c) 2023, 2025 Jonas 'Sortie' Termansen.
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
 * fat.c
 * File Allocation Table filesystem.
 */

#include <endian.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <mount/blockdevice.h>
#include <mount/fat.h>
#include <mount/filesystem.h>
#include <mount/partition.h>
#include <mount/uuid.h>

#include "util.h"

struct fat_private
{
	uint8_t fat_type;
	uint16_t bytes_per_sector;
	uint64_t fat_lba;
};

static size_t fat_probe_amount(struct blockdevice* bdev)
{
	(void) bdev;
	return sizeof(struct fat_bpb);
}

bool fat_read_block(struct blockdevice* bdev, struct fat_private* priv,
                     unsigned char* data, uint64_t block_id)
{
	uint64_t offset = priv->bytes_per_sector * block_id;
	return blockdevice_preadall(bdev, data, priv->bytes_per_sector, offset) ==
	        priv->bytes_per_sector;
}

bool fat_read(struct blockdevice* bdev, struct fat_private* priv,
              uint32_t cluster, uint32_t* out)
{
	uint32_t result = 0;
	unsigned char data[priv->bytes_per_sector];
	//assert(cluster < 2 + priv->cluster_count);
	if ( priv->fat_type == 12 )
	{
		uint32_t position = cluster + (cluster / 2);
		uint32_t lba = position / priv->bytes_per_sector;
		uint32_t offset = position % priv->bytes_per_sector;
		if ( !fat_read_block(bdev, priv, data, priv->fat_lba + lba) )
			return false;
		uint8_t lower = data[offset];
		if ( ++offset == priv->bytes_per_sector )
		{
			offset = 0;
			lba++;
			if ( !fat_read_block(bdev, priv, data, priv->fat_lba + lba) )
				return false;
		}
		uint8_t higher = data[offset];
		uint16_t value = lower | higher << 8;
		if ( cluster & 1 )
			result = value >> 4;
		else
			result = value & 0xFFF;
		return *out = result, true;
	}
	uint32_t fat_size = priv->fat_type / 8;
	uint32_t position = cluster * fat_size;
	uint32_t lba = position / priv->bytes_per_sector;
	uint32_t entry = (position % priv->bytes_per_sector) / fat_size;
	if ( !fat_read_block(bdev, priv, data, priv->fat_lba + lba) )
		return false;
	if ( priv->fat_type == 16 )
		result = le16toh(((uint16_t*) data)[entry]);
	else if ( priv->fat_type == 32 )
		result = le32toh(((uint32_t*) data)[entry]) & 0x0FFFFFFF;
	return *out = result, true;
}

static int fat_determine(struct blockdevice* bdev, const struct fat_bpb* bpb,
                         struct fat_private* priv)
{
	if ( !(bpb->boot_signature[0] == 0x55 && bpb->boot_signature[1] == 0xAA) )
		return false;
	if ( !(bpb->jump[0] == 0xEB && bpb->jump[2] == 0x90) &&
	     !(bpb->jump[0] == 0xE9) )
		return false;
	uint16_t bytes_per_sector =
		bpb->bytes_per_sector_low | bpb->bytes_per_sector_high << 8;
	if ( bytes_per_sector < 512 ||
	     (bytes_per_sector & (bytes_per_sector - 1)) ||
	     4096 < bytes_per_sector )
		return false;
	uint16_t root_dirent_count =
		bpb->root_dirent_count_low | bpb->root_dirent_count_high << 8;
	uint32_t root_dir_size = root_dirent_count * 32;
	uint32_t root_dir_sectors =
		(root_dir_size + bytes_per_sector - 1) / bytes_per_sector;
	if ( (root_dirent_count * 32) % bytes_per_sector )
		return false;
	uint32_t sectors_per_fat =
		le16toh(bpb->sectors_per_fat) ? le16toh(bpb->sectors_per_fat) :
	    le32toh(bpb->fat32_sectors_per_fat);
	if ( !sectors_per_fat )
		return false;
	uint32_t total_sectors =
		bpb->total_sectors_low | bpb->total_sectors_high << 8;
	if ( !total_sectors )
		total_sectors = le32toh(bpb->total_sectors_large);
	if ( !total_sectors )
		return false;
	if ( blockdevice_size(bdev) / bytes_per_sector < total_sectors )
		return false;
	if ( bpb->fat_count < 1 )
		return false;
	uint16_t fat_lba = le16toh(bpb->reserved_sectors);
	if ( !fat_lba )
		return false;
	uint32_t fat_sectors;
	if ( __builtin_mul_overflow(bpb->fat_count, sectors_per_fat, &fat_sectors) )
		return false;
	uint32_t data_offset =
		le16toh(bpb->reserved_sectors) + fat_sectors + root_dir_sectors;
	if ( data_offset > total_sectors )
		return false;
	uint32_t data_sectors = total_sectors - data_offset;
	if ( bpb->sectors_per_cluster == 0 ||
	     (bpb->sectors_per_cluster & (bpb->sectors_per_cluster - 1)) )
		return false;
	uint32_t cluster_count = data_sectors / bpb->sectors_per_cluster;
	if ( cluster_count < 1 || 0xFFFFFF7 - 2 <= cluster_count )
		return false;
	uint8_t fat_type =
		cluster_count < 4085 ? 12 : cluster_count < 65525 ? 16 : 32;
	uint64_t fat_cluster_count =
		((uint64_t) sectors_per_fat * bytes_per_sector * 8) / fat_type;
	if ( fat_cluster_count < 2 + cluster_count )
		return false;
	if ( root_dirent_count < 1 && fat_type < 32 )
		return false;
	if ( fat_type == 32 && le16toh(bpb->fat32_version) != 0x0000 )
		return false;
	if ( fat_type == 32 &&
	     (le32toh(bpb->fat32_root_cluster) < 2 ||
	      2 + cluster_count < le32toh(bpb->fat32_root_cluster)) )
	if ( fat_type == 32 &&
	     (le16toh(bpb->fat32_fsinfo) < 1 ||
	      le16toh(bpb->reserved_sectors) <= le16toh(bpb->fat32_fsinfo)) )
		return false;
	priv->fat_type = fat_type;
	priv->bytes_per_sector = bytes_per_sector;
	priv->fat_lba = fat_lba;
	return true;
}

static bool could_be_fat_partition(struct blockdevice* bdev)
{
	struct partition* p = bdev->p;
	if ( p && p->table_type == PARTITION_TABLE_TYPE_GPT )
	{
		unsigned char bdp_guid[16];
		guid_from_string(bdp_guid, BDP_GPT_TYPE_GUID);
		unsigned char esp_guid[16];
		guid_from_string(esp_guid, ESP_GPT_TYPE_GUID);
		return memcmp(p->gpt_type_guid, bdp_guid, 16) == 0 ||
		       memcmp(p->gpt_type_guid, esp_guid, 16) == 0;
	}
	else if ( p && p->table_type == PARTITION_TABLE_TYPE_MBR )
		return p->mbr_system_id == 0x01 ||
		       p->mbr_system_id == 0x04 ||
		       p->mbr_system_id == 0x06 ||
		       p->mbr_system_id == 0x04 ||
		       p->mbr_system_id == 0x0C ||
		       p->mbr_system_id == 0x0E ||
		       p->mbr_system_id == 0xEF;
	else
		return true;
}

static bool fat_probe(struct blockdevice* bdev,
                      const unsigned char* leading,
                      size_t amount)
{
	if ( amount < sizeof(struct fat_bpb) )
		return false;
	// TODO: Restructure this whole guessing framework based on strongest
	//       evidence (uuids) to least strong evidence (probing).
	if ( !could_be_fat_partition(bdev) )
		return false;
	struct fat_private priv;
	return fat_determine(bdev, (const struct fat_bpb*) leading, &priv);
}

static bool fat_is_esp(struct blockdevice* bdev)
{
	struct partition* p = bdev->p;
	if ( p && p->table_type == PARTITION_TABLE_TYPE_GPT )
	{
		unsigned char esp_guid[16];
		guid_from_string(esp_guid, ESP_GPT_TYPE_GUID);
		return memcmp(p->gpt_type_guid, esp_guid, 16) == 0;
	}
	else if ( p && p->table_type == PARTITION_TABLE_TYPE_MBR )
		return p->mbr_system_id == 0xEF;
	return false;
}

static void fat_release(struct filesystem* fs)
{
	if ( !fs )
		return;
	free(fs);
}

static bool fat_needs_fsck(struct blockdevice* bdev, struct fat_private* priv)
{
	uint32_t entry;
	if ( !fat_read(bdev, priv, 1, &entry) )
		return true;
	if ( priv->fat_type == 12 )
		return ~entry & 0xC00;
	else if ( priv->fat_type == 16 )
		return ~entry & 0xC000;
	else if ( priv->fat_type == 32 )
		return ~entry & 0xC000000;
	__builtin_unreachable();
}

static enum filesystem_error fat_inspect(struct filesystem** fs_ptr,
                                         struct blockdevice* bdev)
{
	*fs_ptr = NULL;
	struct filesystem* fs = CALLOC_TYPE(struct filesystem);
	if ( !fs )
		return FILESYSTEM_ERROR_ERRNO;
	fs->bdev = bdev;
	fs->handler = &fat_handler;
	fs->handler_private = NULL;
	struct fat_bpb bpb;
	if ( blockdevice_preadall(bdev, &bpb, sizeof(bpb), 0) != sizeof(bpb) )
		return fat_release(fs), FILESYSTEM_ERROR_ERRNO;
	struct fat_private priv;
	if ( !fat_determine(bdev, &bpb, &priv) )
		return fat_release(fs), FILESYSTEM_ERROR_UNRECOGNIZED;
	fs->fstype_name = fat_is_esp(fs->bdev) ? "efi": "fat";
	fs->fsck = "fsck.fat";
	fs->driver = "fatfs";
	fs->flags |= FILESYSTEM_FLAG_WRITABLE;
	if ( fat_needs_fsck(bdev, &priv) )
		fs->flags |= FILESYSTEM_FLAG_FSCK_SHOULD |
		             FILESYSTEM_FLAG_FSCK_MUST;
	uint8_t* volume_id;
	uint8_t* volume_label;
	if ( priv.fat_type == 32 )
	{
		volume_id = bpb.fat32_volume_id;
		volume_label = bpb.fat32_volume_label;
	}
	else
	{
		volume_id = bpb.fat12_volume_id;
		volume_label = bpb.fat12_volume_label;
	}
	uint8_t uuid[16];
	memcpy(uuid + 0, volume_id, 4);
	memcpy(uuid + 4, volume_label, 11);
	uuid[15] = 0;
	char uuid_str[UUID_STRING_LENGTH + 1];
	uuid_to_string(uuid, uuid_str);
	char serial_str[sizeof("XXXX-XXXX")];
	snprintf(serial_str, sizeof(serial_str), "%02X%02X-%02X%02X",
	         volume_id[3], volume_id[2], volume_id[1], volume_id[0]);
	char label[12];
	memcpy(label, volume_label, 11);
	label[11] = '\0';
	size_t label_length = 11;
	while ( label_length && label[label_length - 1] == ' ' )
		label[--label_length] = '\0';
	char version[sizeof("FATXX")];
	snprintf(version, sizeof(version), "FAT%hhu", priv.fat_type);
	if ( !filesystem_add_identifier(fs, "TYPE", fs->fstype_name) ||
	     !filesystem_add_identifier(fs, "UUID", uuid_str) ||
	     !filesystem_add_identifier(fs, "SERIAL", serial_str) ||
	     !filesystem_add_identifier(fs, "LABEL", label) ||
	     !filesystem_add_identifier(fs, "VERSION", version) )
		return fat_release(fs), FILESYSTEM_ERROR_ERRNO;
	return *fs_ptr = fs, FILESYSTEM_ERROR_NONE;
}

const struct filesystem_handler fat_handler =
{
	.handler_name = "fat",
	.probe_amount = fat_probe_amount,
	.probe = fat_probe,
	.inspect = fat_inspect,
	.release = fat_release,
};
