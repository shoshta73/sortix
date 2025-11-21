/*
 * Copyright (c) 2011, 2012, 2013, 2014, 2016, 2025 Jonas 'Sortie' Termansen.
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
 * cp.c
 * Copy files and directories.
 */

#include <sys/stat.h>
#include <sys/types.h>

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#ifdef INSTALL
#include <grp.h>
#include <inttypes.h>
#include <libgen.h>
#include <pwd.h>
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum symbolic_dereference
{
	SYMBOLIC_DEREFERENCE_NONE,
	SYMBOLIC_DEREFERENCE_ARGUMENTS,
	SYMBOLIC_DEREFERENCE_ALWAYS,
	SYMBOLIC_DEREFERENCE_DEFAULT,
};

static const int FLAG_RECURSIVE = 1 << 0;
static const int FLAG_VERBOSE = 1 << 1;
static const int FLAG_TARGET_DIR = 1 << 2;
static const int FLAG_NO_TARGET_DIR = 1 << 3;
static const int FLAG_UPDATE = 1 << 4;
static const int FLAG_FORCE = 1 << 5;
#ifdef INSTALL
static const int FLAG_MKDIR = 1 << 31;

static mode_t old_umask;
#endif

#ifdef INSTALL
int mkdir_p(const char* path, mode_t mode)
{
	int saved_errno = errno;
	if ( !mkdir(path, mode) )
		return 0;
	if ( errno == ENOENT )
	{
		char* prev = strdup(path);
		if ( !prev )
			return -1;
		int status =  mkdir_p(dirname(prev), mode | 0500);
		free(prev);
		if ( status < 0 )
			return -1;
		errno = saved_errno;
		if ( !mkdir(path, mode) )
			return 0;
	}
	if ( errno == EEXIST )
		return errno = saved_errno, 0;
	return -1;
}

static bool is_octal_string(const char* str)
{
	if ( !str[0] )
		return false;
	for ( size_t i = 0; str[i]; i++ )
		if ( !('0' <= str[i] && str[i] <= '7') )
			return false;
	return true;
}

static mode_t execute_modespec(const char* str,
                               mode_t mode,
                               mode_t type,
                               mode_t umask)
{
	if ( is_octal_string(str) )
	{
		errno = 0;
		uintmax_t input = strtoumax((char*) str, NULL, 8);
		if ( errno == ERANGE )
			return (mode_t) -1;
		if ( input & ~((uintmax_t) 07777) )
			return (mode_t) -1;
		return (mode_t) input;
	}

	size_t index = 0;
	do
	{
		mode_t who_mask = 01000;
		while ( true )
		{
			if ( str[index] == 'u' && (index++, true) )
				who_mask |= 04700;
			else if ( str[index] == 'g' && (index++, true) )
				who_mask |= 02070;
			else if ( str[index] == 'o' && (index++, true) )
				who_mask |= 00007;
			else if ( str[index] == 'a' && (index++, true) )
				who_mask |= 06777;
			else
				break;
		}
		if ( !(who_mask & 0777) )
			who_mask |= 06777 & ~umask;
		do
		{
			char op;
			switch ( (op = str[index++]) )
			{
			case '+': break;
			case '-': break;
			case '=': break;
			default: return (mode_t) -1;
			};
			mode_t operand = 0;
			if ( str[index] == 'u' || str[index] == 'g' || str[index] == 'o' )
			{
				char permcopy = str[index++];
				switch ( permcopy )
				{
				case 'u': operand = mode >> 6 & 07; break;
				case 'g': operand = mode >> 3 & 07; break;
				case 'o': operand = mode >> 0 & 07; break;
				default: __builtin_unreachable();
				};
				operand = operand << 0 | operand << 3 | operand << 6;
				switch ( permcopy )
				{
				case 'u': if ( mode & 04000) operand |= 06000; break;
				case 'g': if ( mode & 02000) operand |= 06000; break;
				};
				who_mask &= ~((mode_t) 01000);
			}
			else
			{
				bool unknown = false;
				do
				{
					switch ( str[index] )
					{
					case 'r': operand |= 00444; break;
					case 'w': operand |= 00222; break;
					case 'x': operand |= 00111; break;
					case 'X':
						if ( S_ISDIR(type) || (mode & 0111) )
							operand |= 00111;
						break;
					case 's': operand |= 06000; break;
					case 't': operand |= 00000; break;
					default: unknown = true; break;
					}
				} while ( !unknown && (index++, true) );
			}
			switch ( op )
			{
			case '+': mode |= (operand & who_mask); break;
			case '-': mode &= ~(operand & who_mask); break;
			case '=': mode = (mode & ~who_mask) | (operand & who_mask); break;
			default: __builtin_unreachable();
			}
		} while ( str[index] == '+' ||
		          str[index] == '-' ||
		          str[index] == '=' );
	} while ( str[index] == ',' && (index++, true) );
	if ( str[index] )
		return (mode_t) -1;
	return mode;
}

