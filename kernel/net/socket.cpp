/*
 * Copyright (c) 2016, 2017 Jonas 'Sortie' Termansen.
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
 * net/socket.cpp
 * Socket system calls.
 */

#include <sys/socket.h>

#include <fcntl.h>
#include <errno.h>

#include <sortix/kernel/descriptor.h>
#include <sortix/kernel/dtable.h>
#include <sortix/kernel/inode.h>
#include <sortix/kernel/process.h>
#include <sortix/kernel/syscall.h>
#include <sortix/kernel/vnode.h>

#include "fs.h"
#include "ipv4.h"

namespace Sortix {

static Ref<Inode> CreateSocket(int domain, int type, int protocol)
{
	switch ( domain )
	{
	case AF_INET: return IPv4::Socket(type, protocol);
	case AF_UNIX: return NetFS::Socket(type, protocol);
	default: return errno = EAFNOSUPPORT, Ref<Inode>(NULL);
	}
}

int sys_socket(int domain, int type, int protocol)
{
	int dflags = O_READ | O_WRITE;
	int fdflags = 0;
	if ( type & SOCK_NONBLOCK ) dflags |= O_NONBLOCK;
	if ( type & SOCK_CLOEXEC ) fdflags |= FD_CLOEXEC;
	if ( type & SOCK_CLOFORK ) fdflags |= FD_CLOFORK;
	type &= ~(SOCK_NONBLOCK | SOCK_CLOEXEC | SOCK_CLOFORK);

	Ref<Inode> inode = CreateSocket(domain, type, protocol);
	if ( !inode )
		return -1;
	Ref<Vnode> vnode(new Vnode(inode, Ref<Vnode>(NULL), 0, 0));
	if ( !vnode )
		return -1;
	inode.Reset();
	Ref<Descriptor> desc(new Descriptor(vnode, dflags));
	if ( !desc )
		return -1;
	vnode.Reset();

	Process* process = CurrentProcess();
	Ref<DescriptorTable> dtable = process->GetDTable();
	return dtable->Allocate(desc, fdflags);
}

} // namespace Sortix
