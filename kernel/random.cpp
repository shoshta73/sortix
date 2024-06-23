/*
 * Copyright (c) 2014, 2015, 2016, 2024 Jonas 'Sortie' Termansen.
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
 * random.cpp
 * Kernel entropy gathering.
 */

#include <sys/mman.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <sortix/clock.h>
#include <sortix/limits.h>

#include <sortix/kernel/addralloc.h>
#include <sortix/kernel/copy.h>
#include <sortix/kernel/kernel.h>
#include <sortix/kernel/kthread.h>
#include <sortix/kernel/memorymanagement.h>
#include <sortix/kernel/random.h>
#include <sortix/kernel/syscall.h>
#include <sortix/kernel/time.h>

#include "multiboot.h"

// TODO: This implementation does not gather entropy. No claims are being made
//       whatsoever that this implementation is secure. It isn't secure.

namespace Sortix {
namespace Random {

static kthread_mutex_t entropy_lock = KTHREAD_MUTEX_INITIALIZER;
static unsigned char entropy[256];
static size_t entropy_used = 0;
static size_t entropy_available = 0;
static bool any_random_seed = false;
static bool fallback = false;

void Init(multiboot_info_t* bootinfo)
{
	size_t offset = 0;
	// TODO: This assumes the multiboot structures are accessible. That
	//       assumption is wrong in general and we should map them ourselves in
	//       manner that cannot fail.
	struct multiboot_mod_list* modules =
		(struct multiboot_mod_list*) (uintptr_t) bootinfo->mods_addr;
	for ( uint32_t i = 0; i < bootinfo->mods_count; i++ )
	{
		struct multiboot_mod_list* module = &modules[i];
		// TODO: This assumes module is mapped.
		size_t mod_size = module->mod_end - module->mod_start;
		const char* cmdline = (const char*) (uintptr_t) module->cmdline;
		// TODO: This assumes cmdline is mapped.
		if ( strcmp(cmdline, "--random-seed") != 0 )
			continue;
		any_random_seed = true;
		// TODO: Make an early map facility that cannot fail and use it to map
		//       the random seed.
		// TODO: AllocateKernelAddress might invoke randomness in some way in
		//       future.
		addralloc_t addralloc;
		if ( !AllocateKernelAddress(&addralloc, mod_size) )
			PanicF("Random::Init AllocateKernelAddress failed: %m");
		addr_t physfrom = module->mod_start;
		addr_t mapat = addralloc.from;
		for ( size_t i = 0; i < mod_size; i += Page::Size() )
		{
			if ( !Memory::Map(physfrom + i, mapat + i, PROT_KREAD | PROT_KWRITE) )
				PanicF("Random::Init Memory::Map failed: %m");
		}
		Memory::Flush();
		unsigned char* seed = (unsigned char*) addralloc.from;
		for ( size_t i = 0; i < mod_size; i++ )
		{
			entropy[offset++] ^= seed[i];
			if ( entropy_available < offset )
				entropy_available = offset;
			offset %= sizeof(entropy);
		}
		explicit_bzero(seed, mod_size);
		for ( size_t i = 0; i < mod_size; i += Page::Size() )
			Memory::Unmap(mapat + i);
		Memory::Flush();
		FreeKernelAddress(&addralloc);
	}
	fallback = entropy_available < sizeof(entropy);
}

int GetFallbackStatus()
{
	// If in fallback mode, mix in the current time. No particular reason.
	(void) arc4random();

	ScopedLock lock(&entropy_lock);
	if ( !any_random_seed )
		return 1;
	if ( fallback )
		return 2;
	return 0;
}

bool HasEntropy(size_t amount)
{
	ScopedLock lock(&entropy_lock);
	if ( amount <= entropy_available )
		return true;
	// Keep mixing fallback values (current time) in the hope it's better than
	// nothing.
	if ( fallback )
		return true;
	return false;
}

void GetEntropy(void* result, size_t size)
{
	kthread_mutex_lock(&entropy_lock);
	size_t amount = size < entropy_available ? size : entropy_available;
	memcpy(result, entropy + entropy_used, amount);
	explicit_bzero(entropy + entropy_used, amount);
	entropy_used += amount;
	entropy_available -= amount;
	kthread_mutex_unlock(&entropy_lock);
	// If more entropy is needed, mix in the current time and the time since
	// boot in the hope that the unpredictability of exactly when randomness is
	// consumed introduces some entropy.
	struct
	{
		struct timespec realtime;
		struct timespec monotonic;
	} seed;
	size_t sofar = amount;
	while ( sofar < size )
	{
		seed.realtime = Time::Get(CLOCK_REALTIME);
		seed.monotonic = Time::Get(CLOCK_MONOTONIC);
		unsigned char* out = (unsigned char*) result + sofar;
		size_t left = size - sofar;
		amount = left < sizeof(seed) ? left : sizeof(seed);
		memcpy(out, &seed, amount);
		sofar += amount;
	}
	explicit_bzero(&seed, sizeof(seed));
}

} // namespace Random
} // namespace Sortix

namespace Sortix {

int sys_getentropy(void* user_buffer, size_t size)
{
	unsigned char buffer[GETENTROPY_MAX];
	if ( sizeof(buffer) < size )
		return errno = EIO, -1;
	arc4random_buf(buffer, size);
	if ( !CopyToUser(user_buffer, buffer, size) )
		return -1;
	return 0;
}

} // namespace Sortix
