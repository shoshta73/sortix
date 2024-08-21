/*
 * Copyright (c) 2016, 2018, 2020, 2021, 2022, 2023 Jonas 'Sortie' Termansen.
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
#include <getopt.h>
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

static bool has_pending_upgrade(const char* target)
{
	char* sysmerge = join_paths(target, "sysmerge");
	char* boot_sysmerge = join_paths(target, "boot/sysmerge");
	if ( !sysmerge || !boot_sysmerge )
		err(2, "malloc");
	bool result = access_or_die(sysmerge, F_OK) == 0 ||
	              access_or_die(boot_sysmerge, F_OK) == 0;
	free(sysmerge);
	free(boot_sysmerge);
	return result;
}

static void update_grub(struct conf* conf, const char* target)
{
	if ( conf->grub )
	{
		printf(" - Configuring bootloader...\n");
		execute((const char*[]) { "update-grub", NULL }, "ceqQ", target);
	}
	else if ( access_or_die("/etc/default/grub.d/10_sortix", F_OK) == 0 )
	{
		printf(" - Creating bootloader fragment...\n");
		execute((const char*[]) { "/etc/default/grub.d/10_sortix", NULL },
		        "ceq", target);
	}
}

int main(int argc, char* argv[])
{
	setvbuf(stdout, NULL, _IOLBF, 0); // Pipes.

	bool booting = false;
	bool cancel = false;
	bool full = false;
	bool hook_finalize = false;
	bool hook_prepare = false;
	bool ports = false;
	bool system = false;
	const char* target = "/";
	bool wait = false;

	enum longopt
	{
		OPT_BOOTING = 128,
		OPT_HOOK_FINALIZE,
		OPT_HOOK_PREPARE,
	};
	const struct option longopts[] =
	{
		{"booting", no_argument, NULL, OPT_BOOTING},
		{"cancel", no_argument, NULL, 'c'},
		{"full", no_argument, NULL, 'f'},
		{"hook-finalize", no_argument, NULL, OPT_HOOK_FINALIZE},
		{"hook-prepare", no_argument, NULL, OPT_HOOK_PREPARE},
		{"ports", no_argument, NULL, 'p'},
		{"system", no_argument, NULL, 's'},
		{"target", required_argument, NULL, 't'},
		{"wait", no_argument, NULL, 'w'},
		{0, 0, 0, 0}
	};
	const char* opts = "cfpst:w";
	int opt;
	while ( (opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1 )
	{
		switch (opt)
		{
		case OPT_BOOTING: booting = true; break;
		case 'c': cancel = true; break;
		case 'f': full = true; break;
		case OPT_HOOK_FINALIZE: hook_finalize = true; break;
		case OPT_HOOK_PREPARE: hook_prepare = true; break;
		case 'p': ports = true; break;
		case 's': system = true; break;
		case 't': target = optarg; break;
		case 'w': wait = true; break;
		default: return 2;
		}
	}
	if ( 1 < booting + cancel + hook_finalize + hook_prepare + wait )
		errx(2, "Mutually incompatible options were passed");

	bool hook_only = hook_prepare || hook_finalize;
	bool no_source = cancel;
	bool no_cancel = booting || hook_only;

	const char* source;
	if ( no_source )
	{
		source = "";
		if ( optind < argc )
			errx(2, "Unexpected extra operand: %s", argv[optind]);
	}
	else if ( booting )
	{
		source = join_paths(target, "sysmerge");
		if ( !source )
			err(2, "malloc");
		if ( optind < argc )
			errx(2, "Unexpected extra operand: %s", argv[optind]);
		char* system_path = join_paths(target, "sysmerge/tix/sysmerge.system");
		char* ports_path = join_paths(target, "sysmerge/tix/sysmerge.ports");
		char* full_path = join_paths(target, "sysmerge/tix/sysmerge.full");
		if ( !system_path || !ports_path || !full_path )
			err(2, "malloc");
		system = access_or_die(system_path, F_OK) == 0;
		ports = access_or_die(ports_path, F_OK) == 0;
		full = access_or_die(full_path, F_OK) == 0;
		free(system_path);
		free(ports_path);
		free(full_path);
	}
	else
	{
		if ( argc - optind < 1 )
			errx(2, "No source operand was given");
		source = argv[optind];
		if ( 1 < argc - optind )
			errx(2, "Unexpected extra operand: %s", argv[optind + 1]);
	}

	if ( !system && !ports )
		system = ports = true;
	if ( !ports )
		full = false;

	char* system_manifest = join_paths(target, "tix/manifest/system");
	if ( !system_manifest )
		err(2, "malloc");
	bool has_system = !access_or_die(system_manifest, F_OK);
	free(system_manifest);

	if ( !has_system )
		system = false;

	struct conf conf;
	conf_init(&conf);
	char* conf_path = join_paths(target, "etc/upgrade.conf");
	if ( !conf_path )
		err(2, "malloc");
	if ( !conf_load(&conf, conf_path) && errno != ENOENT )
		err(2, "%s", conf_path);

	bool did_cancel = false;
	if ( !no_cancel && has_pending_upgrade(target) )
	{
		char* sysmerge = join_paths(target, "sysmerge");
		char* boot_sysmerge = join_paths(target, "boot/sysmerge");
		if ( !sysmerge || !boot_sysmerge )
			err(2, "malloc");
		execute((const char*[]) { "rm", "-rf", "--", sysmerge, NULL }, "");
		execute((const char*[]) { "rm", "-rf", "--", boot_sysmerge, NULL }, "");
		update_grub(&conf, target);
		printf("Cancelled pending system upgrade.\n");
		did_cancel = true;
	}

	if ( cancel )
	{
		if ( !did_cancel )
			printf("No system upgrade was pending.\n");
		return 0;
	}

	char* old_release_path = join_paths(target, "etc/sortix-release");
	if ( !old_release_path )
		err(2, "malloc");
	struct release old_release;
	if ( !os_release_load(&old_release, old_release_path, old_release_path) )
	{
		if ( has_system || errno != ENOENT )
		{
			if ( errno == ENOENT )
				warn("%s", old_release_path);
			exit(2);
		}
	}
	free(old_release_path);

	char* new_release_path = join_paths(source, "etc/sortix-release");
	if ( !new_release_path )
		err(2, "malloc");
	struct release new_release;
	if ( !os_release_load(&new_release, new_release_path, new_release_path) )
	{
		if ( !system )
			new_release = old_release;
		else
		{
			if ( errno == ENOENT )
				warn("%s", new_release_path);
			exit(2);
		}
	}
	free(new_release_path);

	if ( has_system )
	{
		char* old_machine_path = join_paths(target, "etc/machine");
		if ( !old_machine_path )
			err(2, "malloc");
		char* old_machine = read_string_file(old_machine_path);
		if ( !old_machine )
			err(2, "%s", old_machine_path);
		char* new_machine_path = join_paths(source, "etc/machine");
		if ( !new_machine_path )
			err(2, "malloc");
		char* new_machine = !system ? strdup(old_machine) :
			                read_string_file(new_machine_path);
		if ( !new_machine )
			err(2, "%s", new_machine_path);
		if ( strcmp(old_machine, new_machine) != 0 )
			errx(2, "%s (%s) does not match %s (%s)", new_machine_path,
				 new_machine, old_machine_path, old_machine);
		free(old_machine);
		free(old_machine_path);
		free(new_machine);
		free(new_machine_path);
	}

	// TODO: Check for version (skipping, downgrading).

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

	if ( !system )
	{
		run_prepare = false;
		run_finalize = false;
	}

	if ( wait && !has_system )
		err(2, "--wait requires a system is installed in: %s", target);

	if ( header )
	{
		if ( wait && new_release.pretty_name )
			printf("Scheduling upgrade to %s on next boot using %s:\n",
			       new_release.pretty_name, source);
		else if ( new_release.pretty_name )
			printf("Upgrading to %s using %s:\n",
			       new_release.pretty_name, source);
		else
			printf("Upgrading %s using %s:\n", target, source);
	}

	// Upgrade hooks that runs before the old system is replaced.
	if ( has_system && run_prepare )
	{
		const char* prefix = !strcmp(target, "/") ? "" : target;
		if ( my_prepare )
			upgrade_prepare(&old_release, &new_release, source, prefix);
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
		const char* sysmerge = target;
		if ( wait )
		{
			sysmerge = join_paths(target, "sysmerge");
			if ( !sysmerge )
				err(2, "malloc");
			if ( mkdir(sysmerge, 0755) < 0 )
				err(2, "%s", sysmerge);
			execute((const char*[]) { "tix-collection", sysmerge, "create",
			                           NULL }, "e");
		}
		const char* prefix = !strcmp(sysmerge, "/") ? "" : sysmerge;
		install_manifests_detect(source, prefix, system, ports, full);
	}

	if ( has_system && booting )
	{
		char* path;
		if ( asprintf(&path, "%s/bin:%s/sbin", target, target) < 0 ||
		     setenv("PATH", path, 1) < 0 )
			err(1, "malloc");
		free(path);
	}

	if ( wait )
	{
		printf(" - Scheduling upgrade on next boot...\n");
		char* system_path = join_paths(target, "sysmerge/tix/sysmerge.system");
		char* ports_path = join_paths(target, "sysmerge/tix/sysmerge.ports");
		char* full_path = join_paths(target, "sysmerge/tix/sysmerge.full");
		char* ready_path = join_paths(target, "sysmerge/tix/sysmerge.ready");
		char* sysmerge_boot = join_paths(target, "sysmerge/boot");
		char* boot_sysmerge = join_paths(target, "boot/sysmerge");
		if ( !system_path || !ports_path || !full_path || !ready_path ||
		     !sysmerge_boot || !boot_sysmerge )
			err(2, "malloc");

		if ( full )
		{
			int fd = open(full_path, O_WRONLY | O_CREAT);
			if ( fd < 0 )
				err(2, "%s", full_path);
			close(fd);
		}
		if ( system && !ports )
		{
			int fd = open(system_path, O_WRONLY | O_CREAT);
			if ( fd < 0 )
				err(2, "%s", system_path);
			close(fd);
		}
		if ( ports && !system )
		{
			int fd = open(ports_path, O_WRONLY | O_CREAT);
			if ( fd < 0 )
				err(2, "%s", ports_path);
			close(fd);
		}

		// Generate the new initrd in /sysmerge/boot.
		execute((const char*[]) { "/sysmerge/libexec/sysmerge/prepare",
		                          NULL }, "ce", target);

		// Move the kernel and initrd files to the boot partition where the
		// bootloader is guaranteed to be able to read them.
		execute((const char*[]) { "rm", "-rf", "--", boot_sysmerge,
		                           NULL }, "e");
		execute((const char*[]) { "cp", "-RT", "--", sysmerge_boot,
		                          boot_sysmerge, NULL }, "e");

		// Signal the sysmerge upgrade is ready and isn't partial.
		int fd = open(ready_path, O_WRONLY | O_CREAT);
		if ( fd < 0 )
			err(2, "%s", ready_path);
		close(fd);

		update_grub(&conf, target);

		printf("The system will be upgraded to %s on the next boot.\n",
		       new_release.pretty_name);
		printf("Run %s --cancel to cancel the upgrade.\n", argv[0]);

		return 0;
	}

	// Upgrade hooks that run after the new system is installed.
	if ( has_system && run_finalize )
	{
		const char* prefix = !strcmp(target, "/") ? "" : target;
		if ( my_finalize )
			upgrade_finalize(&old_release, &new_release, source, prefix);
		else
		{
			char* new_sysmerge = join_paths(source, "sbin/sysmerge");
			if ( !new_sysmerge )
				err(2, "malloc");
			execute((const char*[]) { new_sysmerge, "--hook-finalize", source,
			                          NULL }, "e");
			free(new_sysmerge);
		}
		if ( hook_finalize )
			return 0;
	}

	// Remove the upgrade readiness marker now that the upgrade has gone through
	// such that the bootloader configuration and initrds don't try to do the
	// upgrade again.
	if ( has_system && booting )
	{
		char* ready_path = join_paths(target, "sysmerge/tix/sysmerge.ready");
		if ( !ready_path )
			err(2, "malloc");
		unlink(ready_path);
		free(ready_path);
	}

	// Update the initrd and bootloader. The new bootloader config won't refer
	// to the upgrade as it's complete and the marker is gone.
	if ( has_system && access_or_die("/etc/fstab", F_OK) == 0 )
	{
		printf(" - Creating initrd...\n");
		execute((const char*[]) { "update-initrd", NULL }, "ce", target);

		if ( conf.grub )
		{
			char* boot_path = join_paths(target, "boot");
			if ( !boot_path )
				err(2, "malloc");
			int boot_fd = open(boot_path, O_RDONLY);
			if ( boot_fd < 0 )
				err(2, boot_path);
			char* boot_device = atcgetblob(boot_fd, "device-path", NULL);
			if ( !boot_device )
				err(2, "Failed to find device of filesystem: %s", boot_path);
			close(boot_fd);
			free(boot_path);
			// TODO: A better design for finding the parent block device.
			if ( is_partition_name(boot_device) )
				*strrchr(boot_device, 'p') = '\0';
			printf(" - Installing bootloader...\n");
			execute((const char*[]) { "grub-install", boot_device,
				                      NULL }, "ceqQ", target);
			free(boot_device);
		}

		update_grub(&conf, target);
	}

	// Finally clean up /sysmerge and /boot/sysmerge. They were left alone so
	// the system remained bootable with the idempotent upgrade if it failed
	// midway. Okay there's a bit of race conditions in grub-install, though the
	// replacement of grub.cfg is atomic. Everything now points into the new
	// system and nothing refers to the sysmerge directories.
	if ( has_system && booting )
	{
		// TODO: After releasing Sortix 1.1, remove sysmerge.orig compatibility.
		char* kernel = join_paths(target, "boot/sortix.bin.sysmerge.orig");
		char* initrd = join_paths(target, "boot/sortix.initrd.sysmerge.orig");
		char* sysmerge = join_paths(target, "sysmerge");
		char* boot_sysmerge = join_paths(target, "boot/sysmerge");
		if ( !kernel || !initrd || !sysmerge || !boot_sysmerge )
			err(2, "malloc");
		unlink(kernel);
		unlink(initrd);
		execute((const char*[]) { "rm", "-rf", "--", sysmerge, NULL }, "");
		execute((const char*[]) { "rm", "-rf", "--", boot_sysmerge, NULL }, "");
		free(kernel);
		free(initrd);
		free(sysmerge);
		free(boot_sysmerge);
	}

	if ( new_release.pretty_name )
		printf("Successfully upgraded to %s.\n", new_release.pretty_name);
	else
		printf("Successfully upgraded %s.\n", target);

	// Reinitialize the operating system if upgrading on boot.
	return booting ? 3 : 0;
}
