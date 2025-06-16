/*
 * Copyright (c) 2013, 2014, 2015, 2023, 2025 Jonas 'Sortie' Termansen.
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
 * filesystem.cpp
 * Filesystem.
 */

#include <sys/stat.h>
#include <sys/types.h>

#include <assert.h>
#include <ctype.h>
#include <endian.h>
#include <err.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "fat.h"

#include "block.h"
#include "device.h"
#include "filesystem.h"
#include "inode.h"
#include "util.h"

static bool is_8_3_char(char c)
{
	return ('A' <= c && c <= 'Z') || ('0' <= c && c <= '9') ||
	       c == '$' || c == '%' || c == '\'' || c == '-' || c == '_' ||
	       c == '@' || c == '~' || c == '`' || c == '!' || c == '(' ||
	       c == ')' || c == '{' || c == '}' || c == '^' || c == '#' ||
	       c == '&';
}

bool is_8_3(const char* name)
{
	if ( !name[0] )
		return false;
	if ( !strcmp(name, ".") || !strcmp(name, "..") )
		return true;
	size_t b = 0;
	while ( name[b] && is_8_3_char(name[b]) )
		b++;
	if ( !b || 8 < b )
		return false;
	if ( !name[b] )
		return true;
	if ( name[b] != '.' )
		return false;
	size_t e = 0;
	while ( name[b+1+e] && is_8_3_char(name[b+1+e]) )
		e++;
	if ( !e || 3 < e )
		return false;
	if ( name[b+1+e] )
		return false;
	return true;
}

void encode_8_3(const char* decoded, char encoded[8 + 3])
{
	if ( !strcmp(decoded, ".") || !strcmp(decoded, "..") )
	{
		memset(encoded, ' ', 8 + 3);
		memcpy(encoded, decoded, strlen(decoded));
		return;
	}
	size_t i = 0;
	for ( size_t o = 0; o < 8 + 3; o++ )
	{
		unsigned char c = ' ';
		while ( o == 0 && (decoded[i] == '.' || decoded[i] == ' ') )
			i++;
		if ( o == 8 )
		{
			size_t last = strlen(decoded);
			for ( size_t j = i; decoded[j]; j++ )
				if ( decoded[j] == '.' )
					last = j + 1;
			i = last;
		}
		while ( decoded[i] == ' ' )
			i++;
		if ( decoded[i] && decoded[i] != '.' )
			c = (unsigned char) decoded[i++];
		c = toupper(c);
		if ( c != ' ' && !is_8_3_char((char) c) )
			c = '_';
		if ( c == 0xE5 )
			c = 0x05;
		encoded[o] = (char) c;
	}
	if ( encoded[0] == ' ' && encoded[8] == ' ' )
		encoded[0] = '_';
}

void decode_8_3(const char encoded[8 + 3], char decoded[8 + 1 + 3 + 1])
{
	size_t o = 0;
	for ( size_t i = 0; i < 8; i++ )
	{
		char c = encoded[i];
		if ( !c || c == ' ' )
			break;
		if ( c == 0x05 )
			c = (char) 0xE5;
		if ( 127 < (unsigned char) c )
			c = '_';
		decoded[o++] = c;
	}
	for ( size_t i = 8; i < 8 + 3; i++ )
	{
		char c = encoded[i];
		if ( !c || c == ' ' )
			break;
		if ( i == 8 )
			decoded[o++] = '.';
		if ( c == 0x05 )
			c = (char) 0xE5;
		if ( 127 < (unsigned char) c )
			c = '_';
		decoded[o++] = c;
	}
	decoded[o] = '\0';
}

static uint8_t tm_to_fat_centis(const struct tm* tm, long nsec)
{
	uint16_t hundreds = nsec / 10000000;
	if ( tm->tm_sec & 1 )
		hundreds += 100;
	return hundreds;
}

static uint16_t tm_to_fat_time(const struct tm* tm)
{
	return (tm->tm_sec / 2) << 0 | tm->tm_min << 5 | tm->tm_hour << 11;
}

static uint16_t tm_to_fat_date(const struct tm* tm)
{
	return tm->tm_mday << 0 | (tm->tm_mon + 1) << 5 | (tm->tm_year - 80) << 9;
}

void timespec_to_fat(const struct timespec* ts, uint16_t* date, uint16_t* time,
                     uint8_t* centis)
{
	struct tm tm;
	gmtime_r(&ts->tv_sec, &tm);
	*date = htole16(tm_to_fat_date(&tm));
	*time = htole16(tm_to_fat_time(&tm));
	*centis = tm_to_fat_centis(&tm, ts->tv_nsec);
}

