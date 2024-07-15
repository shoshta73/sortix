/*
 * Copyright (c) 2013, 2015, 2016 Jonas 'Sortie' Termansen.
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
 * tix-collection.c
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

static void help(FILE* fp, const char* argv0)
{
	fprintf(fp, "Usage: %s [PREFIX] [OPTION]... COMMAND\n", argv0);
	fprintf(fp, "Administer and configure a tix collection.\n");
}

static void version(FILE* fp, const char* argv0)
{
	fprintf(fp, "%s (Sortix) %s\n", argv0, VERSIONSTR);
}

int main(int argc, char* argv[])
{
	char* collection = NULL;
	char* conf_from_prefix = NULL;
	char* platform = NULL;
	char* prefix = NULL;
	char* generation_string = strdup(DEFAULT_GENERATION);

	const char* argv0 = argv[0];
	for ( int i = 0; i < argc; i++ )
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
			default:
				fprintf(stderr, "%s: unknown option -- '%c'\n", argv0, c);
				help(stderr, argv0);
				exit(1);
			}
		}
		else if ( !strcmp(arg, "--help") )
			help(stdout, argv0), exit(0);
		else if ( !strcmp(arg, "--version") )
			version(stdout, argv0), exit(0);
		else if ( GET_OPTION_VARIABLE("--collection", &collection) ) { }
		else if ( GET_OPTION_VARIABLE("--conf-from", &conf_from_prefix) ) { }
		else if ( GET_OPTION_VARIABLE("--platform", &platform) ) { }
		else if ( GET_OPTION_VARIABLE("--prefix", &prefix) ) { }
		else if ( GET_OPTION_VARIABLE("--generation", &generation_string) ) { }
		else if ( !strcmp(arg, "--disable-multiarch") )
		{
			// TODO: After releasing Sortix 1.1, delete this compatibility that
			//       lets Sortix 1.0 build. This option used to disable
			//       compatibility with Sortix 0.9.
		}
		else
		{
			fprintf(stderr, "%s: unknown option: %s\n", argv0, arg);
			help(stderr, argv0);
			exit(1);
		}
	}

	if ( argc == 1 )
	{
		help(stdout, argv0);
		exit(0);
	}

	compact_arguments(&argc, &argv);

	ParseOptionalCommandLineCollectionPrefix(&collection, &argc, &argv);

	if ( argc == 1 )
	{
		warnx("error: no command specified.");
		exit(1);
	}

	// The collection directory might not exist yet.
	if ( strcmp(argv[1], "create") != 0 )
		VerifyCommandLineCollection(&collection);

	int generation = atoi(generation_string);
	free(generation_string);
	// TODO: After releasing Sortix 1.1, remove generation 2 compatibility.
	if ( generation != 2 && generation != 3 )
		errx(1, "Unsupported generation: %i", generation);

	if ( !prefix )
		prefix = strdup(collection);

	string_array_t conf = string_array_make();
	if ( strcmp(argv[1], "create") != 0 )
	{
		char* conf_path = join_paths(collection, "tix/collection.conf");
		if ( !conf_path )
			err(1, "malloc");
		switch ( variables_append_file_path(&conf, conf_path) )
		{
		case -1: err(1, "%s", conf_path);
		case -2: errx(1, "%s: Syntax error", conf_path);
		}
		free(conf_path);
	}

	string_array_t conf_from = string_array_make();
	if ( conf_from_prefix )
	{
		char* conf_from_path =
			join_paths(conf_from_prefix, "tix/collection.conf");
		if ( !conf_from_path )
			err(1, "malloc");
		switch ( variables_append_file_path(&conf_from, conf_from_path) )
		{
		case -1: err(1, "%s", conf_from_path);
		case -2: errx(1, "%s: Syntax error", conf_from_path);
		}
		free(conf_from_path);
	}

	const char* cmd = argv[1];
	if ( !strcmp(cmd, "create") )
	{
		if ( platform && dictionary_get(&conf, "PLATFORM") &&
		     !(platform = strdup(dictionary_get(&conf, "PLATFORM"))) )
			err(1, "malloc");

		if ( !platform && !(platform = GetBuildTriplet()) )
			err(1, "unable to determine platform, use --platform");

		if ( mkdir_p(collection, 0755) != 0 )
			err(1, "mkdir: `%s'", collection);

		VerifyCommandLineCollection(&collection);

		char* tix_path = join_paths(collection, "tix");
		if ( mkdir_p(tix_path, 0755) != 0 )
			err(1, "mkdir: `%s'", tix_path);

		char* tixdb_path = strdup(tix_path);

		char* tixinfo_path = join_paths(tixdb_path, "tixinfo");
		if ( mkdir_p(tixinfo_path, 0755) != 0 )
			err(1, "mkdir: `%s'", tixinfo_path);
		free(tixinfo_path);

		char* manifest_path = join_paths(tixdb_path, "manifest");
		if ( mkdir_p(manifest_path, 0755) != 0 )
			err(1, "mkdir: `%s'", manifest_path);
		free(manifest_path);

		char* collection_conf_path = join_paths(tixdb_path, "collection.conf");
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
			for ( size_t i = 0; i < conf_from.length; i++ )
			{
				char* key = conf_from.strings[i];
				char* eq = strchr(key, '=');
				assert(eq);
				*eq = '\0';
				char* value = eq + 1;
				if ( !strcmp(key, "TIX_COLLECTION_VERSION") ||
				     !strcmp(key, "PREFIX") ||
				     !strcmp(key, "PLATFORM") )
					continue;
				fwrite_variable(conf_fp, key, value);
			}
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
			const char* repo_list_path = join_paths(tixdb_path, "repository.list");
			FILE* repo_list_fp = fopen(repo_list_path, "w");
			if ( !repo_list_fp )
				err(1, "`%s'", repo_list_path);
			fclose(repo_list_fp);

			const char* inst_list_path = join_paths(tixdb_path, "installed.list");
			FILE* inst_list_fp = fopen(inst_list_path, "w");
			if ( !inst_list_fp )
				err(1, "`%s'", inst_list_path);
			fclose(inst_list_fp);
		}

		return 0;
	}
	else if ( !strcmp(cmd, "set") )
	{
		// TODO: Apply options like --platform.
		char* conf_path = join_paths(collection, "tix/collection.conf");
		char* conf_path_new = join_paths(collection, "tix/collection.conf.new");
		if ( !conf_path || !conf_path_new )
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
			if ( !strcmp(key, "TIX_COLLECTION_VERSION") ||
			     !strcmp(key, "PREFIX") ||
			     !dictionary_get(&conf_from, key) )
				fwrite_variable(conf_fp, key, value);
			*eq = '=';
		}
		for ( size_t i = 0; i < conf_from.length; i++ )
		{
			char* key = conf_from.strings[i];
			char* eq = strchr(key, '=');
			assert(eq);
			*eq = '\0';
			char* value = eq + 1;
			if ( strcmp(key, "TIX_COLLECTION_VERSION") != 0 &&
			     strcmp(key, "PREFIX") != 0 )
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
		if ( rename(conf_path_new, conf_path) < 0 )
			err(1, "rename: %s -> %s", conf_path_new, conf_path);
	}
	else
	{
		fprintf(stderr, "%s: unknown command: `%s'\n", argv0, cmd);
		exit(1);
	}

	return 0;
}
