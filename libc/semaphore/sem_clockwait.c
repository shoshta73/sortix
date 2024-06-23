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
 * semaphore/sem_clockwait.c
 * Lock a semaphore with a timeout.
 */

#include <sys/futex.h>

#include <errno.h>
#include <semaphore.h>
#include <stdbool.h>
#include <time.h>

int sem_clockwait(sem_t* restrict sem, clockid_t clock,
                  const struct timespec* restrict abstime)
{
	while ( true )
	{
		int old = __atomic_load_n(&sem->value, __ATOMIC_SEQ_CST);
		int new = old != -1 ? old - 1 : -1;
		bool waiting = new == -1;
		if ( waiting )
			__atomic_add_fetch(&sem->waiters, 1, __ATOMIC_SEQ_CST);
		if ( old != new &&
		     !__atomic_compare_exchange_n(&sem->value, &old, new, false,
		                                  __ATOMIC_SEQ_CST, __ATOMIC_RELAXED) )
		{
			if ( waiting )
				__atomic_sub_fetch(&sem->waiters, 1, __ATOMIC_SEQ_CST);
			continue;
		}
		if ( !waiting )
			return 0;
		int op = FUTEX_WAIT | FUTEX_ABSOLUTE | FUTEX_CLOCK(clock);
		int ret = futex(&sem->value, op, -1, abstime);
		__atomic_sub_fetch(&sem->waiters, 1, __ATOMIC_SEQ_CST);
		if ( ret < 0 && errno != EAGAIN )
			return -1;
	}
}
