/*
 * Copyright (c) 2013, 2014, 2015, 2018, 2022, 2025 Jonas 'Sortie' Termansen.
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
#include <ctype.h>
#include <errno.h>
#include <endian.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "block.h"
#include "device.h"
#include "filesystem.h"
#include "inode.h"
#include "iso9660fs.h"
#include "util.h"

#ifndef S_SETABLE
#define S_SETABLE 02777
#endif
#ifndef O_WRITE
#define O_WRITE (O_WRONLY | O_RDWR)
#endif

Inode::Inode(Filesystem* filesystem, iso9660_ino_t inode_id)
{
	this->prev_inode = NULL;
	this->next_inode = NULL;
	this->prev_hashed = NULL;
	this->next_hashed = NULL;
	this->data_block = NULL;
	this->data = NULL;
	this->filesystem = filesystem;
	this->reference_count = 1;
	this->remote_reference_count = 0;
	this->inode_id = inode_id;
}

Inode::~Inode()
{
	if ( data_block )
		data_block->Unref();
	Unlink();
}

static unsigned char digit(char c)
{
	return '0' <= c && c <= '9' ? c - '0' : 0;
}

static struct timespec DecodeTimestamp(const uint8_t* time_bytes, uint8_t flags)
{
	struct tm tm;
	memset(&tm, 0, sizeof(tm));
	struct timespec ts;
	int8_t offset;
	if ( flags & ISO9660_TF_LONG_FORM )
	{
		tm.tm_year = digit(time_bytes[0]) * 1000 +
		             digit(time_bytes[1]) * 100 +
		             digit(time_bytes[2]) * 10 +
		             digit(time_bytes[3]) - 1900;
		tm.tm_mon = (digit(time_bytes[4]) * 10 + digit(time_bytes[5])) - 1;
		tm.tm_mday = (digit(time_bytes[6]) * 10 + digit(time_bytes[7]));
		tm.tm_hour = (digit(time_bytes[8]) * 10 + digit(time_bytes[9]));
		tm.tm_min = (digit(time_bytes[10]) * 10 + digit(time_bytes[11]));
		tm.tm_sec = (digit(time_bytes[12]) * 10 + digit(time_bytes[13]));
		ts.tv_nsec = (digit(time_bytes[14]) * 10 + digit(time_bytes[15])) *
		             10000000;
		offset = (int8_t) time_bytes[16];
	}
	else
	{
		tm.tm_year = time_bytes[0];
		tm.tm_mon = time_bytes[1] - 1;
		tm.tm_mday = time_bytes[2];
		tm.tm_hour = time_bytes[3];
		tm.tm_min = time_bytes[4];
		tm.tm_sec = time_bytes[5];
		ts.tv_nsec = 0;
		offset = (int8_t) time_bytes[6];
	}
	time_t tz_offset = offset * 15 * 60;
	// TODO: The timezone offset should've been mixed in with timegm, somehow.
	ts.tv_sec = timegm(&tm) - tz_offset;
	return ts;
}

struct EntryState
{
	const uint8_t* data;
	size_t data_size;
	Block* block;
	uint32_t ce_lba;
	uint32_t ce_offset;
	uint32_t ce_size;
	uint32_t next_ce_lba;
	uint32_t next_ce_offset;
	uint32_t next_ce_size;
	uint32_t block_count;
	uint8_t ext;
};

static void BeginEntries(struct EntryState* state, const uint8_t* data,
                         size_t data_size, uint32_t inode_id,
                         Filesystem* filesystem)
{
	memset(state, 0, sizeof(*state));
	if ( filesystem->no_susp )
		return;
	else if ( inode_id == filesystem->root_ino )
	{
		if ( data_size < 7 )
			return;
		if ( data[0] != 'S' || data[1] != 'P' || data[2] < 7 || data[3] != 1 ||
		     data[4] != 0xBE || data[5] != 0xEF )
			return;
		state->data = data;
		state->data_size = data_size;
	}
	else
	{
		if ( !filesystem->susp_enabled )
			return;
		if ( data_size < filesystem->susp_offset )
			return;
		state->data = data + filesystem->susp_offset;
		state->data_size = data_size - filesystem->susp_offset;
	}
}

void EndEntries(struct EntryState* state)
{
	if ( state->block )
		state->block->Unref();
	memset(state, 0, sizeof(*state));
}

static bool ReadEntry(struct EntryState* state,
                      const uint8_t** out_field,
                      uint8_t* out_field_len,
                      uint8_t* out_field_ext,
                      Filesystem* filesystem)
{
	while ( true )
	{
		while ( 4 <= state->data_size )
		{
			const uint8_t* field = state->data;
			uint8_t len = field[2];
			if ( len < 4 || state->data_size < len )
				return EndEntries(state), errno = EINVAL, false;
			state->data += len;
			state->data_size -= len;
			if ( field[0] == 'C' && field[1] == 'E' &&
			     28 <= len && field[3] == 1 )
			{
				uint32_t bits;
				memcpy(&bits, field + 4, sizeof(bits));
				state->next_ce_lba = le32toh(bits);
				memcpy(&bits, field + 12, sizeof(bits));
				state->next_ce_offset = le32toh(bits);
				memcpy(&bits, field + 20, sizeof(bits));
				state->next_ce_size = le32toh(bits);
				continue;
			}
			else if ( field[0] == 'E' && field[1] == 'S' &&
			          5 <= len && field[3] == 1 )
			{
				state->ext = field[4];
				continue;
			}
			else if ( field[0] == 'S' && field[1] == 'T' &&
			          4 <= len && field[3] == 1 )
				break;
			*out_field = field;
			*out_field_len = len;
			*out_field_ext = state->ext;
			return true;
		}
		if ( state->block )
			state->block->Unref();
		state->block = NULL;
		if ( !state->ce_size && state->next_ce_size )
		{
			uint32_t lba_extra = state->next_ce_offset / filesystem->block_size;
			state->ce_offset = state->next_ce_offset % filesystem->block_size;
			if ( UINT32_MAX - state->next_ce_lba < lba_extra )
				return errno = EINVAL, false;
			state->ce_lba = state->next_ce_lba + lba_extra;
			state->ce_size = state->next_ce_size;
			state->next_ce_lba = 0;
			state->next_ce_offset = 0;
			state->next_ce_size = 0;
		}
		else if ( !state->ce_size )
			return errno = 0, false;
		// Drop additional entries after reaching the block limit.
		if ( 32 <= state->block_count++ )
			return errno = 0, false;
		if ( !(state->block = filesystem->device->GetBlock(state->ce_lba)) )
			return false;
		state->data = state->block->block_data + state->ce_offset;
		state->data_size = state->ce_size < filesystem->block_size ?
		                   state->ce_size : filesystem->block_size;
		state->ce_lba++;
		state->ce_offset = 0;
		state->ce_size -= state->data_size;
	}
}

bool Inode::ActivateExtensions()
{
	assert(inode_id == filesystem->root_ino);
	const uint8_t* block_data = (const uint8_t*) data;
	uint8_t dirent_len = block_data[0];
	uint8_t name_len = block_data[32];
	size_t extended_off = 33 + name_len + !(name_len & 1);
	if ( dirent_len < extended_off )
		return errno = EINVAL, false;
	struct EntryState entry_state;
	BeginEntries(&entry_state, block_data + extended_off,
	             dirent_len - extended_off, inode_id, filesystem);
	const uint8_t* field;
	uint8_t len, ext, next_ext = 0;
	while ( ReadEntry(&entry_state, &field, &len, &ext, filesystem) )
	{
		if ( field[0] == 'S' && field[1] == 'P' && 7 <= len &&
		     field[3] == 1 && field[4] == 0xBE && field[5] == 0xEF &&
		     !filesystem->no_susp )
		{
			filesystem->susp_enabled = true;
			filesystem->susp_offset = field[6];
		}
		else if ( field[0] == 'E' && field[1] == 'R' && 8 <= field[2] &&
		          field[3] == 1 && !filesystem->no_rock )
		{
			const uint8_t* id = field + 8;
			uint8_t id_len = field[4];
			uint8_t desc_len = field[5];
			uint8_t src_len = field[6];
			uint8_t version = field[7];
			if ( len < 8 + id_len + desc_len + src_len )
				continue;
			if ( version == 1 &&
			     ((id_len == 10 && !memcmp(id, "RRIP_1991A", 10)) ||
			      (id_len == 10 && !memcmp(id, "IEEE_P1282", 10)) ||
			      (id_len == 9 && !memcmp(id, "IEEE_1282", 9))) )
				filesystem->rr_ext = next_ext++;
		}
	}
	return !errno;
}

bool Inode::Parse()
{
	const uint8_t* block_data = (const uint8_t*) data;
	uid = 0;
	gid = 0;
	uint8_t file_flags = block_data[25];
	bool is_directory = file_flags & ISO9660_DIRENT_FLAG_DIR;
	mode = 0555 | (is_directory ? ISO9660_S_IFDIR : ISO9660_S_IFREG);
	uint32_t u32;
	memcpy(&u32, block_data + 10, sizeof(u32));
	size = le32toh(u32);
	nlink = 1;
	const uint8_t* time_bytes = block_data + 18;
	mtim = DecodeTimestamp(time_bytes, 0);
	atim = mtim;
	ctim = mtim;
	uint8_t dirent_len = block_data[0];
	uint8_t name_len = block_data[32];
	size_t extended_off = 33 + name_len + !(name_len & 1);
	if ( dirent_len < extended_off )
		return errno = EINVAL, false;
	struct EntryState entry_state;
	BeginEntries(&entry_state, block_data + extended_off,
	             dirent_len - extended_off, inode_id, filesystem);
	const uint8_t* field;
	uint8_t len, ext;
	while ( ReadEntry(&entry_state, &field, &len, &ext, filesystem) )
	{
		if ( ext == filesystem->rr_ext && 36 <= len &&
		     field[0] == 'P' && field[1] == 'X' && field[3] == 1 )
		{
			uint32_t bits;
			memcpy(&bits, field + 4, sizeof(bits));
			mode = le32toh(bits) & 0xFFFF;
			memcpy(&bits, field + 12, sizeof(bits));
			nlink = le32toh(bits);
			memcpy(&bits, field + 20, sizeof(bits));
			uid = le32toh(bits);
			memcpy(&bits, field + 28, sizeof(bits));
			gid = le32toh(bits);
		}
		else if ( ext == filesystem->rr_ext && 5 <= len &&
		          field[0] == 'T' && field[1] == 'F' && field[3] == 1 )
		{
			uint8_t flags = field[4];
			size_t size = flags & ISO9660_TF_LONG_FORM ? 17 : 7;
			const uint8_t* timestamps = field + 5;
			size_t left = len - 5;
			size_t index = 0;
			if ( (flags & ISO9660_TF_CREATION) && size * (index + 1) <= left )
				index++;
			if ( (flags & ISO9660_TF_MODIFY) && size * (index + 1) <= left )
				mtim = DecodeTimestamp(timestamps + size * index++, flags);
			if ( (flags & ISO9660_TF_ACCESS) && size * (index + 1) <= left )
				atim = DecodeTimestamp(timestamps + size * index++, flags);
			if ( (flags & ISO9660_TF_ATTRIBUTES) && size * (index + 1) <= left )
				ctim = DecodeTimestamp(timestamps + size * index++, flags);
		}
	}
	if ( errno )
		return false;
	if ( ISO9660_S_ISLNK(mode) )
	{
		ssize_t amount = ReadLink(NULL, SSIZE_MAX);
		if ( amount < 0 )
			return false;
		size = amount;
	}
	return true;
}

uint32_t Inode::Mode()
{
	return mode;
}

uint32_t Inode::UserId()
{
	return uid;
}

uint32_t Inode::GroupId()
{
	return gid;
}

uint64_t Inode::Size()
{
	return size;
}

Block* Inode::GetBlock(uint32_t offset)
{
	const uint8_t* block_data = (const uint8_t*) data;
	uint32_t lba;
	memcpy(&lba, block_data + 2, sizeof(lba));
	lba = le32toh(lba) + block_data[1];
	uint32_t block_id = lba + offset;
	return filesystem->device->GetBlock(block_id);
}

bool Inode::ReadDirectory(uint64_t* offset_inout,
                          Block** block_inout,
                          uint64_t* block_id_inout,
                          char* name,
                          uint8_t* file_type_out,
                          iso9660_ino_t* inode_id_out)
{
	uint64_t offset = *offset_inout;
next_block:
	uint64_t filesize = Size();
	if ( filesize <= offset )
		return errno = 0, false;
	uint64_t entry_block_id = offset / filesystem->block_size;
	uint64_t entry_block_offset = offset % filesystem->block_size;
	if ( *block_inout && *block_id_inout != entry_block_id )
	{
		(*block_inout)->Unref();
		(*block_inout) = NULL;
	}
	if ( !*block_inout &&
	     !(*block_inout = GetBlock(*block_id_inout = entry_block_id)) )
		return false;
	const uint8_t* block_data = (*block_inout)->block_data + entry_block_offset;
	uint8_t dirent_len = block_data[0];
	if ( !dirent_len )
	{
		offset = (entry_block_id + 1) * filesystem->block_size;
		goto next_block;
	}
	uint64_t reclen = dirent_len + (dirent_len & 1);
	if ( !reclen )
		return errno = EINVAL, false;
	offset += reclen;
	*offset_inout = offset;
	uint8_t name_len = block_data[32];
	const uint8_t* name_data = block_data + 33;
	size_t extended_off = 33 + name_len + !(name_len & 1);
	if ( dirent_len < extended_off )
		return errno = EINVAL, false;
	iso9660_ino_t entry_inode_id =
		((iso9660_ino_t) (*block_inout)->block_id *
		 (iso9660_ino_t) filesystem->block_size) +
		(iso9660_ino_t) entry_block_offset;
	uint8_t file_flags = block_data[25];
	if ( file_flags & ISO9660_DIRENT_FLAG_NO_EXIST )
		goto next_block;
	bool is_directory = file_flags & ISO9660_DIRENT_FLAG_DIR;
	// TODO: ISO9660_DIRENT_FLAG_MULTI_EXTENT is not implemented.
	if ( name_len == 0 || !name_data[0] )
	{
		if ( name )
		{
			name[0] = '.';
			name[1] = '\0';
			name_len = 1;
		}
		entry_inode_id = inode_id;
	}
	else if ( name_len == 1 && name_data[0] == 1 )
	{
		if ( name )
		{
			name[0] = '.';
			name[1] = '.';
			name[2] = '\0';
			name_len = 2;
		}
		if ( inode_id == filesystem->root_ino )
			entry_inode_id = inode_id;
		else
		{
			uint32_t parent_lba;
			memcpy(&parent_lba, block_data + 2, sizeof(parent_lba));
			parent_lba = le32toh(parent_lba);
			entry_inode_id = (iso9660_ino_t) parent_lba *
			                 (iso9660_ino_t) filesystem->block_size;
		}
	}
	else
	{
		if ( name )
		{
			for ( size_t i = 0; i < name_len; i++ )
			{
				if ( name_data[i] == ';' )
				{
					if ( i && name[i - 1] == '.' )
						name[--i] = '\0';
					name_len = i;
					break;
				}
				name[i] = tolower(name_data[i]);
			}
			name[name_len] = '\0';
		}
		if ( is_directory )
		{
			uint32_t lba;
			memcpy(&lba, block_data + 2, sizeof(lba));
			lba = le32toh(lba);
			entry_inode_id = (iso9660_ino_t) lba *
			                 (iso9660_ino_t) filesystem->block_size;
		}
	}
	uint8_t file_type = ISO9660_FT_UNKNOWN;
	bool append_name = false;
	struct EntryState entry_state;
	BeginEntries(&entry_state, block_data + extended_off,
	             dirent_len - extended_off, entry_inode_id, filesystem);
	const uint8_t* field;
	uint8_t len, ext;
	while ( ReadEntry(&entry_state, &field, &len, &ext, filesystem) )
	{
		if ( ext == filesystem->rr_ext && 5 <= len &&
		     field[0] == 'N' && field[1] == 'M' && field[3] == 1 && name )
		{
			uint8_t nm_flags = field[4];
			if ( !append_name )
				name_len = 0;
			const char* data = (const char*) (field + 5);
			uint8_t data_len = len - 5;
			if ( nm_flags & ISO9660_NM_CURRENT )
				data = ".", data_len = 1;
			else if ( nm_flags & ISO9660_NM_PARENT )
				data = "..", data_len = 2;
			size_t available = 255 - name_len;
			if ( available < data_len )
				data_len = available;
			memcpy(name + name_len, data, data_len);
			name_len += data_len;
			name[name_len] = '\0';
			append_name = nm_flags & ISO9660_NM_CONTINUE;
		}
		else if ( ext == filesystem->rr_ext && 36 <= len &&
		          field[0] == 'P' && field[1] == 'X' && field[3] == 1 )
		{
			uint32_t mode;
			memcpy(&mode, field + 4, sizeof(mode));
			mode = le32toh(mode);
			file_type = ISO9660_FT_OF_MODE(mode);
		}
		else if ( ext == filesystem->rr_ext && 12 <= len &&
		          (field[0] == 'C' || field[0] == 'P') && field[1] == 'L' &&
		          field[3] == 1 )
		{
			file_type = ISO9660_FT_DIR;
			uint32_t real_lba;
			memcpy(&real_lba, field + 4, sizeof(real_lba));
			real_lba = le32toh(real_lba);
			entry_inode_id = (iso9660_ino_t) real_lba *
				             (iso9660_ino_t) filesystem->block_size;
		}
		// Skip relocated directories.
		else if ( ext == filesystem->rr_ext && 4 <= len &&
		          field[0] == 'R' && field[1] == 'E' && field[3] == 1 )
		{
			EndEntries(&entry_state);
			goto next_block;
		}
	}
	if ( errno )
		return false;
	if ( file_type == ISO9660_FT_UNKNOWN )
		file_type = is_directory ? ISO9660_FT_DIR : ISO9660_FT_REG_FILE;
	*file_type_out = file_type;
	*inode_id_out = entry_inode_id;
	return true;
}

Inode* Inode::Open(const char* elem, int flags, mode_t mode)
{
	if ( !ISO9660_S_ISDIR(Mode()) )
		return errno = ENOTDIR, (Inode*) NULL;
	size_t elem_length = strlen(elem);
	if ( elem_length == 0 )
		return errno = ENOENT, (Inode*) NULL;
	uint64_t offset = 0;
	Block* block = NULL;
	uint64_t block_id = 0;
	char name[256];
	uint8_t file_type;
	iso9660_ino_t inode_id;
	while ( ReadDirectory(&offset, &block, &block_id, name, &file_type,
	                      &inode_id) )
	{
		size_t name_len = strlen(name);
		if ( name_len == elem_length && memcmp(elem, name, elem_length) == 0 )
		{
			block->Unref();
			if ( (flags & O_CREAT) && (flags & O_EXCL) )
				return errno = EEXIST, (Inode*) NULL;
			if ( (flags & O_DIRECTORY) &&
			     file_type != ISO9660_FT_UNKNOWN &&
			     file_type != ISO9660_FT_DIR &&
			     file_type != ISO9660_FT_SYMLINK )
				return errno = ENOTDIR, (Inode*) NULL;
			Inode* inode = filesystem->GetInode(inode_id);
			if ( !inode )
				return (Inode*) NULL;
			if ( flags & O_DIRECTORY &&
			     !ISO9660_S_ISDIR(inode->Mode()) &&
			     !ISO9660_S_ISLNK(inode->Mode()) )
			{
				inode->Unref();
				return errno = ENOTDIR, (Inode*) NULL;
			}
			if ( flags & O_WRITE )
			{
				inode->Unref();
				return errno = EROFS, (Inode*) NULL;
			}
			return inode;
		}
	}
	if ( block )
		block->Unref();
	if ( flags & O_CREAT )
	{
		(void) mode;
		return errno = EROFS, (Inode*) NULL;
	}
	return errno = ENOENT, (Inode*) NULL;
}

bool Inode::Link(const char* elem, Inode* dest)
{
	if ( !ISO9660_S_ISDIR(Mode()) )
		return errno = ENOTDIR, false;
	if ( ISO9660_S_ISDIR(dest->Mode()) )
		return errno = EISDIR, false;

	size_t elem_length = strlen(elem);
	if ( elem_length == 0 )
		return errno = ENOENT, false;
	uint64_t offset = 0;
	Block* block = NULL;
	uint64_t block_id = 0;
	char name[256];
	uint8_t file_type;
	iso9660_ino_t inode_id;
	while ( ReadDirectory(&offset, &block, &block_id, name, &file_type,
	                      &inode_id) )
	{
		size_t name_len = strlen(name);
		if ( name_len == elem_length && memcmp(elem, name, elem_length) == 0 )
		{
			block->Unref();
			return errno = EEXIST, false;
		}
	}
	if ( block )
		block->Unref();
	return errno = EROFS, false;
}

Inode* Inode::UnlinkKeep(const char* elem, bool directories, bool force)
{
	if ( !ISO9660_S_ISDIR(Mode()) )
		return errno = ENOTDIR, (Inode*) NULL;
	size_t elem_length = strlen(elem);
	if ( elem_length == 0 )
		return errno = ENOENT, (Inode*) NULL;
	uint64_t offset = 0;
	Block* block = NULL;
	uint64_t block_id = 0;
	char name[256];
	uint8_t file_type;
	iso9660_ino_t inode_id;
	while ( ReadDirectory(&offset, &block, &block_id, name, &file_type,
	                      &inode_id) )
	{
		size_t name_len = strlen(name);
		if ( name_len == elem_length && memcmp(elem, name, elem_length) == 0 )
		{
			(void) directories;
			(void) force;
			block->Unref();
			return errno = EROFS, (Inode*) NULL;
		}
	}
	if ( block )
		block->Unref();
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

ssize_t Inode::ReadLink(uint8_t* buf, size_t buf_size)
{
	size_t result = 0;
	const uint8_t* block_data = (const uint8_t*) data;
	uint8_t dirent_len = block_data[0];
	uint8_t name_len = block_data[32];
	size_t extended_off = 33 + name_len + !(name_len & 1);
	if ( dirent_len < extended_off )
		return errno = EINVAL, false;
	bool omit_slash = true;
	struct EntryState entry_state;
	BeginEntries(&entry_state, block_data + extended_off,
	             dirent_len - extended_off, inode_id, filesystem);
	const uint8_t* field;
	uint8_t len, ext;
	while ( ReadEntry(&entry_state, &field, &len, &ext, filesystem) )
	{
		if ( ext == filesystem->rr_ext && 5 <= len &&
		     field[0] == 'S' && field[1] == 'L' && field[3] == 1 )
		{
			for ( size_t n = 5; n < len && 2 <= len - n; )
			{
				uint8_t comp_flags = field[n + 0];
				uint8_t comp_len = field[n + 1];
				if ( len - (n + 2) < comp_len )
					break;
				const char* comp = (const char*) (field + n + 2);
				n += 2 + comp_len;
				if ( !omit_slash )
				{
					if ( result == buf_size )
						break;
					if ( buf )
						buf[result] = '/';
					result++;
				}
				if ( comp_flags & ISO9660_SL_CURRENT )
					comp = ".", comp_len = 1;
				else if ( comp_flags & ISO9660_SL_PARENT )
					comp = "..", comp_len = 2;
				else if ( comp_flags & ISO9660_SL_ROOT )
					comp = "/", comp_len = 1;
				size_t possible = buf_size - result;
				size_t count = comp_len < possible ? comp_len : possible;
				if ( buf )
					memcpy(buf + result, comp, count);
				result += count;
				if ( possible < comp_len )
					break;
				// Older libisofs and genisoimage wrongly set the root bit on
				// non-root components and encode trailing slashes incorrectly.
				// Don't add another slash if the root bit was set.
				// https://lists.gnu.org/archive/html/bug-xorriso/2025-04/msg00000.html
				omit_slash = comp_flags & (ISO9660_SL_CONTINUE|ISO9660_SL_ROOT);
			}
		}
	}
	if ( errno )
		return -1;
	return (ssize_t) result;
}

ssize_t Inode::ReadAt(uint8_t* buf, size_t s_count, off_t o_offset)
{
	if ( !ISO9660_S_ISREG(Mode()) )
		return errno = EISDIR, -1;
	if ( o_offset < 0 )
		return errno = EINVAL, -1;
	if ( (off_t) (uint64_t) o_offset != o_offset )
		return 0;
	if ( SSIZE_MAX < s_count )
		s_count = SSIZE_MAX;
	uint64_t sofar = 0;
	uint64_t count = (uint64_t) s_count;
	uint64_t offset = (uint64_t) o_offset;
	uint64_t file_size = Size();
	if ( file_size <= offset )
		return 0;
	if ( file_size - offset < count )
		count = file_size - offset;
	while ( sofar < count )
	{
		uint64_t block_id = offset / filesystem->block_size;
		uint32_t block_offset = offset % filesystem->block_size;
		uint32_t block_left = filesystem->block_size - block_offset;
		Block* block = GetBlock(block_id);
		if ( !block )
			return sofar ? sofar : -1;
		size_t amount = count - sofar < block_left ? count - sofar : block_left;
		memcpy(buf + sofar, block->block_data + block_offset, amount);
		sofar += amount;
		offset += amount;
		block->Unref();
	}
	return (ssize_t) sofar;
}

bool Inode::Rename(Inode* olddir, const char* oldname, const char* newname)
{
	if ( !strcmp(oldname, ".") || !strcmp(oldname, "..") ||
	     !strcmp(newname, ".") || !strcmp(newname, "..") )
		return errno = EPERM, false;
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
	src_inode->Unref();
	return errno = EROFS, false;
}

bool Inode::RemoveDirectory(const char* path)
{
	return UnlinkKeep(path, true);
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
		delete this;
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
		delete this;
}

void Inode::Use()
{
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
