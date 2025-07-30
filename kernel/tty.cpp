/*
 * Copyright (c) 2012-2016, 2021, 2024, 2025 Jonas 'Sortie' Termansen.
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
 * tty.cpp
 * Terminal line discipline.
 */

#include <sys/ioctl.h>
#include <sys/types.h>

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <sortix/fcntl.h>
#include <sortix/keycodes.h>
// TODO: After releasing Sortix 1.1, remove this bootstrap compatibility.
#if !defined(TTY_NAME_MAX)
#include <sortix/limits.h>
#endif
#include <sortix/poll.h>
#include <sortix/signal.h>
#include <sortix/stat.h>
#include <sortix/termios.h>
#include <sortix/termmode.h>
#include <sortix/winsize.h>

#include <sortix/kernel/descriptor.h>
#include <sortix/kernel/inode.h>
#include <sortix/kernel/interlock.h>
#include <sortix/kernel/ioctx.h>
#include <sortix/kernel/kernel.h>
#include <sortix/kernel/keyboard.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/poll.h>
#include <sortix/kernel/process.h>
#include <sortix/kernel/ptable.h>
#include <sortix/kernel/refcount.h>
#include <sortix/kernel/scheduler.h>
#include <sortix/kernel/thread.h>
#include <sortix/kernel/vnode.h>

#include "tty.h"

#define CONTROL(x) (((x) - 64) & 127)
#define M_CONTROL(x) (128 + CONTROL(x))

namespace Sortix {

static const unsigned int SUPPORTED_TERMMODES = TERMMODE_KBKEY
                                              | TERMMODE_UNICODE
                                              | TERMMODE_SIGNAL
                                              | TERMMODE_UTF8
                                              | TERMMODE_LINEBUFFER
                                              | TERMMODE_ECHO
                                              | TERMMODE_NONBLOCK
                                              | TERMMODE_TERMIOS
                                              | TERMMODE_DISABLE
                                              | TERMMODE_NOOPOST
                                              | TERMMODE_NOONLCR
                                              | TERMMODE_OCRNL;

static inline bool IsByteUnescaped(unsigned char byte)
{
	return (32 <= byte && byte != 127) ||
	       byte == '\t' || byte == '\n' || byte == '\r';
}

static inline bool IsUTF8Continuation(unsigned char byte)
{
	return (byte & 0b11000000) == 0b10000000;
}

DevTTY::DevTTY(dev_t dev, mode_t mode, uid_t owner, gid_t group)
{
	if ( !dev )
		dev = (dev_t) this;
	inode_type = INODE_TYPE_TTY;
	this->dev = dev;
	this->ino = (ino_t) this;
	this->type = S_IFFACTORY;
	this->stat_mode = (mode & S_SETABLE) | this->type;
	this->stat_uid = owner;
	this->stat_gid = group;
}

DevTTY::~DevTTY()
{
}

Ref<Inode> DevTTY::factory(ioctx_t* ctx, const char* filename, int flags,
	                       mode_t mode)
{
	(void) ctx;
	(void) filename;
	(void) flags;
	(void) mode;
	ScopedLock lock(&process_family_lock);
	Process* process = CurrentProcess();
	if ( !process->session )
		return errno = ENOTTY, Ref<Inode>(NULL);
	Ref<Descriptor> tty_desc = process->session->GetTTY();
	if ( !tty_desc )
		return errno = ENOTTY, Ref<Inode>(NULL);
	return tty_desc->vnode->inode;
}

TTY::TTY(dev_t dev, ino_t ino, mode_t mode, uid_t owner, gid_t group,
         const char* name)
{
	if ( !dev )
		dev = (dev_t) this;
	if ( !ino )
		ino = (ino_t) this;
	inode_type = INODE_TYPE_TTY;
	this->dev = dev;
	this->ino = ino;
	this->type = S_IFCHR;
	this->stat_mode = (mode & S_SETABLE) | this->type;
	this->stat_uid = owner;
	this->stat_gid = group;
	// Keep this in sync with utils/stty.c, utils/getty.c.
	memset(&tio, 0, sizeof(tio));
	tio.c_iflag = BRKINT | ICRNL | IXANY | IXON;
	tio.c_oflag = OPOST | ONLCR;
	tio.c_cflag = CS8 /*| CREAD*/ | HUPCL; // CREAD unset for boot security.
	tio.c_lflag = ECHO | ECHOE | ECHOK | ICANON | IEXTEN | ISIG;
	tio.c_cc[VEOF] = CONTROL('D');
	tio.c_cc[VEOL] = 0;
	tio.c_cc[VERASE] = CONTROL('?');
	tio.c_cc[VINTR] = CONTROL('C');
	tio.c_cc[VKILL] = CONTROL('U');
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VQUIT] = CONTROL('\\');
	tio.c_cc[VSTART] = CONTROL('Q');
	tio.c_cc[VSTOP] = CONTROL('S');
	tio.c_cc[VSUSP] = CONTROL('Z');
	tio.c_cc[VTIME] = 0;
	tio.c_cc[VWERASE] = CONTROL('W');
	tio.c_ispeed = B38400;
	tio.c_ospeed = B38400;
	termlock = KTHREAD_MUTEX_INITIALIZER;
	datacond = KTHREAD_COND_INITIALIZER;
	numeofs = 0;
	foreground_pgid = PID_MAX;
	sid = -1;
	hungup = false;
	snprintf(ttyname, sizeof(ttyname), "%s", name);
}

