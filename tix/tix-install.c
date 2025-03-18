/*
 * Copyright (c) 2013, 2015-2017, 2020, 2022-2023, 2025 Jonas 'Sortie' Termansen.
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
 * tix-install.c
 * Install a tix into a tix collection.
 */

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

static bool IsPackageInstalled(const char* tixdb_path, const char* package)
{
	char* tixinfo_dir = join_paths(tixdb_path, "tixinfo");
	if ( !tixinfo_dir )
		err(1, "malloc");
	char* tixinfo = join_paths(tixinfo_dir, package);
	if ( !tixinfo )
		err(1, "malloc");
	bool installed = !access(tixinfo, F_OK);
	free(tixinfo);
	free(tixinfo_dir);
	return installed;
}

// TODO: After releasing Sortix 1.1, delete generation 2 compatibility.
static void MarkPackageAsInstalled(const char* tixdb_path, const char* package)
{
	char* installed_list_path = join_paths(tixdb_path, "installed.list");
	FILE* installed_list_fp = fopen(installed_list_path, "a");
	if ( !installed_list_fp )
		err(1, "`%s'", installed_list_path);

	fprintf(installed_list_fp, "%s\n", package);
	fflush(installed_list_fp);

	if ( ferror(installed_list_fp) || fclose(installed_list_fp) == EOF )
		err(1, "`%s'", installed_list_path);
	free(installed_list_path);
}

static bool ends_with(const char* str, const char* end)
{
	size_t len = strlen(str);
	size_t endlen = strlen(end);
	return endlen <= len && !strcmp(str + len - endlen, end);
}

static int strcmp_indirect(const void* a_ptr, const void* b_ptr)
{
	const char* a = *(const char* const*) a_ptr;
	const char* b = *(const char* const*) b_ptr;
	return strcmp(a, b);
}

static bool file;
static const char* collection = "/";
static bool package;
static bool quiet;
static bool reinstall;

static char* tix_directory_path;
static int generation;
static const char* coll_prefix;
static const char* coll_platform;

void ResolvePackages(string_array_t* packages, string_array_t* fetch_argv);
void InstallPackage(const char* package, string_array_t* fetch_argv);
void InstallFile(const char* tix_path);