static bool is_valid_modespec(const char* str)
{
	return execute_modespec(str, 0, 0, 0) != (mode_t) -1;
}
#endif

static char* join_paths(const char* a, const char* b)
{
	size_t a_len = strlen(a);
	bool has_slash = (a_len && a[a_len-1] == '/') || b[0] == '/';
	char* result;
	if ( (has_slash && asprintf(&result, "%s%s", a, b) < 0) ||
	     (!has_slash && asprintf(&result, "%s/%s", a, b) < 0) )
		return NULL;
	return result;
}

static bool cp_contents(int srcfd, const char* srcpath,
                        int dstfd, const char* dstpath, int flags)
{
	struct stat srcst, dstst;
	if ( fstat(srcfd, &srcst) )
	{
		warn("stat: %s", srcpath);
		return false;
	}
	if ( fstat(dstfd, &dstst) )
	{
		warn("stat: %s", dstpath);
		return false;
	}
	if ( srcst.st_dev == dstst.st_dev && srcst.st_ino == dstst.st_ino )
	{
		warnx("`%s' and `%s' are the same file", srcpath, dstpath);
		return false;
	}
	if ( S_ISDIR(dstst.st_mode) )
	{
		warnx("cannot overwrite directory `%s' with non-directory", dstpath);
		return false;
	}
	if ( lseek(srcfd, 0, SEEK_SET) )
	{
		warn("can't seek: %s", srcpath);
		return false;
	}
	if ( lseek(dstfd, 0, SEEK_SET) )
	{
		warn("can't seek: %s", dstpath);
		return false;
	}
	if ( flags & FLAG_VERBOSE )
		printf("`%s' -> `%s'\n", srcpath, dstpath);
	if ( ftruncate(dstfd, srcst.st_size) < 0 )
	{
		warn("truncate: %s", dstpath);
		return false;
	}
	static unsigned char buffer[64 * 1024];
	while ( true )
	{
		ssize_t numbytes = read(srcfd, buffer, sizeof(buffer));
		if ( numbytes == 0 )
			break;
		if ( numbytes < 0 )
		{
			warn("read: %s", srcpath);
			return false;
		}
		size_t sofar = 0;
		while ( sofar < (size_t) numbytes )
		{
			size_t left = numbytes - sofar;
			ssize_t amount = write(dstfd, buffer + sofar, left);
			if ( amount <= 0 )
			{
				warn("write: %s", dstpath);
				return false;
			}
			sofar += amount;
		}
	}
	return true;
}

