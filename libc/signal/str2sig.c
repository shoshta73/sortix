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
 * signal/str2sig.c
 * Lookup signal by name.
 */

#include <signal.h>
#include <stdlib.h>
#include <string.h>

int str2sig(const char* signame, int* signum)
{
	int ret;
	if ( !strncmp(signame, "RTMIN+", strlen("RTMIN+")) ||
	     !strncmp(signame, "RTMAX-", strlen("RTMAX-")) )
	{
		// TODO: Stricter check.
		int off = atoi(signame + 6);
		if ( off < 1 || SIGRTMAX - SIGRTMIN <= off )
			return -1;
		ret = signame[5] == '+' ? SIGRTMIN + off : SIGRTMAX - off;
	}
	else if ( !strcmp(signame, "HUP") ) ret = SIGHUP;
	else if ( !strcmp(signame, "INT") ) ret = SIGINT;
	else if ( !strcmp(signame, "QUIT") ) ret = SIGQUIT;
	else if ( !strcmp(signame, "ILL") ) ret = SIGILL;
	else if ( !strcmp(signame, "TRAP") ) ret = SIGTRAP;
	else if ( !strcmp(signame, "ABRT") ) ret = SIGABRT;
	else if ( !strcmp(signame, "BUS") ) ret = SIGBUS;
	else if ( !strcmp(signame, "FPE") ) ret = SIGFPE;
	else if ( !strcmp(signame, "KILL") ) ret = SIGKILL;
	else if ( !strcmp(signame, "USR1") ) ret = SIGUSR1;
	else if ( !strcmp(signame, "SEGV") ) ret = SIGSEGV;
	else if ( !strcmp(signame, "USR2") ) ret = SIGUSR2;
	else if ( !strcmp(signame, "PIPE") ) ret = SIGPIPE;
	else if ( !strcmp(signame, "ALRM") ) ret = SIGALRM;
	else if ( !strcmp(signame, "TERM") ) ret = SIGTERM;
	else if ( !strcmp(signame, "SYS") ) ret = SIGSYS;
	else if ( !strcmp(signame, "CHLD") ) ret = SIGCHLD;
	else if ( !strcmp(signame, "CONT") ) ret = SIGCONT;
	else if ( !strcmp(signame, "STOP") ) ret = SIGSTOP;
	else if ( !strcmp(signame, "TSTP") ) ret = SIGTSTP;
	else if ( !strcmp(signame, "TTIN") ) ret = SIGTTIN;
	else if ( !strcmp(signame, "TTOU") ) ret = SIGTTOU;
	else if ( !strcmp(signame, "URG") ) ret = SIGURG;
	else if ( !strcmp(signame, "XCPU") ) ret = SIGXCPU;
	else if ( !strcmp(signame, "XFSZ") ) ret = SIGXFSZ;
	else if ( !strcmp(signame, "VTALRM") ) ret = SIGVTALRM;
	else if ( !strcmp(signame, "PWR") ) ret = SIGPWR;
	else if ( !strcmp(signame, "WINCH") ) ret = SIGWINCH;
	else return -1;
	*signum = ret;
	return 0;
}
