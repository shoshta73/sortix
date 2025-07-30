/*
 * Copyright (c) 2013, 2014, 2016 Jonas 'Sortie' Termansen.
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
 * unistd/ttyname.c
 * Returns the pathname of a terminal.
 */

#include <sys/ioctl.h>

#include <limits.h>
#include <unistd.h>

// TODO: After releasing Sortix 1.1, remove this bootstrap compatibility.
#if !defined(TTY_NAME_MAX)
#include <sortix/limits.h>
#endif

char* ttyname(int fd)
{
	static char name[TTY_NAME_MAX+1];
	name[0] = '/';
	name[1] = 'd';
	name[2] = 'e';
	name[3] = 'v';
	name[4] = '/';
	if ( ioctl(fd, TIOCGNAME, name + 5) < 0 )
		return NULL;
	return name;
}