struct timespec fat_to_timespec(const uint16_t* date_ptr,
                                const uint16_t* time_ptr,
                                const uint8_t* centis)
{
	struct tm tm;
	memset(&tm, 0, sizeof(tm));
	if ( time_ptr )
	{
		uint16_t time = le16toh(*time_ptr);
		tm.tm_sec = ((time >> 0) & 0x1F) * 2;
		tm.tm_min = (time >> 5) & 0x3F;
		tm.tm_hour = (time >> 11) & 0x1F;
	}
	if ( date_ptr )
	{
		uint16_t date = le16toh(*date_ptr);
		tm.tm_mday = (date >> 0) & 0x1F;
		tm.tm_mon = ((date >> 5) & 0xF) - 1;
		tm.tm_year = ((date >> 9) & 0x7F) + 80;
	}
	struct timespec ts;
	ts.tv_sec = mktime(&tm);
	ts.tv_nsec = centis ? (*centis % 100) * 10000000 : 0;
	return ts;
}

Filesystem::Filesystem(Device* device, const char* mount_path, Block* bpb_block)
{
	this->bpb_block = bpb_block;
	this->bpb = (struct fat_bpb*) bpb_block->block_data;
	this->device = device;
	this->mount_path = mount_path;
	this->mode_reg = S_IFREG | 0644;
	this->mode_dir = S_IFDIR | 0755;
	this->uid = 0;
	this->gid = 0;
	this->block_size = device->block_size;
	this->bytes_per_sector =
		bpb->bytes_per_sector_low | bpb->bytes_per_sector_high << 8;
	this->root_dirent_count =
		bpb->root_dirent_count_low | bpb->root_dirent_count_high << 8;
	uint32_t root_dir_sectors =
		divup<uint32_t>(root_dirent_count * sizeof(struct fat_dirent),
		                bytes_per_sector);
	this->sectors_per_fat =
		le16toh(bpb->sectors_per_fat) ? le16toh(bpb->sectors_per_fat) :
		le32toh(bpb->fat32_sectors_per_fat);
	this->total_sectors =
		bpb->total_sectors_low | bpb->total_sectors_high << 8;
	if ( !this->total_sectors )
		this->total_sectors = le32toh(bpb->total_sectors_large);
	this->fat_lba = le16toh(bpb->reserved_sectors);
	this->fat_count = bpb->fat_count;
	this->root_lba = fat_lba + fat_count * sectors_per_fat;
	this->data_lba = root_lba + root_dir_sectors;
	this->sectors_per_cluster = bpb->sectors_per_cluster;
	uint32_t data_lbas = total_sectors - data_lba;
	this->cluster_count = data_lbas / sectors_per_cluster;
	this->cluster_size = sectors_per_cluster * bytes_per_sector;
	this->fat_type = cluster_count < 4085 ? 12 : cluster_count < 65525 ? 16 : 32;
	// Use cluster 1 as the root inode on FAT12/FAT16 since it's not a valid
	// cluster for use in the FAT.
	this->root_inode_id = fat_type == 32 ? le32toh(bpb->fat32_root_cluster) : 1;
	this->eio_cluster =
		fat_type == 12 ? 0xFF7 : fat_type == 16 ? 0xFFF7 : 0xFFFFFF7;
	this->eof_cluster =
		fat_type == 12 ? 0xFFF : fat_type == 16 ? 0xFFFF : 0xFFFFFFF;
	this->free_count = 0xFFFFFFFF;
	this->free_search = 0;
	if ( fat_type == 32 )
	{
		Block* block = device->GetBlock(le16toh(bpb->fat32_fsinfo));
		struct fat_fsinfo* fsinfo =
			block ? (struct fat_fsinfo*) block->block_data : NULL;
		if ( fsinfo &&
		     le32toh(fsinfo->signature1) == FAT_FSINFO_SIGNATURE1 &&
		     le32toh(fsinfo->signature2) == FAT_FSINFO_SIGNATURE2 &&
		     le32toh(fsinfo->signature3) == FAT_FSINFO_SIGNATURE3 )
		{
			uint32_t next_free = le32toh(fsinfo->next_free);
			if ( 2 <= next_free && next_free - 2 < cluster_count )
				this->free_search = next_free - 2;
			uint32_t count_free = le32toh(fsinfo->free_count);
			if ( count_free < cluster_count )
				this->free_count = count_free;
		}
		if ( block )
			block->Unref();
	}
	this->mru_inode = NULL;
	this->lru_inode = NULL;
	this->dirty_inode = NULL;
	for ( size_t i = 0; i < INODE_HASH_LENGTH; i++ )
		this->hash_inodes[i] = NULL;
	this->root = NULL;
	this->dirty = false;
	this->request_check = false;
}

