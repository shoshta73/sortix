/*
 * Copyright (c) 2021, 2022 Jonas 'Sortie' Termansen.
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
 * halt.c
 * Halts the computer.
 */

#include <sys/types.h>

#include <err.h>
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>

int main(int argc, char* argv[])
{
	int opt;
	while ( (opt = getopt(argc, argv, "")) != -1 )
	{
		switch ( opt )
		{
		default: return 1;
		}
	}

	if ( optind < argc )
		errx(1, "extra operand: %s", argv[optind]);

	pid_t init_pid = 1;
	// TODO: Use a more reliable getinit() approach that also works in sshd.
	if ( getenv("INIT_PID") )
		init_pid = atoll(getenv("INIT_PID"));

	if ( kill(init_pid, SIGQUIT) < 0 )
		err(1, "kill: %" PRIdPID, init_pid);

	return 0;
}
