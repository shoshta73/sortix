/*
 * Copyright (c) 2011-2012, 2014-2015, 2017, 2022-2026 Jonas 'Sortie' Termansen.
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
 * x86-family/memorymanagement.cpp
 * Handles memory for the x86 family of architectures.
 */

#include <assert.h>
#include <errno.h>
#include <string.h>

#include <sortix/mman.h>

#include <sortix/kernel/addralloc.h>
#include <sortix/kernel/kernel.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/memorymanagement.h>
#include <sortix/kernel/panic.h>
#include <sortix/kernel/pat.h>
#include <sortix/kernel/random.h>
#include <sortix/kernel/syscall.h>

#include "multiboot.h"
#include "multiboot2.h"
#include "memorymanagement.h"
#include "msr.h"

extern "C" unsigned char multiboot2_pages[2 * 4096];

namespace Sortix {

extern size_t end;

} // namespace Sortix

namespace Sortix {
namespace Page {

void InitPushRegion(addr_t position, size_t length);
size_t pagesnotonstack = 0;
size_t stackused = 0;
size_t stackreserved = 0;
size_t stacklength = 4096 / sizeof(addr_t);
size_t totalmem = 0;
size_t page_usage_counts[PAGE_USAGE_NUM_KINDS];
kthread_mutex_t pagelock = KTHREAD_MUTEX_INITIALIZER;

} // namespace Page
} // namespace Sortix

