/*
 * Copyright (c) 2011, 2012, 2013, 2015, 2016 Jonas 'Sortie' Termansen.
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
 * memorymanagement.cpp
 * Functions that allow modification of virtual memory.
 */

#include <sys/types.h>

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sortix/fcntl.h>
#include <sortix/mman.h>
#include <sortix/seek.h>

#include <sortix/kernel/copy.h>
#include <sortix/kernel/descriptor.h>
#include <sortix/kernel/inode.h>
#include <sortix/kernel/ioctx.h>
#include <sortix/kernel/kernel.h>
#include <sortix/kernel/memorymanagement.h>
#include <sortix/kernel/process.h>
#include <sortix/kernel/segment.h>
#include <sortix/kernel/syscall.h>
#include <sortix/kernel/vnode.h>

#include "fs/kram.h"

namespace Sortix {

int sys_memstat(size_t* memused, size_t* memtotal)
{
	size_t used;
	size_t total;
	Memory::Statistics(&used, &total);
	if ( memused && !CopyToUser(memused, &used, sizeof(used)) )
		return -1;
	if ( memtotal && !CopyToUser(memtotal, &total, sizeof(total)) )
		return -1;
	return 0;
}

} // namespace Sortix

namespace Sortix {
namespace Memory {

void UnmapMemory(Process* process, uintptr_t addr, size_t size)
{
	// process->segment_write_lock is held.
	// process->segment_lock is held.
	assert(Page::IsAligned(addr));
	assert(Page::IsAligned(size));
	assert(process == CurrentProcess());

	if ( UINTPTR_MAX - addr < size )
		size = Page::AlignDown(UINTPTR_MAX - addr);
	if ( !size )
		return;

	struct segment_location loc;
	loc.addr = addr;
	loc.size = size;
	while ( Segment* conflict = FindOverlappingSegment(process, &loc) )
	{
		// Delete the segment if covered entirely by our request.
		if ( addr <= conflict->addr && conflict->addr + conflict->size <= addr + size )
		{
			uintptr_t conflict_offset = (uintptr_t) conflict - (uintptr_t) process->segments;
			size_t conflict_index = conflict_offset / sizeof(Segment);
			UnmapSegment(conflict);
			conflict->~Segment();
			if ( conflict_index + 1 == process->segments_used )
			{
				process->segments_used--;
				continue;
			}
			Segment* old = &process->segments[--process->segments_used];
			Segment* dst = &process->segments[conflict_index];
			*dst = *old;
			old->~Segment();
			// TODO: It's wrong to qsort the Segment class.
			qsort(process->segments, process->segments_used,
			      sizeof(Segment), segmentcmp);
			continue;
		}

		// Delete the middle of the segment if covered there by our request.
		if ( conflict->addr < addr && addr + size - conflict->addr <= conflict->size )
		{
			UnmapSegmentRange(conflict, addr - conflict->addr, size);
			size_t new_addr = addr + size;
			size_t new_size = conflict->addr + conflict->size - (addr + size);
			off_t new_offset = conflict->offset + (new_addr - conflict->addr);
			Segment right_segment(new_addr, new_size, conflict->prot,
			                      conflict->desc, new_offset);
			conflict->size = addr - conflict->addr;
			// TODO: This shouldn't really fail as we free memory above, but
			//       this code isn't really provably reliable.
			if ( !AddSegment(process, &right_segment) )
				Panic("Unexpectedly unable to split memory mapped segment");
			continue;
		}

		// Delete the part of the segment covered partially from the left.
		if ( addr <= conflict->addr )
		{
			UnmapSegmentRange(conflict, 0, addr + size - conflict->addr);
			conflict->size += conflict->addr - (addr + size);
			conflict->offset += conflict->addr - (addr + size);
			conflict->addr = addr + size;
			continue;
		}

		// Delete the part of the segment covered partially from the right.
		if ( conflict->addr <= addr + size )
		{
			UnmapSegmentRange(conflict, addr - conflict->addr,
			                  conflict->addr + conflict->size - addr);
			conflict->size -= conflict->addr + conflict->size - addr;
			continue;
		}
	}
}

bool ProtectMemory(Process* process, uintptr_t addr, size_t size, int prot)
{
	// process->segment_write_lock is held.
	// process->segment_lock is held.
	assert(Page::IsAligned(addr));
	assert(Page::IsAligned(size));
	assert(process == CurrentProcess());

	// First split the segments overlapping with [addr, addr + size) into
	// smaller segments that doesn't cross addr and addr+size, while verifying
	// there are no gaps in that region. This is where the operation can fail as
	// the AddSegment call can run out of memory. There is no harm in splitting
	// the segments into smaller chunks.
	bool any_had_desc = false;
	for ( size_t offset = 0; offset < size; )
	{
		struct segment_location search_region;
		search_region.addr = addr + offset;
		search_region.size = Page::Size();
		Segment* segment = FindOverlappingSegment(process, &search_region);

		if ( !segment )
			return errno = EINVAL, false;

		if ( segment->desc )
			any_had_desc = true;

		// Split the segment into two if it begins before our search region.
		if ( segment->addr < search_region.addr )
		{
			size_t new_addr = search_region.addr;
			size_t new_size = segment->size + segment->addr - new_addr;
			size_t new_offset = segment->offset + segment->addr - new_addr;
			Segment new_segment(new_addr, new_size, segment->prot,
			                    segment->desc, new_offset);
			segment->size = search_region.addr - segment->addr;

			if ( !AddSegment(process, &new_segment) )
			{
				segment->size += new_segment.size;
				return false;
			}

			continue;
		}

		// Split the segment into two if it ends after addr + size.
		if ( size < segment->addr + segment->size - addr )
		{
			size_t new_addr = addr + size;
			size_t new_size = segment->size + segment->addr - new_addr;
			size_t new_offset = segment->offset + segment->addr - new_addr;
			Segment new_segment(new_addr, new_size, segment->prot,
			                    segment->desc, new_offset);
			segment->size = addr + size - segment->addr;

			if ( !AddSegment(process, &new_segment) )
			{
				segment->size += new_segment.size;
				return false;
			}

			continue;
		}

		offset += segment->size;
	}

	// Verify that any backing files allow the new protection.
	ioctx_t ctx; SetupUserIOCtx(&ctx);
	for ( size_t offset = 0; any_had_desc && offset < size; )
	{
		struct segment_location search_region;
		search_region.addr = addr + offset;
		search_region.size = Page::Size();
		Segment* segment = FindOverlappingSegment(process, &search_region);
		assert(segment);

		if ( segment->prot != prot &&
		     segment->desc &&
		     segment->desc->mprotect(&ctx, prot) < 0 )
			return false;
	}

	// Run through all the segments in the region [addr, addr+size) and change
	// the permissions and update the permissions of the virtual memory itself.
	for ( size_t offset = 0; offset < size; )
	{
		struct segment_location search_region;
		search_region.addr = addr + offset;
		search_region.size = Page::Size();
		Segment* segment = FindOverlappingSegment(process, &search_region);
		assert(segment);

		if ( segment->prot != prot )
		{
			// TODO: There is a moment of inconsistency here when the segment
			//       table itself has another protection written than what
			//       what applies to the actual pages.
			// TODO: SECURTIY: Does this have security implications?
			segment->prot = prot;
			for ( size_t i = 0; i < segment->size; i += Page::Size() )
				Memory::PageProtect(segment->addr + i, prot);
			Memory::Flush();
		}

		offset += segment->size;
	}

	return true;
}

bool MapMemory(Process* process, uintptr_t addr, size_t size, int prot)
{
	// process->segment_write_lock is held.
	// process->segment_lock is held.
	assert(Page::IsAligned(addr));
	assert(Page::IsAligned(size));
	assert(process == CurrentProcess());

	UnmapMemory(process, addr, size);

	Segment new_segment(addr, size, prot);

	if ( !MapRange(new_segment.addr, new_segment.size, new_segment.prot, PAGE_USAGE_USER_SPACE) )
		return false;
	Memory::Flush();

	if ( !AddSegment(process, &new_segment) )
	{
		UnmapSegment(&new_segment);
		return false;
	}

	// We have process->segment_write_lock locked, so we know that the memory in
	// user space exists and we can safely zero it here.
	// TODO: Another thread is able to see the old contents of the memory before
	//       we zero it causing potential information leaks.
	// TODO: SECURITY: Information leak.
	memset((void*) new_segment.addr, 0, new_segment.size);

	return true;
}

} // namespace Memory
} // namespace Sortix

