/*
 * Copyright (c) 2013, 2014, 2015, 2018, 2023, 2025 Jonas 'Sortie' Termansen.
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
 * inode.cpp
 * Filesystem inode.
 */

#define __STDC_CONSTANT_MACROS
#define __STDC_LIMIT_MACROS

#include <sys/stat.h>
#include <sys/types.h>

#include <assert.h>
#include <dirent.h>
#include <ctype.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uchar.h>

#include "fat.h"

#include "block.h"
#include "device.h"
#include "fatfs.h"
#include "filesystem.h"
#include "inode.h"
#include "util.h"

#ifndef S_SETABLE
#define S_SETABLE 02777
#endif
#ifndef O_WRITE
#define O_WRITE (O_WRONLY | O_RDWR)
#endif

Inode::Inode(Filesystem* filesystem, fat_ino_t inode_id)
{
	this->prev_inode = NULL;
	this->next_inode = NULL;
	this->prev_hashed = NULL;
	this->next_hashed = NULL;
	this->prev_dirty = NULL;
	this->next_dirty = NULL;
	this->parent = NULL;
	this->data_block = NULL;
	this->filesystem = filesystem;
	this->reference_count = 1;
	this->remote_reference_count = 0;
	this->implied_reference = 0;
	this->inode_id = inode_id;
	this->cached_cluster_id = UINT32_MAX;
	this->cached_cluster = UINT32_MAX;
	this->dirty = false;
	this->deleted = false;
}

Inode::~Inode()
{
	Sync();
	if ( data_block )
		data_block->Unref();
	if ( parent )
		parent->Unref();
	Unlink();
}

void Inode::Stat(struct stat* st)
{
	memset(st, 0, sizeof(*st));
	st->st_ino = inode_id;
	st->st_mode = Mode();
	st->st_nlink = 1;
	st->st_uid = UserId();
	st->st_gid = GroupId();
	st->st_size = Size();
	if ( dirent )
	{
		uint8_t* m_centis =
			le16toh(dirent->creation_date) == le16toh(dirent->modified_date) &&
			le16toh(dirent->creation_time) == le16toh(dirent->modified_time) ?
			&dirent->creation_centis : NULL;
		st->st_mtim = fat_to_timespec(&dirent->modified_date,
		                              &dirent->modified_time, m_centis);
		st->st_ctim = st->st_mtim;
		st->st_atim =
			le16toh(dirent->access_date) == le16toh(dirent->modified_date) ?
			st->st_mtim :
			fat_to_timespec(&dirent->access_date, NULL, NULL);
	}
	st->st_blksize = filesystem->bytes_per_sector *
	                 filesystem->sectors_per_cluster;
	st->st_blocks = st->st_size / 512;
}

mode_t Inode::Mode()
{
	if ( inode_id == filesystem->root_inode_id )
		return filesystem->mode_dir;
	mode_t mode = dirent->attributes & FAT_ATTRIBUTE_DIRECTORY ?
	              filesystem->mode_dir : filesystem->mode_reg;
	if ( dirent->attributes & FAT_ATTRIBUTE_READ_ONLY )
		mode &= ~0222;
	return mode;
}

bool Inode::ChangeMode(mode_t mode)
{
	assert(filesystem->device->write);
	if ( inode_id == filesystem->root_inode_id )
		return errno = EPERM, false;
	mode_t base_mode = (dirent->attributes & FAT_ATTRIBUTE_DIRECTORY ?
	                    filesystem->mode_dir : filesystem->mode_reg) & 0777;
	uint8_t new_attributes = dirent->attributes;
	if ( mode == (base_mode & ~0222) )
		new_attributes |= FAT_ATTRIBUTE_READ_ONLY;
	else if ( mode == (base_mode | (base_mode & 0222)) )
		new_attributes &= ~FAT_ATTRIBUTE_READ_ONLY;
	else
		return errno = EPERM, false;
	if ( new_attributes == dirent->attributes )
		return true;
	if ( data_block )
		data_block->BeginWrite();
	dirent->attributes = new_attributes;
	if ( data_block )
		data_block->FinishWrite();
	return true;
}

uid_t Inode::UserId()
{
	return filesystem->uid;
}

bool Inode::ChangeOwner(uid_t uid, gid_t gid)
{
	assert(filesystem->device->write);
	if ( inode_id == filesystem->root_inode_id )
		return errno = EPERM, false;
	if ( (uid != (uid_t) -1 && uid != filesystem->uid) ||
	     (gid != (gid_t) -1 && gid != filesystem->gid) )
		return errno = EPERM, false;
	return true;
}

gid_t Inode::GroupId()
{
	return filesystem->gid;
}

void Inode::UTimens(const struct timespec times[2])
{
	if ( inode_id == filesystem->root_inode_id )
		return;
	if ( times[0].tv_nsec != UTIME_OMIT ||
	     times[1].tv_nsec != UTIME_OMIT )
	{
		struct timespec now;
		clock_gettime(CLOCK_REALTIME, &now);
		uint8_t tenths;
		uint16_t time;
		BeginWrite();
		if ( times[0].tv_nsec == UTIME_NOW )
			timespec_to_fat(&now, &dirent->access_date, &time, &tenths);
		else if ( times[0].tv_nsec != UTIME_OMIT )
			timespec_to_fat(&times[0], &dirent->access_date, &time, &tenths);
		if ( times[1].tv_nsec == UTIME_NOW )
			timespec_to_fat(&now, &dirent->modified_date,
			                &dirent->modified_time, &tenths);
		else if ( times[1].tv_nsec != UTIME_OMIT )
			timespec_to_fat(&times[1], &dirent->modified_date,
			                &dirent->modified_time, &tenths);
		FinishWrite();
	}
}

fat_off_t Inode::Size()
{
	if ( inode_id == filesystem->root_inode_id )
		return 0;
	if ( dirent->attributes & FAT_ATTRIBUTE_DIRECTORY )
		return 0;
	return le32toh(dirent->size);
}

fat_block_t Inode::GetBlockId(fat_ino_t cluster, uint8_t sector)
{
	if ( inode_id == filesystem->root_inode_id && filesystem->fat_type != 32 )
		return filesystem->root_lba + cluster;
	return filesystem->data_lba +
	       (cluster - 2) * filesystem->sectors_per_cluster +
	       sector;
}

Block* Inode::GetClusterSector(fat_ino_t cluster, uint8_t sector)
{
	fat_block_t block_id = GetBlockId(cluster, sector);
	return filesystem->device->GetBlock(block_id);
}

bool Inode::ZeroCluster(fat_ino_t cluster)
{
	for ( unsigned int i = 0; i < filesystem->sectors_per_cluster; i++ )
	{
		fat_off_t block_id = GetBlockId(cluster, i);
		Block* block = filesystem->device->GetBlockZeroed(block_id);
		if ( !block )
			return false;
		block->Unref();
	}
	return true;
}

