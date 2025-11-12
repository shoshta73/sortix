/*
 * Copyright (c) 2015-2016, 2020-2025 Jonas 'Sortie' Termansen.
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
 * sysinstall.c
 * Operating system installer.
 */

#include <sys/display.h>
#include <sys/ioctl.h>
#include <sys/kernelinfo.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#include <assert.h>
#include <brand.h>
#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fstab.h>
#include <limits.h>
#include <pwd.h>
#include <regex.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

// TODO: After releasing Sortix 1.1, remove this bootstrap compatibility.
#if defined(__sortix__)
#include <sortix/limits.h>
#endif

#include <mount/blockdevice.h>
#include <mount/devices.h>
#include <mount/filesystem.h>
#include <mount/harddisk.h>
#include <mount/partition.h>
#include <mount/uuid.h>

#include "autoconf.h"
#include "conf.h"
#include "devices.h"
#include "execute.h"
#include "fileops.h"
#include "interactive.h"
#include "manifest.h"
#include "release.h"

const char* prompt_man_section = "7";
const char* prompt_man_page = "installation";

static char input[256];

static bool is_valid_hostname(const char* hostname)
{
	if ( !hostname[0] )
		return false;
	for ( size_t i = 0; hostname[i]; i++ )
		if ( !('a' <= hostname[i] && hostname[i] <= 'z') &&
		     !('A' <= hostname[i] && hostname[i] <= 'Z') &&
		     !('0' <= hostname[i] && hostname[i] <= '9') &&
		     hostname[i] != '-' && hostname[i] != '.' )
			return false;
	return true;
}

static bool is_valid_username(const char* username)
{
	if ( !('a' <= username[0] && username[0] <= 'z') &&
	     !('A' <= username[0] && username[0] <= 'Z') )
		return false;
	for ( size_t i = 1; username[i]; i++ )
		if ( !('a' <= username[i] && username[i] <= 'z') &&
		     !('A' <= username[i] && username[i] <= 'Z') &&
		     !('0' <= username[i] && username[i] <= '9') &&
		     username[i] != '-' && username[i] != '_' )
			return false;
	return true;
}

static bool is_valid_user_full_name(const char* name)
{
	if ( !name[0] )
		return false;
	for ( size_t i = 0; name[i]; i++ )
		if ( name[i] == ':' || name[i] == '\\' )
			return false;
	return true;
}

static struct partition_table* search_bios_boot_pt(struct filesystem* root_fs)
{
	struct blockdevice* bdev = root_fs->bdev;
	while ( bdev->p )
		bdev = bdev->p->parent_bdev;
	if ( !bdev->pt )
		return NULL;
	struct partition_table* pt = bdev->pt;
	if ( pt->type != PARTITION_TABLE_TYPE_GPT )
		return NULL;
	return pt;
}

static struct partition* search_bios_boot_search(struct partition_table* pt)
{
	for ( size_t i = 0; i < pt->partitions_count; i++ )
	{
		struct partition* p = pt->partitions[i];
		if ( p->bdev.fs && !strcmp(p->bdev.fs->fstype_name, "biosboot") )
			return p;
	}
	return NULL;
}

static struct partition* search_bios_boot_partition(struct filesystem* root_fs)
{
	struct partition_table* pt = search_bios_boot_pt(root_fs);
	if ( !pt )
		return NULL;
	return search_bios_boot_search(pt);
}

static bool missing_bios_boot_partition(struct filesystem* root_fs)
{
	struct partition_table* pt = search_bios_boot_pt(root_fs);
	if ( !pt )
		return NULL;
	return !search_bios_boot_search(pt);
}

static bool should_install_bootloader_path(const char* mnt,
                                           struct blockdevice* bdev)
{
	char* etc_release = join_paths(mnt, "etc/sortix-release");
	char* lib_release = join_paths(mnt, "lib/sortix-release");
	if ( !etc_release || !lib_release )
	{
		warn("malloc");
		free(etc_release);
		free(lib_release);
		return false;
	}
	const char* release_path = !access(etc_release, F_OK) ?
	                           etc_release : lib_release;
	char* release_errpath;
	if ( asprintf(&release_errpath, "%s: %s",
	              path_of_blockdevice(bdev), release_path) < 0 )
	{
		warn("malloc");
		free(etc_release);
		free(lib_release);
		return false;
	}
	struct release release;
	bool status = os_release_load(&release, release_path, release_errpath);
	free(release_errpath);
	free(etc_release);
	free(lib_release);
	if ( !status )
		return false;
	release_free(&release);
	char* conf_path = join_paths(mnt, "etc/upgrade.conf");
	if ( !conf_path )
	{
		warn("malloc");
		return false;
	}
	struct conf conf;
	conf_init(&conf);
	bool result = false;
	if ( conf_load(&conf, conf_path) )
		result = conf.grub;
	else if ( errno != ENOENT )
		warn("%s: /etc/upgrade.conf", path_of_blockdevice(bdev));
	conf_free(&conf);
	free(conf_path);
	return result;
}

static bool should_ignore_bootloader_on_filesystem(struct blockdevice* bdev)
{
	return bdev->fs && bdev->fs->fstype_name &&
	       !strcmp(bdev->fs->fstype_name, "iso9660");
}

static bool consent_fsck(struct blockdevice* bdev)
{
	textf("Filesystem %s is inconsistent and requires repair to be mounted.\n",
	      device_path_of_blockdevice(bdev));
	while ( true )
	{
		char question[256];
		snprintf(question, sizeof(question), "Repair filesystem %s? (yes/no)",
		         device_path_of_blockdevice(bdev));
		prompt(input, sizeof(input), "consent_fsck", question, "yes");
		if ( !strcasecmp(input, "yes") )
			return true;
		else if ( !strcasecmp(input, "no") )
			return false;
	}
}

static bool should_install_bootloader_bdev(struct blockdevice* bdev)
{
	if ( !bdev->fs )
		return false;
	if ( bdev->fs->flags & FILESYSTEM_FLAG_NOT_FILESYSTEM )
		return false;
	if ( !bdev->fs->driver )
		return false;
	if ( (bdev->fs->flags & FILESYSTEM_FLAG_FSCK_MUST) &&
	     !consent_fsck(bdev) )
	{
		textf("Not probing inconsistent filesystem %s.\n",
		      device_path_of_blockdevice(bdev));
		return false;
	}
	char* mnt = join_paths(get_tmpdir(), "fs.XXXXXX");
	if ( !mnt )
	{
		warn("malloc");
		return false;
	}
	if ( !mkdtemp(mnt) )
	{
		warn("mkdtemp: %s", mnt);
		free(mnt);
		return false;
	}
	struct mountpoint mp = { 0 };
	mp.absolute = mnt;
	mp.fs = bdev->fs;
	mp.entry.fs_spec = (char*) (bdev->hd ? bdev->hd->path : bdev->p->path);
	mp.entry.fs_file = (char*) mnt;
	mp.entry.fs_vfstype = (char*) bdev->fs->fstype_name;
	mp.entry.fs_mntops = (char*) "ro";
	mp.entry.fs_type = (char*) "ro";
	if ( !mountpoint_mount(&mp) )
	{
		rmdir(mnt);
		free(mnt);
		return false;
	}
	bool should = should_install_bootloader_path(mnt, bdev);
	mountpoint_unmount(&mp);
	rmdir(mnt);
	free(mnt);
	return should;
}

static bool should_install_bootloader(void)
{
	char firmware[16];
	if ( !kernelinfo("firmware", firmware, sizeof(firmware)) &&
	     !strcmp(firmware, "efi") )
		return true;
	bool any_systems = false;
	for ( size_t i = 0; i < hds_count; i++ )
	{
		struct harddisk* hd = hds[i];
		if ( !hd->writable )
			continue;
		if ( hd->bdev.pt )
		{
			for ( size_t n = 0; n < hd->bdev.pt->partitions_count; n++ )
			{
				struct partition* p = hd->bdev.pt->partitions[n];
				if ( should_ignore_bootloader_on_filesystem(&p->bdev) )
					continue;
				any_systems = true;
				if ( should_install_bootloader_bdev(&p->bdev) )
					return true;
			}
		}
		else if ( hd->bdev.fs &&
		          !should_ignore_bootloader_on_filesystem(&hd->bdev) )
		{
			any_systems = true;
			if ( should_install_bootloader_bdev(&hd->bdev) )
				return true;
		}
	}
	return !any_systems;
}