namespace Sortix {

const int USER_SETTABLE_PROT = PROT_USER;
const int UNDERSTOOD_MMAP_FLAGS = MAP_SHARED |
                                  MAP_PRIVATE |
                                  MAP_ANONYMOUS |
                                  MAP_FIXED;

static
void* sys_mmap(void* addr_ptr, size_t size, int prot, int flags, int fd,
               off_t offset)
{
	// Verify that that the address is suitable aligned if fixed.
	uintptr_t addr = (uintptr_t) addr_ptr;
	if ( flags & MAP_FIXED && !Page::IsAligned(addr) )
		return errno = EINVAL, MAP_FAILED;
	// We don't allow zero-size mappings.
	if ( size == 0 )
		return errno = EINVAL, MAP_FAILED;
	// Verify that the user didn't request permissions not allowed.
	if ( prot & ~USER_SETTABLE_PROT )
		return errno = EINVAL, MAP_FAILED;
	// Verify that we understand all the flags we were passed.
	if ( flags & ~UNDERSTOOD_MMAP_FLAGS )
		return errno = EINVAL, MAP_FAILED;
	// Verify that MAP_PRIVATE and MAP_SHARED are not both set.
	if ( bool(flags & MAP_PRIVATE) == bool(flags & MAP_SHARED) )
		return errno = EINVAL, MAP_FAILED;
	// Verify the fíle descriptor and the offset is suitable set if needed.
	if ( !(flags & MAP_ANONYMOUS) &&
	     (fd < 0 || offset < 0 || (offset & (Page::Size()-1))) )
		return errno = EINVAL, MAP_FAILED;

	uintptr_t aligned_addr = Page::AlignDown(addr);
	uintptr_t aligned_size = Page::AlignUp(size);

	// Pick a good location near the end of user-space if no hint is given.
	if ( !(flags & MAP_FIXED) && !aligned_addr )
	{
		uintptr_t userspace_addr;
		size_t userspace_size;
		Memory::GetUserVirtualArea(&userspace_addr, &userspace_size);
		addr = aligned_addr =
			Page::AlignDown(userspace_addr + userspace_size - aligned_size);
	}

	// Verify that the offset + size doesn't overflow.
	if ( !(flags & MAP_ANONYMOUS) &&
	     (uintmax_t) (OFF_MAX - offset) < (uintmax_t) aligned_size )
		return errno = EOVERFLOW, MAP_FAILED;

	Process* process = CurrentProcess();

	// Verify whether the backing file is usable for memory mapping.
	ioctx_t ctx; SetupUserIOCtx(&ctx);
	Ref<Descriptor> desc;
	if ( flags & MAP_ANONYMOUS )
	{
		// Create an unnamed ramfs file to back this memory mapping.
		if ( flags & MAP_SHARED )
		{
			Ref<Inode> inode(new KRAMFS::File(INODE_TYPE_FILE, S_IFREG, 0, 0,
			                                  ctx.uid, ctx.gid, 0600));
			if ( !inode )
				return MAP_FAILED;
			Ref<Vnode> vnode(new Vnode(inode, Ref<Vnode>(), 0, 0));
			inode.Reset();
			if ( !vnode )
				return MAP_FAILED;
			desc = Ref<Descriptor>(new Descriptor(vnode, O_READ | O_WRITE));
			vnode.Reset();
			if ( !desc )
				return MAP_FAILED;
			if ( (uintmax_t) OFF_MAX < (uintmax_t) size )
				return errno = EOVERFLOW, MAP_FAILED;
			if ( desc->truncate(&ctx, size) < 0 )
				return MAP_FAILED;
			offset = 0;
		}
	}
	else
	{
		if ( !(desc = process->GetDescriptor(fd)) )
			return MAP_FAILED;
		// Verify if going through the inode mmap interface.
		if ( flags & MAP_SHARED )
		{
			if ( desc->mprotect(&ctx, prot) < 0 )
				return MAP_FAILED;
		}
		// Verify if not going through the inode mmap interface.
		else if ( flags & MAP_PRIVATE )
		{
			// Verify that the file is seekable.
			if ( desc->lseek(&ctx, 0, SEEK_CUR) < 0 )
				return errno = ENODEV, MAP_FAILED;
			// Verify that we have read access to the file.
			if ( desc->read(&ctx, NULL, 0) != 0 )
				return errno = EACCES, MAP_FAILED;
			// Verify that we have write access to the file if needed.
			if ( (prot & PROT_WRITE) && !(flags & MAP_PRIVATE) &&
				 desc->write(&ctx, NULL, 0) != 0 )
				return errno = EACCES, MAP_FAILED;
		}
	}

	if ( prot & PROT_READ )
		prot |= PROT_KREAD;
	if ( prot & PROT_WRITE )
		prot |= PROT_KWRITE;
	if ( flags & MAP_PRIVATE )
		prot |= PROT_FORK;

	ScopedLock lock1(&process->segment_write_lock);
	ScopedLock lock2(&process->segment_lock);

	// Determine where to put the new segment and its protection.
	struct segment_location location;
	if ( flags & MAP_FIXED )
	{
		location.addr = aligned_addr;
		location.size = aligned_size;
	}
	else if ( !PlaceSegment(&location, process, (void*) addr, aligned_size, flags) )
		return errno = ENOMEM, MAP_FAILED;

	if ( flags & MAP_SHARED )
	{
		assert(desc);

		Memory::UnmapMemory(process, location.addr, location.size);

		Segment new_segment(location.addr, 0, prot, desc, offset);

		while ( new_segment.size < location.size )
		{
			off_t offset;
			if ( __builtin_add_overflow(new_segment.offset, new_segment.size,
			                            &offset) )
			{
				errno = EOVERFLOW;
				Memory::Flush();
				UnmapSegment(&new_segment);
				return MAP_FAILED;
			}
			assert(!(offset & (Page::Size() - 1)));

			addr_t addr = desc->mmap(&ctx, offset);
			if ( !addr )
			{
				Memory::Flush();
				UnmapSegment(&new_segment);
				return MAP_FAILED;
			}
			uintptr_t virt = location.addr + new_segment.size;

			if ( !Memory::Map(addr, virt, prot) )
			{
				desc->munmap(&ctx, offset);
				Memory::Flush();
				UnmapSegment(&new_segment);
				return MAP_FAILED;
			}

			new_segment.size += Page::Size();
		}
		Memory::Flush();

		if ( !AddSegment(process, &new_segment) )
		{
			UnmapSegment(&new_segment);
			return MAP_FAILED;
		}
	}
	else
	{
		int first_prot = flags & MAP_ANONYMOUS ? prot : PROT_KWRITE | PROT_FORK;
		Segment new_segment(location.addr, location.size, first_prot);

		// Allocate a memory segment with the desired properties.
		if ( !Memory::MapMemory(process, new_segment.addr, new_segment.size,
		                        new_segment.prot) )
			return MAP_FAILED;

		// Read the file contents into the newly allocated memory.
		if ( !(flags & MAP_ANONYMOUS) )
		{
			// The pread will copy to user-space right requires this lock to be
			// free.
			lock2.Reset();

			ioctx_t kctx; SetupKernelIOCtx(&kctx);
			for ( size_t so_far = 0; so_far < aligned_size; )
			{
				uint8_t* ptr = (uint8_t*) (new_segment.addr + so_far);
				size_t left = aligned_size - so_far;
				off_t pos = offset + so_far;
				ssize_t num_bytes = desc->pread(&kctx, ptr, left, pos);
				if ( num_bytes < 0 )
				{
					// TODO: How should this situation be handled? For now we'll
					//       just ignore the error condition.
					errno = 0;
					break;
				}
				if ( !num_bytes )
				{
					// We got an unexpected early end-of-file condition, but
					// that's alright as the MapMemory call zero'd the new
					// memory and we are expected to zero the remainder.
					break;
				}
				so_far += num_bytes;
			}

			// Finally switch to the desired page protections.
			kthread_mutex_lock(&process->segment_lock);
			Memory::ProtectMemory(CurrentProcess(), new_segment.addr,
				                  new_segment.size, prot);
			kthread_mutex_unlock(&process->segment_lock);
		}
	}

	return (void*) location.addr;
}

int sys_mprotect(const void* addr_ptr, size_t size, int prot)
{
	// Verify that that the address is suitable aligned.
	uintptr_t addr = (uintptr_t) addr_ptr;
	if ( !Page::IsAligned(addr) )
		return errno = EINVAL, -1;
	// Verify that the user didn't request permissions not allowed.
	if ( prot & ~USER_SETTABLE_PROT )
		return errno = EINVAL, -1;

	size = Page::AlignUp(size);
	prot |= PROT_KREAD | PROT_KWRITE | PROT_FORK;

	Process* process = CurrentProcess();
	ScopedLock lock1(&process->segment_write_lock);
	ScopedLock lock2(&process->segment_lock);

	if ( !Memory::ProtectMemory(process, addr, size, prot) )
		return -1;

	return 0;
}

int sys_munmap(void* addr_ptr, size_t size)
{
	// Verify that that the address is suitable aligned.
	uintptr_t addr = (uintptr_t) addr_ptr;
	if ( !Page::IsAligned(addr) )
		return errno = EINVAL, -1;
	// We don't allow zero-size unmappings.
	if ( size == 0 )
		return errno = EINVAL, -1;

	size = Page::AlignUp(size);

	Process* process = CurrentProcess();
	ScopedLock lock1(&process->segment_write_lock);
	ScopedLock lock2(&process->segment_lock);

	Memory::UnmapMemory(process, addr, size);

	return 0;
}

// TODO: We use a wrapper system call here because there are too many parameters
//       to mmap for some platforms. We should extend the system call ABI so we
//       can do system calls with huge parameter lists and huge return values
//       portably - then we'll make sys_mmap use this mechanism if needed.

struct mmap_request /* duplicated in libc/sys/mman/mmap.cpp */
{
	void* addr;
	size_t size;
	int prot;
	int flags;
	int fd;
	off_t offset;
};

void* sys_mmap_wrapper(struct mmap_request* user_request)
{
	struct mmap_request request;
	if ( !CopyFromUser(&request, user_request, sizeof(request)) )
		return MAP_FAILED;
	return sys_mmap(request.addr, request.size, request.prot, request.flags,
	                request.fd, request.offset);
}

} // namespace Sortix
