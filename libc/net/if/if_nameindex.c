/*
 * Copyright (c) 2016, 2017 Jonas 'Sortie' Termansen.
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
 * net/if/if_nameindex.c
 * Build list of network interfaces.
 */

#include <sys/ioctl.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// TODO: Consider turning this into a system call to avoid a number of error
//       cases and work properly in chroots.
struct if_nameindex* if_nameindex(void)
{
	DIR* dir = opendir("/dev");
	if ( !dir )
		return NULL;
	struct if_nameindex* ifs = (struct if_nameindex*)
		reallocarray(NULL, sizeof(struct if_nameindex), 2);
	if ( !ifs )
		return closedir(dir), (struct if_nameindex*) NULL;
	size_t ifs_count = 0;
	size_t ifs_allocated = 2;
	ifs[ifs_count].if_index = 0;
	ifs[ifs_count].if_name = NULL;
	struct dirent* entry;
	while ( (errno = 0, entry = readdir(dir)) )
	{
		if ( entry->d_type != DT_UNKNOWN && entry->d_type != DT_CHR )
			continue;
		int fd = openat(dirfd(dir), entry->d_name, O_RDWR | O_NOFOLLOW);
		if ( fd < 0 )
		{
			int errnum = errno;
			if ( entry->d_type == DT_UNKNOWN )
			{
				struct stat st;
				if ( fstatat(dirfd(dir), entry->d_name, &st,
				             AT_SYMLINK_NOFOLLOW) < 0 )
				{
					closedir(dir);
					if_freenameindex(ifs);
					return NULL;
				}
				if ( !S_ISCHR(st.st_mode) )
					continue;
			}
			if ( errnum == ELOOP || errnum == EACCES || errnum == EPERM )
				continue;
			closedir(dir);
			if_freenameindex(ifs);
			return NULL;
		}
		int type;
		struct if_info info;
		if ( (type = ioctl(fd, IOCGETTYPE)) < 0 ||
		     IOC_TYPE(type) != IOC_TYPE_NETWORK_INTERFACE ||
		     ioctl(fd, NIOC_GETINFO, &info) < 0 )
		{
			close(fd);
			continue;
		}
		close(fd);
		if ( ifs_count + 1 == ifs_allocated )
		{
			size_t new_allocated = 2 * ifs_allocated;
			struct if_nameindex* new_ifs = (struct if_nameindex*)
				reallocarray(ifs, sizeof(struct if_nameindex), new_allocated);
			if ( !new_ifs )
			{
				closedir(dir);
				if_freenameindex(ifs);
				return NULL;
			}
			ifs = new_ifs;
			ifs_allocated = new_allocated;
		}
		char* name = strdup(entry->d_name);
		if ( !name )
		{
			closedir(dir);
			if_freenameindex(ifs);
			return NULL;
		}
		ifs[ifs_count].if_index = info.linkid;
		ifs[ifs_count].if_name = name;
		ifs_count++;
		ifs[ifs_count].if_index = 0;
		ifs[ifs_count].if_name = NULL;
	}
	if ( errno )
	{
		closedir(dir);
		if_freenameindex(ifs);
		return NULL;
	}
	closedir(dir);
	return ifs;
}
