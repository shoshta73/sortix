/*
 * Copyright (c) 2011-2012, 2014-2016, 2023-2024 Jonas 'Sortie' Termansen.
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
 * com.cpp
 * Handles communication to COM serial ports.
 */

#include <sys/ioctl.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>

#include <sortix/fcntl.h>
#include <sortix/stat.h>

#include <sortix/kernel/descriptor.h>
#include <sortix/kernel/inode.h>
#include <sortix/kernel/interlock.h>
#include <sortix/kernel/interrupt.h>
#include <sortix/kernel/ioctx.h>
#include <sortix/kernel/ioport.h>
#include <sortix/kernel/kernel.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/process.h>
#include <sortix/kernel/refcount.h>
#include <sortix/kernel/thread.h>

#include "com.h"
#include "tty.h"

extern "C" unsigned char nullpage[4096];

namespace Sortix {
namespace COM {

static const uint16_t TXR = 0; // Transmit register
static const uint16_t RXR = 0; // Receive register
static const uint16_t IER = 1; // Interrupt Enable
static const uint16_t IIR = 2; // Interrupt ID
static const uint16_t FCR = 2; // FIFO control
static const uint16_t LCR = 3; // Line control
static const uint16_t MCR = 4; // Modem control
static const uint16_t LSR = 5; // Line Status
static const uint16_t MSR = 6; // Modem Status
static const uint16_t SCR = 7; // Scratch Register
static const uint16_t DLL = 0; // Divisor Latch Low
static const uint16_t DLM = 1; // Divisor latch High

static const uint8_t LCR_DLAB   = 0x80; // Divisor latch access bit
static const uint8_t LCR_SBC    = 0x40; // Set break control
static const uint8_t LCR_SPAR   = 0x20; // Stick parity (?)
static const uint8_t LCR_EPAR   = 0x10; // Even parity select
static const uint8_t LCR_PARITY = 0x08; // Parity Enable
static const uint8_t LCR_STOP   = 0x04; // Stop bits: 0=1 bit, 1=2 bits
static const uint8_t LCR_WLEN5  = 0x00; // Wordlength: 5 bits
static const uint8_t LCR_WLEN6  = 0x01; // Wordlength: 6 bits
static const uint8_t LCR_WLEN7  = 0x02; // Wordlength: 7 bits
static const uint8_t LCR_WLEN8  = 0x03; // Wordlength: 8 bits

static const uint8_t LSR_TEMT  = 0x40; // Transmitter empty
static const uint8_t LSR_THRE  = 0x20; // Transmit-hold-register empty
static const uint8_t LSR_READY = 0x01; // Data received
static const uint8_t LSR_BOTH_EMPTY = LSR_TEMT | LSR_THRE;

static const uint8_t IIR_NO_INTERRUPT = 1 << 0;
static const uint8_t IIR_INTERRUPT_TYPE = 1 << 1 | 1 << 2 | 1 << 3;
static const uint8_t IIR_TIMEOUT = 1 << 2 | 1 << 3;
static const uint8_t IIR_RECV_LINE_STATUS = 1 << 1 | 1 << 2;
static const uint8_t IIR_RECV_DATA = 1 << 2;
static const uint8_t IIR_SENT_DATA = 1 << 1;
static const uint8_t IIR_MODEM_STATUS = 0;

static const uint8_t IER_DATA = 1 << 0;
static const uint8_t IER_SENT = 1 << 1;
static const uint8_t IER_LINE_STATUS = 1 << 2;
static const uint8_t IER_MODEM_STATUS = 1 << 3;
static const uint8_t IER_SLEEP_MODE = 1 << 4;
static const uint8_t IER_LOW_POWER = 1 << 5;

static const unsigned BASE_BAUD = 1843200 / 16;

static const speed_t DEFAULT_SPEED = B38400;
static const size_t DEFAULT_COLUMNS = 80;
static const size_t DEFAULT_ROWS = 25;

static const unsigned int UART_8250 = 1;
static const unsigned int UART_16450 = 2;
static const unsigned int UART_16550 = 3;
static const unsigned int UART_16550A = 4;
static const unsigned int UART_16750 = 5;

static const size_t NUM_COM_PORTS = 4;

// Uses various characteristics of the UART chips to determine the hardware.
static unsigned int HardwareProbe(uint16_t port)
{
	// Set the value "0xE7" to the FCR to test the status of the FIFO flags.
	outport8(port + FCR, 0xE7);
	uint8_t iir = inport8(port + IIR);
	if ( iir & (1 << 6) )
	{
		if ( iir & (1 << 7) )
			return iir & (1 << 5) ? UART_16750 : UART_16550A;
		return UART_16550;
	}

	// See if the scratch register returns what we write into it. The 8520
	// doesn't do it. This is technically undefined behavior, but it is useful
	// to detect hardware versions.
	uint16_t any_value = 0x2A;
	outport8(port + SCR, any_value);
	return inport8(port + SCR) == any_value ? UART_16450 : UART_8250;
}

static inline void WaitForEmptyBuffers(uint16_t port)
{
	while ( (inport8(port + LSR) & LSR_BOTH_EMPTY) != LSR_BOTH_EMPTY )
	{
	}
}

static inline bool IsLineReady(uint16_t port)
{
	return inport8(port + LSR) & LSR_READY;
}

static inline bool CanWriteByte(uint16_t port)
{
	return inport8(port + LSR) & LSR_THRE;
}

static bool IsValidSpeed(speed_t speed)
{
	return speed && speed <= 115200 && !(115200 % speed);
}

static void ConfigurePort(uint16_t port, const struct termios* tio,
                          bool enable_interrupts)
{
	uint16_t divisor = 115200 / tio->c_ispeed;
	outport8(port + FCR, 0);
	outport8(port + LCR, LCR_DLAB);
	outport8(port + DLL, divisor & 0xFF);
	outport8(port + DLM, divisor >> 8);
	uint8_t lcr = 0;
	if ( (tio->c_cflag & CSIZE) == CS5 )
		lcr |= LCR_WLEN5;
	else if ( (tio->c_cflag & CSIZE) == CS6 )
		lcr |= LCR_WLEN6;
	else if ( (tio->c_cflag & CSIZE) == CS7 )
		lcr |= LCR_WLEN7;
	else if ( (tio->c_cflag & CSIZE) == CS8 )
		lcr |= LCR_WLEN8;
	if ( tio->c_cflag & CSTOPB )
		lcr |= LCR_STOP;
	if ( tio->c_cflag & PARENB )
		lcr |= LCR_PARITY;
	if ( tio->c_cflag & PARENB )
	{
		lcr |= LCR_PARITY;
		if ( !(tio->c_cflag & PARODD) )
			lcr |= LCR_EPAR;
	}
	outport8(port + LCR, lcr);
	uint8_t mcr = 0x2 /* RTS */;
	if ( tio->c_cflag & CREAD )
		mcr |= 0x1 /* DTR */;
	outport8(port + MCR, mcr);
	uint8_t ier = enable_interrupts ? 1 : 0;
	outport8(port + IER, ier);
}

class DevCOMPort : public TTY
{
public:
	DevCOMPort(dev_t dev, uid_t owner, gid_t group, mode_t mode, uint16_t port,
	           const char* name);
	virtual ~DevCOMPort();
	virtual int ioctl(ioctx_t* ctx, int cmd, uintptr_t arg);
	virtual int sync(ioctx_t* ctx);
	virtual void tty_output(const unsigned char* buffer, size_t length);
	virtual bool Reconfigure(const struct termios* new_tio);

public:
	void ImportConsole(const struct termios* console_tio,
	                   const struct winsize* console_size);
	bool Initialize(int interrupt);
	bool EmergencyIsImpaired();
	bool EmergencyRecoup();
	void EmergencyReset();

private:
	static void InterruptHandler(struct interrupt_context*, void*);
	static void InterruptWorkHandler(void* context);
	void OnInterrupt();
	void InterruptWork();

private:
	kthread_mutex_t port_lock;
	kthread_mutex_t reconfigure_lock;
	struct interrupt_handler irq_registration;
	struct interrupt_work interrupt_work;
	struct winsize ws;
	uint16_t port;
	uint8_t pending_input_byte;
	bool has_pending_input_byte;

};

DevCOMPort::DevCOMPort(dev_t dev, uid_t owner, gid_t group, mode_t mode,
                       uint16_t port, const char* name) : TTY(dev, 0, mode,
                       owner, group, name)
{
	this->port = port;
	this->port_lock = KTHREAD_MUTEX_INITIALIZER;
	this->reconfigure_lock = KTHREAD_MUTEX_INITIALIZER;
	this->has_pending_input_byte = false;
	tio.c_ispeed = DEFAULT_SPEED;
	tio.c_ospeed = DEFAULT_SPEED;
	memset(&ws, 0, sizeof(ws));
	ws.ws_col = DEFAULT_COLUMNS;
	ws.ws_row = DEFAULT_ROWS;
	interrupt_work.handler = InterruptWorkHandler;
	interrupt_work.context = this;
}

DevCOMPort::~DevCOMPort()
{
}

void DevCOMPort::ImportConsole(const struct termios* console_tio,
                               const struct winsize* console_size)
{
	tio.c_cflag = console_tio->c_cflag;
	tio.c_ispeed = console_tio->c_ispeed;
	tio.c_ospeed = console_tio->c_ospeed;
	ws = *console_size;
}

bool DevCOMPort::Initialize(int interrupt)
{

	ConfigurePort(port, &tio, true);
	irq_registration.handler = DevCOMPort::InterruptHandler;
	irq_registration.context = this;
	Interrupt::RegisterHandler(interrupt, &irq_registration);
	return true;
}

void DevCOMPort::InterruptHandler(struct interrupt_context*, void* user)
{
	((DevCOMPort*) user)->OnInterrupt();
}

void DevCOMPort::OnInterrupt()
{
	if ( !IsLineReady(port) )
		return;
	Interrupt::ScheduleWork(&interrupt_work);
}

void DevCOMPort::InterruptWorkHandler(void* context)
{
	((DevCOMPort*) context)->InterruptWork();
}

void DevCOMPort::InterruptWork()
{
	ScopedLock lock1(&termlock);
	ScopedLock lock2(&port_lock);
	while ( IsLineReady(port) )
	{
		unsigned char byte = inport8(port + RXR);
		if ( tio.c_cflag & CREAD )
			ProcessByte(byte);
	}
}

int DevCOMPort::ioctl(ioctx_t* ctx, int cmd, uintptr_t arg)
{
	ScopedLock lock(&termlock);
	if ( hungup )
		return errno = EIO, -1;
	if ( cmd == TIOCGWINSZ )
	{
		struct winsize* user_ws = (struct winsize*) arg;
		if ( !ctx->copy_to_dest(user_ws, &ws, sizeof(ws)) )
			return -1;
		return 0;
	}
	else if ( cmd == TIOCSWINSZ )
	{
		const struct winsize* user_ws = (const struct winsize*) arg;
		if ( !ctx->copy_from_src(&ws, user_ws, sizeof(ws)) )
			return -1;
		winch();
		return 0;
	}
	lock.Reset();
	return TTY::ioctl(ctx, cmd, arg);
}

int DevCOMPort::sync(ioctx_t* /*ctx*/)
{
	ScopedLock lock(&port_lock);
	WaitForEmptyBuffers(port);
	return 0;
}

void DevCOMPort::tty_output(const unsigned char* buffer, size_t length)
{
	for ( size_t i = 0; i < length; i++ )
	{
		unsigned long attempt = 0;
		while ( !CanWriteByte(port) )
		{
			attempt++;
			if ( attempt <= 10 )
				continue;
			if ( attempt <= 15 )
			{
				kthread_mutex_unlock(&port_lock);
				kthread_yield();
				kthread_mutex_lock(&port_lock);
				continue;
			}
			if ( i )
				return;
			// TODO: This is problematic.
			if ( Signal::IsPending() )
			{
				errno = EINTR;
				return;
			}
		}
		outport8(port + TXR, buffer[i]);
	}
}

bool DevCOMPort::Reconfigure(const struct termios* new_tio) // termlock held
{
	if ( !IsValidSpeed(new_tio->c_ispeed) || !IsValidSpeed(new_tio->c_ospeed) )
		return errno = EINVAL, false;
	if ( new_tio->c_ispeed != new_tio->c_ospeed )
		return errno = EINVAL, false;
	if ( tio.c_ispeed != new_tio->c_ispeed ||
	     tio.c_ospeed != new_tio->c_ospeed ||
	     tio.c_cflag != new_tio->c_cflag )
	{
		// Detect if a panic happens midway.
		ScopedLock lock(&reconfigure_lock);
		ConfigurePort(port, new_tio, true);
	}
	return true;
}

bool DevCOMPort::EmergencyIsImpaired()
{
	if ( !kthread_mutex_trylock(&termlock) )
		return true;
	kthread_mutex_unlock(&termlock);
	if ( !kthread_mutex_trylock(&port_lock) )
		return true;
	kthread_mutex_unlock(&port_lock);
	if ( !kthread_mutex_trylock(&reconfigure_lock) )
		return true;
	kthread_mutex_unlock(&reconfigure_lock);
	return false;
}

bool DevCOMPort::EmergencyRecoup()
{
	kthread_mutex_trylock(&termlock);
	kthread_mutex_unlock(&termlock);
	kthread_mutex_trylock(&port_lock);
	kthread_mutex_unlock(&port_lock);
	kthread_mutex_trylock(&reconfigure_lock);
	kthread_mutex_unlock(&reconfigure_lock);
	if ( !kthread_mutex_trylock(&reconfigure_lock) )
		return false;
	kthread_mutex_unlock(&reconfigure_lock);
	return true;
}

void DevCOMPort::EmergencyReset()
{
	kthread_mutex_trylock(&termlock);
	kthread_mutex_unlock(&termlock);
	kthread_mutex_trylock(&port_lock);
	kthread_mutex_unlock(&port_lock);
	kthread_mutex_trylock(&reconfigure_lock);
	kthread_mutex_unlock(&reconfigure_lock);
	ConfigurePort(port, &tio, false);
}

static size_t console_device;
static struct termios console_tio;
static uint16_t console_port;
static kthread_mutex_t console_lock = KTHREAD_MUTEX_INITIALIZER;
static bool console_imported;
static struct winsize console_size;

static Ref<DevCOMPort> com_devices[1 + NUM_COM_PORTS];

static void ConsoleWriteByte(unsigned char byte)
{
	size_t attempt = 0;
	while ( !CanWriteByte(console_port) )
	{
		attempt++;
		if ( attempt <= 10 )
			continue;
		if ( attempt <= 15 )
		{
			kthread_mutex_unlock(&console_lock);
			kthread_yield();
			kthread_mutex_lock(&console_lock);
			continue;
		}
	}
	outport8(console_port + TXR, byte);
}

static size_t ConsoleWrite(void* /*ctx*/, const char* buf, size_t len)
{
	ScopedLock lock(&console_lock);
	if ( console_imported )
	{
		ioctx_t ctx; SetupKernelIOCtx(&ctx);
		Ref<DevCOMPort> com = com_devices[console_device];
		const uint8_t* buffer = (const uint8_t*) buf;
		size_t done = 0;
		while ( done < len )
		{
			ssize_t amount = com->write(&ctx, buffer + done, len - done);
			if ( amount < 0 )
				break; // TODO: Block all signals.
			done += amount;
		}
		return done;
	}
	for ( size_t i = 0; i < len; i++ )
	{
		if ( buf[i] == '\n' )
			ConsoleWriteByte('\r');
		ConsoleWriteByte(buf[i]);
	}
	return len;
}

static size_t ConsoleWidth(void* /*ctx*/)
{
	if ( console_imported )
	{
		ioctx_t ctx; SetupKernelIOCtx(&ctx);
		Ref<DevCOMPort> com = com_devices[console_device];
		struct winsize ws;
		com->ioctl(&ctx, TIOCGWINSZ, (uintptr_t) &ws);
		return ws.ws_col;
	}
	return console_size.ws_col;
}

static size_t ConsoleHeight(void* /*ctx*/)
{
	if ( console_imported )
	{
		ioctx_t ctx; SetupKernelIOCtx(&ctx);
		Ref<DevCOMPort> com = com_devices[console_device];
		struct winsize ws;
		com->ioctl(&ctx, TIOCGWINSZ, (uintptr_t) &ws);
		return ws.ws_row;
	}
	return console_size.ws_row;
}

static void ConsoleGetCursor(void* /*ctx*/, size_t* column, size_t* row)
{
	// TODO: Conceptually this does not make sense.
	*column = 0;
	*row = 0;
}

static bool ConsoleSync(void* /*ctx*/)
{
	ScopedLock lock(&console_lock);
	return true;
}

static void ConsoleInvalidate(void* /*ctx*/)
{
	ScopedLock lock(&console_lock);
}

bool ConsoleEmergencyIsImpaired(void* /*ctx*/)
{
	if ( !kthread_mutex_trylock(&console_lock) )
		return true;
	kthread_mutex_unlock(&console_lock);
	if ( console_imported )
	{
		Ref<DevCOMPort> com = com_devices[console_device];
		return com->EmergencyIsImpaired();
	}
	return false;
}

bool ConsoleEmergencyRecoup(void* /*ctx*/)
{
	kthread_mutex_trylock(&console_lock);
	kthread_mutex_unlock(&console_lock);
	if ( console_imported )
	{
		Ref<DevCOMPort> com = com_devices[console_device];
		return com->EmergencyRecoup();
	}
	return true;
}

void ConsoleEmergencyReset(void* /*ctx*/)
{
	kthread_mutex_trylock(&console_lock);
	kthread_mutex_unlock(&console_lock);
	if ( console_imported )
	{
		Ref<DevCOMPort> com = com_devices[console_device];
		com->EmergencyReset();
	}
}

void InitializeConsole(const char* console)
{
	assert(!strncmp(console, "com", 3));
	assert(isdigit((unsigned char) console[3]));
	char* end;
	unsigned long device = strtoul(console + 3, &end, 10);
	if ( device < 1 || NUM_COM_PORTS < device )
		PanicF("Invalid console: %s", console);
	console_device = device;
	struct termios tio;
	memset(&tio, 0, sizeof(tio));
	console_tio.c_ispeed = DEFAULT_SPEED;
	console_tio.c_cflag = CS8;
	memset(&console_size, 0, sizeof(console_size));
	console_size.ws_col = DEFAULT_COLUMNS;
	console_size.ws_row = DEFAULT_ROWS;
	if ( *end == ',' )
	{
		end++;
		if ( *end != ',' )
		{
			unsigned long value = strtoul(end, &end, 10);
			if ( !IsValidSpeed(value) )
				PanicF("Invalid console options: %s", console);
			console_tio.c_ispeed = value;
			console_tio.c_cflag = 0;
			if ( *end == 'o' )
				console_tio.c_cflag |= PARENB | PARODD;
			else if ( *end == 'e' )
				console_tio.c_cflag |= PARENB;
			else if ( *end != 'n' )
				PanicF("Invalid console options: %s", console);
			end++;
			if ( *end == '5' )
				console_tio.c_cflag |= CS5;
			else if ( *end == '6' )
				console_tio.c_cflag |= CS6;
			else if ( *end == '7' )
				console_tio.c_cflag |= CS7;
			else if ( *end == '8' )
				console_tio.c_cflag |= CS8;
			else
				PanicF("Invalid console options: %s", console);
			end++;
		}
		if ( *end == ',' )
		{
			end++;
			unsigned long width = strtoul(end, &end, 10);
			if ( !width || *end != 'x' )
				PanicF("Invalid console options: %s", console);
			end++;
			unsigned long height = strtoul(end, &end, 10);
			if ( !height || *end )
				PanicF("Invalid console options: %s", console);
			console_size.ws_col = width;
			console_size.ws_row = height;
		}
	}
	else if ( *end )
		PanicF("Invalid console: %s", console);
	console_tio.c_ospeed = console_tio.c_ispeed;

	const uint16_t* bioscom_ports = (const uint16_t*) (nullpage + 0x400);
	if ( !(console_port = bioscom_ports[device - 1]) )
		PanicF("No such hardware device detected: %s", console);
	outport8(console_port + IER, 0x0);
	ConfigurePort(console_port, &console_tio, false);

	Log::fallback_framebuffer = NULL;
	Log::device_callback = ConsoleWrite;
	Log::device_writeraw = ConsoleWrite;
	Log::device_width = ConsoleWidth;
	Log::device_height = ConsoleHeight;
	Log::device_get_cursor = ConsoleGetCursor;
	Log::device_sync = ConsoleSync;
	Log::device_invalidate = ConsoleInvalidate;
	Log::emergency_device_is_impaired = ConsoleEmergencyIsImpaired;
	Log::emergency_device_recoup = ConsoleEmergencyRecoup;
	Log::emergency_device_reset = ConsoleEmergencyReset;
	Log::emergency_device_callback = ConsoleWrite;
	Log::emergency_device_writeraw = ConsoleWrite;
	Log::emergency_device_width = ConsoleWidth;
	Log::emergency_device_height = ConsoleHeight;
	Log::emergency_device_get_cursor = ConsoleGetCursor;
	Log::emergency_device_sync = ConsoleSync;

	snprintf(Log::console_tty, sizeof(Log::console_tty), "/dev/com%lu", device);
}

void Init(const char* devpath, Ref<Descriptor> slashdev)
{
	uint16_t com_ports[1 + NUM_COM_PORTS];
	unsigned int hw_version[1 + NUM_COM_PORTS];

	const uint16_t* bioscom_ports = (const uint16_t*) (nullpage + 0x400);

	for ( size_t i = 1; i <= NUM_COM_PORTS; i++ )
	{
		if ( !(com_ports[i] = bioscom_ports[i-1]) )
			continue;
		hw_version[i] = HardwareProbe(com_ports[i]);
		outport8(com_ports[i] + IER, 0x0);
	}

	(void) hw_version;

	ioctx_t ctx; SetupKernelIOCtx(&ctx);

	for ( size_t i = 1; i <= NUM_COM_PORTS; i++ )
	{
		if ( !com_ports[i] )
		{
			com_devices[i] = Ref<DevCOMPort>();
			continue;
		}
		char ttyname[TTY_NAME_MAX+1];
		snprintf(ttyname, sizeof(ttyname), "com%zu", i);
		Ref<DevCOMPort> com(
			new DevCOMPort(slashdev->dev, 0, 0, 0660, com_ports[i], ttyname));
		if ( !com )
			PanicF("Unable to allocate device for %s", ttyname);
		com_devices[i] = com;
		if ( i == console_device )
		{
			ScopedLock lock(&console_lock);
			com->ImportConsole(&console_tio, &console_size);
		}
		int interrupt = i == 1 || i == 3 ? Interrupt::IRQ4 : Interrupt::IRQ3;
		com->Initialize(interrupt);
		if ( i == console_device )
		{
			ScopedLock lock(&console_lock);
			console_imported = true;
		}
		if ( LinkInodeInDir(&ctx, slashdev, ttyname, com) != 0 )
			PanicF("Unable to link %s/%s.", devpath, ttyname);
	}
}

} // namespace COM
} // namespace Sortix
