/*
 * Copyright (c) 2013, 2015, 2023, 2024 Jonas 'Sortie' Termansen.
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
 * chroot.c
 * Runs a process with another root directory.
 */

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fsmarshall.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static char* mount_point_dev;

static void unmount_handler(int signum)
{
	(void) signum;
	if ( mount_point_dev )
	{
		unmount(mount_point_dev, 0);
		mount_point_dev = NULL;
	}
	raise(signum);
}

int main(int argc, char* argv[])
{
	bool devices = false;
	for ( int i = 1; i < argc; i++ )
	{
		const char* arg = argv[i];
		if ( arg[0] != '-' || !arg[1] )
			break; // Intentionally not continue.
		argv[i] = NULL;
		if ( !strcmp(arg, "--") )
			break;
		if ( arg[1] != '-' )
		{
			char c;
			while ( (c = *++arg) ) switch ( c )
			{
			case 'd': devices = true; break;
			default:
				errx(1, "unknown option -- '%c'", c);
			}
		}
		else if ( !strcmp(arg, "--devices") )
			devices = true;
		else
			errx(1, "unknown option: %s", arg);
	}

	compact_arguments(&argc, &argv);

	if ( argc < 2 )
		errx(1, "missing operand, expected new root directory");

	bool need_cleanup = devices;

	// TODO: Why do we even have signal handling instead of just blocking the
	// signals and waiting for the subprocess to react?

	if ( need_cleanup )
	{
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = unmount_handler;
		sa.sa_flags = SA_RESETHAND;
		sigaction(SIGHUP, &sa, NULL);
		sigaction(SIGINT, &sa, NULL);
		sigaction(SIGQUIT, &sa, NULL);
		sigaction(SIGTERM, &sa, NULL);
	}

	if ( devices )
	{
		if ( asprintf(&mount_point_dev, "%s/dev", argv[1]) < 0 )
			err(1, "malloc");

		// Create a device directory in the root filesystem.
		mkdir(mount_point_dev, 0755);

		// Mount the current device directory inside the new root filesystem.
		int old_dev_fd = open("/dev", O_DIRECTORY | O_RDONLY);
		int new_dev_fd = open(mount_point_dev, O_DIRECTORY | O_RDONLY);
		fsm_fsbind(old_dev_fd, new_dev_fd, 0);
		close(new_dev_fd);
		close(old_dev_fd);
	}

	sigset_t oldset, sigs;
	if ( need_cleanup )
	{
		sigemptyset(&sigs);
		sigaddset(&sigs, SIGHUP);
		sigaddset(&sigs, SIGINT);
		sigaddset(&sigs, SIGQUIT);
		sigaddset(&sigs, SIGTERM);
		sigprocmask(SIG_BLOCK, &sigs, &oldset);
	}

	pid_t child_pid = need_cleanup ? fork() : 0;
	if ( child_pid < 0 )
	{
		int errnum = errno;
		unmount(mount_point_dev, 0);
		mount_point_dev = NULL;
		sigprocmask(SIG_SETMASK, &oldset, NULL);
		errno = errnum;
		err(1, "fork");
	}

	if ( !child_pid )
	{
		if ( need_cleanup )
		{
			signal(SIGHUP, SIG_DFL);
			signal(SIGINT, SIG_DFL);
			signal(SIGQUIT, SIG_DFL);
			signal(SIGTERM, SIG_DFL);
			sigprocmask(SIG_SETMASK, &oldset, NULL);
		}

		if ( chroot(argv[1]) != 0 )
			err(1, "%s", argv[1]);

		if ( chdir("/.") != 0 )
			err(1, "chdir: %s/.", argv[1]);

		char* default_argv[] = { (char*) "sh", (char*) NULL };

		char** exec_argv = 3 <= argc ? argv + 2 : default_argv;
		execvp(exec_argv[0], exec_argv);

		warn("%s", exec_argv[0]);
		_exit(127);
	}

	sigprocmask(SIG_SETMASK, &oldset, NULL);
	int code;
	waitpid(child_pid, &code, 0);
	sigprocmask(SIG_BLOCK, &sigs, &oldset);
	if ( devices )
	{
		if ( unmount(mount_point_dev, 0) < 0 )
			warn("unmount: %s", mount_point_dev);
	}
	sigprocmask(SIG_SETMASK, &oldset, NULL);
	mount_point_dev = NULL;
	if ( WIFEXITED(code) )
		return WEXITSTATUS(code);
	raise(WTERMSIG(code));
	return 128 + WTERMSIG(code);
}
