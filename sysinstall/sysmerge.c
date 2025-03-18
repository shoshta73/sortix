/*
 * Copyright (c) 2016, 2018, 2020-2025 Jonas 'Sortie' Termansen.
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
	return !access_join_or_die(target, "sysmerge", F_OK) ||
	       !access_join_or_die(target, "boot/sysmerge", F_OK);
}

static bool has_ready_upgrade(const char* target)
{
	return !access_join_or_die(target, "sysmerge/tix/sysmerge.ready", F_OK);
}

static void update_grub(struct conf* conf, const char* target)
{
	if ( conf->grub )
	{
		printf(" - Configuring bootloader...\n");
		execute((const char*[]) { "update-grub", NULL }, "ceqQ", target);
	}
	else if ( !access_join_or_die(target,
	                             "etc/default/grub.d/10_sortix", F_OK) &&
	          !access_join_or_die(target, "etc/fstab", F_OK) )
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
	bool is_reboot_needed = false;
	bool move_source = false;
	bool ports = false;
	bool system = false;
	const char* target = "/";
	bool wait = true;
	bool wait_default = true;

	enum longopt
	{
		OPT_BOOTING = 128,
		OPT_HOOK_FINALIZE,
		OPT_HOOK_PREPARE,
		OPT_IS_REBOOT_NEEDED,
	};
	const struct option longopts[] =
	{
		{"booting", no_argument, NULL, OPT_BOOTING},
		{"cancel", no_argument, NULL, 'c'},
		{"full", no_argument, NULL, 'f'},
		{"hook-finalize", no_argument, NULL, OPT_HOOK_FINALIZE},
		{"hook-prepare", no_argument, NULL, OPT_HOOK_PREPARE},
		{"is-reboot-needed", no_argument, NULL, OPT_IS_REBOOT_NEEDED},
		{"move-source", no_argument, NULL, 'm'},
		{"now", no_argument, NULL, 'n'},
		{"ports", no_argument, NULL, 'p'},
		{"system", no_argument, NULL, 's'},
		{"target", required_argument, NULL, 't'},
		{"wait", no_argument, NULL, 'w'},
		{0, 0, 0, 0}
	};
	const char* opts = "cfmnpst:w";
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
		case OPT_IS_REBOOT_NEEDED: is_reboot_needed = true; break;
		case 'm': move_source = true; break;
		case 'n': wait = false; wait_default = false; break;
		case 'p': ports = true; break;
		case 's': system = true; break;
		case 't': target = optarg; break;
		case 'w': wait = true; wait_default = false; break;
		default: return 2;
		}
	}
	if ( 1 < booting + cancel + hook_finalize + hook_prepare + !wait +
	         is_reboot_needed )
		errx(2, "Mutually incompatible options were passed");

	const char* target_prefix = !strcmp(target, "/") ? "" : target;

	if ( is_reboot_needed )
		exit(has_ready_upgrade(target) ? 0 : 1);

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
		system =
			!access_join_or_die(target, "sysmerge/tix/sysmerge.system", F_OK);
		ports =
			!access_join_or_die(target, "sysmerge/tix/sysmerge.ports", F_OK);
		full = !access_join_or_die(target, "sysmerge/tix/sysmerge.full", F_OK);
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

	bool has_system = !access_join_or_die(target, "tix/manifest/system", F_OK);

	if ( !has_system )
		system = false;

	if ( wait_default )
		wait = system && !access_join_or_die(target, "etc/fstab", F_OK);

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

	char* old_etc_release = join_paths(target, "etc/sortix-release");
	char* old_lib_release = join_paths(target, "lib/sortix-release");
	if ( !old_etc_release || !old_lib_release )
		err(2, "malloc");
	const char* old_release_path = !access(old_etc_release, F_OK) ?
	                               old_etc_release : old_lib_release;
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
	free(old_etc_release);
	free(old_lib_release);

	char* new_etc_release = join_paths(source, "etc/sortix-release");
	char* new_lib_release = join_paths(source, "lib/sortix-release");
	if ( !new_etc_release || !new_lib_release )
		err(2, "malloc");
	const char* new_release_path = !access(new_etc_release, F_OK) ?
	                               new_etc_release : new_lib_release;
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
	free(new_etc_release);
	free(new_lib_release);

	if ( has_system )
	{
		char* old_platform = read_platform(target);
		if ( !old_platform )
			err(2, "%s/tix/collection.conf: Failed to find PLATFORM", target);
		char* new_platform = read_platform(source);
		if ( !new_platform )
			err(2, "%s/tix/collection.conf: Failed to find PLATFORM", source);
		if ( strcmp(old_platform, new_platform) != 0 )
			errx(2, "cannot change PLATFORM from %s to %s",
		         old_platform, new_platform);
		free(old_platform);
		free(new_platform);
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
		wait = false;
	}
	else if ( hook_prepare )
	{
		header = false;
		copy_files = false;
		run_prepare = true;
		my_prepare = true;
		run_finalize = false;
		my_finalize = false;
		wait = false;
	}
	else if ( hook_finalize )
	{
		header = false;
		copy_files = false;
		run_prepare = false;
		my_prepare = false;
		run_finalize = true;
		my_finalize = true;
		wait = false;
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

	if ( wait )
	{
		printf(" - Scheduling upgrade on next boot...\n");

		char* sysmerge = join_paths(target, "sysmerge");
		char* system_path = join_paths(target, "sysmerge/tix/sysmerge.system");
		char* ports_path = join_paths(target, "sysmerge/tix/sysmerge.ports");
		char* full_path = join_paths(target, "sysmerge/tix/sysmerge.full");
		char* ready_path = join_paths(target, "sysmerge/tix/sysmerge.ready");
		char* sysmerge_boot = join_paths(target, "sysmerge/boot");
		char* boot_sysmerge = join_paths(target, "boot/sysmerge");
		if ( !sysmerge || !system_path || !ports_path || !full_path ||
		     !ready_path || !sysmerge_boot || !boot_sysmerge )
			err(2, "malloc");

		if ( !move_source || rename(source, sysmerge) < 0 )
		{
			printf(" - Copying %s to %s...\n", source, sysmerge);
			execute((const char*[]) { "cp", "-RT", source, sysmerge, NULL },
			         "e");
		}

		if ( full )
		{
			int fd = open(full_path, O_WRONLY | O_CREAT, 0644);
			if ( fd < 0 )
				err(2, "%s", full_path);
			close(fd);
		}
		if ( system && !ports )
		{
			int fd = open(system_path, O_WRONLY | O_CREAT, 0644);
			if ( fd < 0 )
				err(2, "%s", system_path);
			close(fd);
		}
		if ( ports && !system )
		{
			int fd = open(ports_path, O_WRONLY | O_CREAT, 0644);
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

	// Upgrade hooks that runs before the old system is replaced.
	if ( system && run_prepare )
	{
		if ( my_prepare )
			upgrade_prepare(&old_release, &new_release, source, target_prefix);
		else
		{
			// Run the prepare hooks with the new tools as they have not yet
			// been installed. This is no problem for the finalize hooks.
			char* old_path = strdup(getenv("PATH"));
			if ( !old_path )
				err(1, "malloc");
			char* path;
			if ( asprintf(&path, "%s/bin:%s/sbin", source, source) < 0 ||
			     setenv("PATH", path, 1) < 0 )
				err(1, "malloc");
			free(path);
			char* new_sysmerge = join_paths(source, "sbin/sysmerge");
			if ( !new_sysmerge )
				err(2, "malloc");
			execute((const char*[]) { new_sysmerge, "--hook-prepare", source,
			                          NULL }, "e");
			free(new_sysmerge);
			if ( setenv("PATH", old_path, 1) < 0 )
				err(1, "malloc");
			free(old_path);
		}
		if ( hook_prepare )
			return 0;
	}

	if ( copy_files )
		install_manifests_detect(source, target_prefix, system, ports, full,
		                         booting);

	if ( system && booting )
	{
		char* path;
		if ( asprintf(&path, "%s/bin:%s/sbin", target, target) < 0 ||
		     setenv("PATH", path, 1) < 0 )
			err(1, "malloc");
		free(path);
	}

	// Upgrade hooks that run after the new system is installed.
	if ( system && run_finalize )
	{
		if ( my_finalize )
		{
			upgrade_finalize(&old_release, &new_release, source, target_prefix);
			post_upgrade(source, target);
		}
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
	else if ( run_finalize )
		post_upgrade(source, target);

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
	if ( has_system && !access_join_or_die(target, "/etc/fstab", F_OK) )
	{
		printf(" - Creating initrd...\n");
		execute((const char*[]) { "update-initrd", NULL }, "ce", target);

		if ( conf.grub )
		{
			// TODO: After releasing Sortix 1.1, remove the boot device operand
			//       and start using -q. The installation is old-style for now
			//       to allow a mixed port environment with a new base system
			//       and old grub when bootstrapping Sortix 1.1 on Sortix 1.0.
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

	const char* done = "Successfully upgraded";
	if ( new_release.pretty_name )
		printf("%s to %s.\n", done, new_release.pretty_name);
	else
		printf("%s.\n", done);

	// Reinitialize the operating system if upgrading on boot.
	return booting ? 3 : 0;
}
