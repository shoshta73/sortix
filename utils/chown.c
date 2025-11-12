/*
 * Copyright (c) 2020, 2025 Jonas 'Sortie' Termansen.
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
 * chown.c
 * Change file ownership and group.
 */

#include <sys/stat.h>

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const int FLAG_CHANGES = 1 << 0;
static const int FLAG_VERBOSE = 1 << 1;
static const int FLAG_RECURSIVE = 1 << 2;
static const int FLAG_NOFOLLOW = 1 << 3;

enum symderef
{
	SYMDEREF_NONE,
	SYMDEREF_ARGUMENTS,
	SYMDEREF_ALWAYS,
	SYMDEREF_DEFAULT,
};

static bool do_chown_directory(int fd,
                               const char* path,
                               const char* ownerspec,
                               uid_t uid,
                               const char* groupspec,
                               gid_t gid,
                               int flags,
                               enum symderef symderef);

static bool do_chown(int dirfd,
                     const char* relpath,
                     const char* path,
                     const char* ownerspec,
                     uid_t uid,
                     const char* groupspec,
                     gid_t gid,
                     int flags,
                     enum symderef symderef)
{
	bool success = true;

	int fd_open_flags = O_RDONLY;
	if ( symderef == SYMDEREF_NONE )
		fd_open_flags |= O_SYMLINK_NOFOLLOW;
	int fd = openat(dirfd, relpath, fd_open_flags);
	if ( fd < 0 )
	{
		warn("%s", path);
		return false;
	}

	struct stat st;
	if ( fstat(fd, &st) < 0 )
	{
		warn("stat: %s", path);
		close(fd);
		return false;
	}

	if ( S_ISLNK(st.st_mode) )
	{
		close(fd);
		return true;
	}

	if ( fchown(fd, uid, gid) < 0 )
	{
		if ( uid == (uid_t) -1 )
			warn("changing group to %s: %s", groupspec, path);
		else if ( gid == (gid_t) -1 )
			warn("changing owner to %s: %s", ownerspec, path);
		else
			warn("changing owner to %s and group to %s: %s", ownerspec,
			     groupspec, path);
		success = false;
	}
	else
	{
		if ( uid != (uid_t) -1 &&
			 ((flags & FLAG_VERBOSE) ||
			  (st.st_uid != uid && (flags & FLAG_CHANGES)) ))
		{
			if ( st.st_uid == uid )
				printf("owner of `%s' retained as %s\n", path, ownerspec);
			else
			{
				struct passwd* pwd = getpwuid(st.st_uid);
				if ( pwd )
					printf("owner of `%s' changed from %s to %s\n",
						   path, pwd->pw_name, ownerspec);
				else
					printf("owner of `%s' changed from %ju to %s\n",
						   path, (uintmax_t) st.st_uid, ownerspec);
			}
		}
		if ( gid != (gid_t) -1 &&
			 ((flags & FLAG_VERBOSE) ||
			  (st.st_gid != gid && (flags & FLAG_CHANGES)) ))
		{
			if ( st.st_gid == gid )
				printf("group of `%s' retained as %s\n",
					   path, groupspec);
			else
			{
				struct group* grp = getgrgid(st.st_gid);
				if ( grp )
					printf("group of `%s' changed from %s to %s\n",
						   path, grp->gr_name, groupspec);
				else
					printf("group of `%s' changed from %ju to %s\n",
						   path, (uintmax_t) st.st_gid, groupspec);
			}
		}
	}

	if ( S_ISDIR(st.st_mode) && (flags & FLAG_RECURSIVE) )
	{
		if ( !do_chown_directory(fd, path, ownerspec, uid, groupspec, gid,
		                         flags, symderef) )
			success = false;
	}

	close(fd);
	return success;
}

static bool do_chown_directory(int fd,
                               const char* path,
                               const char* ownerspec,
                               uid_t uid,
                               const char* groupspec,
                               gid_t gid,
                               int flags,
                               enum symderef symderef)
{
	if ( symderef == SYMDEREF_ARGUMENTS )
		symderef = SYMDEREF_NONE;

	int fd_copy = dup(fd);
	if ( fd_copy < 0 )
	{
		warn("dup: %s", path);
		return false;
	}

	DIR* dir = fdopendir(fd_copy);
	if ( !dir )
	{
		warn("fdopendir: %s", path);
		close(fd_copy);
		return false;
	}

	const char* joiner = "/";
	size_t path_length = strlen(path);
	if ( path_length && path[path_length - 1] == '/' )
		joiner = "";

