/*
 * Copyright (c) 2013-2016, 2022-2023, 2025 Jonas 'Sortie' Termansen.
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
 * iso9660fs.cpp
 * Implementation of the ISO 9660 filesystem.
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

#include "block.h"
#include "device.h"
#include "filesystem.h"
#include "inode.h"
#include "ioleast.h"
#include "iso9660.h"
#include "iso9660fs.h"
#include "util.h"

uid_t request_uid;
uid_t request_gid;

mode_t HostModeFromFsMode(uint32_t mode)
{
	mode_t hostmode = mode & 07777;
	if ( ISO9660_S_ISSOCK(mode) ) hostmode |= S_IFSOCK;
	if ( ISO9660_S_ISLNK(mode) ) hostmode |= S_IFLNK;
	if ( ISO9660_S_ISREG(mode) ) hostmode |= S_IFREG;
	if ( ISO9660_S_ISBLK(mode) ) hostmode |= S_IFBLK;
	if ( ISO9660_S_ISDIR(mode) ) hostmode |= S_IFDIR;
	if ( ISO9660_S_ISCHR(mode) ) hostmode |= S_IFCHR;
	if ( ISO9660_S_ISFIFO(mode) ) hostmode |= S_IFIFO;
	return hostmode;
}

uint32_t FsModeFromHostMode(mode_t hostmode)
{
	uint32_t mode = hostmode & 07777;
	if ( S_ISSOCK(hostmode) ) mode |= ISO9660_S_IFSOCK;
	if ( S_ISLNK(hostmode) ) mode |= ISO9660_S_IFLNK;
	if ( S_ISREG(hostmode) ) mode |= ISO9660_S_IFREG;
	if ( S_ISBLK(hostmode) ) mode |= ISO9660_S_IFBLK;
	if ( S_ISDIR(hostmode) ) mode |= ISO9660_S_IFDIR;
	if ( S_ISCHR(hostmode) ) mode |= ISO9660_S_IFCHR;
	if ( S_ISFIFO(hostmode) ) mode |= ISO9660_S_IFIFO;
	return mode;
}

uint8_t HostDTFromFsDT(uint8_t fsdt)
{
	switch ( fsdt )
	{
	case ISO9660_FT_UNKNOWN: return DT_UNKNOWN;
	case ISO9660_FT_REG_FILE: return DT_REG;
	case ISO9660_FT_DIR: return DT_DIR;
	case ISO9660_FT_CHRDEV: return DT_CHR;
	case ISO9660_FT_BLKDEV: return DT_BLK;
	case ISO9660_FT_FIFO: return DT_FIFO;
	case ISO9660_FT_SOCK: return DT_SOCK;
	case ISO9660_FT_SYMLINK: return DT_LNK;
	}
	return DT_UNKNOWN;
}

void StatInode(Inode* inode, struct stat* st)
{
	memset(st, 0, sizeof(*st));
	st->st_ino = inode->inode_id;
	st->st_mode = HostModeFromFsMode(inode->Mode());
	st->st_nlink = inode->nlink;
	st->st_uid = inode->uid;
	st->st_gid = inode->gid;
	st->st_size = inode->size;
	st->st_atim = inode->atim;
	st->st_ctim = inode->ctim;
	st->st_mtim = inode->mtim;
	st->st_blksize = inode->filesystem->block_size;
	st->st_blocks = divup(st->st_size, (off_t) 512);
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
	bool no_rock = false;
	bool no_susp = false;
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
					;
				else if ( !strcmp(tok, "rw") )
					errx(1, "-o rw: filesystem is not writable");
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
				else if ( !strcmp(tok, "norock") )
					no_rock = true;
				else if ( !strcmp(tok, "nosusp") )
					no_susp = true;
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

	int fd = open(device_path, O_RDONLY);
	if ( fd < 0 )
		err(1, "%s", device_path);

	struct stat st;
	if ( fstat(fd, &st) < 0 )
		err(1, "stat: %s", device_path);

	blksize_t sector_size = 2048;
	blksize_t logical_sector_size = sector_size < 2048 ? 2048 : sector_size;

	struct iso9660_pvd pvd;
	off_t pvd_offset = (off_t) logical_sector_size * 16;
	if ( preadall(fd, &pvd, sizeof(pvd), pvd_offset) != sizeof(pvd) )
	{
		if ( errno == EEOF )
			errx(1, "Not a valid ISO 9660 filesystem: %s", device_path);
		else
			err(1, "read: %s", device_path);
	}
	if ( memcmp(pvd.standard_identifier, "CD001", 5) != 0 )
		errx(1, "Not a valid ISO 9660 filesystem: %s", device_path);
	if ( pvd.type != TYPE_PRIMARY_VOLUME_DESCRIPTOR )
		errx(1, "Not a valid ISO 9660 filesystem: %s", device_path);
	if ( pvd.version != 1 || pvd.file_structure_version != 1 )
		errx(1, "Unsupported ISO 9660 filesystem version: %s", device_path);
	blksize_t block_size = le32toh(pvd.logical_block_size_le);
	if ( block_size < 512 || logical_sector_size < block_size ||
	     logical_sector_size % block_size )
		errx(1, "Unsupported ISO 9660 block size: %s: %ju", device_path,
		     (uintmax_t) block_size);
	size_t block_limit = cache_size / block_size;

	Device* dev = new Device(fd, device_path, block_size, block_limit);
	if ( !dev )
		err(1, "malloc");
	Filesystem* fs = new Filesystem(dev, pretend_mount_path, &pvd);
	if ( !fs )
		err(1, "%s", device_path);
	fs->no_rock = no_rock;
	fs->no_susp = no_susp;

	uint32_t root_lba;
	uint32_t root_size;
	memcpy(&root_lba, pvd.root_dirent + 2, sizeof(root_lba));
	memcpy(&root_size, pvd.root_dirent + 10, sizeof(root_size));
	fs->root_ino = (iso9660_ino_t) root_lba * (iso9660_ino_t) block_size;

	Inode* root = fs->GetInode(fs->root_ino);
	if ( !root )
		err(1, "GetInode");
	if ( !root->ActivateExtensions() )
		err(1, "ActivateExtensions");
	root->Unref();

	if ( !mount_path )
		return 0;

#if defined(__sortix__)
	(void) fuse_options;
	return fsmarshall_main(argv[0], mount_path, foreground, fs, dev);
#else
	return iso9660_fuse_main(argv[0], mount_path, fuse_options, foreground, fs,
	                         dev);
#endif
}
