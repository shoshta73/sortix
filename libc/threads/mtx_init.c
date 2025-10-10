/*
 * Copyright (c) 2025 Jonas 'Sortie' Termansen.
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
 * threads/mtx_init.c
 * Initialize a mutex.
 */

#include <pthread.h>
#include <threads.h>

int mtx_init(mtx_t* mtx, int type)
{
	int result = thrd_error;
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	if ( !pthread_mutexattr_settype(&attr, type) &&
	     !pthread_mutex_init(mtx, &attr) )
	     result = thrd_success;
	pthread_mutexattr_destroy(&attr);
	return result;
}
