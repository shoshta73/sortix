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
 * shutdown.c
 * Shut down the computer.
 */

#include <sys/types.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <timespec.h>
#include <unistd.h>

enum action
{
	POWEROFF,
	REBOOT,
	HALT,
	REINIT,
	ACTION_MAX
};

static const int signals[ACTION_MAX] =
{
	SIGTERM,
	SIGINT,
	SIGQUIT,
	SIGHUP,
};

static const char* const actions[ACTION_MAX] =
{
	"powering off",
	"rebooting",
	"halting",
	"reinitializing",
};

static const char* const nologin_path = "/var/run/nologin";

static void on_signal(int signum)
{
	unlink(nologin_path);
	signal(signum, SIG_DFL);
	raise(signum);
}

static void nologin(const char* broadcast)
{
	signal(SIGINT, on_signal);
	signal(SIGQUIT, on_signal);
	signal(SIGTERM, on_signal);
	FILE* fp = fopen(nologin_path, "w");
	if ( !fp )
		warn("%s", nologin_path);
	else
	{
		const char* end = "\n\nUnprivileged logins are no longer permitted.\n";
		if ( fputs(broadcast, fp) == EOF ||
		     fputs(end, fp) == EOF ||
		     fflush(fp) == EOF )
			warn("%s", nologin_path);
		fclose(fp);
	}
}

static struct timespec parse_time(const char* when,
                                  clockid_t* clockid,
                                  struct timespec* now)
{
	char* end;
	if ( !strcmp(when, "now") )
		when = "+0";
	if ( when[0] == '+' )
	{
		*clockid = CLOCK_MONOTONIC;
		clock_gettime(*clockid, now);
		errno = 0;
		long long value = strtoll(when, &end, 10);
		if ( errno )
			errx(1, "invalid time: %s", when);
		time_t multiplier;
		if ( !strcmp(end, "s") || !strcmp(end, "sec") )
			multiplier = 1;
		else if ( !strcmp(end, "") || !strcmp(end, "m") || !strcmp(end, "min") )
			multiplier = 60;
		else if ( !strcmp(end, "h") || !strcmp(end, "hour") )
			multiplier = 3600;
		else
			errx(1, "invalid time: %s", when);
		return timespec_make(now->tv_sec + value * multiplier, now->tv_nsec);
	}
	*clockid = CLOCK_REALTIME;
	clock_gettime(*clockid, now);
	struct tm tm = { .tm_sec = 0 };
	if ( (end = strptime(when, "%H:%M:%S", &tm)) ||
	     (end = strptime(when, "%H:%M", &tm)) )
	{
		if ( *end )
			errx(1, "invalid time: %s", when);
		struct tm localtm;
		localtime_r(&now->tv_sec, &localtm);
		if ( tm.tm_hour < localtm.tm_hour ||
		     (tm.tm_hour == localtm.tm_hour &&
		      (tm.tm_min < localtm.tm_min ||
		       (tm.tm_min == localtm.tm_min &&
		       tm.tm_sec < localtm.tm_sec))) )
		{
			localtm.tm_mday++;
			mktime(&localtm);
		}
		localtm.tm_sec = tm.tm_sec;
		localtm.tm_min = tm.tm_min;
		localtm.tm_hour = tm.tm_hour;
		return timespec_make(mktime(&localtm), 0);
	}
	if ( (end = strptime(when, "%Y-%m-%d %H:%M:%S", &tm)) )
	{
		if ( *end )
			errx(1, "invalid time: %s", when);
		return timespec_make(mktime(&tm), 0);
	}
	errx(1, "invalid time: %s", when);
}

static void wall(const char* message)
{
	pid_t pid = fork();
	if ( pid < 0 )
		return;
	if ( !pid )
	{
		sigset_t set;
		sigemptyset(&set);
		sigaddset(&set, SIGALRM);
		sigprocmask(SIG_UNBLOCK, &set, NULL);
		signal(SIGALRM, SIG_DFL);
		alarm(5);
		execlp("wall", "wall", "-m", message, NULL);
		_exit(127);
	}
	int status;
	waitpid(pid, &status, 0);
}

