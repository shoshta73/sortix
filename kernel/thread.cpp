/*
 * Copyright (c) 2011-2016, 2018, 2021-2022, 2024-2025 Jonas 'Sortie' Termansen.
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
 * thread.cpp
 * Describes a thread belonging to a process.
 */

#include <sys/wait.h>

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <timespec.h>

#include <sortix/clock.h>
#include <sortix/exit.h>
#include <sortix/futex.h>
#include <sortix/mman.h>
#include <sortix/signal.h>

#include <sortix/kernel/copy.h>
#include <sortix/kernel/interrupt.h>
#include <sortix/kernel/ioctx.h>
#include <sortix/kernel/kernel.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/memorymanagement.h>
#include <sortix/kernel/process.h>
#include <sortix/kernel/scheduler.h>
#include <sortix/kernel/syscall.h>
#include <sortix/kernel/thread.h>
#include <sortix/kernel/time.h>

#if defined(__i386__) || defined(__x86_64__)
#include "x86-family/float.h"
#endif

namespace Sortix {

Thread::Thread()
{
	assert(!((uintptr_t) registers.fpuenv & 0xFUL));
	name = "";
	system_tid = (uintptr_t) this;
	yield_to_tid = 0;
	tid = 0;
	process = NULL;
	prev_sibling = NULL;
	next_sibling = NULL;
	scheduler_list_prev = NULL;
	scheduler_list_next = NULL;
	state = NONE;
	memset(&registers, 0, sizeof(registers));
	kernel_stack_pos = 0;
	kernel_stack_size = 0;
	signal_count = 0;
	signal_single_frame = 0;
	signal_canary = 0;
	kernel_stack_malloced = false;
	pledged_destruction = false;
	force_no_signals = false;
	signal_single = false;
	has_saved_signal_mask = false;
	sigemptyset(&signal_pending);
	sigemptyset(&signal_mask);
	sigemptyset(&saved_signal_mask);
	memset(&signal_stack, 0, sizeof(signal_stack));
	signal_stack.ss_flags = SS_DISABLE;
	// execute_clock initialized in member constructor.
	// system_clock initialized in member constructor.
	Time::InitializeThreadClocks(this);
	futex_address = 0;
	kutex_address = 0;
	futex_woken = false;
	kutex_woken = false;
	futex_prev_waiting = NULL;
	futex_next_waiting = NULL;
	kutex_prev_waiting = NULL;
	kutex_next_waiting = NULL;
	yield_operation = YIELD_OPERATION_NONE;
}

Thread::~Thread()
{
	if ( process )
		process->OnThreadDestruction(this);
	assert(CurrentThread() != this);
	if ( kernel_stack_malloced )
		delete[] (uint8_t*) kernel_stack_pos;
}

Thread* CreateKernelThread(Process* process,
                           struct thread_registers* regs,
                           const char* name)
{
	assert(process && regs && process->addrspace);

#if defined(__x86_64__)
	if ( regs->fsbase >> 48 != 0x0000 && regs->fsbase >> 48 != 0xFFFF )
		return errno = EINVAL, (Thread*) NULL;
	if ( regs->gsbase >> 48 != 0x0000 && regs->gsbase >> 48 != 0xFFFF )
		return errno = EINVAL, (Thread*) NULL;
#endif

	kthread_mutex_lock(&process->thread_lock);

	// Note: Only allow the process itself to make threads, except the initial
	// thread. This requirement is because kthread_exit() needs to know when
	// it's the last thread in the process (using threads_not_exiting_count),
	// and that no more threads will appear, so it can run some final process
	// termination steps without any interference. It's always allowed to create
	// threads in the kernel process as it never exits.
	assert(!process->first_thread ||
	       process == CurrentProcess() ||
	       process == Scheduler::GetKernelProcess());

	Thread* thread = new Thread();
	if ( !thread )
		return NULL;
	thread->name = name;

#if defined(__i386__)
	thread->tid = regs->gsbase;
#elif defined(__x86_64__)
	thread->tid = regs->fsbase;
#else
#warning "You need to initialize the tid here"
#endif
	memcpy(&thread->registers, regs, sizeof(struct thread_registers));

	// Create the family tree.
	thread->process = process;
	Thread* firsty = process->first_thread;
	if ( firsty )
		firsty->prev_sibling = thread;
	thread->next_sibling = firsty;
	process->first_thread = thread;
	process->threads_not_exiting_count++;

	kthread_mutex_unlock(&process->thread_lock);

	return thread;
}

static void SetupKernelThreadRegs(struct thread_registers* regs,
                                  Process* process,
                                  void (*entry)(void*),
                                  void* user,
                                  uintptr_t stack,
                                  size_t stack_size)
{
	memset(regs, 0, sizeof(*regs));

	size_t stack_alignment = 16;
	while ( stack & (stack_alignment-1) )
	{
		assert(stack_size);
		stack++;
		stack_size--;
	}

	stack_size &= ~(stack_alignment-1);

#if defined(__i386__)
	uintptr_t* stack_values = (uintptr_t*) (stack + stack_size);

	assert(5 * sizeof(uintptr_t) <= stack_size);

	/* -- 16-byte aligned -- */
	/* -1 padding */
	stack_values[-2] = (uintptr_t) 0; /* null eip */
	stack_values[-3] = (uintptr_t) 0; /* null ebp */
	stack_values[-4] = (uintptr_t) user; /* thread parameter */
	/* -- 16-byte aligned -- */
	stack_values[-5] = (uintptr_t) kthread_exit; /* return to kthread_exit */
	/* upcoming ebp */
	/* -7 padding */
	/* -8 padding */
	/* -- 16-byte aligned -- */

	regs->eip = (uintptr_t) entry;
	regs->esp = (uintptr_t) (stack_values - 5);
	regs->eax = 0;
	regs->ebx = 0;
	regs->ecx = 0;
	regs->edx = 0;
	regs->edi = 0;
	regs->esi = 0;
	regs->ebp = (uintptr_t) (stack_values - 3);
	regs->cs = KCS | KRPL;
	regs->ds = KDS | KRPL;
	regs->ss = KDS | KRPL;
	regs->eflags = FLAGS_RESERVED1 | FLAGS_INTERRUPT | FLAGS_ID;
	regs->kerrno = 0;
	regs->signal_pending = 0;
	regs->kernel_stack = stack + stack_size;
	regs->cr3 = process->addrspace;
	memcpy(regs->fpuenv, Float::fpu_initialized_regs, 512);
#elif defined(__x86_64__)
	uintptr_t* stack_values = (uintptr_t*) (stack + stack_size);

	assert(3 * sizeof(uintptr_t) <= stack_size);

	stack_values[-1] = (uintptr_t) 0; /* null rip */
	stack_values[-2] = (uintptr_t) 0; /* null rbp */
	stack_values[-3] = (uintptr_t) kthread_exit; /* return to kthread_exit */

	regs->rip = (uintptr_t) entry;
	regs->rsp = (uintptr_t) (stack_values - 3);
	regs->rax = 0;
	regs->rbx = 0;
	regs->rcx = 0;
	regs->rdx = 0;
	regs->rdi = (uintptr_t) user;
	regs->rsi = 0;
	regs->rbp = 0;
	regs->r8  = 0;
	regs->r9  = 0;
	regs->r10 = 0;
	regs->r11 = 0;
	regs->r12 = 0;
	regs->r13 = 0;
	regs->r14 = 0;
	regs->r15 = 0;
	regs->cs = KCS | KRPL;
	regs->ds = KDS | KRPL;
	regs->ss = KDS | KRPL;
	regs->rflags = FLAGS_RESERVED1 | FLAGS_INTERRUPT | FLAGS_ID;
	regs->kerrno = 0;
	regs->signal_pending = 0;
	regs->kernel_stack = stack + stack_size;
	regs->cr3 = process->addrspace;
	memcpy(regs->fpuenv, Float::fpu_initialized_regs, 512);
#else
#warning "You need to add kernel thread register initialization support"
#endif
}

Thread* CreateKernelThread(Process* process, void (*entry)(void*), void* user,
                           const char* name, size_t stacksize)
{
	const size_t DEFAULT_KERNEL_STACK_SIZE = 8 * 1024UL;
	if ( !stacksize )
		stacksize = DEFAULT_KERNEL_STACK_SIZE;
	uint8_t* stack = new uint8_t[stacksize];
	if ( !stack )
		return NULL;

	struct thread_registers regs;
	SetupKernelThreadRegs(&regs, process, entry, user, (uintptr_t) stack,
	                      stacksize);

	Thread* thread = CreateKernelThread(process, &regs, name);
	if ( !thread ) { delete[] stack; return NULL; }

	thread->kernel_stack_pos = (uintptr_t) stack;
	thread->kernel_stack_size = stacksize;
	thread->kernel_stack_malloced = true;

	return thread;
}

Thread* CreateKernelThread(void (*entry)(void*), void* user, const char* name,
                           size_t stacksize)
{
	return CreateKernelThread(CurrentProcess(), entry, user, name, stacksize);
}

void StartKernelThread(Thread* thread)
{
	Scheduler::SetThreadState(thread, ThreadState::RUNNABLE);
}

Thread* RunKernelThread(Process* process, struct thread_registers* regs,
                        const char* name)
{
	Thread* thread = CreateKernelThread(process, regs, name);
	if ( !thread )
		return NULL;
	StartKernelThread(thread);
	return thread;
}

Thread* RunKernelThread(Process* process, void (*entry)(void*), void* user,
                        const char* name, size_t stacksize)
{
	Thread* thread = CreateKernelThread(process, entry, user, name, stacksize);
	if ( !thread )
		return NULL;
	StartKernelThread(thread);
	return thread;
}

Thread* RunKernelThread(void (*entry)(void*), void* user, const char* name,
                        size_t stacksize)
{
	Thread* thread = CreateKernelThread(entry, user, name, stacksize);
	if ( !thread )
		return NULL;
	StartKernelThread(thread);
	return thread;
}

int sys_exit_thread(int requested_exit_code,
                    int flags,
                    const struct exit_thread* user_extended)
{
	if ( flags & ~(EXIT_THREAD_ONLY_IF_OTHERS |
	               EXIT_THREAD_UNMAP |
	               EXIT_THREAD_ZERO |
	               EXIT_THREAD_TLS_UNMAP |
	               EXIT_THREAD_PROCESS |
	               EXIT_THREAD_DUMP_CORE |
	               EXIT_THREAD_FUTEX_WAKE) )
		return errno = EINVAL, -1;

	if ( (flags & EXIT_THREAD_ONLY_IF_OTHERS) && (flags & EXIT_THREAD_PROCESS) )
		return errno = EINVAL, -1;

	Thread* thread = CurrentThread();
	Process* process = CurrentProcess();

	struct exit_thread extended;
	if ( !user_extended )
		memset(&extended, 0, sizeof(extended));
	else if ( !CopyFromUser(&extended, user_extended, sizeof(extended)) )
		return -1;

	extended.unmap_size = Page::AlignUp(extended.unmap_size);

	kthread_mutex_lock(&thread->process->thread_lock);
	bool is_others = false;
	for ( Thread* iter = thread->process->first_thread;
	      !is_others && iter;
	     iter = iter->next_sibling )
	{
		if ( iter == thread )
			continue;
		if ( iter->pledged_destruction )
			continue;
		is_others = true;
	}
	if ( !(flags & EXIT_THREAD_ONLY_IF_OTHERS) || is_others )
		thread->pledged_destruction = true;
	bool are_threads_exiting = false;
	bool do_exit = (flags & EXIT_THREAD_PROCESS) || !is_others;
	if ( do_exit )
		process->threads_exiting = true;
	else if ( process->threads_exiting )
		are_threads_exiting = true;
	kthread_mutex_unlock(&thread->process->thread_lock);

	// Self-destruct if another thread began exiting the process.
	if ( are_threads_exiting )
		kthread_exit();

	if ( (flags & EXIT_THREAD_ONLY_IF_OTHERS) && !is_others )
		return errno = ESRCH, -1;

	if ( flags & EXIT_THREAD_UNMAP &&
	     Page::IsAligned((uintptr_t) extended.unmap_from) &&
	     extended.unmap_size )
	{
		ScopedLock lock(&process->segment_lock);
		extended.unmap_size = Page::AlignDown(extended.unmap_size);
		Memory::UnmapMemory(process, (uintptr_t) extended.unmap_from,
		                                         extended.unmap_size);
		Memory::Flush();
		// TODO: The segment is not actually removed!
	}

	if ( flags & EXIT_THREAD_TLS_UNMAP &&
	     Page::IsAligned((uintptr_t) extended.tls_unmap_from) &&
	     extended.tls_unmap_size )
	{
		ScopedLock lock(&process->segment_lock);
		extended.tls_unmap_size = Page::AlignDown(extended.tls_unmap_size);
		Memory::UnmapMemory(process, (uintptr_t) extended.tls_unmap_from,
		                                         extended.tls_unmap_size);
		Memory::Flush();
	}

	if ( flags & EXIT_THREAD_ZERO )
		ZeroUser(extended.zero_from, extended.zero_size);

	if ( flags & EXIT_THREAD_FUTEX_WAKE )
		sys_futex((int*) extended.zero_from, FUTEX_WAKE, 1, NULL);

	if ( do_exit )
	{
		// Validate the requested exit code such that the process can't exit
		// with an impossible exit status or that it wasn't actually terminated.

		int the_nature = WNATURE(requested_exit_code);
		int the_status = WEXITSTATUS(requested_exit_code);
		int the_signal = WTERMSIG(requested_exit_code);

		if ( the_nature == WNATURE_EXITED )
			the_signal = 0;
		else if ( the_nature == WNATURE_SIGNALED )
		{
			if ( the_signal == 0 /* null signal */ ||
			     the_signal == SIGSTOP ||
			     the_signal == SIGTSTP ||
			     the_signal == SIGTTIN ||
			     the_signal == SIGTTOU ||
			     the_signal == SIGCONT )
				the_signal = SIGKILL;
			the_status = 128 + the_signal;
		}
		else
		{
			the_nature = WNATURE_SIGNALED;
			the_signal = SIGKILL;
		}

		requested_exit_code = WCONSTRUCT(the_nature, the_status, the_signal);

		thread->process->ExitWithCode(requested_exit_code);
	}

	kthread_exit();
}

static void futex_timeout(Clock* /*clock*/, Timer* /*timer*/, void* ctx)
{
	Thread* thread = (Thread*) ctx;
	thread->timer_woken = true;
	kthread_wake_futex(thread);
}

int sys_futex(int* user_address,
              int op,
              int value,
              const struct timespec* user_timeout)
{
	ioctx_t ctx; SetupKernelIOCtx(&ctx);
	Thread* thread = CurrentThread();
	Process* process = thread->process;
	if ( FUTEX_GET_OP(op) == FUTEX_WAIT )
	{
		kthread_mutex_lock(&process->futex_lock);
		thread->futex_address = (uintptr_t) user_address;
		thread->futex_woken = false;
		thread->futex_prev_waiting = process->futex_last_waiting;
		thread->futex_next_waiting = NULL;
		(process->futex_last_waiting ?
		 process->futex_last_waiting->futex_next_waiting :
		 process->futex_first_waiting) = thread;
		process->futex_last_waiting = thread;
		kthread_mutex_unlock(&process->futex_lock);
		thread->timer_woken = false;
		Timer timer;
		if ( user_timeout )
		{
			clockid_t clockid = FUTEX_GET_CLOCK(op);
			bool absolute = op & FUTEX_ABSOLUTE;
			struct timespec timeout;
			if ( !CopyFromUser(&timeout, user_timeout, sizeof(timeout)) )
				return -1;
			if ( !timespec_is_canonical(timeout) )
				return errno = EINVAL, -1;
			Clock* clock = Time::GetClock(clockid);
			timer.Attach(clock);
			struct itimerspec timerspec;
			timerspec.it_value = timeout;
			timerspec.it_interval.tv_sec = 0;
			timerspec.it_interval.tv_nsec = 0;
			int timer_flags = (absolute ? TIMER_ABSOLUTE : 0) |
			                  TIMER_FUNC_INTERRUPT_HANDLER;
			timer.Set(&timerspec, NULL, timer_flags, futex_timeout, thread);
		}
		int result = 0;
		int current;
		if ( !ReadAtomicFromUser(&current, user_address) )
			result = -1;
		else if ( current != value )
		{
			errno = EAGAIN;
			result = -1;
		}
		else
			kthread_wait_futex_signal();
		if ( user_timeout )
			timer.Cancel();
		kthread_mutex_lock(&process->futex_lock);
		if ( result == 0 && !thread->futex_woken )
		{
			if ( Signal::IsPending() )
			{
				errno = EINTR;
				result = -1;
			}
			else if ( thread->timer_woken )
			{
				errno = ETIMEDOUT;
				result = -1;
			}
		}
		thread->futex_address = 0;
		thread->futex_woken = false;
		(thread->futex_prev_waiting ?
		 thread->futex_prev_waiting->futex_next_waiting :
		 process->futex_first_waiting) = thread->futex_next_waiting;
		(thread->futex_next_waiting ?
		 thread->futex_next_waiting->futex_prev_waiting :
		 process->futex_last_waiting) = thread->futex_prev_waiting;
		thread->futex_prev_waiting = NULL;
		thread->futex_next_waiting = NULL;
		kthread_mutex_unlock(&process->futex_lock);
		return result;
	}
	else if ( FUTEX_GET_OP(op) == FUTEX_WAKE )
	{
		kthread_mutex_lock(&process->futex_lock);
		int result = 0;
		for ( Thread* waiter = process->futex_first_waiting;
		      0 < value && waiter;
		      waiter = waiter->futex_next_waiting )
		{
			if ( waiter->futex_address == (uintptr_t) user_address )
			{
				waiter->futex_woken = true;
				kthread_wake_futex(waiter);
				if ( value != INT_MAX )
					value--;
				if ( result != INT_MAX )
					result++;
			}
		}
		kthread_mutex_unlock(&process->futex_lock);
		return result;
	}
	else
		return errno = EINVAL, -1;
}

} // namespace Sortix