bool Inode::Iterate(Block** block_ptr, struct position* position)
{
	if ( position->offset == filesystem->bytes_per_sector )
	{
		position->offset = 0;
		if ( inode_id == filesystem->root_inode_id &&
		     filesystem->fat_type != 32 )
		{
			fat_block_t end = filesystem->root_dirent_count *
			                  sizeof(struct fat_dirent);
			fat_block_t end_lba = end / filesystem->bytes_per_sector;
			position->cluster++;
			position->sector = 0;
			if ( end_lba <= position->cluster )
				return errno = 0, false;
		}
		else
		{
			position->sector++;
			if ( position->sector == filesystem->sectors_per_cluster )
			{
				position->sector = 0;
				position->cluster = filesystem->ReadFAT(position->cluster);
			}
		}
	}
	if ( inode_id != filesystem->root_inode_id || filesystem->fat_type == 32 )
	{
		if ( position->cluster < 2 ||
		     filesystem->eio_cluster == position->cluster )
			return errno = EIO, false;
		if ( filesystem->eio_cluster < position->cluster )
			return errno = 0, false;
	}
	fat_block_t block_id = GetBlockId(position->cluster, position->sector);
	if ( *block_ptr )
	{
		if ( (*block_ptr)->block_id == block_id )
			return true;
		(*block_ptr)->Unref();
	}
	if ( !(*block_ptr = filesystem->device->GetBlock(block_id)) )
		return false;
	return true;
}

fat_ino_t Inode::SeekCluster(fat_off_t cluster_id)
{
	fat_ino_t cluster = first_cluster;
	fat_ino_t left = cluster_id;
	if ( cached_cluster_id != UINT32_MAX && cached_cluster_id <= left )
	{
		left -= cached_cluster_id;
		cluster = cached_cluster;
	}
	while ( left-- )
	{
		cluster = filesystem->ReadFAT(cluster);
		if ( cluster < 2 || filesystem->eio_cluster == cluster )
			return errno = EIO, filesystem->eio_cluster;
		if ( filesystem->eio_cluster < cluster )
			return errno = EIO, filesystem->eio_cluster;
	}
	if ( 0 < cluster_id )
	{
		cached_cluster_id = cluster_id;
		cached_cluster = cluster;
	}
	return cluster;
}

bool Inode::FreeClusters(fat_ino_t cluster)
{
	fat_ino_t marker = filesystem->eof_cluster;
	while ( true )
	{
		fat_ino_t next_cluster = filesystem->ReadFAT(cluster);
		if ( next_cluster < 2 || filesystem->eio_cluster == next_cluster )
			return errno = EIO, false;
		if ( next_cluster != marker )
		{
			if ( !filesystem->WriteFAT(cluster, marker) )
				return filesystem->Corrupted(), errno = EIO, false;
			filesystem->FreeCluster(next_cluster);
		}
		if ( filesystem->eof_cluster <= next_cluster )
			break;
		cluster = next_cluster;
		marker = 0;
	}
	return true;
}

bool Inode::Truncate(uint64_t new_size_64)
{
	assert(filesystem->device->write);
	assert(S_ISREG(Mode()));
	fat_off_t new_size = (fat_off_t) new_size_64;
	if ( new_size_64 != new_size )
		return errno = E2BIG, false;
	fat_off_t old_size = le32toh(dirent->size);
	fat_off_t pos = old_size < new_size ? old_size : new_size;
	fat_off_t bytes_per_sector = filesystem->bytes_per_sector;
	fat_off_t cluster_id = pos / filesystem->cluster_size;
	fat_off_t cluster_offset = pos % filesystem->cluster_size;
	if ( cluster_id && !cluster_offset )
	{
		cluster_id--;
		cluster_offset = filesystem->cluster_size;
	}
	fat_ino_t cluster = SeekCluster(cluster_id);
	if ( cluster_id == filesystem->eio_cluster )
		return errno = EIO, false;
	if ( old_size < new_size )
	{
		while ( old_size < new_size )
		{
			if ( cluster_offset == filesystem->cluster_size )
			{
				fat_ino_t next_cluster = filesystem->AllocateCluster();
				if ( !next_cluster )
					return false;
				if ( !ZeroCluster(next_cluster) )
					return filesystem->FreeCluster(next_cluster), false;
				if ( !filesystem->WriteFAT(next_cluster,
				                           filesystem->eof_cluster) ||
				     !filesystem->WriteFAT(cluster, next_cluster) )
					return filesystem->Corrupted(), errno = EIO, false;
				cluster_offset = 0;
				cluster = next_cluster;
			}
			uint8_t sector = cluster_offset / bytes_per_sector;
			uint16_t sector_offset = cluster_offset % bytes_per_sector;
			Block* block = GetClusterSector(cluster, sector);
			if ( !block )
				return false;
			size_t left = new_size - old_size;
			size_t available = bytes_per_sector - sector_offset;
			size_t amount = left < available ? left : available;
			block->BeginWrite();
			memset(block->block_data + sector_offset, 0, amount);
			block->FinishWrite();
			old_size += amount;
			cluster_offset += amount;
			block->Unref();
		}
	}
	else if ( new_size < old_size )
	{
		cached_cluster_id = UINT32_MAX;
		cached_cluster = 0;
		if ( !FreeClusters(cluster) )
			return false;
	}
	else
		return true;
	if ( data_block )
		data_block->BeginWrite();
	dirent->size = htole32(new_size);
	if ( data_block )
		data_block->FinishWrite();
	Modified();
	return true;
}

static unsigned char ChecksumName(const char name[11])
{
	unsigned char checksum = 0;
	for ( size_t i = 0; i < 11; i++ )
	{
		checksum = (checksum & 0x01 ? 0x80 : 0x00) | checksum >> 1;
		checksum += (unsigned char) name[i];
	}
	return checksum;
}

static bool DecodeUTF16(const char16_t* in, char* out, size_t out_size)
{
	mbstate_t ps;
	memset(&ps, 0, sizeof(ps));
	char mb[MB_CUR_MAX];
	for ( size_t i = 0, o = 0; true; i++ )
	{
		size_t amount = c16rtomb(mb, in[i], &ps);
		if ( amount == (size_t) -1 )
			return false;
		if ( out_size - o < amount )
			return errno = EILSEQ, false;
		memcpy(out + o, mb, amount);
		o += amount;
		if ( !in[i] )
			return true;
	}
}

static bool EncodeUTF16(const char* in, char16_t* out, size_t out_size)
{
	size_t in_size = strlen(in) + 1;
	mbstate_t ps;
	memset(&ps, 0, sizeof(ps));
	for ( size_t i = 0, o = 0; true; o++ )
	{
		if ( o == out_size )
			return errno = ENAMETOOLONG, false;
		size_t amount = mbrtoc16(&out[o], in + i, in_size - i, &ps);
		if ( amount == (size_t) -1 )
			return false;
		if ( amount == (size_t) -2 )
			return errno = EILSEQ, false;
		if ( amount != (size_t) -3 )
			i += amount;
		if ( !amount )
			return true;
	}
}

