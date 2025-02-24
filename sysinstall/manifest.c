/*
 * Copyright (c) 2015, 2018, 2020, 2021, 2023 Jonas 'Sortie' Termansen.
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
 * manifest.c
 * Manifest handling functions.
 */

#include <sys/stat.h>
#include <sys/types.h>

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <ioleast.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fileops.h"
#include "manifest.h"
#include "string_array.h"

bool has_manifest(const char* manifest)
{
	char* path = join_paths("/tix/manifest", manifest);
	if ( !path )
	{
		warn("asprintf");
		_exit(2);
	}
	bool result = access_or_die(path, F_OK) == 0;
	free(path);
	return result;
}

char** read_manifest(const char* path, size_t* out_count)
{
	char** files = read_lines_file(path, out_count);
	if ( !files )
		return NULL;
	// TODO: Remove this compatibility after releasing Sortix 1.1. The manifests
	//       in Sortix 1.0 have spurious trailing slashes due to a bug in the
	//       kernel binary package extractor. Remove them here to normalize the
	//       manifests.
	for ( size_t i = 0; i < *out_count; i++ )
	{
		char* file = files[i];
		size_t len = strlen(file);
		if ( 2 <= len && file[len - 1] == '/' )
			file[len - 1] = '\0';
	}
	string_array_sort_strcmp(files, *out_count);
	return files;
}

static void unlink_rename_conflict(const char* path)
{
	if ( !unlink(path) || errno == ENOENT )
		return;
	if ( errno != EISDIR )
	{
		warn("unlink: %s", path);
		_exit(2);
	}
	if ( !rmdir(path) )
		return;
	if ( errno != ENOTEMPTY && errno != EEXIST )
	{
		warn("rmdir: %s", path);
		_exit(2);
	}
	char* conflict;
	if ( asprintf(&conflict, "%s.conflict.XXXXXX", path) < 0 )
	{
		warn("malloc");
		_exit(2);
	}
	if ( !mkdtemp(conflict) )
	{
		warn("mkdtemp: %s.conflict.XXXXXX", path);
		_exit(2);
	}
	if ( rename(path, conflict) < 0 )
	{
		warn("rename: %s -> %s", path, conflict);
		rmdir(conflict);
		_exit(2);
	}
	printf("warning: Moving conflicting directory %s to %s\n", path, conflict);
	free(conflict);
}

struct hardlink
{
	dev_t dev;
	ino_t ino;
	char* path;
};

