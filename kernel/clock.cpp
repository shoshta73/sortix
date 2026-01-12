/*
 * Copyright (c) 2013, 2016, 2017, 2018, 2021, 2022 Jonas 'Sortie' Termansen.
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
 * clock.cpp
 * Clock and timer facility.
 */

#include <assert.h>
#include <timespec.h>

#include <sortix/kernel/clock.h>
#include <sortix/kernel/interrupt.h>
#include <sortix/kernel/kernel.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/signal.h>
#include <sortix/kernel/thread.h>
#include <sortix/kernel/timer.h>
#include <sortix/kernel/worker.h>

namespace Sortix {

static void Clock__InterruptWork(void* context)
{
	((Clock*) context)->InterruptWork();
}

Clock::Clock()
{
	delay_timer = NULL;
	absolute_timer = NULL;
	first_interrupt_timer = NULL;
	last_interrupt_timer = NULL;
	interrupt_work.handler = Clock__InterruptWork;
	interrupt_work.context = this;
	current_time = timespec_nul();
	current_advancement = timespec_nul();
	resolution = timespec_nul();
	clock_mutex = KTHREAD_MUTEX_INITIALIZER;
	clock_callable_from_interrupt = false;
	we_disabled_interrupts = false;
	interrupt_work_scheduled = false;
}

Clock::~Clock()
{
	// TODO: The best solution would probably be to cancel everything that is
	//       waiting on us, but that is a bit dangerous since things have to be
	//       notified carefully that they should not use stale pointers to this
	//       clock. This is a bunch of work and since the clock is being
	//       destroyed, you could argue that you shouldn't be using a clock
	//       whose lifetime you don't control. Therefore assume that all users
	//       of the clock has stopped using it.
	assert(!absolute_timer && !delay_timer);
}

// This clock and timer facility is designed to work even from interrupt
// handlers. For instance, this is needed by the uptime clock that is
// incremented every timer interrupt. If we don't need interrupt handler safety,
// we simply fall back on regular mutual exclusion.

void Clock::SetCallableFromInterrupts(bool callable_from_interrupts)
{
	clock_callable_from_interrupt = callable_from_interrupts;
}

void Clock::LockClock()
{
	if ( clock_callable_from_interrupt )
	{
		if ( Interrupt::IsEnabled() )
		{
			Interrupt::Disable();
			we_disabled_interrupts = true;
		}
		else
			we_disabled_interrupts = false;
	}
	else
		kthread_mutex_lock(&clock_mutex);
}

void Clock::UnlockClock()
{
	if ( clock_callable_from_interrupt )
	{
		if ( we_disabled_interrupts )
			Interrupt::Enable();
	}
	else
		kthread_mutex_unlock(&clock_mutex);
}

void Clock::Set(struct timespec* now, struct timespec* res)
{
	LockClock();

	if ( now )
		current_time = *now;
	if ( res )
		resolution = *res;

	TriggerAbsolute();

	UnlockClock();
}

void Clock::Get(struct timespec* now, struct timespec* res)
{
	LockClock();

	if ( now )
		*now = current_time;
	if ( res )
		*res = resolution;

	UnlockClock();
}

// We maintain two queues of timers; one for timers that sleep for a duration
// and one that that sleeps until a certain point in time. This lets us deal
// nicely with non-monotonic clocks and simplifies the code. The absolute timers
// queue is simply sorted after their wake-up time, while the delay timers queue
// is sorted after their delays, where each node stores the delay between it and
// its previous node (if any, otherwise just the actual time left of the timer).
// This data structure allows constant time detection of whether a timer should
// be fired and the double-linked queue allow constant-time cancellation - this
// is at the expense of linear time insertion, but it is kinda okay since timers
// that are soon will always be at the start (and hence quick to insert), while
// timers in the far future will be last and the calling thread probably
// wouldn't mind a little delay.

// TODO: If locking the clock means disabling interrupts, and a large numbers of
//       timers are attached to this clock, then inserting a timer becomes
//       expensive as the CPU locks up for a moment. Perhaps this is not as bad
//       as it theoretically could be?

void Clock::RegisterAbsolute(Timer* timer) // Lock acquired.
{
	assert(!(timer->flags & TIMER_ACTIVE));
	timer->flags |= TIMER_ACTIVE;

	Timer* before = NULL;
	for ( Timer* iter = absolute_timer; iter; iter = iter->next_timer )
	{
		if ( timespec_lt(timer->value.it_value, iter->value.it_value) )
			break;
		before = iter;
	}

	timer->prev_timer = before;
	timer->next_timer = before ? before->next_timer : absolute_timer;
	if ( timer->next_timer ) timer->next_timer->prev_timer = timer;
	(before ? before->next_timer : absolute_timer) = timer;
}

void Clock::RegisterDelay(Timer* timer) // Lock acquired.
{
	assert(!(timer->flags & TIMER_ACTIVE));
	timer->flags |= TIMER_ACTIVE;

	Timer* before = NULL;
	for ( Timer* iter = delay_timer; iter; iter = iter->next_timer )
	{
		if ( timespec_lt(timer->value.it_value, iter->value.it_value) )
			break;
		timer->value.it_value = timespec_sub(timer->value.it_value, iter->value.it_value);
		before = iter;
	}

	timer->prev_timer = before;
	timer->next_timer = before ? before->next_timer : delay_timer;
	if ( timer->next_timer ) timer->next_timer->prev_timer = timer;
	(before ? before->next_timer : delay_timer) = timer;

	if ( timer->next_timer )
		timer->next_timer->value.it_value =
			timespec_sub(timer->next_timer->value.it_value, timer->value.it_value);
}

void Clock::Register(Timer* timer)
{
	if ( timer->flags & TIMER_ABSOLUTE )
		RegisterAbsolute(timer);
	else
		RegisterDelay(timer);
}

void Clock::UnlinkAbsolute(Timer* timer) // Lock acquired.
{
	assert(timer->flags & TIMER_ACTIVE);
	(timer->prev_timer ? timer->prev_timer->next_timer : absolute_timer) = timer->next_timer;
	if ( timer->next_timer ) timer->next_timer->prev_timer = timer->prev_timer;
	timer->prev_timer = timer->next_timer = NULL;
	timer->flags &= ~TIMER_ACTIVE;
}

void Clock::UnlinkDelay(Timer* timer) // Lock acquired.
{
	assert(timer->flags & TIMER_ACTIVE);
	(timer->prev_timer ? timer->prev_timer->next_timer : delay_timer) = timer->next_timer;
	if ( timer->next_timer ) timer->next_timer->prev_timer = timer->prev_timer;
	if ( timer->next_timer ) timer->next_timer->value.it_value = timespec_add(timer->next_timer->value.it_value, timer->value.it_value);
	timer->prev_timer = timer->next_timer = NULL;
	timer->flags &= ~TIMER_ACTIVE;
}

void Clock::Unlink(Timer* timer) // Lock acquired.
{
	if ( timer->flags & TIMER_ACTIVE )
	{
		if ( timer->flags & TIMER_ABSOLUTE )
			UnlinkAbsolute(timer);
		else
			UnlinkDelay(timer);
	}
}

void Clock::Cancel(Timer* timer)
{
	LockClock();

	Unlink(timer);

	while ( timer->flags & TIMER_FIRING )
	{
		UnlockClock();

		// TODO: This busy-loop is rather inefficient. We could set up some
		//       condition variable and wait on it. However, if the lock is
		//       turning interrupts off, then there is no mutex we can use.
		kthread_yield();

		LockClock();
	}

	UnlockClock();
}

bool Clock::TryCancel(Timer* timer)
{
	LockClock();
	bool active = timer->flags & TIMER_ACTIVE;
	if ( active )
		Unlink(timer);
	UnlockClock();
	return active;
}

static void timer_wakeup(Clock* /*clock*/, Timer* /*timer*/, void* ctx)
{
	Thread* thread = (Thread*) ctx;
	thread->timer_woken = true;
	kthread_wake_futex(thread);
}

struct timespec Clock::SleepDelay(struct timespec duration)
{
	LockClock();
	struct timespec start_advancement = current_advancement;
	UnlockClock();
	Thread* thread = CurrentThread();
	thread->futex_woken = false;
	thread->timer_woken = false;
	Timer timer;
	timer.Attach(this);
	struct itimerspec timerspec;
	timerspec.it_value = duration;
	timerspec.it_interval.tv_sec = 0;
	timerspec.it_interval.tv_nsec = 0;
	int timer_flags = TIMER_FUNC_INTERRUPT_HANDLER;
	timer.Set(&timerspec, NULL, timer_flags, timer_wakeup, thread);
	kthread_wait_futex_signal();
	timer.Cancel();
	LockClock();
	struct timespec end_advancement = current_advancement;
	UnlockClock();
	struct timespec elapsed = timespec_sub(end_advancement, start_advancement);
	if ( timespec_lt(elapsed, duration) )
		return timespec_sub(duration, elapsed);
	return timespec_nul();
}

struct timespec Clock::SleepUntil(struct timespec expiration)
{
	Thread* thread = CurrentThread();
	thread->futex_woken = false;
	thread->timer_woken = false;
	Timer timer;
	timer.Attach(this);
	struct itimerspec timerspec;
	timerspec.it_value = expiration;
	timerspec.it_interval.tv_sec = 0;
	timerspec.it_interval.tv_nsec = 0;
	int timer_flags = TIMER_ABSOLUTE | TIMER_FUNC_INTERRUPT_HANDLER;
	timer.Set(&timerspec, NULL, timer_flags, timer_wakeup, thread);
	kthread_wait_futex_signal();
	timer.Cancel();
	LockClock();
	struct timespec now = current_time;
	UnlockClock();
	struct timespec remaining = timespec_sub(expiration, now);
	if ( timespec_lt(timespec_nul(), remaining) )
		return remaining;
	return timespec_nul();
}

void Clock::Advance(struct timespec duration)
{
	LockClock();

	current_time = timespec_add(current_time, duration);
	current_advancement = timespec_add(current_advancement, duration);
	TriggerDelay(duration);
	TriggerAbsolute();

	UnlockClock();
}

// Fire timers that wait for a certain amount of time.
void Clock::TriggerDelay(struct timespec unaccounted) // Lock acquired.
{
	while ( Timer* timer = delay_timer )
	{
		if ( timespec_lt(unaccounted, timer->value.it_value) )
		{
			timer->value.it_value = timespec_sub(timer->value.it_value, unaccounted);
			break;
		}
		unaccounted = timespec_sub(unaccounted, timer->value.it_value);
		timer->value.it_value = timespec_nul();
		if ( (delay_timer = delay_timer->next_timer) )
			delay_timer->prev_timer = NULL;
		FireTimer(timer);
	}
}

// Fire timers that wait until a certain point in time.
void Clock::TriggerAbsolute() // Lock acquired.
{
	while ( Timer* timer = absolute_timer )
	{
		if ( timespec_lt(current_time, timer->value.it_value) )
			break;
		if ( (absolute_timer = absolute_timer->next_timer) )
			absolute_timer->prev_timer = NULL;
		FireTimer(timer);
	}
}

static void Clock__DoFireTimer(Timer* timer)
{
	timer->callback(timer->clock, timer, timer->user);
}

static void Clock__FireTimer(void* timer_ptr)
{
	Timer* timer = (Timer*) timer_ptr;
	assert(timer->clock);

	// Combine all the additionally pending events into a single one and notify
	// the caller of all the events that he missed because we couldn't call him
	// fast enough.
	timer->clock->LockClock();
	timer->num_overrun_events = timer->num_firings_scheduled;
	timer->num_firings_scheduled = 0;
	bool may_deallocate = timer->flags & TIMER_FUNC_MAY_DEALLOCATE_TIMER;
	timer->clock->UnlockClock();

	Clock__DoFireTimer(timer);

	// The handler may have deallocated the storage for the timer, don't touch
	// it again.
	if ( may_deallocate )
		return;

	// If additional events happened during the time of the event handler, we'll
	// have to handle them because the firing bit is set. We'll schedule another
	// worker thread job and resume there, so this worker thread can continue to
	// do other important stuff.
	timer->clock->LockClock();
	if ( timer->num_firings_scheduled )
		Worker::Schedule(Clock__FireTimer, timer_ptr);
	// If this was the last event, we'll clear the firing bit and the advance
	// thread now has the responsibility of creating worker thread jobs.
	else
		timer->flags &= ~TIMER_FIRING;
	timer->clock->UnlockClock();
}

void Clock::InterruptWork()
{
	Interrupt::Disable();
	Timer* work = first_interrupt_timer;
	first_interrupt_timer = NULL;
	last_interrupt_timer = NULL;
	Interrupt::Enable();
	while ( work )
	{
		Timer* next_work = work->next_interrupt_timer;
		Clock__FireTimer(work);
		work = next_work;
	}
	Interrupt::Disable();
	if ( first_interrupt_timer )
		Interrupt::ScheduleWork(&interrupt_work);
	else
		interrupt_work_scheduled = false;
	Interrupt::Enable();
}

void Clock::FireTimer(Timer* timer)
{
	timer->flags &= ~TIMER_ACTIVE;
	bool may_deallocate = timer->flags & TIMER_FUNC_MAY_DEALLOCATE_TIMER;

	// If the CPU is currently interrupted, we call the timer callback directly
	// only if it is known to work when the interrupts are disabled on this CPU.
	// Otherwise, we forward the timer pointer to a special interrupt-safe
	// worker thread that'll run the callback normally.
	if ( !Interrupt::IsEnabled() )
	{
		if ( timer->flags & TIMER_FUNC_INTERRUPT_HANDLER )
			Clock__DoFireTimer(timer);
		else if ( timer->flags & TIMER_FIRING )
			timer->num_firings_scheduled++;
		else
		{
			if ( !may_deallocate )
				timer->flags |= TIMER_FIRING;
			(last_interrupt_timer ?
			 last_interrupt_timer->next_interrupt_timer :
			 first_interrupt_timer) = timer;
			timer->next_interrupt_timer = NULL;
			last_interrupt_timer = timer;
			if ( !interrupt_work_scheduled )
			{
				Interrupt::ScheduleWork(&interrupt_work);
				interrupt_work_scheduled = true;
			}
		}
	}

	// Normally, we will run the timer callback in a worker thread, but as an
	// optimization, if the callback is known to be short and simple and safely
	// handles this situation, we'll simply call it from the current thread.
	else
	{
		if ( timer->flags & TIMER_FUNC_ADVANCE_THREAD )
			Clock__DoFireTimer(timer);
		else if ( timer->flags & TIMER_FIRING )
			timer->num_firings_scheduled++;
		else
		{
			if ( !may_deallocate )
				timer->flags |= TIMER_FIRING;
			Worker::Schedule(Clock__FireTimer, timer);
		}
	}

	// Rearm the timer only if it is periodic.
	if ( may_deallocate ||
	     timespec_le(timer->value.it_interval, timespec_nul()) )
		return;

	// TODO: If the period is too short (such a single nanosecond) on a delay
	//       timer, then it will try to spend each nanosecond avanced carefully
	//       and reliably schedule a shitload of firings. Not only that, but it
	//       will also loop this function many million timers per tick!

	// TODO: Throtte the timer if firing while the callback is still running!
	// TODO: Doesn't reload properly for absolute timers!
	if ( timer->flags & TIMER_ABSOLUTE )
		timer->value.it_value = timespec_add(timer->value.it_value, timer->value.it_interval);
	else
		timer->value.it_value = timer->value.it_interval;
	Register(timer);
}

} // namespace Sortix