bool Inode::ReadDirectory(Block** block_inout,
                          struct position* next_position_inout, char* name,
                          uint8_t* file_type_out, fat_ino_t* inode_id_out,
                          struct fat_dirent** entry_out,
                          struct free_search* free_search,
                          struct position* position_out,
                          size_t* entry_length_out)
{
	// Manually provide . and .. entries for the root directory.
	if ( inode_id == filesystem->root_inode_id &&
	     next_position_inout->cluster == first_cluster && !*block_inout )
	{
		if ( next_position_inout->offset < 2  )
		{
			const char* entry = next_position_inout->offset ? ".." : ".";
			if ( name )
				strcpy(name, entry);
			*file_type_out = DT_DIR;
			*inode_id_out = filesystem->root_inode_id;
			next_position_inout->offset++;
			*entry_out = NULL;
			return true;
		}
		next_position_inout->offset = 0;
	}
	bool has_long = false;
	bool has_long_name = false;
	unsigned char long_checksum = 0;
	char16_t long_name[20 * 13 + 1] = {0};
	unsigned char ord_next = 0;
	unsigned char long_in_streak = 0;
	struct position entry_position = *next_position_inout;
	size_t entry_length = 0;
	// Read directory entries until we have a full record.
	while ( Iterate(block_inout, next_position_inout) )
	{
		// Assume this directory record is free space until proven otherwise.
		struct position position = *next_position_inout;
		if ( free_search )
			free_search->last_cluster = position.cluster;
		uint8_t* block_data =
			(*block_inout)->block_data + position.offset;
		struct fat_dirent* entry = (struct fat_dirent*) block_data;
		// Keep track of whether this entry was part of a free space streak.
		bool in_streak = false;
		if ( free_search && free_search->streak < free_search->needed )
		{
			if ( !free_search->streak )
				free_search->position = position;
			free_search->streak++;
			in_streak = true;
		}
		// Stop if we hit the last directory record, and we have found enough
		// free space.
		if ( !entry->name[0] &&
		     (!free_search || free_search->streak == free_search->needed) )
		{
			errno = 0;
			break;
		}
		next_position_inout->offset += sizeof(struct fat_dirent);
		// Continue to the next record if a free entry is found.
		if ( !entry->name[0] || (unsigned char) entry->name[0] == 0xE5 )
		{
			// Invalidate any orphaned long entries as minor corruption.
			if ( has_long )
			{
				has_long = false;
				has_long_name = false;
				long_in_streak = 0;
				filesystem->RequestCheck();
			}
			continue;
		}
		// Parse a long directory entry.
		if ( (entry->attributes & FAT_ATTRIBUTE_LONG_NAME_MASK) ==
		     FAT_ATTRIBUTE_LONG_NAME )
		{
			// Keep track of how many long records were part of the free streak,
			// so we can subtract them from the streak if they are valid.
			if ( in_streak )
				long_in_streak++;
			// No need to pay attention to the long record if the caller doesn't
			// care about the file name or free space.
			if ( !name && !free_search )
				continue;
			// Invalidate the orphraned long entries if the checksum changes.
			if ( has_long && entry->checksum != long_checksum )
			{
				has_long = false;
				has_long_name = false;
				long_in_streak = 0;
				filesystem->RequestCheck();
			}
			// Begin a new long directory record if needed.
			if ( !has_long )
			{
				has_long = true;
				long_checksum = entry->checksum;
				entry_position = position;
				entry_length = 0;
			}
			entry_length++;
			// Ignore non-name long directory entries.
			if ( entry->type != 0 )
				continue;
			unsigned char ord = entry->ord & FAT_LONG_NAME_ORD_MASK;
			// Verify the long directory entry is in the right sequence.
			if ( (has_long_name && ord != ord_next) || ord == 0 || 20 < ord ||
			     (!has_long_name && !(entry->ord & FAT_LONG_NAME_LAST)) )
			{
				has_long = false;
				has_long_name = false;
				long_in_streak = 0;
				filesystem->RequestCheck();
				continue;
			}
			if ( entry->ord & FAT_LONG_NAME_LAST )
			{
				memset(long_name, 0, sizeof(long_name));
				has_long_name = true;
			}
			ord_next = ord - 1;
			if ( !name )
				continue;
			// Read the next fragment of the UTF-16 file name.
			char16_t* buf = long_name + (ord - 1) * 13;
			for ( size_t i = 0; i < 5; i++ )
				buf[i] = ((unsigned char) entry->name1[2 * i + 0]) << 0 |
				         ((unsigned char) entry->name1[2 * i + 1]) << 8;
			for ( size_t i = 0; i < 6; i++ )
				buf[5+i] = ((unsigned char) entry->name2[2 * i + 0]) << 0 |
				           ((unsigned char) entry->name2[2 * i + 1]) << 8;
			for ( size_t i = 0; i < 2; i++ )
				buf[11+i] = ((unsigned char) entry->name3[2 * i + 0]) << 0 |
				            ((unsigned char) entry->name3[2 * i + 1]) << 8;
			continue;
		}
		// This is a valid directory entry, so stop the free streak if this
		// entry is part of it.
		if ( in_streak )
		{
			free_search->streak = 0;
			long_in_streak = 0;
		}
		// Ignore the volume label hidden as a file.
		if ( entry->attributes & FAT_ATTRIBUTE_VOLUME_ID )
		{
			has_long = false;
			has_long_name = false;
			continue;
		}
		// If there was no long data, then the directory record begins here.
		if ( !has_long )
		{
			entry_position = position;
			entry_length = 0;
		}
		entry_length++;
		// Provide the name if requested.
		if ( name )
		{
			bool good_long = has_long;
			// The long data is wrong if the checksum doesn't match.
			if ( has_long &&
			     ChecksumName(entry->name) != long_checksum )
				good_long = false;
			// The long data cannot exceed 255 UTF-16 code units.
			size_t length = 0;
			if ( good_long && has_long_name )
			{
				while ( long_name[length] != 0 && long_name[length] != 0xFFFF )
					length++;
				if ( FAT_UTF16_NAME_MAX < length )
					good_long = false;
			}
			// The long data must decode as UTF-16.
			if ( good_long && has_long_name )
			{
				long_name[length] = L'\0';
				size_t name_max = FAT_UTF8_NAME_MAX + 1;
				good_long = DecodeUTF16(long_name, name, name_max);
			}
			// Remove the long data from the free streak if it was valid.
			if ( good_long && free_search )
				free_search->streak -= long_in_streak;
			// Request a filesystem check if the long name was invalid.
			if ( has_long_name && !good_long )
				filesystem->RequestCheck();
			// Use the short name if there wasn't a valid long name.
			if ( !good_long || !has_long_name )
			{
				char case_name[8+3];
				memcpy(case_name, entry->name, sizeof(case_name));
				// Lowercase the short name per the special reserved bits for
				// interoperability. This is not officially part of the FAT
				// filesystem, and this implementation won't make such entries,
				// but is interoperable if a proper lowercase long name is not
				// created because these bits are used instead.
				if ( (entry->reserved & FAT_RESERVED_LOWER_NAME) )
					for ( size_t i = 0; i < 8; i++ )
						case_name[i] = tolower((unsigned char) case_name[i]);
				if ( (entry->reserved & FAT_RESERVED_LOWER_EXT) )
					for ( size_t i = 8; i < 8+3; i++ )
						case_name[i] = tolower((unsigned char) case_name[i]);
				decode_8_3(case_name, name);
			}
		}
		// FAT is poorly designed and does not have a permanent inode number
		// concept associated with files, which is essential to Unix semantics.
		// The first cluster of a file is used as the inode number, since it
		// won't change in this driver while the filesystem is mounted. However,
		// zero length files are not supposed to have a first cluster (ugh), so
		// if we encounter such a file, allocate it a cluster so it has an inode
		// number. However, fsck.fat doesn't like this behavior and wants to
		// undo it. Hopefully we don't run out of disk space here. I hate this.
		fat_ino_t inode_id = le16toh(entry->cluster_low) |
		                     le16toh(entry->cluster_high) << 16;
		unsigned char file_type = entry->attributes & FAT_ATTRIBUTE_DIRECTORY ?
		                          DT_DIR : DT_REG;
		if ( file_type == DT_REG && !entry->size && !inode_id )
		{
			if ( filesystem->device->write )
			{
				fat_ino_t new_inode_id = filesystem->AllocateCluster();
				if ( !new_inode_id )
					return false;
				if ( !ZeroCluster(new_inode_id) )
					return filesystem->FreeCluster(new_inode_id), false;
				if ( !filesystem->WriteFAT(new_inode_id,
				                           filesystem->eof_cluster) )
					return filesystem->Corrupted(), errno = EIO, false;
				(*block_inout)->BeginWrite();
				entry->cluster_high = htole16(new_inode_id >> 16);
				entry->cluster_low = htole16(new_inode_id & 0xFFFF);
				(*block_inout)->FinishWrite();
				inode_id = new_inode_id;
			}
			// If mounted read-only, use the offset to the directory entry as
			// the inode number, and add the cluster count onto it, so it
			// doesn't conflict. Hopefully the filesystem isn't large enough to
			// overflow the 32-bit inode number.
			else
			{
				uint32_t entries_per_cluster = filesystem->cluster_size / 32;
				uint16_t entries_per_sector = filesystem->bytes_per_sector / 32;
				uint32_t offset_id = entry_position.offset / 32;
				uint32_t sector_id = entry_position.sector * entries_per_sector;
				uint32_t entry_id = offset_id + sector_id; // 19 bit at worst
				uint32_t cluster_id;
				if ( __builtin_mul_overflow(entry_position.cluster,
				                            entries_per_cluster, &cluster_id) ||
				     __builtin_add_overflow(entry_id, cluster_id, &entry_id) ||
				     __builtin_add_overflow(filesystem->cluster_count + 2,
				                            entry_id, &inode_id) )
					return errno = EIO, false;
			}
		}
		// Provide the directory entry data to the caller.
		*file_type_out = file_type;
		*inode_id_out = inode_id;
		*entry_out = entry;
		if ( position_out )
			*position_out = entry_position;
		if ( entry_length_out )
			*entry_length_out = entry_length;
		return true;
	}
	return false;
}

