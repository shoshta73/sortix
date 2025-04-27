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
 * inode.h
 * Filesystem inode.
 */

#ifndef INODE_H
#define INODE_H

#include "iso9660.h"

class Block;
class Filesystem;

class Inode
{
public:
	Inode(Filesystem* filesystem, iso9660_ino_t inode_id);
	~Inode();

public:
	Inode* prev_inode;
	Inode* next_inode;
	Inode* prev_hashed;
	Inode* next_hashed;
	Block* data_block;
	struct iso9660_dirent* data;
	Filesystem* filesystem;
	size_t reference_count;
	size_t remote_reference_count;
	iso9660_ino_t inode_id;
	uint32_t uid;
	uint32_t gid;
	uint64_t size;
	uint32_t mode;
	uint32_t nlink;
	struct timespec mtim;
	struct timespec atim;
	struct timespec ctim;

public:
	bool ActivateExtensions();
	bool Parse();
	uint32_t Mode();
	uint32_t UserId();
	uint32_t GroupId();
	uint64_t Size();
	Block* GetBlock(uint32_t offset);
	bool ReadDirectory(uint64_t* offset_inout, Block** block_inout,
	                   uint64_t* block_id_inout, char* name,
	                   uint8_t* file_type_out, iso9660_ino_t* inode_id_out);
	Inode* Open(const char* elem, int flags, mode_t mode);
	bool Link(const char* elem, Inode* dest);
	bool Unlink(const char* elem, bool directories, bool force=false);
	Inode* UnlinkKeep(const char* elem, bool directories, bool force=false);
	ssize_t ReadLink(uint8_t* buf, size_t bufsize);
	ssize_t ReadAt(uint8_t* buffer, size_t count, off_t offset);
	bool Rename(Inode* olddir, const char* oldname, const char* newname);
	bool RemoveDirectory(const char* path);
	void Refer();
	void Unref();
	void RemoteRefer();
	void RemoteUnref();
	void Use();
	void Unlink();
	void Prelink();

};

#endif