TTY::~TTY()
{
}

int TTY::settermmode(ioctx_t* /*ctx*/, unsigned int termmode)
{
	ScopedLock lock(&termlock);
	if ( hungup )
		return errno = EIO, -1;
	if ( !RequireForeground(SIGTTOU) )
		return -1;
	if ( termmode & ~SUPPORTED_TERMMODES )
		return errno = EINVAL, -1;
	tcflag_t old_cflag = tio.c_cflag;
	tcflag_t new_cflag = old_cflag;
	tcflag_t old_lflag = tio.c_lflag;
	tcflag_t new_lflag = old_lflag;
	tcflag_t old_oflag = tio.c_oflag;
	tcflag_t new_oflag = old_oflag;
	if ( termmode & TERMMODE_KBKEY )
		new_lflag |= ISORTIX_KBKEY;
	else
		new_lflag &= ~ISORTIX_KBKEY;
	if ( !(termmode & TERMMODE_UNICODE) )
		new_lflag |= ISORTIX_CHARS_DISABLE;
	else
		new_lflag &= ~ISORTIX_CHARS_DISABLE;
	if ( termmode & TERMMODE_SIGNAL )
		new_lflag |= ISIG;
	else
		new_lflag &= ~ISIG;
	if ( !(termmode & TERMMODE_UTF8) )
		new_lflag |= ISORTIX_32BIT;
	else
		new_lflag &= ~ISORTIX_32BIT;
	if ( termmode & TERMMODE_LINEBUFFER )
		new_lflag |= ICANON;
	else
		new_lflag &= ~ICANON;
	if ( termmode & TERMMODE_ECHO )
		new_lflag |= ECHO | ECHOE;
	else
		new_lflag &= ~(ECHO | ECHOE);
	if ( termmode & TERMMODE_NONBLOCK )
		new_lflag |= ISORTIX_NONBLOCK;
	else
		new_lflag &= ~ISORTIX_NONBLOCK;
	if ( !(termmode & TERMMODE_TERMIOS) )
		new_lflag |= ISORTIX_TERMMODE;
	else
		new_lflag &= ~ISORTIX_TERMMODE;
	if ( !(termmode & TERMMODE_DISABLE) )
		new_cflag |= CREAD;
	else
		new_cflag &= ~CREAD;
	if ( !(termmode & TERMMODE_NOOPOST) )
		new_oflag |= OPOST;
	else
		new_oflag &= ~OPOST;
	if ( !(termmode & TERMMODE_NOONLCR) )
		new_oflag |= ONLCR;
	else
		new_oflag &= ~ONLCR;
	if ( termmode & TERMMODE_OCRNL )
		new_oflag |= OCRNL;
	else
		new_oflag &= ~OCRNL;
	bool old_no_utf8 = old_lflag & ISORTIX_32BIT;
	bool new_no_utf8 = new_lflag & ISORTIX_32BIT;
	struct termios new_tio = tio;
	new_tio.c_cflag = new_cflag;
	new_tio.c_lflag = new_lflag;
	new_tio.c_oflag = new_oflag;
	if ( !Reconfigure(&new_tio) )
		return -1;
	tio = new_tio;
	if ( old_no_utf8 != new_no_utf8 )
		memset(&read_ps, 0, sizeof(read_ps));
	if ( !(tio.c_lflag & ICANON) )
		CommitLineBuffer();
	return 0;
}

