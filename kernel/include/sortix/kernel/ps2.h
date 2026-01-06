/*
 * Copyright (c) 2015, 2026 Jonas 'Sortie' Termansen.
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
 * sortix/kernel/ps2.h
 * Various interfaces for keyboard devices and layouts.
 */

#ifndef _INCLUDE_SORTIX_KERNEL_PS2_H
#define _INCLUDE_SORTIX_KERNEL_PS2_H

#include <stdint.h>

#include <sortix/kernel/timer.h>
#include <sortix/kernel/kthread.h>

namespace Sortix {

class PS2Controller;
class PS2Device;

class PS2Controller
{
public:
	PS2Controller();
	bool Init(PS2Device* keyboard, PS2Device* mouse);
	bool Send(uint8_t port, uint8_t byte);
	bool SendSync(uint8_t port, uint8_t byte, uint8_t* answer = NULL);
	bool ReadSync(uint8_t port, uint8_t* byte);
	bool DetectDevice(uint8_t port, uint8_t* response, size_t* response_size);
	void OnPortByte(uint8_t port, uint8_t byte);

private:
	kthread_mutex_t ps2_lock;
	bool dual;
	PS2Device* devices[2];

};

class PS2Device
{
public:
	virtual ~PS2Device() { }
	virtual bool PS2DeviceInitialize(PS2Controller* controller, uint8_t port,
                                     uint8_t* id, size_t id_size) = 0;
	virtual void PS2DeviceOnByte(uint8_t byte) = 0;

};

} // namespace Sortix

#endif
