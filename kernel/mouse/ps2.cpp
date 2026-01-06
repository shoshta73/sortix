/*
 * Copyright (c) 2015, 2021, 2024, 2026 Jonas 'Sortie' Termansen.
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
 * mouse/ps2.cpp
 * PS2 Mouse.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <sortix/poll.h>
#include <sortix/stat.h>

#include <sortix/kernel/ioctx.h>
#include <sortix/kernel/kernel.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/ps2.h>
#include <sortix/kernel/random.h>

#include "ps2.h"

namespace Sortix {

static const uint8_t DEVICE_RESET_OK = 0xAA;
static const uint8_t DEVICE_ECHO = 0xEE;
static const uint8_t DEVICE_ACK = 0xFA;
static const uint8_t DEVICE_RESEND = 0xFE;
static const uint8_t DEVICE_ERROR = 0xFF;

static const uint8_t DEVICE_CMD_ENABLE_SCAN = 0xF4;
static const uint8_t DEVICE_CMD_DISABLE_SCAN = 0xF5;
static const uint8_t DEVICE_CMD_IDENTIFY = 0xF2;
static const uint8_t DEVICE_CMD_RESET = 0xFF;

static const size_t DEVICE_RETRIES = 5;

PS2Mouse::PS2Mouse()
{
	queue = NULL;
	queue_length = 0;
	queue_offset = 0;
	queue_used = 0;
	owner = NULL;
	owner_ptr = NULL;
	mlock = KTHREAD_MUTEX_INITIALIZER;
}

PS2Mouse::~PS2Mouse()
{
	delete[] queue;
}

bool PS2Mouse::PS2DeviceInitialize(PS2Controller* controller, uint8_t port,
                                   uint8_t* id, size_t id_size)
{
	if ( sizeof(this->id) < id_size )
		id_size = sizeof(this->id);

	memcpy(this->id, id, id_size);
	this->id_size = id_size;

	controller->SendSync(port, DEVICE_CMD_ENABLE_SCAN);

	return true;
}

void PS2Mouse::PS2DeviceOnByte(uint8_t byte)
{
	Random::MixNow(Random::SOURCE_INPUT);
	Random::Mix(Random::SOURCE_INPUT, &byte, 1);

	ScopedLock lock(&mlock);
	PushByte(byte);
	lock.Reset();

	NotifyOwner();
}

void PS2Mouse::NotifyOwner()
{
	if ( !owner )
		return;
	owner->OnMouseByte(this, owner_ptr);
}

void PS2Mouse::SetOwner(PS2MouseDevice* owner, void* user)
{
	this->owner = owner;
	this->owner_ptr = user;
	if ( queue_used )
		NotifyOwner();
}

bool PS2Mouse::PushByte(uint8_t byte)
{
	// TODO: Just allocate a 1024 byte buffer initially?

	// Check if we need to allocate or resize the circular queue.
	if ( queue_used == queue_length )
	{
		size_t newqueue_length = queue_length ? 2 * queue_length : 32UL;
		if ( 1024 < newqueue_length )
			return false;
		uint8_t* newqueue = new uint8_t[newqueue_length];
		if ( !newqueue )
			return false;
		size_t elemsize = sizeof(*queue);
		size_t leadingavai = queue_length - queue_offset;
		size_t leading = leadingavai < queue_used ? leadingavai : queue_used;
		size_t trailing = queue_used - leading;
		if ( queue )
		{
			memcpy(newqueue, queue + queue_offset, leading * elemsize);
			memcpy(newqueue + leading, queue, trailing * elemsize);
			delete[] queue;
		}
		queue = newqueue;
		queue_length = newqueue_length;
		queue_offset = 0;
	}

	queue[(queue_offset + queue_used++) % queue_length] = byte;
	return true;
}

uint8_t PS2Mouse::PopByte()
{
	if ( !queue_used )
		return 0;
	uint8_t byte = queue[queue_offset];
	queue_offset = (queue_offset + 1) % queue_length;
	queue_used--;
	return byte;
}

uint8_t PS2Mouse::Read()
{
	ScopedLock lock(&mlock);
	return PopByte();
}

size_t PS2Mouse::GetPending() const
{
	ScopedLock lock(&mlock);
	return queue_used;
}

bool PS2Mouse::HasPending() const
{
	ScopedLock lock(&mlock);
	return queue_used;
}

PS2MouseDevice::PS2MouseDevice(dev_t dev, mode_t mode, uid_t owner, gid_t group,
                               PS2Mouse* mouse)
{
	inode_type = INODE_TYPE_TTY;
	this->dev = dev;
	this->ino = (ino_t) this;
	this->type = S_IFCHR;
	this->stat_mode = (mode & S_SETABLE) | this->type;
	this->stat_uid = owner;
	this->stat_gid = group;
	this->mouse = mouse;
	this->dev_lock = KTHREAD_MUTEX_INITIALIZER;
	this->data_cond = KTHREAD_COND_INITIALIZER;
	mouse->SetOwner(this, NULL);
}

PS2MouseDevice::~PS2MouseDevice()
{
	delete mouse;
}

ssize_t PS2MouseDevice::read(ioctx_t* ctx, uint8_t* buf, size_t count)
{
	ScopedLockSignal lock(&dev_lock);
	if ( !lock.IsAcquired() )
		return errno = EINTR, -1;
	// TODO: The data should be copied more efficiently to userspace.
	size_t sofar = 0;
	while ( sofar < count )
	{
		if ( !mouse->HasPending() )
		{
			if ( sofar != 0 )
				break;
			do
			{
				if ( !kthread_cond_wait_signal(&data_cond, &dev_lock) )
					return sofar ? sofar : -1;
			} while ( !mouse->HasPending() );
		}
		uint8_t byte = mouse->Read();
		if ( !ctx->copy_to_dest(buf + sofar, &byte, 1) )
			return sofar ? sofar : -1;
		sofar++;
	}
	return (ssize_t) sofar;
}

short PS2MouseDevice::PollEventStatus()
{
	short status = 0;
	if ( mouse->HasPending() )
		status |= POLLIN | POLLRDNORM;
	return status;
}

int PS2MouseDevice::poll(ioctx_t* /*ctx*/, PollNode* node)
{
	ScopedLock lock(&dev_lock);
	short ret_status = PollEventStatus() & node->events;
	if ( ret_status )
	{
		node->master->revents |= ret_status;
		return 0;
	}
	poll_channel.Register(node);
	return errno = EAGAIN, -1;
}

void PS2MouseDevice::OnMouseByte(PS2Mouse* /*mouse*/, void* /*user*/)
{
	ScopedLock lock(&dev_lock);
	poll_channel.Signal(PollEventStatus());
	kthread_cond_signal(&data_cond);
}

} // namespace Sortix
