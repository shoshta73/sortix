/*
 * Copyright (c) 2011, 2012, 2014, 2015, 2024, 2026 Jonas 'Sortie' Termansen.
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
#include <sortix/kernel/random.h>

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

PS2Keyboard::PS2Keyboard(uint8_t scancode_set)
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
	this->scancode_set = scancode_set;
	fingerprint_used = 0;
}

PS2Keyboard::~PS2Keyboard()
{
	delete[] queue;
}

bool PS2Keyboard::PS2DeviceInitialize(PS2Controller* controller, uint8_t port,
                                      uint8_t* id, size_t id_size)
{
	uint8_t byte;

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

	// Translation is supposed to be disabled, but the device identity was
	// translated from 0xAB 0x83 to 0xAB 0x41, which means either:
	// 1) The device does not support scancode set 2.
	// 2) The controller is buggy and might translate from set 1 to 2 anyway.
	//    But on real hardware, it also might misidentify and send set 2.
	// We have no idea what scancode set we're actually getting, so we have to
	// fingerprint the byte sequences we receive instead.
	if ( scancode_set == 0 && id_size == 2 && id[0] == 0xAB && id[1] == 0x41 )
		scancode_set = 4;
	else
	{
		// Try setting scancode set 2.
		if ( (scancode_set == 0 || scancode_set == 2) &&
			 controller->SendSync(port, DEVICE_CMD_SCANCODE_SET) )
			 controller->SendSync(port, 2);
		// Query the scancode set to see if it was set.
		if ( scancode_set == 0 &&
			 controller->SendSync(port, DEVICE_CMD_SCANCODE_SET) &&
			 controller->SendSync(port, 0) &&
			 controller->ReadSync(port, &byte) &&
			 (byte == 0x02 || byte == 0x41) )
			scancode_set = 2;
		// Otherwise try to set scancode set 1.
		if ( (scancode_set == 0 || scancode_set == 1) &&
			 controller->SendSync(port, DEVICE_CMD_SCANCODE_SET) )
			 controller->SendSync(port, 1);
		// Query the scancode set to see if it was set.
		if ( scancode_set == 0 &&
			 controller->SendSync(port, DEVICE_CMD_SCANCODE_SET) &&
			 controller->SendSync(port, 0) &&
			 controller->ReadSync(port, &byte) &&
			 (byte == 0x01 || byte == 0x43) )
			scancode_set = 1;
	}
	// If we failed to find the scancode set, we have to fingerprint.
	if ( scancode_set == 0 || scancode_set == 4 )
		scancode_set = 4;

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

	if ( OnKeyboardByte(byte) )
	{
		lock.Reset();
		NotifyOwner();
	}
}

bool PS2Keyboard::OnKeyboardByte(uint8_t byte)  // Locked: ps2_lock, kblock
{
	int kbkey = 0;

	if ( scancode_set == 1 )
	{
		if ( state == STATE_NORMAL && byte == DEVICE_SCANCODE_ESCAPE )
			state = STATE_NORMAL_ESCAPED;
		else if ( state == STATE_NORMAL )
		{
			kbkey = byte & 0x7F;
			kbkey = byte & 0x80 ? -kbkey : kbkey;
		}
		else if ( state == STATE_NORMAL_ESCAPED )
		{
			state = STATE_NORMAL;
			kbkey = (byte & 0x7F) + 0x80;
			kbkey = byte & 0x80 ? -kbkey : kbkey;
		}
	}
	else if ( scancode_set == 2 )
	{
		// TODO: Implement the pause key and print screen and other specials.
		if ( state == STATE_NORMAL && byte == DEVICE_SCANCODE_ESCAPE )
			state = STATE_NORMAL_ESCAPED;
		else if ( state == STATE_NORMAL && byte == DEVICE_SCANCODE_RELEASE )
			state = STATE_NORMAL_RELEASED;
		else if ( state == STATE_NORMAL_ESCAPED &&
		          byte == DEVICE_SCANCODE_RELEASE )
			state = STATE_NORMAL_ESCAPED_RELEASED;
		else if ( state == STATE_NORMAL )
			kbkey = unescaped_translation[byte];
		else if ( state == STATE_NORMAL_RELEASED )
		{
			kbkey = -unescaped_translation[byte];
			state = STATE_NORMAL;
		}
		else if ( state == STATE_NORMAL_ESCAPED )
		{
			kbkey = escaped_translation[byte];
			state = STATE_NORMAL;
		}
		else if ( state == STATE_NORMAL_ESCAPED_RELEASED )
		{
			kbkey = -escaped_translation[byte];
			state = STATE_NORMAL;
		}
	}
	else if ( scancode_set == 4 )
	{
		// We need to study the input bytes to detect the scancode set in use.
		// The 0xF0 byte appears only in a scancode set 2 key release sequence.
		// We can fingerprint scancode set 2 when any key is released.
		if ( byte == DEVICE_SCANCODE_RELEASE )
			scancode_set = 2;
		// The 0xE0 escape byte can happen in both scansets.
		else if ( byte == DEVICE_SCANCODE_ESCAPE )
			state = STATE_NORMAL_ESCAPED;
		// The 0xE1 escape byte can happen in both scansets, but the next bytes
		// will determine the scanset.
		else if ( byte == 0xE1 )
			state = STATE_NORMAL;
		// 0x83 means the '2' key was released in scancode set 1, but for some
		// reason it also means the 'F7' key was pressed in scancode set 2. We
		// unfortunately cannot tell these cases apart. The 0x03 byte means the
		// 'F5' key was pressed in scancode set 2, so the 0x03 0x83 sequence
		// either means '2' was pressed and released, or 'F5' and 'F7' were both
		// pressed (and not released yet).
		else if ( state == STATE_NORMAL && byte == 0x83 )
		{
			// 'F5'+'F7' is very rare and '2' is very common. It is better to
			// assume scancode set 1 instead of confusing a user on scancode set
			// 1 that pressed and released '2' without anything happening, even
			// though the 'F5'+'F7' combo would have a wrong result on scancode
			// set 1. Check if the '2' key had already been pressed.
			for ( size_t i = 0; i < fingerprint_used; i++ )
			{
				if ( fingerprint[i] == DEVICE_SCANCODE_ESCAPE )
					i += 1;
				else if ( fingerprint[i] == 0xE1 )
					i += 2;
				else if ( fingerprint[i] == 0x03 /* '2' Key */ )
				{
					scancode_set = 1;
					break;
				}
			}
		}
		// Otherwise if the high bit is set, the byte is a key release sequence
		// from scancode set 1. This bit is never set in scancode set 2 in any
		// other case than the above cases. We can fingerprint scancode set 1
		// when any key (except '2') is released.
		else if ( byte & 0x80 )
			scancode_set = 1;
		// There are many gaps in scancode set 2, if the byte doesn't have any
		// known keys associated with it, it must be scancode set 1.
		else if ( state == STATE_NORMAL_ESCAPED )
		{
			// TODO: There are a number of multimedia keys that aren't supported
			//       yet, which currently are false gaps in the escaped
			//       translation and pressing them would cause the wrong scanset
			//       to be determined.
			//if ( !escaped_translation[byte] )
			//	scancode_set = 1;
			state = STATE_NORMAL;
		}
		else
		{
			if ( !unescaped_translation[byte] )
				scancode_set = 1;
		}
		if ( scancode_set != 5 )
		{
			state = STATE_NORMAL;
			int any = 0;
			for ( size_t i = 0; i < fingerprint_used; i++ )
				any |= OnKeyboardByte(fingerprint[i]);
			any |= OnKeyboardByte(byte);
			return any;
		}
		else
		{
			// If somehow too much data was collected and a key hasn't been
			// released yet, discard the first key press and continue.
			if ( fingerprint_used == sizeof(fingerprint) )
			{
				size_t skip = fingerprint[0] == 0xE1 ? 3 :
				              fingerprint[0] == DEVICE_SCANCODE_ESCAPE ? 2 : 1;
				memmove(fingerprint, fingerprint + skip,
				        sizeof(fingerprint) - skip);
				fingerprint_used -= skip;
			}
			fingerprint[fingerprint_used++] = byte;
		}
	}

	if ( kbkey )
	{
		OnKeyboardKey(kbkey);
		return true;
	}

	return false;
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
