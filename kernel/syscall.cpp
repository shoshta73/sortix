/*
 * Copyright (c) 2011-2016, 2021-2026 Jonas 'Sortie' Termansen.
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
 * syscall.cpp
 * Handles system calls from user-space.
 */

#include <errno.h>
#include <signal.h>
#include <stddef.h>

#include <sortix/syscall.h>

#include <sortix/kernel/descriptor.h>
#include <sortix/kernel/ioctx.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/log.h>
#include <sortix/kernel/process.h>
#include <sortix/kernel/thread.h>
#include <sortix/kernel/syscall.h>

namespace Sortix {

extern "C" {
void* syscall_list[SYSCALL_MAX_NUM + 1] =
{
	[SYSCALL_BAD_SYSCALL] = (void*) sys_bad_syscall,
	[SYSCALL_EXIT] = (void*) sys_bad_syscall,
	[SYSCALL_SLEEP] = (void*) sys_bad_syscall,
	[SYSCALL_USLEEP] = (void*) sys_bad_syscall,
	[SYSCALL_PRINT_STRING] = (void*) sys_bad_syscall,
	[SYSCALL_CREATE_FRAME] = (void*) sys_bad_syscall,
	[SYSCALL_CHANGE_FRAME] = (void*) sys_bad_syscall,
	[SYSCALL_DELETE_FRAME] = (void*) sys_bad_syscall,
	[SYSCALL_RECEIVE_KEYSTROKE] = (void*) sys_bad_syscall,
	[SYSCALL_SET_FREQUENCY] = (void*) sys_bad_syscall,
	[SYSCALL_EXECVE] = (void*) sys_execve,
	[SYSCALL_PRINT_PATH_FILES] = (void*) sys_bad_syscall,
	[SYSCALL_FORK] = (void*) sys_bad_syscall,
	[SYSCALL_GETPID] = (void*) sys_getpid,
	[SYSCALL_GETPPID] = (void*) sys_getppid,
	[SYSCALL_GET_FILEINFO] = (void*) sys_bad_syscall,
	[SYSCALL_GET_NUM_FILES] = (void*) sys_bad_syscall,
	[SYSCALL_WAITPID] = (void*) sys_waitpid,
	[SYSCALL_READ] = (void*) sys_read,
	[SYSCALL_WRITE] = (void*) sys_write,
	[SYSCALL_PIPE] = (void*) sys_bad_syscall,
	[SYSCALL_CLOSE] = (void*) sys_close,
	[SYSCALL_DUP] = (void*) sys_dup,
	[SYSCALL_OPEN] = (void*) sys_bad_syscall,
	[SYSCALL_READDIRENTS] = (void*) sys_readdirents,
	[SYSCALL_CHDIR] = (void*) sys_bad_syscall,
	[SYSCALL_GETCWD] = (void*) sys_bad_syscall,
	[SYSCALL_UNLINK] = (void*) sys_bad_syscall,
	[SYSCALL_REGISTER_ERRNO] = (void*) sys_bad_syscall,
	[SYSCALL_REGISTER_SIGNAL_HANDLER] = (void*) sys_bad_syscall,
	[SYSCALL_SIGRETURN] = (void*) sys_bad_syscall,
	[SYSCALL_KILL] = (void*) sys_kill,
	[SYSCALL_MEMSTAT] = (void*) sys_memstat,
	[SYSCALL_ISATTY] = (void*) sys_isatty,
	[SYSCALL_UPTIME] = (void*) sys_bad_syscall,
	[SYSCALL_SBRK] = (void*) sys_bad_syscall,
	[SYSCALL_LSEEK] = (void*) sys_lseek,
	[SYSCALL_GETPAGESIZE] = (void*) sys_getpagesize,
	[SYSCALL_MKDIR] = (void*) sys_bad_syscall,
	[SYSCALL_RMDIR] = (void*) sys_bad_syscall,
	[SYSCALL_TRUNCATE] = (void*) sys_bad_syscall,
	[SYSCALL_FTRUNCATE] = (void*) sys_ftruncate,
	[SYSCALL_SETTERMMODE] = (void*) sys_bad_syscall,
	[SYSCALL_GETTERMMODE] = (void*) sys_bad_syscall,
	[SYSCALL_STAT] = (void*) sys_bad_syscall,
	[SYSCALL_FSTAT] = (void*) sys_fstat,
	[SYSCALL_FCNTL] = (void*) sys_fcntl,
	[SYSCALL_ACCESS] = (void*) sys_bad_syscall,
	[SYSCALL_KERNELINFO] = (void*) sys_kernelinfo,
	[SYSCALL_PREAD] = (void*) sys_pread,
	[SYSCALL_PWRITE] = (void*) sys_pwrite,
	[SYSCALL_TFORK] = (void*) sys_tfork,
	[SYSCALL_TCGETWINSIZE] = (void*) sys_tcgetwinsize,
	[SYSCALL_RAISE] = (void*) sys_raise,
	[SYSCALL_OPENAT] = (void*) sys_openat,
	[SYSCALL_DISPMSG_ISSUE] = (void*) sys_dispmsg_issue,
	[SYSCALL_FSTATAT] = (void*) sys_fstatat,
	[SYSCALL_CHMOD] = (void*) sys_bad_syscall,
	[SYSCALL_CHOWN] = (void*) sys_bad_syscall,
	[SYSCALL_LINK] = (void*) sys_bad_syscall,
	[SYSCALL_DUP2] = (void*) sys_dup2,
	[SYSCALL_UNLINKAT] = (void*) sys_unlinkat,
	[SYSCALL_FACCESSAT] = (void*) sys_faccessat,
	[SYSCALL_MKDIRAT] = (void*) sys_mkdirat,
	[SYSCALL_FCHDIR] = (void*) sys_fchdir,
	[SYSCALL_TRUNCATEAT_NOFLAGS] = (void*) sys_truncateat_noflags,
#if !defined(__i386__)
	[SYSCALL_FCHOWNAT] = (void*) sys_fchownat,
#else
	[SYSCALL_FCHOWNAT] = (void*) sys_fchownat_wrapper,
#endif
	[SYSCALL_FCHOWN] = (void*) sys_fchown,
	[SYSCALL_FCHMOD] = (void*) sys_fchmod,
	[SYSCALL_FCHMODAT] = (void*) sys_fchmodat,
	[SYSCALL_LINKAT] = (void*) sys_linkat,
	[SYSCALL_FSM_FSBIND] = (void*) sys_fsm_fsbind,
	[SYSCALL_PPOLL] = (void*) sys_ppoll,
	[SYSCALL_RENAMEAT] = (void*) sys_renameat,
	[SYSCALL_READLINKAT] = (void*) sys_readlinkat,
	[SYSCALL_FSYNC] = (void*) sys_fsync,
	[SYSCALL_GETUID] = (void*) sys_getuid,
	[SYSCALL_GETGID] = (void*) sys_getgid,
	[SYSCALL_SETUID] = (void*) sys_setuid,
	[SYSCALL_SETGID] = (void*) sys_setgid,
	[SYSCALL_GETEUID] = (void*) sys_geteuid,
	[SYSCALL_GETEGID] = (void*) sys_getegid,
	[SYSCALL_SETEUID] = (void*) sys_seteuid,
	[SYSCALL_SETEGID] = (void*) sys_setegid,
	[SYSCALL_IOCTL] = (void*) sys_ioctl,
	[SYSCALL_UTIMENSAT] = (void*) sys_utimensat,
	[SYSCALL_FUTIMENS] = (void*) sys_futimens,
	[SYSCALL_RECV] = (void*) sys_recv,
	[SYSCALL_SEND] = (void*) sys_send,
	[SYSCALL_ACCEPT4] = (void*) sys_accept4,
	[SYSCALL_BIND] = (void*) sys_bind,
	[SYSCALL_CONNECT] = (void*) sys_connect,
	[SYSCALL_LISTEN] = (void*) sys_listen,
	[SYSCALL_READV] = (void*) sys_readv,
	[SYSCALL_WRITEV] = (void*) sys_writev,
	[SYSCALL_PREADV] = (void*) sys_preadv,
	[SYSCALL_PWRITEV] = (void*) sys_pwritev,
	[SYSCALL_TIMER_CREATE] = (void*) sys_timer_create,
	[SYSCALL_TIMER_DELETE] = (void*) sys_timer_delete,
	[SYSCALL_TIMER_GETOVERRUN] = (void*) sys_timer_getoverrun,
	[SYSCALL_TIMER_GETTIME] = (void*) sys_timer_gettime,
	[SYSCALL_TIMER_SETTIME] = (void*) sys_timer_settime,
	[SYSCALL_ALARMNS] = (void*) sys_alarmns,
	[SYSCALL_CLOCK_GETTIMERES] = (void*) sys_clock_gettimeres,
	[SYSCALL_CLOCK_SETTIMERES] = (void*) sys_clock_settimeres,
	[SYSCALL_CLOCK_NANOSLEEP] = (void*) sys_clock_nanosleep,
	[SYSCALL_TIMENS] = (void*) sys_timens,
	[SYSCALL_UMASK] = (void*) sys_umask,
	[SYSCALL_FCHDIRAT_NOFLAGS] = (void*) sys_fchdirat_noflags,
	[SYSCALL_FCHROOT] = (void*) sys_fchroot,
	[SYSCALL_FCHROOTAT_NOFLAGS] = (void*) sys_fchrootat_noflags,
	[SYSCALL_MKPARTITION] = (void*) sys_mkpartition,
	[SYSCALL_GETPGID] = (void*) sys_getpgid,
	[SYSCALL_SETPGID] = (void*) sys_setpgid,
	[SYSCALL_TCGETPGRP] = (void*) sys_tcgetpgrp,
	[SYSCALL_TCSETPGRP] = (void*) sys_tcsetpgrp,
	[SYSCALL_MMAP_WRAPPER] = (void*) sys_mmap_wrapper,
	[SYSCALL_MPROTECT] = (void*) sys_mprotect,
	[SYSCALL_MUNMAP] = (void*) sys_munmap,
	[SYSCALL_GETPRIORITY] = (void*) sys_getpriority,
	[SYSCALL_SETPRIORITY] = (void*) sys_setpriority,
	[SYSCALL_PRLIMIT] = (void*) sys_prlimit,
	[SYSCALL_DUP3] = (void*) sys_dup3,
	[SYSCALL_SYMLINKAT] = (void*) sys_symlinkat,
	[SYSCALL_TCGETWINCURPOS] = (void*) sys_tcgetwincurpos,
	[SYSCALL_PIPE2] = (void*) sys_pipe2,
	[SYSCALL_GETUMASK] = (void*) sys_getumask,
	[SYSCALL_FSTATVFS] = (void*) sys_fstatvfs,
	[SYSCALL_FSTATVFSAT] = (void*) sys_fstatvfsat,
	[SYSCALL_RDMSR] = (void*) sys_rdmsr,
	[SYSCALL_WRMSR] = (void*) sys_wrmsr,
	[SYSCALL_SCHED_YIELD] = (void*) sys_sched_yield,
	[SYSCALL_EXIT_THREAD] = (void*) sys_exit_thread,
	[SYSCALL_SIGACTION] = (void*) sys_sigaction,
	[SYSCALL_SIGALTSTACK] = (void*) sys_sigaltstack,
	[SYSCALL_SIGPENDING] = (void*) sys_sigpending,
	[SYSCALL_SIGPROCMASK] = (void*) sys_sigprocmask,
	[SYSCALL_SIGSUSPEND] = (void*) sys_sigsuspend,
	[SYSCALL_SENDMSG] = (void*) sys_sendmsg,
	[SYSCALL_RECVMSG] = (void*) sys_recvmsg,
	[SYSCALL_GETSOCKOPT] = (void*) sys_getsockopt,
	[SYSCALL_SETSOCKOPT] = (void*) sys_setsockopt,
	[SYSCALL_TCGETBLOB] = (void*) sys_tcgetblob,
	[SYSCALL_TCSETBLOB] = (void*) sys_tcsetblob,
	[SYSCALL_GETPEERNAME] = (void*) sys_getpeername,
	[SYSCALL_GETSOCKNAME] = (void*) sys_getsockname,
	[SYSCALL_SHUTDOWN] = (void*) sys_shutdown,
	[SYSCALL_GETENTROPY] = (void*) sys_getentropy,
	[SYSCALL_GETHOSTNAME] = (void*) sys_gethostname,
	[SYSCALL_SETHOSTNAME] = (void*) sys_sethostname,
	[SYSCALL_UNMOUNTAT] = (void*) sys_unmountat,
	[SYSCALL_FSM_MOUNTAT] = (void*) sys_fsm_mountat,
	[SYSCALL_CLOSEFROM] = (void*) sys_closefrom,
	[SYSCALL_MKPTY] = (void*) sys_mkpty,
	[SYSCALL_PSCTL] = (void*) sys_psctl,
	[SYSCALL_TCDRAIN] = (void*) sys_tcdrain,
	[SYSCALL_TCFLOW] = (void*) sys_tcflow,
	[SYSCALL_TCFLUSH] = (void*) sys_tcflush,
	[SYSCALL_TCGETATTR] = (void*) sys_tcgetattr,
	[SYSCALL_TCGETSID] = (void*) sys_tcgetsid,
	[SYSCALL_TCSENDBREAK] = (void*) sys_tcsendbreak,
	[SYSCALL_TCSETATTR] = (void*) sys_tcsetattr,
	[SYSCALL_SCRAM] = (void*) sys_scram,
	[SYSCALL_GETSID] = (void*) sys_getsid,
	[SYSCALL_SETSID] = (void*) sys_setsid,
	[SYSCALL_SOCKET] = (void*) sys_socket,
	[SYSCALL_GETDNSCONFIG] = (void*) sys_getdnsconfig,
	[SYSCALL_SETDNSCONFIG] = (void*) sys_setdnsconfig,
	[SYSCALL_FUTEX] = (void*) sys_futex,
	[SYSCALL_MEMUSAGE] = (void*) sys_memusage,
	[SYSCALL_GETINIT] = (void*) sys_getinit,
	[SYSCALL_SETINIT] = (void*) sys_setinit,
	[SYSCALL_PATHCONFAT] = (void*) sys_pathconfat,
	[SYSCALL_FPATHCONF] = (void*) sys_fpathconf,
	[SYSCALL_TRUNCATEAT] = (void*) sys_truncateat,
	[SYSCALL_FCHDIRAT] = (void*) sys_fchdirat,
	[SYSCALL_FCHROOTAT] = (void*) sys_fchrootat,
	[SYSCALL_EXECVEAT] = (void*) sys_execveat,
	[SYSCALL_FEXECVE] = (void*) sys_fexecve,
	[SYSCALL_TKILL] = (void*) sys_tkill,
	[SYSCALL_GETGROUPS] = (void*) sys_getgroups,
	[SYSCALL_SETGROUPS] = (void*) sys_setgroups,
	[SYSCALL_SOCKATMARK] = (void*) sys_sockatmark,
	[SYSCALL_MAX_NUM] = (void*) sys_bad_syscall,
};
void* strace_list[SYSCALL_MAX_NUM + 1];
void** syscall_ptr = syscall_list;
void strace(void);
} /* extern "C" */

static kthread_mutex_t global_strace_lock = KTHREAD_MUTEX_INITIALIZER;

static void strace_message(Thread* thread, const char* msg)
{
	sigset_t set, oldset;
	sigfillset(&set);
	Signal::UpdateMask(SIG_SETMASK, &set, &oldset);
	// TODO: Avoid SIGPIPE.
	ioctx_t ctx; SetupKernelIOCtx(&ctx);
	size_t sofar = 0;
	size_t count = strlen(msg);
	// TODO: A global strace lock is suboptimal, but does ensure that the pipe
	//       messages are not interleaved if multiple threads are traced on the
	//       same pipe at the same time. We can avoid this by either making the
	//       lock per-strace-pipe or implementing PIPE_BUF semantics.
	ScopedLock lock(&global_strace_lock);
	while ( sofar < count )
	{
		ssize_t amount = thread->strace_log->write(&ctx, (uint8_t*) msg + sofar,
		                                           count - sofar);
		if ( amount < 0 )
			break;
		sofar += amount;
	}
	lock.Reset();
	Signal::UpdateMask(SIG_SETMASK, &oldset, NULL);
}

extern "C"
#if defined(__x86_64__)
void syscall_start(uint64_t rdi, uint64_t rsi, uint64_t rdx, uint64_t rcx,
                   uint64_t r8, uint64_t r9, uint64_t rax)
#elif defined(__i386__)
void syscall_start(uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4,
                   uint32_t p5, uint32_t eax)
#else
#warning "You need to implement syscall_start"
void syscall_start(void)
#endif
{
	Thread* thread = CurrentThread();
	ScopedLock lock(&thread->strace_lock);
	if ( !thread->strace_flags || !thread->strace_log )
		return;
	char msg[256];
	msg[0] = 0;
#if defined(__x86_64__)
	snprintf(msg, sizeof(msg),
	         "%jd 0x%lx 0x%lx(0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx)\n",
	         (intmax_t) thread->process->pid, thread->system_tid,
	         rax, rdi, rsi, rdx, rcx, r8, r9);
#elif defined(__i386__)
	snprintf(msg, sizeof(msg),
	         "%jd 0x%x 0x%x(0x%x, 0x%x, 0x%x, 0x%x, 0x%x)\n",
	         (intmax_t) thread->process->pid, thread->system_tid,
	         eax, p1, p2, p3, p4, p5);
#endif
	strace_message(thread, msg);
}

extern "C"
#if defined(__x86_64__) || defined(__i386__)
void syscall_end(uint64_t result)
#else
#warning "You need to implement syscall_end"
void syscall_end(void)
#endif
{
	Thread* thread = CurrentThread();
	ScopedLock lock(&thread->strace_lock);
	if ( !thread->strace_flags || !thread->strace_log )
		return;
	char msg[256];
	msg[0] = 0;
#if defined(__x86_64__)
	snprintf(msg, sizeof(msg), "%jd 0x%lx = 0x%lx (%d)\n",
	         (intmax_t) thread->process->pid, thread->system_tid,
	         result, errno);
#elif defined(__i386__)
	snprintf(msg, sizeof(msg), "%jd 0x%x = 0x%llx (%d)\n",
	         (intmax_t) thread->process->pid, thread->system_tid,
	         (unsigned long long) result, errno);
#endif
	strace_message(thread, msg);
}

int sys_bad_syscall(void)
{
	return errno = ENOSYS, -1;
}

namespace Syscall {

void Init()
{
	for ( size_t i = 0; i <= SYSCALL_MAX_NUM; i++ )
		strace_list[i] = (void*) strace;
}

void Trace(bool enable)
{
	syscall_ptr = enable ? strace_list : syscall_list;
}


} // namespace Syscall

} // namespace Sortix