namespace Sortix {
namespace Memory {

addr_t PAT2PMLFlags[PAT_NUM];

static addr_t multiboot2_page = (addr_t) -1;
static size_t multiboot2_size;

// We need to map the arbitrarily sized sized multiboot information into virtual
// memory before we're able to allocate memory, since it provides the memory
// map. We need to operate on it using O(1) memory, and the solution is to
// simply map a window of it and act on at most a page worth of data at a time.
// Since the data structures are not always page aligned, we actually map two
// pages so it's always safe to access one page of data regardless of the offset
// inside the physical page.
static unsigned char* Multiboot2Map(addr_t physical)
{
	addr_t page = Page::AlignDown(physical);
	size_t offset = physical & (4096UL - 1);
	// Update the multiboot window if we need to map another page.
	if ( page != multiboot2_page )
	{
		int prot = PROT_KREAD;
		uintptr_t virt = (uintptr_t) multiboot2_pages;
		Memory::Map(page + 0 * 4096UL, virt + 0 * 4096UL, prot);
		Memory::Map(page + 1 * 4096UL, virt + 1 * 4096UL, prot);
		Flush();
		multiboot2_page = page;
	}
	return &multiboot2_pages[offset];
}

// Check whether an address conflicts with an used object in physical memory,
// and calculate the distance to the end of the conflict if conflicting, or the
// distance to the object if non-conflicting. This function lets us iterate the
// physical address space while skipping pages that are already used, while
// using O(1) memory before memory allocation is online.
static bool CheckUsedRange(addr_t test,
                           addr_t from_unaligned,
                           size_t size_unaligned,
                           size_t* dist_ptr)
{
	addr_t from = Page::AlignDown(from_unaligned);
	size_unaligned += from_unaligned - from;
	size_t size = Page::AlignUp(size_unaligned);
	if ( from <= test && test < from + size )
		return *dist_ptr = from + size - test, true;
	if ( test < from && from - test < *dist_ptr )
		*dist_ptr = from - test;
	return false;
}

// Check if an address collides with a string.
static bool CheckUsedString(addr_t test,
                            const char* string,
                            size_t* dist_ptr)
{
	size_t size = strlen(string) + 1;
	return CheckUsedRange(test, (addr_t) string, size, dist_ptr);
}

// Check if an address collides with the multiboot information.
static bool CheckUsedRangesMultiboot(struct multiboot_info* multiboot,
                                     addr_t test,
                                     size_t* dist_ptr)
{
	if ( CheckUsedRange(test, (addr_t) multiboot, sizeof(*multiboot), dist_ptr) )
		return true;

	const char* cmdline = (const char*) (uintptr_t) multiboot->cmdline;
	if ( CheckUsedString(test, cmdline, dist_ptr) )
		return true;

	size_t mods_size = multiboot->mods_count * sizeof(struct multiboot_mod_list);
	if ( CheckUsedRange(test, multiboot->mods_addr, mods_size, dist_ptr) )
		return true;

	struct multiboot_mod_list* modules =
		(struct multiboot_mod_list*) (uintptr_t) multiboot->mods_addr;
	for ( uint32_t i = 0; i < multiboot->mods_count; i++ )
	{
		struct multiboot_mod_list* module = &modules[i];
		assert(module->mod_start <= module->mod_end);
		size_t mod_size = module->mod_end - module->mod_start;
		if ( CheckUsedRange(test, module->mod_start, mod_size, dist_ptr) )
			return true;
		const char* mod_cmdline = (const char*) (uintptr_t) module->cmdline;
		if ( CheckUsedString(test, mod_cmdline, dist_ptr) )
			return true;
	}

	if ( CheckUsedRange(test, multiboot->mmap_addr, multiboot->mmap_length,
	                      dist_ptr) )
		return true;

	return false;
}

// Check if an address collides with the multiboot 2 information.
static bool CheckUsedRangesMultiboot2(struct multiboot2_info* multiboot2_phys,
                                      addr_t test,
                                      size_t* dist_ptr)
{
	addr_t physical = (addr_t) multiboot2_phys;
	unsigned char* ptr = Multiboot2Map(physical);
	struct multiboot2_info* multiboot2 = (struct multiboot2_info*) ptr;

	if ( CheckUsedRange(test, (addr_t) multiboot2_phys, multiboot2->total_size,
	                    dist_ptr) )
		return true;

	physical += sizeof(*multiboot2);
	physical = -(-physical & ~7UL);

	// Carefully iterate the multiboot 2 information using the multiboot window,
	// and check for collision with any known objects that we wish to save for
	// later. See the InitMultiboot2 comment for information on the approach.
	while ( true )
	{
		unsigned char* ptr = Multiboot2Map(physical);
		struct multiboot2_tag* tag = (struct multiboot2_tag*) ptr;
		if ( tag->type == 0 )
			break;
		if ( tag->type == MULTIBOOT2_TAG_TYPE_MODULE )
		{
			struct multiboot2_tag_module* module =
				(struct multiboot2_tag_module*) tag;
			assert(module->mod_start <= module->mod_end);
			size_t mod_size = module->mod_end - module->mod_start;
			if ( CheckUsedRange(test, module->mod_start, mod_size, dist_ptr) )
				return true;
		}
		physical += tag->size;
		physical = -(-physical & ~7UL);
	}

	return false;
}

// Check if an address collides with any objects we'll use later.
static bool CheckUsedRanges(struct boot_info* boot_info,
                            addr_t test,
                            size_t* dist_ptr)
{
	addr_t kernel_end = (addr_t) &end;
	if ( CheckUsedRange(test, 0, kernel_end, dist_ptr) )
		return true;

	if ( CheckUsedRange(test, (addr_t) boot_info, sizeof(*boot_info), dist_ptr) )
		return true;

	if ( boot_info->multiboot &&
	     CheckUsedRangesMultiboot(boot_info->multiboot, test, dist_ptr) )
		return true;

	if ( boot_info->multiboot2 &&
	     CheckUsedRangesMultiboot2(boot_info->multiboot2, test, dist_ptr) )
		return true;

	return false;
}

// A memory map region has been found, process it for page allocation.
static void OnMemoryRegion(struct boot_info* boot_info,
                           uint64_t addr,
                           uint64_t size,
                           uint32_t type)
{
	// Check that we can use this kind of RAM.
	if ( type != 1 )
		return;

	// Truncate the memory area if needed.
#if defined(__i386__)
	if ( 0xFFFFFFFFULL < addr )
		return;
	if ( 0xFFFFFFFFULL < addr + size )
		size = 0x100000000ULL - addr;
#endif

	// Properly page align the entry if needed.
	// TODO: Is the bootloader required to page align this? This could be
	//       raw BIOS data that might not be page aligned? But that would
	//       be a silly computer.
	addr_t base_unaligned = (addr_t) addr;
	addr_t base = Page::AlignUp(base_unaligned);
	if ( size < base - base_unaligned )
		return;
	size_t length_unaligned = size - (base - base_unaligned);
	size_t length = Page::AlignDown(length_unaligned);
	if ( !length )
		return;

	// Count the amount of usable RAM.
	Page::totalmem += length;

	// Give all the physical memory to the physical memory allocator, but make
	// sure not to give it things we already use.
	addr_t processed = base;
	while ( processed < base + length )
	{
		// If the address collides with an object, skip it, otherwise add the
		// memory until the next collision (if any).
		size_t distance = base + length - processed;
		if ( !CheckUsedRanges(boot_info, processed, &distance) )
			Page::InitPushRegion(processed, distance);
		processed += distance;
	}
}

// Iterate the memory map using the multiboot 1 information.
// TODO: This assumes the multiboot structures are accessible. That assumption
//       is wrong in general and we should map them ourselves in manner that
//       cannot fail. That's a bit tricky because multiboot structure contains
//       various pointers to physical memory. However, the multiboot 2
//       implementation does not have this problem and multiboot 1 support will
//       be removed after the next stable release.
static void InitMultiboot(struct boot_info* boot_info)
{
	struct multiboot_info* multiboot = boot_info->multiboot;

	if ( !(multiboot->flags & MULTIBOOT_INFO_MEM_MAP) )
		Panic("The memory map flag was't set in the multiboot structure.");

	typedef const multiboot_memory_map_t* mmap_t;

	// Loop over every detected memory region.
	for ( mmap_t mmap = (mmap_t) (addr_t) multiboot->mmap_addr;
	      (addr_t) mmap < multiboot->mmap_addr + multiboot->mmap_length;
	      mmap = (mmap_t) ((addr_t) mmap + mmap->size + sizeof(mmap->size)) )
	{
		Random::Mix(Random::SOURCE_WEAK, mmap, sizeof(*mmap));
		OnMemoryRegion(boot_info, mmap->addr, mmap->len, mmap->type);
	}
}

// Iterate the memory map using the multiboot 2 information.
static void InitMultiboot2(struct boot_info* boot_info)
{
	addr_t physical = (addr_t) boot_info->multiboot2;
	bool got_header = false;
	bool got_tag = false;
	size_t entries_left = 0;
	size_t entry_size = 0;

	// The multiboot 2 information has an arbitrary size, and we cannot allocate
	// memory yet because we don't know what memory is available yet, and we
	// can't map arbitrarily sized objects yet because that may require PMLs
	// that require pages (that we don't have yet). Fortunately we can iterate
	// the information using fixed sized objects that are smaller than the page
	// size using a sliding window mapping the multiboot information.
	while ( true )
	{
		unsigned char* ptr = Multiboot2Map(physical);
		// The header tells us the size of the multiboot 2 information, which is
		// stored for later, so we can map the entire object when memory
		// allocation is online.
		if ( !got_header )
		{
			struct multiboot2_info* multiboot2 = (struct multiboot2_info*) ptr;
			multiboot2_size = multiboot2->total_size;
			got_header = true;
			physical += sizeof(*multiboot2);
			physical = -(-physical & ~7UL);
			continue;
		}
		// Look for the memory map tag and skip past else.
		if ( !got_tag )
		{
			struct multiboot2_tag* tag = (struct multiboot2_tag*) ptr;
			if ( tag->type == 0 )
				Panic("The memory map wasn't in the multiboot2 structure");
			if ( tag->type != MULTIBOOT2_TAG_TYPE_MMAP )
			{
				physical += tag->size;
				physical = -(-physical & ~7UL);
				continue;
			}
			struct multiboot2_tag_mmap* mmap =
				(struct multiboot2_tag_mmap*) tag;
			physical += sizeof(*mmap);
			entry_size = mmap->entry_size;
			entries_left = (mmap->size - sizeof(*mmap)) / entry_size;
			got_tag = true;
			continue;
		}
		// Process one memory map entry at a time. The entries are 24 bytes,
		// which may cross a page boundary, which is why Multiboot2Map uses two
		// pages to ensure we can always access up to one page worth of data
		// from our current physical offset.
		if ( !entries_left )
			break;
		struct multiboot2_mmap_entry* entry =
			(struct multiboot2_mmap_entry*) ptr;
		OnMemoryRegion(boot_info, entry->addr, entry->len, entry->type);
		physical += entry_size;
		entries_left--;
	}
}

// Initialize multiboot 1 things now that memory allocation is online.
static void PostInitMultiboot(struct boot_info* boot_info)
{
	// Map the kernel command line into memory.
	uintptr_t physical = (addr_t) boot_info->multiboot->cmdline;
	uintptr_t info_page = Page::AlignDown(physical);
	uintptr_t info_offset = physical & (4096UL - 1);
	size_t cmdline_limit = 16 * Page::Size();
	uintptr_t info_size = Page::AlignUp(info_offset + cmdline_limit);
	addralloc_t alloc;
	if ( !AllocateKernelAddress(&alloc, info_size) )
		Panic("Failed to allocate virtual space for multiboot cmdline");
	for ( size_t i = 0; i < info_size; i += Page::Size() )
	{
		int prot = PROT_KREAD;
		if ( !Memory::Map(info_page + i, alloc.from + i, prot) )
			Panic("Failed to memory map multiboot cmdline");
	}
	Flush();
	boot_info->cmdline = (char*) (alloc.from + info_offset);
}

// Initialize multiboot 2 things now that memory allocation is online.
static void PostInitMultiboot2(struct boot_info* boot_info)
{
	// Map the entire multiboot 2 information into memory.
	uintptr_t physical = (addr_t) boot_info->multiboot2;
	uintptr_t info_page = Page::AlignDown(physical);
	uintptr_t info_offset = physical & (4096UL - 1);
	uintptr_t info_size = Page::AlignUp(info_offset + multiboot2_size);
	addralloc_t alloc;
	if ( !AllocateKernelAddress(&alloc, info_size) )
		Panic("Failed to allocate virtual space for multiboot information");
	for ( size_t i = 0; i < info_size; i += Page::Size() )
	{
		int prot = PROT_KREAD;
		if ( !Memory::Map(info_page + i, alloc.from + i, prot) )
			Panic("Failed to memory map multiboot information");
	}
	Flush();
	struct multiboot2_info* multiboot2 =
		(struct multiboot2_info*) (alloc.from + info_offset);
	boot_info->multiboot2 = multiboot2;

	// Locate the kernel command line.
	struct multiboot2_tag_string* cmdline_tag = (struct multiboot2_tag_string*)
		multiboot2_tag_lookup(multiboot2, MULTIBOOT2_TAG_TYPE_CMDLINE);
	boot_info->cmdline = cmdline_tag ? cmdline_tag->string : "";
}

// Initialize memory allocation using the boot information.
void Init(struct boot_info* boot_info)
{
	// If supported, setup the Page Attribute Table feature that allows
	// us to control the memory type (caching) of memory more precisely.
	if ( IsPATSupported() )
	{
		InitializePAT();
		for ( addr_t i = 0; i < PAT_NUM; i++ )
			PAT2PMLFlags[i] = EncodePATAsPMLFlag(i);
	}
	// Otherwise, reroute all requests to the backwards compatible scheme.
	// TODO: Not all early 32-bit x86 CPUs supports these values.
	else
	{
		PAT2PMLFlags[PAT_UC] = PML_WRTHROUGH | PML_NOCACHE;
		PAT2PMLFlags[PAT_WC] = PML_WRTHROUGH | PML_NOCACHE; // Approx.
		PAT2PMLFlags[2] = 0; // No such flag.
		PAT2PMLFlags[3] = 0; // No such flag.
		PAT2PMLFlags[PAT_WT] = PML_WRTHROUGH;
		PAT2PMLFlags[PAT_WP] = PML_WRTHROUGH; // Approx.
		PAT2PMLFlags[PAT_WB] = 0;
		PAT2PMLFlags[PAT_UCM] = PML_NOCACHE;
	}

	// Detect available memory using the boot protocol in use.
	if ( boot_info->multiboot )
		InitMultiboot(boot_info);
	else if ( boot_info->multiboot2 )
		InitMultiboot2(boot_info);

	// Prepare the non-forkable kernel PMLs such that forking the kernel address
	// space will always keep the kernel mapped.
	for ( size_t i = ENTRIES / 2; i < ENTRIES; i++ )
	{
		PML* const pml = PMLS[TOPPMLLEVEL];
		if ( pml->entry[i] & PML_PRESENT )
			continue;

		addr_t page = Page::Get(PAGE_USAGE_PAGING_OVERHEAD);
		if ( !page )
			Panic("Out of memory allocating boot PMLs.");

		pml->entry[i] = page | PML_WRITABLE | PML_PRESENT;

		// Invalidate the new PML and reset it to zeroes.
		addr_t pmladdr = (addr_t) (PMLS[TOPPMLLEVEL-1] + i);
		InvalidatePage(pmladdr);
		memset((void*) pmladdr, 0, sizeof(PML));
	}

	// Memory allocation is now online and the boot protocol can now allocate.
	if ( boot_info->multiboot )
		PostInitMultiboot(boot_info);
	else if ( boot_info->multiboot2 )
		PostInitMultiboot2(boot_info);

	// The physical pages in the location of the virtual address space for the
	// multiboot window are actually never used and can be allocated.
	uintptr_t mb2_pages = (uintptr_t) multiboot2_pages;
	Unmap(mb2_pages + 0);
	Unmap(mb2_pages + 4096);
	Page::Put(mb2_pages + 0, PAGE_USAGE_WASNT_ALLOCATED);
	Page::Put(mb2_pages + 4096, PAGE_USAGE_WASNT_ALLOCATED);
}

void Statistics(size_t* used, size_t* total, size_t* purposes)
{
	ScopedLock lock(&Page::pagelock);
	size_t memfree = (Page::stackused - Page::stackreserved) << 12UL;
	size_t memused = Page::totalmem - memfree;
	if ( used )
		*used = memused;
	if ( total )
		*total = Page::totalmem;
	if ( purposes )
	{
		for ( size_t i = 0; i < PAGE_USAGE_NUM_KINDS; i++ )
			purposes[i] = Page::page_usage_counts[i] << 12UL;
	}
}

} // namespace Memory
} // namespace Sortix

