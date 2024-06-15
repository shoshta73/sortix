/*
 * Copyright (c) 2011, 2012, 2013, 2014, 2017, 2021 Jonas 'Sortie' Termansen.
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
 * sortix/kernel/scheduler.h
 * Decides the order to execute threads in and switching between them.
 */

#ifndef _INCLUDE_SORTIX_KERNEL_SCHEDULER_H
#define _INCLUDE_SORTIX_KERNEL_SCHEDULER_H

#include <sortix/kernel/decl.h>
#include <sortix/kernel/registers.h>

namespace Sortix {
class Process;
class Thread;
} // namespace Sortix

namespace Sortix {
enum ThreadState { NONE, RUNNABLE, FUTEX_WAITING, DEAD };
} // namespace Sortix

namespace Sortix {
namespace Scheduler {

void Switch(struct interrupt_context* intctx);
void SwitchTo(struct interrupt_context* intctx, Thread* new_thread);
void SetThreadState(Thread* thread, ThreadState state, bool wake_only = false);
void SetSignalPending(Thread* thread, unsigned long is_pending);
ThreadState GetThreadState(Thread* thread);
void SetIdleThread(Thread* thread);
Process* GetKernelProcess();
void InterruptYieldCPU(struct interrupt_context* intctx, void* user);
void ThreadExitCPU(struct interrupt_context* intctx, void* user);
void SaveInterruptedContext(const struct interrupt_context* intctx,
                            struct thread_registers* registers);
void LoadInterruptedContext(struct interrupt_context* intctx,
                            const struct thread_registers* registers);
void ScheduleTrueThread();

} // namespace Scheduler
} // namespace Sortix

#endif