static bool passwd_check(const char* passwd_path,
                         bool (*check)(struct passwd*, void*),
                         void* check_ctx)
{
	FILE* passwd = fopen(passwd_path, "r");
	if ( !passwd )
	{
		if ( errno != ENOENT )
			warn("%s", passwd_path);
		return false;
	}
	char* line = NULL;
	size_t size = 0;
	ssize_t length;
	while ( 0 < (length = getline(&line, &size, passwd) ) )
	{
		if ( line[size - 1] == '\n' )
			line[--size] = '\0';
		struct passwd pwd;
		if ( scanpwent(line, &pwd) && check(&pwd, check_ctx) )
		{
			free(line);
			fclose(passwd);
			return true;
		}
	}
	free(line);
	if ( ferror(passwd) )
		warn("%s", passwd_path);
	fclose(passwd);
	return false;
}

static bool passwd_has_uid_check(struct passwd* pwd, void* ctx)
{
	return pwd->pw_uid == *(uid_t*) ctx;
}

static bool passwd_has_uid(const char* passwd_path, uid_t uid)
{
	return passwd_check(passwd_path, passwd_has_uid_check, &uid);
}

static bool passwd_has_name_check(struct passwd* pwd, void* ctx)
{
	return !strcmp(pwd->pw_name, (const char*) ctx);
}

static bool passwd_has_name(const char* passwd_path, const char* name)
{
	return passwd_check(passwd_path, passwd_has_name_check, (void*) name);
}

static void install_skel(const char* home, uid_t uid, gid_t gid)
{
	if ( access("etc/skel", F_OK) < 0 && errno == ENOENT )
		return;
	const char* argv[] = { "cp", "-RT", "--", "etc/skel", home, NULL };
	execute(argv, "ug", uid, gid);
}

__attribute__((format(printf, 3, 4)))
static bool install_configurationf(const char* path,
                                   const char* mode,
                                   const char* format,
                                   ...)
{
	FILE* fp = fopen(path, mode);
	if ( !fp )
	{
		warn("%s", path);
		return false;
	}
	va_list ap;
	va_start(ap, format);
	int status = vfprintf(fp, format, ap);
	va_end(ap);
	if ( status < 0 )
	{
		warn("%s", path);
		fclose(fp);
		return false;
	}
	if ( fclose(fp) == EOF )
	{
		warn("%s", path);
		return false;
	}
	return true;
}

static void grub_hash_password(char* buffer, size_t buffer_size, const char* pw)
{
	int pipe_fds[2];
	if ( pipe(pipe_fds) < 0 )
		err(2, "pipe");
	pid_t pid = fork();
	if ( pid < 0 )
		err(2, "fork");
	if ( pid == 0 )
	{
		close(pipe_fds[0]);
		if ( dup2(pipe_fds[1], 1) < 0 )
			_exit(2);
		close(pipe_fds[1]);
		const char* argv[] = { "grub-mkpasswd-pbkdf2", "-p", pw, NULL };
		execvp(argv[0], (char* const*) argv);
		_exit(127);
	}
	close(pipe_fds[1]);
	size_t done = 0;
	while ( done < buffer_size )
	{
		ssize_t amount = read(pipe_fds[0], buffer + done, buffer_size - done);
		if ( amount < 0 )
			err(2, "read");
		if ( amount == 0 )
			break;
		done += amount;
	}
	if ( done && buffer[done-1] == '\n' )
		done--;
	if ( done == buffer_size )
		done--;
	buffer[done] = '\0';
	close(pipe_fds[0]);
	int exit_code;
	waitpid(pid, &exit_code, 0);
	if ( !WIFEXITED(exit_code) || WEXITSTATUS(exit_code) != 0 )
		errx(2, "grub password hash failed");
}


static const char* const ignore_kernel_options[] =
{
	"--firmware=bios",
	"--firmware=efi",
	"--firmware=pc",
	"--no-random-seed",
	"--random-seed",
	NULL,
};

static char* normalize_kernel_options(void)
{
	char* options = akernelinfo("options");
	if ( !options )
	{
		warn("kernelinfo: options");
		return NULL;
	}
	size_t i = 0, o = 0;
	while ( options[i] )
	{
		if ( isspace((unsigned char) options[i]) )
		{
			i++;
			continue;
		}
		if ( options[i] != '-' ) // Imperfect since quoting options is allowed.
			break;
		if ( !strncmp(options + i, "--", 2) &&
		     (!options[i + 2] || isspace((unsigned char) options[i + 2])) )
			break;
		bool ignored = false;
		for ( size_t n = 0; ignore_kernel_options[n]; n++ )
		{
			const char* opt = ignore_kernel_options[n];
			size_t len = strlen(opt);
			if ( !strncmp(options + i, opt, len) &&
			     (!options[i + len] ||
			      isspace((unsigned char) options[i + len])) )
			{
				i += len;
				ignored = true;
				break;
			}
		}
		if ( ignored )
			continue;
		bool singly = false;
		bool doubly = false;
		bool escaped = false;
		for ( ; options[i]; i++ )
		{
			char c = options[i];
			options[o++] = c;
			if ( !escaped && !singly && !doubly && isspace((unsigned char) c) )
				break;
			if ( !escaped && !doubly && c == '\'' )
			{
				singly = !singly;
				continue;
			}
			if ( !escaped && !singly && c == '"' )
			{
				doubly = !doubly;
				continue;
			}
			if ( !singly && !escaped && c == '\\' )
			{
				escaped = true;
				continue;
			}
			escaped = false;
		}
	}
	while ( o && isspace((unsigned char) options[o - 1]) )
		o--;
	options[o] = '\0';
	return options;
}

static pid_t main_pid;
static struct mountpoint* mountpoints;
static size_t mountpoints_used;
static bool etc_made = false;
static char* etc;
static bool fs_made = false;
static char* fs;
static int exit_gui_code = -1;