namespace Sortix {
namespace Page {

void PageUsageRegisterUse(addr_t where, enum page_usage usage)
{
	if ( PAGE_USAGE_NUM_KINDS <= usage )
		return;
	(void) where;
	page_usage_counts[usage]++;
}

void PageUsageRegisterFree(addr_t where, enum page_usage usage)
{
	if ( PAGE_USAGE_NUM_KINDS <= usage )
		return;
	(void) where;
	assert(page_usage_counts[usage] != 0);
	page_usage_counts[usage]--;
}

void ExtendStack()
{
	// This call will always succeed, if it didn't, then the stack
	// wouldn't be full, and thus this function won't be called.
	addr_t page = GetUnlocked(PAGE_USAGE_PHYSICAL);

	// This call will also succeed, since there are plenty of physical
	// pages available and it might need some.
	addr_t virt = (addr_t) (STACK + stacklength);
	if ( !Memory::Map(page, virt, PROT_KREAD | PROT_KWRITE) )
		Panic("Unable to extend page stack, which should have worked");

	// TODO: This may not be needed during the boot process!
	//Memory::InvalidatePage((addr_t) (STACK + stacklength));

	stacklength += 4096UL / sizeof(addr_t);
}

void InitPushRegion(addr_t position, size_t length)
{
	// Align our entries on page boundaries.
	addr_t newposition = Page::AlignUp(position);
	length = Page::AlignDown((position + length) - newposition);
	position = newposition;

	while ( length )
	{
		if ( unlikely(stackused == stacklength) )
		{
			if ( stackused == MAXSTACKLENGTH )
			{
				pagesnotonstack += length / 4096UL;
				return;
			}

			ExtendStack();
		}

		addr_t* stackentry = &(STACK[stackused++]);
		*stackentry = position;

		length -= 4096UL;
		position += 4096UL;
	}
}

bool ReserveUnlocked(size_t* counter, size_t least, size_t ideal)
{
	assert(least < ideal);
	size_t available = stackused - stackreserved;
	if ( least < available )
		return errno = ENOMEM, false;
	if ( available < ideal )
		ideal = available;
	stackreserved += ideal;
	*counter += ideal;
	return true;
}

bool Reserve(size_t* counter, size_t least, size_t ideal)
{
	ScopedLock lock(&pagelock);
	return ReserveUnlocked(counter, least, ideal);
}

bool ReserveUnlocked(size_t* counter, size_t amount)
{
	return ReserveUnlocked(counter, amount, amount);
}

bool Reserve(size_t* counter, size_t amount)
{
	ScopedLock lock(&pagelock);
	return ReserveUnlocked(counter, amount);
}

addr_t GetReservedUnlocked(size_t* counter, enum page_usage usage)
{
	if ( !*counter )
		return 0;
	assert(stackused); // After all, we did _reserve_ the memory.
	addr_t result = STACK[--stackused];
	assert(result == AlignDown(result));
	stackreserved--;
	(*counter)--;
	PageUsageRegisterUse(result, usage);
	return result;
}

addr_t GetReserved(size_t* counter, enum page_usage usage)
{
	ScopedLock lock(&pagelock);
	return GetReservedUnlocked(counter, usage);
}

addr_t GetUnlocked(enum page_usage usage)
{
	assert(stackreserved <= stackused);
	if ( unlikely(stackreserved == stackused) )
		return errno = ENOMEM, 0;
	addr_t result = STACK[--stackused];
	assert(result == AlignDown(result));
	PageUsageRegisterUse(result, usage);
	return result;
}

addr_t Get(enum page_usage usage)
{
	ScopedLock lock(&pagelock);
	return GetUnlocked(usage);
}

// TODO: This competes with the normal allocation for precious 32-bit pages, we
//       should use different pools for this, and preferably preallocate some
//       32-bit pages exclusively for driver usage. Also, get proper hardware
//       without these issues.
addr_t Get32BitUnlocked(enum page_usage usage)
{
	assert(stackreserved <= stackused);
	if ( unlikely(stackreserved == stackused) )
		return errno = ENOMEM, 0;
	for ( size_t ii = stackused; 0 < ii; ii-- )
	{
		size_t i = ii - 1;
		addr_t result = STACK[i];
		assert(result == AlignDown(result));
		if ( 4 < sizeof(void*) && UINT32_MAX < result )
			continue;
		if ( i + 1 != stackused )
		{
			STACK[i] = STACK[stackused - 1];
			STACK[stackused - 1] = result;
		}
		stackused--;
		PageUsageRegisterUse(result, usage);
		return result;
	}
	return errno = ENOMEM, 0;
}

addr_t Get32Bit(enum page_usage usage)
{
	ScopedLock lock(&pagelock);
	return Get32BitUnlocked(usage);
}

void PutUnlocked(addr_t page, enum page_usage usage)
{
	assert(page == AlignDown(page));
	if ( unlikely(stackused == stacklength) )
	{
		if ( stackused == MAXSTACKLENGTH )
		{
			pagesnotonstack++;
			return;
		}
		ExtendStack();
	}
	STACK[stackused++] = page;
	PageUsageRegisterFree(page, usage);
}

void Put(addr_t page, enum page_usage usage)
{
	ScopedLock lock(&pagelock);
	PutUnlocked(page, usage);
}

void Lock()
{
	kthread_mutex_lock(&pagelock);
}

void Unlock()
{
	kthread_mutex_unlock(&pagelock);
}

} // namespace Page
} // namespace Sortix

