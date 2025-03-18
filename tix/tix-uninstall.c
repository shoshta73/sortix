/*
 * Copyright (c) 2025 Jonas 'Sortie' Termansen.
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
 * tix-uninstall.c
 * Uninstall a package from a tix collection.
 */

#include <sys/stat.h>
#include <sys/wait.h>

#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

static void uninstall(const char* collection, const char* package, bool quiet)
{
	if ( !is_valid_package_name(package) )
		err(1, "Invalid package name: %s", package);
	const char* prefix = collection;
	if ( !strcmp(prefix, "/") )
		prefix = "";
	int collection_fd = open(collection, O_RDONLY | O_DIRECTORY);
	if ( collection_fd < 0 )
		err(1, "%s", collection);
	char* tix_path = join_paths(collection, "tix");
	if ( !tix_path )
		err(1, "malloc");
	char* tixinfo_path = join_paths(tix_path, "tixinfo");
	char* manifest_path = join_paths(tix_path, "manifest");
	if ( !tixinfo_path || !manifest_path )
		err(1, "malloc");
	char* pkg_tixinfo_path = join_paths(tixinfo_path, package);
	char* pkg_manifest_path = join_paths(manifest_path, package);
	if ( !pkg_tixinfo_path || !pkg_manifest_path )
		err(1, "malloc");
	if ( access(pkg_tixinfo_path, F_OK) < 0 )
	{
		if ( errno == ENOENT )
			errx(1, "Package is not installed: %s", package);
		err(1, "%s", pkg_tixinfo_path);
	}
	if ( !quiet )
	{
		printf("Uninstalling %s", package);
		if ( strcmp(collection, "/") != 0 )
			printf(" in `%s'", collection);
		printf("...\n");
		fflush(stdout);
	}
	FILE* manifest_fp = fopen(pkg_manifest_path, "r");
	if ( !manifest_fp && errno != ENOENT )
		err(1, "%s", pkg_manifest_path);
	else if ( manifest_fp )
	{
		string_array_t paths = string_array_make();
		if ( !string_array_append_file(&paths, manifest_fp) )
			err(1, "%s", pkg_manifest_path);
		for ( size_t n = paths.length; n; n-- )
		{
			const char* path = paths.strings[n - 1];
			if ( path[0] != '/' || !path[1] )
				continue;
			path++;
			if ( unlinkat(collection_fd, path, 0) < 0 )
			{
				if ( errno == EISDIR )
				{
				     if ( unlinkat(collection_fd, path, AT_REMOVEDIR) < 0 &&
					      errno != ENOTEMPTY )
						err(1, "rmdir: %s/%s", prefix, path);
				}
				else if ( errno != ENOENT )
					err(1, "unlink: %s/%s", prefix, path);
			}
		}
		string_array_reset(&paths);
		fclose(manifest_fp);
	}
	if ( unlink(pkg_manifest_path) < 0 && errno != ENOENT )
		err(1, "%s", pkg_manifest_path);
	if ( unlink(pkg_tixinfo_path) < 0 && errno != ENOENT )
		err(1, "%s", pkg_tixinfo_path);
	close(collection_fd);
	free(tix_path);
	free(tixinfo_path);
	free(manifest_path);
	free(pkg_tixinfo_path);
	free(pkg_manifest_path);
}

int main(int argc, char* argv[])
{
	const char* collection = "/";
	bool quiet = false;

	const struct option longopts[] =
	{
		{"collection", required_argument, NULL, 'C'},
		{"quiet", no_argument, NULL, 'q'},
		{0, 0, 0, 0}
	};
	const char* opts = "C:q";
	int opt;
	while ( (opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1 )
	{
		switch ( opt )
		{
		case 'C': collection = optarg; break;
		case 'q': quiet = true; break;
		default: return 1;
		}
	}

	if ( !collection[0] )
		collection = "/";

	if ( argc - optind < 1 )
		errx(1, "expected package name to uninstall");

	char* tix_path = join_paths(collection, "tix");
	char* coll_conf_path = join_paths(tix_path, "collection.conf");
	if ( !tix_path || !coll_conf_path )
		err(1, "malloc");

	if ( !IsDirectory(collection) )
	{
		if ( errno == ENOENT )
			errx(1, "%s is not a tix collection", collection);
		err(1, "%s", collection);
	}
	else if ( !IsDirectory(tix_path) )
	{
		if ( errno == ENOENT )
			errx(1, "%s is not a tix collection", collection);
		err(1, "%s", tix_path);
	}
	else if ( !IsFile(coll_conf_path) )
	{
		if ( errno == ENOENT )
			errx(1, "%s is not a tix collection", collection);
		err(1, "%s", coll_conf_path);
	}

	string_array_t coll_conf = string_array_make();
	switch ( variables_append_file_path(&coll_conf, coll_conf_path) )
	{
	case -1: err(1, "%s", coll_conf_path);
	case -2: errx(2, "%s: Syntax error", coll_conf_path);
	}

	VerifyTixCollectionConfiguration(&coll_conf, coll_conf_path);

	for ( int i = optind; i < argc; i++ )
		uninstall(collection, argv[i], quiet);

	return 0;
}
