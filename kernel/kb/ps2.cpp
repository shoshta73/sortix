/*
 * Copyright (c) 2011, 2012, 2014, 2015, 2024, 2026 Jonas 'Sortie' Termansen.
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
 * kb/ps2.cpp
 * PS2 Keyboard.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <sortix/keycodes.h>

#include <sortix/kernel/kernel.h>
#include <sortix/kernel/keyboard.h>
#include <sortix/kernel/ps2.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/random.h>

#include "ps2.h"

// TODO: This driver doesn't deal with keyboard scancode sets yet.

namespace Sortix {

static const uint8_t DEVICE_RESET_OK = 0xAA;
static const uint8_t DEVICE_SCANCODE_ESCAPE = 0xE0;
static const uint8_t DEVICE_ECHO = 0xEE;
static const uint8_t DEVICE_ACK = 0xFA;
static const uint8_t DEVICE_RESEND = 0xFE;
static const uint8_t DEVICE_ERROR = 0xFF;

static const uint8_t DEVICE_CMD_SET_LED = 0xED;
static const uint8_t DEVICE_CMD_SET_TYPEMATIC = 0xF3;
static const uint8_t DEVICE_CMD_ENABLE_SCAN = 0xF4;
static const uint8_t DEVICE_CMD_DISABLE_SCAN = 0xF5;
static const uint8_t DEVICE_CMD_IDENTIFY = 0xF2;
static const uint8_t DEVICE_CMD_RESET = 0xFF;

static const uint8_t DEVICE_LED_SCRLCK = 1 << 0;
static const uint8_t DEVICE_LED_NUMLCK = 1 << 1;
static const uint8_t DEVICE_LED_CAPSLCK = 1 << 2;

static const size_t DEVICE_RETRIES = 5;

PS2Keyboard::PS2Keyboard()
{
	queue = NULL;
	queue_length = 0;
	queue_offset = 0;
	queue_used = 0;
	owner = NULL;
	owner_ptr = NULL;
	// TODO: Initial LED status can be read from the BIOS data area. If so, we
	//       need to emulate fake presses of the modifier keys to keep the
	//       keyboard layout in sync.
	leds = 0;
	kblock = KTHREAD_MUTEX_INITIALIZER;
	state = STATE_NORMAL;
}

PS2Keyboard::~PS2Keyboard()
{
	delete[] queue;
}

bool PS2Keyboard::PS2DeviceInitialize(PS2Controller* controller, uint8_t port,
                                      uint8_t* id, size_t id_size)
{
	if ( sizeof(this->id) < id_size )
		id_size = sizeof(this->id);
	this->controller = controller;
	this->port = port;
	memcpy(this->id, id, id_size);
	this->id_size = id_size;

	if ( controller->SendSync(port, DEVICE_CMD_SET_LED) )
		controller->SendSync(port, leds & 0x07);

	uint8_t rate = 0b00000; // 33.36 ms/repeat.
	uint8_t delay = 0b01; // 500 ms.
	uint8_t typematic = delay << 3 | rate << 0;
	if ( controller->SendSync(port, DEVICE_CMD_SET_TYPEMATIC) )
		controller->SendSync(port, typematic);

	controller->SendSync(port, DEVICE_CMD_ENABLE_SCAN);

	return true;
}

void PS2Keyboard::PS2DeviceOnByte(uint8_t byte) // Locked: ps2_lock
{
	Random::MixNow(Random::SOURCE_INPUT);
	Random::Mix(Random::SOURCE_INPUT, &byte, 1);

	ScopedLock lock(&kblock);

	if ( byte == DEVICE_RESEND || byte == DEVICE_ACK )
		return;

	if ( byte == DEVICE_SCANCODE_ESCAPE )
	{
		state = STATE_NORMAL_ESCAPED;
		return;
	}

	if ( state == STATE_NORMAL )
	{
		int kbkey = byte & 0x7F;
		OnKeyboardKey(byte & 0x80 ? -kbkey : kbkey);
		lock.Reset();
		NotifyOwner();
		return;
	}

	if ( state == STATE_NORMAL_ESCAPED )
	{
		state = STATE_NORMAL;
		int kbkey = (byte & 0x7F) + 0x80;
		OnKeyboardKey(byte & 0x80 ? -kbkey : kbkey);
		lock.Reset();
		NotifyOwner();
		return;
	}
}

void PS2Keyboard::OnKeyboardKey(int kbkey) // Locked: ps2_lock, kblock
{
	if ( !PushKey(kbkey) )
		return;

	uint8_t newleds = leds;

	if ( kbkey == KBKEY_CAPSLOCK )
		newleds ^= DEVICE_LED_CAPSLCK;
	if ( kbkey == KBKEY_SCROLLLOCK )
		newleds ^= DEVICE_LED_SCRLCK;
	if ( kbkey == KBKEY_NUMLOCK )
		newleds ^= DEVICE_LED_NUMLCK;

	if ( newleds != leds )
	{
		leds = newleds;
		UpdateLEDs();
	}
}

void PS2Keyboard::NotifyOwner() // Locked: ps2_lock
{
	if ( !owner )
		return;
	owner->OnKeystroke(this, owner_ptr);
}

void PS2Keyboard::UpdateLEDs() // Locked: ps2_lock, kblock
{
	if ( controller->Send(port, DEVICE_CMD_SET_LED) )
		controller->Send(port, leds & 0x07);
}

void PS2Keyboard::SetOwner(KeyboardOwner* owner, void* user)
{
	ScopedLock lock(&kblock);
	this->owner = owner;
	this->owner_ptr = user;
	lock.Reset();
	if ( queue_used )
		NotifyOwner();
}

bool PS2Keyboard::PushKey(int key) // Locked: ps2_lock, kblock
{
	// Check if we need to allocate or resize the circular queue.
	if ( queue_used == queue_length )
	{
		size_t new_queue_length = queue_length ? 2 * queue_length : 32UL;
		if ( 16 * 1024 < new_queue_length )
			return false;
		int* new_queue = new int[new_queue_length];
		if ( !new_queue )
			return false;
		if ( queue )
		{
			size_t element_size = sizeof(*queue);
			size_t available = queue_length - queue_offset;
			size_t leading = available < queue_used ? available : queue_used;
			size_t trailing = queue_used - leading;
			memcpy(new_queue, queue + queue_offset, leading * element_size);
			memcpy(new_queue + leading, queue, trailing * element_size);
			delete[] queue;
		}
		queue = new_queue;
		queue_length = new_queue_length;
		queue_offset = 0;
	}

	queue[(queue_offset + queue_used++) % queue_length] = key;
	return true;
}

int PS2Keyboard::PopKey() // Locked: kblock
{
	if ( !queue_used )
		return 0;
	int kbkey = queue[queue_offset];
	queue_offset = (queue_offset + 1) % queue_length;
	queue_used--;
	return kbkey;
}

int PS2Keyboard::Read()
{
	ScopedLock lock(&kblock);
	return PopKey();
}

size_t PS2Keyboard::GetPending() const
{
	ScopedLock lock(&kblock);
	return queue_used;
}

bool PS2Keyboard::HasPending() const
{
	ScopedLock lock(&kblock);
	return queue_used;
}

} // namespace Sortix