int TTY::gettermmode(ioctx_t* ctx, unsigned int* mode)
{
	ScopedLock lock(&termlock);
	if ( hungup )
		return errno = EIO, -1;
	unsigned int termmode = 0;
	if ( tio.c_lflag & ISORTIX_KBKEY )
		termmode |= TERMMODE_KBKEY;
	if ( !(tio.c_lflag & ISORTIX_CHARS_DISABLE) )
		termmode |= TERMMODE_UNICODE;
	if ( tio.c_lflag & ISIG )
		termmode |= TERMMODE_SIGNAL;
	if ( !(tio.c_lflag & ISORTIX_32BIT) )
		termmode |= TERMMODE_UTF8;
	if ( tio.c_lflag & ICANON )
		termmode |= TERMMODE_LINEBUFFER;
	if ( tio.c_lflag & (ECHO | ECHOE) )
		termmode |= TERMMODE_ECHO;
	if ( tio.c_lflag & ISORTIX_NONBLOCK )
		termmode |= TERMMODE_NONBLOCK;
	if ( !(tio.c_lflag & ISORTIX_TERMMODE) )
		termmode |= TERMMODE_TERMIOS;
	if ( !(tio.c_cflag & CREAD) )
		termmode |= TERMMODE_DISABLE;
	if ( !(tio.c_oflag & OPOST) )
		termmode |= TERMMODE_NOOPOST;
	if ( !(tio.c_oflag & ONLCR) )
		termmode |= TERMMODE_NOONLCR;
	if ( tio.c_oflag & OCRNL )
		termmode |= TERMMODE_OCRNL;
	if ( !ctx->copy_to_dest(mode, &termmode, sizeof(termmode)) )
		return -1;
	return 0;
}

int TTY::tcgetwincurpos(ioctx_t* /*ctx*/, struct wincurpos* /*wcp*/)
{
	return errno = ENOTSUP, -1;
}

int TTY::tcsetpgrp(ioctx_t* /*ctx*/, pid_t pgid)
{
	ScopedLock lock(&termlock);
	if ( hungup )
		return errno = EIO, -1;
	ScopedLock family_lock(&process_family_lock);
	if ( !RequireForegroundUnlocked(SIGTTOU) )
		return -1;
	Process* process = CurrentProcess();
	if ( sid < 0 || !process->session || process->session->pid != sid )
		return errno = ENOTTY, -1;
	if ( pgid <= 0 )
		return errno = EINVAL, -1;
	Process* group = CurrentProcess()->GetPTable()->Get(pgid);
	if ( !group )
		return errno = ESRCH, -1;
	if ( !group->group_first ||
	     group->group_first->session != process->session )
		return errno = EPERM, -1;
	foreground_pgid = pgid;
	return 0;
}

pid_t TTY::tcgetpgrp(ioctx_t* /*ctx*/)
{
	ScopedLock lock(&termlock);
	if ( hungup )
		return errno = EIO, -1;
	ScopedLock family_lock(&process_family_lock);
	Process* process = CurrentProcess();
	if ( sid < 0 || !process->session || process->session->pid != sid )
		return errno = ENOTTY, -1;
	return foreground_pgid;
}

void TTY::hup()
{
	ScopedLock lock(&termlock);
	ScopedLock family_lock(&process_family_lock);
	hungup = true;
	if ( 0 < foreground_pgid )
	{
		Process* process = CurrentProcess()->GetPTable()->Get(foreground_pgid);
		if ( process )
			process->DeliverGroupSignal(SIGHUP);
	}
	kthread_cond_broadcast(&datacond);
	poll_channel.Signal(POLLHUP);
}

void TTY::winch() // termlock taken
{
	ScopedLock family_lock(&process_family_lock);
	if ( 0 < foreground_pgid )
	{
		Process* process = CurrentProcess()->GetPTable()->Get(foreground_pgid);
		if ( process )
			process->DeliverGroupSignal(SIGWINCH);
	}
}

void TTY::ProcessUnicode(uint32_t unicode)
{
	mbstate_t ps;
	memset(&ps, 0, sizeof(ps));
	char mb[MB_CUR_MAX];
	size_t amount = wcrtomb(mb, unicode, &ps);
	for ( size_t i = 0; amount != (size_t) -1 && i < amount; i++ )
		ProcessByte((unsigned char) mb[i]);
}