static bool cp(int srcdirfd, const char* srcrel, const char* srcpath,
               int dstdirfd, const char* dstrel, const char* dstpath,
               int flags, enum symbolic_dereference symbolic_dereference,
               const char* modespec, uid_t uid, gid_t gid)
{
	struct stat srcst;
	int deref_flags = O_RDONLY;
	if ( symbolic_dereference == SYMBOLIC_DEREFERENCE_NONE )
		deref_flags |= O_NOFOLLOW;
	int srcfd = openat(srcdirfd, srcrel, O_RDONLY | deref_flags);
	if ( srcfd < 0 &&
	     symbolic_dereference == SYMBOLIC_DEREFERENCE_NONE &&
	     errno == ELOOP )
	{
		// TODO: How to handle the update flag in this case?
		struct stat srcst;
		if ( fstatat(srcdirfd, srcrel, &srcst, AT_SYMLINK_NOFOLLOW) < 0 )
		{
			warn("%s", srcpath);
			return false;
		}
		if ( (uintmax_t) SIZE_MAX <= (uintmax_t) srcst.st_size )
		{
			errno = EOVERFLOW;
			warn("%s", srcpath);
			return false;
		}
		size_t size = (size_t) srcst.st_size + 1;
		char* content = (char*) malloc(size);
		if ( !content )
		{
			warn("malloc: %s", srcpath);
			return false;
		}
		ssize_t amount = readlinkat(srcdirfd, srcrel, content, size);
		if ( amount < 0 )
		{
			warn("readlink: %s", srcpath);
			free(content);
			return false;
		}
		if ( size <= (size_t) amount )
		{
			errno = EOVERFLOW;
			warn("readlink: %s", srcpath);
			free(content);
			return false;
		}
		content[amount] = '\0';
		int ret = symlinkat(content, dstdirfd, dstrel);
		if ( ret < 0 && errno == EEXIST )
		{
			if ( unlinkat(dstdirfd, dstrel, 0) == 0 )
			{
				if ( flags & FLAG_VERBOSE )
					printf("removed `%s'\n", dstpath);
				ret = symlinkat(content, dstdirfd, dstrel);
			}
			else if ( errno == EISDIR )
			{
				warnx("cannot overwrite directory `%s' with non-directory",
	                  dstpath);
				free(content);
				return false;
			}
			else
			{
				warn("unlink: %s", dstpath);
				free(content);
				return false;
			}
		}
		if ( ret < 0 )
		{
			warn("symlink: %s", dstpath);
			free(content);
			return false;
		}
		free(content);
		return true;
	}
	if ( srcfd < 0 )
	{
		warn("%s", srcpath);
		return false;
	}
	if ( fstat(srcfd, &srcst) )
	{
		warn("stat: %s", srcpath);
		return close(srcfd), false;
	}
	if ( symbolic_dereference == SYMBOLIC_DEREFERENCE_ARGUMENTS )
		symbolic_dereference = SYMBOLIC_DEREFERENCE_NONE;
	if ( S_ISDIR(srcst.st_mode) )
	{
		if ( !(flags & FLAG_RECURSIVE) )
		{
			warnx("omitting directory `%s'", srcpath);
			return close(srcfd), false;
		}
		int dstfd = openat(dstdirfd, dstrel, O_RDONLY | O_DIRECTORY);
		if ( dstfd < 0 )
		{
			if ( errno != ENOENT )
			{
				warn("%s", dstpath);
				return close(srcfd), false;
			}
#ifdef INSTALL
			mode_t omode = execute_modespec(modespec, srcst.st_mode,
			                                srcst.st_mode, old_umask);
#else
			(void) modespec;
			mode_t omode = srcst.st_mode & 03777;
#endif
			int ret = mkdirat(dstdirfd, dstrel, omode);
			if ( ret )
			{
				warn("cannot create directory `%s'", dstpath);
				return close(srcfd), false;
			}
			int dstfdflags = O_RDONLY | O_DIRECTORY;
			if ( (dstfd = openat(dstdirfd, dstrel, dstfdflags)) < 0 )
			{
				warn("%s", dstpath);
				return close(srcfd), false;
			}
#ifdef INSTALL
			if ( (uid != (uid_t) -1 || gid != (gid_t) -1) &&
			     fchown(dstfd, uid, gid) < 0 )
			{
				warn("chown: %s", dstpath);
				return close(srcfd), false;
			}
#else
			(void) uid;
			(void) gid;
#endif
		}
		struct stat srcst, dstst;
		if ( fstat(srcfd, &srcst) < 0 )
		{
			warn("stat: %s", srcpath);
			return close(dstfd), close(srcfd), false;
		}
		if ( fstat(dstfd, &dstst) < 0 )
		{
			warn("stat: %s", dstpath);
			return close(dstfd), close(srcfd), false;
		}
		if ( srcst.st_dev == dstst.st_dev && srcst.st_ino == dstst.st_ino )
		{
			warnx("error: `%s' and `%s' are the same file", srcpath, dstpath);
			return close(dstfd), close(srcfd), false;
		}
		DIR* srcdir = fdopendir(srcfd);
		if ( !srcdir )
			return warn("fdopendir: %s", srcpath), false;
		if ( flags & FLAG_VERBOSE )
			printf("`%s' -> `%s'\n", srcpath, dstpath);
		bool ret = true;
		struct dirent* entry;
		while ( (errno = 0, entry = readdir(srcdir)) )
		{
			const char* name = entry->d_name;
			if ( !strcmp(name, ".") || !strcmp(name, "..") )
				continue;
			char* srcpath_new = join_paths(srcpath, name);
			if ( !srcpath_new )
				err(1, "malloc");
			char* dstpath_new = join_paths(dstpath, name);
			if ( !dstpath_new )
				err(1, "malloc");
			bool ok = cp(dirfd(srcdir), name, srcpath_new,
			             dstfd, name, dstpath_new,
			             flags, symbolic_dereference, modespec, uid, gid);
			free(srcpath_new);
			free(dstpath_new);
			ret = ret && ok;
		}
		if ( errno )
		{
			warn("readdir: %s", srcpath);
			ret = false;
		}
		close(dstfd);
		closedir(srcdir);
		return ret;
	}
	else
	{
		if ( flags & FLAG_UPDATE )
		{
			struct stat dstst;
			if ( fstatat(dstdirfd, dstrel, &dstst, 0) == 0 &&
			     S_ISREG(dstst.st_mode) )
			{
				if ( srcst.st_mtim.tv_sec < dstst.st_mtim.tv_sec ||
				     (srcst.st_mtim.tv_sec == dstst.st_mtim.tv_sec &&
				      srcst.st_mtim.tv_nsec <= dstst.st_mtim.tv_nsec) )
					return close(srcfd), true;
			}
		}

		int oflags = O_WRONLY | O_CREAT;
#ifdef INSTALL
		mode_t omode = execute_modespec(modespec, srcst.st_mode,
			                            srcst.st_mode, old_umask);
#else
		(void) modespec;
		mode_t omode = srcst.st_mode & 03777;
#endif
		int dstfd = openat(dstdirfd, dstrel, oflags, omode);
		if ( dstfd < 0 &&
		     flags & FLAG_FORCE &&
		     faccessat(dstdirfd, dstrel, F_OK, AT_SYMLINK_NOFOLLOW) == 0 )
		{
			if ( unlinkat(dstdirfd, dstrel, 0) < 0 )
			{
				warn("%s", dstpath);
				return close(srcfd), false;
			}
			dstfd = openat(dstdirfd, dstrel, oflags, omode);
		}
		if ( dstfd < 0 )
		{
			warn("%s", dstpath);
			return close(srcfd), false;
		}
#ifdef INSTALL
		if ( (uid != (uid_t) -1 || uid != (uid_t) -1) &&
		     fchown(dstfd, uid, gid) < 0 )
		{
			warn("chown: %s", dstpath);
			return close(srcfd), false;
		}
#endif
		bool ret = cp_contents(srcfd, srcpath, dstfd, dstpath, flags);
		close(dstfd);
		close(srcfd);
		return ret;
	}
}