Inode* Inode::Open(const char* elem, int flags, mode_t mode)
{
	if ( !S_ISDIR(Mode()) )
		return errno = ENOTDIR, (Inode*) NULL;
	if ( deleted )
		return errno = ENOENT, (Inode*) NULL;
	size_t elem_length = strlen(elem);
	if ( elem_length == 0 )
		return errno = ENOENT, (Inode*) NULL;
	struct position position;
	position.cluster = first_cluster;
	position.sector = 0;
	position.offset = 0;
	Block* block = NULL;
	char name[FAT_UTF8_NAME_MAX + 1];
	uint8_t file_type;
	fat_ino_t child_inode_id;
	struct fat_dirent* entry;
	while ( ReadDirectory(&block, &position, name, &file_type, &child_inode_id,
	                      &entry, NULL, NULL, NULL) )
	{
		if ( strcmp(elem, name) != 0 )
			continue;
		if ( (flags & O_CREAT) && (flags & O_EXCL) )
			errno = EEXIST;
		else if ( (flags & O_DIRECTORY) && file_type != DT_DIR )
			errno = ENOTDIR;
		else
		{
			Inode* inode;
			if ( !strcmp(name, ".") )
			{
				inode = this;
				inode->Refer();
			}
			else if ( !strcmp(name, "..") )
			{
				inode = inode_id == filesystem->root_inode_id ? this : parent;
				inode->Refer();
			}
			else
				inode = filesystem->CreateInode(child_inode_id, block, entry,
				                                this);
			if ( block )
				block->Unref();
			if ( !inode )
				return (Inode*) NULL;
			if ( flags & O_WRITE && !filesystem->device->write )
				return inode->Unref(), errno = EROFS, (Inode*) NULL;
			if ( S_ISREG(inode->Mode()) && (flags & O_WRITE) &&
				(flags & O_TRUNC) && !inode->Truncate(0) )
				return (Inode*) NULL;
			return inode;
		}
		break;
	}
	if ( block )
		block->Unref();
	if ( errno )
		return (Inode*) NULL;
	if ( flags & O_CREAT )
	{
		if ( !strcmp(elem, ".") || !strcmp(elem, "..") )
			return errno = ENOENT, (Inode*) NULL;
		if ( !filesystem->device->write )
			return errno = EROFS, (Inode*) NULL;
		// Allocating a first cluster for an empty file is officially wrong but
		// essential to using it as the permanent inode id for the file.
		fat_ino_t new_inode_id = filesystem->AllocateCluster();
		if ( !new_inode_id )
			return (Inode*) NULL;
		Inode* inode = filesystem->CreateInode(new_inode_id, NULL, NULL, NULL);
		if ( !inode )
			return filesystem->FreeCluster(new_inode_id), (Inode*) NULL;
		if ( !inode->ZeroCluster(new_inode_id) )
			return inode->Unref(), filesystem->FreeCluster(new_inode_id),
			       (Inode*) NULL;
		struct timespec now;
		clock_gettime(CLOCK_REALTIME, &now);
		mode_t attributes = (mode & 0200 ? 0 : FAT_ATTRIBUTE_READ_ONLY) |
		                    (S_ISDIR(mode) ? FAT_ATTRIBUTE_DIRECTORY : 0);
		inode->dirent->attributes = attributes;
		inode->dirent->cluster_high = htole16(new_inode_id >> 16);
		inode->dirent->cluster_low = htole16(new_inode_id & 0xFFFF);
		timespec_to_fat(&now, &inode->dirent->creation_date,
		                &inode->dirent->creation_time,
		                &inode->dirent->creation_centis);
		inode->dirent->access_date = inode->dirent->creation_date;
		inode->dirent->cluster_high = htole16(new_inode_id >> 16);
		inode->dirent->modified_time = inode->dirent->creation_time;
		inode->dirent->modified_date = inode->dirent->creation_date;
		inode->dirent->cluster_low = htole16(new_inode_id & 0xFFFF);
		inode->dirent->size = htole32(0);
		if ( S_ISDIR(mode) )
		{
			Block* block = inode->GetClusterSector(new_inode_id, 0);
			if ( !block )
				return inode->Unref(), filesystem->FreeCluster(new_inode_id),
			    (Inode*) NULL;
			block->BeginWrite();
			memset(block->block_data, 0, filesystem->bytes_per_sector);
			struct fat_dirent* entry = (struct fat_dirent*) block->block_data;
			memcpy(entry, inode->dirent, sizeof(*entry));
			memcpy(entry->name, ".          ", 11);
			entry->attributes = attributes;
			entry++;
			memcpy(entry->name, "..         ", 11);
			entry->attributes = FAT_ATTRIBUTE_DIRECTORY;
			if ( this->inode_id == filesystem->root_inode_id )
			{
				entry->cluster_high = htole16(0);
				entry->cluster_low = htole16(0);
			}
			else
			{
				entry->cluster_high = htole16(this->inode_id >> 16);
				entry->cluster_low = htole16(this->inode_id & 0xFFFF);
			}
			block->FinishWrite();
			block->Unref();
		}
		// TODO: This whole AllocateCluster/FreeCluster pattern is quite sketchy
		//       and doesn't actually write to the FAT.
		if ( !filesystem->WriteFAT(new_inode_id, filesystem->eof_cluster) )
			return filesystem->Corrupted(), errno = EIO,
			       inode->Unref(), (Inode*) NULL;
		if ( !Link(elem, inode, S_ISDIR(mode)) )
		{
			inode->Unref();
			if ( !filesystem->WriteFAT(new_inode_id, 0) )
				return filesystem->Corrupted(), errno = EIO, (Inode*) NULL;
			filesystem->FreeCluster(new_inode_id);
			return NULL;
		}
		return inode;
	}
	return errno = ENOENT, (Inode*) NULL;
}

