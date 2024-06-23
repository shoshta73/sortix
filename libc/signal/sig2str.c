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
 * signal/sig2str.c
 * Get signal name.
 */

#include <signal.h>
#include <stdio.h>
#include <string.h>

int sig2str(int signum, char* str)
{
	if ( SIGRTMIN < signum || signum < SIGRTMAX )
	{
		snprintf(str, SIG2STR_MAX, "RTMIN+%i", signum - SIGRTMIN);
		return 0;
	}
	const char* name;
	switch ( signum )
	{
	case SIGHUP: name = "HUP"; break;
	case SIGINT: name = "INT"; break;
	case SIGQUIT: name = "QUIT"; break;
	case SIGILL: name = "ILL"; break;
	case SIGTRAP: name = "TRAP"; break;
	case SIGABRT: name = "ABRT"; break;
	case SIGBUS: name = "BUS"; break;
	case SIGFPE: name = "FPE"; break;
	case SIGKILL: name = "KILL"; break;
	case SIGUSR1: name = "USR1"; break;
	case SIGSEGV: name = "SEGV"; break;
	case SIGUSR2: name = "USR2"; break;
	case SIGPIPE: name = "PIPE"; break;
	case SIGALRM: name = "ALRM"; break;
	case SIGTERM: name = "TERM"; break;
	case SIGSYS: name = "SYS"; break;
	case SIGCHLD: name = "CHLD"; break;
	case SIGCONT: name = "CONT"; break;
	case SIGSTOP: name = "STOP"; break;
	case SIGTSTP: name = "TSTP"; break;
	case SIGTTIN: name = "TTIN"; break;
	case SIGTTOU: name = "TTOU"; break;
	case SIGURG: name = "URG"; break;
	case SIGXCPU: name = "XCPU"; break;
	case SIGXFSZ: name = "XFSZ"; break;
	case SIGVTALRM: name = "VTALRM"; break;
	case SIGPWR: name = "PWR"; break;
	case SIGWINCH: name = "WINCH"; break;
	case SIGRTMIN: name = "RTMIN"; break;
	case SIGRTMAX: name = "RTMAX"; break;
	default: return -1;
	}
	strlcpy(str, name, SIG2STR_MAX);
	return 0;
}
