/*
 * Copyright (c) 2011, 2012, 2014, 2015, 2026 Jonas 'Sortie' Termansen.
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
 * kb/ps2.h
 * PS2 Keyboard.
 */

#ifndef SORTIX_KB_PS2_H
#define SORTIX_KB_PS2_H

#include <stddef.h>
#include <stdint.h>

#include <sortix/kernel/kthread.h>
#include <sortix/kernel/keyboard.h>
#include <sortix/kernel/ps2.h>

namespace Sortix {

class PS2Keyboard : public Keyboard, public PS2Device
{
public:
	PS2Keyboard();
	virtual ~PS2Keyboard();
	virtual int Read();
	virtual size_t GetPending() const;
	virtual bool HasPending() const;
	virtual void SetOwner(KeyboardOwner* owner, void* user);
	virtual bool PS2DeviceInitialize(PS2Controller* controller, uint8_t port,
                                     uint8_t* id, size_t id_size);
	virtual void PS2DeviceOnByte(uint8_t byte);

private:
	void OnKeyboardKey(int kbkey);
	void UpdateLEDs();
	bool PushKey(int key);
	int PopKey();
	void NotifyOwner();

private:
	mutable kthread_mutex_t kblock;
	int* queue;
	size_t queue_length;
	size_t queue_offset;
	size_t queue_used;
	KeyboardOwner* owner;
	void* owner_ptr;
	PS2Controller* controller;
	uint8_t port;
	enum
	{
		STATE_NORMAL = 0,
		STATE_NORMAL_ESCAPED,
	} state;
	uint8_t leds;
	uint8_t id[2];
	size_t id_size;

};

} // namespace Sortix

#endif
