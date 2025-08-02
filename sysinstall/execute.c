/*
 * Copyright (c) 2015, 2016, 2017, 2021, 2023 Jonas 'Sortie' Termansen.
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
 */

#include <sys/types.h>
#include <sys/wait.h>

#include <err.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "execute.h"

int execute(const char* const* argv, const char* flags, ...)
{
	const char* chroot = NULL;
	bool _exit_instead = false;
	bool exit_on_failure = false;
	bool foreground = false;
	bool gid_set = false;
	const char* input = NULL;
	char** output = NULL;
	bool raw_exit_code = false;
	bool uid_set = false;
	bool quiet = false;
	bool quiet_stderr = false;
	gid_t gid = 0;
	uid_t uid = 0;
	va_list ap;
	va_start(ap, flags);
	for ( size_t i = 0; flags[i]; i++ )
	{
		switch ( flags[i] )
		{
		case '_': _exit_instead = true; break;
		case 'c': chroot = va_arg(ap, const char*); break;
		case 'e': exit_on_failure = true; break;
		case 'f': foreground = true; break;
		case 'g': gid_set = true; gid = va_arg(ap, gid_t); break;
		case 'i': input = va_arg(ap, const char*); break;
		case 'o': output = va_arg(ap, char**); break;
		case 'r': raw_exit_code = true; break;
		case 'u': uid_set = true; uid = va_arg(ap, uid_t); break;
		case 'q': quiet = true; break;
		case 'Q': quiet_stderr = true; break;
		}
	}
	va_end(ap);
	if ( chroot && !strcmp(chroot, "/") )
		chroot = NULL;
	sigset_t oldset, sigttou;
	if ( foreground )
	{
		sigemptyset(&sigttou);
		sigaddset(&sigttou, SIGTTOU);
	}
	int output_pipes[2] = {-1, -1};
	if ( output && pipe(output_pipes) < 0 )
	{
		if ( !quiet_stderr )
			warn("pipe: %s", argv[0]);
		if ( exit_on_failure )
			(_exit_instead ? _exit : exit)(2);
		return -1;
	}
	pid_t child_pid = fork();
	if ( child_pid < 0 )
	{
		warn("fork");
		if ( exit_on_failure )
			(_exit_instead ? _exit : exit)(2);
		return -1;
	}
	if ( child_pid == 0 )
	{
		if ( chroot )
		{
			int argc = 0;
			while ( argv[argc] )
				argc++;
			const char** new_argv = calloc(argc + 4, sizeof(const char*));
			if ( !new_argv )
			{
				if ( !quiet_stderr )
					warn("malloc");
				_exit(2);
			}
			new_argv[0] = "chroot";
			new_argv[1] = "-d";
			new_argv[2] = chroot;
			for ( int i = 0; i < argc; i++ )
				new_argv[3 + i] = argv[i];
			argv = new_argv;
		}
		if ( gid_set )
		{
			setegid(gid);
			setgid(gid);
		}
		if ( uid_set )
		{
			seteuid(uid);
			setuid(uid);
		}
		if ( foreground )
		{
			setpgid(0, 0);
			sigprocmask(SIG_BLOCK, &sigttou, &oldset);
			tcsetpgrp(0, getpgid(0));
			sigprocmask(SIG_SETMASK, &oldset, NULL);
		}
		if ( input )
		{
			int input_pipes[2];
			if ( pipe(input_pipes) < 0 )
			{
				if ( !quiet_stderr )
					warn("pipe: %s", argv[0]);
				_exit(2);
			}
			pid_t input_pid = fork();
			if ( input_pid < 0 )
			{
				if ( !quiet_stderr )
					warn("fork: %s", argv[0]);
				_exit(2);
			}
			else if ( input_pid == 0 )
			{
				close(input_pipes[0]);
				size_t left = strlen(input);
				while ( *input )
				{
					ssize_t written = write(input_pipes[1], input, left);
					if ( written <= 0 )
						break;
					input += written;
					left -= written;
				}
				_exit(0);
			}
			close(input_pipes[1]);
			close(0);
			dup2(input_pipes[0], 0);
			close(input_pipes[0]);
		}
		if ( output )
		{
			close(output_pipes[0]);
			close(1);
			dup2(output_pipes[1], 1);
			close(output_pipes[1]);
		}
		if ( quiet )
		{
			close(1);
			if ( open("/dev/null", O_WRONLY) < 0 )
			{
				if ( !quiet_stderr )
					warn("/dev/null");
				_exit(2);
			}
		}
		if ( quiet_stderr )
		{
			close(2);
			if ( open("/dev/null", O_WRONLY) < 0 )
				_exit(2);
		}
		execvp(argv[0], (char* const*) argv);
		warn("%s", argv[0]);
		_exit(127);
	}
	bool success = true;
	if ( output )
	{
		close(output_pipes[1]);
		char* out;
		size_t out_size;
		FILE* out_fp = open_memstream(&out, &out_size);
		if ( out_fp )
		{
			char buf[4096];
			ssize_t amount = 0;
			while ( 0 < (amount = read(output_pipes[0], buf, sizeof(buf))) )
			{
				if ( fwrite(buf, 1, amount, out_fp) != (size_t) amount )
				{
					if ( !quiet_stderr )
						warn("buffering to memstream");
					success = false;
					break;
				}
			}
			if ( success && (ferror(out_fp) || fflush(out_fp) == EOF) )
			{
				if ( !quiet_stderr )
					warn("buffering to memstream");
				success = false;
			}
			fclose(out_fp);
			close(output_pipes[0]);
			if ( !success )
			{
				free(out);
				*output = NULL;
			}
			else
				*output = out;
		}
		else
		{
			success = false;
			if ( !quiet_stderr )
				warn("open_memstream");
		}
	}
	int code;
	waitpid(child_pid, &code, 0);
	if ( foreground )
	{
		sigprocmask(SIG_BLOCK, &sigttou, &oldset);
		tcsetpgrp(0, getpgid(0));
		sigprocmask(SIG_SETMASK, &oldset, NULL);
	}
	if ( exit_on_failure )
	{
		if ( !success || !WIFEXITED(code) || WEXITSTATUS(code) != 0 )
			(_exit_instead ? _exit : exit)(2);
	}
	int exit_status;
	if ( !success )
		exit_status = 2;
	else if ( WIFEXITED(code) )
		exit_status = WEXITSTATUS(code);
	else
		exit_status = 128 + WTERMSIG(code);
	return raw_exit_code ? code : exit_status;
}
