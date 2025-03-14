/*
 * Copyright (c) 2013, 2018, 2020, 2025 Jonas 'Sortie' Termansen.
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
 * ln.c
 * Create a hard or symbolic link.
 */

#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool lnat(const char* source,
                 int source_dirfd,
                 const char* source_basename,
                 const char* target,
                 int target_dirfd,
                 const char* target_basename,
                 bool force,
                 bool symbolic,
                 bool physical,
                 bool no_dereference,
                 bool no_target_directory,
                 bool verbose)
{
	for ( int attempt = 0; true; attempt++ )
	{
		if ( (symbolic ?
		      symlinkat(source, target_dirfd, target_basename) :
		      linkat(source_dirfd, source_basename, target_dirfd,
		             target_basename, physical ? 0 : AT_SYMLINK_FOLLOW)) < 0 )
		{
			int error = errno;
			if ( !attempt && error == EEXIST && !no_target_directory )
			{
				struct stat target_st;
				if ( !fstatat(target_dirfd, target_basename, &target_st,
				              no_dereference ? AT_SYMLINK_NOFOLLOW : 0) &&
				     S_ISDIR(target_st.st_mode) )
				{
					int new_target_dirfd = openat(target_dirfd, target_basename,
					                              O_RDONLY | O_DIRECTORY);
					if ( new_target_dirfd < 0 )
					{
						warn("%s", target);
						return false;
					}
					char* new_target;
					if ( asprintf(&new_target, "%s/%s", target,
					              source_basename) < 0 )
						err(1, "malloc");
					const char* new_target_basename = source_basename;
					bool result =
						lnat(source, source_dirfd, source_basename,
						     new_target, new_target_dirfd, new_target_basename,
						     force, symbolic, physical, no_dereference,
						     no_target_directory, verbose);
					free(new_target);
					close(new_target_dirfd);
					return result;
				}
			}
			if ( !attempt && error == EEXIST && force )
			{
				if ( !symbolic && !strcmp(source_basename, target_basename) )
				{
					struct stat source_dirst, target_dirst;
					fstat(source_dirfd, &source_dirst);
					fstat(target_dirfd, &target_dirst);
					if ( source_dirst.st_dev == target_dirst.st_dev &&
					     source_dirst.st_ino == target_dirst.st_ino )
					{
						warnx("'%s' and '%s' are the same file", source,
						      target);
						return false;
					}
				}
				if ( unlinkat(target_dirfd, target_basename, 0) < 0 )
				{
					warn("unlink: %s", target);
					return false;
				}
				continue;
			}
			errno = error;
			warn("%s: %s -> %s", symbolic ? "symlink" : "link", source, target);
			return false;
		}
		if ( verbose )
			printf("`%s' => `%s'\n", source, target);
		return true;
	}
}

// Retains the trailing slashes unlike basename(3) so ln foo/ bar/ fails.
static const char* basename_with_slashes(const char* path)
{
	size_t offset = strlen(path);
	while ( offset && path[offset - 1] == '/' )
		offset--;
	while ( offset && path[offset - 1] != '/' )
		offset--;
	return path + offset;
}

static bool ln(const char* source,
               const char* target,
               bool force,
               bool symbolic,
               bool physical,
               bool no_dereference,
               bool no_target_directory,
               bool verbose)
{
	char* source_dup = strdup(source);
	if ( !source_dup )
		err(1, "malloc");
	const char* source_basename = basename_with_slashes(source);
	char* source_dirname = dirname(source_dup);
	int source_dirfd = symbolic ?
	                   AT_FDCWD :
	                   open(source_dirname, O_RDONLY | O_DIRECTORY);
	if ( !symbolic && source_dirfd < 0 )
	{
		warn("%s", source);
		free(source_dup);
		return false;
	}
	char* target_dup = strdup(target);
	if ( !target_dup )
		err(1, "malloc");
	const char* target_basename = basename_with_slashes(target);
	char* target_dirname = dirname(target_dup);
	int target_dirfd = open(target_dirname, O_RDONLY | O_DIRECTORY);
	if ( target_dirfd < 0 )
	{
		warn("%s", target);
		if ( symbolic )
			close(source_dirfd);
		free(source_dup);
		free(target_dup);
		return false;
	}
	bool result = lnat(source, source_dirfd, source_basename,
	                   target, target_dirfd, target_basename,
	                   force, symbolic, physical, no_dereference,
	                   no_target_directory, verbose);
	close(source_dirfd);
	close(target_dirfd);
	free(source_dup);
	free(target_dup);
	return result;
}

static bool ln_into_directory(const char* source,
                              const char* target,
                              bool force,
                              bool symbolic,
                              bool physical,
                              bool no_dereference,
                              bool verbose)
{
	char* source_copy = strdup(source);
	if ( !source_copy )
		err(1, "malloc");
	const char* base_name = basename(source_copy);
	size_t source_length = strlen(source);
	bool has_slash = source_length && source[source_length - 1] == '/';
	char* new_target;
	if ( asprintf(&new_target,
	              "%s%s%s",
	              target,
	              has_slash ? "" : "/",
	              base_name) < 0 )
		err(1, "malloc");
	free(source_copy);
	bool ret = ln(source, new_target, force, symbolic, physical, no_dereference,
	              true, verbose);
	free(new_target);
	return ret;
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
	bool force = false;
	bool symbolic = false;
	bool physical = true;
	bool no_dereference = false;
	bool no_target_directory = false;
	bool verbose = false;

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
			case 'f': force = true; break;
			case 'h': no_dereference = true; break;
			case 'L': physical = false; break;
			case 'n': no_dereference = true; break;
			case 'P': physical = true; break;
			case 's': symbolic = true; break;
			case 'T': no_target_directory = true; break;
			case 'v': verbose = true; break;
			default:
				errx(1, "unknown option -- '%c'", c);
			}
		}
		else if ( !strcmp(arg, "--force") )
			force = true;
		else if ( !strcmp(arg, "--logical") )
			physical = false;
		else if ( !strcmp(arg, "--physical") )
			physical = true;
		else if ( !strcmp(arg, "--symbolic") )
			symbolic = true;
		else if ( !strcmp(arg, "--verbose") )
			verbose = true;
		else
			errx(1, "unknown option: %s", arg);
	}

	compact_arguments(&argc, &argv);

	if ( argc == 1 )
		errx(1, "expected file operand");

	if ( no_target_directory && argc != 3 )
		errx(1, "unexpected extra operand");

	if ( argc == 2 )
		return ln_into_directory(argv[1], ".", force, symbolic, physical,
		                         no_dereference, verbose) ? 0 : 1;

	if ( argc == 3 )
		return ln(argv[1], argv[2], force, symbolic, physical, no_dereference,
		          no_target_directory, verbose) ? 0 : 1;

	const char* target = argv[argc - 1];
	bool success = true;

	for ( int i = 1; i < argc - 1; i++ )
	{
		const char* source = argv[i];
		if ( !ln_into_directory(source, target, force, symbolic, physical,
		                        no_dereference, verbose) )
			success = false;
	}

	return success ? 0 : 1;
}