bool TTY::CheckHandledByte(tcflag_t lflags,
                           unsigned char key,
                           unsigned char byte)
{
	return (tio.c_lflag & lflags) == lflags && key && key == byte;
}

void TTY::ProcessByte(unsigned char byte, uint32_t control_unicode)
{
	if ( byte == '\r' && tio.c_iflag & IGNCR )
		return;
	if ( byte == '\r' && tio.c_iflag & ICRNL )
		byte = '\n';
	else if ( byte == '\n' && tio.c_iflag & INLCR )
		byte = '\r';

	// TODO: tio.c_cc[VEOL]
	// TODO: tio.c_cc[VSTART]
	// TODO: tio.c_cc[VSTOP]
	// TODO: tio.c_cc[VSUSP]

	if ( CheckHandledByte(ISIG, tio.c_cc[VQUIT], byte) )
	{
		while ( linebuffer.CanBackspace() )
			linebuffer.Backspace();
		ScopedLock lock(&process_family_lock);
		if ( Process* process = CurrentProcess()->GetPTable()->Get(foreground_pgid) )
			process->DeliverGroupSignal(SIGQUIT);
		return;
	}

	if ( CheckHandledByte(ISIG, tio.c_cc[VINTR], byte) )
	{
		while ( linebuffer.CanBackspace() )
			linebuffer.Backspace();
		ScopedLock lock(&process_family_lock);
		if ( Process* process = CurrentProcess()->GetPTable()->Get(foreground_pgid) )
			process->DeliverGroupSignal(SIGINT);
		return;
	}

	if ( CheckHandledByte(ISIG | ICANON, tio.c_cc[VEOF], byte) )
	{
		if ( !linebuffer.CanPop() )
		{
			numeofs++;
			kthread_cond_broadcast(&datacond);
			poll_channel.Signal(POLLIN | POLLRDNORM);
		}
		return;
	}

	if ( CheckHandledByte(ISIG, tio.c_cc[VQUIT], byte) )
	{
		while ( linebuffer.CanBackspace() )
			linebuffer.Backspace();
		ScopedLock lock(&process_family_lock);
		if ( Process* process = CurrentProcess()->GetPTable()->Get(foreground_pgid) )
			process->DeliverGroupSignal(SIGQUIT);
		return;
	}

	if ( CheckHandledByte(ICANON, tio.c_cc[VERASE], byte) ||
	     CheckHandledByte(ICANON | ISORTIX_TERMMODE, '\b', byte) )
	{
		while ( linebuffer.CanBackspace() )
		{
			uint32_t delchar = linebuffer.Backspace();
			if ( 256 <= delchar )
				continue;
			if ( IsUTF8Continuation(delchar) )
				continue;
			if ( tio.c_lflag & ECHOE )
			{
				// TODO: Handle tab specially. (Is that even possible without
				//       knowing cursor position?).
				tty_output("\b \b");
				if ( !IsByteUnescaped(delchar) )
					tty_output("\b \b");
			}
			break;
		}
		return;
	}

	if ( CheckHandledByte(ICANON, tio.c_cc[VWERASE], byte) )
	{
		bool had_non_whitespace = false;
		while ( linebuffer.CanBackspace() )
		{
			uint32_t delchar = linebuffer.WouldBackspace();
			if ( 256 <= delchar )
				continue;
			if ( IsUTF8Continuation(delchar) )
				continue;
			if ( delchar == L' ' || delchar == L'\t' || delchar == L'\n' )
			{
				if ( had_non_whitespace )
					break;
			}
			else
				had_non_whitespace = true;
			linebuffer.Backspace();
			if ( tio.c_lflag & ECHOE )
			{
				tty_output("\b \b");
				if ( !IsByteUnescaped(delchar) )
					tty_output("\b \b");
			}
		}
		return;
	}

	if ( CheckHandledByte(ICANON, tio.c_cc[VKILL], byte) )
	{
		while ( linebuffer.CanBackspace() )
		{
			uint32_t delchar = linebuffer.Backspace();
			if ( 256 <= delchar )
				continue;
			if ( IsUTF8Continuation(delchar) )
				continue;
			if ( tio.c_lflag & ECHOE )
			{
				tty_output("\b \b");
				if ( !IsByteUnescaped(delchar) )
					tty_output("\b \b");
			}
		}
		return;
	}

	if ( CheckHandledByte(ICANON | ISORTIX_TERMMODE, CONTROL('L'), byte) )
	{
		while ( linebuffer.CanBackspace() )
			linebuffer.Backspace();
		ProcessUnicode(KBKEY_ENCODE(KBKEY_ENTER));
		ProcessByte('\n');
		ProcessUnicode(KBKEY_ENCODE(-KBKEY_ENTER));
		tty_output("\e[H\e[2J");
		return;
	}

	if ( tio.c_lflag & ISORTIX_CHARS_DISABLE )
		return;

	if ( control_unicode &&
	     !(tio.c_lflag & (ICANON | ISIG)) &&
	     tio.c_lflag & ISORTIX_KBKEY )
	{
		ProcessUnicode(control_unicode);
		return;
	}

	if ( !linebuffer.Push(byte) )
		return;

	if ( tio.c_lflag & ECHO )
	{
		if ( byte == '\n' )
		{
			if ( tio.c_oflag & OPOST && tio.c_oflag & ONLCR )
				tty_output("\r\n");
			else if ( tio.c_oflag & OPOST && tio.c_oflag & OCRNL )
				tty_output("\r");
			else
				tty_output("\n");
		}
		else if ( IsByteUnescaped(byte) )
			tty_output(&byte, 1);
		else
		{
			unsigned char cs[2] = { '^', (unsigned char) CONTROL(byte) };
			tty_output(cs, sizeof(cs));
		}
	}

	if ( !(tio.c_lflag & ICANON) || byte == '\n' )
		CommitLineBuffer();
}