static
bool cp_directory(int srcdirfd, const char* srcrel, const char* srcpath,
                  int dstdirfd, const char* dstrel, const char* dstpath,
                  int flags, enum symbolic_dereference symbolic_dereference,
                  const char* modespec, uid_t uid, gid_t gid)
{
	size_t srcrel_last_slash = strlen(srcrel);
	while ( srcrel_last_slash && srcrel[srcrel_last_slash-1] != '/' )
		srcrel_last_slash--;
	const char* src_basename = srcrel + srcrel_last_slash;
	int dstfd = openat(dstdirfd, dstrel, O_RDONLY | O_DIRECTORY);
	if ( dstfd < 0 )
	{
		warn("%s", dstpath);
		return false;
	}
	char* dstpath_new = join_paths(dstpath, src_basename);
	if ( !dstpath_new )
		err(1, "malloc");
	bool ret = cp(srcdirfd, srcrel, srcpath,
	              dstfd, src_basename, dstpath_new,
	              flags, symbolic_dereference, modespec, uid, gid);
	free(dstpath_new);
	return ret;
}

static
bool cp_ambigious(int srcdirfd, const char* srcrel, const char* srcpath,
                  int dstdirfd, const char* dstrel, const char* dstpath,
                  int flags, enum symbolic_dereference symbolic_dereference,
                  const char* modespec, uid_t uid, gid_t gid)
{
	struct stat dstst;
	if ( fstatat(dstdirfd, dstrel, &dstst, 0) < 0 )
	{
		if ( errno != ENOENT )
		{
			warn("%s", dstpath);
			return false;
		}
		dstst.st_mode = S_IFREG;
	}
	if ( S_ISDIR(dstst.st_mode) )
		return cp_directory(srcdirfd, srcrel, srcpath,
		                    dstdirfd, dstrel, dstpath,
		                    flags, symbolic_dereference, modespec, uid, gid);
	else
		return cp(srcdirfd, srcrel, srcpath,
		          dstdirfd, dstrel, dstpath,
		          flags, symbolic_dereference, modespec, uid, gid);
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
#ifdef INSTALL
	const char* groupspec = "";
	const char* modespec = "0755";
	const char* ownerspec = "";
#else
	const char* modespec = NULL;
#endif

	int flags = 0;
	const char* target_directory = NULL;
	const char* preserve_list = NULL;
	enum symbolic_dereference symbolic_dereference = SYMBOLIC_DEREFERENCE_DEFAULT;
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
#ifdef INSTALL
			case 'b': /* ignored */ break;
			case 'c': /* ignored */ break;
			case 'C': /* ignored */ break;
			case 'd': flags |= FLAG_MKDIR; break;
			case 'g':
				if ( *(arg + 1) )
					groupspec = arg + 1;
				else if ( i + 1 == argc )
					errx(1, "option requires an argument -- '%c'", c);
				else
				{
					groupspec = argv[i+1];
					argv[++i] = NULL;
				}
				arg = "g";
				break;
			case 'm':
				if ( *(arg + 1) )
					modespec = arg + 1;
				else if ( i + 1 == argc )
					errx(1, "option requires an argument -- '%c'", c);
				else
				{
					modespec = argv[i+1];
					argv[++i] = NULL;
				}
				arg = "m";
				break;
			case 'o':
				if ( *(arg + 1) )
					ownerspec = arg + 1;
				else if ( i + 1 == argc )
					errx(1, "option requires an argument -- '%c'", c);
				else
				{
					ownerspec = argv[i+1];
					argv[++i] = NULL;
				}
				arg = "o";
				break;
			case 's': /* ignored */ break;
#endif
			case 'f': flags |= FLAG_FORCE; break;
			case 'H': symbolic_dereference = SYMBOLIC_DEREFERENCE_ARGUMENTS; break;
			case 'L': symbolic_dereference = SYMBOLIC_DEREFERENCE_ALWAYS; break;
			case 'r':
			case 'R': flags |= FLAG_RECURSIVE; break;
			case 'v': flags |= FLAG_VERBOSE; break;
			case 't':
				flags |= FLAG_TARGET_DIR;
				if ( *(arg + 1) )
					target_directory = arg + 1;
				else if ( i + 1 == argc )
					errx(1, "option requires an argument -- '%c'", c);
				else
				{
					target_directory = argv[i+1];
					argv[++i] = NULL;
				}
				arg = "t";
				break;
			case 'T': flags |= FLAG_NO_TARGET_DIR; break;
			case 'u': flags |= FLAG_UPDATE; break;
			case 'p': preserve_list = "mode,ownership,timestamps"; break;
			case 'P': symbolic_dereference = SYMBOLIC_DEREFERENCE_NONE; break;
			default:
				errx(1, "unknown option -- '%c'", c);
			}
		}
		else if ( !strcmp(arg, "--dereference") )
			symbolic_dereference = SYMBOLIC_DEREFERENCE_ALWAYS;
		else if ( !strcmp(arg, "--recursive") )
			flags |= FLAG_RECURSIVE;
		else if ( !strcmp(arg, "--verbose") )
			flags |= FLAG_VERBOSE;
		else if ( !strcmp(arg, "--preserve") )
			preserve_list = "mode,ownership,timestamps";
		else if ( !strncmp(arg, "--preserve=", strlen("--preserve=")) )
			preserve_list = arg + strlen("--preserve=");
		else if ( !strcmp(arg, "--target-directory") )
		{
			if ( i + 1 == argc )
				errx(1, "option '--target-directory' requires an argument");
			target_directory = argv[++i];
			argv[i] = NULL;
			flags |= FLAG_TARGET_DIR;
		}
		else if ( !strncmp(arg, "--target-directory=", strlen("--target-directory=")) )
		{
			target_directory = arg + strlen("--target-directory=");
			flags |= FLAG_TARGET_DIR;
		}
		else if ( !strcmp(arg, "--no-target-directory") )
			flags |= FLAG_NO_TARGET_DIR;
		else if ( !strcmp(arg, "--update") )
			flags |= FLAG_UPDATE;
		else if ( !strcmp(arg, "--no-dereference") )
			symbolic_dereference = SYMBOLIC_DEREFERENCE_NONE;
		else
			errx(1, "unknown option: %s", arg);
	}

	if ( (flags & FLAG_TARGET_DIR) && (flags & FLAG_NO_TARGET_DIR) )
		errx(1, "cannot combine --target-directory (-t) and --no-target-directory (-T)");

	if ( symbolic_dereference == SYMBOLIC_DEREFERENCE_DEFAULT )
	{
		if ( flags & FLAG_RECURSIVE )
			symbolic_dereference = SYMBOLIC_DEREFERENCE_NONE;
		else
			symbolic_dereference = SYMBOLIC_DEREFERENCE_ALWAYS;
	}

	// TODO: Actually preserve.
	(void) preserve_list;

	compact_arguments(&argc, &argv);

	if ( argc < 2 )
		errx(1, "missing file operand");

	uid_t uid = (uid_t) -1;
	gid_t gid = (gid_t) -1;