void install_manifest(const char* manifest,
                      const char* from_prefix,
                      const char* to_prefix,
                      const char* const* preserved,
                      size_t preserved_count,
                      bool may_hardlink)
{
	struct stat from_st, to_st;
	if ( stat(from_prefix[0] ? from_prefix : "/", &from_st) < 0 ||
	     stat(to_prefix[0] ? to_prefix : "/", &to_st) < 0 )
	{
		warn("malloc");
		_exit(2);
	}
	bool can_hardlink = may_hardlink && from_st.st_dev == to_st.st_dev;
	struct hardlink* hardlinks = NULL;
	size_t hardlinks_used = 0;
	size_t hardlinks_length = 0;
	size_t buffer_size = 1 << 16;
	char* buffer = malloc(buffer_size);
	if ( !buffer )
	{
		warn("malloc");
		_exit(2);
	}
	mode_t old_umask = umask(0000);
	// Read the input and output manifests if they exist. Consider a manifest
	// that doesn't exist as being empty.
	char* inmanifest;
	char* outmanifest;
	char* outnewmanifest;
	if ( asprintf(&inmanifest, "%s/tix/manifest/%s", from_prefix,
	              manifest) < 0 ||
	     asprintf(&outmanifest, "%s/tix/manifest/%s", to_prefix,
	              manifest) < 0 ||
	     asprintf(&outnewmanifest, "%s/tix/manifest/%s.new", to_prefix,
	              manifest) < 0 )
	{
		warn("malloc");
		_exit(2);
	}
	bool in_exists = !access_or_die(inmanifest, F_OK);
	bool out_exists = !access_or_die(outmanifest, F_OK);
	const char* action = in_exists && out_exists ? "Upgrading" :
	                     in_exists ? "Installing" :
	                     "Uninstalling";
	printf(" - %s %s...\n", action, manifest);
	char** empty = (char*[]){};
	char** in_files = empty;
	size_t in_files_count = 0;
	if ( in_exists &&
	     !(in_files = read_manifest(inmanifest, &in_files_count)) )
	{
		warn("%s", inmanifest);
		_exit(2);
	}
	char** out_files = empty;
	size_t out_files_count = 0;
	if ( out_exists &&
	     !(out_files = read_manifest(outmanifest, &out_files_count)) )
	{
		warn("%s", outmanifest);
		_exit(2);
	}
	// Directories to be cleaned up afterwards when they might be empty.
	size_t rmdirs_count;
	size_t rmdirs_length;
	char** rmdirs;
	if ( !string_array_init(&rmdirs, &rmdirs_count, &rmdirs_length) )
	{
		warn("malloc");
		_exit(2);
	}
	// Find the differences by mutually iterating the manifests in sorted
	// order.
	size_t in_i = 0;
	size_t out_i = 0;
	while ( in_i < in_files_count || out_i < out_files_count )
	{
		const char* in = in_i < in_files_count ? in_files[in_i] : NULL;
		const char* out = out_i < out_files_count ? out_files[out_i] : NULL;
		if ( !in || (out && strcmp(in, out) > 0) )
		{
			out_i++;
			const char* path = out;
			char* out_path = join_paths(to_prefix, path);
			if ( !out_path )
			{
				warn("asprintf");
				_exit(2);
			}
			// Don't delete a path if it will be added in later by another
			// manifest. This supports files moving from one manifest to another
			// and directories only being cleaned up when no manifest mentions
			// them.
			if ( string_array_contains_bsearch_strcmp(preserved,
			                                          preserved_count, path) )
			{
				// Handle a directory becoming a symbolic link, which will be
				// renamed to a conflict directory and replaced with a symbolic
				// link, but we must take care not to delete anything through
				// the symbolic link. This case happens if the directory becomes
				// a symlink in another manifest.
				struct stat outst;
				if ( !lstat(out_path, &outst) )
				{
					if ( S_ISLNK(outst.st_mode) )
					{
						size_t path_length = strlen(path);
						while ( out_i < out_files_count &&
						        !strncmp(path, out_files[out_i], path_length) &&
						        out_files[out_i][path_length] == '/' )
							out_i++;
					}
				}
				else if ( errno != ENOENT && errno != ENOTDIR )
				{
					warn("%s", out_path);
					_exit(2);
				}
				free(out_path);
				continue;
			}
			if ( unlink(out_path) < 0 )
			{
				if ( errno == EISDIR )
				{
					if ( rmdir(out_path) < 0 )
					{
						if ( errno == ENOTEMPTY || errno == EEXIST )
						{
							if ( !string_array_append(&rmdirs, &rmdirs_count,
							                          &rmdirs_length, path) )
							{
								warn("malloc");
								_exit(2);
							}
						}
						else if ( errno != ENOENT )
						{
							warn("unlink: %s", out_path);
							_exit(2);
						}
					}
				}
				else if ( errno != ENOENT )
				{
					warn("unlink: %s", out_path);
					_exit(2);
				}
			}
			free(out_path);
			continue;
		}
		in_i++;
		if ( out && !strcmp(in, out) )
			out_i++;
		const char* path = in;
		char* in_path = join_paths(from_prefix, path);
		char* out_path = join_paths(to_prefix, path);
		if ( !in_path || !out_path )
		{
			warn("asprintf");
			_exit(2);
		}
		struct stat inst;
		if ( lstat(in_path, &inst) < 0 )
		{
			warn("%s", in_path);
			_exit(2);
		}
		struct hardlink* hardlink = NULL;
		if ( S_ISREG(inst.st_mode) && 2 <= inst.st_nlink )
		{
			for ( size_t i = 0; i < hardlinks_used; i++ )
			{
				if ( hardlinks[i].dev != inst.st_dev ||
				     hardlinks[i].ino != inst.st_ino )
					continue;
				hardlink = &hardlinks[i];
				break;
			}
		}
		if ( hardlink )
		{
			unlink_rename_conflict(out_path);
			if ( link(hardlink->path, out_path) < 0 )
			{
				warn("link: %s -> %s", hardlink->path, out_path);
				_exit(2);
			}
		}
		else if ( S_ISDIR(inst.st_mode) )
		{
			if ( unlink(out_path) < 0 && errno != ENOENT && errno != EISDIR )
			{
				warn("unlink: %s", out_path);
				_exit(2);
			}
			if ( mkdir(out_path, inst.st_mode & 07777) < 0 )
			{
				if ( errno == EEXIST )
				{
					if ( chmod(out_path, inst.st_mode & 07777) < 0 )
					{
						warn("chmod: %s", out_path);
						_exit(2);
					}
				}
				else
				{
					warn("mkdir: %s", out_path);
					_exit(2);
				}
			}
		}
		else if ( can_hardlink && S_ISREG(inst.st_mode) &&
		          (unlink_rename_conflict(out_path), true) &&
		          !link(in_path, out_path) )
			;
		else if ( S_ISREG(inst.st_mode) )
		{
			unlink_rename_conflict(out_path);
			int in_fd = open(in_path, O_RDONLY);
			if ( in_fd < 0 )
			{
				warn("%s", in_path);
				_exit(2);
			}
			int out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC | O_EXCL,
			                  inst.st_mode & 07777);
			if ( out_fd < 0 )
			{
				warn("%s", out_path);
				_exit(2);
			}
			while ( true )
			{
				ssize_t amount = read(in_fd, buffer, buffer_size);
				if ( amount < 0 )
				{
					warn("read: %s", in_path);
					_exit(2);
				}
				if ( amount == 0 )
					break;
				if ( writeall(out_fd, buffer, (size_t) amount) <
				     (size_t) amount )
				{
					warn("write: %s", out_path);
					_exit(2);
				}
			}
			close(out_fd);
			close(in_fd);
			if ( 2 <= inst.st_nlink )
			{
				if ( hardlinks_used == hardlinks_length )
				{
					size_t new_length = hardlinks_length ? hardlinks_length : 8;
					struct hardlink* new_hardlinks =
						reallocarray(hardlinks, new_length,
						             2 * sizeof(struct hardlink));
					if ( !new_hardlinks )
					{
						warn("malloc");
						_exit(2);
					}
					hardlinks = new_hardlinks;
					hardlinks_length = 2 * new_length;
				}
				hardlinks[hardlinks_used].ino = inst.st_ino;
				hardlinks[hardlinks_used].dev = inst.st_dev;
				if ( !(hardlinks[hardlinks_used].path = strdup(out_path)) )
				{
					warn("strdup");
					_exit(2);
				}
				hardlinks_used++;
			}
		}
		else if ( S_ISLNK(inst.st_mode) )
		{
			ssize_t amount = readlink(in_path, buffer, buffer_size - 1);
			if ( amount < 0 )
			{
				warn("readlink: %s", in_path);
				_exit(2);
			}
			buffer[amount] = '\0';
			unlink_rename_conflict(out_path);
			if ( symlink(buffer, out_path) < 0 && errno != EEXIST )
			{
				warn("symlink: %s", out_path);
				_exit(2);
			}
			// Handle a directory becoming a symbolic link, which will be
			// renamed to a conflict directory and replaced with a symbolic
			// link, but we must take care not to delete anything through
			// the symbolic link. This case happens if the directory becomes a
			// symlink in the same manifest.
			size_t path_length = strlen(path);
			while ( out_i < out_files_count &&
			        !strncmp(path, out_files[out_i], path_length) &&
			        out_files[out_i][path_length] == '/' )
				out_i++;
		}
		else
		{
			warnx("%s: Don't know how to copy this object", in_path);
			_exit(2);
		}
		free(in_path);
		free(out_path);
	}
	// Delete directories that might not be empty in backwards order to ensure
	// subdirectories are deleted before their parent directories.
	for ( size_t i = rmdirs_count; i; i-- )
	{
		const char* path = rmdirs[i - 1];
		char* out_path;
		if ( asprintf(&out_path, "%s%s", to_prefix, path) < 0 )
		{
			warn("asprintf");
			_exit(2);
		}
		if ( rmdir(out_path) < 0 &&
		     errno != ENOTEMPTY && errno != EEXIST && errno != ENOENT )
		{
			warn("unlink: %s", out_path);
			_exit(2);
		}
		free(out_path);
		(void) path;
	}
	string_array_free(&rmdirs, &rmdirs_count, &rmdirs_length);
	if ( in_exists )
	{
		if ( unlink(outnewmanifest) < 0 && errno != ENOENT )
		{
			warn("unlink: %s", outnewmanifest);
			_exit(2);
		}
		mode_t temp_umask = umask(0022);
		FILE* fp = fopen(outnewmanifest, "w");
		if ( !fp )
		{
			warn("%s", outnewmanifest);
			_exit(2);
		}
		umask(temp_umask);
		for ( size_t i = 0; i < in_files_count; i++ )
		{
			const char* path = in_files[i];
			if ( fputs(path, fp) == EOF || fputc('\n', fp) == EOF )
			{
				warn("%s", outnewmanifest);
				_exit(2);
			}
		}
		if ( fclose(fp) == EOF )
		{
			warn("%s", outnewmanifest);
			_exit(2);
		}
		if ( rename(outnewmanifest, outmanifest) < 0 )
		{
			warn("rename: %s -> %s", outnewmanifest, outmanifest);
			_exit(2);
		}
	}
	else if ( out_exists )
	{
		if ( unlink(outmanifest) < 0 && errno != ENOENT )
		{
			warn("unlink: %s", outmanifest);
			_exit(2);
		}
	}
	// Write out the new tixinfo afterwards to ensure no paths are leaked if the
	// operation is aborted part way.
	char* in_tixinfo;
	char* out_tixinfo;
	if ( asprintf(&in_tixinfo, "%s/tix/tixinfo/%s", from_prefix,
	              manifest) < 0 ||
	     asprintf(&out_tixinfo, "%s/tix/tixinfo/%s", to_prefix,
	              manifest) < 0 )
	{
		warn("malloc");
		_exit(2);
	}
	// Update or delete the tixinfo accordingly.
	bool is_tix = !access_or_die(in_tixinfo, F_OK);
	if ( is_tix )
	{
		int in_fd = open(in_tixinfo, O_RDONLY);
		if ( in_fd < 0 )
		{
			warn("%s", in_tixinfo);
			_exit(2);
		}
		unlink(out_tixinfo);
		int out_fd = open(out_tixinfo, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if ( out_fd < 0 )
		{
			warn("%s", out_tixinfo);
			_exit(2);
		}
		while ( true )
		{
			ssize_t amount = read(in_fd, buffer, buffer_size);
			if ( amount < 0 )
			{
				warn("read: %s", in_tixinfo);
				_exit(2);
			}
			if ( amount == 0 )
				break;
			if ( writeall(out_fd, buffer, (size_t) amount) < (size_t) amount )
			{
				warn("write: %s", out_tixinfo);
				_exit(2);
			}
		}
		close(out_fd);
		close(in_fd);
	}
	else
	{
		if ( unlink(out_tixinfo) < 0 && errno != ENOENT )
		{
			warn("unlink: %s", out_tixinfo);
			_exit(2);
		}
	}
	free(in_tixinfo);
	free(out_tixinfo);
	if ( in_files != empty )
	{
		for ( size_t i = 0; i < in_files_count; i++ )
			free(in_files[i]);
		free(in_files);
	}
	if ( out_files != empty )
	{
		for ( size_t i = 0; i < out_files_count; i++ )
			free(out_files[i]);
		free(out_files);
	}
	free(inmanifest);
	free(outmanifest);
	free(outnewmanifest);
	umask(old_umask);
	free(buffer);
	for ( size_t i = 0; i < hardlinks_used; i++ )
		free(hardlinks[i].path);
	free(hardlinks);
}