void TTY::CommitLineBuffer()
{
	linebuffer.Commit();
	if ( linebuffer.CanPop() || numeofs )
	{
		kthread_cond_broadcast(&datacond);
		poll_channel.Signal(POLLIN | POLLRDNORM);
	}
}

ssize_t TTY::read(ioctx_t* ctx, uint8_t* userbuf, size_t count)
{
	ScopedLockSignal lock(&termlock);
	if ( !lock.IsAcquired() )
		return errno = EINTR, -1;
	if ( hungup )
		return errno = EIO, -1;
	if ( !RequireForeground(SIGTTIN) )
		return -1;
	size_t sofar = 0;
	size_t left = count;
	bool nonblocking = tio.c_lflag & ISORTIX_NONBLOCK ||
	                   ctx->dflags & O_NONBLOCK;
	while ( left )
	{
		while ( !(linebuffer.CanPop() || numeofs) )
		{
			if ( tio.c_lflag & ICANON )
			{
				if ( sofar )
					return sofar;
				if ( nonblocking )
					return errno = EWOULDBLOCK, -1;
				if ( !kthread_cond_wait_signal(&datacond, &termlock) )
					return sofar ? sofar : (errno = EINTR, -1);
				if ( hungup )
					return sofar ? sofar : (errno = EIO, -1);
			}
			else
			{
				// TODO: Study POSIX 11.1.7 Non-Canonical Mode Input Processing.
				if ( tio.c_cc[VMIN] <= sofar )
					return sofar;
				if ( 0 < tio.c_cc[VMIN] )
				{
					if ( nonblocking )
						return errno = EWOULDBLOCK, -1;
					if ( 0 < sofar && 0 < tio.c_cc[VTIME] )
					{
						// TODO: Only wait up until tio.c_cc[VTIME] * 0.1
						// seconds between bytes.
					}
					if ( !kthread_cond_wait_signal(&datacond, &termlock) )
						return sofar ? sofar : (errno = EINTR, -1);
					if ( hungup )
						return sofar ? sofar : (errno = EIO, -1);
				}
				else if ( tio.c_cc[VMIN] == 0 && 0 < tio.c_cc[VTIME] )
				{
					if ( sofar || true /* tio.c_cc[VTIME] * 0.1 seconds passed
					                      since start of read function */ )
						return sofar;
					if ( nonblocking )
						return errno = EWOULDBLOCK, -1;
					// TODO: Only wait up until tio.c_cc[VTIME] * 0.1 seconds.
					if ( !kthread_cond_wait_signal(&datacond, &termlock) )
						return sofar ? sofar : (errno = EINTR, -1);
					if ( hungup )
						return sofar ? sofar : (errno = EIO, -1);
				}
				else
					return sofar;
			}
			if ( !RequireForeground(SIGTTIN) )
				return sofar ? sofar : (errno = EINTR, -1);
		}
		if ( numeofs )
		{
			if ( sofar )
				return sofar;
			return numeofs--, 0;
		}
		uint32_t codepoint = linebuffer.Peek();
		if ( tio.c_lflag & ISORTIX_32BIT )
		{
			if ( left < sizeof(codepoint) )
				return sofar;
			linebuffer.Pop();
			if ( 256 <= codepoint && !(tio.c_lflag & ISORTIX_KBKEY) )
				continue;
			if ( codepoint < 256 && tio.c_lflag & ISORTIX_CHARS_DISABLE )
				continue;
			if ( codepoint < 256 )
			{
				char c = codepoint;
				wchar_t wc;
				size_t amount = mbrtowc(&wc, &c, 1, &read_ps);
				if ( amount == (size_t) -2 )
					continue;
				if ( amount == (size_t) -1 )
				{
					memset(&read_ps, 0, sizeof(read_ps));
					wc = 0xFFFD; /* REPLACEMENT CHARACTER */
				}
				codepoint = wc;
			}
			if ( !ctx->copy_to_dest(userbuf + sofar, &codepoint,
			                        sizeof(codepoint)) )
				return sofar ? sofar : -1;
			left -= sizeof(codepoint);
			sofar += sizeof(codepoint);
			continue;
		}
		if ( 256 <= codepoint )
		{
			linebuffer.Pop();
			continue;
		}
		unsigned char c = codepoint;
		if ( !ctx->copy_to_dest(userbuf + sofar, &c, 1) )
			return sofar ? sofar : -1;
		linebuffer.Pop();
		left -= 1;
		sofar += 1;
		if ( tio.c_lflag & ICANON && c == '\n' )
			break;
	}
	return sofar;
}

