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
 * extfs.cpp
 * Implementation of the extended filesystem.
 */

#include <sys/stat.h>
#include <sys/types.h>

#include <assert.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__sortix__)
#include "fsmarshall.h"
#else
#include "fuse.h"
#endif

#include "ext-constants.h"
#include "ext-structs.h"

#include "blockgroup.h"
#include "block.h"
#include "device.h"
#include "extfs.h"
#include "filesystem.h"
#include "inode.h"
#include "ioleast.h"

// These must be kept up to date with libmount/ext2.c.
static const uint32_t EXT2_FEATURE_COMPAT_SUPPORTED = 0;
static const uint32_t EXT2_FEATURE_INCOMPAT_SUPPORTED = \
                      EXT2_FEATURE_INCOMPAT_FILETYPE;
static const uint32_t EXT2_FEATURE_RO_COMPAT_SUPPORTED = \
                      EXT2_FEATURE_RO_COMPAT_LARGE_FILE;

uid_t request_uid;
uid_t request_gid;

mode_t HostModeFromExtMode(uint32_t extmode)
{
	mode_t hostmode = extmode & 0777;
	if ( extmode & EXT2_S_ISVTX ) hostmode |= S_ISVTX;
	if ( extmode & EXT2_S_ISGID ) hostmode |= S_ISGID;
	if ( extmode & EXT2_S_ISUID ) hostmode |= S_ISUID;
	if ( EXT2_S_ISSOCK(extmode) ) hostmode |= S_IFSOCK;
	if ( EXT2_S_ISLNK(extmode) ) hostmode |= S_IFLNK;
	if ( EXT2_S_ISREG(extmode) ) hostmode |= S_IFREG;
	if ( EXT2_S_ISBLK(extmode) ) hostmode |= S_IFBLK;
	if ( EXT2_S_ISDIR(extmode) ) hostmode |= S_IFDIR;
	if ( EXT2_S_ISCHR(extmode) ) hostmode |= S_IFCHR;
	if ( EXT2_S_ISFIFO(extmode) ) hostmode |= S_IFIFO;
	return hostmode;
}

uint32_t ExtModeFromHostMode(mode_t hostmode)
{
	uint32_t extmode = hostmode & 0777;
	if ( hostmode & S_ISVTX ) extmode |= EXT2_S_ISVTX;
	if ( hostmode & S_ISGID ) extmode |= EXT2_S_ISGID;
	if ( hostmode & S_ISUID ) extmode |= EXT2_S_ISUID;
	if ( S_ISSOCK(hostmode) ) extmode |= EXT2_S_IFSOCK;
	if ( S_ISLNK(hostmode) ) extmode |= EXT2_S_IFLNK;
	if ( S_ISREG(hostmode) ) extmode |= EXT2_S_IFREG;
	if ( S_ISBLK(hostmode) ) extmode |= EXT2_S_IFBLK;
	if ( S_ISDIR(hostmode) ) extmode |= EXT2_S_IFDIR;
	if ( S_ISCHR(hostmode) ) extmode |= EXT2_S_IFCHR;
	if ( S_ISFIFO(hostmode) ) extmode |= EXT2_S_IFIFO;
	return extmode;
}

uint8_t HostDTFromExtDT(uint8_t extdt)
{
	switch ( extdt )
	{
	case EXT2_FT_UNKNOWN: return DT_UNKNOWN;
	case EXT2_FT_REG_FILE: return DT_REG;
	case EXT2_FT_DIR: return DT_DIR;
	case EXT2_FT_CHRDEV: return DT_CHR;
	case EXT2_FT_BLKDEV: return DT_BLK;
	case EXT2_FT_FIFO: return DT_FIFO;
	case EXT2_FT_SOCK: return DT_SOCK;
	case EXT2_FT_SYMLINK: return DT_LNK;
	}
	return DT_UNKNOWN;
}