static void unmount_all_but_root(void)
{
	for ( size_t n = mountpoints_used; n != 0; n-- )
	{
		size_t i = n - 1;
		struct mountpoint* mountpoint = &mountpoints[i];
		if ( !strcmp(mountpoint->entry.fs_file, "/") )
			continue;
		mountpoint_unmount(mountpoint);
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
	if ( etc_made )
		execute((const char*[]) { "rm", "-rf", etc, NULL }, "");
	if ( 0 <= exit_gui_code )
		gui_shutdown(exit_gui_code);
}

void exit_gui(int code)
{
	exit_gui_code = code;
	exit(code);
}

static void cancel_on_sigint(int signum)
{
	(void) signum;
	errx(2, "fatal: Installation canceled");
}

int main(void)
{
	shlvl();

	if ( getuid() != 0 )
		errx(2, "You need to be root to install %s", BRAND_DISTRIBUTION_NAME);
	if ( getgid() != 0 )
		errx(2, "You need to be group root to install %s", BRAND_DISTRIBUTION_NAME);

	main_pid = getpid();
	if ( atexit(exit_handler) != 0 )
		err(2, "atexit");

	if ( !(etc = join_paths(get_tmpdir(), "etc.XXXXXX")) )
		err(2, "malloc");
	if ( !mkdtemp(etc) )
		err(2, "mkdtemp: %s", etc);
	etc_made = true;
	// Export for the convenience of users escaping to a shell.
	setenv("SYSINSTALL_ETC", fs, 1);

	if ( chdir(etc) < 0 )
		err(2, "chdir: %s", etc);

	struct utsname uts;
	if ( uname(&uts) < 0 )
		err(1, "uname");

	char firmware[16];
	if ( kernelinfo("firmware", firmware, sizeof(firmware)) != 0 )
		err(1, "kernelinfo");

	struct conf conf;
	conf_init(&conf);
	if ( !conf_load(&conf, "/etc/upgrade.conf") && errno != ENOENT )
		warn("/etc/upgrade.conf");

	autoconf_load("/etc/autoinstall.conf");

	char* accepts_defaults = autoconf_eval("accept_defaults");
	bool non_interactive = accepts_defaults &&
	                       !strcasecmp(accepts_defaults, "yes");
	free(accepts_defaults);

	if ( !non_interactive && !isatty(0) )
		errx(2, "fatal: stdin is not a terminal");
	if ( !non_interactive && !isatty(1) )
		errx(2, "fatal: stdout is not a terminal");
	if ( !non_interactive && !isatty(2) )
		errx(2, "fatal: stderr is not a terminal");

	setvbuf(stdout, NULL, _IOLBF, 0);

	textf("Hello and welcome to the " BRAND_DISTRIBUTION_NAME " " VERSIONSTR ""
	      " installer for %s.\n\n", uts.machine);

	if ( non_interactive ||
	     (autoconf_has("ready") &&
	      (autoconf_has("disked") || autoconf_has("confirm_install"))) )
	{
		int countdown = 10;
		if ( autoconf_has("countdown") )
		{
			char* string = autoconf_eval("countdown");
			countdown = atoi(string);
			free(string);
		}
		sigset_t old_set;
		sigset_t new_set;
		sigemptyset(&new_set);
		sigaddset(&new_set, SIGINT);
		sigprocmask(SIG_BLOCK, &new_set, &old_set);
		struct sigaction old_sa;
		struct sigaction new_sa = { 0 };
		new_sa.sa_handler = cancel_on_sigint;
		sigaction(SIGINT, &new_sa, &old_sa);
		for ( ; 0 < countdown; countdown-- )
		{
			textf("Automatically installing " BRAND_DISTRIBUTION_NAME " "
			      VERSIONSTR " in %i %s... (Control-C to cancel)\n", countdown,
			      countdown != 1 ? "seconds" : "second");
			sigprocmask(SIG_SETMASK, &old_set, NULL);
			sleep(1);
			sigprocmask(SIG_BLOCK, &new_set, &old_set);
		}
		textf("Automatically installing " BRAND_DISTRIBUTION_NAME " "
		      VERSIONSTR "...\n");
		text("\n");
		sigaction(SIGINT, &old_sa, NULL);
		sigprocmask(SIG_SETMASK, &old_set, NULL);
	}

	// '|' rather than '||' is to ensure side effects.
	if ( missing_program("cut") |
	     missing_program("dash") |
	     missing_program("fsck.ext2") |
	     missing_program("grub-install") |
	     missing_program("man") |
	     (!strcmp(firmware, "efi") && missing_program("mkfs.fat")) |
	     missing_program("sed") |
	     missing_program("xargs") )
	{
		text("Warning: This system does not have the necessary third party "
		     "software installed to properly install this operating system.\n");
		while ( true )
		{
			prompt(input, sizeof(input), "ignore_missing_programs",
			       "Sure you want to proceed?", "no");
			if ( strcasecmp(input, "no") == 0 )
				return 0;
			if ( strcasecmp(input, "yes") == 0 )
				break;
		}
		text("\n");
	}

	text("You are about to install a new operating system on this computer. "
	     "This is not something you should do on a whim or when you are "
	     "impatient. Take the time to read the documentation and be patient "
	     "while you learn the new system. This is a very good time to start an "
	     "external music player that plays soothing classical music on loop.\n\n");

	if ( !access_or_die("/tix/tixinfo/ssh", F_OK) &&
	     access_or_die("/root/.ssh/authorized_keys", F_OK) < 0 )
		text("If you wish to ssh into your new installation, it's recommended "
		     "to first add your public keys to the .iso and obtain "
		     "fingerprints per release-iso-modification(7) before installing."
		     "\n\n");

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
		"It's very good music",
	};
	size_t num_readies = sizeof(readies) / sizeof(readies[0]);
	const char* ready = readies[arc4random_uniform(num_readies)];
	if ( autoconf_has("disked") )
		text("Warning: This installer will perform automatic harddisk "
		     "partitioning!\n");
	if ( autoconf_has("confirm_install") )
		text("Warning: This installer will automatically install an operating "
		     "system!\n");
	prompt(input, sizeof(input), "ready", "Ready?", ready);
	text("\n");

	text("This is not yet a fully fledged operating system. You should adjust "
	     "your expectations accordingly. The system protects against remote "
	     "threats. However, you should not consider the system secure for local "
	     "multi-user use. Filesystem permissions are not enforced yet. There "
	     "are known local-user security issues, so setuid(2) currently "
	     "blatantly allows any user to become root, to not give a false sense "
	     "of security.\n\n");

	text("You can always escape to a shell by answering '!' to any regular "
	     "prompt. You can view the installation(7) manual page by "
	     "answering '!man'. Default answers are in []'s and can be selected by "
	     "pressing enter.\n\n");

	install_configurationf("upgrade.conf", "a", "src = yes\n");

	bool kblayout_setable = 0 <= tcgetblob(0, "kblayout", NULL, 0) ||
	                        getenv("DISPLAY_SOCKET");
	while ( kblayout_setable )
	{
		// TODO: Detect the name of the current keyboard layout.
		prompt(input, sizeof(input), "kblayout",
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
	{
		if ( !input[0] || !strcmp(input, "default") )
			text("/etc/kblayout will not be created (default).\n");
		else
		{
			textf("/etc/kblayout will be set to \"%s\".\n", input);
			mode_t old_umask = getumask();
			umask(022);
			install_configurationf("kblayout", "w", "%s\n", input);
			umask(old_umask);
		}
		text("\n");
	}

	struct dispmsg_crtc_mode mode;
	if ( get_video_mode(&mode) )
	{
		bool good = (mode.control & DISPMSG_CONTROL_VALID) &&
		            (mode.control & DISPMSG_CONTROL_GOOD_DEFAULT);
		if ( mode.control & DISPMSG_CONTROL_VM_AUTO_SCALE )
		{
			text("The display resolution will automatically change to "
			     "match the size of the virtual machine window.\n\n");
			good = true;
		}
		const char* def = non_interactive || good ? "no" : "yes";
		while ( true )
		{
			prompt(input, sizeof(input), "videomode",
			       "Select display resolution? "
			       "(yes/no/WIDTHxHEIGHTxBPP)", def);
			unsigned int xres, yres, bpp;
			bool set = sscanf(input, "%ux%ux%u", &xres, &yres, &bpp) == 3;
			if ( !strcasecmp(input, "no") )
			{
				input[0] = '\0';
				break;
			}
			const char* r = set ? input : NULL;
			if ( execute((const char*[]) { "chvideomode", r, NULL }, "f") != 0 )
				continue;
			input[0] = '\0';
			if ( !get_video_mode(&mode) ||
			     !(mode.control & DISPMSG_CONTROL_VALID) ||
			     mode.control & DISPMSG_CONTROL_VGA )
				continue;
			snprintf(input, sizeof(input), "%ux%ux%u",
			         mode.view_xres, mode.view_yres, mode.fb_format);
			break;
		}

		if ( !input[0] )
			text("/etc/videomode will not be created.\n");
		else
		{
			textf("/etc/videomode will be set to \"%s\".\n", input);
			mode_t old_umask = getumask();
			umask(022);
			install_configurationf("videomode", "w", "%s\n", input);
			umask(old_umask);
		}
		text("\n");
	}

	text("Searching for existing installations...\n");
	scan_devices();
	bool bootloader_default = should_install_bootloader();
	text("\n");

	textf("You need a bootloader to start the operating system. GRUB is the "
	      "standard %s bootloader and this installer comes with a copy.\n\n",
	      BRAND_DISTRIBUTION_NAME);
	if ( !strcmp(firmware, "efi") )
	{
		text("This GRUB bootloader will live side by side with other "
		     "bootloaders in the EFI System Partition. You should accept this "
		     "bootloader, otherwise you will have to manually arrange for "
		     "bootloading by configuring any existing multiboot compliant "
		     "bootloader.\n");
	}
	else
	{
		text(" Single-boot installations should accept this bootloader.\n");
		text("Dual-boot systems should refuse it and manually arrange for "
		     "bootloading by configuring any existing multiboot compliant "
		     "bootloader.\n");
	}
	text("\n");
	char accept_grub[10];
	char accept_grub_password[10];
	char grub_password[512];
	while ( true )
	{
		const char* def = bootloader_default ? "yes" : "no";
		prompt(accept_grub, sizeof(accept_grub), "grub",
		       "Install the GRUB bootloader?", def);
		if ( strcasecmp(accept_grub, "no") == 0 ||
		     strcasecmp(accept_grub, "yes") == 0 )
			break;
	}
	text("\n");

	char efi_bootloader[256 + sizeof("/boot/EFI//BOOTXXXX.EFI")] = "";
	char grub_distributor[256] = "";
	if ( strcasecmp(accept_grub, "yes") == 0 && !strcmp(firmware, "efi") )
	{
		char lower_brand[] = BRAND_DISTRIBUTION_NAME;
		for ( size_t i = 0; lower_brand[i]; i++ )
			lower_brand[i] = tolower((unsigned char) lower_brand[i]);
		#ifdef __x86_64__
		char efi_boot[12] = "BOOTX64.EFI";
		char efi_grub[12] = "grubx64.efi";
		#elif defined(__i386__)
		char efi_boot[12] = "BOOTIA32.EFI";
		char efi_grub[12] = "grubia32.efi";
		#elif defined(__ia64__)
		char efi_boot[12] = "BOOTIA64.EFI";
		char efi_grub[12] = "grubia64.efi";
		#elif defined(__arm__)
		char efi_boot[12] = "BOOTARM.EFI";
		char efi_grub[12] = "grubarm.efi";
		#elif defined(__aarch64__)
		char efi_boot[12] = "BOOTAA64.EFI";
		char efi_grub[12] = "grubaa64.efi";
		#elif defined(__riscv) && __INTPTR_WIDTH__ == 32
		char efi_boot[12] = "BOOTRISCV32.EFI";
		char efi_grub[12] = "grubriscv32.efi";
		#elif defined(__riscv) && __INTPTR_WIDTH__ == 64
		char efi_boot[12] = "BOOTRISCV64.EFI";
		char efi_grub[12] = "grubriscv64.efi";
		#elif defined(__riscv) && __INTPTR_WIDTH__ == 128
		char efi_boot[12] = "BOOTRISCV128.EFI";
		char efi_grub[12] = "grubriscv128.efi";
		#elif defined(__loongarch__)
		char efi_boot[12] = "BOOTLOONGARCH32.EFI";
		char efi_grub[12] = "grubloongarch32.efi";
		#elif defined(__loongarch64)
		char efi_boot[12] = "BOOTLOONGARCH64.EFI";
		char efi_grub[12] = "grubloongarch64.efi";
		#else
		#warning Add EFI architecture here
		char efi_boot[12] = "BOOT.EFI";
		char efi_grub[12] = "grub.efi";
		#endif
		textf("Bootloaders live side-by-side in the EFI System Partition "
		      "(/boot/efi) inside EFI subdirectories. The /boot/efi/EFI/BOOT/%s"
		      " bootloader is booted as a fallback if no boot order has been "
		      "configured in EFI's non-volatile memory. If boot entries are "
		      "written to EFI non-volatile memory, then operating system "
		      "installation specific directories, such as /boot/efi/EFI/%s, "
		      "can be used to dual boot systems.\n\n", efi_boot, lower_brand);
		textf(" - 'BOOT' should be picked for single boot systems, and creates "
		     "a removable EFI installation that isn't computer specific.\n");
		textf(" - '%s' should be picked for dual boot systems, but you will "
		      "need to manually use the firmware settings to set the boot "
		      "variable, or use efibootmgr on another operating system.\n\n",
		      lower_brand);
		char question[256];
		snprintf(question, sizeof(question),
		         "EFI directory to install GRUB into? "
		         "(BOOT/%s/...)", lower_brand);
		while ( true )
		{
			prompt(grub_distributor, sizeof(grub_distributor),
			       "grub_distributor", question, "BOOT");
			if ( strchr(grub_distributor, '/') )
				continue;
			break;
		}
		if ( !strcasecmp(grub_distributor, "BOOT") )
		{
			strlcpy(grub_distributor, "BOOT", sizeof(grub_distributor));
			install_configurationf("grub", "w", "GRUB_REMOVABLE=true\n");
		}
		else
		{
			install_configurationf("grub", "w", "GRUB_DISTRIBUTOR='%s'\n",
			                       grub_distributor);
			snprintf(efi_bootloader, sizeof(efi_bootloader), "EFI\\%s\\%s",
			         grub_distributor, efi_grub);
		}
		text("\n");
	}

	if ( strcasecmp(accept_grub, "yes") == 0 )
	{
		install_configurationf("upgrade.conf", "a", "grub = yes\n");

		text("If an unauthorized person has access to the bootloader command "
		     "line, then the whole system security can be compromised. You can "
		     "prevent this by password protecting interactive use of the "
		     "bootloader, but still allowing anyone to start the system "
		     "normally. Similarly you may wish to manually go into your "
		     "firmware and password protect it.\n");
		text("\n");
		while ( true )
		{
			const char* def =
				non_interactive &&
				!autoconf_has("grub_password_hash") ? "no" : "yes";
			prompt(accept_grub_password, sizeof(accept_grub_password),
			       "grub_password",
			       "Password protect interactive bootloader? (yes/no)", def);
			if ( strcasecmp(accept_grub_password, "no") == 0 ||
			     strcasecmp(accept_grub_password, "yes") == 0 )
				break;
		}
		if ( autoconf_has("grub_password_hash") )
		{
			char* hash = autoconf_eval("grub_password_hash");
			install_configurationf("grubpw", "w", "%s\n", hash);
			free(hash);
		}
		else while ( !strcasecmp(accept_grub_password, "yes") )
		{
			char first[128];
			char second[128];
			password(first, sizeof(first),
			         "Bootloader root password? (will not echo)");
			password(second, sizeof(second),
			         "Bootloader root password? (again)");
			if ( strcmp(first, second) != 0 )
			{
				printf("Passwords do not match, try again.\n");
				continue;
			}
			explicit_bzero(second, sizeof(second));
			if ( !strcmp(first, "") )
			{
				char answer[32];
				prompt(answer, sizeof(answer), "grub_password_empty",
				       "Empty password is stupid, are you sure? (yes/no)", "no");
				if ( strcasecmp(answer, "yes") != 0 )
					continue;
			}
			grub_hash_password(grub_password, sizeof(grub_password), first);
			textf("/etc/grubpw will be made with grub-mkpasswd-pbkdf2.\n");
			mode_t old_umask = getumask();
			umask(077);
			install_configurationf("grubpw", "w", "%s\n", grub_password);
			umask(old_umask);
			break;
		}
		text("\n");
	}

	char* kernel_options = normalize_kernel_options();
	if ( (autoconf_has("kernel_options") ||
	      (kernel_options && kernel_options[0])) &&
	     !access_or_die("/tix/tixinfo/grub", F_OK) )
	{
		text("The operating system was booted with explicit kernel(7) options. "
		     "Would you like set them permanently in /etc/grub?\n\n");

		while ( true )
		{
			char options[1024];
			prompt(options, sizeof(options), "kernel_options",
				   "Kernel options? (OPTIONS/no)", kernel_options);
			if ( !strcasecmp(options, "no") )
			{
				kernel_options = NULL;
				break;
			}
			if ( options[0] )
			{
				install_configurationf("grub", "w",
				                       "GRUB_CMDLINE_SORTIX='%s'\n", options);
				textf("/etc/grub will be made with the kernel options.\n");
			}
			break;
		}
		text("\n");
	}
	free(kernel_options);

	// TODO: Offer the user an automatic layout of partitions if the disk is
	//       empty.

	// TODO: Perhaps let the user know the size of the system that will be
	//       installed?

	text("You need to select a root filesystem and other mountpoints now. You "
	     "will now be dumped into a partition editor. Create and format a "
	     "root filesystem partition as needed.\n");
	text("\n");
	const char* mktable_tip = "";
	if ( check_lacking_partition_table() )
		mktable_tip = "Type mktable to make a new partition table. ";
	const char* devices_tip = "";
	if ( check_multiple_harddisks() )
		devices_tip = "Type devices to list the devices. "
		              "Type device 1 to switch to device 1. ";
	textf("Type ls to list partitions on the device. "
	      "%s"
	      "%s"
	      "Type mkpart to make a new partition. "
	      "Type mount 2 / to create a mountpoint for partition 2. "
	      "Type exit when done. "
	      "There is partitioning advice in installation(7). "
	      "Type man 8 disked to display the disked(8) man page.\n",
	      mktable_tip, devices_tip);
	struct filesystem* root_filesystem = NULL;
	struct filesystem* boot_filesystem = NULL;
	struct filesystem* esp_filesystem = NULL;
	struct filesystem* bootloader_filesystem = NULL;
	bool not_first = false;
	while ( true )
	{
		if ( not_first )
			text("Type man to display the disked(8) man page.\n");
		not_first = true;
		const char* argv[] = { "disked", "--fstab=fstab", NULL };
		char* disked_input = autoconf_eval("disked");
		if ( execute(argv, "fi", disked_input) != 0 )
		{
			if ( disked_input )
				errx(2, "partitioning failed");
			free(disked_input);
			// TODO: We also end up here on SIGINT.
			// TODO: Offer a shell here instead of failing?
			warnx("partitioning failed");
			sleep(1);
			continue;
		}
		free(disked_input);
		free_mountpoints(mountpoints, mountpoints_used);
		mountpoints = NULL;
		mountpoints_used = 0;
		scan_devices();
		if ( !load_mountpoints("fstab", &mountpoints, &mountpoints_used) )
		{
			if ( errno == ENOENT )
				text("You have not created any mountpoints. Try again.\n");
			else
				warn("fstab");
			continue;
		}
		bool found_rootfs = false;
		for ( size_t i = 0; !found_rootfs && i < mountpoints_used; i++ )
			if ( !strcmp(mountpoints[i].entry.fs_file, "/") )
				found_rootfs = true;
		if ( !found_rootfs )
		{
			text("You have no root filesystem mountpoint. Try again.\n");
			continue;
		}
		root_filesystem = NULL;
		boot_filesystem = NULL;
		esp_filesystem = NULL;
		bool cant_mount = false;
		for ( size_t i = 0; i < mountpoints_used; i++ )
		{
			struct mountpoint* mnt = &mountpoints[i];
			const char* spec = mnt->entry.fs_spec;
			if ( !(mnt->fs = search_for_filesystem_by_spec(spec)) )
			{
				warnx("fstab: %s: Found no mountable filesystem matching `%s'",
				      mnt->entry.fs_file, spec);
				cant_mount = true;
				continue;
			}
			if ( !mnt->fs->driver )
			{
				warnx("fstab: %s: %s: Don't know how to mount this %s filesystem",
				      mnt->entry.fs_file,
				      path_of_blockdevice(mnt->fs->bdev),
				      mnt->fs->fstype_name);
				cant_mount = true;
				continue;
			}
			if ( !strcmp(mnt->entry.fs_file, "/") )
				root_filesystem = mnt->fs;
			if ( !strcmp(mnt->entry.fs_file, "/boot") )
				boot_filesystem = mnt->fs;
			if ( !strcmp(mnt->entry.fs_file, "/boot/efi") )
				esp_filesystem = mnt->fs;
		}
		if ( cant_mount )
			continue;
		assert(root_filesystem);
		bootloader_filesystem = boot_filesystem ? boot_filesystem : root_filesystem;
		assert(bootloader_filesystem);
		if ( !strcasecmp(accept_grub, "yes") &&
		     !strcmp(firmware, "bios") &&
		     missing_bios_boot_partition(bootloader_filesystem) )
		{
			const char* where = boot_filesystem ? "/boot" : "root";
			const char* dev = device_path_of_blockdevice(bootloader_filesystem->bdev);
			assert(dev);
			textf("You are installing a BIOS bootloader and the %s "
			      "filesystem is located on a GPT partition, but you haven't "
			      "made a BIOS boot partition on the %s GPT disk. Pick "
			      "biosboot during mkpart and make a 1 MiB partition.\n",
			      where, dev);
			char return_to_disked[10];
			while ( true )
			{
				prompt(return_to_disked, sizeof(return_to_disked),
				       "missing_bios_boot_partition",
				       "Return to disked to make a BIOS boot partition?", "yes");
				if ( strcasecmp(accept_grub, "no") == 0 ||
					 strcasecmp(accept_grub, "yes") == 0 )
					break;
			}
			if ( !strcasecmp(return_to_disked, "yes") )
				continue;
			text("Proceeding, but expect the installation to fail.\n");
		}
		else if ( !strcasecmp(accept_grub, "yes") &&
		          !strcmp(firmware, "efi") &&
		          !esp_filesystem )
		{
			textf("You are installing an EFI bootloader, but you haven't made "
			      "an EFI System Partition. Pick efi during mkpart and make a "
			      "partition and mount it as /boot/efi.\n");
			char return_to_disked[10];
			while ( true )
			{
				prompt(return_to_disked, sizeof(return_to_disked),
				       "missing_esp_partition",
				       "Return to disked to make an EFI partition?", "yes");
				if ( strcasecmp(accept_grub, "no") == 0 ||
					 strcasecmp(accept_grub, "yes") == 0 )
					break;
			}
			if ( !strcasecmp(return_to_disked, "yes") )
				continue;
			text("Proceeding, but expect the installation to fail.\n");
		}

		break;
	}
	text("\n");

	textf("We are now ready to install %s %s. Take a moment to verify "
	      "everything is in order.\n", BRAND_DISTRIBUTION_NAME, VERSIONSTR);
	text("\n");
	printf("  %-16s  system architecture\n", uts.machine);
	for ( size_t i = 0; i < mountpoints_used; i++ )
	{
		struct mountpoint* mnt = &mountpoints[i];
		const char* devname = path_of_blockdevice(mnt->fs->bdev);
		const char* where = mnt->entry.fs_file;
		printf("  %-16s  use as %s\n", devname, where);
	}
	if ( strcasecmp(accept_grub, "yes") == 0 && !strcmp(firmware, "bios") )
	{
		struct partition* bbp = search_bios_boot_partition(bootloader_filesystem);
		if ( bbp )
			printf("  %-16s  bios boot partition\n",
			       path_of_blockdevice(&bbp->bdev));
		printf("  %-16s  bootloader installation target\n",
		       device_path_of_blockdevice(bootloader_filesystem->bdev));
	}
	if ( strcasecmp(accept_grub, "yes") == 0 && !strcmp(firmware, "efi") )
	{
		printf("  %-16s  grub efi directory\n",
		       grub_distributor);
	}
	text("\n");

	while ( true )
	{
		prompt(input, sizeof(input), "confirm_install",
		       "Install " BRAND_DISTRIBUTION_NAME "? "
		       "(yes/no/exit/poweroff/reboot/halt)", "yes");
		if ( !strcasecmp(input, "yes") )
			break;
		else if ( !strcasecmp(input, "no") )
		{
			text("Answer '!' to get a shell. Type !man to view the "
			     "installation(7) manual page.\n");
			text("Alternatively, you can answer 'poweroff', 'reboot', or "
			     "'halt' to cancel the installation.\n");
			continue;
		}
		else if ( !strcasecmp(input, "exit") )
			exit(0);
		else if ( !strcasecmp(input, "poweroff") )
			exit_gui(0);
		else if ( !strcasecmp(input, "reboot") )
			exit_gui(1);
		else if ( !strcasecmp(input, "halt") )
			exit_gui(2);
		else
			continue;
	}
	text("\n");

	text("Installing " BRAND_DISTRIBUTION_NAME " " VERSIONSTR " now:\n");

	printf(" - Mounting filesystems...\n");

	if ( !(fs = join_paths(get_tmpdir(), "fs.XXXXXX")) )
		err(2, "malloc");
	if ( !mkdtemp(fs) )
		err(2, "mkdtemp: %s", fs);
	fs_made = true;
	// Export for the convenience of users escaping to a shell.
	setenv("SYSINSTALL_TARGET", fs, 1);

	for ( size_t i = 0; i < mountpoints_used; i++ )
	{
		struct mountpoint* mnt = &mountpoints[i];
		char* absolute;
		if ( asprintf(&absolute, "%s%s", fs, mnt->absolute) < 0 )
			err(2, "asprintf");
		free(mnt->absolute);
		mnt->absolute = absolute;
		if ( mkdir_p(mnt->absolute, 0755) < 0 )
			err(2, "mkdir: %s", mnt->absolute);
		if ( !mountpoint_mount(mnt) )
			exit(2);
	}

	if ( chdir(fs) < 0 )
		err(2, "chdir: %s", fs);

	pid_t install_pid = fork();
	if ( install_pid < 0 )
		err(2, "fork");
	if ( install_pid == 0 )
	{
		printf(" - Populating root filesystem...\n");
		chmod(".", 0755);
		execute((const char*[]) { "tix-create", "-C", ".", "--import=/", NULL },
		        "_e");
		install_manifests_detect("", ".", true, true, true, false);
		// TODO: Preserve the existing /src if it exists like in sysupgrade.
		if ( has_manifest("src") )
			install_manifest("src", "", ".", (const char*[]){}, 0, false);
		printf(" - Creating configuration files...\n");
		// TODO: Preserve mode/ownership/timestamps?
		execute((const char*[]) { "cp", "-RTP", etc, "etc", NULL }, "_e");
		// TODO: Auto detect appropriate bcrypt rounds and set up etc/login.conf
		//       and use those below instead of bcrypt,a.
		if ( access_or_die("boot/random.seed", F_OK) < 0 )
		{
			printf(" - Creating random seed...\n");
			write_random_seed("boot/random.seed");
		}
		printf(" - Creating initrd...\n");
		execute((const char*[]) { "update-initrd", "--sysroot", fs, NULL }, "_e");
		if ( strcasecmp(accept_grub, "yes") == 0 )
		{
			printf(" - Installing bootloader...\n");
			execute((const char*[]) { "grub-install", "-q", NULL }, "_ce", ".");
			printf(" - Configuring bootloader...\n");
			execute((const char*[]) { "update-grub", NULL }, "_ceqQ", ".");
		}
		else if ( access_or_die("etc/default/grub.d/10_sortix", F_OK) == 0 )
		{
			// Help dual booters by making /etc/default/grub.d/10_sortix.cache.
			printf(" - Creating bootloader fragment...\n");
			execute((const char*[]) { "chroot", "-d", ".",
			                          "/etc/default/grub.d/10_sortix",
			                          NULL }, "_eq");
		}
		printf(" - Finishing installation...\n");
		fflush(stdout);
		_exit(0);
	}
	int install_code;
	waitpid(install_pid, &install_code, 0);
	if ( WIFEXITED(install_code) && WEXITSTATUS(install_code) == 0 )
	{
	}
	else if ( WIFEXITED(install_code) )
		errx(2, "installation failed with exit status %i", WEXITSTATUS(install_code));
	else if ( WIFSIGNALED(install_code) )
		errx(2, "installation failed: %s", strsignal(WTERMSIG(install_code)));
	else
		errx(2, "installation failed: unknown waitpid code %i", install_code);

	unsetenv("SYSINSTALL_ETC");
	execute((const char*[]) { "rm", "-r", etc, NULL }, "");
	etc_made = false;

	text("\n");
	text("System files are now installed. We'll now make the system functional "
	     "by configuring a few essential matters.\n\n");

	umask(0022);

	if ( access("etc/hostname", F_OK) == 0 )
		textf("/etc/hostname already exists, skipping creating it.\n");
	else while ( true )
	{
		char defhost[HOST_NAME_MAX + 1] = "";
		if ( non_interactive )
			gethostname(defhost, sizeof(defhost));
		FILE* defhost_fp = fopen("etc/hostname", "r");
		if ( defhost_fp )
		{
			fgets(defhost, sizeof(defhost), defhost_fp);
			size_t defhostlen = strlen(defhost);
			if ( defhostlen && defhost[defhostlen-1] == '\n' )
				defhost[defhostlen-1] = '\0';
			fclose(defhost_fp);
		}
		char hostname[HOST_NAME_MAX + 1] = "";
		prompt(hostname, sizeof(hostname), "hostname", "System hostname?",
		       defhost[0] ? defhost : NULL);
		if ( !is_valid_hostname(hostname) )
		{
			if ( non_interactive )
				errx(2, "fatal: invalid hostname: %s", hostname);
			text("Invalid hostname\n");
			continue;
		}
		if ( !install_configurationf("etc/hostname", "w", "%s\n", hostname) )
			continue;
		textf("/etc/hostname was set to \"%s\".\n", hostname);
		break;
	}
	text("\n");

	if ( mkdir("root", 0700) < 0 )
	{
		if ( errno == EEXIST )
		{
			if ( chmod("root", 0700) < 0 )
				warn("chmod: root");
		}
		else
			warn("mkdir: root");
	}
	if ( passwd_has_uid("etc/passwd", 0) ||
	     passwd_has_name("etc/passwd", "root") )
	{
		textf("Root account already exists, skipping creating it.\n");
	}
	else if ( non_interactive || autoconf_has("password_hash_root") )
	{
		char* hash = autoconf_eval("password_hash_root");
		if ( !hash && !(hash = strdup("x")) )
			err(2, "malloc");
		if ( !install_configurationf("etc/passwd", "a",
			"root:%s:0:0:root:/root:sh\n"
			"include /etc/default/passwd.d/*\n", hash) )
			err(2, "etc/passwd");
		textf("User '%s' added to /etc/passwd\n", "root");
		if ( !install_configurationf("etc/group", "a",
			"root::0:root\n"
			"include /etc/default/group.d/*\n") )
			err(2, "etc/passwd");
		install_skel("/root", 0, 0);
		textf("Group '%s' added to /etc/group.\n", "root");
		free(hash);
	}
	else while ( true )
	{
		char first[128];
		char second[128];
		password(first, sizeof(first), "Password for root account? (will not echo)");
		password(second, sizeof(second), "Password for root account? (again)");
		if ( strcmp(first, second) != 0 )
		{
			printf("Passwords do not match, try again.\n");
			continue;
		}
		explicit_bzero(second, sizeof(second));
		if ( !strcmp(first, "") )
		{
			char answer[32];
			prompt(answer, sizeof(answer), "empty_password",
			       "Empty password is stupid, are you sure? (yes/no)", "no");
			if ( strcasecmp(answer, "yes") != 0 )
				continue;
		}
		char hash[128];
		if ( crypt_newhash(first, "bcrypt,a", hash, sizeof(hash)) < 0 )
		{
			explicit_bzero(first, sizeof(first));
			warn("crypt_newhash");
			continue;
		}
		explicit_bzero(first, sizeof(first));
		if ( !install_configurationf("etc/passwd", "a",
			"root:%s:0:0:root:/root:sh\n"
			"include /etc/default/passwd.d/*\n", hash) )
			continue;
		textf("User '%s' added to /etc/passwd\n", "root");
		if ( !install_configurationf("etc/group", "a",
			"root::0:root\n"
			"include /etc/default/group.d/*\n") )
			continue;
		install_skel("/root", 0, 0);
		textf("Group '%s' added to /etc/group.\n", "root");
		break;
	}

	struct ssh_file
	{
		const char* key;
		const char* path;
		const char* pub;
	};
	const struct ssh_file ssh_files[] =
	{
		{"copy_ssh_authorized_keys_root", "/root/.ssh/authorized_keys", NULL},
		{"copy_ssh_config_root", "/root/.ssh/config", NULL},
		{"copy_ssh_id_rsa_root", "/root/.ssh/id_rsa", "/root/.ssh/id_rsa.pub"},
		{"copy_ssh_known_hosts_root", "/root/.ssh/known_hosts", NULL},
	};
	size_t ssh_files_count = sizeof(ssh_files) / sizeof(ssh_files[0]);
	bool any_ssh_keys = false;
	for ( size_t i = 0; i < ssh_files_count; i++ )
	{
		const struct ssh_file* file = &ssh_files[i];
		if ( access_or_die(file->path, F_OK) < 0 )
			continue;
		text("\n");
		textf("Found %s\n", file->path);
		if ( file->pub && !access_or_die(file->pub, F_OK) )
			textf("Found %s\n", file->pub);
		while ( true )
		{
			char question[256];
			snprintf(question, sizeof(question),
			         "Copy %s from installer environment? (yes/no)",
			         file->path);
			prompt(input, sizeof(input), file->key, question, "yes");
			if ( strcasecmp(input, "no") == 0 )
				break;
			if ( strcasecmp(input, "yes") != 0 )
				continue;
			mkdir_or_chmod_or_die("root/.ssh", 0700);
			textf("Copying %s -> %s\n", file->path, file->path + 1);
			execute((const char*[])
				{"cp", file->path, file->path+ 1, NULL }, "f");
			if ( file->pub )
			{
				textf("Copying %s -> %s\n", file->pub, file->pub + 1);
				execute((const char*[])
					{"cp", file->pub, file->pub + 1, NULL }, "f");
			}
			any_ssh_keys = true;
			break;
		}
	}
	text("\n");

	if ( mkdir("etc/init", 0755) < 0 )
	{
		if ( errno == EEXIST )
		{
			if ( chmod("etc/init", 0755) < 0 )
				warn("chmod: etc/init");
		}
		else
			warn("mkdir: etc/init");
	}
	install_configurationf("etc/init/default", "w",
		"require multi-user exit-code\n");

	text("Congratulations, the system is now functional! This is a good time "
	     "to do further customization of the system.\n\n");

	// TODO: autoconf users support.
	bool made_user = false;
	for ( uid_t uid = 1000; !has_autoconf; )
	{
		while ( passwd_has_uid("etc/passwd", uid) )
			uid++;
		gid_t gid = (gid_t) uid;
		static char userstr[256];
		const char* question = "Setup a user? (enter username or 'no')";
		if ( made_user )
			question = "Setup another user? (enter username or 'no')";
		prompt(userstr, sizeof(userstr), NULL, question, "no");
		if ( !strcmp(userstr, "no") )
			break;
		if ( !strcmp(userstr, "yes") )
			continue;
		const char* user = userstr;
		while ( user[0] == ' ')
			user++;
		if ( !is_valid_username(user) )
		{
			if ( non_interactive )
				errx(2, "fatal: invalid username: %s", user);
			text("Invalid username\n");
			continue;
		}
		if ( passwd_has_name("etc/passwd", user) )
		{
			textf("Account '%s' already exists.\n", user);
			continue;
		}
		static char name[256];
		while ( true )
		{
			prompt(name, sizeof(name), NULL, "Full name of user?", user);
			if ( !is_valid_user_full_name(name) )
			{
				if ( non_interactive )
					errx(2, "fatal: invalid full name: %s", name);
				text("Invalid full name\n");
				continue;
			}
			break;
		}
		char first[128];
		char second[128];
		while ( true )
		{
			password(first, sizeof(first), "Password for user? (will not echo)");
			password(second, sizeof(second), "Password for user? (again)");
			if ( strcmp(first, second) != 0 )
			{
				printf("Passwords do not match, try again.\n");
				continue;
			}
			explicit_bzero(second, sizeof(second));
			if ( !strcmp(first, "") )
			{
				char answer[32];
				prompt(answer, sizeof(answer), "empty_password",
				       "Empty password is stupid, are you sure? (yes/no)", "no");
				if ( strcasecmp(answer, "yes") != 0 )
					continue;
			}
			break;
		}
		char hash[128];
		if ( crypt_newhash(first, "bcrypt,a", hash, sizeof(hash)) < 0 )
		{
			explicit_bzero(first, sizeof(first));
			warn("crypt_newhash");
			continue;
		}
		explicit_bzero(first, sizeof(first));
		if ( !install_configurationf("etc/passwd", "a",
				"%s:%s:%" PRIuUID ":%" PRIuGID ":%s:/home/%s:sh\n",
				user, hash, uid, gid, name, user) )
			continue;
		if ( !install_configurationf("etc/group", "a",
				"%s::%" PRIuGID ":%s\n", user, gid, user) )
			continue;
		char* home;
		if ( asprintf(&home, "home/%s", user) < 0 )
		{
			warn("asprintf");
			continue;
		}
		if ( mkdir(home, 0700) < 0 && errno != EEXIST )
		{
			warn("mkdir: %s", home);
			free(home);
			continue;
		}
		chown(home, uid, gid);
		install_skel(home, uid, gid);
		free(home);
		textf("User '%s' added to /etc/passwd\n", user);
		textf("Group '%s' added to /etc/group.\n", user);
		text("\n");
		uid++;
		made_user = true;
	}
	// TODO: autoconf support.
	if ( !has_autoconf )
		text("\n");

	while ( true )
	{
		prompt(input, sizeof(input), "enable_gui",
			   "Enable graphical user interface?",
		       getenv("DISPLAY_SOCKET") ? "yes" : "no");
		if ( strcasecmp(input, "no") == 0 )
			break;
		if ( strcasecmp(input, "yes") != 0 )
			continue;
		if ( !install_configurationf("etc/session", "w",
		                             "#!sh\nexec display\n") ||
		     chmod("etc/session", 0755) < 0 )
		{
			warn("etc/session");
			continue;
		}
		text("Added 'exec display' to /etc/session\n");
		break;
	}
	text("\n");

	bool autoupgrade = false;
	while ( true )
	{
		text("The operating system can automatically download new versions "
		     "in the background and schedule an upgrade on the subsequent "
		     "boot. Alternatively you can invoke tix-upgrade(8) manually.\n\n");
		text("Privacy notice: If enabled, the operating system's website and "
		     "download mirror will be periodically checked for new releases, "
		     "which will be downloaded and installed.\n\n");
		prompt(input, sizeof(input), "enable_autoupgrade",
			   "Enable automatically upgrading the operating system?", "no");
		if ( strcasecmp(input, "no") == 0 )
			break;
		if ( strcasecmp(input, "yes") != 0 )
			continue;
		if ( !install_configurationf("etc/init/local", "a",
		                             "require autoupgrade optional\n") )
		{
			warn("etc/init/local");
			continue;
		}
		text("Added 'require autoupgrade optional' to /etc/init/local\n");
		autoupgrade = true;
		break;
	}
	text("\n");

	while ( autoupgrade )
	{
		text("The system can optionally automatically reboot whenever an "
		     "upgrade has been scheduled for the next boot. The reboot time "
		     "can be selected in the shutdown(8) format: Reboot immediately "
		     "(now), at a given time (HH:MM), or after a delay of n minutes "
		     "(+n). If yes, the default delay is one minute (+1). "
		     "Warning messages will be broadcast with wall(1).\n\n");
		prompt(input, sizeof(input), "enable_autoupgrade_reboot",
			   "Automatically reboot to upgrade? (no/yes/now/HH:MM/+minutes)",
		       "no");
		if ( strcasecmp(input, "no") == 0 )
			break;
		if ( !install_configurationf("tix/collection.conf", "a",
		                             "AUTOUPGRADE_REBOOT=true\n") )
		{
			warn("tix/collection.conf");
			continue;
		}
		text("Added 'AUTOUPGRADE_REBOOT=true' to /tix/collection.conf\n");
		if ( strcasecmp(input, "yes") == 0 )
			break;
		if ( !install_configurationf("tix/collection.conf", "a",
		                             "AUTOUPGRADE_REBOOT_TIME=%s\n", input) )
		{
			warn("tix/collection.conf");
			continue;
		}
		textf("Added 'AUTOUPGRADE_REBOOT_TIME=%s' to /tix/collection.conf\n",
		      input);
		break;
	}
	if ( autoupgrade )
		text("\n");

	bool is_stable = !strchr(VERSIONSTR, '-');
	bool ask_channel = autoconf_has("channel") || autoupgrade;
	while ( ask_channel )
	{
		char* release_url;
		execute((const char*[]) {"tix-vars", "tix/collection.conf",
			                     "RELEASE_URL", NULL}, "eo", &release_url);
		size_t release_url_len = strlen(release_url);
		if ( release_url_len && release_url[release_url_len - 1] == '\n' )
			release_url[release_url_len - 1] = '\0';
		regex_t re;
		if ( regcomp(&re,
		             "^.*/channel/+([^/]+)/+(([0-9]+\\.[0-9]+)([-.][^/]+)?)/*$",
		             REG_EXTENDED) )
			errx(2, "regcomp failed");
		regmatch_t match[5];
		int r = regexec(&re, release_url, 5, match, 0);
		regfree(&re);
		if ( r )
		{
			free(release_url);
			ask_channel = false;
			break;
		}
		char* channel = strndup(release_url + match[1].rm_so,
		                        match[1].rm_eo - match[1].rm_so);
		char* version = strndup(release_url + match[2].rm_so,
		                        match[2].rm_eo - match[2].rm_so);
		char* major_minor = strndup(release_url + match[3].rm_so,
		                            match[3].rm_eo - match[3].rm_so);
		if ( !channel || !version || !major_minor )
			err(2, "malloc");
		release_url[match[1].rm_so] = '\0';

		text("You can receive upgrades on different channels:\n\n");
		if ( is_stable )
		{
			text("stable: stable operating system releases\n");
			textf("%s: %s.x stable patch releases only\n",
			      major_minor, major_minor);
		}
		text("nightly: daily builds with the latest features\n");
		if ( strcmp(channel, "stable") != 0 &&
		     strcmp(channel, major_minor) != 0 &&
		     strcmp(channel, "nightly") != 0 )
			textf("%s: the default channel for this release\n", channel);
		text("\n");
		char question[] =
			"What upgrade channel to use? (stable/" VERSIONSTR "/nightly)";
		if ( is_stable )
		{
			textf("The 'stable' and 'nightly' channels will upgrade across "
			      "major operating system releases. The '%s' channel may be "
			      "ideal for production systems, as only bug fixes will be "
			      "delivered, and you can manually upgrade to new major "
			      "releases with incompatible changes.\n\n", major_minor);
			snprintf(question, sizeof(question),
			         "What upgrade channel to use? (stable/%s/nightly)",
			         major_minor);
		}
		else
			snprintf(question, sizeof(question),
			         "What upgrade channel to use? (nightly)");
		prompt(input, sizeof(input), "channel", question, channel);
		char* new_release_url;
		if ( asprintf(&new_release_url, "%s%s/%s", release_url, input,
		               version) < 0 )
			err(2, "malloc");
		free(channel);
		free(version);
		free(major_minor);
		free(release_url);
		execute((const char*[]) { "tix-create", "-C", ".", "--release-url",
		                           new_release_url, "--release-key=", NULL },
		        "e");
		textf("Updated /tix/collection.conf RELEASE_URL to %s\n",
		      new_release_url);
		free(new_release_url);
		break;
	}
	if ( ask_channel )
		text("\n");

	if ( !access_or_die("/tix/tixinfo/ntpd", F_OK) )
	{
		text("A Network Time Protocol client (ntpd) has been installed that "
		     "can automatically synchronize the current time with the internet."
		     "\n\n");
		text("Privacy notice: If enabled, the default configuration will "
		     "obtain time from pool.ntp.org and time.cloudflare.com; and "
		     "compare with HTTPS timestamps from quad9 and www.google.com. "
		     "You are encouraged to edit /etc/ntpd.conf per the ntpd.conf(5) "
		     "manual with your preferences.\n\n");
		bool copied = false;
		while ( true )
		{
			prompt(input, sizeof(input), "enable_ntpd",
			       "Automatically get time from the network? (yes/no/edit/man)",
			       copied ? "yes" : "no");
			if ( strcasecmp(input, "no") == 0 )
				break;
			if ( strcasecmp(input, "man") == 0 )
			{
				execute((const char*[]) {"man", "5", "ntpd.conf", NULL}, "fi");
				continue;
			}
			if ( strcasecmp(input, "edit") == 0 )
			{
				if ( !copied )
				{
					execute((const char*[]) {"cp", "etc/default/ntpd.conf",
						                     "etc/ntpd.conf", NULL}, "f");
					copied = true;
				}
				const char* editor =
					getenv("EDITOR") ? getenv("EDITOR") : "editor";
				execute((const char*[]) {editor, "etc/ntpd.conf", NULL}, "f");
				text("Created /etc/ntpd.conf from /etc/default/ntpd.conf\n");
				continue;
			}
			if ( strcasecmp(input, "yes") != 0 )
				continue;
			if ( !install_configurationf("etc/init/local", "a",
			                             "require ntpd optional\n") )
			{
				warn("etc/init/local");
				continue;
			}
			if ( !install_configurationf("etc/init/time", "a",
			                             "furthermore\n"
			                             "require ntpd optional\n") )
			{
				warn("etc/init/time");
				continue;
			}
			text("Added 'require ntpd optional' to /etc/init/local\n");
			text("Added 'require ntpd optional' to /etc/init/time\n");
			break;
		}
		text("\n");
	}

	struct sshd_key_file
	{
		const char* pri;
		const char* pub;
	};
	const struct sshd_key_file sshd_key_files[] =
	{
		{"/etc/ssh_host_ecdsa_key", "/etc/ssh_host_ecdsa_key.pub"},
		{"/etc/ssh_host_ed25519_key", "/etc/ssh_host_ed25519_key.pub"},
		{"/etc/ssh_host_rsa_key", "/etc/ssh_host_rsa_key.pub"},
	};
	size_t sshd_key_files_count
		= sizeof(sshd_key_files) / sizeof(sshd_key_files[0]);
	bool any_sshd_keys = false;
	for ( size_t i = 0; i < sshd_key_files_count; i++ )
	{
		if ( !access_or_die(sshd_key_files[i].pri, F_OK) )
		{
			textf("Found %s\n", sshd_key_files[i].pri);
			any_sshd_keys = true;
		}
	}

	bool enabled_sshd = false;
	if ( !access_or_die("/tix/tixinfo/ssh", F_OK) )
	{
		text("A ssh server has been installed. You have the option of starting "
		     "it on boot to allow remote login over a cryptographically secure "
		     "channel. Answer no if you don't know what ssh is.\n\n");
		bool might_want_sshd =
			any_ssh_keys ||
			any_sshd_keys ||
			!access_or_die("/etc/sshd_config", F_OK);
		while ( true )
		{
			prompt(input, sizeof(input), "enable_sshd",
			       "Enable ssh server? (yes/no)",
			       might_want_sshd ? "yes" : "no");
			if ( strcasecmp(input, "no") == 0 )
				break;
			if ( strcasecmp(input, "yes") != 0 )
				continue;
			if ( !install_configurationf("etc/init/local", "a",
			                             "require sshd optional\n") )
			{
				warn("etc/init/local");
				continue;
			}
			enabled_sshd = true;
			text("Added 'require sshd optional' to /etc/init/local\n");
			text("The ssh server will be started when the system boots.\n");
			break;
		}
		text("\n");
	}

	bool has_sshd_config = false;
	if ( !access_or_die("/etc/sshd_config", F_OK) )
	{
		while ( true )
		{
			prompt(input, sizeof(input), "copy_sshd_config",
			       "Copy /etc/sshd_config from installer environment? (yes/no)",
			       "yes");
			if ( strcasecmp(input, "no") == 0 )
				break;
			if ( strcasecmp(input, "yes") != 0 )
				continue;
			const char* file =  "/etc/sshd_config";
			textf("Copying %s -> %s\n", file, file + 1);
			execute((const char*[]) {"cp", file, file + 1}, "f");
			has_sshd_config = true;
			break;
		}
		text("\n");
	}

	if ( enabled_sshd && !has_sshd_config )
	{
		text("Password authentication has been disabled by default in sshd to "
		     "prevent remotely guessing insecure passwords. The recommended "
		     "approach is to put your public key in the installation .iso and "
		     "generate the sshd credentials ahead of time as documented in "
		     "release-iso-modification(7). However, you could enable password "
		     "authentication if you picked a very strong password.\n\n");
		bool enable_sshd_password = false;
		while ( true )
		{
			prompt(input, sizeof(input), "enable_sshd_password",
				   "Enable sshd password authentication? (yes/no)", "no");
			if ( strcasecmp(input, "no") == 0 )
				break;
			if ( strcasecmp(input, "yes") != 0 )
				continue;
			if ( !install_configurationf("etc/sshd_config", "a",
			                             "PasswordAuthentication yes\n") )
			{
				warn("etc/sshd_config");
				continue;
			}
			enable_sshd_password = true;
			text("Added 'PasswordAuthentication yes' to /etc/sshd_config\n");
			break;
		}
		while ( enable_sshd_password )
		{
			prompt(input, sizeof(input), "enable_sshd_root_password",
				   "Enable sshd password authentication for root? (yes/no)",
			       "no");
			if ( strcasecmp(input, "no") == 0 )
				break;
			if ( strcasecmp(input, "yes") != 0 )
				continue;
			if ( !install_configurationf("etc/sshd_config", "a",
			                             "PermitRootLogin yes\n") )
			{
				warn("etc/sshd_config");
				continue;
			}
			text("Added 'PermitRootLogin yes' to /etc/sshd_config\n");
			break;
		}
		text("\n");
	}

	if ( any_sshd_keys )
	{
		while ( true )
		{
			const char* question =
				"Copy sshd private keys from installer environment? (yes/no)";
			prompt(input, sizeof(input), "copy_sshd_private_keys",
			       question,
			       "yes");
			if ( strcasecmp(input, "no") == 0 )
				break;
			if ( strcasecmp(input, "yes") != 0 )
				continue;
			for ( size_t i = 0; i < sshd_key_files_count; i++ )
			{
				const struct sshd_key_file* file = &sshd_key_files[i];
				if ( access_or_die(file->pri, F_OK) < 0 )
					continue;
				textf("Copying %s -> %s\n", file->pri, file->pri + 1);
				execute((const char*[])
					{"cp", file->pri, file->pri + 1, NULL }, "f");
				textf("Copying %s -> %s\n", file->pub, file->pub + 1);
				execute((const char*[])
					{"cp", file->pub, file->pub + 1, NULL }, "f");
			}
			break;
		}
		text("\n");
	}

	if ( efi_bootloader[0] )
	{
		text("Note: You must set a boot variable in the EFI non-volatile "
		     "memory in order to boot " BRAND_DISTRIBUTION_NAME ", "
		     "so after finishing the installation, either:\n\n");
		textf("1) Reboot into the firmware settings and add a boot entry for "
		      "'%s'; or\n", efi_bootloader);
		textf("2) Use another operating system and run: "
		      "efibootmgr -c -w -L '%s' -l '%s\n\n",
		      grub_distributor, efi_bootloader);
		prompt(input, sizeof(input), "confirm_efibootmgr", "Acknowledge?",
		       "yes");
		text("\n");
	}

	if ( strcasecmp(accept_grub, "no") == 0 )
	{
		text("Note: You did not accept a bootloader and you must set up a "
		     "bootloader yourself in order to boot " BRAND_DISTRIBUTION_NAME
		     ". etc/default/grub.d/10_sortix.include is a GRUB configuration "
		     "fragment that boots the newly installed system. "
		     "You should add its contents to the /etc/grub.d/40_custom file of "
		     "an existing GRUB installation and then run update-grub. "
		     "Enter ! now to escape to a shell, so you can copy its "
		     "contents.\n\n");
		prompt(input, sizeof(input), "confirm_grub_include", "Acknowledge?",
		       "yes");
	}

	text("It's time to boot into the newly installed system.\n\n");

	text("Upon boot, you'll be greeted with a login screen. Enter your "
	     "credentials to get a command line. Login as user 'poweroff' as "
	     "described in login(8) to power off the machine or run poweroff(8). "
	     "After logging in, type 'man user-guide' to view the introductory "
	     "documentation.\n");
	text("\n");

	while ( true )
	{
		struct statvfs stvfs;
		bool is_live = !access("/etc/fstab", F_OK) &&
		               statvfs("/", &stvfs) && !(stvfs.f_flag & ST_RDONLY);
		const char* question =
			is_live ? "What now? (exit/poweroff/reboot/halt/boot/chroot)" :
			          "What now? (exit/poweroff/reboot/halt/chroot)";
		const char* answer = is_live ? "boot" : "reboot";
		prompt(input, sizeof(input), "finally", question, answer);
		if ( !strcasecmp(input, "exit") )
			exit(0);
		else if ( !strcasecmp(input, "poweroff") )
			exit_gui(0);
		else if ( !strcasecmp(input, "reboot") )
			exit_gui(1);
		else if ( !strcasecmp(input, "halt") )
			exit_gui(2);
		else if ( !strcasecmp(input, "boot") )
		{
			if ( !is_live )
			{
				printf("Only a live environment can reinit installations.\n");
				continue;
			}
			execute((const char*[]) {"mkdir", "-p", "/etc/init", NULL }, "ef");
			execute((const char*[]) {"cp", "etc/fstab", "/etc/fstab", NULL },
			        "ef");
			execute((const char*[]) {"sh", "-c",
			                         "echo 'require chain exit-code' > "
			                         "/etc/init/default", NULL },
			        "ef");
			exit_gui(3);
		}
		else if ( !strcasecmp(input, "chroot") )
		{
			unmount_all_but_root();
			unsetenv("SYSINSTALL_TARGET");
			unsetenv("SHLVL");
			exit(execute((const char*[]) { "chroot", "-dI", fs,
			                               "/sbin/init", NULL }, "f"));
		}
	}
}
