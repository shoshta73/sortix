/*
 * Copyright (c) 2014, 2025 Jonas 'Sortie' Termansen.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF7
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * timeout.c
 * Execute a command with a timeout.
 */

#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <timespec.h>
#include <unistd.h>

static pid_t signal_pid;

static void on_signal(int sig)
{
	// All signals are blocked here, so there's no worry about recursion.
	// Don't propagate SIGCHLD to avoid false wakeups in the children.
	if ( sig == SIGCHLD )
		return;
	if ( 0 < signal_pid)
		kill(signal_pid, sig);
	else if ( !signal_pid )
	{
		// Broadcast the signal to the process group but avoid receiving it
		// outselves again by temporarily ignoring and unblocking it.
		struct sigaction old, ign = { .sa_handler = SIG_IGN };
		sigaction(sig, &ign, &old);
		sigset_t sigset, old_sigset;
		sigemptyset(&sigset);
		sigaddset(&sigset, sig);
		sigprocmask(SIG_UNBLOCK, &sigset, &old_sigset);
		kill(0, sig);
		sigprocmask(SIG_SETMASK, &old_sigset, NULL);
		sigaction(sig, &old, NULL);
	}
}

static bool is_valid_interval(const char* string)
{
	size_t index = 0;
	while ( '0' <= string[index] && string[index] <= '9' )
		index++;
	if ( string[index] == '.' )
	{
		index++;
		while ( '0' <= string[index] && string[index] <= '9' )
			index++;
		if ( index == 1 && string[index-1] == '.' )
			return false;
	}
	if ( index == 0 )
		return false;
	return strcmp(string + index, "ns") == 0 ||
	       strcmp(string + index, "us") == 0 ||
	       strcmp(string + index, "ms") == 0 ||
	       strcmp(string + index, "") == 0 ||
	       strcmp(string + index, "s") == 0 ||
	       strcmp(string + index, "m") == 0 ||
	       strcmp(string + index, "h") == 0 ||
	       strcmp(string + index, "d") == 0;
}

static struct timespec parse_interval(const char* string)
{
	struct timespec ts;
	ts.tv_sec = 0;
	ts.tv_nsec = 0;
	size_t index = 0;

	while ( '0' <= string[index] && string[index] <= '9' )
	{
		char c = string[index++];
		// TODO: Prevent the multiplication from overflowing!
		ts.tv_sec = ts.tv_sec * 10;
		// TODO: Prevent the addition from overflowing!
		ts.tv_sec += c - '0';
	}

	if ( string[index] == '.' )
	{
		index++;

		// Pick 9 digits as nanoseconds.
		long contribution = 100L * 1000L * 1000L;
		while ( contribution && '0' <= string[index] && string[index] <= '9' )
		{
			char c = string[index++];
			ts.tv_nsec += (c - '0') * contribution;
			contribution /= 10;
		}

		// Remember whether there are more non-zero digits we didn't handle.
		bool any_non_zero_digits = false;
		for ( size_t i = index; string[i]; i++ )
		{
			if ( !('0' <= string[i] && string[i] <= '9') )
				break;
			if ( string[i] - '0' != 0 )
				any_non_zero_digits = true;
		}

		// If there are more digits we didn't handle, we'll round based on that.
		if ( '0' <= string[index] && string[index] <= '9' )
		{
			char c = string[index++];
			if ( 5 <= c - '0' )
				ts.tv_nsec++;
			if ( ts.tv_nsec == 1000000000L )
			{
				ts.tv_nsec = 0;
				// TODO: This could overflow, which case we shouldn't have added
				//       1 to tv_nsec regardless.
				ts.tv_sec++;
			}
		}

		// If all the digits we handled were zeroes, but there were some obscure
		// non-zero digits we didn't handle, we'll wait at least a nanosecond.
		if ( ts.tv_sec == 0 && ts.tv_nsec == 0 && any_non_zero_digits )
			ts.tv_nsec = 1;
	}

	if ( !strcmp(string + index, "ns") )
	{
		ts.tv_nsec = ts.tv_sec % 1000000000L;
		ts.tv_sec /= 1000000000L;
	}

	if ( !strcmp(string + index, "us") )
	{
		ts.tv_nsec /= 1000000L;
		ts.tv_nsec += (ts.tv_sec % 1000000L) * 1000L;
		ts.tv_sec /= 1000000L;
	}

	if ( !strcmp(string + index, "ms") )
	{
		ts.tv_nsec /= 1000L;
		ts.tv_nsec += (ts.tv_sec % 1000L) * 100000L;
		ts.tv_sec /= 1000L;
	}

	if ( !strcmp(string + index, "") || !strcmp(string + index, "s") )
	{
	}

	if ( !strcmp(string + index, "m") )
	{
		// TODO: Properly handle overflow here!
		ts.tv_nsec *= 60;
		ts.tv_sec *= 60;
		ts = timespec_canonalize(ts);
	}

	if ( !strcmp(string + index, "h") )
	{
		// TODO: Properly handle overflow here!
		ts.tv_nsec *= 3600;
		ts.tv_sec *= 3600;
		ts = timespec_canonalize(ts);
	}

	if ( !strcmp(string + index, "d") )
	{
		// TODO: Properly handle overflow here!
		ts.tv_nsec *= 86400;
		ts.tv_sec *= 86400;
		ts = timespec_canonalize(ts);
	}

	return ts;
}

