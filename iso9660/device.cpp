/*
 * Copyright (c) 2013, 2014, 2015, 2022, 2025 Jonas 'Sortie' Termansen.
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
 * device.cpp
 * Block device.
 */

#include <sys/stat.h>
#include <sys/types.h>

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "block.h"
#include "device.h"
#include "ioleast.h"

Device::Device(int fd, const char* path, uint32_t block_size,
               size_t block_limit)
{
	this->mru_block = NULL;
	this->lru_block = NULL;
	for ( size_t i = 0; i < DEVICE_HASH_LENGTH; i++ )
		hash_blocks[i] = NULL;
	struct stat st;
	fstat(fd, &st);
	this->device_size = st.st_size;
	this->path = path;
	this->block_size = block_size;
	this->fd = fd;
	this->block_count = 0;
	this->block_limit = block_limit;
}

Device::~Device()
{
	while ( mru_block )
		delete mru_block;
	close(fd);
}

Block* Device::AllocateBlock()
{
	if ( block_limit <= block_count )
	{
		for ( Block* block = lru_block; block; block = block->prev_block )
		{
			if ( block->reference_count )
				continue;
			block->Destruct(); // Syncs.
			return block;
		}
	}
	uint8_t* data = new uint8_t[block_size];
	if ( !data ) // TODO: Use operator new nothrow!
		return NULL;
	Block* block = new Block();
	if ( !block ) // TODO: Use operator new nothrow!
		return delete[] data, (Block*) NULL;
	block->block_data = data;
	block_count++;
	return block;
}

Block* Device::GetBlock(uint32_t block_id)
{
	if ( Block* block = GetCachedBlock(block_id) )
		return block;
	Block* block = AllocateBlock();
	if ( !block )
		return NULL;
	block->Construct(this, block_id);
	off_t file_offset = (off_t) block_size * (off_t) block_id;
	preadall(fd, block->block_data, block_size, file_offset);
	block->Prelink();
	return block;
}

Block* Device::GetCachedBlock(uint32_t block_id)
{
	size_t bin = block_id % DEVICE_HASH_LENGTH;
	for ( Block* iter = hash_blocks[bin]; iter; iter = iter->next_hashed )
		if ( iter->block_id == block_id )
			return iter->Refer(), iter;
	return NULL;
}