void install_manifests(const char* const* manifests,
                       size_t manifests_count,
                       const char* from_prefix,
                       const char* to_prefix,
                       bool may_hardlink)
{
	// Load all the paths mentioned in the new set of manifests, which are used
	// to ensure no files and directories are deleted part way if they are moved
	// from one manifest to another.
	printf(" - Loading manifests...\n");
	size_t all_count;
	size_t all_length;
	char** all;
	if ( !string_array_init(&all, &all_count, &all_length) )
	{
		warn("malloc");
		_exit(2);
	}
	for ( size_t i = 0; i < manifests_count; i++ )
	{
		// Read the input manifests if they exist. Consider a manifest that
		// doesn't exist as being empty.
		const char* manifest = manifests[i];
		char* inmanifest;
		if ( asprintf(&inmanifest, "%s/tix/manifest/%s", from_prefix,
		              manifest) < 0 )
		{
			warn("asprintf");
			_exit(2);
		}
		char** empty = (char*[]){};
		char** in_files = empty;
		size_t in_files_count = 0;
		if ( !access_or_die(inmanifest, F_OK) &&
		     !(in_files = read_manifest(inmanifest, &in_files_count)) )
		{
			warn("%s", inmanifest);
			_exit(2);
		}
		// Directories can appear in multiple manifests, so keep track of all
		// input paths so we later can find duplicates.
		for ( size_t i = 0; i < in_files_count; i++ )
		{
			if ( !string_array_append(&all, &all_count, &all_length,
			                          in_files[i]) )
			{
				warn("malloc");
				_exit(2);
			}
		}
		if ( in_files != empty )
		{
			for ( size_t i = 0; i < in_files_count; i++ )
				free(in_files[i]);
			free(in_files);
		}
		free(inmanifest);
	}
	string_array_sort_strcmp(all, all_count);
	all_count = string_array_deduplicate(all, all_count);
	for ( size_t i = 0; i < manifests_count; i++ )
		install_manifest(manifests[i], from_prefix, to_prefix,
		                 (const char* const*) all, all_count, may_hardlink);
	string_array_free(&all, &all_count, &all_length);
}

