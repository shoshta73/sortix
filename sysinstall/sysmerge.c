/*
 * Copyright (c) 2016, 2017, 2018 Jonas 'Sortie' Termansen.
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
 * sysmerge.c
 * Upgrade current operating system from a sysroot.
 */

#include <sys/stat.h>
#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "conf.h"
#include "execute.h"
#include "hooks.h"
#include "fileops.h"
#include "manifest.h"
#include "release.h"

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

static bool has_pending_upgrade(void)
{
	return access_or_die("/boot/sortix.bin.sysmerge.orig", F_OK) == 0 ||
	       access_or_die("/boot/sortix.initrd.sysmerge.orig", F_OK) == 0 ||
	       access_or_die("/sysmerge", F_OK) == 0;
}

static void help(FILE* fp, const char* argv0)
{
	fprintf(fp, "Usage: %s [OPTION]... SOURCE\n", argv0);
	fprintf(fp, "Merge the files from SOURCE onto the current system.\n");
}

static void version(FILE* fp, const char* argv0)
{
	fprintf(fp, "%s (Sortix) %s\n", argv0, VERSIONSTR);
}

int main(int argc, char* argv[])
{
	setvbuf(stdout, NULL, _IOLBF, 0); // Pipes.

	bool booting = false;
	bool cancel = false;
	bool hook_finalize = false;
	bool hook_prepare = false;
	const char* repository = NULL;
	bool wait = false;

	const char* argv0 = argv[0];
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
			case 'c': cancel = false; break;
			case 'w': wait = true; break;
			default:
				fprintf(stderr, "%s: unknown option -- '%c'\n", argv0, c);
				help(stderr, argv0);
				exit(2);
			}
		}
		else if ( !strcmp(arg, "--help") )
			help(stdout, argv0), exit(0);
		else if ( !strcmp(arg, "--version") )
			version(stdout, argv0), exit(0);
		else if ( !strcmp(arg, "--booting") )
			booting = true;
		else if ( !strcmp(arg, "--cancel") )
			cancel = true;
		else if ( !strcmp(arg, "--hook-finalize") )
			hook_finalize = true;
		else if ( !strcmp(arg, "--hook-prepare") )
			hook_prepare = true;
		else if ( !strncmp(arg, "--repository=", strlen("--repository=")) )
			repository = arg + strlen("--repository=");
		else if ( !strcmp(arg, "--repository") )
		{
			if ( i + 1 == argc )
				errx(2, "option '--repository' requires an argument");
			repository = argv[i+1];
			argv[++i] = NULL;
		}
		else if ( !strcmp(arg, "--wait") )
			wait = true;
		else
		{
			fprintf(stderr, "%s: unknown option: %s\n", argv0, arg);
			help(stderr, argv0);
			exit(2);
		}
	}

	compact_arguments(&argc, &argv);

	if ( 1 < booting + cancel + hook_finalize + hook_prepare + wait )
		errx(2, "Mutually incompatible options were passed");

	bool hook_only = hook_prepare || hook_finalize;
	bool no_source = cancel;
	bool no_cancel = booting || hook_only;

	const char* source;
	if ( no_source )
	{
		source = "";
		if ( 1 < argc )
			errx(2, "Unexpected extra operand `%s'", argv[1]);
	}
	else if ( booting )
	{
		source = "/sysmerge";
		if ( 1 < argc )
			errx(2, "Unexpected extra operand `%s'", argv[1]);
		if ( !repository )
		{
			if ( access("/sysmerge/repository", R_OK) == 0 )
				repository = "/sysmerge/repository";
			else if ( errno != ENOENT )
				warn("%s", "/sysmerge/repository");
		}
	}
	else
	{
		if ( argc < 2 )
			errx(2, "No source operand was given");
		source = argv[1];
		if ( 2 < argc )
			errx(2, "Unexpected extra operand `%s'", argv[2]);
	}

	bool did_cancel = false;
	if ( !no_cancel && has_pending_upgrade() )
	{
		rename("/boot/sortix.bin.sysmerge.orig", "/boot/sortix.bin");
		rename("/boot/sortix.initrd.sysmerge.orig", "/boot/sortix.initrd");
		execute((const char*[]) { "rm", "-rf", "/sysmerge", NULL }, "");
		execute((const char*[]) { "update-initrd", NULL }, "e");
		printf("Cancelled pending system upgrade.\n");
		did_cancel = true;
	}

	if ( cancel )
	{
		if ( !did_cancel )
			printf("No system upgrade was pending.\n");
		return 0;
	}

	const char* old_release_path = "/etc/sortix-release";
	struct release old_release;
	if ( !os_release_load(&old_release, old_release_path, old_release_path) )
	{
		if ( errno == ENOENT )
			warn("%s", old_release_path);
		exit(2);
	}

	char* new_release_path;
	if ( asprintf(&new_release_path, "%s/etc/sortix-release", source) < 0 )
		err(2, "asprintf");
	struct release new_release;
	if ( !os_release_load(&new_release, new_release_path, new_release_path) )
	{
		if ( errno == ENOENT )
			warn("%s", new_release_path);
		exit(2);
	}
	free(new_release_path);

	const char* old_machine_path = "/etc/machine";
	char* old_machine = read_string_file(old_machine_path);
	if ( !old_machine )
		err(2, "%s", old_machine_path);
	char* new_machine_path;
	if ( asprintf(&new_machine_path, "%s/etc/machine", source) < 0 )
		err(2, "asprintf");
	char* new_machine = read_string_file(new_machine_path);
	if ( !new_machine )
		err(2, "%s", new_machine_path);
	if ( strcmp(old_machine, new_machine) != 0 )
		errx(2, "%s (%s) does not match %s (%s)", new_machine_path,
		     new_machine, old_machine_path, old_machine);
	free(old_machine);
	free(new_machine_path);
	free(new_machine);

	// TODO: Check for version (skipping, downgrading).

	struct conf conf;
	conf_init(&conf);
	if ( !conf_load(&conf, "/etc/upgrade.conf") && errno == ENOENT )
		err(2, "/etc/upgrade.conf");

	bool can_run_new_abi =
		abi_compatible(new_release.abi_major, new_release.abi_minor,
		               old_release.abi_major, old_release.abi_minor);

	bool header;
	bool copy_files;
	bool run_prepare;
	bool run_finalize;
	bool my_prepare;
	bool my_finalize;
	if ( booting )
	{
		header = true;
		copy_files = true;
		run_prepare = true;
		my_prepare = true;
		run_finalize = true;
		my_finalize = true;
	}
	else if ( hook_prepare )
	{
		header = false;
		copy_files = false;
		run_prepare = true;
		my_prepare = true;
		run_finalize = false;
		my_finalize = false;
	}
	else if ( hook_finalize )
	{
		header = false;
		copy_files = false;
		run_prepare = false;
		my_prepare = false;
		run_finalize = true;
		my_finalize = true;
	}
	else
	{
		if ( !wait && !can_run_new_abi )
		{
			printf("%lu.%lu -> %lu.%lu ABI transition, "
				   "delaying upgrade to next boot.\n",
				   old_release.abi_major, old_release.abi_major,
				   new_release.abi_major, new_release.abi_major);
			wait = true;
		}
		header = true;
		copy_files = true;
		run_prepare = !wait;
		my_prepare = false;
		run_finalize = !wait;
		my_finalize = false;
	}

	if ( header )
	{
		if ( wait )
			printf("Scheduling upgrade to %s on next boot using %s:\n",
			       new_release.pretty_name, source);
		else
			printf("Upgrading to %s using %s:\n",
			       new_release.pretty_name, source);
	}

	// Compatibility hooks that runs before the old system is replaced.
	if ( run_prepare )
	{
		if ( my_prepare )
		{
			upgrade_prepare(&old_release, &new_release, source, "/");
		}
		else
		{
			char* new_sysmerge = join_paths(source, "sbin/sysmerge");
			if ( !new_sysmerge )
				err(2, "asprintf");
			execute((const char*[]) { new_sysmerge, "--hook-prepare", source,
			                          NULL }, "e");
			free(new_sysmerge);
		}
		if ( hook_prepare )
			return 0;
	}

	if ( copy_files )
	{
		// TODO: Update /etc/upgrade.conf with new release values.
		// TODO: What about native upgrades using make sysmerge? Should those
		//       values be updated then? Should there be an option to control
		//       this behavior?
		const char* target = "";
		if ( wait )
		{
			target = "/sysmerge";
			if ( mkdir(target, 0755) < 0 )
				err(2, "%s", target);
			execute((const char*[]) { "tix-collection", "/sysmerge", "create",
				                      NULL }, "e");
		}
		install_manifest("system", source, target);
		install_ports(source, target, "", repository, wait);
	}

	if ( wait )
	{
		printf(" - Scheduling upgrade on next boot...\n");
		execute((const char*[]) { "cp", "/boot/sortix.bin",
		                                "/boot/sortix.bin.sysmerge.orig", NULL }, "e");
		execute((const char*[]) { "cp", "/boot/sortix.initrd",
		                                "/boot/sortix.initrd.sysmerge.orig", NULL }, "e");
		execute((const char*[]) { "cp", "/sysmerge/boot/sortix.bin",
		                                "/boot/sortix.bin", NULL }, "e");
		execute((const char*[]) { "/sysmerge/sbin/update-initrd", NULL }, "e");

		printf("The system will be upgraded to %s on the next boot.\n",
		       new_release.pretty_name);
		printf("Run %s --cancel to cancel the upgrade.\n", argv[0]);

		return 0;
	}

	// Compatibility hooks that run after the new system is installed.
	if ( run_finalize )
	{
		if ( my_finalize )
		{
			upgrade_finalize(&old_release, &new_release, source, "/");
			post_install_ports("/");
		}
		else
		{
			char* new_sysmerge = join_paths(source, "sbin/sysmerge");
			if ( !new_sysmerge )
				err(2, "asprintf");
			execute((const char*[]) { new_sysmerge, "--hook-finalize", source,
			                          NULL }, "e");
			free(new_sysmerge);
		}
		if ( hook_finalize )
			return 0;
	}

	if ( booting )
	{
		unlink("/boot/sortix.bin.sysmerge.orig");
		unlink("/boot/sortix.initrd.sysmerge.orig");
		execute((const char*[]) { "rm", "-rf", "/sysmerge", NULL }, "");
	}

	if ( access_or_die("/etc/fstab", F_OK) == 0 )
	{
		printf(" - Creating initrd...\n");
		execute((const char*[]) { "update-initrd", NULL }, "e");

		if ( conf.grub )
		{
			// TODO: Figure out the root device.
			//printf(" - Installing bootloader...\n");
			//execute((const char*[]) { "grub-install", "/", NULL }, "eqQ");
			printf(" - Configuring bootloader...\n");
			execute((const char*[]) { "update-grub", NULL }, "eqQ");
		}
		else if ( access_or_die("/etc/grub.d/10_sortix", F_OK) == 0 )
		{
			printf(" - Creating bootloader fragment...\n");
			execute((const char*[]) { "/etc/grub.d/10_sortix", NULL }, "eq");
		}
	}

	printf("Successfully upgraded to %s.\n", new_release.pretty_name);

	return 0;
}