namespace Sortix {
namespace Memory {

addr_t ProtectionToPMLFlags(int prot)
{
	addr_t result = PML_NX;
	if ( prot & PROT_EXEC )
	{
		result |= PML_USERSPACE;
		result &= ~PML_NX;
	}
	if ( prot & PROT_READ )
		result |= PML_USERSPACE;
	if ( prot & PROT_WRITE )
		result |= PML_USERSPACE | PML_WRITABLE;
	if ( prot & PROT_KEXEC )
		result &= ~PML_NX;
	if ( prot & PROT_KREAD )
		result |= 0;
	if ( prot & PROT_KWRITE )
		result |= PML_WRITABLE;
	if ( prot & PROT_FORK )
		result |= PML_FORK;
	return result;
}

int PMLFlagsToProtection(addr_t flags)
{
	int prot = PROT_KREAD;
	if ( (flags & PML_USERSPACE) && !(flags & PML_NX) )
		prot |= PROT_EXEC;
	if ( (flags & PML_USERSPACE) )
		prot |= PROT_READ;
	if ( (flags & PML_USERSPACE) && (flags & PML_WRITABLE) )
		prot |= PROT_WRITE;
	if ( !(flags & PML_NX) )
		prot |= PROT_KEXEC;
	if ( flags & PML_WRITABLE )
		prot |= PROT_KWRITE;
	if ( flags & PML_FORK )
		prot |= PROT_FORK;
	return prot;
}

int ProvidedProtection(int prot)
{
	return PMLFlagsToProtection(ProtectionToPMLFlags(prot));
}

bool LookUp(addr_t mapto, addr_t* physical, int* protection)
{
	// Translate the virtual address into PML indexes.
	const size_t MASK = (1<<TRANSBITS)-1;
	size_t pmlchildid[TOPPMLLEVEL + 1];
	for ( size_t i = 1; i <= TOPPMLLEVEL; i++ )
		pmlchildid[i] = mapto >> (12 + (i-1) * TRANSBITS) & MASK;

	int prot = PROT_USER | PROT_KERNEL | PROT_FORK;

	// For each PML level, make sure it exists.
	size_t offset = 0;
	for ( size_t i = TOPPMLLEVEL; i > 1; i-- )
	{
		size_t childid = pmlchildid[i];
		PML* pml = PMLS[i] + offset;

		addr_t entry = pml->entry[childid];
		if ( !(entry & PML_PRESENT) )
			return false;
		addr_t entryflags = entry & ~PML_ADDRESS;
		int entryprot = PMLFlagsToProtection(entryflags);
		prot &= entryprot;

		// Find the index of the next PML in the fractal mapped memory.
		offset = offset * ENTRIES + childid;
	}

	addr_t entry = (PMLS[1] + offset)->entry[pmlchildid[1]];
	if ( !(entry & PML_PRESENT) )
		return false;

	addr_t entryflags = entry & ~PML_ADDRESS;
	int entryprot = PMLFlagsToProtection(entryflags);
	prot &= entryprot;
	addr_t phys = entry & PML_ADDRESS;

	if ( physical )
		*physical = phys;
	if ( protection )
		*protection = prot;

	return true;
}

void InvalidatePage(addr_t /*addr*/)
{
	// TODO: Actually just call the instruction.
	Flush();
}

addr_t GetAddressSpace()
{
	addr_t result;
	asm ( "mov %%cr3, %0" : "=r"(result) );
	return result;
}

addr_t SwitchAddressSpace(addr_t addrspace)
{
	assert(Page::IsAligned(addrspace));

	addr_t previous = GetAddressSpace();
	asm volatile ( "mov %0, %%cr3" : : "r"(addrspace) );
	return previous;
}

void Flush()
{
	addr_t previous;
	asm ( "mov %%cr3, %0" : "=r"(previous) );
	asm volatile ( "mov %0, %%cr3" : : "r"(previous) );
}

bool MapRange(addr_t where, size_t bytes, int protection, enum page_usage usage)
{
	for ( addr_t page = where; page < where + bytes; page += 4096UL )
	{
		addr_t physicalpage = Page::Get(usage);
		if ( physicalpage == 0 )
		{
			while ( where < page )
			{
				page -= 4096UL;
				physicalpage = Unmap(page);
				Page::Put(physicalpage, usage);
			}
			return false;
		}

		Map(physicalpage, page, protection);
	}

	return true;
}

bool UnmapRange(addr_t where, size_t bytes, enum page_usage usage)
{
	for ( addr_t page = where; page < where + bytes; page += 4096UL )
	{
		addr_t physicalpage = Unmap(page);
		if ( physicalpage )
			Page::Put(physicalpage, usage);
	}
	return true;
}

static bool MapInternal(addr_t physical, addr_t mapto, int prot, addr_t extraflags = 0)
{
	addr_t flags = ProtectionToPMLFlags(prot) | PML_PRESENT;

	// Translate the virtual address into PML indexes.
	const size_t MASK = (1<<TRANSBITS)-1;
	size_t pmlchildid[TOPPMLLEVEL + 1];
	for ( size_t i = 1; i <= TOPPMLLEVEL; i++ )
		pmlchildid[i] = mapto >> (12 + (i-1) * TRANSBITS) & MASK;

	// For each PML level, make sure it exists.
	size_t offset = 0;
	for ( size_t i = TOPPMLLEVEL; i > 1; i-- )
	{
		size_t childid = pmlchildid[i];
		PML* pml = PMLS[i] + offset;

		addr_t& entry = pml->entry[childid];

		// Find the index of the next PML in the fractal mapped memory.
		size_t childoffset = offset * ENTRIES + childid;

		if ( !(entry & PML_PRESENT) )
		{
			// TODO: Possible memory leak when page allocation fails.
			addr_t page = Page::Get(PAGE_USAGE_PAGING_OVERHEAD);

			if ( !page )
				return false;
			addr_t pmlflags = PML_PRESENT | PML_WRITABLE | PML_USERSPACE
			                | PML_FORK;
			entry = page | pmlflags;

			// Invalidate the new PML and reset it to zeroes.
			addr_t pmladdr = (addr_t) (PMLS[i-1] + childoffset);
			InvalidatePage(pmladdr);
			memset((void*) pmladdr, 0, sizeof(PML));
		}

		offset = childoffset;
	}

	// Actually map the physical page to the virtual page.
	const addr_t entry = physical | flags | extraflags;
	(PMLS[1] + offset)->entry[pmlchildid[1]] = entry;
	return true;
}

bool Map(addr_t physical, addr_t mapto, int prot)
{
	return MapInternal(physical, mapto, prot);
}

void PageProtect(addr_t mapto, int protection)
{
	addr_t phys;
	if ( !LookUp(mapto, &phys, NULL) )
		return;
	Map(phys, mapto, protection);
}

void PageProtectAdd(addr_t mapto, int protection)
{
	addr_t phys;
	int prot;
	if ( !LookUp(mapto, &phys, &prot) )
		return;
	prot |= protection;
	Map(phys, mapto, prot);
}

void PageProtectSub(addr_t mapto, int protection)
{
	addr_t phys;
	int prot;
	if ( !LookUp(mapto, &phys, &prot) )
		return;
	prot &= ~protection;
	Map(phys, mapto, prot);
}

addr_t Unmap(addr_t mapto)
{
	// Translate the virtual address into PML indexes.
	const size_t MASK = (1<<TRANSBITS)-1;
	size_t pmlchildid[TOPPMLLEVEL + 1];
	for ( size_t i = 1; i <= TOPPMLLEVEL; i++ )
	{
		pmlchildid[i] = mapto >> (12 + (i-1) * TRANSBITS) & MASK;
	}

	// For each PML level, make sure it exists.
	size_t offset = 0;
	for ( size_t i = TOPPMLLEVEL; i > 1; i-- )
	{
		size_t childid = pmlchildid[i];
		PML* pml = PMLS[i] + offset;

		addr_t& entry = pml->entry[childid];

		if ( !(entry & PML_PRESENT) )
			PanicF("Attempted to unmap virtual page 0x%jX, but the virtual"
			       " page was wasn't mapped. This is a bug in the code "
			       "code calling this function", (uintmax_t) mapto);

		// Find the index of the next PML in the fractal mapped memory.
		offset = offset * ENTRIES + childid;
	}

	addr_t& entry = (PMLS[1] + offset)->entry[pmlchildid[1]];
	addr_t result = entry & PML_ADDRESS;
	entry = 0;

	// TODO: If all the entries in PML[N] are not-present, then who
	// unmaps its entry from PML[N-1]?

	return result;
}

bool MapPAT(addr_t physical, addr_t mapto, int prot, addr_t mtype)
{
	addr_t extraflags = PAT2PMLFlags[mtype];
	return MapInternal(physical, mapto, prot, extraflags);
}

void ForkCleanup(size_t i, size_t level)
{
	PML* destpml = FORKPML + level;
	if ( !i )
		return;
	for ( size_t n = 0; n < i-1; n++ )
	{
		addr_t entry = destpml->entry[n];
		if ( !(entry & PML_FORK ) )
			continue;
		addr_t phys = entry & PML_ADDRESS;
		if ( 1 < level )
		{
			addr_t destaddr = (addr_t) (FORKPML + level-1);
			Map(phys, destaddr, PROT_KREAD | PROT_KWRITE);
			InvalidatePage(destaddr);
			ForkCleanup(ENTRIES+1UL, level-1);
		}
		enum page_usage usage = 1 < level ? PAGE_USAGE_PAGING_OVERHEAD
		                                  : PAGE_USAGE_USER_SPACE;
		Page::Put(phys, usage);
	}
}

// TODO: Copying every frame is endlessly useless in many uses. It'd be
// nice to upgrade this to a copy-on-write algorithm.
bool Fork(size_t level, size_t pmloffset)
{
	PML* destpml = FORKPML + level;
	for ( size_t i = 0; i < ENTRIES; i++ )
	{
		addr_t entry = (PMLS[level] + pmloffset)->entry[i];

		// Link the entry if it isn't supposed to be forked.
		if ( !(entry & PML_PRESENT) || !(entry & PML_FORK ) )
		{
			destpml->entry[i] = entry;
			continue;
		}

		enum page_usage usage = 1 < level ? PAGE_USAGE_PAGING_OVERHEAD
		                                  : PAGE_USAGE_USER_SPACE;
		addr_t phys = Page::Get(usage);
		if ( unlikely(!phys) )
		{
			ForkCleanup(i, level);
			return false;
		}

		addr_t flags = entry & PML_FLAGS;
		destpml->entry[i] = phys | flags;

		// Map the destination page.
		addr_t destaddr = (addr_t) (FORKPML + level-1);
		Map(phys, destaddr, PROT_KREAD | PROT_KWRITE);
		InvalidatePage(destaddr);

		size_t offset = pmloffset * ENTRIES + i;

		if ( 1 < level )
		{
			if ( !Fork(level-1, offset) )
			{
				Page::Put(phys, usage);
				ForkCleanup(i, level);
				return false;
			}
			continue;
		}

		// Determine the source page's address.
		const void* src = (const void*) (offset * 4096UL);

		// Determine the destination page's address.
		void* dest = (void*) (FORKPML + level - 1);

		memcpy(dest, src, 4096UL);
	}

	return true;
}

bool Fork(addr_t dir, size_t level, size_t pmloffset)
{
	PML* destpml = FORKPML + level;

	// This call always succeeds.
	Map(dir, (addr_t) destpml, PROT_KREAD | PROT_KWRITE);
	InvalidatePage((addr_t) destpml);

	return Fork(level, pmloffset);
}

// Create an exact copy of the current address space.
addr_t Fork()
{
	addr_t dir = Page::Get(PAGE_USAGE_PAGING_OVERHEAD);
	if ( dir == 0 )
		return 0;
	if ( !Fork(dir, TOPPMLLEVEL, 0) )
	{
		Page::Put(dir, PAGE_USAGE_PAGING_OVERHEAD);
		return 0;
	}

	// Now, the new top pml needs to have its fractal memory fixed.
	const addr_t flags = PML_PRESENT | PML_WRITABLE;
	addr_t mapto;
	addr_t childaddr;

	(FORKPML + TOPPMLLEVEL)->entry[ENTRIES-1] = dir | flags;
	childaddr = (FORKPML + TOPPMLLEVEL)->entry[ENTRIES-2] & PML_ADDRESS;

	for ( size_t i = TOPPMLLEVEL-1; i > 0; i-- )
	{
		mapto = (addr_t) (FORKPML + i);
		Map(childaddr, mapto, PROT_KREAD | PROT_KWRITE);
		InvalidatePage(mapto);
		(FORKPML + i)->entry[ENTRIES-1] = dir | flags;
		childaddr = (FORKPML + i)->entry[ENTRIES-2] & PML_ADDRESS;
	}
	return dir;
}

} // namespace Memory
} // namespace Sortix
