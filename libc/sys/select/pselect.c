/*
 * Copyright (c) 2013, 2016, 2017 Jonas 'Sortie' Termansen.
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
 * sys/select/pselect.c
 * Waiting on multiple file descriptors.
 */

#include <sys/select.h>

#include <errno.h>
#include <poll.h>
#include <string.h>

static const int READ_EVENTS = POLLIN | POLLRDNORM | POLLERR | POLLHUP;
static const int WRITE_EVENTS = POLLOUT | POLLWRNORM | POLLERR;
static const int EXCEPT_EVENTS = POLLERR | POLLHUP;

int pselect(int nfds, fd_set* restrict readfds, fd_set* restrict writefds,
           fd_set* restrict exceptfds, struct timeval* restrict timeout,
           const sigset_t* __restrict sigmask)
{
	if ( nfds < 0 || FD_SETSIZE < nfds )
		return errno = EINVAL, -1;
	if ( timeout )
	{
		if ( timeout->tv_sec < 0 )
			return errno = EINVAL, -1;
		if ( timeout->tv_usec < 0 || 1000000 <= timeout->tv_usec )
			return errno = EINVAL, -1;
	}
	struct pollfd fds[FD_SETSIZE];
	nfds_t fds_count = 0;
	for ( int i = 0; i < nfds; i++ )
	{
		fds[fds_count].fd = i;
		fds[fds_count].events = fds[fds_count].revents = 0;
		if ( readfds && FD_ISSET(i, readfds) )
			fds[fds_count].events |= READ_EVENTS;
		if ( writefds && FD_ISSET(i, writefds) )
			fds[fds_count].events |= WRITE_EVENTS;
		if ( exceptfds && FD_ISSET(i, exceptfds) )
			fds[fds_count].events |= EXCEPT_EVENTS;
		if ( fds[fds_count].events )
			fds_count++;
	}
	struct timespec* timeout_tsp = NULL;
	struct timespec timeout_ts;
	if ( timeout )
	{
		timeout_tsp = &timeout_ts;
		timeout_tsp->tv_sec = timeout->tv_sec;
		timeout_tsp->tv_nsec = (long) timeout->tv_usec * 1000L;
	}
	int num_occur = ppoll(fds, fds_count, timeout_tsp, sigmask);
	if ( num_occur < 0 )
		return -1;
	size_t fd_bytes = ((size_t) nfds + 7) / 8;
	if ( readfds )
		memset(readfds, 0, fd_bytes);
	if ( writefds )
		memset(writefds, 0, fd_bytes);
	if ( exceptfds )
		memset(exceptfds, 0, fd_bytes);
	int ret = 0;
	for ( nfds_t i = 0; i < fds_count; i++ )
	{
		int fd = fds[i].fd;
		int events = fds[i].revents;
		if ( events & READ_EVENTS && readfds )
		{
			FD_SET(fd, readfds);
			ret++;
		}
		if ( events & WRITE_EVENTS && writefds )
		{
			FD_SET(fd, writefds);
			ret++;
		}
		if ( events & EXCEPT_EVENTS && exceptfds )
		{
			FD_SET(fd, exceptfds);
			ret++;
		}
	}
	return ret;
}
