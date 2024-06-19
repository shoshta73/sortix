/*
 * Copyright (c) 2012, 2014, 2021, 2022 Jonas 'Sortie' Termansen.
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
 * kthread.cpp
 * Utility and synchronization mechanisms for kernel threads.
 */

#include <limits.h>

#include <sortix/signal.h>

#include <sortix/kernel/kernel.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/process.h>
#include <sortix/kernel/scheduler.h>
#include <sortix/kernel/signal.h>
#include <sortix/kernel/thread.h>
#include <sortix/kernel/worker.h>

#include "uart.h"

namespace Sortix {

static kthread_mutex_t kutex_lock = KTHREAD_MUTEX_INITIALIZER;
static Thread* kutex_first_waiting;
static Thread* kutex_last_waiting;

void kthread_yield()
{
	Thread* thread = CurrentThread();
	thread->yield_operation = YIELD_OPERATION_NONE;
#if defined(__i386__) || defined(__x86_64__)
	asm volatile ("int $129");
#else
#error "kthread_yield needs to be implemented"
#endif
}

void kthread_wait_futex()
{
	Thread* thread = CurrentThread();
	thread->yield_operation = YIELD_OPERATION_WAIT_FUTEX;
#if defined(__i386__) || defined(__x86_64__)
	asm volatile ("int $129");
#else
#error "kthread_wait_futex needs to be implemented"
#endif
}

void kthread_wait_futex_signal()
{
	Thread* thread = CurrentThread();
	thread->yield_operation = YIELD_OPERATION_WAIT_FUTEX_SIGNAL;
#if defined(__i386__) || defined(__x86_64__)
	asm volatile ("int $129");
#else
#error "kthread_wait_futex needs to be implemented"
#endif
}

static void kthread_wait_kutex()
{
	Thread* thread = CurrentThread();
	thread->yield_operation = YIELD_OPERATION_WAIT_KUTEX;
#if defined(__i386__) || defined(__x86_64__)
	asm volatile ("int $129");
#else
#error "kthread_wait_kutex needs to be implemented"
#endif
}

static void kthread_wait_kutex_signal()
{
	Thread* thread = CurrentThread();
	thread->yield_operation = YIELD_OPERATION_WAIT_KUTEX_SIGNAL;
#if defined(__i386__) || defined(__x86_64__)
	asm volatile ("int $129");
#else
#error "kthread_wait_kutex needs to be implemented"
#endif
}

void kthread_wake_futex(Thread* thread)
{
	Scheduler::SetThreadState(thread, ThreadState::RUNNABLE, true);
}

static const int UNLOCKED = 0;
static const int LOCKED = 1;
static const int CONTENDED = 2;

void kthread_spinlock_lock(kthread_mutex_t* mutex)
{
	while ( true )
	{
		int state = UNLOCKED;
		int desired = LOCKED;
		if ( __atomic_compare_exchange_n(mutex, &state, desired, false,
	                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) )
			break;
	}
}

void kthread_spinlock_unlock(kthread_mutex_t* mutex)
{
	__atomic_store_n(mutex, UNLOCKED, __ATOMIC_SEQ_CST);
}

static bool kutex_wait(int* address, int value, bool signal)
{
	// TODO: Use a per-mutex wait queue instead.
	Thread* thread = CurrentThread();
	bool was_enabled = Interrupt::SetEnabled(false);
	kthread_spinlock_lock(&kutex_lock);
	thread->kutex_address = (uintptr_t) address;
	thread->kutex_woken = false;
	thread->kutex_prev_waiting = kutex_last_waiting;
	thread->kutex_next_waiting = NULL;
	(kutex_last_waiting ?
	 kutex_last_waiting->kutex_next_waiting :
	 kutex_first_waiting) = thread;
	kutex_last_waiting = thread;
	kthread_spinlock_unlock(&kutex_lock);
	Interrupt::SetEnabled(was_enabled);
	thread->timer_woken = false;
	if ( __atomic_load_n(address, __ATOMIC_SEQ_CST) == value )
	{
		if ( signal )
			kthread_wait_kutex_signal();
		else
			kthread_wait_kutex();
	}
	Interrupt::SetEnabled(false);
	kthread_spinlock_lock(&kutex_lock);
	bool result = thread->kutex_woken || !signal || !Signal::IsPending();
	thread->kutex_address = 0;
	thread->kutex_woken = false;
	(thread->kutex_prev_waiting ?
	 thread->kutex_prev_waiting->kutex_next_waiting :
	 kutex_first_waiting) = thread->kutex_next_waiting;
	(thread->kutex_next_waiting ?
	 thread->kutex_next_waiting->kutex_prev_waiting :
	 kutex_last_waiting) = thread->kutex_prev_waiting;
	thread->kutex_prev_waiting = NULL;
	thread->kutex_next_waiting = NULL;
	kthread_spinlock_unlock(&kutex_lock);
	Interrupt::SetEnabled(was_enabled);
	return result;
}

static void kutex_wake(int* address, int count)
{
	bool was_enabled = Interrupt::SetEnabled(false);
	kthread_spinlock_lock(&kutex_lock);
	for ( Thread* waiter = kutex_first_waiting;
	      0 < count && waiter;
	      waiter = waiter->kutex_next_waiting )
	{
		if ( waiter->kutex_address == (uintptr_t) address )
		{
			waiter->kutex_woken = true;
			kthread_wake_futex(waiter);
			if ( count != INT_MAX )
				count--;
		}
	}
	kthread_spinlock_unlock(&kutex_lock);
	Interrupt::SetEnabled(was_enabled);
}

bool kthread_mutex_trylock(kthread_mutex_t* mutex)
{
	int state = UNLOCKED;
	if ( !__atomic_compare_exchange_n(mutex, &state, LOCKED, false,
	                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) )
		return false;
	return true;
}

void kthread_mutex_lock(kthread_mutex_t* mutex)
{
	int state = UNLOCKED;
	int desired = LOCKED;
	while ( !__atomic_compare_exchange_n(mutex, &state, desired, false,
	                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) )
	{
		if ( state == LOCKED &&
		     !__atomic_compare_exchange_n(mutex, &state, CONTENDED, false,
		                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) )
		{
			state = UNLOCKED;
			continue;
		}
		desired = CONTENDED;
		kutex_wait(mutex, CONTENDED, false);
		state = UNLOCKED;
	}
}

bool kthread_mutex_lock_signal(kthread_mutex_t* mutex)
{
	int state = UNLOCKED;
	int desired = LOCKED;
	while ( !__atomic_compare_exchange_n(mutex, &state, desired, false,
	                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) )
	{
		if ( state == LOCKED &&
		     !__atomic_compare_exchange_n(mutex, &state, CONTENDED, false,
		                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) )
		{
			state = UNLOCKED;
			continue;
		}
		desired = CONTENDED;
		if ( !kutex_wait(mutex, CONTENDED, true) )
			return false;
		state = UNLOCKED;
	}
	return true;
}

void kthread_mutex_unlock(kthread_mutex_t* mutex)
{
	// TODO: Multiple threads could have caused the contention and this wakes
	//       only one such thread, but then the mutex returns to normal and the
	//       subsequent unlocks doesn't wake the other contended threads. Work
	//       around this bug by waking all of them instead, but it's better to
	//       instead count the number of waiting threads.
	if ( __atomic_exchange_n(mutex, UNLOCKED, __ATOMIC_SEQ_CST) == CONTENDED )
		kutex_wake(mutex, INT_MAX);
}

// The kernel thread needs another stack to delete its own stack.
static void kthread_do_kill_thread(void* user)
{
	Thread* thread = (Thread*) user;
	while ( thread->state != ThreadState::DEAD )
		kthread_yield();
	delete thread;
}

void kthread_exit()
{
	Process* process = CurrentProcess();
	// Note: This requires all threads in this process to have been made by
	// only threads in this process, except the initial thread. Otherwise more
	// threads may appear, and we can't conclude whether this is the last thread
	// in the process to exit.
	kthread_mutex_lock(&process->thread_lock);
	bool is_last_to_exit = --process->threads_not_exiting_count == 0;
	kthread_mutex_unlock(&process->thread_lock);
	// All other threads in the process have committed to exiting, though they
	// might not have exited yet. However, we know they are only running the
	// below code that schedules thread termination. It's therefore safe to run
	// a final process termination step without interference.
	if ( is_last_to_exit )
		process->OnLastThreadExit();
	Worker::Schedule(kthread_do_kill_thread, CurrentThread());
#if defined(__i386__) || defined(__x86_64__)
	asm volatile ("int $132");
#else
#error "kthread_exit needs to be implemented"
#endif
	__builtin_unreachable();
}

struct kthread_cond_elem
{
	kthread_cond_elem_t* next;
	kthread_cond_elem_t* prev;
	int woken;
};

void kthread_cond_wait(kthread_cond_t* cond, kthread_mutex_t* mutex)
{
	kthread_cond_elem_t elem;
	elem.next = NULL;
	elem.prev = cond->last;
	elem.woken = 0;
	if ( cond->last )
		cond->last->next = &elem;
	if ( !cond->first )
		cond->first = &elem;
	cond->last = &elem;
	kthread_mutex_unlock(mutex);
	while ( !__atomic_load_n(&elem.woken, __ATOMIC_SEQ_CST) )
	        kutex_wait(&elem.woken, 0, false);
	kthread_mutex_lock(mutex);
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
}

bool kthread_cond_wait_signal(kthread_cond_t* cond, kthread_mutex_t* mutex)
{
	if ( Signal::IsPending() )
		return false;
	kthread_cond_elem_t elem;
	elem.next = NULL;
	elem.prev = cond->last;
	elem.woken = 0;
	if ( cond->last )
		cond->last->next = &elem;
	if ( !cond->first )
		cond->first = &elem;
	cond->last = &elem;
	kthread_mutex_unlock(mutex);
	bool result = true;
	while ( !__atomic_load_n(&elem.woken, __ATOMIC_SEQ_CST) )
	{
		if ( !kutex_wait(&elem.woken, 0, true) )
		{
			result = false;
			break;
		}
	}
	kthread_mutex_lock(mutex);
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
	return result;
}

void kthread_cond_signal(kthread_cond_t* cond)
{
	if ( cond->first )
	{
		struct kthread_cond_elem* elem = cond->first;
		if ( elem->next )
			elem->next->prev = elem->prev;
		else
			cond->last = elem->prev;
		cond->first = elem->next;
		elem->next = NULL;
		elem->prev = NULL;
		__atomic_store_n(&elem->woken, 1, __ATOMIC_SEQ_CST);
		kutex_wake(&elem->woken, 1);
	}
}

void kthread_cond_broadcast(kthread_cond_t* cond)
{
	while ( cond->first )
		kthread_cond_signal(cond);
}

} // namespace Sortix
