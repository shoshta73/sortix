/*
 * Copyright (c) 2014, 2015, 2016, 2024, 2025, 2026 Jonas 'Sortie' Termansen.
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

#include <assert.h>
#include <errno.h>
#include <sha2.h>
#include <stdlib.h>
#include <string.h>
#include <timespec.h>

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
#include "multiboot2.h"

// This is the entropy collection subsystem.
//
// Potentially weak and hostile entropy sources (such as the previous boot's
// entropy seed, hardware details and serial numbers, interrupt timing data and
// details, peripheral input, network checksums, registers on preemption, etc.)
// are mixed together into an entropy stream where an attacker cannot guess all
// of the data especially as the entropy collects over time.
//
// Incoming source data is written to its channel's ring buffer. If the buffer
// is full, then the new data is XOR'd into the buffer.
//
// Channels are mixed together by replacing each block in the channel buffer
// with its SHA256 digest and then XOR'ing together all of the channel buffers
// into a single entropy buffer.
//
// Random numbers are provided using the arc4random(3) functions which uses the
// entropy collected here. New entropy is stired together and used whenever:
//
//  - No entropy has ever been provided.
//  - getentropy(2) is being called and new entropy is available in any channel.
//  - Any channel has new entropy whose buffer has never filled.
//  - New entropy is available and one second has passed since the last stir.
//
// The random number generator is available immediately after boot, although it
// won't be strong until the random seed is mixed in, and if none is provided,
// then it will take some system activity for entropy to collect. The goal is
// to have strong entropy by the time a fresh interactive system installation is
// completed.

namespace Sortix {
namespace Random {

struct Channel
{
	unsigned char entropy[GETENTROPY_MAX];
	size_t collected;
	size_t offset;
	size_t total;
};

static Channel channels[SOURCE_MAX];

static kthread_mutex_t entropy_lock = KTHREAD_MUTEX_INITIALIZER;
static bool want_stir;
static bool has_any_entropy;
static bool has_new_record;
static struct timespec last_stir_at;

static void SeedModule(addr_t phys_from, size_t size)
{
	// TODO: Make an early map facility that cannot fail and use it to map
	//       the random seed.
	// TODO: AllocateKernelAddress might invoke randomness in some way in
	//       future.
	addralloc_t addralloc;
	if ( !AllocateKernelAddress(&addralloc, size) )
		PanicF("Random::SeedModule AllocateKernelAddress failed: %m");
	addr_t map_at = addralloc.from;
	for ( size_t i = 0; i < size; i += Page::Size() )
	{
		if ( !Memory::Map(phys_from + i, map_at + i, PROT_KREAD | PROT_KWRITE) )
			PanicF("Random::SeedModule Memory::Map failed: %m");
	}
	Memory::Flush();
	unsigned char* seed = (unsigned char*) addralloc.from;
	Mix(SOURCE_SEED, seed, size);
	explicit_bzero(seed, size);
	for ( size_t i = 0; i < size; i += Page::Size() )
		Memory::Unmap(map_at + i);
	Memory::Flush();
	FreeKernelAddress(&addralloc);
}

static void InitMultiboot(struct boot_info* boot_info)
{
	struct multiboot_info* multiboot = boot_info->multiboot;

	// Mix in the random seed if provided as a kernel module.
	// TODO: This assumes the multiboot structures are accessible. That
	//       assumption is wrong in general and we should map them ourselves in
	//       manner that cannot fail.
	struct multiboot_mod_list* modules =
		(struct multiboot_mod_list*) (uintptr_t) multiboot->mods_addr;
	for ( uint32_t i = 0; i < multiboot->mods_count; i++ )
	{
		struct multiboot_mod_list* module = &modules[i];
		// TODO: This assumes module is mapped.
		size_t mod_size = module->mod_end - module->mod_start;
		const char* cmdline = (const char*) (uintptr_t) module->cmdline;
		// TODO: This assumes cmdline is mapped.
		if ( strcmp(cmdline, "--random-seed") != 0 )
			continue;
		SeedModule(module->mod_start, mod_size);
	}
	Random::Mix(Random::SOURCE_WEAK, boot_info->cmdline,
	            strlen(boot_info->cmdline));
}

static void InitMultiboot2(struct boot_info* boot_info)
{
	struct multiboot2_tag* tag = multiboot2_tag_begin(boot_info->multiboot2);
	while ( tag )
	{
		if ( tag->type == MULTIBOOT2_TAG_TYPE_MODULE )
		{
			struct multiboot2_tag_module* module =
				(struct multiboot2_tag_module*) tag;
			if ( !strcmp(module->cmdline, "--random-seed") )
				SeedModule(module->mod_start,
				           module->mod_end - module->mod_start);
		}
		tag = multiboot2_tag_next(tag);
	}
	Random::Mix(Random::SOURCE_WEAK, boot_info->multiboot2,
	            boot_info->multiboot2->total_size);
}

void Init(struct boot_info* boot_info)
{
	if ( boot_info->multiboot )
		InitMultiboot(boot_info);
	else if ( boot_info->multiboot2 )
		InitMultiboot2(boot_info);
}

int GetFallbackStatus()
{
	ScopedLock lock(&entropy_lock);
	if ( channels[SOURCE_SEED].collected == 0 )
		return 1;
	else if ( channels[SOURCE_SEED].collected < GETENTROPY_MAX )
		return 2;
	else
		return 0;
}

void Mix(enum Source source, const void* ptr, size_t size)
{
	bool in_interrupt =
		source == SOURCE_PREEMPTION || source == SOURCE_INTERRUPT;
	const unsigned char* buffer = (const unsigned char*) ptr;
	Channel* channel = &channels[source];
	// TODO: Use a spin lock and rethink SOURCE_PREEMPTION when ready for SMP.
	bool locked = true;
	if ( in_interrupt )
		locked = kthread_mutex_trylock(&entropy_lock);
	else
		kthread_mutex_lock(&entropy_lock);
	size_t done = 0;
	// Repeatedly mix in the data into the ring buffer.
	while ( done < size )
	{
		size_t left = size - done;
		size_t available = sizeof(channel->entropy) - channel->offset;
		size_t count = left < available ? left : available;
		// XOR the new data into the ring buffer to preserve entropy already
		// there with fresh data that may be weak and hostile (but wouldn't know
		// the internal state of the channels).
		for ( size_t i = 0; i < count; i++ )
		{
			unsigned char input = channel->entropy[channel->offset + i];
			unsigned char seed = buffer[done + i];
			unsigned char output = input ^ seed;
			channel->entropy[channel->offset + i] = output;
		}
		channel->offset += count;
		// Stir the entropy if the channel is starting up and has new data.
		if ( locked && channel->collected < channel->offset )
		{
			channel->collected = channel->offset;
			has_new_record = true;
		}
		if ( channel->offset == sizeof(channel->entropy) )
			channel->offset = 0;
		channel->total += count;
		done += count;
	}
	if ( locked )
	{
		if ( done )
			has_any_entropy = true;
		kthread_mutex_unlock(&entropy_lock);
	}
}

void MixNow(enum Source source)
{
	// The exact uptime and estimated realtime of an event may not be known,
	// and whether the event occurred, but it largely may be guessed. Mixing in
	// enough of these slight unknowns will exponentially increase the
	// possibilities. The uptime is used here since it might not be public,
	// meanwhile the realtime is public, but the network time estimation will be
	// somewhat inaccurate. The lower bits of tv_nsec are likely to be the most
	// random and the higher bits of tv_sec rarely changed, so mix it together
	// into a 32-bit hash to avoid filling the channel with repetitive data.
	struct timespec rt = Time::Get(CLOCK_REALTIME);
	struct timespec bt = Time::Get(CLOCK_BOOTTIME);
	struct timespec sum = timespec_add(rt, bt);
	uint32_t hash = sum.tv_nsec ^ (sum.tv_sec >> 32) ^ sum.tv_sec;
	Mix(source, &hash, sizeof(hash));
}

bool HasEntropy(size_t amount)
{
	ScopedLock lock(&entropy_lock);
	// Stir fresh entropy for arc4random(3) on the conditions documented above.
	return amount <= GETENTROPY_MAX &&
	       has_any_entropy &&
	       (want_stir ||
	        has_new_record ||
	        timespec_le(timespec_make(1, 0),
	                    timespec_sub(Time::Get(CLOCK_BOOTTIME), last_stir_at)));
}

void GetEntropy(void* result, size_t size)
{
	unsigned char* output = (unsigned char*) result;
	// The channel ring buffer size must be a multiple of the SHA2 block size.
	assert(size <= GETENTROPY_MAX);
	static_assert(SHA256_DIGEST_LENGTH < GETENTROPY_MAX,
	              "SHA256_DIGEST_LENGTH < GETENTROPY_MAX");
	static_assert(GETENTROPY_MAX % SHA256_DIGEST_LENGTH == 0,
	              "GETENTROPY_MAX % SHA256_DIGEST_LENGTH == 0");
	// Mix all of the channels together into a single entropy buffer.
	unsigned char entropy[GETENTROPY_MAX];
	SHA2_CTX ctx;
	kthread_mutex_lock(&entropy_lock);
	for ( size_t i = 0; i < SOURCE_MAX; i++ )
	{
		Channel* channel = &channels[i];
		// SHA256 digest each block in the channel's buffer and replace the
		// block with the digest. This digest massively mixes the bits together
		// so they don't have obvious patterns, unlike the weak data presently
		// in the buffer. The newly hashed data remains in the buffer and will
		// be fit-flipped with XOR by new data being mixed in later.
		for ( size_t b = 0; b < GETENTROPY_MAX / SHA256_DIGEST_LENGTH; b++ )
		{
			size_t offset = b * SHA256_DIGEST_LENGTH;
			SHA256Init(&ctx);
			SHA256Update(&ctx, channel->entropy + offset, SHA256_DIGEST_LENGTH);
			SHA256Final(channel->entropy + offset, &ctx);
		}
		// XOR the combined entropy buffer with the channel's digested entropy.
		for ( size_t n = 0; n < GETENTROPY_MAX; n++ )
			entropy[n] ^= channel->entropy[n];
	}
	last_stir_at = Time::Get(CLOCK_BOOTTIME);
	has_any_entropy = false;
	has_new_record = false;
	want_stir = false;
	kthread_mutex_unlock(&entropy_lock);
	// Copy the entropy to the caller. Too much entropy was probably generated,
	// so repeatedly XOR the remaining entropy into the caller's buffer, so the
	// excess entropy isn't lost.
	memcpy(result, entropy, size);
	for ( size_t i = size, o = 0; i < GETENTROPY_MAX; i++ )
	{
		output[o++] ^= entropy[i];
		if ( o == size )
			o = 0;
	}
	explicit_bzero(&entropy, sizeof(entropy));
	explicit_bzero(&ctx, sizeof(ctx));
}

} // namespace Random
} // namespace Sortix

namespace Sortix {

int sys_getentropy(void* user_buffer, size_t size)
{
	unsigned char buffer[GETENTROPY_MAX];
	if ( sizeof(buffer) < size )
		return errno = EIO, -1;
	// Always stir in new entropy if any is available, since this syscall may be
	// called on system shutdown to get the best entropy gathered so far, so it
	// can be stored for the next boot.
	kthread_mutex_lock(&Random::entropy_lock);
	Random::want_stir = true;
	kthread_mutex_unlock(&Random::entropy_lock);
	arc4random_buf(buffer, size);
	if ( !CopyToUser(user_buffer, buffer, size) )
		return -1;
	return 0;
}

} // namespace Sortix