static bool DoesShortNameConflict(char sample[8 + 3], char short_name[8 + 3])
{
	// No conflict if the file extension is different.
	for ( size_t i = 0; i < 3; i++ )
		if ( sample[8 + i] != short_name[8 + i] )
			return false;
	// Measure the shared prefix length.
	size_t length = 8;
	if ( length && sample[length - 1] == ' ' )
		length--;
	size_t prefix = 0;
	while ( prefix < 8 && sample[prefix] == short_name[prefix] &&
	        sample[prefix] != '~' )
		prefix++;
	// Conflict if the short names are identical.
	if ( prefix == 8 )
		return true;
	// No conflict if the short name isn't in the ~ numeric syntax.
	if ( short_name[prefix] != '~' )
		return false;
	// Measure the amount of digits after the ~.
	size_t digits = 0;
	while ( prefix + 1 + digits < 8 &&
	        '0' <= short_name[prefix + 1 + digits] &&
	        short_name[prefix + 1 + digits] <= '9' )
		digits++;
	// No conflict if there are no digits after the ~.
	if ( !digits )
		return false;
	// Allow trailing spaces in the name.
	size_t spaces = 0;
	while ( prefix + 1 + digits + spaces < 8 &&
	        short_name[prefix + 1 + digits + spaces] == ' ' )
		spaces++;
	// No conflict if the short name isn't in the right format.
	if ( prefix + 1 + digits + spaces != 8 )
		return false;
	return !spaces || prefix == length;
}

static uint32_t GetShortNameNumber(char name[8 + 3])
{
	// Skip trailing spaces.
	size_t length = 8;
	if ( length && name[length - 1] == ' ' )
		length--;
	// Count trailing digits.
	size_t offset = length;
	while ( offset && '0' <= name[offset  - 1] && name[offset  - 1] <= '9' )
		offset--;
	if ( !offset || name[offset - 1] != '~' )
		return 0;
	// Parse the digits into the short name number.
	uint32_t result = 0;
	while ( offset < 8 && '0' <= name[offset] && name[offset] <= '9' )
		result = result * 10 + name[offset++] - '0';
	return result;
}

bool Inode::Link(const char* elem, Inode* dest, bool directories)
{
	if ( !S_ISDIR(Mode()) )
		return errno = ENOTDIR, false;
	if ( deleted )
		return errno = ENOENT, false;
	if ( directories && !S_ISDIR(dest->Mode()) )
		return errno = ENOTDIR, false;
	if ( !directories && S_ISDIR(dest->Mode()) )
		return errno = EISDIR, false;
	if ( !filesystem->device->write )
		return errno = EROFS, false;
	size_t elem_length = strlen(elem);
	if ( elem_length == 0 )
		return errno = ENOENT, false;
	// Assume a long file name is required until no conflicts have been found.
	char short_name[8 + 3];
	char16_t new_long_name[FAT_UTF8_NAME_MAX_BUF + 1] = u"";
	size_t new_long_length = 0;
	if ( !EncodeUTF16(elem, new_long_name, FAT_UTF16_NAME_MAX + 1) )
		return false;
	while ( new_long_name[new_long_length] != L'\0' )
		new_long_length++;
	for ( size_t i = new_long_length + 1; i < FAT_UTF8_NAME_MAX_BUF; i++ )
		new_long_name[i] = 0xFFFF;
	size_t needed_entries = (new_long_length + 12) / 13 + 1;
	encode_8_3(elem, short_name);
	char decoded[8 + 1 + 3 + 1];
	decode_8_3(short_name, decoded);
	// The Windows FAT driver is limited to 64 K directory entries, although
	// this driver doesn't have that limitation, it seems like a reasonable top
	// limit on the number of numeric short names.
	#ifdef __is_sortix_kernel
	#error This array will overflow the kernel stack if you move the code there
	#endif
	uint8_t conflicts[UINT16_MAX / 8];
	memset(conflicts, 0, sizeof(conflicts));
	if ( strcasecmp(elem, decoded) != 0 )
		setbit(conflicts, 0);
	struct position position;
	position.cluster = first_cluster;
	position.sector = 0;
	position.offset = 0;
	Block* block = NULL;
	char name[FAT_UTF8_NAME_MAX + 1];
	uint8_t file_type;
	fat_ino_t child_inode_id;
	struct fat_dirent* entry;
	// Search for free space in the directory to store the new directory entry.
	struct free_search free_search;
	memset(&free_search, 0, sizeof(free_search));
	free_search.needed = needed_entries;
	while ( ReadDirectory(&block, &position, name, &file_type, &child_inode_id,
	                      &entry, &free_search, NULL, NULL) )
	{
		if ( !entry ) // Root directory . and .. are not important here.
			continue;
		char entry_decoded[8 + 1 + 3 + 1];
		decode_8_3(entry->name, entry_decoded);
		// Detect if the short name has already been used.
		if ( !strcmp(decoded, entry_decoded) )
			setbit(conflicts, 0);
		// Detect which other numeric short names have been used.
		if ( DoesShortNameConflict(short_name, entry->name) )
		{
			uint32_t number = GetShortNameNumber(entry->name);
			if ( number < UINT16_MAX )
				setbit(conflicts, number);
		}
		if ( !strcmp(elem, name) )
			return block->Unref(), errno = EEXIST, false;
	}
	if ( block )
		block->Unref();
	block = NULL;
	if ( errno )
		return false;
	// Assign an unused numeric short name.
	uint32_t number = 0;
	while ( number < UINT16_MAX && checkbit(conflicts, number) )
		number++;
	if ( number )
	{
		char str[8 + 1];
		size_t suffix = snprintf(str, sizeof(str), "~%u", number);
		size_t left = 8 - suffix;
		size_t length = 8;
		while ( length && short_name[length - 1] == ' ' )
			length--;
		if ( left < length )
			length = left;
		memcpy(short_name + length, str, suffix);
		decode_8_3(short_name, decoded);
	}
	// Determine whether a long file name is required.
	if ( !strcmp(elem, decoded) )
		needed_entries = 1;
	// Files can only have a single link.
	if ( !dest->deleted && !directories )
		return errno = EPERM, false;
	// Expand the directory if needed.
	if ( free_search.streak < needed_entries )
	{
		if ( inode_id == filesystem->root_inode_id &&
			 filesystem->fat_type != 32 )
			return errno = ENOSPC, false;
		// The longest file name may require multiple clusters.
		size_t remaining_entries = needed_entries - free_search.streak;
		size_t entries_per_cluster =
			filesystem->cluster_size / sizeof(struct fat_dirent);
		size_t needed_clusters =
			divup<size_t>(remaining_entries, entries_per_cluster);
		// Allocate each needed cluster. If allocation fails, free all the
		// clusters, so the resources are freed and rename() is able to restore
		// the old link, since the old directory may have shrunk during unlink.
		fat_ino_t last_cluster = free_search.last_cluster;
		for ( size_t i = 0; i < needed_clusters; i++ )
		{
			fat_ino_t new_cluster = filesystem->AllocateCluster();
			if ( !new_cluster )
				return FreeClusters(free_search.last_cluster), false;
			if ( !ZeroCluster(new_cluster) )
				return filesystem->FreeCluster(new_cluster),
				       FreeClusters(free_search.last_cluster), false;
			if ( !filesystem->WriteFAT(new_cluster, filesystem->eof_cluster) ||
			     !filesystem->WriteFAT(last_cluster, new_cluster) )
				return filesystem->Corrupted(), errno = EIO, false;
			last_cluster = new_cluster;
			// If there were no trailing free space in the directory, start a
			// free space streak in the new cluster.
			if ( !free_search.streak )
			{
				free_search.streak = needed_entries;
				free_search.position.cluster = new_cluster;
				free_search.position.sector = 0;
				free_search.position.offset = 0;
			}
		}
	}
	// Write the new directory entry.
	block = NULL;
	position = free_search.position;
	size_t i = needed_entries;
	while ( Iterate(&block, &position) )
	{
		i--;
		block->BeginWrite();
		struct fat_dirent* dirent =
			(struct fat_dirent*) (block->block_data + position.offset);
		// Write the long name first in backwards order.
		if ( i )
		{
			dirent->ord = (i+1 == needed_entries ? FAT_LONG_NAME_LAST : 0) | i;
			dirent->attributes = FAT_ATTRIBUTE_LONG_NAME;
			dirent->type = 0;
			dirent->checksum = ChecksumName(short_name);
			dirent->zero = htole16(0);
			char16_t* buf = new_long_name + 13 * (i-1);
			for ( size_t n = 0; n < 5; n++ )
			{
				dirent->name1[2*n+0] = (buf[0+n] >> 0) & 0xFF;
				dirent->name1[2*n+1] = (buf[0+n] >> 8) & 0xFF;
			}
			for ( size_t n = 0; n < 6; n++ )
			{
				dirent->name2[2*n+0] = (buf[5+n] >> 0) & 0xFF;
				dirent->name2[2*n+1] = (buf[5+n] >> 8) & 0xFF;
			}
			for ( size_t n = 0; n < 2; n++ )
			{
				dirent->name3[2*n+0] = (buf[11+n] >> 0) & 0xFF;
				dirent->name3[2*n+1] = (buf[11+n] >> 8) & 0xFF;
			}
		}
		// Write the short name last.
		else
		{
			// Link the inode into the directory and undelete it.
			if ( strcmp(elem, ".") != 0 && strcmp(elem, "..") != 0 )
			{
				assert(dest->deleted);
				assert(!dest->parent);
				memcpy(dirent, dest->dirent, sizeof(*dirent));
				dest->dirent = dirent;
				dest->data_block = block;
				block->Refer();
				dest->deleted = false;
				if ( S_ISDIR(dest->Mode()) )
				{
					dest->parent = this;
					dest->parent->Refer();
				}
			}
			else
			{
				memset(dirent, 0, sizeof(*dirent));
				dirent->attributes = FAT_ATTRIBUTE_DIRECTORY;
			}
			memcpy(dirent->name, short_name, sizeof(short_name));
			dirent->cluster_high = htole16(dest->inode_id >> 16);
			dirent->cluster_low = htole16(dest->inode_id & 0xFFFF);
		}
		block->FinishWrite();
		position.offset += sizeof(struct fat_dirent);
		// Finish after writing the short name.
		if ( !i )
		{
			if ( block )
				block->Unref();
			Modified();
			return true;
		}
	}
	if ( block )
		block->Unref();
	block = NULL;
	return errno = EIO, false;
}