int main(int argc, char* argv[])
{
	enum
	{
		OPT_REINSTALL = 256,
	};
	const struct option longopts[] =
	{
		{"collection", required_argument, NULL, 'C'},
		{"file", no_argument, NULL, 'f'},
		{"package", no_argument, NULL, 'p'},
		{"quiet", no_argument, NULL, 'q'},
		{"reinstall", no_argument, NULL, OPT_REINSTALL},
		{0, 0, 0, 0}
	};
	const char* opts = "C:fpq";
	int opt;
	while ( (opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1 )
	{
		switch ( opt )
		{
		case 'C': collection = optarg; break;
		case 'f': file = true; package = false; break;
		case 'p': file = false; package = true; break;
		case 'q': quiet = true; break;
		case OPT_REINSTALL: reinstall = true; break;
		default: return 1;
		}
	}

	if ( !collection[0] )
		collection = "/";

	if ( argc - optind < 1 )
		errx(1, "expected package to install");

	tix_directory_path = join_paths(collection, "tix");
	char* coll_conf_path = join_paths(tix_directory_path, "collection.conf");
	if ( !tix_directory_path || !coll_conf_path )
		err(1, "malloc");

	if ( !IsDirectory(collection) )
	{
		if ( errno == ENOENT )
			errx(1, "%s is not a tix collection", collection);
		err(1, "%s", collection);
	}
	else if ( !IsDirectory(tix_directory_path) )
	{
		if ( errno == ENOENT )
			errx(1, "%s is not a tix collection", collection);
		err(1, "%s", tix_directory_path);
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

	const char* coll_generation =
		dictionary_get(&coll_conf, "TIX_COLLECTION_VERSION");
	// TODO: After releasing Sortix 1.1, remove generation 2 compatibility.
	if ( !coll_generation )
		coll_generation = dictionary_get(&coll_conf, "collection.generation");
	if ( !coll_generation )
		err(1, "%s: No TIX_COLLECTION_VERSION was set", coll_conf_path);
	generation = atoi(coll_generation);
	if ( generation == 3 )
	{
		coll_prefix = dictionary_get(&coll_conf, "PREFIX");
		coll_platform = dictionary_get(&coll_conf, "PLATFORM");
	}
	// TODO: After releasing Sortix 1.1, remove generation 2 compatibility.
	else if ( generation == 2  )
	{
		coll_prefix = dictionary_get(&coll_conf, "collection.prefix");
		coll_platform = dictionary_get(&coll_conf, "collection.platform");
	}
	else
		errx(1, "%s: Unsupported TIX_COLLECTION_VERSION: %i",
		     coll_conf_path, generation);

	const char* fetch_options = dictionary_get(&coll_conf, "FETCH_OPTIONS");
	string_array_t fetch_argv = string_array_make();
	if ( !string_array_append(&fetch_argv, "tix-fetch") )
		err(1, "malloc");
	if ( fetch_options )
	{
		char* copy = strdup(fetch_options);
		if ( !copy )
			err(1, "malloc");
		char* state = copy;
		char* arg;
		while ( (arg = strsep(&state, " \t\n")) )
		{
			if ( !*arg )
				continue;
			if ( *arg && !string_array_append(&fetch_argv, arg) )
				err(1, "malloc");
		}
		free(copy);
	}

	// TODO: After releasing Sortix 1.1, drop the implicit detection of the
	//       .tix.tar.xz file extension and require -f.
	if ( !package || !file )
	{
		for ( int i = optind; i < argc; i++ )
		{
			if ( ends_with(argv[i], ".tix.tar.xz") )
			{
				file = true;
				break;
			}
		}
	}

	if ( file )
	{
		for ( int i = optind; i < argc; i++ )
			InstallFile(argv[i]);
	}
	else
	{
		string_array_t packages = string_array_make();
		for ( int i = optind; i < argc; i++ )
		{
			if ( IsPackageInstalled(tix_directory_path, argv[i]) )
			{
				if ( !quiet )
				{
					printf("Package %s is already installed\n", argv[i]);
					fflush(stdout);
				}
			}
			else if ( !string_array_append(&packages, argv[i]) )
				err(1, "malloc");
		}
		ResolvePackages(&packages, &fetch_argv);
		for ( size_t i = 0; i < packages.length; i++ )
			InstallPackage(packages.strings[i], &fetch_argv);
		string_array_reset(&packages);
	}

	string_array_reset(&fetch_argv);
	string_array_reset(&coll_conf);

	free(tix_directory_path);
	free(coll_conf_path);

	return 0;
}

struct pkg
{
	char* name;
	char* deps;
	bool picked;
};

static int pkg_search(const void* name_ptr, const void* pkg_ptr)
{
	const char* name = (const char*) name_ptr;
	struct pkg* pkg = *(struct pkg**) pkg_ptr;
	return strcmp(name, pkg->name);
}

static struct pkg* pkg_lookup(struct pkg** pkgs, size_t used, const char* name)
{
	struct pkg** entry_ptr =
		bsearch(name, pkgs, used, sizeof(struct pkg*), pkg_search);
	return entry_ptr ? *entry_ptr : NULL;
}

static int pkg_cmp_indirect(const void* a_ptr, const void* b_ptr)
{
	struct pkg* a = *(struct pkg* const*) a_ptr;
	struct pkg* b = *(struct pkg* const*) b_ptr;
	return strcmp(a->name, b->name);
}

void WantPackage(string_array_t* packages, const char* package,
                 struct pkg** pkgs, size_t pkgs_used)
{
	struct pkg* pkg = pkg_lookup(pkgs, pkgs_used, package);
	if ( pkg->picked )
		return;
	pkg->picked = true;
	if ( IsPackageInstalled(tix_directory_path, package) )
		return;
	if ( !string_array_append(packages, package) )
		err(1, "malloc");
}

void ResolvePackages(string_array_t* packages, string_array_t* fetch_argv)
{
	if ( !packages->length )
		return;
	char* cache = join_paths(collection, "var/cache/tix");
	if ( !cache )
		err(1, "malloc");
	char* release_info = join_paths(cache, "release.info");
	char* sha256sum = join_paths(cache, "sha256sum");
	char* dependencies_list = join_paths(cache, "dependencies.list");
	if ( !release_info || !sha256sum || !dependencies_list )
		err(1, "malloc");
	if ( mkdir_p(cache, 0755) < 0 && errno != EEXIST )
		err(1, "mkdir: %s", cache);
	if ( fork_and_wait_or_death() )
	{
		if ( quiet && !string_array_append(fetch_argv, "-q") )
			err(1, "malloc");
		const char* args[] =
		{
			"-C", collection,
			"-c",
			"-O", cache,
			"--output-release-info", release_info,
			"--output-sha256sum", sha256sum,
			"dependencies.list",
			NULL
		};
		for ( size_t i = 0; i < sizeof(args) / sizeof(args[0]); i++ )
			if ( !string_array_append(fetch_argv, args[i]) )
				err(1, "malloc");
		execvp(fetch_argv->strings[0], (char* const*) fetch_argv->strings);
		err(127, "%s", fetch_argv->strings[0]);
	}
	FILE* dependencies_fp = fopen(dependencies_list, "r");
	if ( !dependencies_fp )
		err(1, "%s", dependencies_list);
	struct pkg** pkgs = NULL;
	size_t pkgs_used = 0;
	size_t pkgs_length = 0;
	char* line = NULL;
	size_t line_size = 0;
	ssize_t line_len;
	bool sorted = true;
	while ( 0 < (line_len = getline(&line, &line_size, dependencies_fp)) )
	{
		if ( line[line_len-1] == '\n' )
			line[--line_len] = '\0';
		char* sep = strchr(line, ':');
		if ( !sep )
			err(1, "%s: invalid line: %s", dependencies_list, line);
		*sep = '\0';
		const char* key = line;
		const char* value = sep + 1;
		struct pkg* pkg = calloc(1, sizeof(struct pkg));
		if ( !pkg ||
		     !(pkg->name = strdup(key)) ||
		     !(pkg->deps = strdup(value)) )
			err(1, "malloc");
		if ( pkgs_used == pkgs_length )
		{
			size_t old_length = pkgs_length ? pkgs_length : 4;
			struct pkg** new_pkgs =
				reallocarray(pkgs, old_length, 2 * sizeof(struct pkg));
			if ( !new_pkgs )
				err(1, "malloc");
			pkgs = new_pkgs;
			pkgs_length = 2 * old_length;
		}
		if ( sorted && pkgs_used )
			sorted = strcmp(pkgs[pkgs_used - 1]->name, key) < 0;
		pkgs[pkgs_used++] = pkg;
	}
	free(line);
	if ( ferror(dependencies_fp) )
		err(1, "%s", dependencies_list);
	fclose(dependencies_fp);
	if ( !sorted )
		qsort(pkgs, pkgs_used, sizeof(pkgs[0]), pkg_cmp_indirect);
	for ( size_t i = 0; i < packages->length; i++ )
	{
		struct pkg* pkg = pkg_lookup(pkgs, pkgs_used, packages->strings[i]);
		if ( pkg )
			pkg->picked = true;
	}
	for ( size_t i = 0; i < packages->length; i++ )
	{
		const char* package = packages->strings[i];
		struct pkg* pkg = pkg_lookup(pkgs, pkgs_used, package);
		if ( !pkg )
			errx(1, "No such package: %s", package);
		char* copy = strdup(pkg->deps);
		if ( !copy )
			err(1, "malloc");
		char* state = copy;
		char* dep;
		while ( (dep = strsep(&state, " \t")) )
		{
			if ( !*dep )
				continue;
			if ( !strcmp(dep, "*") )
			{
				for ( size_t i = 0; i < pkgs_used; i++ )
					WantPackage(packages, pkgs[i]->name, pkgs, pkgs_used);
				continue;
			}
			else
				WantPackage(packages, dep, pkgs, pkgs_used);
		}
		free(copy);
	}
	qsort(packages->strings, packages->length, sizeof(char*), strcmp_indirect);
	for ( size_t i = 0; i < pkgs_used; i++ )
	{
		free(pkgs[i]->name);
		free(pkgs[i]->deps);
		free(pkgs[i]);
	}
	free(pkgs);
	free(dependencies_list);
	free(sha256sum);
	free(release_info);
	free(cache);
}

void InstallPackage(const char* package_name, string_array_t* fetch_argv)
{
	char* cache = join_paths(collection, "var/cache/tix");
	if ( !cache )
		err(1, "malloc");
	char* release_info = join_paths(cache, "release.info");
	char* sha256sum = join_paths(cache, "sha256sum");
	char* package_file = print_string("%s.tix.tar.xz", package_name);
	if ( !release_info || !sha256sum || !package_file )
		err(1, "malloc");
	if ( fork_and_wait_or_death() )
	{
		if ( quiet && !string_array_append(fetch_argv, "-q") )
			err(1, "malloc");
		const char* args[] =
		{
			"-C", collection,
			"-c",
			"-O", cache,
			"--input-release-info", release_info,
			"--input-sha256sum", sha256sum,
			package_file,
			NULL
		};
		for ( size_t i = 0; i < sizeof(args) / sizeof(args[0]); i++ )
			if ( !string_array_append(fetch_argv, args[i]) )
				err(1, "malloc");
		execvp(fetch_argv->strings[0], (char* const*) fetch_argv->strings);
		err(127, "%s", fetch_argv->strings[0]);
	}
	char* package_path = join_paths(cache, package_file);
	if ( !package_file )
		err(1, "malloc");
	InstallFile(package_path);
	unlink(package_path);
	free(package_path);
	free(package_file);
	free(sha256sum);
	free(release_info);
	free(cache);
}

void InstallFile(const char* tix_path)
{
	if ( !IsFile(tix_path) )
		err(1, "`%s'", tix_path);

	// TODO: After releasing Sortix 1.1, delete generation 2 compatibility.
	bool modern = true;
	const char* tixinfo_path = "tix/tixinfo/";
	if ( !TarContainsFile(tix_path, tixinfo_path) )
	{
		const char* tixinfo_path_old = "tix/tixinfo";
		if ( !TarContainsFile(tix_path, tixinfo_path_old) )
			errx(1, "`%s' doesn't contain a `%s' directory", tix_path,
			     tixinfo_path);
		tixinfo_path = tixinfo_path_old;
		modern = false;
	}

	string_array_t tixinfo = string_array_make();
	FILE* tixinfo_fp = TarOpenFile(tix_path, tixinfo_path);
	switch ( variables_append_file(&tixinfo, tixinfo_fp) )
	{
	case -1: err(1, "%s: %s", tix_path, tixinfo_path);
	case -2: errx(1, "%s: %s: Syntax error", tix_path, tixinfo_path);
	}

	fclose(tixinfo_fp);
	const char* version = dictionary_get(&tixinfo, "TIX_VERSION");
	if ( modern && (!version || strcmp(version, "3") != 0) )
		errx(1, "%s: unsupported TIX_VERSION: %s", tix_path, version);

	const char* package_name =
		dictionary_get(&tixinfo, modern ? "NAME" : "pkg.name");
	assert(package_name);

	const char* package_prefix =
		dictionary_get(&tixinfo, modern ? "PREFIX" : "pkg.prefix");
	assert(package_prefix || !package_prefix);

	const char* package_platform =
		dictionary_get(&tixinfo, modern ? "PLATFORM" : "tix.platform");
	assert(package_platform || !package_platform);

	bool already_installed =
		IsPackageInstalled(tix_directory_path, package_name);
	if ( already_installed && !reinstall )
		errx(1, "error: package `%s' is already installed. Use --reinstall "
		        "to force reinstallation.", package_name);

	if ( package_prefix && strcmp(coll_prefix, package_prefix) != 0 )
	{
		warnx("error: `%s' is compiled with the prefix `%s', "
		      "but the destination collection has the prefix `%s'.",
		      tix_path, package_prefix, coll_prefix);
		errx(1, "you need to recompile the package with "
		        "--prefix=\"%s\".", coll_prefix);
	}

	if ( package_platform && strcmp(coll_platform, package_platform) != 0 )
	{
		warnx("error: `%s' is compiled with the platform `%s', "
		      "but the destination collection has the platform `%s'.",
		      tix_path, package_platform, coll_platform);
		errx(1, "you need to recompile the package with "
		        "--host=%s\".", coll_platform);
	}

	if ( !quiet )
	{
		printf("Installing %s", package_name);
		if ( strcmp(collection, "/") != 0 )
			printf(" into `%s'", collection);
		printf("...\n");
		fflush(stdout);
	}

	const char* data = modern ? "" : "data";
	char* data_and_prefix = package_prefix && package_prefix[0] ?
	                        print_string("%s%s", data, package_prefix) :
	                        strdup(data);

	if ( !modern )
	{
		char* tixinfo_out_path =
			print_string("%s/tixinfo/%s", tix_directory_path, package_name);
		int tixinfo_fd =
			open(tixinfo_out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if ( tixinfo_fd < 0 )
			err(1, "%s", tixinfo_out_path);
		TarExtractFileToFD(tix_path, "tix/tixinfo", tixinfo_fd);
		close(tixinfo_fd);

		FILE* index_fp = TarOpenIndex(tix_path);
		string_array_t files = string_array_make();
		string_array_append_file(&files, index_fp);
		qsort(files.strings, files.length, sizeof(char*), strcmp_indirect);
		char* manifest_path =
			print_string("%s/manifest/%s", tix_directory_path, package_name);
		FILE* manifest_fp = fopen(manifest_path, "w");
		if ( !manifest_fp )
			err(1, "%s", manifest_path);
		for ( size_t i = 0; i < files.length; i++ )
		{
			char* str = files.strings[i];
			if ( strncmp(str, "data", strlen("data")) != 0 )
				continue;
			str += strlen("data");
			if ( str[0] && str[0] != '/' )
				continue;
			size_t len = strlen(str);
			while ( 2 <= len && str[len-1] == '/' )
				str[--len] = '\0';
			if ( fprintf(manifest_fp, "%s\n", str) < 0 )
				err(1, "%s", manifest_path);
		}
		if ( ferror(manifest_fp) || fflush(manifest_fp) == EOF )
			err(1, "%s", manifest_path);
		fclose(manifest_fp);
		string_array_reset(&files);
		fclose(index_fp);
	}

	if ( fork_and_wait_or_death() )
	{
		size_t num_strips = count_tar_components(data_and_prefix);
		const char* cmd_argv[] =
		{
			"tar",
			"-C", collection,
			"--extract",
			"--file", tix_path,
			"--keep-directory-symlink",
			"--same-permissions",
			"--no-same-owner",
			modern ? NULL : print_string("--strip-components=%zu", num_strips),
			modern ? NULL : data_and_prefix,
			NULL
		};
		execvp(cmd_argv[0], (char* const*) cmd_argv);
		err(127, "%s", cmd_argv[0]);
	}
	free(data_and_prefix);

	// TODO: After releasing Sortix 1.1, delete generation 2 compatibility.
	if ( generation <= 2 && !already_installed )
		MarkPackageAsInstalled(tix_directory_path, package_name);

	string_array_reset(&tixinfo);
}