#ifdef INSTALL
	old_umask = umask(0);
	if ( !is_valid_modespec(modespec) )
		errx(1, "invalid mode: `%s'", modespec);
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

	if ( flags & FLAG_MKDIR )
	{
		mode_t mode = execute_modespec(modespec, 0777 & ~old_umask, S_IFDIR,
		                               old_umask);
		bool success = true;
		for ( int i = 1; i < argc; i++ )
		{
			if ( mkdir_p(argv[i], mode) < 0 )
			{
				warn("%s", argv[i]);
				success = false;
			}
		}
		return success ? 0 : 1;
	}
#endif

	if ( flags & FLAG_NO_TARGET_DIR )
	{
		const char* src = argv[1];
		if ( argc < 3 )
			errx(1, "missing destination file operand after `%s'", src);
		const char* dst = argv[2];
		if ( 3 < argc )
			errx(1, "extra operand `%s'", argv[3]);
		return cp(AT_FDCWD, src, src,
		          AT_FDCWD, dst, dst,
		          flags, symbolic_dereference, modespec, uid, gid) ? 0 : 1;
	}

	if ( !(flags & FLAG_TARGET_DIR) && argc <= 3 )
	{
		const char* src = argv[1];
		if ( argc < 3 )
			errx(1, "missing destination file operand after `%s'", src);
		const char* dst = argv[2];
		return cp_ambigious(AT_FDCWD, src, src,
		                    AT_FDCWD, dst, dst,
		                    flags, symbolic_dereference, modespec, uid, gid)
		       ? 0 : 1;
	}

	if ( !target_directory )
	{
		target_directory = argv[--argc];
		argv[argc] = NULL;
	}

	if ( argc < 2 )
		errx(1, "missing file operand");

	bool success = true;
	for ( int i = 1; i < argc; i++ )
	{
		const char* src = argv[i];
		const char* dst = target_directory;
		if ( !cp_directory(AT_FDCWD, src, src,
		                   AT_FDCWD, dst, dst,
		                   flags, symbolic_dereference, modespec, uid, gid) )
			success = false;
	}

	return success ? 0 : 1;
}