char** read_installed_list(const char* prefix, size_t* out_count)
{
	char* tixinfo;
	if ( asprintf(&tixinfo, "%s/tix/tixinfo", prefix) < 0 )
	{
		warn("malloc");
		_exit(2);
	}
	size_t count;
	size_t length;
	char** installed;
	if ( !string_array_init(&installed, &count, &length) )
	{
		warn("malloc");
		_exit(2);
	}
	DIR* dir = opendir(tixinfo);
	if ( !dir )
	{
		if ( errno == ENOENT )
			return *out_count = count, installed;
		warn("opendir: %s", tixinfo);
		_exit(2);
	}
	struct dirent* entry;
	while ( (errno = 0, entry = readdir(dir)) )
	{
		if ( entry->d_name[0] == '.' )
			continue;
		if ( !strcmp(entry->d_name, "system") )
			continue;
		if ( !string_array_append(&installed, &count, &length, entry->d_name) )
		{
			warn("malloc");
			_exit(2);
		}
	}
	if ( errno )
	{
		warn("readdir: %s", tixinfo);
		_exit(2);
	}
	free(tixinfo);
	string_array_sort_strcmp(installed, count);
	return *out_count = count, installed;
}

void install_manifests_detect(const char* from_prefix,
                              const char* to_prefix,
                              bool system,
                              bool detect_from,
                              bool detect_to,
                              bool may_hardlink)
{
	char** manifests;
	size_t manifests_count;
	size_t manifests_length;
	string_array_init(&manifests, &manifests_count, &manifests_length);
	if ( system &&
	     !string_array_append(&manifests, &manifests_count, &manifests_length,
	                          "system") )
	{
		warn("malloc");
		_exit(2);
	}
	size_t system_offset = system ? 1 : 0;
	const char* prefixes[] =
	{
		detect_from ? from_prefix : NULL,
		detect_to ? to_prefix : NULL,
	};
	for ( size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++ )
	{
		const char* prefix = prefixes[i];
		if ( !prefix )
			continue;
		size_t installed_count;
		char** installed = read_installed_list(prefix, &installed_count);
		for ( size_t i = 0; i < installed_count; i++ )
		{
			if ( !string_array_append(&manifests, &manifests_count,
			                          &manifests_length, installed[i]) )
			{
				warn("malloc");
				_exit(2);
			}
			free(installed[i]);
		}
		free(installed);
	}
	// Keep the system manifest first and otherwise sort and deduplicate.
	string_array_sort_strcmp(manifests + system_offset,
	                         manifests_count - system_offset);
	manifests_count = string_array_deduplicate(manifests, manifests_count);
	install_manifests((const char* const*) manifests, manifests_count,
	                  from_prefix, to_prefix, may_hardlink);
	string_array_free(&manifests, &manifests_count, &manifests_length);
}
