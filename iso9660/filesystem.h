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
 * filesystem.h
 * ISO 9660 filesystem implementation.
 */

#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include "iso9660.h"

class Device;
class Inode;

static const size_t INODE_HASH_LENGTH = 1 << 16;

class Filesystem
{
public:
	Filesystem(Device* device, const char* mount_path,
	           const struct iso9660_pvd* pvd);
	~Filesystem();

public:
	const struct iso9660_pvd* pvd;
	Device* device;
	const char* mount_path;
	iso9660_ino_t root_ino;
	uint32_t block_size;
	Inode* mru_inode;
	Inode* lru_inode;
	Inode* hash_inodes[INODE_HASH_LENGTH];
	bool susp_enabled;
	uint8_t susp_offset;
	int rr_ext;
	bool no_rock;
	bool no_susp;

public:
	Inode* GetInode(iso9660_ino_t inode_id);

};

#endif
