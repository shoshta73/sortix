/*
 * Copyright (c) 2015, 2016, 2024 Jonas 'Sortie' Termansen.
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
 * sortix/kernel/random.h
 * Kernel entropy gathering.
 */

#ifndef _INCLUDE_SORTIX_KERNEL_RANDOM_H
#define _INCLUDE_SORTIX_KERNEL_RANDOM_H

#include <stddef.h>

typedef struct multiboot_info multiboot_info_t;

namespace Sortix {
namespace Random {

enum Source
{
	// Random data provided from a previous boot.
	SOURCE_SEED,

	// Non-random data that an attacker would have difficulty guessing, such as
	// the exact hardware tree, MAC addresses, serial numbers, boot time, etc.
	SOURCE_WEAK,

	// Keyboard and mouse input and timing data.
	SOURCE_INPUT,

	// Network checksums and timing data.
	SOURCE_NETWORK,

	// Registers whenever a thread is preempted (interrupts disabled).
	SOURCE_PREEMPTION,

	// Details and timing data on interrupts (interrupts disabled).
	SOURCE_INTERRUPT,

	// Tasks and timing data in the interrupt worker thread.
	SOURCE_INTERRUPT_WORKER,

	SOURCE_MAX,
};

void Init(multiboot_info_t* bootinfo);
bool HasEntropy(size_t amount);
void GetEntropy(void* buffer, size_t size);
int GetFallbackStatus();
void Mix(enum Source source, const void* buffer, size_t size);
void MixNow(enum Source source);

} // namespace Random
} // namespace Sortix

#endif
