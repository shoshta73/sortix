/*
 * Copyright (c) 2013, 2015, 2016, 2023, 2024, 2025 Jonas 'Sortie' Termansen.
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
 * tix-create.c
 * Administer and configure a tix collection.
 */

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

static void download_release_key(const char* release_url, const char* tix_path,
                                 bool quiet)
{
	if ( strncmp(release_url, "https://", strlen("https://")) != 0 )
		errx(1, "error: Public keys can only be downloaded via HTTPS: %s",
		        release_url);
	char* release_pub_path = join_paths(tix_path, "release.pub");
	char* release_pub_url = join_paths(release_url, "release.pub");
	if ( !release_pub_path || !release_pub_url )
		err(1, "malloc");
	if ( !quiet )
	{
		printf(" - Downloading public key... %s\n", release_pub_url);
		fflush(stdout);
	}
	pid_t pid = fork();
	if ( pid < 0 )
		err(1, "fork");
	if ( !pid )
	{
		execlp("wget", "wget", "-q", release_pub_url, "-O", release_pub_path,
		       NULL);
		warn("wget");
		_exit(127);
	}
	int status;
	waitpid(pid, &status, 0);
	if ( !WIFEXITED(status) || WEXITSTATUS(status) )
	{
		const char* m = "Unknown exit";
		if ( WIFEXITED(status) )
		{
			switch ( WEXITSTATUS(status) )
			{
			case 1: m = "Generic error"; break;
			case 2: m = "Parse error"; break;
			case 3: m = "File I/O error"; break;
			case 4: m = "Network I/O error"; break;
			case 5: m = "Transport Layer Security verification failure"; break;
			case 6: m = "Username/password failure"; break;
			case 7: m = "Protocol error"; break;
			case 8: m = "Error response"; break;
			case 127: m = "No such program"; break;
			default: m = "Unknown error"; break;
			}
		}
		else if ( WIFSIGNALED(status) )
			m = strsignal(status);
		errx(1, "error: Download failed: %s -> %s: wget: %s",
		     release_pub_url, release_pub_path, m);
	}
	free(release_pub_path);
	free(release_pub_url);
}

static void copy_release_key(const char* release_key, const char* tix_path)
{
	if ( access(release_key, F_OK) < 0 && errno == ENOENT )
		return;
	char* release_pub_path = join_paths(tix_path, "release.pub");
	if ( !release_pub_path )
		err(1, "malloc");
	pid_t pid = fork();
	if ( pid < 0 )
		err(1, "fork");
	if ( !pid )
	{
		execlp("cp", "cp", release_key, "--", release_pub_path, NULL);
		warn("cp");
		_exit(127);
	}
	int status;
	waitpid(pid, &status, 0);
	if ( !WIFEXITED(status) || WEXITSTATUS(status) )
		errx(1, "Copy failed: %s -> %s", release_key, release_pub_path);
	free(release_pub_path);
}

