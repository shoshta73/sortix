/*
 * Copyright (c) 2026 Jonas 'Sortie' Termansen.
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
 * pthread/pthread_spin_lock.c
 * Lock a spinlock.
 */

#include <pthread.h>
#include <sched.h>
#include <stdbool.h>

static const int UNLOCKED = 0;
static const int LOCKED = 1;

int pthread_spin_lock(pthread_spinlock_t* lock)
{
	int state = UNLOCKED;
	int desired = LOCKED;
	while ( !__atomic_compare_exchange_n(lock, &state, desired, false,
	                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) )
	{
		// SMP is not implemented yet, so stop wasting time and run another
		// thread that could potentially unlock us.
		sched_yield();
		state = UNLOCKED;
	}
	return 0;
}