ssize_t TTY::write(ioctx_t* ctx, const uint8_t* io_buffer, size_t count)
{
	ScopedLockSignal lock(&termlock);
	if ( !lock.IsAcquired() )
		return errno = EINTR, -1;
	if ( hungup )
		return errno = EIO, -1;
	if ( tio.c_lflag & TOSTOP && !RequireForeground(SIGTTOU) )
		return -1;
	// TODO: Add support for ioctx to the kernel log.
	unsigned char buffer[256];
	size_t max_incoming = sizeof(buffer);
	if ( tio.c_oflag & OPOST && tio.c_oflag & ONLCR )
		max_incoming = sizeof(buffer) / 2;
	size_t sofar = 0;
	size_t rounds = 0;
	while ( sofar < count )
	{
		size_t incoming = count - sofar;
		if ( max_incoming < incoming )
			incoming = max_incoming;
		if ( !ctx->copy_from_src(buffer, io_buffer + sofar, incoming) )
			return -1;
		size_t offset;
		size_t outgoing;
		if ( tio.c_oflag & OPOST && tio.c_oflag & ONLCR )
		{
			offset = sizeof(buffer);
			outgoing = incoming;
			for ( size_t ii = incoming; ii; ii-- )
			{
				size_t i = ii - 1;
				if ( buffer[i] == '\n' )
				{
					buffer[--offset] = '\n';
					buffer[--offset] = '\r';
					outgoing++;
				}
				else
					buffer[--offset] = buffer[i];
			}
		}
		else
		{
			offset = 0;
			outgoing = incoming;
			if ( tio.c_oflag & OPOST && tio.c_oflag & OCRNL )
			{
				for ( size_t i = 0; i < incoming; i++ )
					if ( buffer[i] == '\r' )
						buffer[i] = '\n';
			}
		}
		tty_output(buffer + offset, outgoing);
		sofar += incoming;
		if ( ++rounds % 16 == 0 )
		{
			kthread_mutex_unlock(&termlock);
			kthread_yield();
			kthread_mutex_lock(&termlock);
			if ( hungup )
				return sofar;
		}
		if ( Signal::IsPending() )
			return sofar;
	}
	return (ssize_t) sofar;
}

short TTY::PollEventStatus()
{
	short status = 0;
	if ( hungup )
		status |= POLLHUP;
	if ( linebuffer.CanPop() || numeofs )
		status |= POLLIN | POLLRDNORM;
	if ( true /* can always write */ )
		status |= POLLOUT | POLLWRNORM;
	return status;
}