int main(int argc, char* argv[])
{
	bool process_group = true;
	const char* kill_timeout_string = NULL;
	const char* signame = "TERM";
	bool preserve = false;

	int opt;
	while ( (opt = getopt(argc, argv, "fk:ps:")) != -1 )
	{
		switch ( opt )
		{
		case 'f': process_group = false; break;
		case 'k': kill_timeout_string = optarg; break;
		case 'p': preserve = true; break;
		case 's': signame = optarg; break;
		default: return 125;
		}
	}

	if ( argc - optind < 1 )
		errx(125, "expected timeout");
	if ( argc - optind < 2 )
		errx(125, "expected command");

	// Determine which signal to send.
	int signum;
	if ( str2sig(signame, &signum) < 0 )
		errx(125, "invalid signal name: %s", signame);

	// Parse the timeout.
	const char* timeout_string = argv[optind];
	if ( !is_valid_interval(timeout_string) )
		errx(125, "invalid timeout: %s", timeout_string);
	struct timespec timeout = parse_interval(timeout_string);

	// Parse the second SIGKILL timeout if requested.
	struct timespec kill_timeout = {0};
	if ( kill_timeout_string )
	{
		if ( !is_valid_interval(kill_timeout_string) )
			errx(125, "invalid timeout: %s", kill_timeout_string);
		kill_timeout = parse_interval(kill_timeout_string);
	}

	// Become a process group leader (if not already) in order to manage
	// descendant processes. However, this is as background process group.
	if ( process_group && getpgid(0) != getpid() && setpgid(0, 0) < 0 )
		err(125, "setpgid");

	// Block all signals, so all signals received after this point can be
	// propagated to the child process.
	sigset_t old_set;
	sigset_t all_set;
	sigfillset(&all_set);
	sigprocmask(SIG_BLOCK, &all_set, &old_set);

	// Calculate when the timeout will become reached.
	struct timespec begun;
	clock_gettime(CLOCK_MONOTONIC, &begun);
	struct timespec end = timespec_add(begun, timeout);

	// Execute the command in a child process.
	pid_t pid = fork();
	if ( pid < 0 )
		err(125, "fork");
	if ( !pid )
	{
		// Restore the signal disposition for the timeout signal so it will be
		// deadly, and carefully retain all other dispositions and the mask.
		signal(signum, SIG_DFL);
		sigdelset(&old_set, signum);
		sigprocmask(SIG_SETMASK, &old_set, NULL);
		execvp(argv[optind + 1], argv + optind + 1);
		err(errno == ENOENT ? 127 : 126, "%s", argv[optind + 1]);
	}

	// Propagate all signals to the children and ignore SIGTTIN and SIGTTOU.
	signal_pid = process_group ? 0 : pid;
	struct sigaction sa = { .sa_handler = on_signal };
	sigfillset(&sa.sa_mask);
	for ( int sig = 1; sig < NSIG; sig++ )
	{
		if ( sig == SIGTTIN || sig == SIGTTOU )
			signal(sig, SIG_IGN);
		else if ( sig != SIGSTOP && sig != SIGKILL )
			sigaction(sig, &sa, NULL);
	}

	// Main loop waiting for process termination, timeout, or signals.
	bool sent_signal = false;
	bool sent_kill = false;
	int status;
	struct timespec left = {0};
	// Zero timeout means infinite timeout.
	bool wait_forever = !timeout.tv_sec && !timeout.tv_nsec;
	while ( true )
	{
		// Send the signal upon timeout.
		if ( !wait_forever && !sent_kill )
		{
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			left = timespec_sub(end, now);
			if ( left.tv_sec < 0 || (!left.tv_sec && !left.tv_nsec) )
			{
				// Send the timeout signal. SIGKILL (unless -f) will kill the
				// entire process group, including ourselves.
				on_signal(signum);
				sent_signal = true;
				if ( signum == SIGKILL )
					sent_kill = true;
				// Do a second timeout with SIGKILL if requested.
				else if ( kill_timeout_string )
				{
					signum = SIGKILL;
					end = timespec_add(end, kill_timeout);
					continue;
				}
				else
					sent_kill = true;
			}
		}
		// Wait for the timeout or SIGCHLD while propagating signals.
		struct pollfd pfd;
		bool no_timeout = wait_forever || sent_kill;
		if ( ppoll(&pfd, 0, no_timeout ? NULL : &left, &old_set) < 0 )
		{
			if ( errno != EINTR )
				err(125, "ppoll");
		}
		// See if the child has finished execution, but don't block here, as we
		// are not able to propagate signals in a controlled fashion and doing
		// the timeout using waitpid. If a timeout has been reached, also catch
		// if the child is stopped, so we can continue it.
		int flags = WNOHANG | (sent_signal ? WUNTRACED : 0);
		pid_t child = waitpid(pid, &status, flags);
		if ( child < 0 )
			err(125, "waitpid");
		if ( 0 < child )
		{
			// If the child has stopped after the timeout, continue it so it can
			// received the timeout signal.
			if ( WIFSTOPPED(status) )
				on_signal(SIGCONT);
			break;
		}
	}

	// Exit specially if the timeout was reached. Note that if SIGKILL was sent
	// and -f was not used, then we'll die from SIGKILL earlier than here.
	if ( !preserve && sent_signal )
		return 124;

	// Exit in the exact same manner as the child without producing a core dump.
	exit_thread(status, EXIT_THREAD_PROCESS, NULL);
	__builtin_unreachable();
}
