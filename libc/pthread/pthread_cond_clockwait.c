/*
 * Copyright (c) 2014, 2021, 2024 Jonas 'Sortie' Termansen.
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
 * pthread/pthread_cond_clockwait.c
 * Waits on a condition or until a timeout happens.
 */

#include <sys/futex.h>

#include <errno.h>
#include <pthread.h>

int pthread_cond_clockwait(pthread_cond_t* restrict cond,
                           pthread_mutex_t* restrict mutex,
                           clockid_t clock,
                           const struct timespec* restrict abstime)
{
	struct pthread_cond_elem elem;
	pthread_mutex_lock(&cond->lock);
	elem.next = NULL;
	elem.prev = cond->last;
	elem.woken = 0;
	if ( cond->last )
		cond->last->next = &elem;
	if ( !cond->first )
		cond->first = &elem;
	cond->last = &elem;
	pthread_mutex_unlock(&cond->lock);
	pthread_mutex_unlock(mutex);
	int op = FUTEX_WAIT | FUTEX_ABSOLUTE | FUTEX_CLOCK(clock);
	int result = 0;
	while ( !__atomic_load_n(&elem.woken, __ATOMIC_SEQ_CST) &&
	        futex(&elem.woken, op, 0, abstime) < 0 )
	{
		if ( errno == EINTR )
			continue;
		if ( errno != EAGAIN )
			result = errno;
		break;
	}
	pthread_mutex_lock(mutex);
	pthread_mutex_lock(&cond->lock);
	if ( !__atomic_load_n(&elem.woken, __ATOMIC_SEQ_CST) )
	{
		if ( elem.next )
			elem.next->prev = elem.prev;
		else
			cond->last = elem.prev;
		if ( elem.prev )
			elem.prev->next = elem.next;
		else
			cond->first = elem.next;
	}
	pthread_mutex_unlock(&cond->lock);
	return result;
}
