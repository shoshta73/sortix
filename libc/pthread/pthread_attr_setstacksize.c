/*
 * Copyright (c) 2013, 2026 Jonas 'Sortie' Termansen.
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
 * pthread/pthread_attr_setstacksize.c
 * Sets the requested stack size in a thread attribute object.
 */

#include <errno.h>
#include <limits.h>
#include <pthread.h>

// TODO: After releasing Sortix 1.1, remove this bootstrap compatibility.
#ifdef __sortix__
#include <sortix/limits.h>
#endif

int pthread_attr_setstacksize(pthread_attr_t* attr, size_t stack_size)
{
	// Fail if the stack is too small.
	if ( stack_size < PTHREAD_STACK_MIN )
		return errno = EINVAL;
	attr->stack_size = stack_size;
	return 0;
}
