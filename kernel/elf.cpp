/*
 * Copyright (c) 2011, 2012, 2013, 2014, 2024 Jonas 'Sortie' Termansen.
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
 * elf.cpp
 * Load a program in the Executable and Linkable Format into this process.
 */

#include <sys/types.h>

#include <assert.h>
#include <elf.h>
#include <endian.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <system-elf.h>

#include <__/wordsize.h>

#include <sortix/mman.h>

#include <sortix/kernel/descriptor.h>
#include <sortix/kernel/elf.h>
#include <sortix/kernel/ioctx.h>
#include <sortix/kernel/kernel.h>
#include <sortix/kernel/memorymanagement.h>
#include <sortix/kernel/process.h>
#include <sortix/kernel/segment.h>

namespace Sortix {
namespace ELF {

static bool is_power_of_two(uintptr_t value)
{
	return value && !(value & (value - 1));
}

uintptr_t Load(Ref<Descriptor> program, Auxiliary* aux)
{
	static const uintmax_t off_max = OFF_MAX; // Silence false -Wtype-limits

	assert(program);

	ioctx_t ctx; SetupKernelIOCtx(&ctx);

	memset(aux, 0, sizeof(*aux));

	Process* process = CurrentProcess();

	uintptr_t userspace_addr;
	size_t userspace_size;
	Memory::GetUserVirtualArea(&userspace_addr, &userspace_size);
	uintptr_t userspace_end = userspace_addr + userspace_size;

	Elf_Ehdr ehdr;
	for ( size_t done = 0; done < sizeof(ehdr); )
	{
		ssize_t amount = program->pread(&ctx, (uint8_t*) &ehdr + done,
		                                sizeof(ehdr) - done, done);
		if ( amount < 0 )
			return 0;
		if ( !amount )
			return errno = ENOEXEC, 0;
		done += amount;
	}

	if ( memcmp(&ehdr, ELFMAG, SELFMAG) != 0 )
		return errno = ENOEXEC, 0;

#if __WORDSIZE == 32
	if ( ehdr.e_ident[EI_CLASS] != ELFCLASS32 )
		return errno = EINVAL, 0;
#elif __WORDSIZE == 64
	if ( ehdr.e_ident[EI_CLASS] != ELFCLASS64 )
		return errno = EINVAL, 0;
#else
#error "You need to add support for your elf class."
#endif

#if BYTE_ORDER == LITTLE_ENDIAN
	if ( ehdr.e_ident[EI_DATA] != ELFDATA2LSB )
		return errno = EINVAL, 0;
#elif BYTE_ORDER == BIG_ENDIAN
	if ( ehdr.e_ident[EI_DATA] != ELFDATA2MSB )
		return errno = EINVAL, 0;
#else
#error "You need to add support for your endian."
#endif

	if ( ehdr.e_ident[EI_VERSION] != EV_CURRENT )
		return errno = EINVAL, 0;

	if ( ehdr.e_ident[EI_OSABI] != ELFOSABI_SORTIX )
		return errno = EINVAL, 0;

	if ( ehdr.e_ident[EI_ABIVERSION] != 0 )
		return errno = EINVAL, 0;

	if ( ehdr.e_ehsize < sizeof(Elf_Ehdr) )
		return errno = EINVAL, 0;

#if defined(__i386__)
	if ( ehdr.e_machine != EM_386 )
		return errno = EINVAL, 0;
#elif defined(__x86_64__)
	if ( ehdr.e_machine != EM_X86_64 )
		return errno = EINVAL, 0;
#else
#error "Please recognize your processor in e_machine."
#endif

	if ( ehdr.e_type != ET_EXEC )
		return errno = EINVAL, 0;

	if ( ehdr.e_entry < userspace_addr || userspace_end <= ehdr.e_entry )
		return errno = EINVAL, 0;

	if ( ehdr.e_phentsize < sizeof(Elf_Phdr) )
		return errno = EINVAL, 0;

	if ( ehdr.e_shentsize < sizeof(Elf_Shdr) )
		return errno = EINVAL, 0;

	process->ResetForExecute();

	if ( ehdr.e_phnum == (Elf_Half) -1 )
		return errno = EINVAL, 0;
	if ( ehdr.e_shnum == (Elf_Half) -1 )
		return errno = EINVAL, 0;

	for ( Elf_Half i = 0; i < ehdr.e_phnum; i++ )
	{
		off_t phdr_offset;
		off_t phdr_extra;
		Elf_Phdr phdr;
		if ( (uintmax_t) off_max < (uintmax_t) ehdr.e_phoff ||
		     __builtin_mul_overflow((off_t) i, (off_t) ehdr.e_phentsize,
		                            &phdr_extra) ||
		     __builtin_add_overflow((off_t) ehdr.e_phoff, phdr_extra,
		                            &phdr_offset) ||
		     (uintmax_t) OFF_MAX - phdr_offset < sizeof(phdr) )
			return errno = EINVAL, 0;
		for ( size_t done = 0; done < sizeof(phdr); )
		{
			ssize_t amount =
				program->pread(&ctx, (uint8_t*) &phdr + done,
				               sizeof(phdr) - done, phdr_offset + done);
			if ( amount < 0 )
				return 0;
			if ( !amount )
				return errno = EINVAL, 0;
			done += amount;
		}

		switch ( phdr.p_type )
		{
		case PT_TLS: break;
		case PT_NOTE: break;
		case PT_LOAD: break;
		default: continue;
		};

		if ( !is_power_of_two(phdr.p_align) )
			return errno = EINVAL, 0;

		if ( phdr.p_type == PT_TLS )
		{
			if ( phdr.p_memsz < phdr.p_filesz )
				return errno = EINVAL, 0;

			aux->tls_file_offset = phdr.p_offset;
			aux->tls_file_size = phdr.p_filesz;
			aux->tls_mem_size = phdr.p_memsz;
			aux->tls_mem_align = phdr.p_align;
			continue;
		}

		if ( phdr.p_type == PT_NOTE )
		{
			size_t notes_offset = 0;
			while ( notes_offset < phdr.p_filesz )
			{
				size_t available = phdr.p_filesz - notes_offset;
				size_t note_header_size = 3 * sizeof(uint32_t);
				if ( available < note_header_size )
					return errno = EINVAL, 0;
				uint32_t note[16];
				if ( available < sizeof(note) )
					available = sizeof(note);
				size_t note_size =
					available < sizeof(note) ? available : sizeof(note);
				off_t file_offset;
				if ( (uintmax_t) off_max < (uintmax_t) phdr.p_offset ||
				     (uintmax_t) off_max < (uintmax_t) notes_offset ||
				     __builtin_add_overflow((off_t) phdr.p_offset,
				                            (off_t) notes_offset,
				                            &file_offset) ||
				     (uintmax_t) OFF_MAX - file_offset < note_size )
					return errno = EINVAL, 0;
				for ( size_t done = 0; done < note_size; )
				{
					ssize_t amount =
						program->pread(&ctx, (uint8_t*) note + done,
						               note_size - done, file_offset);
					if ( amount < 0 )
						return 0;
					if ( !amount )
						return errno = EINVAL, 0;
					done += amount;
				}

				uint32_t* note_header = (uint32_t*) note;
				uint32_t namesz = note_header[0];
				uint32_t descsz = note_header[1];
				uint32_t type = note_header[2];
				uint32_t namesz_aligned = -(-namesz & ~(sizeof(uint32_t) - 1));
				uint32_t descsz_aligned = -(-descsz & ~(sizeof(uint32_t) - 1));
				// TODO: Skip too large unknown notes.
				available -= note_header_size;
				if ( available < namesz_aligned )
					return errno = EINVAL, 0;
				available -= namesz_aligned;
				if ( available < descsz_aligned )
					return errno = EINVAL, 0;
				available -= descsz_aligned;
				(void) available;
				if ( __builtin_add_overflow(notes_offset, note_header_size,
				                            &notes_offset) ||
				     __builtin_add_overflow(notes_offset, namesz_aligned,
				                            &notes_offset) ||
				     __builtin_add_overflow(notes_offset, descsz_aligned,
				                            &notes_offset) )
					return errno = EINVAL, 0;

				const char* name = ((const char*) note) + note_header_size;
				if ( strnlen(name, namesz_aligned) == namesz_aligned )
					return errno = EINVAL, 0;
				const uint32_t* desc_32bits = &note[3 + namesz_aligned / 4];

				if ( strcmp(name, ELF_NOTE_SORTIX) == 0 )
				{
					if ( type == ELF_NOTE_SORTIX_UTHREAD_SIZE )
					{
						if ( descsz_aligned != 2 * sizeof(size_t) )
							return errno = EINVAL, 0;
#if __WORDSIZE == 32
						aux->uthread_size = desc_32bits[0];
						aux->uthread_align = desc_32bits[1];
#elif __WORDSIZE == 64 && BYTE_ORDER == LITTLE_ENDIAN
						aux->uthread_size = (uint64_t) desc_32bits[0] << 0 |
						                    (uint64_t) desc_32bits[1] << 32;
						aux->uthread_align = (uint64_t) desc_32bits[2] << 0 |
						                     (uint64_t) desc_32bits[3] << 32;
#elif __WORDSIZE == 64 && BYTE_ORDER == BIG_ENDIAN
						aux->uthread_size = (uint64_t) desc_32bits[1] << 0 |
						                    (uint64_t) desc_32bits[0] << 32;
						aux->uthread_align = (uint64_t) desc_32bits[3] << 0 |
						                     (uint64_t) desc_32bits[2] << 32;
#else
#error "You need to correctly read the uthread note"
#endif
						if ( !is_power_of_two(aux->uthread_align) )
							return errno = EINVAL, 0;
					}
				}
			}
			continue;
		}

		if ( phdr.p_type == PT_LOAD )
		{
			if ( phdr.p_memsz < phdr.p_filesz )
				return errno = EINVAL, 0;
			if ( phdr.p_filesz &&
			     phdr.p_vaddr % phdr.p_align !=
			     phdr.p_offset % phdr.p_align )
				return errno = EINVAL, 0;
			int kprot = PROT_WRITE | PROT_KWRITE | PROT_FORK;
			int prot = PROT_FORK;
			if ( phdr.p_flags & PF_X )
				prot |= PROT_EXEC;
			if ( phdr.p_flags & PF_R )
				prot |= PROT_READ | PROT_KREAD;
			if ( phdr.p_flags & PF_W )
				prot |= PROT_WRITE | PROT_KWRITE;

			if ( phdr.p_vaddr < userspace_addr )
				return errno = EINVAL, 0;
			if ( userspace_end < phdr.p_vaddr )
				return errno = EINVAL, 0;
			if ( userspace_end - phdr.p_vaddr < phdr.p_memsz )
				return errno = EINVAL, 0;

			uintptr_t map_start = Page::AlignDown(phdr.p_vaddr);
			uintptr_t map_end = Page::AlignUp(phdr.p_vaddr + phdr.p_memsz);
			size_t map_size = map_end - map_start;

			struct segment segment;
			segment.addr =  map_start;
			segment.size = map_size;
			segment.prot = kprot;

			assert(IsUserspaceSegment(&segment));

			kthread_mutex_lock(&process->segment_write_lock);
			kthread_mutex_lock(&process->segment_lock);

			if ( IsSegmentOverlapping(process, &segment) )
			{
				kthread_mutex_unlock(&process->segment_lock);
				kthread_mutex_unlock(&process->segment_write_lock);
				return errno = EINVAL, 0;
			}

			if ( !Memory::MapRange(segment.addr, segment.size, kprot, PAGE_USAGE_USER_SPACE) )
			{
				kthread_mutex_unlock(&process->segment_lock);
				kthread_mutex_unlock(&process->segment_write_lock);
				return errno = EINVAL, 0;
			}

			if ( !AddSegment(process, &segment) )
			{
				Memory::UnmapRange(segment.addr, segment.size, PAGE_USAGE_USER_SPACE);
				kthread_mutex_unlock(&process->segment_lock);
				kthread_mutex_unlock(&process->segment_write_lock);
				return errno = EINVAL, 0;
			}

			memset((void*) segment.addr, 0, segment.size);

			ioctx_t user_ctx; SetupUserIOCtx(&user_ctx);

			kthread_mutex_unlock(&process->segment_lock);
			if ( (uintmax_t) off_max < (uintmax_t) phdr.p_offset ||
			     OFF_MAX - phdr.p_offset < phdr.p_filesz )
				return errno = EINVAL, 0;
			for ( size_t done = 0; done < phdr.p_filesz; )
			{
				ssize_t amount = program->pread(&user_ctx,
				                                (uint8_t*) phdr.p_vaddr + done,
				                                phdr.p_filesz - done,
				                                phdr.p_offset + done);
				if ( amount < 0 )
					return 0;
				if ( !amount )
					return errno = EINVAL, 0;
				done += amount;
			}
			kthread_mutex_lock(&process->segment_lock);

			Memory::ProtectMemory(CurrentProcess(), segment.addr, segment.size, prot);

			kthread_mutex_unlock(&process->segment_lock);
			kthread_mutex_unlock(&process->segment_write_lock);
		}
	}

	return ehdr.e_entry;
}

} // namespace ELF
} // namespace Sortix