int TTY::poll(ioctx_t* /*ctx*/, PollNode* node)
{
	ScopedLock lock(&termlock);
	short ret_status = PollEventStatus() & node->events;
	if ( ret_status )
	{
		node->master->revents |= ret_status;
		return 0;
	}
	poll_channel.Register(node);
	return errno = EAGAIN, -1;
}

int TTY::tcdrain(ioctx_t* /*ctx*/)
{
	ScopedLock lock(&termlock);
	if ( hungup )
		return errno = EIO, -1;
	if ( !RequireForeground(SIGTTOU) )
		return -1;
	return 0;
}

int TTY::tcflow(ioctx_t* /*ctx*/, int action)
{
	ScopedLock lock(&termlock);
	if ( !RequireForeground(SIGTTOU) )
		return -1;
	switch ( action )
	{
	case TCOOFF: break; // TODO: Suspend output.
	case TCOON: break; // TODO: Resume suspended output.
	case TCIOFF: break; // TODO: Transmit STOP character.
	case TCION: break; // TODO: Transmit START character.
	default: return errno = EINVAL, -1;
	}
	return 0;
}

int TTY::tcflush(ioctx_t* /*ctx*/, int queue_selector)
{
	if ( queue_selector & ~TCIOFLUSH )
		return errno = EINVAL, -1;
	ScopedLock lock(&termlock);
	if ( hungup )
		return errno = EIO, -1;
	if ( !RequireForeground(SIGTTOU) )
		return -1;
	if ( queue_selector & TCIFLUSH )
		linebuffer.Flush();
	return 0;
}

int TTY::tcgetattr(ioctx_t* ctx, struct termios* io_tio)
{
	ScopedLock lock(&termlock);
	if ( hungup )
		return errno = EIO, -1;
	if ( !ctx->copy_to_dest(io_tio, &tio, sizeof(tio)) )
		return -1;
	return 0;
}

pid_t TTY::tcgetsid(ioctx_t* /*ctx*/)
{
	ScopedLock lock(&termlock);
	if ( hungup )
		return errno = EIO, -1;
	ScopedLock family_lock(&process_family_lock);
	Process* process = CurrentProcess();
	if ( sid < 0 || !process->session || process->session->pid != sid )
		return errno = ENOTTY, -1;
	return sid;
}

int TTY::tcsendbreak(ioctx_t* /*ctx*/, int /*duration*/)
{
	ScopedLock lock(&termlock);
	if ( hungup )
		return errno = EIO, -1;
	if ( !RequireForeground(SIGTTOU) )
		return -1;
	return 0;
}

int TTY::tcsetattr(ioctx_t* ctx, int actions, const struct termios* io_tio)
{
	ScopedLock lock(&termlock);
	if ( hungup )
		return -1;
	if ( !RequireForeground(SIGTTOU) )
		return -1;
	switch ( actions )
	{
	case TCSANOW: break;
	case TCSADRAIN: break;
	case TCSAFLUSH: linebuffer.Flush(); break;
	default: return -1;
	}
	struct termios new_tio;
	if ( !ctx->copy_from_src(&new_tio, io_tio, sizeof(new_tio)) )
		return -1;
	bool old_no_utf8 = tio.c_lflag & ISORTIX_32BIT;
	bool new_no_utf8 = new_tio.c_lflag & ISORTIX_32BIT;
	if ( !Reconfigure(&new_tio) )
		return -1;
	tio = new_tio;
	if ( old_no_utf8 != new_no_utf8 )
		memset(&read_ps, 0, sizeof(read_ps));
	if ( !(tio.c_lflag & ICANON) )
		CommitLineBuffer();
	return 0;
}