	bool success = true;
	struct dirent* entry;
	while ( (errno = 0, entry = readdir(dir)) )
	{
		if ( !strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") )
			continue;

		char* entry_path;
		if ( asprintf(&entry_path, "%s%s%s", path, joiner, entry->d_name) < 0 )
		{
			warn("asprintf: `%s%s%s'", path, joiner, entry->d_name);
			success = false;
			continue;
		}

		if ( !do_chown(fd, entry->d_name, entry_path, ownerspec, uid, groupspec,
		               gid, flags, symderef) )
			success = false;

		free(entry_path);
	}

	if ( errno != 0 )
	{
		warn("reading directory: %s", path);
		closedir(dir);
		return false;
	}

	closedir(dir);
	return success;
}

static void compact_arguments(int* argc, char*** argv)
{
	for ( int i = 0; i < *argc; i++ )
	{
		while ( i < *argc && !(*argv)[i] )
		{
			for ( int n = i; n < *argc; n++ )
				(*argv)[n] = (*argv)[n+1];
			(*argc)--;
		}
	}
}

int main(int argc, char* argv[])
{
	int flags = 0;
	enum symderef symderef = SYMDEREF_DEFAULT;

	for ( int i = 1; i < argc; i++ )
	{
		const char* arg = argv[i];
		if ( arg[0] != '-' || !arg[1] )
			continue;
		argv[i] = NULL;
		if ( !strcmp(arg, "--") )
			break;
		if ( arg[1] != '-' )
		{
			char c;
			while ( (c = *++arg) ) switch ( c )
			{
			case 'c': flags |= FLAG_CHANGES; break;
			case 'h': flags |= FLAG_NOFOLLOW; break;
			case 'H': symderef = SYMDEREF_ARGUMENTS; break;
			case 'L': symderef = SYMDEREF_ALWAYS; break;
			case 'P': symderef = SYMDEREF_NONE; break;
			case 'R': flags |= FLAG_RECURSIVE; break;
			case 'v': flags |= FLAG_VERBOSE; break;
			default:
				errx(1, "unknown option -- '%c'", c);
			}
		}
		else if ( !strcmp(arg, "--changes") )
			flags |= FLAG_CHANGES;
		else if ( !strcmp(arg, "--verbose") )
			flags |= FLAG_VERBOSE;
		else if ( !strcmp(arg, "--recursive") )
			flags |= FLAG_RECURSIVE;
		else
			errx(1, "unknown option: %s", arg);
	}

	compact_arguments(&argc, &argv);

	if ( (flags & FLAG_RECURSIVE) && (flags & FLAG_NOFOLLOW) )
		errx(1, "the -R and -h options are mutually incompatible");

	if ( flags & FLAG_RECURSIVE )
	{
		if ( symderef == SYMDEREF_DEFAULT )
			symderef = SYMDEREF_NONE;
	}
	else
		symderef = flags & FLAG_NOFOLLOW ? SYMDEREF_NONE : SYMDEREF_ALWAYS;

	if ( argc == 1 )
		errx(1, "missing operand");

	char* spec = argv[1];
	uid_t uid = (uid_t) -1;
	gid_t gid = (gid_t) -1;
#ifdef CHGRP
	const char* ownerspec = "";
	const char* groupspec = spec;
#else
	size_t offset = strcspn(spec, ":");
	const char* ownerspec = spec;
	const char* groupspec = spec[offset] ? spec + offset + 1 : "";
	spec[offset] = '\0';
#endif
	char* end;
	if ( ownerspec[0] )
	{
		errno = 0;
		uid = strtoumax(ownerspec, &end, 10);
		if ( errno || *end )
		{
			struct passwd* pwd = getpwnam(ownerspec);
			if ( !pwd )
				err(1, "no such user: %s", ownerspec);
			uid = pwd->pw_uid;
		}
	}
	if ( groupspec[0] )
	{
		errno = 0;
		gid = strtoumax(groupspec, &end, 10);
		if ( errno || *end )
		{
			struct group* grp = getgrnam(groupspec);
			if ( !grp )
				err(1, "no such user: %s", groupspec);
			gid = grp->gr_gid;
		}
	}

	if ( uid == (uid_t) -1 && gid == (gid_t) -1 )
		errx(1, "a new owner and/or group must be specified");

	if ( argc == 2 )
		errx(1, "missing operand after `%s'", spec);

	bool success = true;
	for ( int i = 2; i < argc; i++ )
	{
		const char* arg = argv[i];
		if ( !do_chown(AT_FDCWD, arg, arg, ownerspec, uid, groupspec, gid,
		               flags, symderef) )
			success = false;
	}

	return success ? 0 : 1;
}
