/*
 * Copyright (c) 2013, 2016 Jonas 'Sortie' Termansen.
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
 * sortix/kernel/segment.h
 * Structure representing a segment in a process.
 */

#ifndef INCLUDE_SORTIX_KERNEL_SEGMENT_H
#define INCLUDE_SORTIX_KERNEL_SEGMENT_H

#include <stddef.h>
#include <stdint.h>

#include <sortix/kernel/descriptor.h>

namespace Sortix {

class Process;

struct segment_location
{
	uintptr_t addr;
	size_t size;
};

class Segment : public segment_location
{
public:
	Segment() { } // For operator new[].
	Segment(uintptr_t addr, size_t size, int prot) :
		segment_location({addr, size}), prot(prot), desc(NULL), offset(0) { }
	Segment(uintptr_t addr, size_t size, int prot, Ref<Descriptor> desc, off_t offset) :
		segment_location({addr, size}), prot(prot), desc(desc), offset(offset) { }
	int prot;
	Ref<Descriptor> desc;
	off_t offset;
};

static inline int segmentcmp(const void* a_ptr, const void* b_ptr)
{
	const Segment* a = (const Segment*) a_ptr;
	const Segment* b = (const Segment*) b_ptr;
	return a->addr < b->addr ? -1 :
	       b->addr < a->addr ?  1 :
	       a->size < b->size ? -1 :
	       b->size < a->size ?  1 :
	                            0 ;
}

bool AreSegmentsOverlapping(const struct segment_location* a,
                            const struct segment_location* b);
bool IsUserspaceSegment(const Segment* segment);
Segment* FindOverlappingSegment(Process* process,
                                const struct segment_location* location);
bool IsSegmentOverlapping(Process* process, const segment_location* location);
bool AddSegment(Process* process, const Segment* new_segment);
bool PlaceSegment(struct segment_location* solution, Process* process,
                  void* addr_ptr, size_t size, int flags);
void UnmapSegment(Segment* segment);
void UnmapSegmentRange(Segment* segment, size_t offset, size_t size);

} // namespace Sortix

#endif