int main(int argc, char* argv[])
{
	// TODO: After releasing Sortix 1.1, remove tix-collection backwards
	//       compatibility.
	bool is_tix_collection =
		!strcmp("tix-collection", non_modify_basename(argv[0]));

	const char* build_id = NULL;
	const char* collection = "/";
	const char* import = NULL;
	int force_mirror = -1;
	const char* generation_string = DEFAULT_GENERATION;
	const char* mirror = NULL;
	const char* platform = NULL;
	const char* prefix = NULL;
	bool quiet = false;
	const char* release_key = NULL;
	const char* release_url = NULL;
	enum
	{
		OPT_BUILD_ID = 256,
		OPT_IMPORT = 257,
		OPT_FORCE_MIRROR = 258,
		OPT_GENERATION = 259,
		OPT_MIRROR = 260,
		OPT_PLATFORM = 261,
		OPT_PREFIX = 262,
		OPT_REINSTALL = 263,
		OPT_RELEASE_KEY = 264,
		OPT_DISABLE_MULTIARCH = 300,
	};
	const struct option longopts[] =
	{
		{"build-id", required_argument, NULL, OPT_BUILD_ID},
		{"collection", required_argument, NULL, 'C'},
		{"force-mirror", optional_argument, NULL, OPT_FORCE_MIRROR},
		{"generation", required_argument, NULL, OPT_GENERATION},
		{"import", required_argument, NULL, OPT_IMPORT},
		{"mirror", required_argument, NULL, OPT_MIRROR},
		{"platform", required_argument, NULL, OPT_PLATFORM},
		{"prefix", required_argument, NULL, OPT_PREFIX},
		{"quiet", no_argument, NULL, 'q'},
		{"release-key", required_argument, NULL, OPT_RELEASE_KEY},
		{"release-url", required_argument, NULL, 'u'},
		// TODO: After releasing Sortix 1.1, delete this compatibility that lets
		//       Sortix 1.0 build. This option used to disable compatibility
		//       with Sortix 0.9.
		{"disable-multiarch", no_argument, NULL, OPT_DISABLE_MULTIARCH},
		{0, 0, 0, 0}
	};
	// TODO: After releasing Sortix 1.1, remove this compatibility.
#if 1
	const char* cmd = NULL;
	if ( is_tix_collection )
	{
		optind = 1;
		if ( 1 <= argc - optind && argv[optind][0] != '-' &&
			IsCollectionPrefixRatherThanCommand(argv[optind]) )
			collection = argv[optind++];
		if ( 1 <= argc - optind && argv[optind][0] != '-' )
			cmd = argv[optind++];
	}
#endif
	const char* opts = "C:u:q";
	int opt;
	while ( (opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1 )
	{
		switch ( opt )
		{
		case 'C': collection = optarg; break;
		case 'q': quiet = true; break;
		case 'u': release_url = optarg; break;
		case OPT_BUILD_ID: build_id = optarg; break;
		case OPT_DISABLE_MULTIARCH: break;
		case OPT_FORCE_MIRROR:
			force_mirror = !optarg || !strcmp(optarg, "true"); break;
		case OPT_GENERATION: generation_string = optarg; break;
		case OPT_IMPORT: import = optarg; break;
		case OPT_MIRROR: mirror = optarg; break;
		case OPT_PLATFORM: platform = optarg; break;
		case OPT_PREFIX: prefix = optarg; break;
		case OPT_RELEASE_KEY: release_key = optarg; break;
		default: return 1;
		}
	}

	// TODO: After releasing Sortix 1.1, remove this compatibility.
	if ( is_tix_collection && 1 <= optind - argc &&
	     IsCollectionPrefixRatherThanCommand(argv[optind]) )
		collection = argv[optind++];

	if ( is_tix_collection && !cmd )
	{
		if ( optind == argc )
			errx(1, "error: No command specified");
		cmd = argv[optind++];
	}

	string_array_t conf_from = string_array_make();
	if ( import )
	{
		char* conf_from_path = join_paths(import, "tix/collection.conf");
		if ( !conf_from_path )
			err(1, "malloc");
		switch ( variables_append_file_path(&conf_from, conf_from_path) )
		{
		case -1: err(1, "%s", conf_from_path);
		case -2: errx(1, "%s: Syntax error", conf_from_path);
		}
		free(conf_from_path);
		if ( !prefix && dictionary_get(&conf_from, "PREFIX") &&
		     !(prefix = dictionary_get(&conf_from, "PREFIX")) )
			err(1, "malloc");
		if ( !platform && dictionary_get(&conf_from, "PLATFORM") &&
		     !(platform = dictionary_get(&conf_from, "PLATFORM")) )
			err(1, "malloc");
		if ( !release_key &&
		     !(release_key = join_paths(import, "tix/release.pub")) )
			err(1, "malloc");
		if ( !generation_string &&
		     dictionary_get(&conf_from, "TIX_COLLECTION_VERSION") &&
		     !(generation_string =
		       dictionary_get(&conf_from, "TIX_COLLECTION_VERSION")) )
			err(1, "malloc");
	}

	int generation = atoi(generation_string);
	// TODO: After releasing Sortix 1.1, remove generation 2 compatibility.
	if ( generation != 2 && generation != 3 )
		errx(1, "Unsupported generation: %i", generation);

	char* conf_path = join_paths(collection, "tix/collection.conf");
	if ( !conf_path )
		err(1, "malloc");
	string_array_t conf = string_array_make();
	if ( !is_tix_collection )
		cmd = !access(conf_path, F_OK) ? "set" : "create";
	if ( strcmp(cmd, "create") != 0 )
	{
		switch ( variables_append_file_path(&conf, conf_path) )
		{
		case -1: err(1, "%s", conf_path);
		case -2: errx(1, "%s: Syntax error", conf_path);
		}
	}

	// TODO: Unify create and set logic once compatibility has been dropped.
	if ( !strcmp(cmd, "create") )
	{
		if ( optind != argc )
			errx(1, "unexpected extra operand: %s", argv[optind]);

		if ( !platform && !(platform = GetBuildTriplet()) )
			err(1, "unable to determine platform, use --platform");

		if ( mkdir_p(collection, 0755) != 0 )
			err(1, "mkdir: `%s'", collection);

		if ( !prefix )
		{
			if ( is_absolute_path(collection) )
				prefix = collection;
			else if ( !(prefix = realpath(collection, NULL)) )
				err(1, "realpath: %s", collection);
			if ( !strcmp(prefix, "/") )
				prefix = "";
		}

		char* tix_path = join_paths(collection, "tix");
		if ( mkdir_p(tix_path, 0755) != 0 )
			err(1, "mkdir: `%s'", tix_path);

		char* tixinfo_path = join_paths(tix_path, "tixinfo");
		if ( mkdir_p(tixinfo_path, 0755) != 0 )
			err(1, "mkdir: `%s'", tixinfo_path);
		free(tixinfo_path);

		char* manifest_path = join_paths(tix_path, "manifest");
		if ( mkdir_p(manifest_path, 0755) != 0 )
			err(1, "mkdir: `%s'", manifest_path);
		free(manifest_path);

		char* collection_conf_path = join_paths(tix_path, "collection.conf");
		FILE* conf_fp = fopen(collection_conf_path, "wx");
		if ( !conf_fp && errno == EEXIST )
			errx(1, "error: `%s' already exists, a tix collection is "
			        "already installed at `%s'.", collection_conf_path,
			        collection);
		if ( 3 <= generation )
		{
			fwrite_variable(conf_fp, "TIX_COLLECTION_VERSION", "3");
			fwrite_variable(conf_fp, "PREFIX",
			                !strcmp(prefix, "/") ? "" : prefix);
			fwrite_variable(conf_fp, "PLATFORM", platform);
			if ( build_id )
				fwrite_variable(conf_fp, "BUILD_ID", build_id);
			if ( release_url )
				fwrite_variable(conf_fp, "RELEASE_URL", release_url);
			if ( mirror )
				fwrite_variable(conf_fp, "MIRROR", mirror);
			if ( 0 <= force_mirror )
				fwrite_variable(conf_fp, "FORCE_MIRROR",
				                force_mirror ? "true" : "false");
			for ( size_t i = 0; i < conf_from.length; i++ )
			{
				char* key = conf_from.strings[i];
				char* eq = strchr(key, '=');
				assert(eq);
				*eq = '\0';
				char* value = eq + 1;
				if ( !strcmp(key, "TIX_COLLECTION_VERSION") ||
				     !strcmp(key, "PREFIX") ||
				     !strcmp(key, "PLATFORM") ||
				     (build_id && !strcmp(key, "BUILD_ID")) ||
				     (release_url && !strcmp(key, "RELEASE_URL")) ||
				     (release_url && !strcmp(key, "MIRROR")) ||
				     (0 <= force_mirror && !strcmp(key, "FORCE_MIRROR")) )
					continue;
				fwrite_variable(conf_fp, key, value);
			}

			if ( release_key )
				copy_release_key(release_key, tix_path);
			else if ( release_url )
				download_release_key(release_url, tix_path, quiet);
		}
		// TODO: After releasing Sortix 1.1, delete generation 2 compatibility.
		else
		{
			fprintf(conf_fp, "tix.version=1\n");
			fprintf(conf_fp, "tix.class=collection\n");
			fprintf(conf_fp, "collection.generation=%i\n", generation);
			fprintf(conf_fp, "collection.prefix=%s\n",
			        !strcmp(prefix, "/") ? "" : prefix);
			fprintf(conf_fp, "collection.platform=%s\n", platform);
		}
		fclose(conf_fp);
		free(collection_conf_path);

		// TODO: After releasing Sortix 1.1, delete generation 2 compatibility.
		if ( generation < 3 )
		{
			const char* repo_list_path = join_paths(tix_path, "repository.list");
			FILE* repo_list_fp = fopen(repo_list_path, "w");
			if ( !repo_list_fp )
				err(1, "`%s'", repo_list_path);
			fclose(repo_list_fp);

			const char* inst_list_path = join_paths(tix_path, "installed.list");
			FILE* inst_list_fp = fopen(inst_list_path, "w");
			if ( !inst_list_fp )
				err(1, "`%s'", inst_list_path);
			fclose(inst_list_fp);
		}

		return 0;
	}
	else if ( !strcmp(cmd, "set") )
	{
		if ( optind != argc )
			errx(1, "unexpected extra operand: %s", argv[optind]);

		for ( size_t i = 0; i < conf_from.length; i++ )
		{
			char* key = conf_from.strings[i];
			char* eq = strchr(key, '=');
			assert(eq);
			*eq = '\0';
			char* value = eq + 1;
			if ( !dictionary_set(&conf, key, value) )
				err(1, "malloc");
			*eq = '=';
		}

		if ( (prefix && !dictionary_set(&conf, "PREFIX", prefix)) ||
		     (platform && !dictionary_set(&conf, "PLATFORM", platform)) ||
		     (release_url &&
		      !dictionary_set(&conf, "RELEASE_URL", release_url)) ||
		     (mirror && !dictionary_set(&conf, "MIRROR", mirror)) ||
		     (0 <= force_mirror &&
		      !dictionary_set(&conf, "FORCE_MIRROR",
		                      force_mirror ? "true" : "false")) )
			err(1, "malloc");

		char* tix_path = join_paths(collection, "tix");
		if ( mkdir_p(tix_path, 0755) != 0 )
			err(1, "mkdir: `%s'", tix_path);
		char* conf_path_new = join_paths(tix_path, "collection.conf.new");
		if ( !conf_path_new )
			err(1, "malloc");
		FILE* conf_fp = fopen(conf_path_new, "w");
		if ( !conf_fp )
			err(1, "%s", conf_path_new);

		for ( size_t i = 0; i < conf.length; i++ )
		{
			char* key = conf.strings[i];
			char* eq = strchr(key, '=');
			assert(eq);
			*eq = '\0';
			char* value = eq + 1;
			fwrite_variable(conf_fp, key, value);
			*eq = '=';
		}

		if ( ferror(conf_fp) || fflush(conf_fp) == EOF )
			err(1, "%s", conf_path_new);
		struct stat st;
		if ( stat(conf_path, &st) < 0 )
			err(1, "stat: %s", conf_path);
		fchmod(fileno(conf_fp), st.st_mode & 07777);
		(void) fchown(fileno(conf_fp), st.st_uid, st.st_gid);
		fclose(conf_fp);

		if ( release_key )
			copy_release_key(release_key, tix_path);
		else if ( release_url )
			download_release_key(release_url, tix_path, quiet);

		if ( rename(conf_path_new, conf_path) < 0 )
			err(1, "rename: %s -> %s", conf_path_new, conf_path);
	}
	else
		errx(1, "error: Unknown command: %s", cmd);

	return 0;
}
