/*
 * Copyright (c) 2015, 2016, 2020, 2021 Jonas 'Sortie' Termansen.
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
 * sysupgrade.c
 * Operating system upgrader.
 */

#include <sys/display.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#include <brand.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fstab.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <mount/blockdevice.h>
#include <mount/filesystem.h>
#include <mount/harddisk.h>
#include <mount/partition.h>

#include "conf.h"
#include "devices.h"
#include "execute.h"
#include "fileops.h"
#include "hooks.h"
#include "interactive.h"
#include "manifest.h"
#include "release.h"

const char* prompt_man_section = "7";
const char* prompt_man_page = "upgrade";

struct installation
{
	struct blockdevice* bdev;
	struct release release;
	struct mountpoint* mountpoints;
	size_t mountpoints_used;
	char* machine;
};

static struct installation* installations;
static size_t installations_count;
static size_t installations_length;
static pid_t main_pid;
static struct mountpoint* mountpoints;
static size_t mountpoints_used;
static bool fs_made = false;
static char fs[] = "/tmp/fs.XXXXXX";

static bool add_installation(struct blockdevice* bdev,
                             struct release* release,
                             struct mountpoint* mountpoints,
                             size_t mountpoints_used,
                             char* machine)
{
	if ( installations_count == installations_length )
	{
			size_t length = installations_length;
			if ( !length )
				length = 8;
			struct installation* new_installations = (struct installation*)
				reallocarray(NULL, length, 2 * sizeof(struct installation));
			if ( !new_installations )
				return false;
			installations = new_installations;
			installations_length = 2 * length;
	}
	struct installation* installation = &installations[installations_count++];
	installation->bdev = bdev;
	installation->release = *release;
	installation->mountpoints = mountpoints;
	installation->mountpoints_used = mountpoints_used;
	installation->machine = machine;
	return true;
}

static void search_installation_path(const char* mnt, struct blockdevice* bdev)
{
	char* release_errpath;
	if ( asprintf(&release_errpath, "%s: /etc/sortix-release",
	              path_of_blockdevice(bdev)) < 0 )
	{
		warn("malloc");
		return;
	}
	char* release_path;
	if ( asprintf(&release_path, "%s/etc/sortix-release", mnt) < 0 )
	{
		warn("malloc");
		free(release_errpath);
		return;
	}
	struct release release;
	bool status = os_release_load(&release, release_path, release_errpath);
	free(release_path);
	free(release_errpath);
	if ( !status )
		return;
	char* fstab_path;
	if ( asprintf(&fstab_path, "%s/etc/fstab", mnt) < 0 )
	{
		warn("malloc");
		release_free(&release);
		return;
	}
	struct mountpoint* mountpoints;
	size_t mountpoints_used;
	status = load_mountpoints(fstab_path, &mountpoints, &mountpoints_used);
	free(fstab_path);
	if ( !status )
	{
		warn("%s: %s", path_of_blockdevice(bdev), "/etc/fstab");
		release_free(&release);
		return;
	}
	char* machine_path;
	if ( asprintf(&machine_path, "%s/etc/machine", mnt) < 0 )
	{
		warn("%s: malloc", path_of_blockdevice(bdev));
		free_mountpoints(mountpoints, mountpoints_used);
		release_free(&release);
		return;
	}
	char* machine = read_string_file(machine_path);
	free(machine_path);
	if ( !machine )
	{
		warn("%s/etc/machine", path_of_blockdevice(bdev));
		free_mountpoints(mountpoints, mountpoints_used);
		release_free(&release);
		return;
	}
	if ( !add_installation(bdev, &release, mountpoints, mountpoints_used,
	                       machine) )
	{
		free(machine);
		free_mountpoints(mountpoints, mountpoints_used);
		release_free(&release);
		return;
	}
}

static void search_installation_bdev(const char* mnt, struct blockdevice* bdev)
{
	if ( !bdev->fs )
		return;
	if ( !bdev->fs->driver )
		return;
	struct mountpoint mp = { 0 };
	mp.absolute = (char*) mnt;
	mp.fs = bdev->fs;
	mp.entry.fs_file = (char*) mnt;
	if ( !mountpoint_mount(&mp) )
		return;
	search_installation_path(mnt, bdev);
	mountpoint_unmount(&mp);
}

