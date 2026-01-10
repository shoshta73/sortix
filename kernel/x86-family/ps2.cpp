/*
 * Copyright (c) 2015, 2017, 2026 Jonas 'Sortie' Termansen.
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
 * x86-family/ps2.cpp
 * 8042 PS/2 Controller.
 */

#include <assert.h>
#include <stdint.h>

#include <sortix/clock.h>

#include <sortix/kernel/clock.h>
#include <sortix/kernel/interrupt.h>
#include <sortix/kernel/ioport.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/log.h>
#include <sortix/kernel/panic.h>
#include <sortix/kernel/ps2.h>
#include <sortix/kernel/time.h>
#include <sortix/kernel/timer.h>

#include "ps2.h"

namespace Sortix {
namespace PS2 {

static const uint16_t REG_DATA    = 0x0060;
static const uint16_t REG_COMMAND = 0x0064;
static const uint16_t REG_STATUS  = 0x0064;

static const uint8_t REG_COMMAND_READ_RAM            = 0x20;
static const uint8_t REG_COMMAND_WRITE_RAM           = 0x60;
static const uint8_t REG_COMMAND_DISABLE_SECOND_PORT = 0xA7;
static const uint8_t REG_COMMAND_ENABLE_SECOND_PORT  = 0xA8;
static const uint8_t REG_COMMAND_TEST_SECOND_PORT    = 0xA9;
static const uint8_t REG_COMMAND_TEST_CONTROLLER     = 0xAA;
static const uint8_t REG_COMMAND_TEST_FIRST_PORT     = 0xAB;
static const uint8_t REG_COMMAND_DISABLE_FIRST_PORT  = 0xAD;
static const uint8_t REG_COMMAND_ENABLE_FIRST_PORT   = 0xAE;
static const uint8_t REG_COMMAND_ECHO_PORT_1         = 0xD2;
static const uint8_t REG_COMMAND_ECHO_PORT_2         = 0xD3;
static const uint8_t REG_COMMAND_SEND_TO_PORT_2      = 0xD4;

static const uint8_t REG_STATUS_OUTPUT   = 1 << 0;
static const uint8_t REG_STATUS_INPUT    = 1 << 1;
static const uint8_t REG_STATUS_SYSTEM   = 1 << 2;
static const uint8_t REG_STATUS_COMMAND  = 1 << 3;
static const uint8_t REG_STATUS_UNKNOWN1 = 1 << 4;
static const uint8_t REG_STATUS_UNKNOWN2 = 1 << 5;
static const uint8_t REG_STATUS_TIMEOUT  = 1 << 6;
static const uint8_t REG_STATUS_PARITY   = 1 << 7;

static const uint8_t REG_CONFIG_FIRST_INTERRUPT   = 1 << 0;
static const uint8_t REG_CONFIG_SECOND_INTERRUPT  = 1 << 1;
static const uint8_t REG_CONFIG_SYSTEM            = 1 << 2;
static const uint8_t REG_CONFIG_ZERO1             = 1 << 3;
static const uint8_t REG_CONFIG_NO_FIRST_CLOCK    = 1 << 4;
static const uint8_t REG_CONFIG_NO_SECOND_CLOCK   = 1 << 5;
static const uint8_t REG_CONFIG_FIRST_TRANSLATION = 1 << 6;
static const uint8_t REG_CONFIG_ZERO2             = 1 << 7;

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
static const size_t DEVICE_MAX_UNRELATED = 1000;

// The 50 ms timeout was required on sortie's 2020 desktop.
// TODO: Measure the actual delay on that machine.
static const unsigned int TIMEOUT_MS = 50;

static bool WaitInput()
{
	return wait_inport8_clear(REG_STATUS, REG_STATUS_INPUT, false, TIMEOUT_MS);
}

static bool WaitOutput()
{
	return wait_inport8_set(REG_STATUS, REG_STATUS_OUTPUT, false, TIMEOUT_MS);
}

static bool TryReadByte(uint8_t* result)
{
	if ( !WaitOutput() )
		return false;
	*result = inport8(REG_DATA);
	return true;
}

static bool TryWriteByte(uint8_t byte)
{
	if ( !WaitInput() )
		return false;
	outport8(REG_DATA, byte);
	return true;
}

static bool TryWriteCommand(uint8_t byte)
{
	if ( !WaitInput() )
		return false;
	outport8(REG_COMMAND, byte);
	return true;
}

static bool TryWriteToPort(uint8_t byte, uint8_t port)
{
	if ( port == 2 && !TryWriteCommand(REG_COMMAND_SEND_TO_PORT_2) )
		return false;
	return TryWriteByte(byte);
}

static bool IsKeyboardResponse(uint8_t* response, size_t size)
{
	// Original AT keyboards do not identify themselves.
	if ( size == 0 )
		return true;
	// "Standard" PS/2 keyboards reply AB 83 or AB C1.
	// If translation is enabled, AB 83 becomes AB 41 and AB C1 stays as-in
	if ( size == 2 && response[0] == 0xAB && response[1] == 0x83 )
		return true;
	if ( size == 2 && response[0] == 0xAB && response[1] == 0x41 )
		return true;
	if ( size == 2 && response[0] == 0xAB && response[1] == 0xC1 )
		return true;
	// "Compact" PS/2 keyboards reply AB 84.
	// If translation is enabled, AB 84 becomes AB 54.
	if ( size == 2 && response[0] == 0xAB && response[1] == 0x84 )
		return true;
	if ( size == 2 && response[0] == 0xAB && response[1] == 0x54 )
		return true;
	return false;
}

static bool IsMouseResponse(uint8_t* response, size_t size)
{
	if ( size == 1 && response[0] == 0x00 )
		return true;
	if ( size == 1 && response[0] == 0x03 )
		return true;
	if ( size == 1 && response[0] == 0x04 )
		return true;
	return false;
}

static void IRQ1Work(void* /*context*/);
static void IRQ12Work(void* /*context*/);

static struct interrupt_handler irq1_registration;
static struct interrupt_handler irq12_registration;
static struct interrupt_work irq1_work = { NULL, IRQ1Work, NULL };
static struct interrupt_work irq12_work = { NULL, IRQ12Work, NULL };
static PS2Controller* ps2_controller;
static unsigned char irq1_buffer[128];
static unsigned char irq12_buffer[128];
static size_t irq1_offset;
static size_t irq12_offset;
static size_t irq1_used;
static size_t irq12_used;
static bool irq1_working;
static bool irq12_working;

static void IRQ1Work(void* /*context*/)
{
	Interrupt::Disable();
	size_t todo = irq1_used;
	for ( size_t i = 0; i < todo; i++ )
	{
		unsigned char byte = irq1_buffer[irq1_offset++];
		if ( sizeof(irq1_buffer) <= irq1_offset )
			irq1_offset = 0;
		irq1_used--;
		Interrupt::Enable();
		ps2_controller->OnPortByte(1, byte);
		Interrupt::Disable();
	}
	if ( irq1_used )
		Interrupt::ScheduleWork(&irq1_work);
	else
		irq1_working = false;
	Interrupt::Enable();
}

static void IRQ12Work(void* /*context*/)
{
	Interrupt::Disable();
	size_t todo = irq12_used;
	for ( size_t i = 0; i < todo; i++ )
	{
		unsigned char byte = irq12_buffer[irq12_offset++];
		if ( sizeof(irq12_buffer) <= irq12_offset )
			irq12_offset = 0;
		irq12_used--;
		Interrupt::Enable();
		ps2_controller->OnPortByte(2, byte);
		Interrupt::Disable();
	}
	if ( irq12_used )
		Interrupt::ScheduleWork(&irq12_work);
	else
		irq12_working = false;
	Interrupt::Enable();
}

static void OnIRQ1(struct interrupt_context* /*intctx*/, void* /*user*/)
{
	if ( inport8(REG_STATUS) & REG_STATUS_OUTPUT )
	{
		uint8_t byte = inport8(REG_DATA);
		// TODO: This drops incoming bytes if the buffer is full. Instead make
		// the device not send further interrupts until we have available bytes.
		if ( irq1_used < sizeof(irq1_buffer) )
		{
			size_t index = irq1_offset + irq1_used++;
			if ( sizeof(irq1_buffer) <= index )
				index -= sizeof(irq1_buffer);
			irq1_buffer[index] = byte;
			if ( !irq1_working )
			{
				Interrupt::ScheduleWork(&irq1_work);
				irq1_working = true;
			}
		}
	}
}

static void OnIRQ12(struct interrupt_context* intctx, void* user)
{
	(void) intctx;
	(void) user;
	if ( inport8(REG_STATUS) & REG_STATUS_OUTPUT )
	{
		uint8_t byte = inport8(REG_DATA);
		// TODO: This drops incoming bytes if the buffer is full. Instead make
		// the device not send further interrupts until we have available bytes.
		if ( irq12_used < sizeof(irq12_buffer) )
		{
			size_t index = irq12_offset + irq12_used++;
			if ( sizeof(irq12_buffer) <= index )
				index -= sizeof(irq12_buffer);
			irq12_buffer[index] = byte;
			if ( !irq12_working )
			{
				Interrupt::ScheduleWork(&irq12_work);
				irq12_working = true;
			}
		}
	}
}

void Init(PS2Device* keyboard, PS2Device* mouse)
{
	ps2_controller = new PS2Controller();
	if ( !ps2_controller )
		Panic("Failed to allocate PS2Controller");
	ps2_controller->Init(keyboard, mouse);
}

} // namespace PS2
} // namespace Sortix

