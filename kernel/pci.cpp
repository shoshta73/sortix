/*
 * Copyright (c) 2011-2017, 2024 Jonas 'Sortie' Termansen.
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
 * pci.cpp
 * Functions for handling PCI devices.
 */

#include <assert.h>
#include <endian.h>

#include <sortix/kernel/cpu.h>
#include <sortix/kernel/interrupt.h>
#include <sortix/kernel/ioport.h>
#include <sortix/kernel/kernel.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/interrupt.h>
#include <sortix/kernel/pci.h>
#include <sortix/kernel/random.h>

namespace Sortix {
namespace PCI {

static kthread_mutex_t pci_lock = KTHREAD_MUTEX_INITIALIZER;

static const uint16_t CONFIG_ADDRESS = 0xCF8;
static const uint16_t CONFIG_DATA = 0xCFC;

uint32_t MakeDevAddr(uint8_t bus, uint8_t slot, uint8_t func)
{
	//assert(bus < 1UL<<8UL); // bus is 8 bit anyways.
	assert(slot < 1UL<<5UL);
	assert(func < 1UL<<3UL);
	return func << 8U | slot << 11U | bus << 16U | 1 << 31U;
}

void SplitDevAddr(uint32_t devaddr, uint8_t* vals /* bus, slot, func */)
{
	vals[0] = devaddr >> 16U & ((1UL<<8UL)-1);
	vals[1] = devaddr >> 11U & ((1UL<<3UL)-1);
	vals[2] = devaddr >>  8U & ((1UL<<5UL)-1);
}

uint32_t ReadRaw32(uint32_t devaddr, uint8_t off)
{
	assert((off & 0x3) == 0);
	outport32(CONFIG_ADDRESS, devaddr + off);
	return inport32(CONFIG_DATA);
}

void WriteRaw32(uint32_t devaddr, uint8_t off, uint32_t val)
{
	assert((off & 0x3) == 0);
	outport32(CONFIG_ADDRESS, devaddr + off);
	outport32(CONFIG_DATA, val);
}

uint32_t Read32(uint32_t devaddr, uint8_t off)
{
	return le32toh(ReadRaw32(devaddr, off));
}

void Write32(uint32_t devaddr, uint8_t off, uint32_t val)
{
	WriteRaw32(devaddr, off, htole32(val));
}

void Write16(uint32_t devaddr, uint8_t off, uint16_t val)
{
	assert((off & 0x1) == 0);
	uint8_t alignedoff = off & ~0x3;
	union { uint8_t val8[4]; uint32_t val32; };
	val32 = ReadRaw32(devaddr, alignedoff);
	val8[(off & 0x3) + 0] = val >> 0 & 0xFF;
	val8[(off & 0x3) + 1] = val >> 8 & 0xFF;
	WriteRaw32(devaddr, alignedoff, val32);
}

uint16_t Read16(uint32_t devaddr, uint8_t off)
{
	assert((off & 0x1) == 0);
	uint8_t alignedoff = off & ~0x3;
	union { uint8_t val8[4]; uint32_t val32; };
	val32 = ReadRaw32(devaddr, alignedoff);
	return (uint16_t) val8[(off & 0x3) + 0] << 0 |
	       (uint16_t) val8[(off & 0x3) + 1] << 8;
}

void Write8(uint32_t devaddr, uint8_t off, uint8_t val)
{
	uint8_t alignedoff = off & ~0x3;
	union { uint8_t val8[4]; uint32_t val32; };
	val32 = ReadRaw32(devaddr, alignedoff);
	val8[(off & 0x3)] = val;
	WriteRaw32(devaddr, alignedoff, val32);
}

uint8_t Read8(uint32_t devaddr, uint8_t off)
{
	uint8_t alignedoff = off & ~0x3;
	union { uint8_t val8[4]; uint32_t val32; };
	val32 = ReadRaw32(devaddr, alignedoff);
	return val8[(off & 0x3)];
}

uint32_t CheckDevice(uint8_t bus, uint8_t slot, uint8_t func)
{
	return Read32(MakeDevAddr(bus, slot, func), 0x0);
}

pciid_t GetDeviceId(uint32_t devaddr)
{
	pciid_t ret;
	ret.deviceid = Read16(devaddr, PCIFIELD_DEVICE_ID);
	ret.vendorid = Read16(devaddr, PCIFIELD_VENDOR_ID);
	return ret;
}

pcitype_t GetDeviceType(uint32_t devaddr)
{
	pcitype_t ret;
	ret.classid = Read8(devaddr, PCIFIELD_CLASS);
	ret.subclassid = Read8(devaddr, PCIFIELD_SUBCLASS);
	ret.progif = Read8(devaddr, PCIFIELD_PROG_IF);
	ret.revid = Read8(devaddr, PCIFIELD_REVISION_ID);
	return ret;
}

static void MakeCoarsePattern(pcifind_t* coarse,
                              const pcifind_t* patterns,
                              size_t pattern_count)
{
	if ( pattern_count < 1 )
	{
		coarse->vendorid = 0xffff;
		coarse->deviceid = 0xffff;
		coarse->classid = 0xff;
		coarse->subclassid = 0xff;
		coarse->progif = 0xff;
		coarse->revid = 0xff;
		return;
	}
	const pcifind_t* first = patterns;
	coarse->vendorid = first->vendorid;
	coarse->deviceid = first->deviceid;
	coarse->classid = first->classid;
	coarse->subclassid = first->subclassid;
	coarse->progif = first->progif;
	coarse->revid = first->revid;
	for ( size_t i = 1; i < pattern_count; i++ )
	{
		const pcifind_t* pattern = patterns + i;
		if ( coarse->vendorid != pattern->vendorid )
			coarse->vendorid = 0xffff;
		if ( coarse->deviceid != pattern->deviceid )
			coarse->deviceid = 0xffff;
		if ( coarse->classid != pattern->classid )
			coarse->classid = 0xff;
		if ( coarse->subclassid != pattern->subclassid )
			coarse->subclassid = 0xff;
		if ( coarse->progif != pattern->progif )
			coarse->progif = 0xff;
		if ( coarse->revid != pattern->revid )
			coarse->revid = 0xff;
	}
}

static bool MatchesPattern(const pciid_t* id,
                           const pcitype_t* type,
                           const pcifind_t* pattern)
{
	if ( id->vendorid == 0xFFFF && id->deviceid == 0xFFFF )
		return false;
	if ( pattern->vendorid != 0xFFFF && id->vendorid != pattern->vendorid )
		return false;
	if ( pattern->deviceid != 0xFFFF && id->deviceid != pattern->deviceid )
		return false;
	if ( pattern->classid != 0xFF && type->classid != pattern->classid )
		return false;
	if ( pattern->subclassid != 0xFF &&
	     type->subclassid != pattern->subclassid )
		return false;
	if ( pattern->progif != 0xFF && type->progif != pattern->progif )
		return false;
	if ( pattern->revid != 0xFF && type->revid != pattern->revid )
		return false;
	return true;
}

static const pcifind_t* MatchesPatterns(const pciid_t* id,
                                        const pcitype_t* type,
                                        const pcifind_t* patterns,
                                        size_t pattern_count)
{
	if ( id->vendorid == 0xFFFF || id->deviceid == 0xFFFF )
		return NULL;
	for ( size_t i = 0; i < pattern_count; i++ )
	{
		const pcifind_t* pattern = &patterns[i];
		if ( MatchesPattern(id, type, pattern) )
			return pattern;
	}
	return NULL;
}

static bool SearchBus(bool (*callback)(uint32_t,
                                       const pciid_t*,
                                       const pcitype_t*,
                                       void*,
                                       void*),
                      void* context,
                      const pcifind_t* coarse_pattern,
                      const pcifind_t* patterns,
                      size_t pattern_count,
                      uint8_t bus)
{
	for ( unsigned int slot = 0; slot < 32; slot++ )
	{
		unsigned int num_functions = 1;
		for ( unsigned int function = 0; function < num_functions; function++ )
		{
			uint32_t devaddr = MakeDevAddr(bus, slot, function);
			pciid_t id = GetDeviceId(devaddr);
			if ( id.vendorid == 0xFFFF && id.deviceid == 0xFFFF )
				continue;
			pcitype_t type = GetDeviceType(devaddr);
			uint8_t header = Read8(devaddr, PCIFIELD_HEADER_TYPE);
			if ( header & 0x80 ) // Multi function device.
				num_functions = 8;
			if ( (header & 0x7F) == 0x01 ) // PCI to PCI bus.
			{
				uint8_t subbusid = Read8(devaddr, PCIFIELD_SECONDARY_BUS_NUMBER);
				bool search = SearchBus(callback, context, coarse_pattern,
				                        patterns, pattern_count, subbusid);
				if ( !search )
					return false;
			}
			// Do a coarse pattern before the more detailed one to save time.
			if ( 1 < pattern_count &&
			     !MatchesPattern(&id, &type, coarse_pattern) )
				continue;
			const pcifind_t* pattern =
				MatchesPatterns(&id, &type, patterns, pattern_count);
			if ( !pattern )
				continue;
			// Unlock PCI in this scope to allow the callback to lock and change
			// settings. Stop the search if the callback fails.
			kthread_mutex_unlock(&pci_lock);
			bool continue_search =
				callback(devaddr, &id, &type, context, pattern->context);
			kthread_mutex_lock(&pci_lock);
			if ( !continue_search )
				return false;
		}
	}
	return true;
}

void Search(bool (*callback)(uint32_t,
                             const pciid_t*,
                             const pcitype_t*,
                             void*,
                             void*),
            void* context,
            const pcifind_t* patterns,
            size_t pattern_count)
{
	pcifind_t coarse_pattern;
	MakeCoarsePattern(&coarse_pattern, patterns, pattern_count);
	ScopedLock lock(&pci_lock);
	SearchBus(callback, context, &coarse_pattern, patterns, pattern_count, 0);
}

// TODO: This iterates the whole PCI device tree on each call! Transition the
//       callers to use the new callback API and delete this API.
static uint32_t SearchForDevicesOnBus(uint8_t bus, pcifind_t pcifind, uint32_t last = 0)
{
	bool found_any_device = false;
	uint32_t next_device = 0;

	for ( unsigned int slot = 0; slot < 32; slot++ )
	{
		unsigned int num_functions = 1;
		for ( unsigned int function = 0; function < num_functions; function++ )
		{
			uint32_t devaddr = MakeDevAddr(bus, slot, function);
			pciid_t id = GetDeviceId(devaddr);
			if ( id.vendorid == 0xFFFF && id.deviceid == 0xFFFF )
				continue;
			pcitype_t type = GetDeviceType(devaddr);
			if ( last < devaddr &&
			     (!found_any_device || devaddr < next_device) &&
			     MatchesPattern(&id, &type, &pcifind) )
				next_device = devaddr, found_any_device = true;
			uint8_t header = Read8(devaddr, PCIFIELD_HEADER_TYPE);
			if ( header & 0x80 ) // Multi function device.
				num_functions = 8;
			if ( (header & 0x7F) == 0x01 ) // PCI to PCI bus.
			{
				uint8_t subbusid = Read8(devaddr, PCIFIELD_SECONDARY_BUS_NUMBER);
				uint32_t recret = SearchForDevicesOnBus(subbusid, pcifind, last);
				if ( last < recret &&
				     (!found_any_device || recret < next_device) )
					next_device = recret, found_any_device = true;
			}
		}
	}

	if ( !found_any_device )
		return 0;

	return next_device;
}

uint32_t SearchForDevices(pcifind_t pcifind, uint32_t last)
{
	// Search on bus 0 and recurse on other detected busses.
	return SearchForDevicesOnBus(0, pcifind, last);
}

pcibar_t GetBAR(uint32_t devaddr, uint8_t bar)
{
	ScopedLock lock(&pci_lock);

	uint32_t low = PCI::Read32(devaddr, 0x10 + 4 * (bar+0));

	pcibar_t result;
	result.addr_raw = low;
	result.size_raw = 0;
	if ( result.is_64bit() )
	{
		uint32_t high = PCI::Read32(devaddr, 0x10 + 4 * (bar+1));
		result.addr_raw |= (uint64_t) high << 32;
		PCI::Write32(devaddr, 0x10 + 4 * (bar+0), 0xFFFFFFFF);
		PCI::Write32(devaddr, 0x10 + 4 * (bar+1), 0xFFFFFFFF);
		uint32_t size_low = PCI::Read32(devaddr, 0x10 + 4 * (bar+0));
		uint32_t size_high = PCI::Read32(devaddr, 0x10 + 4 * (bar+1));
		PCI::Write32(devaddr, 0x10 + 4 * (bar+0), low);
		PCI::Write32(devaddr, 0x10 + 4 * (bar+1), high);
		result.size_raw = (uint64_t) size_high << 32 | (uint64_t) size_low << 0;
		result.size_raw = ~(result.size_raw & 0xFFFFFFFFFFFFFFF0) + 1;
	}
	else if ( result.is_32bit() )
	{
		PCI::Write32(devaddr, 0x10 + 4 * (bar+0), 0xFFFFFFFF);
		uint32_t size_low = PCI::Read32(devaddr, 0x10 + 4 * (bar+0));
		PCI::Write32(devaddr, 0x10 + 4 * (bar+0), low);
		result.size_raw = (uint64_t) size_low << 0;
		result.size_raw = ~(result.size_raw & 0xFFFFFFF0) + 1;
		result.size_raw &= 0xFFFFFFFF;
	}
	else if ( result.is_iospace() )
	{
		PCI::Write32(devaddr, 0x10 + 4 * (bar+0), 0xFFFFFFFF);
		uint32_t size_low = PCI::Read32(devaddr, 0x10 + 4 * (bar+0));
		PCI::Write32(devaddr, 0x10 + 4 * (bar+0), low);
		result.size_raw = (uint64_t) size_low << 0;
		result.size_raw = ~(result.size_raw & 0xFFFFFFFC) + 1;
		result.size_raw &= 0xFFFFFFFF;
	}

	return result;
}

pcibar_t GetExpansionROM(uint32_t devaddr)
{
	const uint32_t ROM_ADDRESS_MASK = ~UINT32_C(0x7FF);

	ScopedLock lock(&pci_lock);

	uint32_t low = PCI::Read32(devaddr, 0x30);
	PCI::Write32(devaddr, 0x30, ROM_ADDRESS_MASK | low);
	uint32_t size_low = PCI::Read32(devaddr, 0x30);
	PCI::Write32(devaddr, 0x30, low);

	pcibar_t result;
	result.addr_raw = (low & ROM_ADDRESS_MASK) | PCIBAR_TYPE_32BIT;
	result.size_raw = ~(size_low & ROM_ADDRESS_MASK) + 1;
	return result;
}

void EnableExpansionROM(uint32_t devaddr)
{
	ScopedLock lock(&pci_lock);
	PCI::Write32(devaddr, 0x30, PCI::Read32(devaddr, 0x30) | 0x1);
}

void DisableExpansionROM(uint32_t devaddr)
{
	ScopedLock lock(&pci_lock);
	PCI::Write32(devaddr, 0x30, PCI::Read32(devaddr, 0x30) & ~UINT32_C(0x1));
}

bool IsExpansionROMEnabled(uint32_t devaddr)
{
	ScopedLock lock(&pci_lock);
	return PCI::Read32(devaddr, 0x30) & 0x1;
}

static bool IsOkayInterruptLine(uint8_t line)
{
	if ( line == 0 )
		return false; // Conflict with PIT.
	if ( line == 2 )
		return false; // Cascade, can't be received.
	if ( 16 <= line )
		return false; // Not in set of valid IRQs.
	return true;
}

uint8_t SetupInterruptLine(uint32_t devaddr)
{
	ScopedLock lock(&pci_lock);
	uint8_t line = Read8(devaddr, PCIFIELD_INTERRUPT_LINE);
	if ( !IsOkayInterruptLine(line) )
		return 0;
	return Interrupt::IRQ0 + line;
}

void EnableBusMaster(uint32_t devaddr)
{
	ScopedLock lock(&pci_lock);
	uint16_t command = PCI::Read16(devaddr, PCIFIELD_COMMAND);
	PCI::Write16(devaddr, PCIFIELD_COMMAND,
		command | PCIFIELD_COMMAND_BUS_MASTER);
}

void DisableBusMaster(uint32_t devaddr)
{
	ScopedLock lock(&pci_lock);
	uint16_t command = PCI::Read16(devaddr, PCIFIELD_COMMAND);
	PCI::Write16(devaddr, PCIFIELD_COMMAND,
		command & ~PCIFIELD_COMMAND_BUS_MASTER);
}

void EnableMemoryWrite(uint32_t devaddr)
{
	ScopedLock lock(&pci_lock);
	uint16_t command = PCI::Read16(devaddr, PCIFIELD_COMMAND);
	PCI::Write16(devaddr, PCIFIELD_COMMAND,
		command | PCIFIELD_COMMAND_MEMORY_WRITE_AND_INVALIDATE);
}

void DisableMemoryWrite(uint32_t devaddr)
{
	ScopedLock lock(&pci_lock);
	uint16_t command = PCI::Read16(devaddr, PCIFIELD_COMMAND);
	PCI::Write16(devaddr, PCIFIELD_COMMAND,
		command & ~PCIFIELD_COMMAND_MEMORY_WRITE_AND_INVALIDATE);
}

void EnableInterruptLine(uint32_t devaddr)
{
	ScopedLock lock(&pci_lock);
	uint16_t command = PCI::Read16(devaddr, PCIFIELD_COMMAND);
	PCI::Write16(devaddr, PCIFIELD_COMMAND,
		command & ~PCIFIELD_COMMAND_INTERRUPT_DISABLE);
}

void DisableInterruptLine(uint32_t devaddr)
{
	ScopedLock lock(&pci_lock);
	uint16_t command = PCI::Read16(devaddr, PCIFIELD_COMMAND);
	PCI::Write16(devaddr, PCIFIELD_COMMAND,
		command | PCIFIELD_COMMAND_INTERRUPT_DISABLE);
}

uint8_t GetInterruptIndex(uint32_t devaddr)
{
	ScopedLock lock(&pci_lock);
	uint32_t line = PCI::Read8(devaddr, PCIFIELD_INTERRUPT_LINE) & 0xf;
	return Interrupt::IRQ0 + line;
}

static bool SeedRandom(uint32_t devaddr, const pciid_t* id,
                       const pcitype_t* type, void* /*context*/,
                       void* /*pattern_context*/)
{
	Random::Mix(Random::SOURCE_WEAK, &devaddr, sizeof(devaddr));
	Random::Mix(Random::SOURCE_WEAK, id, sizeof(*id));
	Random::Mix(Random::SOURCE_WEAK, type, sizeof(*type));
	return true;
}

void Init()
{
	pcifind_t everything(NULL, 0xFFFF, 0xFFFF);
	Search(SeedRandom, NULL, &everything, 1);
}

} // namespace PCI
} // namespace Sortix
