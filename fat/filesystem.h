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
 * filesystem.h
 * Filesystem.
 */

#ifndef FILESYSTEM_H
#define FILESYSTEM_H

bool is_8_3(const char* name);
void encode_8_3(const char* decoded, char encoded[8 + 3]);
void decode_8_3(const char encoded[8 + 3], char decoded[8 + 1 + 3 + 1]);
void timespec_to_fat(const struct timespec* ts, uint16_t* date, uint16_t* time,
                     uint8_t* centis);
struct timespec fat_to_timespec(const uint16_t* date, const uint16_t* time,
                                const uint8_t* centis);

class Device;
class Inode;

static const size_t INODE_HASH_LENGTH = 1 << 16;

class Filesystem
{
public:
	Filesystem(Device* device, const char* mount_path, Block* bpb_block);
	~Filesystem();

public:
	Block* bpb_block;
	struct fat_bpb* bpb;
	Device* device;
	const char* mount_path;
	mode_t mode_reg;
	mode_t mode_dir;
	uid_t uid;
	gid_t gid;
	uint32_t block_size;
	uint16_t bytes_per_sector;
	uint16_t root_dirent_count;
	uint32_t sectors_per_fat;
	uint32_t root_inode_id;
	uint32_t total_sectors;
	fat_block_t fat_lba;
	fat_block_t root_lba;
	fat_block_t data_lba;
	uint32_t cluster_count;
	uint32_t cluster_size;
	uint8_t fat_type;
	uint8_t fat_count;
	uint8_t sectors_per_cluster;
	uint32_t eio_cluster;
	uint32_t eof_cluster;
	uint32_t free_count;
	uint32_t free_search;
	Inode* mru_inode;
	Inode* lru_inode;
	Inode* dirty_inode;
	Inode* hash_inodes[INODE_HASH_LENGTH];
	Inode* root;
	bool dirty;
	bool request_check;

public:
	bool WasUnmountedCleanly();
	bool MarkMounted();
	bool MarkUnmounted();
	void RequestCheck();
	void Corrupted();
	Inode* GetInode(fat_ino_t inode_id);
	Inode* CreateInode(fat_ino_t inode_id, Block* dirent_block,
	                   struct fat_dirent* dirent, Inode* parent);
	bool WriteInfo();
	fat_ino_t AllocateCluster();
	void FreeCluster(fat_ino_t cluster);
	fat_ino_t ReadFAT(fat_ino_t cluster);
	bool WriteFAT(fat_ino_t cluster, fat_ino_t value);
	fat_ino_t CalculateFreeCount();
	void BeginWrite();
	void FinishWrite();
	void Sync();

};

#endif
