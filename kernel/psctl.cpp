/*
 * Copyright (c) 2015, 2016, 2022 Jonas 'Sortie' Termansen.
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
 * psctl.cpp
 * Process control interface.
 */

#include <sys/ioctl.h>
#include <sys/types.h>

#include <errno.h>
#include <limits.h>
#include <string.h>

#if !defined(TTY_NAME_MAX)
#include <sortix/limits.h>
#endif
#include <sortix/psctl.h>

#include <sortix/kernel/clock.h>
#include <sortix/kernel/descriptor.h>
#include <sortix/kernel/interrupt.h>
#include <sortix/kernel/copy.h>
#include <sortix/kernel/ioctx.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/process.h>
#include <sortix/kernel/ptable.h>
#include <sortix/kernel/refcount.h>
#include <sortix/kernel/syscall.h>

namespace Sortix {

int sys_psctl(pid_t pid, int request, void* ptr)
{
	ScopedLock lock(&process_family_lock);
	Ref<ProcessTable> ptable = CurrentProcess()->GetPTable();
	if ( request == PSCTL_PREV_PID )
	{
		struct psctl_prev_pid resp;
		memset(&resp, 0, sizeof(resp));
		resp.prev_pid = ptable->Prev(pid);
		return CopyToUser(ptr, &resp, sizeof(resp)) ? 0 : -1;
	}
	else if ( request == PSCTL_NEXT_PID )
	{
		struct psctl_next_pid resp;
		memset(&resp, 0, sizeof(resp));
		resp.next_pid = ptable->Next(pid);
		return CopyToUser(ptr, &resp, sizeof(resp)) ? 0 : -1;
	}
	Process* process = ptable->Get(pid);
	if ( !process )
		return errno = ESRCH, -1;
	if ( request == PSCTL_STAT )
	{
		struct psctl_stat psst;
		memset(&psst, 0, sizeof(psst));
		psst.pid = pid;
		if ( process->parent )
		{
			Process* parent = process->parent;
			psst.ppid = parent->pid;
			psst.ppid_prev = process->prevsibling ? process->prevsibling->pid : -1;
			psst.ppid_next = process->nextsibling ? process->nextsibling->pid : -1;
		}
		else
		{
			psst.ppid = -1;
			psst.ppid_prev = -1;
			psst.ppid_next = -1;
		}
		psst.ppid_first = process->firstchild ? process->firstchild->pid : -1;
		if ( process->group )
		{
			Process* group = process->group;
			psst.pgid = group->pid;
			psst.pgid_prev = process->groupprev ? process->groupprev->pid : -1;
			psst.pgid_next = process->groupnext ? process->groupnext->pid : -1;
		}
		else
		{
			psst.pgid = -1;
			psst.pgid_prev = -1;
			psst.pgid_next = -1;
		}
		psst.pgid_first = process->groupfirst ? process->groupfirst->pid : -1;
		if ( process->session )
		{
			Process* session = process->session;
			psst.sid = session->pid;
			psst.sid_prev = process->sessionprev ? process->sessionprev->pid : -1;
			psst.sid_next = process->sessionnext ? process->sessionnext->pid : -1;
		}
		else
		{
			psst.sid = -1;
			psst.sid_prev = -1;
			psst.sid_next = -1;
		}
		psst.sid_first = process->sessionfirst ? process->sessionfirst->pid : -1;
		// TODO: Implement init groupings.
		psst.init = 1;
		psst.init_prev = ptable->Prev(pid);
		psst.init_next = ptable->Next(pid);
		psst.init_first = pid == 1 ? 1 : -1;
		kthread_mutex_lock(&process->idlock);
		psst.uid = process->uid;
		psst.euid = process->euid;
		psst.gid = process->gid;
		psst.egid = process->egid;
		kthread_mutex_unlock(&process->idlock);
		kthread_mutex_lock(&process->threadlock);
		psst.status = process->exit_code;
		kthread_mutex_unlock(&process->threadlock);
		kthread_mutex_lock(&process->nicelock);
		psst.nice = process->nice;
		kthread_mutex_unlock(&process->nicelock);
		kthread_mutex_lock(&process->segment_lock);
		// TODO: Cache these.
		for ( size_t i = 0; i < process->segments_used; i++ )
		{
			const struct segment* segment = &process->segments[i];
			psst.pss += segment->size;
			psst.rss += segment->size;
			psst.uss += segment->size;
			psst.vms += segment->size;
		}
		kthread_mutex_unlock(&process->segment_lock);
		// Note: It is safe to access the clocks in this manner as each of them
		//       are locked by disabling interrupts. This is perhaps not
		//       SMP-ready, but it will do for now.
		Interrupt::Disable();
		psst.tmns.tmns_utime = process->execute_clock.current_time;
		psst.tmns.tmns_stime = process->system_clock.current_time;
		psst.tmns.tmns_cutime = process->child_execute_clock.current_time;
		psst.tmns.tmns_cstime = process->child_system_clock.current_time;
		Interrupt::Enable();
		return CopyToUser(ptr, &psst, sizeof(psst)) ? 0 : -1;
	}
	else if ( request == PSCTL_PROGRAM_PATH )
	{
		struct psctl_program_path ctl;
		if ( !CopyFromUser(&ctl, ptr, sizeof(ctl)) )
			return -1;
		// TODO: program_image_path is not properly protected at this time.
		const char* path = process->program_image_path;
		if ( !path )
			path = "";
		size_t size = strlen(path) + 1;
		struct psctl_program_path resp = ctl;
		resp.size = size;
		if ( !CopyToUser(ptr, &resp, sizeof(resp)) )
			return -1;
		if ( ctl.buffer )
		{
			if ( ctl.size < size )
				return errno = ERANGE, -1;
			if ( !CopyToUser(ctl.buffer, path, size) )
				return -1;
		}
		return 0;
	}
	else if ( request == PSCTL_TTYNAME )
	{
		struct psctl_ttyname ctl;
		if ( !CopyFromUser(&ctl, ptr, sizeof(ctl)) )
			return -1;
		ioctx_t kctx; SetupKernelIOCtx(&kctx);
		if ( !process->session )
			return errno = ENOTTY, -1;
		Ref<Descriptor> tty = process->session->GetTTY();
		if ( !tty )
			return errno = ENOTTY, -1;
		char ttyname[TTY_NAME_MAX-5+1];
		if ( tty->ioctl(&kctx, TIOCGNAME, (uintptr_t) ttyname) < 0 )
			return -1;
		size_t size = strlen(ttyname) + 1;
		struct psctl_ttyname resp = ctl;
		resp.size = size;
		if ( !CopyToUser(ptr, &resp, sizeof(resp)) )
			return -1;
		if ( ctl.buffer )
		{
			if ( ctl.size < size )
				return errno = ERANGE, -1;
			if ( !CopyToUser(ctl.buffer, ttyname, size) )
				return -1;
		}
		return 0;
	}
	return errno = EINVAL, -1;
}

} // namespace Sortix
