/*
 * Copyright (c) 2025 Jonas 'Sortie' Termansen.
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
 * stdlib/aligned_alloc.c
 * Allocate aligned memory.
 */

#include <assert.h>
#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#if defined(HEAP_NO_ASSERT)
#define __heap_verify() ((void) 0)
#undef assert
#define assert(x) do { ((void) 0); } while ( 0 )
#endif

void* aligned_alloc(size_t alignment, size_t original_size)
{
	if ( !alignment ||
	     (alignment & (alignment - 1)) ||
	     SIZE_MAX / 2 <= alignment )
		return errno = EINVAL, (void*) NULL;

	if ( alignment < heap_align(1) )
		return malloc(original_size);

	// It's assumed 4 times this size is a power of two for alignment.
	static_assert(sizeof(struct heap_chunk) * 8 == 2 * __WORDSIZE,
		         "sizeof(struct heap_chunk) * 8 == 2 * __WORDSIZE");
	if ( alignment < 8 * __WORDSIZE )
		alignment = 8 * __WORDSIZE;

	if ( !heap_size_has_bin(original_size) )
		return errno = ENOMEM, (void*) NULL;

	// Allocate an extra large chunk that can be split into an unused padding
	// chunk and the real aligned chunk.
	size_t outer_size = 2 * sizeof(struct heap_chunk) +
	                    2 * sizeof(struct heap_chunk_post);
	size_t inner_size = heap_align(original_size);
	size_t chunk_size = outer_size;
	if ( __builtin_add_overflow(chunk_size, inner_size, &chunk_size) |
	     __builtin_add_overflow(chunk_size, alignment, &chunk_size) )
		return errno = ENOMEM, (void*) NULL;

	if ( !heap_size_has_bin(chunk_size) )
		return errno = ENOMEM, (void*) NULL;

	// Decide which bins are large enough for our allocation.
	size_t smallest_desirable_bin = heap_bin_for_allocation(chunk_size);
	size_t smallest_desirable_bin_size = heap_size_of_bin(smallest_desirable_bin);
	size_t desirable_bins = ~0UL << smallest_desirable_bin;

	__heap_lock();
	__heap_verify();

	// Determine whether there are any bins that we can use.
	size_t usable_bins = desirable_bins & __heap_state.bin_filled_bitmap;

	// If there are no usable bins, attempt to expand the current part of the
	// heap or create a new part.
	if ( !usable_bins && __heap_expand_current_part(smallest_desirable_bin_size) )
		usable_bins = desirable_bins & __heap_state.bin_filled_bitmap;

	// If we failed to expand the current part or make a new one - then we are
	// officially out of memory until someone deallocates something.
	if ( !usable_bins )
	{
		__heap_verify();
		__heap_unlock();
		return (void*) NULL;
	}

	// Pick the smallest of the usable bins.
	size_t bin_index = heap_bsf(usable_bins);

	// Pick the first element of this bins linked list. This is our allocation.
	struct heap_chunk* result_chunk = __heap_state.bin[bin_index];
	assert(result_chunk);
	assert(HEAP_IS_POINTER_ALIGNED(result_chunk, result_chunk->chunk_size));

	assert(chunk_size <= result_chunk->chunk_size);

	// Add a padding chunk if the chunk isn't aligned enough.
	uintptr_t beginning = (uintptr_t) heap_chunk_to_data(result_chunk);
	uintptr_t location = -(-beginning & ~(alignment - 1));
	if ( beginning != location )
	{
		struct heap_chunk* padding_chunk = result_chunk;
		size_t offset = location - (uintptr_t) padding_chunk;
		assert(heap_can_split_chunk(padding_chunk, offset));
		heap_split_chunk(padding_chunk, offset);
		result_chunk = heap_chunk_right(result_chunk);
	}

	assert(!((uintptr_t) heap_chunk_to_data(result_chunk) & (alignment - 1)));

	// Mark our chosen chunk as used and remove it from its bin.
	heap_remove_chunk(result_chunk);

	// Recompute how big the aligned chunk needs to be.
	outer_size = sizeof(struct heap_chunk) +
	             sizeof(struct heap_chunk_post);
	inner_size = heap_align(original_size);
	chunk_size = outer_size + inner_size;

	assert(chunk_size <= result_chunk->chunk_size);

	// If our chunk is larger than what we really needed and it is possible to
	// split the chunk into two, then we should split off a part of it and
	// return it to the heap for further allocation.
	if ( heap_can_split_chunk(result_chunk, chunk_size) )
		heap_split_chunk(result_chunk, chunk_size);

	__heap_verify();
	__heap_unlock();

	// Return the inner data associated with the chunk to the caller.
	return heap_chunk_to_data(result_chunk);
}
