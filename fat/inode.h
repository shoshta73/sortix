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
 * inode.h
 * Filesystem inode.
 */

#ifndef INODE_H
#define INODE_H

class Block;
class Filesystem;

struct position
{
	uint32_t cluster;
	uint8_t sector;
	uint16_t offset;
};

struct free_search
{
	struct position position;
	uint8_t needed;
	uint8_t streak;
	fat_ino_t last_cluster;
};

class Inode
{
public:
	Inode(Filesystem* filesystem, fat_ino_t inode_id);
	~Inode();

public:
	Inode* prev_inode;
	Inode* next_inode;
	Inode* prev_hashed;
	Inode* next_hashed;
	Inode* prev_dirty;
	Inode* next_dirty;
	Inode* parent;
	Block* data_block;
	struct fat_dirent* dirent;
	struct fat_dirent deleted_dirent;
	fat_ino_t first_cluster;
	Filesystem* filesystem;
	size_t reference_count;
	size_t remote_reference_count;
	size_t implied_reference;
	fat_ino_t inode_id;
	fat_off_t cached_cluster_id;
	fat_off_t cached_cluster;
	bool dirty;
	bool deleted;

public:
	void Stat(struct stat* st);
	mode_t Mode();
	uid_t UserId();
	gid_t GroupId();
	fat_off_t Size();
	void UTimens(const struct timespec times[2]);
	bool ChangeMode(mode_t mode);
	bool ChangeOwner(uid_t uid, gid_t gid);
	bool FreeClusters(fat_ino_t cluster_id);
	bool Truncate(uint64_t new_size);
	fat_block_t GetBlockId(fat_ino_t cluster, uint8_t sector);
	Block* GetClusterSector(fat_ino_t cluster, uint8_t sector);
	bool ZeroCluster(fat_ino_t cluster);
	bool Iterate(Block** block_ptr, struct position* position);
	fat_ino_t SeekCluster(fat_off_t cluster_id);
	bool ReadDirectory(Block** block_inout,
	                   struct position* next_position_inout, char* name,
	                   uint8_t* file_type_out, fat_ino_t* inode_id_out,
	                   struct fat_dirent** entry,
	                   struct free_search* free_search,
	                   struct position* position_out,
	                   size_t* entry_length_out);
	Inode* Open(const char* elem, int flags, mode_t mode);
	bool Link(const char* elem, Inode* dest, bool directories);
	bool Symlink(const char* elem, const char* dest);
	bool Unlink(const char* elem, bool directories, bool force=false);
	Inode* UnlinkKeep(const char* elem, bool directories, bool force=false);
	bool RelinkParent(Inode* new_parent);
	ssize_t ReadAt(uint8_t* buffer, size_t count, off_t offset);
	ssize_t WriteAt(const uint8_t* buffer, size_t count, off_t offset);
	bool Rename(Inode* olddir, const char* oldname, const char* newname);
	Inode* CreateDirectory(const char* path, mode_t mode);
	bool RemoveDirectory(const char* path);
	bool IsEmptyDirectory();
	void Refer();
	void Unref();
	void RemoteRefer();
	void RemoteUnref();
	void Sync();
	void BeginWrite();
	void FinishWrite();
	void Modified();
	void Use();
	void Unlink();
	void Prelink();
	void Delete();

};

#endif
