/*
 * Copyright (c) 2013, 2016, 2017, 2025 Jonas 'Sortie' Termansen.
 * Copyright (c) 2023 dzwdz.
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
 * sys/select/select.c
 * Waiting on multiple file descriptors.
 */

#include <errno.h>

#include <sys/select.h>

int select(int nfds, fd_set* restrict readfds, fd_set* restrict writefds,
           fd_set* restrict exceptfds, struct timeval* restrict timeout)
{
	struct timespec* timeout_tsp = NULL;
	struct timespec timeout_ts;
	if ( timeout )
	{
		if ( timeout->tv_sec < 0 )
			return errno = EINVAL, -1;
		if ( timeout->tv_usec < 0 || 1000000 <= timeout->tv_usec )
			return errno = EINVAL, -1;
		timeout_tsp = &timeout_ts;
		timeout_tsp->tv_sec = timeout->tv_sec;
		timeout_tsp->tv_nsec = (long) timeout->tv_usec * 1000L;
	}
	return pselect(nfds, readfds, writefds, exceptfds, timeout_tsp, NULL);
}
