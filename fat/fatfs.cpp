/*
 * Copyright (c) 2013, 2014, 2015, 2016, 2023, 2025 Jonas 'Sortie' Termansen.
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
 * fatfs.cpp
 * The File Allocation Table (FAT) filesystem.
 */

#include <sys/stat.h>
#include <sys/types.h>

#include <assert.h>
#include <dirent.h>
#include <endian.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <locale.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__sortix__)
#include "fsmarshall.h"
#else
#include "fuse.h"
#endif

#include "block.h"
#include "device.h"
#include "fat.h"
#include "fatfs.h"
#include "filesystem.h"
#include "inode.h"
#include "ioleast.h"
#include "util.h"

uid_t request_uid;
uid_t request_gid;

int main(int argc, char* argv[])
{
	setlocale(LC_ALL, "");

	size_t memory;
#ifdef __sortix__
	memstat(NULL, &memory);
#else
	memory = (uintmax_t) sysconf(_SC_PAGE_SIZE) *
	         (uintmax_t) sysconf(_SC_PHYS_PAGES);
#endif
	size_t cache_size = (memory / 10);

	const char* fuse_options = NULL;
	const char* pretend_mount_path = NULL;
	bool foreground = false;
	bool write = true;
	enum
	{
		OPT_FUSE_OPTIONS = 257,
	};
	const struct option longopts[] =
	{
		{"fuse-options", required_argument, NULL, OPT_FUSE_OPTIONS},
		{"background", no_argument, NULL, 'b'},
		{"foreground", no_argument, NULL, 'f'},
		{"pretend-mount-path", required_argument, NULL, 'p'},
		{0, 0, 0, 0}
	};
	const char* opts = "bfo:p:";
	int opt;
	while ( (opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1 )
	{
		switch ( opt )
		{
		case OPT_FUSE_OPTIONS: fuse_options = optarg; break;
		case 'b': foreground = false; break;
		case 'f': foreground = true; break;
		case 'o':
		{
			char* arg = optarg;
			char* save;
			char* tok;
			while ( (tok = strtok_r(arg, ",", &save)) )
			{
				if ( !strcmp(tok, "ro") )
					write = false;
				else if ( !strcmp(tok, "rw") )
					write = true;
				else if ( !strncmp(tok, "cache=", strlen("cache=")) )
				{
					char* end;
					uintmax_t val = strtoumax(tok + strlen("cache="), &end, 10);
					uintmax_t mult;
					if ( !strcmp(end, "%" ) )
						mult = (memory / 100);
					else if ( !strcmp(end, "K" ) )
						mult = 1024ULL << 0;
					else if ( !strcmp(end, "M" ) )
						mult = 1024ULL << 10;
					else if ( !strcmp(end, "G" ) )
						mult = 1024ULL << 20;
					else if ( !strcmp(end, "" ) )
						mult = 1;
					else
						errx(1, "warning: invalid cache size: %s", tok);
					if ( __builtin_mul_overflow(val, mult, &cache_size) )
						errx(1, "warning: invalid cache size: %s", tok);
				}
				else
					warnx("warning: unknown mount option: %s", tok);
				arg = NULL;
			}
			break;
		}
		case 'p': pretend_mount_path = optarg; break;
		default: return 1;
		}
	}

	if ( argc - optind < 1 )
		errx(1, "expected device");
	if ( argc - optind < 2 )
		errx(1, "expected mountpoint");

	const char* device_path = argv[optind + 0];
	const char* mount_path = argv[optind + 1];

	if ( !pretend_mount_path )
		pretend_mount_path = mount_path;

	int fd = open(device_path, write ? O_RDWR : O_RDONLY);
	if ( fd < 0 )
		err(1, "%s", device_path);

	struct stat st;
	fstat(fd, &st);

	// Read the bios parameter block from the filesystem so we can verify it.
	struct fat_bpb bpb;
	if ( preadall(fd, &bpb, sizeof(bpb), 0) != sizeof(bpb) )
	{
		if ( errno == EEOF )
			errx(1, "%s: Isn't a FAT filesystem (too short)", device_path);
		else
			err(1, "read: %s", device_path);
	}

	// Verify the boot signature.
	if ( !(bpb.boot_signature[0] == 0x55 && bpb.boot_signature[1] == 0xAA) )
		errx(1, "%s: Isn't a FAT filesystem (no boot signature)", device_path);
	// Verify the jump instruction at the start of the boot sector.
	if ( !(bpb.jump[0] == 0xEB && bpb.jump[2] == 0x90) &&
	     !(bpb.jump[0] == 0xE9) )
		errx(1, "%s: Isn't a FAT filesystem (bad jump)", device_path);
	// Verify the sector size.
	uint16_t bytes_per_sector =
		bpb.bytes_per_sector_low | bpb.bytes_per_sector_high << 8;
	if ( bytes_per_sector < 512 ||
	     (bytes_per_sector & (bytes_per_sector - 1)) ||
	     4096 < bytes_per_sector )
		errx(1, "%s: Bad number of bytes per sector: %u",
		     device_path, bytes_per_sector);
	// Verify the size of the root directory.
	uint16_t root_dirent_count =
		bpb.root_dirent_count_low | bpb.root_dirent_count_high << 8;
	uint32_t root_dir_sectors =
		divup<uint32_t>(root_dirent_count * sizeof(struct fat_dirent),
		                bytes_per_sector);
	if ( (root_dirent_count * 32) % bytes_per_sector )
		errx(1, "%s: Root directory entry size (%u) is not sector aligned (%u)",
		     device_path, root_dirent_count * 32, bytes_per_sector);
	// Verify the number of sectors per FAT.
	uint32_t sectors_per_fat =
		le16toh(bpb.sectors_per_fat) ? le16toh(bpb.sectors_per_fat) :
	    le32toh(bpb.fat32_sectors_per_fat);
	if ( !sectors_per_fat )
		errx(1, "%s: Bad number of sectors per fat: %u",
		     device_path, sectors_per_fat);
	// Verify the total number of sectors.
	uint32_t total_sectors =
		bpb.total_sectors_low | bpb.total_sectors_high << 8;
	if ( !total_sectors )
		total_sectors = le32toh(bpb.total_sectors_large);
	if ( !total_sectors )
		errx(1, "%s: Bad total number of sectors: %u",
		     device_path, total_sectors);
	if ( st.st_size / bytes_per_sector < total_sectors )
		errx(1, "%s: Device has fewer sectors (%u) than filesystem (%u)",
		     device_path, (uint32_t) (st.st_size / bytes_per_sector),
		     total_sectors);
	// Verify the FAT count is non-zero.
	if ( bpb.fat_count < 1 )
		errx(1, "%s: Bad fat count: %u", device_path, bpb.fat_count);
	// Verify the FAT offset is beyond the BPB.
	if ( !le16toh(bpb.reserved_sectors) )
		errx(1, "%s: Bad reserved sector count: %u",
		     device_path, le16toh(bpb.reserved_sectors));
	// Verify the number of sectors for the FAT.
	uint32_t fat_sectors;
	if ( __builtin_mul_overflow(bpb.fat_count, sectors_per_fat, &fat_sectors) )
		errx(1, "%s: Bad fat size: %u * %u",
		     device_path, bpb.fat_count, sectors_per_fat);
	// Verify the sector where the data begins.
	uint32_t data_offset =
		le16toh(bpb.reserved_sectors) + fat_sectors + root_dir_sectors;
	if ( data_offset > total_sectors )
		errx(1, "%s: Bad data offset (%u) is greater than sector total (%u)",
		     device_path, data_offset, total_sectors);
	uint32_t data_sectors = total_sectors - data_offset;
	// Verify the sectors per cluster is a non-zero power of two.
	if ( bpb.sectors_per_cluster == 0 ||
	     (bpb.sectors_per_cluster & (bpb.sectors_per_cluster - 1)) )
		errx(1, "%s: Bad number of sectors per cluster: %u",
		     device_path, bpb.sectors_per_cluster);
	// Verify the number ofc clusters.
	uint32_t cluster_count = data_sectors / bpb.sectors_per_cluster;
	if ( cluster_count < 1 || 0xFFFFFF7 - 2 <= cluster_count )
		errx(1, "%s: Bad number of clusters: %u", device_path, cluster_count);
	// Determine the FAT type.
	uint8_t fat_type =
		cluster_count < 4085 ? 12 : cluster_count < 65525 ? 16 : 32;
	// Verify the FAT has enough clusters.
	uint64_t fat_cluster_count =
		((uint64_t) sectors_per_fat * bytes_per_sector * 8) / fat_type;
	if ( fat_cluster_count < 2 + cluster_count )
		errx(1, "%s: FAT doesn't have enough clusters: %u < %u",
		     device_path, (uint32_t) fat_cluster_count, 2 + cluster_count);
	// Verify the root directory isn't empty on FAT12/16.
	if ( root_dirent_count < 1 && fat_type < 32 )
		errx(1, "%s: Bad root directory entries: %u",
		     device_path, bytes_per_sector);
	// Verify the filesystem version.
	if ( fat_type == 32 && le16toh(bpb.fat32_version) != 0x0000 )
		errx(1, "%s: Unsupported filesystem version 0x%04x", device_path,
		     le16toh(bpb.fat32_version));
	// Verify the root directory cluster is within bounds.
	if ( fat_type == 32 &&
	     (le32toh(bpb.fat32_root_cluster) < 2 ||
	      2 + cluster_count < le32toh(bpb.fat32_root_cluster)) )
		errx(1, "%s: Bad root directory cluister: %u",
		     device_path, le32toh(bpb.fat32_root_cluster));
	// Verify fsinfo is within bounds.
	if ( fat_type == 32 &&
	     (le16toh(bpb.fat32_fsinfo) < 1 ||
	      le16toh(bpb.reserved_sectors) <= le16toh(bpb.fat32_fsinfo)) )
		errx(1, "%s: Bad fsinfo sector: %u",
		     device_path, le16toh(bpb.fat32_fsinfo));

	size_t block_limit = cache_size / bytes_per_sector;

	// TODO: The FAT and clusters are not aligned to cluster size so
	//       we can't use the cluster size here. Perhaps refactor the
	//       device so we can deal with whole clusters.
	Device* dev = new Device(fd, device_path, bytes_per_sector, block_limit,
	                         write);
	if ( !dev ) // TODO: Use operator new nothrow!
		err(1, "malloc");
	Block* bpb_block = dev->GetBlock(0);
	if ( !bpb_block )
		err(1, "Reading bpb block");
	Filesystem* fs = new Filesystem(dev, pretend_mount_path, bpb_block);
	if ( !fs ) // TODO: Use operator new nothrow!
		err(1, "malloc");
	if ( !fs->WasUnmountedCleanly() )
		warnx("warning: %s: Filesystem wasn't unmounted cleanly", device_path);
	if ( write && !fs->MarkMounted() )
		err(1, "failed to mark filesystem as mounted");
	if ( !(fs->root = fs->CreateInode(fs->root_inode_id, NULL, NULL, NULL)) )
		err(1, "opening /");

	if ( !mount_path )
		return 0;

#if defined(__sortix__)
	(void) fuse_options;
	return fsmarshall_main(argv[0], mount_path, foreground, fs, dev);
#else
	return fat_fuse_main(argv[0], mount_path, fuse_options, foreground, fs,
	                     dev);
#endif
}
