/*
 * Copyright (c) 2012, 2014, 2016, 2017, 2022 Jonas 'Sortie' Termansen.
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
 * gpu/bga/bga.cpp
 * Driver for the Bochs VBE Extensions.
 */

#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sortix/mman.h>

#include <sortix/kernel/addralloc.h>
#include <sortix/kernel/cpu.h>
#include <sortix/kernel/ioport.h>
#include <sortix/kernel/kernel.h>
#include <sortix/kernel/memorymanagement.h>
#include <sortix/kernel/pci.h>
#include <sortix/kernel/pci-mmio.h>
#include <sortix/kernel/refcount.h>
#include <sortix/kernel/string.h>
#include <sortix/kernel/textbuffer.h>
#include <sortix/kernel/video.h>

#if defined(__i386__) || defined(__x86_64__)
#include "x86-family/vbox.h"
#endif

#include "lfbtextbuffer.h"
#include "bga.h"

namespace Sortix {
namespace BGA {

const uint16_t VBE_DISPI_INDEX_ID = 0;
const uint16_t VBE_DISPI_INDEX_XRES = 1;
const uint16_t VBE_DISPI_INDEX_YRES = 2;
const uint16_t VBE_DISPI_INDEX_BPP = 3;
const uint16_t VBE_DISPI_INDEX_ENABLE = 4;
const uint16_t VBE_DISPI_INDEX_BANK = 5;
const uint16_t VBE_DISPI_INDEX_VIRT_WIDTH = 6;
const uint16_t VBE_DISPI_INDEX_VIRT_HEIGHT = 7;
const uint16_t VBE_DISPI_INDEX_X_OFFSET = 8;
const uint16_t VBE_DISPI_INDEX_Y_OFFSET = 9;
const uint16_t VBE_DISPI_NUM_REGISTERS = 10;

#if defined(__i386__) || defined(__x86_64__)
const uint16_t VBE_DISPI_IOPORT_INDEX = 0x01CE;
const uint16_t VBE_DISPI_IOPORT_DATA = 0x01CF;
#endif

const uint16_t VBE_DISPI_BPP_4 = 0x04;
const uint16_t VBE_DISPI_BPP_8 = 0x08;
const uint16_t VBE_DISPI_BPP_15 = 0x0F;
const uint16_t VBE_DISPI_BPP_16 = 0x10;
const uint16_t VBE_DISPI_BPP_24 = 0x18;
const uint16_t VBE_DISPI_BPP_32 = 0x20;

const uint16_t VBE_DISPI_DISABLED = 0x00;
const uint16_t VBE_DISPI_ENABLED = 0x01;
const uint16_t VBE_DISPI_GETCAPS = 0x02;
const uint16_t VBE_DISPI_8BIT_DAC = 0x20;
const uint16_t VBE_DISPI_LFB_ENABLED = 0x40;
const uint16_t VBE_DISPI_NOCLEARMEM = 0x80;

const uint16_t VBE_MIN_SUP_VERSION = 0xB0C0;
const uint16_t VBE_MIN_POS_VERSION = 0xB0C0;
const uint16_t VBE_MAX_POS_VERSION = 0xB0CF;

static bool IsStandardResolution(uint16_t width, uint16_t height, uint16_t depth)
{
	if ( depth != VBE_DISPI_BPP_32 ) { return false; }
	if ( width == 720 && height == 400 ) { return true; }
	if ( width == 800 && height == 600 ) { return true; }
	if ( width == 1024 && height == 768 ) { return true; }
	if ( width == 1280 && height == 720 ) { return true; }
	if ( width == 1280 && height == 1024 ) { return true; }
	if ( width == 1600 && height == 900 ) { return true; }
	if ( width == 1920 && height == 1080 ) { return true; }
	return false;
}

class BGADevice : public VideoDevice
{
public:
	BGADevice(uint32_t devaddr, addralloc_t fb_alloc, addralloc_t mmio_alloc);
	virtual ~BGADevice();

public:
	virtual uint64_t GetConnectorCount();
	virtual bool GetDefaultMode(uint64_t connector, struct dispmsg_crtc_mode* mode);
	virtual bool GetCurrentMode(uint64_t connector, struct dispmsg_crtc_mode* mode);
	virtual bool SwitchMode(uint64_t connector, struct dispmsg_crtc_mode mode);
	virtual bool Supports(uint64_t connector, struct dispmsg_crtc_mode mode);
	virtual struct dispmsg_crtc_mode* GetModes(uint64_t connector, size_t* num_modes);
	virtual off_t FrameSize();
	virtual ssize_t WriteAt(ioctx_t* ctx, off_t off, const void* buf, size_t count);
	virtual ssize_t ReadAt(ioctx_t* ctx, off_t off, void* buf, size_t count);
	virtual TextBuffer* CreateTextBuffer(uint64_t connector, struct dispmsg_crtc_mode mode);

public:
	bool Initialize();

private:
	bool DetectModes();
	uint16_t WriteRegister(uint16_t index, uint16_t value);
	uint16_t ReadRegister(uint16_t index);
	uint16_t GetCapability(uint16_t index);
	bool SetVideoMode(uint16_t width, uint16_t height, uint16_t depth, bool keep);
	bool SupportsResolution(uint16_t width, uint16_t height, uint16_t depth);

private:
#if defined(__i386__) || defined(__x86_64__)
	VBox::GuestAdditions* guest_additions;
#endif
	size_t num_modes;
	struct dispmsg_crtc_mode* modes;
	struct dispmsg_crtc_mode current_mode;
	addralloc_t fb_alloc;
	addralloc_t mmio_alloc;
	uint32_t devaddr;
	uint16_t version;
	uint16_t maxbpp;
	uint16_t maxxres;
	uint16_t maxyres;

};

BGADevice::BGADevice(uint32_t devaddr, addralloc_t fb_alloc, addralloc_t mmio_alloc) :
	fb_alloc(fb_alloc), mmio_alloc(mmio_alloc), devaddr(devaddr)
{
#if defined(__i386__) || defined(__x86_64__)
	guest_additions = NULL;
#endif
	num_modes = 0;
	modes = NULL;
	memset(&current_mode, 0, sizeof(current_mode));
	version = 0;
	maxbpp = 0;
	maxxres = 0;
	maxyres = 0;
}

BGADevice::~BGADevice()
{
#if defined(__i386__) || defined(__x86_64__)
	if ( guest_additions )
		guest_additions->UnregisterVideoDevice(device_index);
#endif
	UnmapPCIBar(&fb_alloc);
	UnmapPCIBar(&mmio_alloc);
	delete[] modes;
}

uint16_t BGADevice::WriteRegister(uint16_t index, uint16_t value)
{
	assert(index < VBE_DISPI_NUM_REGISTERS);
#if defined(__i386__) || defined(__x86_64__)
	if ( mmio_alloc.size == 0 )
	{
		outport16(VBE_DISPI_IOPORT_INDEX, index);
		return outport16(VBE_DISPI_IOPORT_DATA, value);
	}
#endif
	volatile little_uint16_t* regs =
		(volatile little_uint16_t*) (mmio_alloc.from + 0x500);
	return regs[index] = value;
}

uint16_t BGADevice::ReadRegister(uint16_t index)
{
#if defined(__i386__) || defined(__x86_64__)
	if ( mmio_alloc.size == 0 )
	{
	    outport16(VBE_DISPI_IOPORT_INDEX, index);
	    return inport16(VBE_DISPI_IOPORT_DATA);
	}
#endif
	assert(index < VBE_DISPI_NUM_REGISTERS);
	volatile little_uint16_t* regs =
		(volatile little_uint16_t*) (mmio_alloc.from + 0x500);
	return regs[index];
}

uint16_t BGADevice::GetCapability(uint16_t index)
{
	uint16_t was_enabled = ReadRegister(VBE_DISPI_INDEX_ENABLE);
	WriteRegister(VBE_DISPI_INDEX_ENABLE, was_enabled | VBE_DISPI_GETCAPS);
	uint16_t cap = ReadRegister(index);
	WriteRegister(VBE_DISPI_INDEX_ENABLE, was_enabled);
	return cap;
}

bool BGADevice::Initialize()
{
	if ( (version = ReadRegister(VBE_DISPI_INDEX_ID)) < VBE_MIN_SUP_VERSION )
	{
		Log::PrintF("[BGA device @ PCI:0x%X] Hardware version 0x%X is too old, "
		            "minimum version supported is 0x%X\n",
		            devaddr, version, VBE_MIN_SUP_VERSION);
		return false;
	}

	maxbpp = GetCapability(VBE_DISPI_INDEX_BPP);
	maxxres = GetCapability(VBE_DISPI_INDEX_XRES);
	maxyres = GetCapability(VBE_DISPI_INDEX_YRES);

	if ( !Video::RegisterDevice("bga", this) )
	{
		Log::PrintF("[BGA device @ PCI:0x%X] Unable to register device: %s\n",
		            devaddr, strerror(errno));
		return false;
	}

#if defined(__i386__) || defined(__x86_64__)
	guest_additions = VBox::GetGuestAdditions();
	if ( guest_additions &&
	     !guest_additions->RegisterVideoDevice(device_index) )
		guest_additions = NULL;
#endif

	Video::ConfigureDevice(this);

#if defined(__i386__) || defined(__x86_64__)

	if ( guest_additions )
		guest_additions->ReadyVideoDevice(device_index);
#endif

	return true;
}

bool BGADevice::SetVideoMode(uint16_t width, uint16_t height, uint16_t depth, bool keep)
{
	bool uselinear = true;
	WriteRegister(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
	WriteRegister(VBE_DISPI_INDEX_XRES, width);
	WriteRegister(VBE_DISPI_INDEX_YRES, height);
	WriteRegister(VBE_DISPI_INDEX_BPP, depth);
	WriteRegister(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED |
	 (uselinear ? VBE_DISPI_LFB_ENABLED : 0) |
	 (keep ? VBE_DISPI_NOCLEARMEM : 0));
	// TODO: How do we verify the video mode was *actually* set?
	return true;
}

uint64_t BGADevice::GetConnectorCount()
{
	return 1;
}

// TODO: Need a better method of detecting available/desired resolutions.
bool BGADevice::SupportsResolution(uint16_t width, uint16_t height, uint16_t depth)
{
	if ( !width || !height || !depth )
		return errno = EINVAL, false;
	if ( maxxres < width || maxyres < height || maxbpp < depth )
		return errno = EINVAL, false;
	// TODO: Is this actually a restriction?
	// TODO: This is not a restriction in VirtualBox anymore at least.
	//if ( width % 8U )
	//	return errno = EINVAL, false;
	// TODO: Can we determine this more closely in advance? Perhaps if the
	//       framebuffer we will be using is larger than video memory?
	return true;
}

bool BGADevice::GetDefaultMode(uint64_t connector,
                               struct dispmsg_crtc_mode* mode_out)
{
	if ( connector )
		return errno = EINVAL, false;

	bool good = false;
	uint32_t xres;
	uint32_t yres;
	uint32_t bpp;
#if 0
#if defined(__i386__) || defined(__x86_64__)
	if ( guest_additions &&
	     guest_additions->GetBestVideoMode(connector, &xres, &yres, &bpp) )
	{
		good = true;
	}
	else
#endif
#endif
	if ( connector == 0 && Log::fallback_framebuffer &&
	     SupportsResolution(Log::fallback_framebuffer_width,
	                        Log::fallback_framebuffer_height,
	                        32) )
	{
		xres = Log::fallback_framebuffer_width;
		yres = Log::fallback_framebuffer_height;
		bpp = 32;
	}
	else if ( connector == 0 && Log::fallback_framebuffer &&
	          SupportsResolution(Log::fallback_framebuffer_width,
	                             Log::fallback_framebuffer_height,
	                             Log::fallback_framebuffer_bpp) )
	{
		xres = Log::fallback_framebuffer_width;
		yres = Log::fallback_framebuffer_height;
		bpp = Log::fallback_framebuffer_bpp;
	}
	else
	{
		return errno = EINVAL, false;
	}

	struct dispmsg_crtc_mode mode;
	memset(&mode, 0, sizeof(0));
	mode.driver_index = 0;
	mode.magic = 0;
	mode.control = DISPMSG_CONTROL_VALID | DISPMSG_CONTROL_DEFAULT;
	if ( good )
		mode.control |= DISPMSG_CONTROL_GOOD_DEFAULT;
	mode.fb_format = bpp;
	mode.view_xres = xres;
	mode.view_yres = yres;
	mode.fb_location = 0;
	mode.pitch = xres * (bpp + 7) / 8;
	mode.surf_off_x = 0;
	mode.surf_off_y = 0;
	mode.start_x = 0;
	mode.start_y = 0;
	mode.end_x = 0;
	mode.end_y = 0;
	mode.desktop_height = yres;
	*mode_out = mode;

	return true;
}

bool BGADevice::GetCurrentMode(uint64_t connector,
                               struct dispmsg_crtc_mode* mode)
{
	if ( connector != 0 )
		return false;
	*mode = current_mode;
#if defined(__i386__) || defined(__x86_64__)
	if ( guest_additions )
		mode->control |= DISPMSG_CONTROL_VM_AUTO_SCALE;
#endif
	return true;
}

bool BGADevice::SwitchMode(uint64_t connector, struct dispmsg_crtc_mode mode)
{
	if ( !Supports(connector, mode) )
		return false;

	if ( connector != 0 )
		return errno = EINVAL, false;

	if ( !SetVideoMode(mode.view_xres, mode.view_yres, mode.fb_format, false) )
		return false;

	current_mode = mode;

	return true;
}

bool BGADevice::Supports(uint64_t connector, struct dispmsg_crtc_mode mode)
{
	if ( connector != 0 )
		return errno = EINVAL, false;

	if ( mode.control & DISPMSG_CONTROL_VGA )
		return errno = EINVAL, false;
	if ( !(mode.control & DISPMSG_CONTROL_VALID) )
		return errno = EINVAL, false;

	if ( UINT16_MAX < mode.view_xres )
		return errno = EINVAL, false;
	if ( UINT16_MAX < mode.view_yres )
		return errno = EINVAL, false;
	// TODO: This is wrong, list the right values as above.
	if ( mode.fb_format != VBE_DISPI_BPP_4 &&
	     mode.fb_format != VBE_DISPI_BPP_8 &&
	     mode.fb_format != VBE_DISPI_BPP_15 &&
	     mode.fb_format != VBE_DISPI_BPP_16 &&
	     mode.fb_format != VBE_DISPI_BPP_24 &&
	     mode.fb_format != VBE_DISPI_BPP_32 )
		return errno = EINVAL, false;

	// TODO: This is disabled because its support needs to be verified, see the
	//       framebuffer size calculation above?
	if ( mode.fb_format != VBE_DISPI_BPP_24 &&
	     mode.fb_format != VBE_DISPI_BPP_32 )
		return errno = ENOSYS, false;

	size_t new_framesize = (size_t) mode.view_xres *
	                       (size_t) mode.view_yres *
	                       ((size_t) mode.fb_format + 7) / 8UL;
	// TODO: Use a better error code than ENOSPC?
	if ( fb_alloc.size < new_framesize )
		return errno = ENOSPC, false;

	return SupportsResolution(mode.view_xres, mode.view_yres, mode.fb_format);
}

struct dispmsg_crtc_mode* BGADevice::GetModes(uint64_t connector, size_t* retnum)
{
	if ( connector != 0 )
		return errno = EINVAL, (struct dispmsg_crtc_mode*) NULL;

	if ( !modes && !DetectModes() )
		return NULL;
	struct dispmsg_crtc_mode* result = new struct dispmsg_crtc_mode[num_modes];
	if ( !result )
		return NULL;
	for ( size_t i = 0; i < num_modes; i++ )
		result[i] = modes[i];
	*retnum = num_modes;
	return result;
}

off_t BGADevice::FrameSize()
{
	return (off_t) fb_alloc.size;
}

ssize_t BGADevice::WriteAt(ioctx_t* ctx, off_t off, const void* buf, size_t count)
{
	uint8_t* frame = (uint8_t*) fb_alloc.from;
	if ( (off_t) fb_alloc.size <= off )
		return 0;
	if ( fb_alloc.size < off + count )
		count = fb_alloc.size - off;
	if ( !ctx->copy_from_src(frame + off, buf, count) )
		return -1;
	return count;
}

ssize_t BGADevice::ReadAt(ioctx_t* ctx, off_t off, void* buf, size_t count)
{
	const uint8_t* frame = (const uint8_t*) fb_alloc.from;
	if ( (off_t) fb_alloc.size <= off )
		return 0;
	if ( fb_alloc.size < off + count )
		count = fb_alloc.size - off;
	if ( !ctx->copy_to_dest(buf, frame + off, count) )
		return -1;
	return count;
}

bool BGADevice::DetectModes()
{
	num_modes = 0;
	unsigned bpp = VBE_DISPI_BPP_32;
	for ( unsigned w = 0; w < maxxres; w += 4U )
	{
		for ( unsigned h = 0; h < maxyres; h += 4UL )
		{
			if ( !IsStandardResolution(w, h, bpp) )
				continue;
			if ( !((BGADevice*) this)->SupportsResolution(w, h, bpp) )
				continue;
			num_modes++;
		}
	}
	num_modes++;
	modes = new struct dispmsg_crtc_mode[num_modes];
	if ( !modes )
		return false;
	memset(modes, 0, sizeof(char*) * num_modes);
	size_t current_mode_id = 0;
	for ( unsigned w = 0; w < maxxres; w += 4U )
	{
		for ( unsigned h = 0; h < maxyres; h += 4UL )
		{
			if ( !IsStandardResolution(w, h, bpp) )
				continue;
			if ( !((BGADevice*) this)->SupportsResolution(w, h, bpp) )
				continue;
			struct dispmsg_crtc_mode mode;
			memset(&mode, 0, sizeof(mode));
			mode.view_xres = w;
			mode.view_yres = h;
			mode.fb_format = bpp;
			mode.control = DISPMSG_CONTROL_VALID;
			modes[current_mode_id++] = mode;
		}
	}

	struct dispmsg_crtc_mode any_mode;
	memset(&any_mode, 0, sizeof(any_mode));
	any_mode.view_xres = 0;
	any_mode.view_yres = 0;
	any_mode.fb_format = 0;
	any_mode.control = DISPMSG_CONTROL_OTHER_RESOLUTIONS;
#if defined(__i386__) || defined(__x86_64__)
	if ( guest_additions )
		any_mode.control |= DISPMSG_CONTROL_VM_AUTO_SCALE;
#endif

	modes[num_modes-1] = any_mode;

	return true;
}

TextBuffer* BGADevice::CreateTextBuffer(uint64_t connector,
                                        struct dispmsg_crtc_mode mode)
{
	if ( !Supports(connector, mode) )
		return NULL;
	if ( connector != 0 )
		return errno = EINVAL, (TextBuffer*) NULL;

	uint8_t* lfb = (uint8_t*) fb_alloc.from;
	uint32_t lfbformat = mode.fb_format;
	size_t scansize = mode.view_xres * mode.fb_format / 8UL;
	return CreateLFBTextBuffer(lfb, lfbformat, mode.view_xres, mode.view_yres, scansize);
}

static void TryInitializeDevice(uint32_t devaddr)
{
	pciid_t id = PCI::GetDeviceId(devaddr);

	bool is_qemu_bga = id.vendorid == 0x1234 && id.deviceid == 0x1111;
	bool is_vbox_bga = id.vendorid == 0x80EE && id.deviceid == 0xBEEF;

	(void) is_qemu_bga;
	(void) is_vbox_bga;

	pcibar_t fb_bar;
	pcibar_t mmio_bar;
	addralloc_t fb_alloc;
	addralloc_t mmio_alloc;
	bool has_mmio = false;
	bool fallback_ioport = false;

	fb_bar = PCI::GetBAR(devaddr, 0);
	if ( !MapPCIBAR(&fb_alloc, fb_bar, Memory::PAT_WC) )
	{
		Log::PrintF("[BGA device @ PCI:0x%X] Framebuffer could not be mapped: %s\n",
		            devaddr, strerror(errno));
		return;
	}

	if ( is_qemu_bga )
		mmio_bar = PCI::GetBAR(devaddr, 2);

	if ( is_qemu_bga && mmio_bar.is_mmio() && 4096 <= mmio_bar.size() )
	{
		has_mmio = true;

		if ( !MapPCIBAR(&mmio_alloc, mmio_bar, Memory::PAT_UC) )
		{
			Log::PrintF("[BGA device @ PCI:0x%X] Memory-mapped registers could not be mapped: %s\n",
			            devaddr, strerror(errno));
			UnmapPCIBar(&fb_alloc);
			return;
		}
	}

	else
	{
		// This device doesn't come with its own set of registers, so we have to
		// assume that the global BGA io port registers are available and that
		// only a single such device is present (since two concurrent devices)
		// could not exist then. This is only available on x86-family systems.
#if defined(__i386__) || defined(__x86_64__)
		fallback_ioport = true;
#endif
	}

	if ( !has_mmio && !fallback_ioport )
	{
		Log::PrintF("[BGA device @ PCI:0x%X] Device provides no registers.\n",
		            devaddr);
		UnmapPCIBar(&fb_alloc);
		return;
	}

	if ( fallback_ioport )
		memset(&mmio_alloc, 0, sizeof(mmio_alloc));

	BGADevice* bga_device = new BGADevice(devaddr, fb_alloc, mmio_alloc);
	if ( !bga_device )
	{
		Log::PrintF("[BGA device @ PCI:0x%X] Unable to allocate driver structure: %s\n",
		            devaddr, strerror(errno));
		UnmapPCIBar(&mmio_alloc);
		UnmapPCIBar(&fb_alloc);
		return;
	}

	if ( !bga_device->Initialize() )
	{
		delete bga_device;
		return;
	}
}

static bool OnDevice(uint32_t devaddr, const pciid_t*, const pcitype_t*, void*,
                     void*)
{
	TryInitializeDevice(devaddr);
	return true;
}

void Init()
{
	pcifind_t patterns[2] =
	{
		{ NULL, 0x1234, 0x1111 },
		{ NULL, 0x80EE, 0xBEEF },
	};
	PCI::Search(OnDevice, NULL, patterns, sizeof(patterns)/sizeof(patterns[0]));
}

} // namespace BGA
} // namespace Sortix
