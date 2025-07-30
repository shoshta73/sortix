/*
 * Copyright (c) 2024 Jonas 'Sortie' Termansen.
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
 * wall.c
 * Write message to all users.
 */

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// TODO: After releasing Sortix 1.1, remove this bootstrap compatibility.
#ifdef __sortix__
#include <sortix/limits.h>
#endif

static char* message;
static size_t message_size;

void on_alarm(int sig)
{
	(void) sig;
	_exit(0);
}

void* wall(void* arg)
{
	int fd = (int) (uintptr_t) arg;
	size_t done = 0;
	while ( done < message_size )
	{
		ssize_t amount = write(fd, message + done, message_size - done);
		if ( amount < 0 )
			break;
		done += amount;
	}
	return NULL;
}

void wall_dir(const char* path)
{
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	DIR* dir = opendir(path);
	if ( !dir )
	{
		warn("opendir: %s", path);
		return;
	}
	struct dirent* dirent;
	while ( (errno = 0, dirent = readdir(dir)) )
	{
		if ( !strcmp(dirent->d_name, ".") ||
		     !strcmp(dirent->d_name, "..") ||
		     !strcmp(dirent->d_name, "tty") )
			continue;
		int fd = openat(dirfd(dir), dirent->d_name, O_WRONLY);
		if ( fd < 0 )
			continue;
		if ( !isatty(fd) )
		{
			close(fd);
			continue;
		}
		pthread_t pth;
		int errnum = pthread_create(&pth, &attr, wall, (void*) (uintptr_t) fd);
		if ( errnum )
		{
			errno = errnum;
			warn("pthread_create: %s/%s", path, dirent->d_name);
		}
	}
	if ( errno )
		warn("readdir: %s", path);
	closedir(dir);
	pthread_attr_destroy(&attr);
}

int main(int argc, char* argv[])
{
	tzset();

	const char* msg = NULL;

	int opt;
	while ( (opt = getopt(argc, argv, "m:")) != -1 )
	{
		switch ( opt )
		{
		case 'm': msg = optarg; break;
		default: return 1;
		}
	}

	const char* stdin_path = "stdin";
	if ( 2 <= argc - optind )
		errx(1, "extra operand: %s", argv[optind + 1]);
	else if ( 1 <= argc - optind )
	{
		stdin_path = argv[optind];
		if ( !freopen(stdin_path, "r", stdin) )
			err(1, "%s", stdin_path);
	}

	char* login = getlogin();
	char hostname[HOST_NAME_MAX] = "?";
	gethostname(hostname, sizeof(hostname));
	char tty[TTY_NAME_MAX] = "";
	int tty_fd = open("/dev/tty", O_RDONLY);
	if ( 0 <= tty_fd )
	{
		ttyname_r(tty_fd, tty, sizeof(tty));
		close(tty_fd);
	}
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	struct tm tm;
	localtime_r(&now.tv_sec, &tm);
	char datetime[64];
	strftime(datetime, sizeof(datetime), "%Y-%m-%d %H:%M:%S %Z", &tm);

	FILE* fp = open_memstream(&message, &message_size);
	if ( !fp )
		err(1, "malloc");
	fprintf(fp, "\r\nBroadcast message from %s@%s%s%s (%s):\r\n\r\n",
	        login ? login : "?", hostname, tty[0] ? " on " : "", tty, datetime);
	bool cr = false;
	bool nl = false;
	size_t i = 0;
	while ( true )
	{
		int c = msg ? (unsigned char) msg[i++] : getchar();
		if ( !c || c == EOF )
		{
			if ( c == EOF && ferror(stdin) )
				err(1, "%s", stdin_path);
			break;
		}
		if ( c != '\t' && c != '\r' && c != '\n' && c < 32 )
			c = '?';
		if ( c == '\r' )
			cr = true, nl = false;
		else if ( c == '\n' )
		{
			if ( !cr )
				fputc('\r', fp);
			cr = true;
			nl = true;
		}
		else
			cr = false, nl = false;
		fputc(c, fp);
	}
	if ( !cr )
		fputc('\r', fp);
	if ( !nl )
		fputc('\n', fp);
	fputs("\r\n", fp);
	if ( feof(fp) || fflush(fp) == EOF )
		err(1, "malloc");
	fclose(fp);

	wall_dir("/dev");
	wall_dir("/dev/pts");

	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGALRM);
	sigprocmask(SIG_UNBLOCK, &set, NULL);
	signal(SIGALRM, SIG_DFL);
	alarm(5);

	pthread_exit(NULL);
}