void StatInode(Inode* inode, struct stat* st)
{
	memset(st, 0, sizeof(*st));
	st->st_ino = inode->inode_id;
	st->st_mode = HostModeFromExtMode(inode->Mode());
	st->st_nlink = inode->data->i_links_count;
	st->st_uid = inode->UserId();
	st->st_gid = inode->GroupId();
	st->st_size = inode->Size();
	st->st_atim.tv_sec = inode->data->i_atime;
	st->st_atim.tv_nsec = 0;
	st->st_ctim.tv_sec = inode->data->i_ctime;
	st->st_ctim.tv_nsec = 0;
	st->st_mtim.tv_sec = inode->data->i_mtime;
	st->st_mtim.tv_nsec = 0;
	st->st_blksize = inode->filesystem->block_size;
	st->st_blocks = inode->data->i_blocks;
}

int main(int argc, char* argv[])
{
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

	// Read the super block from the filesystem so we can verify it.
	struct ext_superblock sb;
	if ( preadall(fd, &sb, sizeof(sb), 1024) != sizeof(sb) )
	{
		if ( errno == EEOF )
			errx(1, "%s: Isn't a valid extended filesystem", device_path);
		else
			err(1, "read: %s", device_path);
	}

	// Verify the magic value to detect a compatible filesystem.
	if ( sb.s_magic != EXT2_SUPER_MAGIC )
		errx(1, "%s: Isn't a valid extended filesystem", device_path);

	// Test whether this revision of the extended filesystem is supported.
	if ( sb.s_rev_level == EXT2_GOOD_OLD_REV )
		errx(1, "%s: Is formatted with an obsolete filesystem revision",
		     device_path);

	// Verify that no incompatible features are in use.
	if ( sb.s_feature_compat & ~EXT2_FEATURE_INCOMPAT_SUPPORTED )
		errx(1, "%s: Uses unsupported and incompatible features", device_path);

	if ( write && sb.s_feature_ro_compat & ~EXT2_FEATURE_RO_COMPAT_SUPPORTED )
	{
		warnx("warning: %s: Uses unsupported and incompatible features, "
		      "falling back to read-only access", device_path);
		// TODO: Modify the file descriptor such that writing fails!
		write = false;
	}

	// Check whether any features are in use that we can safely disregard.
	if ( sb.s_feature_compat & ~EXT2_FEATURE_COMPAT_SUPPORTED )
		warnx("%s: Filesystem uses unsupported but compatible features",
		      device_path);

	// Check the block size is sane. 64 KiB may have issues, 32 KiB then.
	if ( sb.s_log_block_size > (15-10) /* 32 KiB blocks */ )
		errx(1, "%s: Filesystem has excess block size", device_path);

	// Check whether the filesystem was unmounted cleanly.
	if ( sb.s_state != EXT2_VALID_FS )
		warnx("warning: %s: Filesystem wasn't unmounted cleanly", device_path);

	uint32_t block_size = 1024U << sb.s_log_block_size;
	size_t block_limit = cache_size / block_size;

	Device* dev = new Device(fd, device_path, block_size, block_limit, write);
	if ( !dev ) // TODO: Use operator new nothrow!
		err(1, "malloc");
	Filesystem* fs = new Filesystem(dev, pretend_mount_path);
	if ( !fs ) // TODO: Use operator new nothrow!
		err(1, "malloc");

	fs->block_groups = new BlockGroup*[fs->num_groups];
	if ( !fs->block_groups ) // TODO: Use operator new nothrow!
		err(1, "malloc");
	for ( size_t i = 0; i < fs->num_groups; i++ )
		fs->block_groups[i] = NULL;

	if ( !mount_path )
		return 0;

#if defined(__sortix__)
	(void) fuse_options;
	return fsmarshall_main(argv[0], mount_path, foreground, fs, dev);
#else
	return ext2_fuse_main(argv[0], mount_path, fuse_options, foreground, fs,
	                      dev);
#endif
}
