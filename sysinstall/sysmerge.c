/*
 * Copyright (c) 2016, 2018, 2020, 2021, 2022 Jonas 'Sortie' Termansen.
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

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "conf.h"
#include "execute.h"
#include "hooks.h"
#include "fileops.h"
#include "manifest.h"
#include "release.h"

static char* atcgetblob(int fd, const char* name, size_t* size_ptr)
{
	ssize_t size = tcgetblob(fd, name, NULL, 0);
	if ( size < 0 )
		return NULL;
	char* result = (char*) malloc((size_t) size + 1);
	if ( !result )
		return NULL;
	ssize_t second_size = tcgetblob(fd, name, result, (size_t) size);
	if ( second_size != size )
		return free(result), (char*) NULL;
	result[(size_t) size] = '\0';
	if ( size_ptr )
		*size_ptr = (size_t) size;
	return result;
}

static bool is_partition_name(const char* path)
{
	const char* name = path;
	for ( size_t i = 0; path[i]; i++ )
		if ( path[i] == '/' )
			name = path + i + 1;
	if ( !isalpha((unsigned char) *name) )
		return false;
	name++;
	while ( isalpha((unsigned char) *name) )
		name++;
	if ( !isdigit((unsigned char) *name) )
		return false;
	name++;
	while ( isdigit((unsigned char) *name) )
		name++;
	if ( *name != 'p' )
		return false;
	name++;
	if ( !isdigit((unsigned char) *name) )
		return false;
	name++;
	while ( isdigit((unsigned char) *name) )
		name++;
	return *name == '\0';
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

static bool has_pending_upgrade(void)
{
	return access_or_die("/boot/sortix.bin.sysmerge.orig", F_OK) == 0 ||
	       access_or_die("/boot/sortix.initrd.sysmerge.orig", F_OK) == 0 ||
	       access_or_die("/sysmerge", F_OK) == 0;
}

int main(int argc, char* argv[])
{
	setvbuf(stdout, NULL, _IOLBF, 0); // Pipes.

	bool booting = false;
	bool cancel = false;
	bool full = false;
	bool hook_finalize = false;
	bool hook_prepare = false;
	bool wait = false;

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
			case 'c': cancel = true; break;
			case 'f': full = true; break;
			case 'w': wait = true; break;
			default:
				errx(1, "unknown option -- '%c'", c);
			}
		}
		else if ( !strcmp(arg, "--booting") )
			booting = true;
		else if ( !strcmp(arg, "--cancel") )
			cancel = true;
		else if ( !strcmp(arg, "--full") )
			full = true;
		else if ( !strcmp(arg, "--hook-finalize") )
			hook_finalize = true;
		else if ( !strcmp(arg, "--hook-prepare") )
			hook_prepare = true;
		else if ( !strcmp(arg, "--wait") )
			wait = true;
		else
			errx(1, "unknown option: %s", arg);
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
		full = access_or_die("/sysmerge/tix/sysmerge.full", F_OK) == 0;
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
			       old_release.abi_major, old_release.abi_minor,
			       new_release.abi_major, new_release.abi_minor);
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

	// Upgrade hooks that runs before the old system is replaced.
	if ( run_prepare )
	{
		if ( my_prepare )
			upgrade_prepare(&old_release, &new_release, source, "");
		else
		{
			char* new_sysmerge = join_paths(source, "sbin/sysmerge");
			if ( !new_sysmerge )
				err(2, "malloc");
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
		install_manifests_detect(source, target, true, true, full);
	}

	if ( wait )
	{
		printf(" - Scheduling upgrade on next boot...\n");
		if ( full )
		{
			int fd = open("/sysmerge/tix/sysmerge.full", O_WRONLY | O_CREAT);
			if ( fd < 0 )
				err(1, "/sysmerge/tix/sysmerge.full");
			close(fd);
		}
		execute((const char*[]) { "cp", "/boot/sortix.bin",
		                                "/boot/sortix.bin.sysmerge.orig",
		                                NULL }, "e");
		execute((const char*[]) { "cp", "/boot/sortix.initrd",
		                                "/boot/sortix.initrd.sysmerge.orig",
		                                NULL }, "e");
		execute((const char*[]) { "cp", "/sysmerge/boot/sortix.bin",
		                                "/boot/sortix.bin", NULL }, "e");
		execute((const char*[]) { "/sysmerge/sbin/update-initrd", NULL }, "e");

		printf("The system will be upgraded to %s on the next boot.\n",
		       new_release.pretty_name);
		printf("Run %s --cancel to cancel the upgrade.\n", argv[0]);

		return 0;
	}

	// Upgrade hooks that run after the new system is installed.
	if ( run_finalize )
	{
		if ( my_finalize )
			upgrade_finalize(&old_release, &new_release, source, "");
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
			int boot_fd = open("/boot", O_RDONLY);
			if ( boot_fd < 0 )
				err(2, "/boot");
			char* boot_device = atcgetblob(boot_fd, "device-path", NULL);
			if ( !boot_device )
				err(2, "Failed to find device of filesystem: /boot");
			close(boot_fd);
			// TODO: A better design for finding the parent block device of a
			//       partition without scanning every block device.
			if ( is_partition_name(boot_device) )
				*strrchr(boot_device, 'p') = '\0';
			printf(" - Installing bootloader...\n");
			execute((const char*[]) { "grub-install", boot_device,
			                          NULL },"eqQ");
			free(boot_device);
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