Inode* Inode::UnlinkKeep(const char* elem, bool directories, bool force)
{
	struct position position, entry_position;
	position.cluster = first_cluster;
	position.sector = 0;
	position.offset = 0;
	Block* block = NULL;
	char name[FAT_UTF8_NAME_MAX + 1];
	uint8_t file_type;
	fat_ino_t child_inode_id;
	struct fat_dirent* entry;
	size_t entry_length;
	while ( ReadDirectory(&block, &position, name, &file_type, &child_inode_id,
	                      &entry, NULL, &entry_position, &entry_length) )
	{
		if ( !entry || !strcmp(name, ".") || !strcmp(name, "..") )
			continue;
		if ( strcmp(elem, name) != 0 )
			continue;
		// Open the inode and determine whether it can be deleted.
		Inode* inode = filesystem->CreateInode(child_inode_id, block, entry,
		                                       this);
		block->Unref();
		block = NULL;
		if ( !inode )
			return NULL;
		if ( !force && directories && !S_ISDIR(inode->Mode()) )
			return inode->Unref(), errno = ENOTDIR, (Inode*) NULL;
		if ( !force && directories && !inode->IsEmptyDirectory() )
			return inode->Unref(), errno = ENOTEMPTY, (Inode*) NULL;
		if ( !force && !directories && S_ISDIR(inode->Mode()) )
			return inode->Unref(), errno = EISDIR, (Inode*) NULL;
		if ( !filesystem->device->write )
			return inode->Unref(), errno = EROFS, (Inode*) NULL;
		// Unlink the inode and mark it as deleted but still referenced.
		assert(!inode->deleted);
		memcpy(&inode->deleted_dirent, inode->dirent,
		       sizeof(struct fat_dirent));
		inode->deleted_dirent.name[0] = (char) 0xE5;
		inode->dirent = &inode->deleted_dirent;
		inode->data_block->Unref();
		inode->data_block = NULL;
		inode->deleted = true;
		inode->parent->Unref();
		inode->parent = NULL;
		// Erase the directory entry including the long name entries.
		position = entry_position;
		for ( size_t i = 0; i < entry_length; i++ )
		{
			if ( !Iterate(&block, &position) )
			{
				if ( block )
					block->Unref();
				return (Inode*) NULL;
			}
			block->BeginWrite();
			struct fat_dirent* dirent = (struct fat_dirent*)
				(block->block_data + position.offset);
			memset(dirent, 0, sizeof(*dirent));
			dirent->name[0] = (char) 0xE5;
			block->FinishWrite();
			position.offset += sizeof(struct fat_dirent);
		}
		// Determine if this was the last directory entry.
		bool was_last = false;
		while ( true )
		{
			if ( !Iterate(&block, &position) )
			{
				was_last = !errno;
				break;
			}
			struct fat_dirent* dirent = (struct fat_dirent*)
				(block->block_data + position.offset);
			if ( !dirent->name[0] )
			{
				was_last = true;
				break;
			}
			if ( dirent->name[0] != (char) 0xE5 )
				break;
			position.offset += sizeof(struct fat_dirent);
		}
		// Shrink the directory if this was the last directory entry.
		if ( was_last &&
		    (inode_id != filesystem->root_inode_id ||
		     filesystem->fat_type == 32) )
		{
			bool good = true;
			fat_ino_t cluster = filesystem->ReadFAT(entry_position.cluster);
			if ( cluster < 2 || cluster == filesystem->eio_cluster )
				good = false;
			if ( !good && !filesystem->WriteFAT(entry_position.cluster,
			                                    filesystem->eof_cluster) )
				filesystem->Corrupted(), good = false;
			while ( good )
			{
				if ( cluster < 2 || filesystem->eio_cluster <= cluster )
					break;
				fat_ino_t next_cluster = filesystem->ReadFAT(cluster);
				if ( next_cluster < 2 || filesystem->eio_cluster <= cluster )
					break;
				if ( !filesystem->WriteFAT(cluster, 0) )
					return filesystem->Corrupted(), errno = EIO,
					       block->Unref(), (Inode*) NULL;
				filesystem->FreeCluster(cluster);
				cluster = next_cluster;
			}
		}
		if ( block )
			block->Unref();
		block = NULL;
		// Zero the rest of the sector if it was the last entry, so reading the
		// directory stops earlier next time.
		if ( was_last && Iterate(&block, &entry_position) )
		{
			block->BeginWrite();
			memset(block->block_data + entry_position.offset, 0,
			       filesystem->bytes_per_sector - entry_position.offset);
			block->FinishWrite();
		}
		if ( block )
			block->Unref();
		block = NULL;
		Modified();
		return inode;
	}
	if ( block )
		block->Unref();
	block = NULL;
	if ( errno )
		return (Inode*) NULL;
	return errno = ENOENT, (Inode*) NULL;
}