Filesystem::~Filesystem()
{
	Sync();
	Inode* inode = mru_inode;
	while ( inode )
	{
		Inode* next = mru_inode->next_inode;
		size_t remote_count = inode->remote_reference_count;
		for ( size_t i = 0; i < remote_count; i++ )
			inode->RemoteUnref();
		inode = next;
	}
	root->Unref();
	root = NULL;
	while ( mru_inode )
	{
		warn("leaked inode: %u", mru_inode->inode_id);
		delete mru_inode;
	}
	bpb_block->Unref();
}

bool Filesystem::WasUnmountedCleanly()
{
	if ( fat_type == 12 )
		return true;
	fat_ino_t flags = ReadFAT(1);
	fat_ino_t mask = fat_type == 16 ? 0xC000 : 0xC000000;
	return (flags & mask) == mask;
}

bool Filesystem::MarkMounted()
{
	fat_ino_t mounted = fat_type == 12 ? 0x7FF :
	                    fat_type == 16 ? 0x7FFF :
	                    0x7FFFFFF;
	if ( !WriteFAT(1, mounted) )
		return false;
	Sync();
	return true;
}

bool Filesystem::MarkUnmounted()
{
	if ( request_check )
		return false;
	fat_ino_t unmounted = fat_type == 12 ? 0xFFF :
	                      fat_type == 16 ? 0xFFFF :
	                      0xFFFFFFF;
	if ( !WriteFAT(1, unmounted) )
		return false;
	Sync();
	return true;
}

void Filesystem::RequestCheck()
{
	request_check = true;
}

void Filesystem::Corrupted()
{
	request_check = true;
	device->write = false;
	warn("filesystem may be corrupted, remounting read-only");
}

void Filesystem::BeginWrite()
{
	bpb_block->BeginWrite();
}

void Filesystem::FinishWrite()
{
	dirty = true;
	bpb_block->FinishWrite();
}

void Filesystem::Sync()
{
	while ( dirty_inode )
		dirty_inode->Sync();
	if ( dirty )
	{
		bpb_block->Sync();
		dirty = false;
	}
	device->Sync();
}

Inode* Filesystem::GetInode(fat_ino_t inode_id)
{
	size_t bin = inode_id % INODE_HASH_LENGTH;
	for ( Inode* iter = hash_inodes[bin]; iter; iter = iter->next_hashed )
		if ( iter->inode_id == inode_id )
			return iter->Refer(), iter;
	errno = ENOENT;
	return NULL;
}

Inode* Filesystem::CreateInode(fat_ino_t inode_id, Block* dirent_block,
                               struct fat_dirent* dirent, Inode* parent)
{
	Inode* inode = GetInode(inode_id);
	if ( inode )
		return inode;
	inode = new Inode(this, inode_id);
	if ( !inode )
		return (Inode*) NULL;
	inode->first_cluster =
		inode_id == root_inode_id && fat_type != 32 ? 0 : inode_id;
	if ( dirent || inode_id == root_inode_id )
	{
		if ( (inode->data_block = dirent_block) )
			inode->data_block->Refer();
		inode->dirent = dirent;
	}
	else
	{
		memset(&inode->deleted_dirent, 0, sizeof(inode->deleted_dirent));
		inode->dirent = &inode->deleted_dirent;
		inode->deleted = true;
	}
	if ( (inode->parent = parent) )
		inode->parent->Refer();
	inode->Prelink();

	return inode;
}

bool Filesystem::WriteInfo()
{
	if ( fat_type != 32 )
		return true;
	Block* block = device->GetBlock(le16toh(bpb->fat32_fsinfo));
	if ( !block )
		return false;
	struct fat_fsinfo* fsinfo = (struct fat_fsinfo*) block->block_data;
	if ( le32toh(fsinfo->free_count) != free_count ||
	     le32toh(fsinfo->next_free) != 2 + free_search )
	{
		block->BeginWrite();
		fsinfo->signature1 = htole32(FAT_FSINFO_SIGNATURE1);
		fsinfo->signature2 = htole32(FAT_FSINFO_SIGNATURE2);
		fsinfo->free_count = htole32(free_count);
		fsinfo->next_free = htole32(2 + free_search);
		fsinfo->signature3 = htole32(FAT_FSINFO_SIGNATURE3);
		block->FinishWrite();
	}
	block->Unref();
	return true;
}

