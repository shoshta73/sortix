/*
 * Copyright (c) 2011, 2012, 2014, 2015 Jonas 'Sortie' Termansen.
 * Copyright (c) 2022 Juhani 'nortti' Krekelä.
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

#include "ps2.h"

namespace Sortix {

static const uint8_t DEVICE_RESET_OK = 0xAA;
static const uint8_t DEVICE_SCANCODE_ESCAPE = 0xE0;
static const uint8_t DEVICE_SCANCODE_RELEASE = 0xF0;
static const uint8_t DEVICE_ECHO = 0xEE;
static const uint8_t DEVICE_ACK = 0xFA;
static const uint8_t DEVICE_RESEND = 0xFE;
static const uint8_t DEVICE_ERROR = 0xFF;

static const uint8_t DEVICE_CMD_SET_LED = 0xED;
static const uint8_t DEVICE_CMD_SCANCODE_SET = 0xF0;
static const uint8_t DEVICE_CMD_SET_TYPEMATIC = 0xF3;
static const uint8_t DEVICE_CMD_ENABLE_SCAN = 0xF4;
static const uint8_t DEVICE_CMD_DISABLE_SCAN = 0xF5;
static const uint8_t DEVICE_CMD_IDENTIFY = 0xF2;
static const uint8_t DEVICE_CMD_RESET = 0xFF;

static const uint8_t DEVICE_LED_SCRLCK = 1 << 0;
static const uint8_t DEVICE_LED_NUMLCK = 1 << 1;
static const uint8_t DEVICE_LED_CAPSLCK = 1 << 2;

static const size_t DEVICE_RETRIES = 5;

static const unsigned char unescaped_translation[256] =
{
	// 0x00
	0,
	KBKEY_F9,
	0,
	KBKEY_F5,
	KBKEY_F3,
	KBKEY_F1,
	KBKEY_F2,
	KBKEY_F12,
	0,
	KBKEY_F10,
	KBKEY_F8,
	KBKEY_F6,
	KBKEY_F4,
	KBKEY_TAB,
	KBKEY_SYM7,
	0,
	// 0x10
	0,
	KBKEY_LALT,
	KBKEY_LSHIFT,
	0,
	KBKEY_LCTRL,
	KBKEY_Q,
	KBKEY_NUM1,
	0, 0, 0,
	KBKEY_Z,
	KBKEY_S,
	KBKEY_A,
	KBKEY_W,
	KBKEY_NUM2,
	0,
	// 0x20
	0,
	KBKEY_C,
	KBKEY_X,
	KBKEY_D,
	KBKEY_E,
	KBKEY_NUM4,
	KBKEY_NUM3,
	0, 0,
	KBKEY_SPACE,
	KBKEY_V,
	KBKEY_F,
	KBKEY_T,
	KBKEY_R,
	KBKEY_NUM5,
	0,
	// 0x30
	0,
	KBKEY_N,
	KBKEY_B,
	KBKEY_H,
	KBKEY_G,
	KBKEY_Y,
	KBKEY_NUM6,
	0, 0, 0,
	KBKEY_M,
	KBKEY_J,
	KBKEY_U,
	KBKEY_NUM7,
	KBKEY_NUM8,
	0,
	// 0x40
	0,
	KBKEY_SYM9,
	KBKEY_K,
	KBKEY_I,
	KBKEY_O,
	KBKEY_NUM0,
	KBKEY_NUM9,
	0, 0,
	KBKEY_SYM10,
	KBKEY_SYM11,
	KBKEY_L,
	KBKEY_SYM5,
	KBKEY_P,
	KBKEY_SYM1,
	0,
	// 0x50
	0, 0,
	KBKEY_SYM6,
	0,
	KBKEY_SYM3,
	KBKEY_SYM2,
	0, 0,
	KBKEY_CAPSLOCK,
	KBKEY_RSHIFT,
	KBKEY_ENTER,
	KBKEY_SYM4,
	0,
	KBKEY_SYM8,
	0,
	0,
	// 0x60
	0,
	KBKEY_NO_STANDARD_MEANING_2,
	0, 0, 0, 0,
	KBKEY_BKSPC,
	0, 0,
	KBKEY_KPAD1,
	0,
	KBKEY_KPAD4,
	KBKEY_KPAD7,
	0, 0, 0,
	// 0x70
	KBKEY_KPAD0,
	KBKEY_SYM15,
	KBKEY_KPAD2,
	KBKEY_KPAD5,
	KBKEY_KPAD6,
	KBKEY_KPAD8,
	KBKEY_ESC,
	KBKEY_NUMLOCK,
	KBKEY_F11,
	KBKEY_SYM14,
	KBKEY_KPAD3,
	KBKEY_SYM13,
	KBKEY_SYM12,
	KBKEY_KPAD9,
	KBKEY_SCROLLLOCK,
	0,
	// 0x80
	0, 0, 0,
	KBKEY_F7,
};

static const unsigned char escaped_translation[256] =
{
	// 0xE0 0x00
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	// 0xE0 0x10
	0,
	KBKEY_RALT,
	0, 0,
	KBKEY_RCTRL,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	KBKEY_LSUPER,
	// 0xE0 0x20
	0, 0, 0, 0, 0, 0, 0,
	KBKEY_RSUPER,
	0, 0, 0, 0, 0, 0, 0,
	KBKEY_MENU,
	// 0xE0 0x30
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	// 0xE0 0x40
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	KBKEY_SYM16,
	0, 0, 0, 0, 0,
	// 0xE0 0x50
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	KBKEY_KPADENTER,
	0, 0, 0, 0, 0,
	// 0xE0 0x60
	0, 0, 0, 0, 0, 0, 0, 0, 0,
	KBKEY_END,
	0,
	KBKEY_LEFT,
	KBKEY_HOME,
	0, 0, 0,
	// 0xE0 0x70
	KBKEY_INSERT,
	KBKEY_DELETE,
	KBKEY_DOWN,
	0,
	KBKEY_RIGHT,
	KBKEY_UP,
	0, 0, 0, 0,
	KBKEY_PGDOWN,
	0, 0,
	KBKEY_PGUP,
};

PS2Keyboard::PS2Keyboard()
{
	this->queue = NULL;
	this->queuelength = 0;
	this->queueoffset = 0;
	this->queueused = 0;
	this->owner = NULL;
	this->ownerptr = NULL;
	// TODO: Initial LED status can be read from the BIOS data area. If so, we
	//       need to emulate fake presses of the modifier keys to keep the
	//       keyboard layout in sync.
	this->leds = 0;
	this->kblock = KTHREAD_MUTEX_INITIALIZER;
}

PS2Keyboard::~PS2Keyboard()
{
	delete[] queue;
}

void PS2Keyboard::PS2DeviceInitialize(void* send_ctx, bool (*send)(void*, uint8_t),
                                      uint8_t* id, size_t id_size)
{
	if ( sizeof(this->id) < id_size )
		id_size = sizeof(this->id);
	this->send_ctx = send_ctx;
	this->send = send;
	this->state = STATE_INIT;
	this->tries = 0;
	memcpy(this->id, id, id_size);
	this->id_size = id_size;
	PS2DeviceOnByte(DEVICE_RESEND);
}

void PS2Keyboard::PS2DeviceOnByte(uint8_t byte)
{
	ScopedLock lock(&kblock);

	if ( state == STATE_INIT )
	{
		state = STATE_RESET_LED;
		tries = DEVICE_RETRIES;
		byte = DEVICE_RESEND;
	}

	if ( state == STATE_RESET_LED )
	{
		if ( byte == DEVICE_RESEND && tries-- )
		{
			if ( send(send_ctx, DEVICE_CMD_SET_LED) &&
			     send(send_ctx, leds & 0x07) )
				return;
		}
		state = STATE_RESET_TYPEMATIC;
		tries = DEVICE_RETRIES;
		byte = DEVICE_RESEND;
	}

	if ( state == STATE_RESET_TYPEMATIC )
	{
		if ( byte == DEVICE_RESEND && tries-- )
		{
			uint8_t rate = 0b00000; // 33.36 ms/repeat.
			uint8_t delay = 0b01; // 500 ms.
			uint8_t typematic = delay << 3 | rate << 0;
			if ( send(send_ctx, DEVICE_CMD_SET_TYPEMATIC) &&
			     send(send_ctx, typematic) )
				return;
		}
		state = STATE_SET_SCANCODE_SET;
		tries = DEVICE_RETRIES;
		byte = DEVICE_RESEND;
	}

	if ( state == STATE_SET_SCANCODE_SET )
	{
		if ( byte == DEVICE_RESEND && tries-- )
		{
			if ( send(send_ctx, DEVICE_CMD_SCANCODE_SET) &&
			     send(send_ctx, 2) )
				return;
		}
		state = STATE_ENABLE_SCAN;
		tries = DEVICE_RETRIES;
		byte = DEVICE_RESEND;
	}

	if ( state == STATE_ENABLE_SCAN )
	{
		if ( byte == DEVICE_RESEND && tries-- )
		{
			if ( send(send_ctx, DEVICE_CMD_ENABLE_SCAN) )
				return;
		}
		state = STATE_NORMAL;
		tries = DEVICE_RETRIES;
		byte = DEVICE_RESEND;
	}

	if ( byte == DEVICE_RESEND || byte == DEVICE_ACK )
		return;

	if ( state == STATE_NORMAL && byte == DEVICE_SCANCODE_ESCAPE )
	{
		state = STATE_NORMAL_ESCAPED;
		return;
	}
	else if ( state == STATE_NORMAL && byte == DEVICE_SCANCODE_RELEASE)
	{
		state = STATE_NORMAL_RELEASED;
		return;
	}
	else if ( state == STATE_NORMAL_ESCAPED && byte == DEVICE_SCANCODE_RELEASE )
	{
		state = STATE_NORMAL_ESCAPED_RELEASED;
		return;
	}

	if ( state == STATE_NORMAL ||
	     state == STATE_NORMAL_RELEASED ||
	     state == STATE_NORMAL_ESCAPED ||
	     state == STATE_NORMAL_ESCAPED_RELEASED )
	{
		switch ( state )
		{
		case STATE_NORMAL:
			OnKeyboardKey(unescaped_translation[byte]);
			break;
		case STATE_NORMAL_RELEASED:
			OnKeyboardKey(-unescaped_translation[byte]);
			break;
		case STATE_NORMAL_ESCAPED:
			OnKeyboardKey(escaped_translation[byte]);
			break;
		case STATE_NORMAL_ESCAPED_RELEASED:
			OnKeyboardKey(-escaped_translation[byte]);
			break;
		}
		state = STATE_NORMAL;
		lock.Reset();
		NotifyOwner();
		return;
	}
}

void PS2Keyboard::OnKeyboardKey(int kbkey)
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
		UpdateLEDs(leds = newleds);
}

void PS2Keyboard::NotifyOwner()
{
	if ( !owner )
		return;
	owner->OnKeystroke(this, ownerptr);
}

void PS2Keyboard::UpdateLEDs(int ledval)
{
	send(send_ctx, DEVICE_CMD_SET_LED) &&
	send(send_ctx, ledval);
}

void PS2Keyboard::SetOwner(KeyboardOwner* owner, void* user)
{
	kthread_mutex_lock(&kblock);
	this->owner = owner;
	this->ownerptr = user;
	kthread_mutex_unlock(&kblock);
	if ( queueused )
		NotifyOwner();
}

bool PS2Keyboard::PushKey(int key)
{
	// Check if we need to allocate or resize the circular queue.
	if ( queueused == queuelength )
	{
		size_t newqueuelength = queuelength ? 2 * queuelength : 32UL;
		if ( 16 * 1024 < newqueuelength )
			return false;
		int* newqueue = new int[newqueuelength];
		if ( !newqueue )
			return false;
		if ( queue )
		{
			size_t elemsize = sizeof(*queue);
			size_t leadingavai = queuelength - queueoffset;
			size_t leading = leadingavai < queueused ? leadingavai : queueused;
			size_t trailing = queueused - leading;
			memcpy(newqueue, queue + queueoffset, leading * elemsize);
			memcpy(newqueue + leading, queue, trailing * elemsize);
			delete[] queue;
		}
		queue = newqueue;
		queuelength = newqueuelength;
		queueoffset = 0;
	}

	queue[(queueoffset + queueused++) % queuelength] = key;
	return true;
}

int PS2Keyboard::PopKey()
{
	if ( !queueused )
		return 0;
	int kbkey = queue[queueoffset];
	queueoffset = (queueoffset + 1) % queuelength;
	queueused--;
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
	return queueused;
}

bool PS2Keyboard::HasPending() const
{
	ScopedLock lock(&kblock);
	return queueused;
}

} // namespace Sortix