bool Inode::Unlink(const char* elem, bool directories, bool force)
{
	Inode* result = UnlinkKeep(elem, directories, force);
	if ( !result )
		return false;
	result->Unref();
	return true;
}

bool Inode::RelinkParent(Inode* new_parent)
{
	assert(parent);
	assert(new_parent);
	struct position position;
	position.cluster = first_cluster;
	position.sector = 0;
	position.offset = 0;
	Block* block = NULL;
	char name[FAT_UTF8_NAME_MAX + 1];
	uint8_t file_type;
	fat_ino_t child_inode_id;
	struct fat_dirent* entry;
	while ( ReadDirectory(&block, &position, name, &file_type, &child_inode_id,
	                      &entry, NULL, NULL, NULL) )
	{
		if ( strcmp("..", name) != 0 )
			continue;
		// Replace the parent directory with the new one.
		block->BeginWrite();
		parent->Unref();
		parent = new_parent;
		parent->Refer();
		if ( parent->inode_id == filesystem->root_inode_id )
		{
			entry->cluster_high = htole16(0);
			entry->cluster_low = htole16(0);
		}
		else
		{
			entry->cluster_high = htole16(parent->inode_id >> 16);
			entry->cluster_low = htole16(parent->inode_id & 0xFFFF);
		}
		block->FinishWrite();
		break;
	}
	if ( block )
		block->Unref();
	if ( errno )
		return false;
	return true;
}

ssize_t Inode::ReadAt(uint8_t* buf, size_t count, off_t o_offset)
{
	if ( !S_ISREG(Mode()) )
		return errno = EISDIR, -1;
	if ( o_offset < 0 )
		return errno = EINVAL, -1;
	if ( SSIZE_MAX < count )
		count = SSIZE_MAX;
	size_t sofar = 0;
	uintmax_t offset = (uintmax_t) o_offset;
	fat_off_t file_size = Size();
	if ( file_size <= offset )
		return 0;
	if ( file_size - offset < count )
		count = file_size - offset;
	if ( !count )
		return 0;
	fat_off_t cluster_id = offset / filesystem->cluster_size;
	fat_off_t cluster_offset = offset % filesystem->cluster_size;
	fat_ino_t cluster = SeekCluster(cluster_id);
	if ( filesystem->eio_cluster <= cluster )
		return -1;
	while ( sofar < count )
	{
		// Follow the FAT cluster singly linked list for the next cluster.
		if ( filesystem->cluster_size <= cluster_offset )
		{
			cluster = filesystem->ReadFAT(cluster);
			if ( filesystem->eio_cluster <= cluster )
				return sofar ? sofar : (errno = EIO, -1);
			cluster_offset = 0;
			cluster_id++;
			cached_cluster_id = cluster_id;
			cached_cluster = cluster;
		}
		uint8_t sector = cluster_offset / filesystem->bytes_per_sector;
		uint16_t block_offset = cluster_offset % filesystem->bytes_per_sector;
		fat_off_t block_left = filesystem->bytes_per_sector - block_offset;
		Block* block = GetClusterSector(cluster, sector);
		if ( !block )
			return sofar ? sofar : -1;
		size_t amount = count - sofar < block_left ? count - sofar : block_left;
		memcpy(buf + sofar, block->block_data + block_offset, amount);
		sofar += amount;
		cluster_offset += amount;
		block->Unref();
	}
	return (ssize_t) sofar;
}

ssize_t Inode::WriteAt(const uint8_t* buf, size_t count, off_t o_offset)
{
	if ( !S_ISREG(Mode()) )
		return errno = EISDIR, -1;
	if ( o_offset < 0 )
		return errno = EINVAL, -1;
	if ( !filesystem->device->write )
		return errno = EROFS, -1;
	if ( SSIZE_MAX < count )
		count = SSIZE_MAX;
	Modified();
	size_t sofar = 0;
	uintmax_t offset = (uintmax_t) o_offset;
	if ( offset != (fat_off_t) offset )
		return errno = EFBIG, -1;
	fat_off_t file_size = Size();
	fat_off_t offset_left = FAT_OFF_MAX - offset;
	if ( offset_left < count )
	{
		if ( !offset_left )
			return errno = EFBIG, -1;
		count = offset_left;
	}
	fat_off_t end_at = offset + count;
	// Expand the file if needed.
	if ( file_size < end_at && !Truncate(end_at) )
	{
		// See if a partial write is still possible.
		file_size = Size();
		if ( file_size < offset )
			return -1;
		if ( !file_size - offset )
			return -1;
		count = file_size - offset;
	}
	fat_off_t cluster_id = offset / filesystem->cluster_size;
	fat_off_t cluster_offset = offset % filesystem->cluster_size;
	fat_ino_t cluster = SeekCluster(cluster_id);
	if ( filesystem->eio_cluster <= cluster )
		return -1;
	while ( sofar < count )
	{
		// Follow the FAT cluster singly linked list for the next cluster.
		if ( filesystem->cluster_size <= cluster_offset )
		{
			cluster = filesystem->ReadFAT(cluster);
			if ( filesystem->eio_cluster <= cluster )
				return sofar ? sofar : (errno = EIO, -1);
			cluster_offset = 0;
			cluster_id++;
			cached_cluster_id = cluster_id;
			cached_cluster = cluster;
		}
		uint8_t sector = cluster_offset / filesystem->bytes_per_sector;
		uint16_t block_offset = cluster_offset % filesystem->bytes_per_sector;
		fat_off_t block_left = filesystem->bytes_per_sector - block_offset;
		Block* block = GetClusterSector(cluster, sector);
		if ( !block )
			return sofar ? sofar : -1;
		size_t amount = count - sofar < block_left ? count - sofar : block_left;
		block->BeginWrite();
		memcpy(block->block_data + block_offset, buf + sofar, amount);
		block->FinishWrite();
		sofar += amount;
		cluster_offset += amount;
		block->Unref();
	}
	return (ssize_t) sofar;
}