fat_ino_t Filesystem::AllocateCluster()
{
	for ( size_t i = 0; i < cluster_count; i++ )
	{
		size_t n = 2 + (free_search + i) % cluster_count;
		if ( !ReadFAT(n) )
		{
			free_search = (i + 1) % cluster_count;
			WriteInfo();
			return n;
		}
	}
	return errno = ENOSPC, 0;
}

void Filesystem::FreeCluster(fat_ino_t cluster)
{
	if ( !free_count || free_search == cluster + 1 )
		free_search = cluster;
	if ( free_count < cluster_count )
		free_count++;
	WriteInfo();
}

fat_ino_t Filesystem::ReadFAT(fat_ino_t cluster)
{
	assert(cluster < 2 + cluster_count);
	if ( fat_type == 12 )
	{
		fat_block_t position = cluster + (cluster / 2);
		fat_block_t lba = position / bytes_per_sector;
		size_t offset = position % bytes_per_sector;
		Block* block = device->GetBlock(fat_lba + lba);
		if ( !block )
			return eio_cluster;
		uint8_t lower = block->block_data[offset];
		if ( ++offset == bytes_per_sector )
		{
			block->Unref();
			offset = 0;
			lba++;
			if ( !(block = device->GetBlock(fat_lba + lba)) )
				return eio_cluster;
		}
		uint8_t higher = block->block_data[offset];
		block->Unref();
		uint16_t value = lower | higher << 8;
		if ( cluster & 1 )
			return value >> 4;
		else
			return value & 0xFFF;
	}
	fat_block_t fat_size = fat_type / 8;
	fat_block_t position = (fat_block_t) cluster * (fat_block_t) fat_size;
	fat_block_t lba = position / bytes_per_sector;
	size_t entry = (position % bytes_per_sector) / fat_size;
	Block* block = device->GetBlock(fat_lba + lba);
	if ( !block )
		return eio_cluster;
	fat_ino_t result = 0;
	if ( fat_type == 16 )
		result = le16toh(((uint16_t*) block->block_data)[entry]);
	else if ( fat_type == 32 )
		result = le32toh(((uint32_t*) block->block_data)[entry]) & 0x0FFFFFFF;
	block->Unref();
	if ( result == eio_cluster )
		errno = EIO;
	return result;
}

bool Filesystem::WriteFAT(fat_ino_t cluster, fat_ino_t value)
{
	assert(device->write);
	assert(cluster < 2 + cluster_count);
	for ( uint8_t copy = 0; copy < fat_count; copy++ )
	{
		fat_block_t base_lba = fat_lba + copy * sectors_per_fat;
		if ( fat_type == 12 )
		{
			size_t position = cluster + (cluster / 2);
			size_t lba = position / bytes_per_sector;
			size_t offset = position % bytes_per_sector;
			Block* block = device->GetBlock(base_lba + lba);
			if ( !block )
				return false;
			fat_ino_t data = cluster & 1 ? value << 4 : value;
			uint16_t mask = cluster & 1 ? 0xFFF0 : 0x0FFF;
			block->BeginWrite();
			block->block_data[offset] &= ~mask;
			block->block_data[offset] |= data;
			if ( ++offset == bytes_per_sector )
			{
				block->FinishWrite();
				block->Unref();
				offset = 0;
				lba++;
				if ( !(block = device->GetBlock(base_lba + lba )) )
					return false;
				block->BeginWrite();
			}
			block->block_data[offset] &= ~(mask >> 8);
			block->block_data[offset] |= data >> 8;
			block->FinishWrite();
			block->Unref();
			continue;
		}
		size_t fat_size = fat_type / 8;
		size_t position = cluster * fat_size;
		size_t lba = position / bytes_per_sector;
		size_t entry = (position % bytes_per_sector) / fat_size;
		Block* block = device->GetBlock(base_lba + lba);
		if ( !block )
			return false;
		block->BeginWrite();
		if ( fat_type == 16 )
			((uint16_t*) block->block_data)[entry] = htole16(value);
		else if ( fat_type == 32 )
		{
			uint32_t old_raw = le32toh(((uint32_t*) block->block_data)[entry]);
			uint32_t old = old_raw & 0xF0000000;
			((uint32_t*) block->block_data)[entry] = htole32(value | old);
		}
		block->FinishWrite();
		block->Unref();
	}
	return true;
}

fat_ino_t Filesystem::CalculateFreeCount()
{
	if ( free_count != 0xFFFFFFFF )
		return free_count;
	size_t count = 0;
	for ( size_t i = 0; i < cluster_count; i++ )
		if ( !ReadFAT(2 + i) )
			count++;
	free_count = count;
	WriteInfo();
	return count;
}