int TTY::ioctl(ioctx_t* ctx, int cmd, uintptr_t arg)
{
	ScopedLock lock(&termlock);
	if ( hungup )
		return errno = EIO, -1;
	if ( cmd == TIOCSCTTY )
	{
		// TODO: After releasing Sortix 1.1, restore the invalid arguments check
		// that is temporarily omitted since some Sortix 1.1 commits invoke the
		// ioctl without an argument which was accidentally undefined. For now
		// the kernel should be able to largely run older userspaces without
		// issue, however terminal(1) did this which was fairly essential.
		//if ( ((int) arg) & ~3 )
		//	return errno = EINVAL, -1;
		bool force = arg & 1;
		bool reset = arg & 2;
		if ( !force && 0 <= sid )
			return errno = EPERM, -1;
		ScopedLock family_lock(&process_family_lock);
		Process* process = CurrentProcess();
		if ( !force && !process->session_first )
			return errno = EPERM, -1;
		Process* session = process->session;
		Process* group = process->group;
		if ( session->pid == sid )
		{
			foreground_pgid = group->pid;
			return 0;
		}
		if ( session->GetTTY() && !reset )
			return errno = EPERM, -1;
		if ( (ctx->dflags & (O_READ | O_WRITE)) != (O_READ | O_WRITE) )
			return errno = EPERM, -1;
		Ref<Vnode> vnode(new Vnode(Ref<Inode>(this), Ref<Vnode>(NULL), 0, 0));
		if ( !vnode )
			return -1;
		Ref<Descriptor> desc(new Descriptor(vnode, O_READ | O_WRITE));
		if ( !desc )
			return -1;
		if ( 0 <= sid )
		{
			Process* owner = process->GetPTable()->Get(sid);
			if ( owner )
				owner->SetTTY(Ref<Descriptor>());
			// TODO: SIGHUP?
		}
		sid = session->pid;
		foreground_pgid = group->pid;
		session->SetTTY(desc);
		return 0;
	}
	else if ( cmd == TIOCUCTTY )
	{
		if ( ((int) arg) & ~1 )
			return errno = EINVAL, -1;
		if ( 0 <= sid )
		{
			ScopedLock family_lock(&process_family_lock);
			Process* process = CurrentProcess();
			Process* owner = process->GetPTable()->Get(sid);
			bool force = arg & 1;
			if ( owner != process && !force )
				return errno = EPERM, -1;
			if ( owner )
				owner->SetTTY(Ref<Descriptor>());
			// TODO: SIGHUP?
		}
		sid = -1;
		foreground_pgid = PID_MAX;
	}
	else if ( cmd == TIOCSPTLCK )
	{
		// TODO: Figure out what locked ptys are and implement it if sensible.
		const int* arg_ptr = (const int*) arg;
		int new_locked;
		if ( !ctx->copy_from_src(&new_locked, arg_ptr, sizeof(new_locked)) )
			return -1;
		return 0;
	}
	else if ( cmd == TIOCGPTLCK )
	{
		// TODO: Figure out what locked ptys are and implement it if sensible.
		int* arg_ptr = (int*) arg;
		int locked = 0;
		if ( !ctx->copy_to_dest(arg_ptr, &locked, sizeof(locked)) )
			return -1;
		return 0;
	}
	else if ( cmd == TIOCGNAME )
	{
		char* arg_ptr = (char*) arg;
		size_t ttynamesize = strlen(ttyname) + 1;
		if ( !ctx->copy_to_dest(arg_ptr, ttyname, ttynamesize) )
			return -1;
		return 0;
	}
	lock.Reset();
	return AbstractInode::ioctl(ctx, cmd, arg);
}

bool TTY::RequireForeground(int sig)
{
	ScopedLock family_lock(&process_family_lock);
	return RequireForegroundUnlocked(sig);
}

static bool IsOrphanedGroup(Process* group) // process_family_lock held
{
	// TODO: This can be cached in a more efficient manner.
	for ( Process* process = group->group_first;
	      process;
	      process = process->group_next )
		if ( !process->parent ||
		     (process->parent->group != group &&
		      process->parent->session == process->session) )
			return false;
	return true;
}

bool TTY::RequireForegroundUnlocked(int sig) // process_family_lock held
{
	Thread* thread = CurrentThread();
	Process* process = thread->process;
	if ( !process->session || process->session->pid != sid || !process->group )
		return true;
	if ( foreground_pgid < 1 || process->group->pid == foreground_pgid )
		return true;
	if ( sigismember(&thread->signal_mask, sig) )
		return true;
	ScopedLock signal_lock(&process->signal_lock);
	if ( process->signal_actions[sig].sa_handler == SIG_IGN )
		return true;
	signal_lock.Reset();
	// TODO: This behavior needs to be refined and os-test'd.
	if ( IsOrphanedGroup(process->group) )
	{
		if ( sig == SIGTTOU )
			return errno = EIO, false;
		return true;
	}
	process->group->DeliverGroupSignal(sig);
	errno = EINTR;
	return false;
}

} // namespace Sortix