namespace Sortix {

using namespace Sortix::PS2;

PS2Controller::PS2Controller()
{
	ps2_lock = KTHREAD_MUTEX_INITIALIZER;
	dual = false;
	devices[0] = NULL;
	devices[1] = NULL;
}

bool PS2Controller::Init(PS2Device* keyboard, PS2Device* mouse)
{
	uint8_t byte;
	uint8_t config;

	// Disable both ports to make sure no more data is sent.
	if ( !TryWriteCommand(REG_COMMAND_DISABLE_FIRST_PORT) ||
	     !TryWriteCommand(REG_COMMAND_DISABLE_SECOND_PORT) )
		return false;
	// Read all the data that might be pending.
	while ( inport8(REG_STATUS) & REG_STATUS_OUTPUT )
		inport8(REG_DATA);
	// Read the configuration byte.
	if ( !TryWriteCommand(REG_COMMAND_READ_RAM) ||
	     !TryReadByte(&config) )
		return false;
	// Disable interrupts and make sure port 1 is enabled.
	config &= ~REG_CONFIG_FIRST_INTERRUPT;
	config &= ~REG_CONFIG_SECOND_INTERRUPT;
	config &= ~REG_CONFIG_NO_FIRST_CLOCK;
	config &= ~REG_CONFIG_FIRST_TRANSLATION;
	// Write the updated configuration byte.
	if ( !TryWriteCommand(REG_COMMAND_WRITE_RAM) ||
	     !TryWriteByte(config) )
		return false;
	// Perform a controller self-test to make sure it works.
	if ( !TryWriteCommand(REG_COMMAND_TEST_CONTROLLER) ||
	     !TryReadByte(&byte) )
		return false;
	if ( byte == 0xFF )
		return false;
	if ( byte != 0x55 )
	{
		Log::PrintF("ps2: Self-test failure resulted in 0x%02X "
		            "instead of 0x55\n", byte);
		return false;
	}
	// Write the configuration byte again, since the osdev wiki claims that some
	// hardware might reset the PS/2 controller upon the self-test.
	if ( !TryWriteCommand(REG_COMMAND_WRITE_RAM) ||
	     !TryWriteByte(config) )
		return false;
	// If the second port is not enabled, detect if is available.
	dual = !(config & REG_CONFIG_NO_SECOND_CLOCK);
	if ( !dual )
	{
		// See if the enable command works for the second port.
		if ( !TryWriteCommand(REG_COMMAND_ENABLE_SECOND_PORT) ||
		     !TryWriteCommand(REG_COMMAND_READ_RAM) ||
		     !TryReadByte(&config) )
			return false;
		dual = !(config & REG_CONFIG_NO_SECOND_CLOCK);
		// TODO: Data could be sent here?
		// If it did, temporarily disable it again.
		if ( dual && !TryWriteCommand(REG_COMMAND_DISABLE_SECOND_PORT) )
			return false;
	}
	bool port_1 = true;
	bool port_2 = dual;
#if 0 // Disabled due to some emulated PS/2 controllers not handling this well.
	if ( port_1 )
	{
		if ( !TryWriteCommand(REG_COMMAND_TEST_FIRST_PORT) ||
		     !TryReadByte(&byte) )
			return;
		port_1 = byte == 0x00;
	}
	if ( port_2 )
	{
		if ( !TryWriteCommand(REG_COMMAND_TEST_SECOND_PORT) ||
		     !TryReadByte(&byte) )
			return;
		port_2 = byte == 0x00;
	}
#endif
	// Detect if the devices are available.
	size_t port_1_resp_size = 0;
	uint8_t port_1_resp[2];
	if ( port_1 && !DetectDevice(1, port_1_resp, &port_1_resp_size) )
		port_1 = false;
	size_t port_2_resp_size = 0;
	uint8_t port_2_resp[2];
	if ( port_2 && !DetectDevice(2, port_2_resp, &port_2_resp_size) )
		port_2 = false;
	if ( port_1 && !devices[0] &&
	     IsKeyboardResponse(port_1_resp, port_1_resp_size) )
		devices[0] = keyboard;
	if ( port_2 && !devices[1] &&
	     IsMouseResponse(port_2_resp, port_2_resp_size) )
		devices[1] = mouse;
	if ( port_1 && !devices[0] &&
	     IsMouseResponse(port_1_resp, port_1_resp_size) )
		devices[0] = mouse;
	if ( port_2 && !devices[1] &&
	     IsKeyboardResponse(port_2_resp, port_2_resp_size) )
		devices[1] = keyboard;
	port_1 = port_1 && devices[0];
	port_2 = port_2 && devices[1];
	// Initialize the ports. The firmware might not send IRQs in response to
	// commands on the ports, so perform the initialization before interrupts
	// are enabled. Ensure that only one port is enabled at a time, so the
	// ports don't talk at the same time and the driver doesn't know which
	// port sent the bytes.
	if ( devices[0] )
	{
		if ( !TryWriteCommand(REG_COMMAND_ENABLE_FIRST_PORT) )
			return false;
		devices[0]->PS2DeviceInitialize(this, 1,
		                                port_1_resp, port_1_resp_size);
		if ( !TryWriteCommand(REG_COMMAND_DISABLE_FIRST_PORT) )
			return false;
	}
	if ( devices[1] )
	{
		if ( !TryWriteCommand(REG_COMMAND_ENABLE_SECOND_PORT) )
			return false;
		devices[1]->PS2DeviceInitialize(this, 2,
		                                port_2_resp, port_2_resp_size);
		if ( !TryWriteCommand(REG_COMMAND_DISABLE_SECOND_PORT) )
			return false;
	}
	// Enable both ports.
	if ( port_1 && !TryWriteCommand(REG_COMMAND_ENABLE_FIRST_PORT) )
		return false;
	if ( port_2 && !TryWriteCommand(REG_COMMAND_ENABLE_SECOND_PORT) )
		return false;
	// Enable the interrupts now that we are ready to process them.
	if ( !TryWriteCommand(REG_COMMAND_READ_RAM) ||
	     !TryReadByte(&config) )
		return false;
	irq1_registration.handler = OnIRQ1;
	irq1_registration.context = NULL;
	Interrupt::RegisterHandler(Interrupt::IRQ1, &irq1_registration);
	irq12_registration.handler = OnIRQ12;
	irq12_registration.context = NULL;
	Interrupt::RegisterHandler(Interrupt::IRQ12, &irq12_registration);
	config |= port_1 ? REG_CONFIG_FIRST_INTERRUPT : 0;
	config |= port_2 ? REG_CONFIG_SECOND_INTERRUPT : 0;
	if ( !TryWriteCommand(REG_COMMAND_WRITE_RAM) ||
	     !TryWriteByte(config) )
		return false;
	return true;
}

bool PS2Controller::DetectDevice(uint8_t port,
                                 uint8_t* response, size_t* response_size)
{
	uint8_t enable = port == 1 ? REG_COMMAND_ENABLE_FIRST_PORT :
	                             REG_COMMAND_ENABLE_SECOND_PORT;
	uint8_t disable = port == 1 ? REG_COMMAND_DISABLE_FIRST_PORT :
	                              REG_COMMAND_DISABLE_SECOND_PORT;
	uint8_t byte;
	if ( !TryWriteCommand(enable) )
		return false;
	// TODO: The port is not reset. A reset may or may not be desirable.
	if ( !SendSync(port, DEVICE_CMD_DISABLE_SCAN, &byte) )
	{
		if ( byte == DEVICE_RESEND )
		{
			// HARDWARE BUG:
			// This may be incomplete PS/2 emulation that simulates the
			// controller but the devices always responds with 0xFE to anything
			// they receive. This happened on sortie's old and broken 2009
			// desktop. The keyboard device still supplies IRQ1's and scancodes.
			// Let's assume the devices are stil there even though we can't
			// control them.
			if ( port == 1 )
			{
				*response_size = 2;
				response[0] = 0xAB;
				response[1] = 0x83;
				if ( !TryWriteCommand(disable) )
					return false;
				return true;
			}
			if ( port == 2 )
			{
				*response_size = 1;
				response[0] = 0x00;
				if ( !TryWriteCommand(disable) )
					return false;
				return true;
			}
		}
		TryWriteCommand(disable);
		return false;
	}
	// Empty pending buffer.
	while ( TryReadByte(&byte) )
		continue;
	if ( !SendSync(port, DEVICE_CMD_IDENTIFY) )
	{
		TryWriteCommand(disable);
		return false;
	}
	*response_size = 0;
	if ( TryReadByte(&byte) )
	{
		response[(*response_size)++] = byte;
		if ( TryReadByte(&byte) )
			response[(*response_size)++] = byte;
	}
	if ( !TryWriteCommand(disable) )
		return false;
	return true;
}

void PS2Controller::OnPortByte(uint8_t port, uint8_t byte)
{
	ScopedLock lock(&ps2_lock);
	devices[port - 1]->PS2DeviceOnByte(byte);
}

// This function is safe only if interrupts are enabled and the devices are
// properly initialized.
bool PS2Controller::Send(uint8_t port, uint8_t byte) // Locked: ps2_lock
{
	if ( !TryWriteToPort(byte, port) )
		return false;
	return true;
}

// This function is safe only if interrupts are disabled and the other port is
// disabled so it won't send bytes unexpectedly.
bool PS2Controller::SendSync(uint8_t port, uint8_t command, uint8_t* answer)
{
	if ( answer )
		*answer = DEVICE_ERROR;
	size_t unrelated = 0;
	for ( size_t retry = 0;
	      retry < DEVICE_RETRIES && unrelated < DEVICE_MAX_UNRELATED;
	      retry++ )
	{
		if ( !TryWriteToPort(command, port) )
			return false;
		while ( unrelated < DEVICE_MAX_UNRELATED )
		{
			uint8_t byte;
			if ( !TryReadByte(&byte) )
				return false;
			if ( answer )
				*answer = byte;
			if ( byte == DEVICE_ACK || byte == DEVICE_ECHO )
				return true;
			if ( byte != DEVICE_RESEND )
			{
				// We received a weird response, probably pending data, discard
				// it and hope we receive a real acknowledgement.
				if ( 1000 <= unrelated )
					return false;
				unrelated++;
				continue;
			}
			break;
		}
	}
	return false;
}

// This function is safe only if interrupts are disabled and the other port is
// disabled so it won't send bytes unexpectedly.
bool PS2Controller::ReadSync(uint8_t port, uint8_t* byte)
{
	(void) port;
	if ( TryReadByte(byte) )
		return true;
	return false;
}

} // namespace Sortix