static void search_installations(const char* mnt)
{
	for ( size_t i = 0; i < installations_count; i++ )
	{
		struct installation* inst = &installations[i];
		free_mountpoints(inst->mountpoints, inst->mountpoints_used);
		release_free(&inst->release);
	}
	free(installations);
	installations_count = 0;
	installations_length = 0;

	for ( size_t i = 0; i < hds_count; i++ )
	{
		struct harddisk* hd = hds[i];
		if ( hd->bdev.pt )
		{
			for ( size_t n = 0; n < hd->bdev.pt->partitions_count; n++ )
			{
				struct partition* p = hd->bdev.pt->partitions[n];
				search_installation_bdev(mnt, &p->bdev);
			}
		}
		else
			search_installation_bdev(mnt, &hd->bdev);
	}
}

static void next_version(const struct release* current, struct release* new)
{
	// Next release of a development snapshot is the final release.
	if ( current->version_dev )
	{
		new->version_major = current->version_major;
		new->version_minor = current->version_minor;
		new->version_dev = false;
		return;
	}

	// Releases increment by 0.1.
	new->version_major = current->version_major;
	new->version_minor = current->version_minor + 1;
	new->version_dev = false;

	// Major increments instead of minor release 10.
	if ( new->version_minor == 10 )
	{
		new->version_major++;
		new->version_minor = 0;
	}
}

static bool downgrading_version(const struct release* old,
                                const struct release* new)
{
	if ( new->version_major < old->version_major )
		return true;
	if ( new->version_major > old->version_major )
		return false;
	if ( new->version_minor < old->version_minor )
		return true;
	if ( new->version_minor > old->version_minor )
		return false;
	if ( new->version_dev && !old->version_dev )
		return true;
	return false;
}

static bool skipping_version(const struct release* old,
                             const struct release* new)
{
	// Not skipping a release if upgrading to older release.
	if ( downgrading_version(old, new) )
		return false;

	// Not skipping a release if upgrading to same release.
	if ( new->version_major == old->version_major &&
	     new->version_minor == old->version_minor &&
	     new->version_dev == old->version_dev )
		return false;

	// Not skipping a release if upgrading to the next release.
	struct release next;
	next_version(old, &next);
	if ( new->version_major == next.version_major &&
	     new->version_minor == next.version_minor )
		return false;

	return true;
}

