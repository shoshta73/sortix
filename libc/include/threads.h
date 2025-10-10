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
 * threads.h
 * Thread API.
 */

#ifndef _INCLUDE_THREADS_H
#define _INCLUDE_THREADS_H

#include <sys/cdefs.h>

#include <__/pthread.h>

#include <time.h>

#define thread_local _Thread_local
#define ONCE_FLAG_INIT __PTHREAD_ONCE_INIT
#define TSS_DTOR_ITERATIONS 4

typedef __pthread_cond_t cnd_t;
typedef __pthread_mutex_t mtx_t;
typedef __pthread_once_t once_flag;
typedef __pthread_key_t tss_t;
typedef __pthread_t thrd_t;
typedef int (*thrd_start_t)(void*);
typedef void (*tss_dtor_t)(void*);

enum
{
	mtx_plain = __PTHREAD_MUTEX_NORMAL,
	mtx_recursive = __PTHREAD_MUTEX_RECURSIVE,
	mtx_timed = 0,
};

enum
{
	thrd_success,
	thrd_busy,
	thrd_error,
	thrd_nomem,
	thrd_timedout,
};

#ifdef __cplusplus
extern "C" {
#endif

void call_once(once_flag*, void (*)(void));
int cnd_broadcast(cnd_t*);
void cnd_destroy(cnd_t*);
int cnd_init(cnd_t*);
int cnd_signal(cnd_t*);
int cnd_timedwait(cnd_t* __restrict, mtx_t* __restrict,
                  const struct timespec* __restrict);
int cnd_wait(cnd_t*, mtx_t*);
void mtx_destroy(mtx_t*);
int mtx_init(mtx_t*, int);
int mtx_lock(mtx_t*);
int mtx_timedlock(mtx_t* __restrict, const struct timespec* __restrict);
int mtx_trylock(mtx_t*);
int mtx_unlock(mtx_t*);
int thrd_create(thrd_t*, thrd_start_t, void*);
thrd_t thrd_current(void);
int thrd_detach(thrd_t);
int thrd_equal(thrd_t, thrd_t);
void thrd_exit(int) __attribute__((__noreturn__));
int thrd_join(thrd_t, int*);
int thrd_sleep(const struct timespec*, struct timespec*);
void thrd_yield(void);
int tss_create(tss_t*, tss_dtor_t);
void tss_delete(tss_t);
void* tss_get(tss_t);
int tss_set(tss_t, void*);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