static void alert(enum action action,
                  const struct timespec left,
                  const struct timespec* at,
                  const char* message)
{
	struct timespec remaining = left;
	if ( 500000000 <= left.tv_nsec )
		remaining.tv_sec++, remaining.tv_nsec = 0;
	else
		remaining.tv_nsec = 0;
	char sep = message ? '\n' : '\0';
	char* broadcast;
	time_t value = remaining.tv_sec;
	const char* unit = "second";
	if ( 24 * 60 * 60 <= value )
		value /= 24 * 60 * 60, unit = "day";
	else if ( 60 * 60 <= value )
		value /= 60 * 60, unit = "hour";
	else if ( 60 <= value )
		value /= 60, unit = "minute";
	char when[128];
	if ( value <= 0 )
		snprintf(when, sizeof(when), "NOW!");
	else
	{
		struct tm tm;
		localtime_r(&at->tv_sec, &tm);
		char date[64];
		strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S %Z", &tm);
		snprintf(when, sizeof(when), "in %lli %s%s at %s%c", (long long) value,
		         unit, value != 1 ? "s" : "", date,
		         remaining.tv_sec <= 5 * 60 ? '!' : '.');
	}
	if ( 0 <= asprintf(&broadcast, "The system is %s %s%c%c%s",
	                   actions[action], when, sep, sep, message) )
	{
		wall(broadcast);
		if ( remaining.tv_sec <= 5 * 60 )
			nologin(broadcast);
		free(broadcast);
	}
}

static time_t get_interval(time_t left)
{
	if ( left > 24 * 60 * 60 )
		return 24 * 60 * 60;
	else if ( left > 4 * 60 * 60 )
		return 4 * 60 * 60;
	else if ( left > 2 * 60 * 60 )
		return 2 * 60 * 60;
	else if ( left > 60 * 60 )
		return 60 * 60;
	else if ( left > 30 * 60 )
		return 30 * 60;
	else if ( left > 15 * 60 )
		return 15 * 60;
	else if ( left > 5 * 60 )
		return 5 * 60;
	else
		return 1 * 60;
}

int main(int argc, char* argv[])
{
	tzset();

	enum action action = POWEROFF;
	bool dry = false;
	bool detach = true;

	int opt;
	while ( (opt = getopt(argc, argv, "Dhikpr")) != -1 )
	{
		switch ( opt )
		{
		case 'D': detach = false; break;
		case 'h': action = HALT; break;
		case 'i': action = REINIT; break;
		case 'k': dry = true; break;
		case 'p': action = POWEROFF; break;
		case 'r': action = REBOOT; break;
		default: return 1;
		}
	}

	const char* when = "now";
	if ( 0 < argc - optind )
		when = argv[optind + 0];
	char* message = argv[optind + 1];
	if ( 2 < argc - optind )
	{
		size_t message_size;
		FILE* fp = open_memstream(&message, &message_size);
		if ( !fp )
			err(1, "malloc");
		for ( int i = optind + 1; i < argc; i++ )
		{
			if ( i != optind + 1 )
				fputs(" ", fp);
			fputs(argv[i], fp);
		}
		if ( ferror(fp) || fflush(fp) == EOF )
			err(1, "malloc");
		fclose(fp);
	}

	struct timespec now;
	clockid_t clock;
	struct timespec at = parse_time(when, &clock, &now);
	struct timespec at_realtime = at;
	if ( clock != CLOCK_REALTIME )
	{
		clock_gettime(CLOCK_REALTIME, &at_realtime);
		at_realtime = timespec_add(at_realtime, timespec_sub(at, now));
	}

	if ( detach )
	{
		pid_t child = fork();
		if ( child < 0 )
			err(1, "fork");
		else if ( child )
			return 0;
		setpgid(0, 0);
		// TODO: /dev/null stdin, stdout, stderr
	}

	while ( true )
	{
		struct timespec left = timespec_sub(at, now);
		alert(action, left, &at_realtime, message);
		if ( timespec_le(left, timespec_nul()) )
			break;
		time_t interval = get_interval(left.tv_sec);
		struct timespec next_remaining;
		next_remaining.tv_sec = ((left.tv_sec - 1) / interval) * interval;
		next_remaining.tv_nsec = 0;
		struct timespec next = timespec_sub(at, next_remaining);
		clock_nanosleep(clock, TIMER_ABSTIME, &next, NULL);
		clock_gettime(clock, &now);
	}

	if ( dry )
		return 0;

	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGQUIT);
	sigaddset(&set, SIGTERM);
	sigprocmask(SIG_BLOCK, &set, NULL);

	pid_t init_pid = getinit(0);
	if ( kill(init_pid, signals[action]) < 0 )
		err(1, "kill: %" PRIdPID, init_pid);

	return 0;
}