static void preserve_src(const char* where)
{
	if ( access_or_die(where, F_OK) < 0 )
		return;
	if ( access_or_die("oldsrc", F_OK) < 0 )
	{
		if ( mkdir("oldsrc", 0755) < 0 )
		{
			warn("oldsrc");
			_exit(2);
		}
	}
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char buf[64];
	snprintf(buf, sizeof(buf), "oldsrc/%s-%i-%02i-%02i",
	         where, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
	if ( access_or_die(buf, F_OK) == 0 )
	{
		snprintf(buf, sizeof(buf), "oldsrc/%s-%i-%02i-%02i-%02i-%02i-%02i",
		         where, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		         tm.tm_hour, tm.tm_min, tm.tm_sec);
		if ( access_or_die(buf, F_OK) == 0 )
		{
			snprintf(buf, sizeof(buf), "oldsrc/%s.XXXXXX", where);
			if ( !mkdtemp(buf) )
			{
				warnx("failed to find location to store old /%s", where);
				_exit(2);
			}
			rmdir(buf);
		}
	}
	printf(" - Moving /%s to /%s\n", where, buf);
	if ( rename(where, buf) < 0 )
	{
		warn("rename: %s -> %s", where, buf);
		_exit(2);
	}
}

void exit_handler(void)
{
	if ( getpid() != main_pid )
		 return;
	chdir("/");
	for ( size_t n = mountpoints_used; n != 0; n-- )
	{
		size_t i = n - 1;
		struct mountpoint* mountpoint = &mountpoints[i];
		mountpoint_unmount(mountpoint);
	}
	if ( fs_made )
		rmdir(fs);
}

int main(void)
{
	shlvl();

	if ( !isatty(0) )
		errx(2, "fatal: stdin is not a terminal");
	if ( !isatty(1) )
		errx(2, "fatal: stdout is not a terminal");
	if ( !isatty(2) )
		errx(2, "fatal: stderr is not a terminal");

	if ( getuid() != 0 )
		errx(2, "You need to be root to install %s", BRAND_DISTRIBUTION_NAME);
	if ( getgid() != 0 )
		errx(2, "You need to be group root to install %s", BRAND_DISTRIBUTION_NAME);

	main_pid = getpid();
	if ( atexit(exit_handler) != 0 )
		err(2, "atexit");

	struct utsname uts;
	uname(&uts);

	static char input[256];

	textf("Hello and welcome to the " BRAND_DISTRIBUTION_NAME " " VERSIONSTR ""
	      " upgrader for %s.\n\n", uts.machine);

	// '|' rather than '||' is to ensure side effects.
	if ( missing_program("cut") |
	     missing_program("dash") |
	     missing_program("fsck.ext2") |
	     missing_program("grub-install") |
	     missing_program("man") |
	     missing_program("sed") |
	     missing_program("xargs") )
	{
		text("Warning: This system does not have the necessary third party "
		     "software installed to properly upgrade installations.\n");
		while ( true )
		{
			prompt(input, sizeof(input), "Sure you want to proceed?", "no");
			if ( strcasecmp(input, "no") == 0 )
				return 0;
			if ( strcasecmp(input, "yes") == 0 )
				break;
		}
		text("\n");
	}

	text("This program will upgrade an existing installation to this "
	     "version. You can always escape to a shell by answering '!' to any "
	     "regular prompt. You can view the upgrade(7) manual page by answering "
	     "'!man'. Default answers are in []'s and can be selected by pressing "
	     "enter.\n\n");

	const char* readies[] =
	{
		"Ready",
		"Yes",
		"Yeah",
		"Yep",
		"Let's go",
		"Let's do this",
		"Betcha",
		"Sure am",
		"You bet",
		"This time it will listen to my music",
	};
	size_t num_readies = sizeof(readies) / sizeof(readies[0]);
	const char* ready = readies[arc4random_uniform(num_readies)];
	prompt(input, sizeof(input), "Ready?", ready);
	text("\n");

	bool kblayout_setable = 0 <= tcgetblob(0, "kblayout", NULL, 0);
	while ( kblayout_setable )
	{
		// TODO: Detect the name of the current keyboard layout.
		prompt(input, sizeof(input),
		       "Choose your keyboard layout ('?' or 'L' for list)", "default");
		if ( !strcmp(input, "?") ||
		     !strcmp(input, "l") ||
		     !strcmp(input, "L") )
		{
			DIR* dir = opendir("/share/kblayout");
			if ( !dir )
			{
				warn("%s", "/share/kblayout");
				continue;
			}
			bool space = false;
			struct dirent* entry;
			while ( (entry = readdir(dir)) )
			{
				if ( entry->d_name[0] == '.' )
					continue;
				if ( space )
					putchar(' ');
				fputs(entry->d_name, stdout);
				space = true;
			}
			closedir(dir);
			if ( !space )
				fputs("(No keyboard layouts available)", stdout);
			putchar('\n');
			continue;
		}
		if ( !strcmp(input, "default") )
			break;
		const char* argv[] = { "chkblayout", "--", input, NULL };
		if ( execute(argv, "f") == 0 )
			break;
	}
	if ( kblayout_setable )
		text("\n");

	struct tiocgdisplay display;
	struct tiocgdisplays gdisplays;
	memset(&gdisplays, 0, sizeof(gdisplays));
	gdisplays.count = 1;
	gdisplays.displays = &display;
	struct dispmsg_get_driver_name dgdn = { 0 };
	dgdn.msgid = DISPMSG_GET_DRIVER_NAME;
	dgdn.device = 0;
	dgdn.driver_index = 0;
	dgdn.name.byte_size = 0;
	dgdn.name.str = NULL;
	if ( ioctl(1, TIOCGDISPLAYS, &gdisplays) == 0 &&
	     0 < gdisplays.count &&
	     (dgdn.device = display.device, true) &&
	     (dispmsg_issue(&dgdn, sizeof(dgdn)) == 0 || errno != ENODEV) )
	{
		struct dispmsg_get_crtc_mode get_mode;
		memset(&get_mode, 0, sizeof(get_mode));
		get_mode.msgid = DISPMSG_GET_CRTC_MODE;
		get_mode.device = 0;
		get_mode.connector = 0;
		bool good = false;
		if ( dispmsg_issue(&get_mode, sizeof(get_mode)) == 0 )
		{
			good = (get_mode.mode.control & DISPMSG_CONTROL_VALID) &&
			       (get_mode.mode.control & DISPMSG_CONTROL_GOOD_DEFAULT);
			if ( get_mode.mode.control & DISPMSG_CONTROL_VM_AUTO_SCALE )
			{
				text("The display resolution will automatically change to "
				     "match the size of the virtual machine window.\n\n");
				good = true;
			}
		}
		const char* def = good ? "no" : "yes";
		while ( true )
		{
			prompt(input, sizeof(input),
			       "Select display resolution? (yes/no)", def);
			if ( strcasecmp(input, "no") && strcasecmp(input, "yes") )
				continue;
			if ( strcasecmp(input, "no") == 0 )
				break;
			if ( execute((const char*[]) { "chvideomode", NULL }, "f") != 0 )
				continue;
			break;
		}
		text("\n");
	}

	struct release new_release;
	if ( !os_release_load(&new_release, "/etc/sortix-release",
	                                    "/etc/sortix-release") )
	{
		if ( errno == ENOENT )
			warn("/etc/sortix-release");
		exit(2);
	}

	if ( !mkdtemp(fs) )
		err(2, "mkdtemp: %s", "/tmp/fs.XXXXXX");
	fs_made = true;
	// Export for the convenience of users escaping to a shell.
	setenv("SYSINSTALL_TARGET", fs, 1);

	struct installation* target = NULL;
	while ( true )
	{
		text("Searching for existing installations...\n");
		scan_devices();
		search_installations(fs);
		text("\n");

		if ( installations_count == 0 )
		{
			while ( true )
			{
				prompt(input, sizeof(input), "No existing installations found, "
					   "run installer instead? (yes/no)", "yes");
				if ( !strcasecmp(input, "no") || !strcasecmp(input, "yes") )
					break;
			}
			if ( !strcasecmp(input, "yes") )
			{
				text("\n");
				rmdir(fs);
				execlp("sysinstall", "sysinstall", (const char*) NULL);
				err(2, "sysinstall");
			}
			continue;
		}

		while ( true )
		{
			for ( size_t i = 0; i < installations_count; i++ )
			{
				struct installation* installation = &installations[i];
				printf("  %-16s  %s (%s)\n",
				       path_of_blockdevice(installation->bdev),
				       installation->release.pretty_name,
				       installation->machine);
			}
			text("\n");

			const char* def = NULL;
			if ( installations_count == 1 )
				def = path_of_blockdevice(installations[0].bdev);
			prompt(input, sizeof(input), "Which installation to upgrade?", def);
			target = NULL;
			for ( size_t i = 0; i < installations_count; i++ )
			{
				struct installation* installation = &installations[i];
				const char* path = path_of_blockdevice(installation->bdev);
				if ( strcmp(input, path) != 0 )
					continue;
				target = installation;
			}
			if ( !target )
			{
				text("Answer was not one of the found devices.\n\n");
				continue;
			}
			break;
		}

		break;
	}
	text("\n");

	struct release* target_release = &target->release;

	char* source_machine = read_string_file("/etc/machine");
	if ( !source_machine )
		err(2, "/etc/machine");
	if ( strcmp(target->machine, source_machine) != 0 )
	{
		textf("Warning: You are changing an existing installation to another "
		      "architecture! (%s -> %s) This is not supported and there is no "
		      "promise this will work!\n", target->machine, source_machine);
		while ( true )
		{
			prompt(input, sizeof(input),
			       "Change the existing installation to another architecture?",
			       "no");
			if ( !strcasecmp(input, "no") || !strcasecmp(input, "yes") )
				break;
		}
		if ( !strcasecmp(input, "no") )
			errx(2, "upgrade aborted because of architecture mismatch");
		text("\n");
	}
	free(source_machine);

	if ( downgrading_version(target_release, &new_release) )
	{
		text("Warning: You are downgrading an existing installation to an "
		     "earlier release. This is not supported and there is no promise "
		     "this will work!\n\n");

		while ( true )
		{
			prompt(input, sizeof(input),
			       "Downgrade to an earlier release?", "no");
			if ( !strcasecmp(input, "no") || !strcasecmp(input, "yes") )
				break;
		}
		if ( !strcasecmp(input, "no") )
			errx(2, "Upgrade aborted due to version downgrade");
		text("\n");
	}
	else if ( skipping_version(target_release, &new_release) )
	{
		text("Warning: You are not upgrading this installation to its next "
		     "release. You cannot skip releases. This is not supported and "
		     "there is no promise this will will work!\n\n");

		while ( true )
		{
			prompt(input, sizeof(input),
			       "Skip across releases?", "no");
			if ( !strcasecmp(input, "no") || !strcasecmp(input, "yes") )
				break;
		}
		if ( !strcasecmp(input, "no") )
			errx(2, "Upgrade aborted due to skipping releases");
		text("\n");
	}

	if ( abi_compare(new_release.abi_major, new_release.abi_minor,
	                 target_release->abi_major, target_release->abi_minor) < 0 )
	{
		text("Warning: You are downgrading an existing installation to an "
		     "release with an earlier ABI. This is not supported and there is "
		     "no promise this will work!\n\n");

		while ( true )
		{
			prompt(input, sizeof(input),
			       "Downgrade to an earlier ABI?", "no");
			if ( !strcasecmp(input, "no") || !strcasecmp(input, "yes") )
				break;
		}
		if ( !strcasecmp(input, "no") )
			errx(2, "Upgrade aborted due to ABI downgrade");
		text("\n");
	}

	bool can_run_old_abi =
		abi_compatible(target_release->abi_major, target_release->abi_minor,
		               new_release.abi_major, new_release.abi_minor);

	mountpoints = target->mountpoints;
	mountpoints_used = target->mountpoints_used;

	struct blockdevice* bdev = target->bdev;
	struct blockdevice* bootloader_bdev = target->bdev;

	for ( size_t i = 0; i < mountpoints_used; i++ )
	{
		struct mountpoint* mnt = &mountpoints[i];
		const char* spec = mnt->entry.fs_spec;
		if ( !(mnt->fs = search_for_filesystem_by_spec(spec)) )
			errx(2, "fstab: %s: Found no mountable filesystem matching `%s'",
			     mnt->entry.fs_file, spec);
		if ( !mnt->fs->driver )
			errx(2, "fstab: %s: %s: Don't know how to mount this %s filesystem",
			     mnt->entry.fs_file,
			     path_of_blockdevice(mnt->fs->bdev),
			     mnt->fs->fstype_name);
	}

	for ( size_t i = 0; i < mountpoints_used; i++ )
	{
		struct mountpoint* mnt = &mountpoints[i];
		if ( !strcmp(mnt->entry.fs_file, "/boot") )
			bootloader_bdev = mnt->fs->bdev;
		char* absolute;
		if ( asprintf(&absolute, "%s%s", fs, mnt->absolute) < 0 )
			err(2, "asprintf");
		free(mnt->absolute);
		mnt->absolute = absolute;
		if ( !mountpoint_mount(mnt) )
			exit(2);
	}

	const char* bdev_path = path_of_blockdevice(bdev);
	const char* bootloader_dev_path = device_path_of_blockdevice(bootloader_bdev);

	if ( chdir(fs) < 0 )
		err(2, "chdir: %s", fs);

	if ( access_or_die("sysmerge", F_OK) == 0 )
	{
		text("Warning: A sysmerge(8) upgrade is scheduled for the next boot. "
		     "You must cancel this to proceed.\n\n");
		if ( !can_run_old_abi )
		{
			text("Error: Can't cancel pending upgrade due to ABI change.\n");
			errx(2, "Upgrade aborted due to pending sysmerge(8) upgrade");
		}

		while ( true )
		{
			prompt(input, sizeof(input),
			       "Cancel pending sysmerge upgrade?", "yes");
			if ( !strcasecmp(input, "no") || !strcasecmp(input, "yes") )
				break;
		}
		if ( !strcasecmp(input, "no") )
			errx(2, "Upgrade aborted due to pending sysmerge(8) upgrade");
		text("\n");
		execute((const char*[]) { "chroot", "-d", ".", "sysmerge", "--cancel",
		                          NULL }, "e");
	}

	bool do_upgrade_bootloader;
	struct conf conf;
	conf_init(&conf);
	while ( true )
	{
		conf_free(&conf);
		if ( !conf_load(&conf, "etc/upgrade.conf") && errno != ENOENT )
			err(2, "etc/upgrade.conf");

		do_upgrade_bootloader =
			conf.grub && (conf.ports || (conf.system && can_run_old_abi));

		textf("We are now ready to upgrade to %s %s. Take a moment to verify "
			  "everything is in order.\n", BRAND_DISTRIBUTION_NAME, VERSIONSTR);
		text("\n");
		char abibuf[16];
		printf("  %-16s  system architecture\n", uts.machine);
		printf("  %-16s  root filesystem\n", bdev_path);
		if ( do_upgrade_bootloader )
			printf("  %-16s  bootloader installation target\n", bootloader_dev_path);
		printf("  %-16s  old version\n", target_release->pretty_name);
		printf("  %-16s  new version\n", new_release.pretty_name);
		snprintf(abibuf, sizeof(abibuf), "%lu.%lu",
			     target_release->abi_major, target_release->abi_minor);
		printf("  %-16s  old ABI\n", abibuf);
		snprintf(abibuf, sizeof(abibuf), "%lu.%lu",
			     new_release.abi_major, new_release.abi_minor);
		printf("  %-16s  new ABI\n", abibuf);
		if ( conf.system )
			printf("  %-16s  will be updated\n", "system");
		else
			printf("  %-16s  will not be updated\n", "system");
		if ( conf.ports )
			printf("  %-16s  will be updated\n", "ports");
		else
			printf("  %-16s  will not be updated\n", "ports");
		if ( has_manifest("src") )
		{
			if ( conf.newsrc )
				printf("  %-16s  new source code\n", "/newsrc");
			else if ( conf.src )
				printf("  %-16s  will be updated\n", "/src");
			else
				printf("  %-16s  will not be updated\n", "/src");
		}
		else
			printf("  %-16s  will not be updated\n", "/src");
		if ( do_upgrade_bootloader )
			printf("  %-16s  will be updated\n", "bootloader");
		else
			printf("  %-16s  will not be updated\n", "bootloader");
		text("\n");

		while ( true )
		{
			promptx(input, sizeof(input),
				   "Upgrade? (yes/no/poweroff/reboot/halt)", "yes", true);
			if ( !strcasecmp(input, "yes") )
				break;
			else if ( !strcasecmp(input, "no") )
			{
				text("Answer '!' to get a shell. Type !man to view the "
				     "upgrade(7) manual page. You can edit the upgrade.conf(5) "
				     "configuration file of the target system to change which "
				     "upgrade operations are performed.\n");
				text("Alternatively, you can answer 'poweroff', 'reboot', or "
				     "'halt' or cancel the upgrade.\n");
				continue;
			}
			else if ( !strcasecmp(input, "poweroff") )
				exit(0);
			else if ( !strcasecmp(input, "reboot") )
				exit(1);
			else if ( !strcasecmp(input, "halt") )
				exit(2);
			else if ( !strcasecmp(input, "!") )
				break;
			else
				continue;
		}
		if ( !strcasecmp(input, "yes") )
			break;
	}
	text("\n");

	// TODO: Switch to local time zone of the existing system?

	text("Upgrading to " BRAND_DISTRIBUTION_NAME " " VERSIONSTR " now:\n");

	pid_t upgrade_pid = fork();
	if ( upgrade_pid < 0 )
		err(2, "fork");
	if ( upgrade_pid == 0 )
	{
		umask(0022);
		if ( conf.system )
			upgrade_prepare(target_release, &new_release, "", ".");
		install_manifests_detect("", ".", conf.system, conf.ports, conf.ports);
		if ( has_manifest("src") )
		{
			if ( conf.newsrc )
			{
				bool has_src = access_or_die("src", F_OK) == 0;
				if ( has_src )
				{
					preserve_src("newsrc");
					if ( rename("src", "src.tmp") < 0 )
					{
						warn("rename: /src -> /src.tmp");
						_exit(2);
					}
				}
				install_manifest("src", "", ".", (const char*[]){}, 0);
				if ( has_src )
				{
					if ( rename("src", "newsrc") < 0 )
					{
						warn("rename: /src -> /newsrc");
						_exit(2);
					}
					if ( rename("src.tmp", "src") < 0 )
					{
						warn("rename: /src.tmp -> /src");
						_exit(2);
					}
				}
			}
			else if ( conf.src )
			{
				preserve_src("src");
				install_manifest("src", "", ".", (const char*[]){}, 0);
			}
		}
		if ( conf.system )
			upgrade_finalize(target_release, &new_release, "", ".");
		if ( conf.system )
		{
			printf(" - Creating initrd...\n");
			execute((const char*[]) { "update-initrd", "--sysroot", fs, NULL }, "_e");
		}
		if ( do_upgrade_bootloader )
		{
			printf(" - Installing bootloader...\n");
			execute((const char*[]) { "chroot", "-d", ".", "grub-install",
			        bootloader_dev_path, NULL },
			        "_eqQ");
			printf(" - Configuring bootloader...\n");
			execute((const char*[]) { "chroot", "-d", ".", "update-grub", NULL },
			        "_eqQ");
		}
		else if ( conf.system &&
		          access_or_die("etc/grub.d/10_sortix", F_OK) == 0 )
		{
			// Help dual booters by making /etc/grub.d/10_sortix.cache.
			printf(" - Creating bootloader fragment...\n");
			execute((const char*[]) { "chroot", "-d", ".",
			                          "/etc/grub.d/10_sortix", NULL }, "_eq");
		}
		printf(" - Finishing upgrade...\n");
		_exit(0);
	}
	int upgrade_code;
	waitpid(upgrade_pid, &upgrade_code, 0);
	if ( WIFEXITED(upgrade_code) && WEXITSTATUS(upgrade_code) == 0 )
	{
	}
	else if ( WIFEXITED(upgrade_code) )
		errx(2, "upgrade failed with exit status %i", WEXITSTATUS(upgrade_code));
	else if ( WIFSIGNALED(upgrade_code) )
		errx(2, "upgrade failed: %s", strsignal(WTERMSIG(upgrade_code)));
	else
		errx(2, "upgrade failed: unknown waitpid code %i", upgrade_code);
	text("\n");

	if ( conf.system )
		textf("The %s installation has now been upgraded to %s.\n\n",
		      bdev_path, new_release.pretty_name);
	else if ( conf.newsrc )
		textf("The %s installation now contains the new source code in /newsrc. "
		      "You need to build it as described in development(7).\n\n",
		      bdev_path);
	else if ( conf.src )
		textf("The %s installation now contains the new source code in /src. "
		      "You need to build it as described in development(7).\n\n",
		      bdev_path);
	else
		textf("The %s installation has been upgraded to %s as requested.\n\n",
		      bdev_path, new_release.pretty_name);

	if ( target_release->abi_major < new_release.abi_major )
	{
		text("Note: The system has been upgraded across a major ABI change. "
		     "Locally compiled programs must be recompiled as they no longer "
		     "can be expected to work.\n\n");
	}
	else if ( target_release->abi_major == new_release.abi_major &&
	          target_release->abi_minor < new_release.abi_minor )
	{
		text("Note: The system has been upgraded across a minor ABI change.\n\n");
	}
	else if ( new_release.abi_major < target_release->abi_major ||
	          (target_release->abi_major == new_release.abi_major &&
	           new_release.abi_minor < target_release->abi_minor) )
	{
		text("Note: The system has been downgraded to an earlier ABI. "
		     "Locally compiled programs must be recompiled as they no longer "
		     "can be expected to work.\n\n");
	}

	while ( true )
	{
		prompt(input, sizeof(input),
		       "What now? (poweroff/reboot/halt)", "reboot");
		if ( !strcasecmp(input, "poweroff") )
			return 0;
		if ( !strcasecmp(input, "reboot") )
			return 1;
		if ( !strcasecmp(input, "halt") )
			return 2;
	}
}