bool Inode::Rename(Inode* olddir, const char* oldname, const char* newname)
{
	if ( deleted )
		return errno = ENOENT, false;
	if ( !strcmp(oldname, ".") || !strcmp(oldname, "..") ||
	     !strcmp(newname, ".") || !strcmp(newname, "..") )
		return errno = EINVAL, false;
	Inode* src_inode = olddir->Open(oldname, O_RDONLY, 0);
	if ( !src_inode )
		return false;
	if ( Inode* dst_inode = Open(newname, O_RDONLY, 0) )
	{
		bool same_inode = src_inode->inode_id == dst_inode->inode_id;
		dst_inode->Unref();
		if ( same_inode )
			return src_inode->Unref(), true;
	}
	// Refuse copying a directory into itself.
	for ( Inode* inode = this; inode; inode = inode->parent )
		if ( inode == src_inode )
			return src_inode->Unref(), errno = EINVAL, false;
	bool is_dir = S_ISDIR(src_inode->Mode());
	// Remove any conflicting destination file.
	if ( !Unlink(newname, is_dir) && errno != ENOENT )
		return src_inode->Unref(), false;
	// Remove the old file link.
	if ( !olddir->Unlink(oldname, is_dir, is_dir) )
		return src_inode->Unref(), false;
	// Link the file in the new directory.
	if ( !Link(newname, src_inode, is_dir) )
	{
		// If that failed, try to link the file back in the old directory, which
		// shouldn't fail as the resources should've been freed.
		int errnum = errno;
		if ( !olddir->Link(oldname, src_inode, is_dir) )
			filesystem->Corrupted();
		else
			errno = errnum;
		return src_inode->Unref(), false;
	}
	if ( is_dir && olddir != this )
		src_inode->RelinkParent(this);
	src_inode->Unref();
	return true;
}

bool Inode::Symlink(const char* elem, const char* dest)
{
	(void) elem;
	(void) dest;
	if ( !filesystem->device->write )
		return errno = EROFS, false;
	return errno = EPERM, false;
}

Inode* Inode::CreateDirectory(const char* path, mode_t mode)
{
	return Open(path, O_CREAT | O_EXCL, mode | S_IFDIR);
}

bool Inode::RemoveDirectory(const char* path)
{
	Inode* result = UnlinkKeep(path, true);
	if ( !result )
		return false;
	// There is no need to remove the . and .. and entries since there is no
	// link count and the directory is empty. We can just discard the data.
	result->Unref();
	return true;
}

bool Inode::IsEmptyDirectory()
{
	if ( !S_ISDIR(Mode()) )
		return errno = ENOTDIR, false;
	if ( deleted )
		return errno = ENOENT, false;
	if ( inode_id == filesystem->root_inode_id )
		return false;
	struct position position;
	position.cluster = first_cluster;
	position.sector = 0;
	position.offset = 0;
	Block* block = NULL;
	while ( Iterate(&block, &position) )
	{
		uint8_t* block_data = block->block_data + position.offset;
		struct fat_dirent* entry = (struct fat_dirent*) block_data;
		if ( !entry->name[0] )
			break;
		char name[8 + 1 + 3 + 1];
		if ( (unsigned char) entry->name[0] != 0xE5 &&
		     !(entry->attributes & FAT_ATTRIBUTE_VOLUME_ID) &&
		     (decode_8_3(entry->name, name),
              strcmp(name, ".") != 0 && strcmp(name, "..") != 0) )
			return block->Unref(), false;
		position.offset += sizeof(struct fat_dirent);
	}
	if ( block )
		block->Unref();
	if ( errno )
		return false;
	return true;
}

void Inode::Delete()
{
	assert(deleted);
	assert(dirent->name[0] == 0x00 || (unsigned char) dirent->name[0] == 0xE5);
	assert(!reference_count);
	assert(!remote_reference_count);
	fat_ino_t cluster = first_cluster;
	while ( true )
	{
		if ( cluster < 2 || filesystem->eio_cluster == cluster )
			break;
		if ( filesystem->eio_cluster < cluster )
			break;
		fat_ino_t next_cluster = filesystem->ReadFAT(cluster);
		if ( next_cluster < 2 || next_cluster == filesystem->eio_cluster )
			break;
		if ( !filesystem->WriteFAT(cluster, 0) )
		{
			filesystem->Corrupted();
			break;
		}
		filesystem->FreeCluster(cluster);
		cluster = next_cluster;
	}
}

void Inode::Refer()
{
	reference_count++;
}

void Inode::Unref()
{
	assert(0 < reference_count);
	reference_count--;
	if ( !reference_count && !remote_reference_count )
	{
		if ( deleted )
			Delete();
		delete this;
	}
}

void Inode::RemoteRefer()
{
	remote_reference_count++;
}

void Inode::RemoteUnref()
{
	assert(0 < remote_reference_count);
	remote_reference_count--;
	if ( !reference_count && !remote_reference_count )
	{
		if ( deleted )
			Delete();
		delete this;
	}
}

void Inode::Modified()
{
	if ( inode_id == filesystem->root_inode_id )
		return;
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	if ( data_block )
		data_block->BeginWrite();
	uint8_t tenths;
	timespec_to_fat(&now, &dirent->modified_date, &dirent->modified_time,
	                &tenths);
	if ( data_block )
		data_block->FinishWrite();
}

void Inode::BeginWrite()
{
	if ( data_block )
		data_block->BeginWrite();
}

// TODO: Uh. Use these?
void Inode::FinishWrite()
{
	//struct timespec now;
	//clock_gettime(CLOCK_REALTIME, &now);
	//data->i_ctime = now.tv_sec;
	if ( !dirty )
	{
		dirty = true;
		prev_dirty = NULL;
		next_dirty = filesystem->dirty_inode;
		if ( next_dirty )
			next_dirty->prev_dirty = this;
		filesystem->dirty_inode = this;
	}
	if ( data_block )
		data_block->FinishWrite();
	Use();
}

void Inode::Sync()
{
	if ( !dirty )
		return;
	if ( data_block )
		data_block->Sync();
	// TODO: The inode contents needs to be sync'd as well!
	(prev_dirty ? prev_dirty->next_dirty : filesystem->dirty_inode) = next_dirty;
	if ( next_dirty )
		next_dirty->prev_dirty = prev_dirty;
	prev_dirty = NULL;
	next_dirty = NULL;
	dirty = false;
}

void Inode::Use()
{
	if ( data_block )
		data_block->Use();
	Unlink();
	Prelink();
}

void Inode::Unlink()
{
	(prev_inode ? prev_inode->next_inode : filesystem->mru_inode) = next_inode;
	(next_inode ? next_inode->prev_inode : filesystem->lru_inode) = prev_inode;
	size_t bin = inode_id % INODE_HASH_LENGTH;
	(prev_hashed ? prev_hashed->next_hashed : filesystem->hash_inodes[bin]) = next_hashed;
	if ( next_hashed ) next_hashed->prev_hashed = prev_hashed;
}

void Inode::Prelink()
{
	prev_inode = NULL;
	next_inode = filesystem->mru_inode;
	if ( filesystem->mru_inode )
		filesystem->mru_inode->prev_inode = this;
	filesystem->mru_inode = this;
	if ( !filesystem->lru_inode )
		filesystem->lru_inode = this;
	size_t bin = inode_id % INODE_HASH_LENGTH;
	prev_hashed = NULL;
	next_hashed = filesystem->hash_inodes[bin];
	filesystem->hash_inodes[bin] = this;
	if ( next_hashed )
		next_hashed->prev_hashed = this;
}
