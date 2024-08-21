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
 * x86-family/vbox.cpp
 * VirtualBox Guest Additions.
 */

#include <sys/mman.h>

#include <assert.h>
#include <errno.h>
#include <endian.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <sortix/kernel/addralloc.h>
#include <sortix/kernel/clock.h>
#include <sortix/kernel/interrupt.h>
#include <sortix/kernel/ioport.h>
#include <sortix/kernel/kernel.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/memorymanagement.h>
#include <sortix/kernel/pci.h>
#include <sortix/kernel/pci-mmio.h>
#include <sortix/kernel/time.h>
#include <sortix/kernel/video.h>

#include "vbox.h"

namespace Sortix {
namespace VBox {

#define VBOX_VMMDEV_VERSION 0x00010003
#define VBOX_REQUEST_HEADER_VERSION 0x10001

#define VBOX_EVENT_MOUSE_CAPABILITIES_CHANGED (1U << 0)
#define VBOX_EVENT_HGCM (1U << 1)
#define VBOX_EVENT_DISPLAY_CHANGE_REQUEST (1U << 2)
#define VBOX_EVENT_JUDGE_CREDENTIALS (1U << 3)
#define VBOX_EVENT_RESTORED (1U << 4)
#define VBOX_EVENT_SEAMLESS_MODE_CHANGE_REQUEST (1U << 5)
#define VBOX_EVENT_BALLOON_CHANGE_REQUEST (1U << 6)
#define VBOX_EVENT_STATISTICS_INTERVAL_CHANGE_REQUEST (1U << 7)
#define VBOX_EVENT_VRDP (1U << 8)
#define VBOX_EVENT_MOUSE_POSITION_CHANGED (1U << 9)
#define VBOX_EVENT_CPU_HOTPLUG (1U << 10)

struct registers
{
	uint32_t size;
	uint32_t version;
	uint32_t host_events;
	uint32_t guest_event_mask;
};

struct vbox_header
{
	uint32_t size;
	uint32_t version;
	uint32_t request_type;
	int32_t rc;
	uint32_t reserved[2];
};

#define VBOX_REQUEST_GET_HOST_VERSION 4
struct vbox_host_version
{
	struct vbox_header hdr;
	uint16_t major;
	uint16_t minor;
	uint32_t build;
	uint32_t revision;
	uint32_t features;
};

#define VBOX_REQUEST_GET_HOST_TIME 10
struct vbox_host_time
{
	struct vbox_header hdr;
	uint64_t time;
};

#define VBOX_REQUEST_ACK_EVENTS 41
struct vbox_ack_events
{
	struct vbox_header hdr;
	uint32_t events;
};

#define VBOX_REQUEST_GUEST_INFO 50
struct vbox_guest_info
{
	struct vbox_header hdr;
	uint32_t version;
	uint32_t ostype;
};

#define VBOX_REQUEST_GET_DISPLAY_CHANGE2 54
struct vbox_get_display_change2
{
	struct vbox_header hdr;
	uint32_t xres;
	uint32_t yres;
	uint32_t bpp;
	uint32_t eventack;
	uint32_t display;
};

#define VBOX_REQUEST_SET_GUEST_CAPS2 56
struct vbox_guest_caps2
{
	struct vbox_header hdr;
	uint32_t caps_or;
	uint32_t caps_not;
};
#define VBOX_GUEST_SUPPORTS_SEAMLESS (1U << 0)
#define VBOX_GUEST_SUPPORTS_GUEST_HOST_WINDOW_MAPPING (1U << 1)
#define VBOX_GUEST_SUPPORTS_GRAPHICS (1U << 2)

#define VBOX_REQUEST_VIDEO_MODE_SUPPORTED2 57
struct vbox_video_mode_supported2
{
	struct vbox_header hdr;
	uint32_t display;
	uint32_t xres;
	uint32_t yres;
	uint32_t bpp;
	bool is_supported;
};

class VBoxDevice : public GuestAdditions
{
public:
	VBoxDevice(uint32_t devaddr);
	virtual ~VBoxDevice();

public:
	virtual bool IsSupportedVideoMode(uint32_t display, uint32_t xres,
	                                  uint32_t yres, uint32_t bpp);
	virtual bool GetBestVideoMode(uint32_t display, uint32_t* xres,
	                              uint32_t* yres, uint32_t* bpp);
	virtual bool RegisterVideoDevice(uint64_t device_id);
	virtual void ReadyVideoDevice(uint64_t device_id);
	virtual void UnregisterVideoDevice(uint64_t device_id);

public:
	bool Initialize();
	void OnInterrupt();
	void InterruptWork();

private:
	void Request(void* bufptr, size_t size);
	void RequestIRQ(void* bufptr, size_t size);
	void ReportCapabilities();
	__attribute__((format(printf, 2, 3)))
	void LogF(const char* format, ...);

private:
	struct vbox_host_version vbox_version;
	struct interrupt_handler interrupt_registration;
	struct interrupt_work interrupt_work;
	kthread_mutex_t buffer_lock;
	uint64_t video_device;
	volatile struct registers* regs;
	volatile unsigned char* buffer1;
	volatile unsigned char* buffer2;
	addr_t buffer1_frame;
	addr_t buffer2_frame;
	uint32_t devaddr;
	uint32_t capabilities;
	uint32_t listening_events;
	uint32_t interrupt_work_events;
	addralloc_t mmio_alloc;
	addralloc_t buffer1_alloc;
	addralloc_t buffer2_alloc;
	uint16_t port;
	unsigned char interrupt_index;
	bool has_mmio_alloc;
	bool has_buffer1_alloc;
	bool has_buffer1_mapped;
	bool has_buffer2_alloc;
	bool has_buffer2_mapped;
	bool has_interrupt_registered;
	bool has_video_device;

};

void VBoxDevice__InterruptWork(void* context)
{
	((VBoxDevice*) context)->InterruptWork();
}

VBoxDevice::VBoxDevice(uint32_t devaddr)
{
	this->buffer_lock = KTHREAD_MUTEX_INITIALIZER;
	this->devaddr = devaddr;
	this->buffer1_frame = 0;
	this->buffer2_frame = 0;
	this->has_mmio_alloc = false;
	this->has_buffer1_alloc = false;
	this->has_buffer1_mapped = false;
	this->has_buffer2_alloc = false;
	this->has_buffer2_mapped = false;
	this->has_interrupt_registered = false;
	this->has_video_device = false;
	this->interrupt_work.handler = VBoxDevice__InterruptWork;
	this->interrupt_work.context = this;
	this->interrupt_work_events = 0;
}

VBoxDevice::~VBoxDevice()
{
	if ( has_interrupt_registered )
		Interrupt::UnregisterHandler(interrupt_index, &interrupt_registration);
	if ( has_buffer2_mapped )
		Memory::Unmap(buffer2_alloc.from);
	if ( has_buffer1_mapped )
		Memory::Unmap(buffer1_alloc.from);
	if ( has_buffer2_alloc )
		FreeKernelAddress(&buffer2_alloc);
	if ( has_buffer1_alloc )
		FreeKernelAddress(&buffer1_alloc);
	if ( buffer2_frame )
		Page::Put(buffer2_frame, PAGE_USAGE_DRIVER);
	if ( buffer1_frame )
		Page::Put(buffer1_frame, PAGE_USAGE_DRIVER);
	if ( has_mmio_alloc )
		UnmapPCIBar(&mmio_alloc);
}

void VBoxDevice__OnInterrupt(struct interrupt_context*, void* context)
{
	((VBoxDevice*) context)->OnInterrupt();
}

bool VBoxDevice::Initialize()
{
	ScopedLock lock(&buffer_lock);
	interrupt_index = PCI::SetupInterruptLine(devaddr);
	if ( !interrupt_index )
	{
		LogF("error: cannot determine interrupt line");
		return errno = EINVAL, false;
	}
	pcibar_t port_bar = PCI::GetBAR(devaddr, 0);
	if ( !port_bar.is_iospace() )
	{
		LogF("error: BAR 0 is invalid");
		return false;
	}
	pcibar_t mmio_bar = PCI::GetBAR(devaddr, 1);
	if ( !(mmio_bar.is_mmio() && 4096 <= mmio_bar.size()) )
	{
		LogF("error: BAR 1 is invalid");
		return false;
	}
	port = port_bar.ioaddr();
	if ( !MapPCIBAR(&mmio_alloc, mmio_bar, Memory::PAT_UC) )
	{
		LogF("error: failed to memory map BAR 1: %m");
		return false;
	}
	has_mmio_alloc = true;
	regs = (volatile struct registers*) mmio_alloc.from;
	if ( !(buffer1_frame = Page::Get32Bit(PAGE_USAGE_DRIVER)) )
	{
		LogF("error: buffer page allocation failure");
		return false;
	}
	if ( !(buffer2_frame = Page::Get32Bit(PAGE_USAGE_DRIVER)) )
	{
		LogF("error: buffer page allocation failure");
		return false;
	}
	if ( !AllocateKernelAddress(&buffer1_alloc, Page::Size()) )
	{
		LogF("error: buffer page virtual address allocation failure");
		return false;
	}
	has_buffer1_alloc = true;
	if ( !AllocateKernelAddress(&buffer2_alloc, Page::Size()) )
	{
		LogF("error: buffer page virtual address allocation failure");
		return false;
	}
	has_buffer2_alloc = true;
	int prot = PROT_KREAD | PROT_KWRITE;
	if ( !Memory::Map(buffer1_frame, buffer1_alloc.from, prot) )
	{
		LogF("error: buffer page virtual mapping failure");
		return false;
	}
	has_buffer1_mapped = true;
	buffer1 = (volatile unsigned char*) buffer1_alloc.from;
	if ( !Memory::Map(buffer2_frame, buffer2_alloc.from, prot) )
	{
		LogF("error: buffer page virtual mapping failure");
		return false;
	}
	has_buffer2_mapped = true;
	buffer2 = (volatile unsigned char*) buffer2_alloc.from;

	memset(&vbox_version, 0, sizeof(vbox_version));
	vbox_version.hdr.size = sizeof(vbox_version);
	vbox_version.hdr.version = VBOX_REQUEST_HEADER_VERSION;
	vbox_version.hdr.request_type = VBOX_REQUEST_GET_HOST_VERSION;
	vbox_version.hdr.rc = -1;
	Request(&vbox_version, sizeof(vbox_version));
	if ( vbox_version.hdr.rc != 0 )
	{
		LogF("error: REQUEST_GET_HOST_VERSION failed with rc = %i\n",
		     vbox_version.hdr.rc);
		return false;
	}

	struct vbox_guest_info guest_info;
	memset(&guest_info, 0, sizeof(guest_info));
	guest_info.hdr.size = sizeof(guest_info);
	guest_info.hdr.version = VBOX_REQUEST_HEADER_VERSION;
	guest_info.hdr.request_type = VBOX_REQUEST_GUEST_INFO;
	guest_info.version = VBOX_VMMDEV_VERSION;
	guest_info.ostype = 0;
	Request(&guest_info, sizeof(guest_info));

	struct vbox_host_time host_time;
	memset(&host_time, 0, sizeof(host_time));
	host_time.hdr.size = sizeof(host_time);
	host_time.hdr.version = VBOX_REQUEST_HEADER_VERSION;
	host_time.hdr.request_type = VBOX_REQUEST_GET_HOST_TIME;
	Request(&host_time, sizeof(host_time));
	if ( host_time.hdr.rc == 0 )
	{
		struct timespec realtime;
		realtime.tv_sec = host_time.time / 1000;
		realtime.tv_nsec = (host_time.time % 1000) * 1000000L;
		Time::GetClock(CLOCK_REALTIME)->Set(&realtime, NULL);
	}

	capabilities = 0;
	listening_events = 0;

	ReportCapabilities();

	interrupt_registration.handler = VBoxDevice__OnInterrupt;
	interrupt_registration.context = this;
	Interrupt::RegisterHandler(interrupt_index, &interrupt_registration);
	has_interrupt_registered = true;

	regs->guest_event_mask = listening_events;

	return true;
}

void VBoxDevice::OnInterrupt()
{
	uint32_t host_events = regs->host_events;
	if ( !host_events )
		return;

	regs->guest_event_mask = 0;

	assert(interrupt_work_events == 0);
	interrupt_work_events = host_events;

	// TODO: There's no protection that this interrupt handler isn't clobbering
	//       the buffer because we don't have the lock.

	struct vbox_ack_events ack_events;
	memset(&ack_events, 0, sizeof(ack_events));
	ack_events.hdr.size = sizeof(ack_events);
	ack_events.hdr.version = VBOX_REQUEST_HEADER_VERSION;
	ack_events.hdr.request_type = VBOX_REQUEST_ACK_EVENTS;
	ack_events.events = host_events; // TODO: Or?
	RequestIRQ(&ack_events, sizeof(ack_events));

	Interrupt::ScheduleWork(&interrupt_work);
}

void VBoxDevice::InterruptWork()
{
	ScopedLock lock(&buffer_lock);
	uint32_t host_events = interrupt_work_events;

	if ( host_events & VBOX_EVENT_DISPLAY_CHANGE_REQUEST )
	{
		struct vbox_get_display_change2 get_display_change;
		memset(&get_display_change, 0, sizeof(get_display_change));
		get_display_change.hdr.size = sizeof(get_display_change);
		get_display_change.hdr.version = VBOX_REQUEST_HEADER_VERSION;
		get_display_change.hdr.request_type = VBOX_REQUEST_GET_DISPLAY_CHANGE2;
		get_display_change.eventack = VBOX_EVENT_DISPLAY_CHANGE_REQUEST;
		Request(&get_display_change, sizeof(get_display_change));
		uint32_t display = get_display_change.display;
		uint32_t bpp = get_display_change.bpp;
		uint32_t xres = get_display_change.xres;
		uint32_t yres = get_display_change.yres;
		if ( has_video_device )
			Video::ResizeDisplay(video_device, display, xres, yres, bpp);
	}

	interrupt_work_events = 0;
	regs->guest_event_mask = listening_events;
}

void VBoxDevice::Request(void* bufptr, size_t size)
{
	assert(size <= buffer1_alloc.size);
	unsigned char* buf = (unsigned char*) bufptr;
	for ( size_t i = 0; i < size; i++ )
		buffer1[i] = buf[i];
	outport32(port, buffer1_frame);
	for ( size_t i = 0; i < size; i++ )
		buf[i] = buffer1[i];
}

void VBoxDevice::RequestIRQ(void* bufptr, size_t size)
{
	assert(size <= buffer1_alloc.size);
	unsigned char* buf = (unsigned char*) bufptr;
	for ( size_t i = 0; i < size; i++ )
		buffer1[i] = buf[i];
	outport32(port, buffer1_frame);
	for ( size_t i = 0; i < size; i++ )
		buf[i] = buffer1[i];
}

void VBoxDevice::ReportCapabilities()
{
	struct vbox_guest_caps2 guest_caps;
	memset(&guest_caps, 0, sizeof(guest_caps));
	guest_caps.hdr.size = sizeof(guest_caps);
	guest_caps.hdr.version = VBOX_REQUEST_HEADER_VERSION;
	guest_caps.hdr.request_type = VBOX_REQUEST_SET_GUEST_CAPS2;
	guest_caps.caps_or = capabilities;
	guest_caps.caps_not = ~capabilities;
	Request(&guest_caps, sizeof(guest_caps));
}

void VBoxDevice::LogF(const char* format, ...)
{
	// TODO: Print this line in an atomic manner.
	Log::PrintF("vbox: pci 0x%X: ", devaddr);
	va_list ap;
	va_start(ap, format);
	Log::PrintFV(format, ap);
	va_end(ap);
	Log::PrintF("\n");
}

bool VBoxDevice::IsSupportedVideoMode(uint32_t display, uint32_t xres,
                                      uint32_t yres, uint32_t bpp)
{
	ScopedLock lock(&buffer_lock);
	struct vbox_video_mode_supported2 video_mode_supported;
	memset(&video_mode_supported, 0, sizeof(video_mode_supported));
	video_mode_supported.hdr.size = sizeof(video_mode_supported);
	video_mode_supported.hdr.version = VBOX_REQUEST_HEADER_VERSION;
	video_mode_supported.hdr.request_type = VBOX_REQUEST_VIDEO_MODE_SUPPORTED2;
	video_mode_supported.display = display;
	video_mode_supported.xres = xres;
	video_mode_supported.yres = yres;
	video_mode_supported.bpp = bpp;
	Request(&video_mode_supported, sizeof(video_mode_supported));
	if ( video_mode_supported.hdr.rc != 0 )
		return false;
	return video_mode_supported.is_supported;
}

bool VBoxDevice::GetBestVideoMode(uint32_t display, uint32_t* xres_ptr,
                                  uint32_t* yres_ptr, uint32_t* bpp_ptr)
{
	uint32_t bpp = 32;
	uint32_t xres = 1;
	uint32_t yres = 1;
	if ( !IsSupportedVideoMode(display, xres, yres, bpp) )
	{
		LogF("unsupported %ux%u\n", xres, yres);
		return false;
	}
	while ( IsSupportedVideoMode(display, xres + 1, yres, bpp) )
		xres++;
	while ( IsSupportedVideoMode(display, xres, yres + 1, bpp) )
		yres++;
	*xres_ptr = xres;
	*yres_ptr = yres;
	*bpp_ptr = bpp;
	return true;
}

bool VBoxDevice::RegisterVideoDevice(uint64_t device_id)
{
	ScopedLock lock(&buffer_lock);
	if ( has_video_device )
		return errno = EINVAL, false;
	video_device = device_id;
	has_video_device = true;
	return true;
}

void VBoxDevice::ReadyVideoDevice(uint64_t device_id)
{
	ScopedLock lock(&buffer_lock);
	if ( !has_video_device || device_id != video_device )
		return;
	capabilities |= VBOX_GUEST_SUPPORTS_GRAPHICS;
	ReportCapabilities();
	listening_events |= VBOX_EVENT_DISPLAY_CHANGE_REQUEST;
	regs->guest_event_mask = listening_events;
}

void VBoxDevice::UnregisterVideoDevice(uint64_t device_id)
{
	ScopedLock lock(&buffer_lock);
	if ( !has_video_device || device_id != video_device )
		return;
	has_video_device = false;
	listening_events &= ~VBOX_EVENT_DISPLAY_CHANGE_REQUEST;
	regs->guest_event_mask = listening_events;
	capabilities &= ~VBOX_GUEST_SUPPORTS_GRAPHICS;
	ReportCapabilities();
}

static VBoxDevice* vbox;

GuestAdditions* GetGuestAdditions()
{
	return vbox;
}

void Init()
{
	pcifind_t pcifind(NULL, 0x80EE, 0xCAFE);

	uint32_t devaddr = PCI::SearchForDevices(pcifind, 0);
	if ( !devaddr )
		return;

	vbox = new VBoxDevice(devaddr);
	if ( !vbox )
		Panic("Failed to allocate virtualbox guest additions driver");

	if ( !vbox->Initialize() )
	{
		delete vbox;
		vbox = NULL;
		return;
	}
}

} // namespace VBox
} // namespace Sortix
