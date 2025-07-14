/*
 * Copyright (c) 2011-2016, 2023 Jonas 'Sortie' Termansen.
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
 * initrd.cpp
 * Extracts initrds into the initial memory filesystem.
 */

#include <sys/types.h>

#include <assert.h>
#include <errno.h>
#include <libgen.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <timespec.h>

#include <sortix/dirent.h>
#include <sortix/fcntl.h>
#include <sortix/mman.h>
#include <sortix/stat.h>
#include <sortix/tar.h>

#include <sortix/kernel/addralloc.h>
#include <sortix/kernel/descriptor.h>
#include <sortix/kernel/fsfunc.h>
#include <sortix/kernel/ioctx.h>
#include <sortix/kernel/kernel.h>
#include <sortix/kernel/memorymanagement.h>
#include <sortix/kernel/process.h>
#include <sortix/kernel/string.h>
#include <sortix/kernel/syscall.h>
#include <sortix/kernel/vnode.h>

#include "initrd.h"
#include "multiboot.h"

namespace Sortix {

struct initrd_context
{
	uint8_t* initrd;
	size_t initrd_size;
	addr_t initrd_unmap_start;
	addr_t initrd_unmap_end;
	ioctx_t ioctx;
};

// TODO: GRUB is currently buggy and doesn't ensure that other things are placed
//       at the end of a module, i.e. that the module doesn't own all the bugs
//       that it spans. It's thus risky to actually recycle the last page if the
//       module doesn't use all of it. Remove this compatibility when this has
//       been fixed in GRUB and a few years have passed such that most GRUB
//       systems have this fixed.
static void UnmapInitrdPage(struct initrd_context* ctx, addr_t vaddr)
{
	if ( !Memory::LookUp(vaddr, NULL, NULL) )
		return;
	addr_t addr = Memory::Unmap(vaddr);
	if ( !(ctx->initrd_unmap_start <= addr && addr < ctx->initrd_unmap_end) )
		return;
	Page::Put(addr, PAGE_USAGE_WASNT_ALLOCATED);
}

struct TAR
{
	unsigned char* tar_file;
	size_t tar_file_size;
	size_t next_offset;
	size_t offset;
	size_t data_offset;
	char* name;
	char* linkname;
	unsigned char* data;
	size_t size;
	mode_t mode;
	char typeflag;
};

static void OpenTar(TAR* TAR, unsigned char* tar_file, size_t tar_file_size)
{
	memset(TAR, 0, sizeof(*TAR));
	TAR->tar_file = tar_file;
	TAR->tar_file_size = tar_file_size;
}

static void CloseTar(TAR* TAR)
{
	free(TAR->name);
	free(TAR->linkname);
	memset(TAR, 0, sizeof(*TAR));
}

static bool ReadTar(TAR* TAR)
{
	free(TAR->name);
	free(TAR->linkname);
	TAR->name = NULL;
	TAR->linkname = NULL;
	while ( true )
	{
		if ( TAR->tar_file_size - TAR->next_offset < sizeof(struct TAR) )
			return false;
		TAR->offset = TAR->next_offset;
		struct tar* tar = (struct tar*) (TAR->tar_file + TAR->offset);
		if ( tar->size[sizeof(tar->size) - 1] != '\0' &&
		     tar->size[sizeof(tar->size) - 1] != ' ' )
			return false;
		size_t size = strtoul(tar->size, NULL, 8);
		size_t dist = sizeof(struct tar) + -(-size & ~((size_t) 512 - 1));
		if ( TAR->tar_file_size - TAR->offset < dist )
			return false;
		TAR->next_offset = TAR->offset + dist;
		TAR->data_offset = TAR->offset + 512;
		TAR->data = TAR->tar_file + TAR->data_offset;
		TAR->size = size;
		if ( tar->mode[sizeof(tar->mode) - 1] != '\0' &&
		     tar->mode[sizeof(tar->mode) - 1] != ' ' )
			return false;
		TAR->mode = strtoul(tar->mode, NULL, 8) & 07777;
		TAR->typeflag = tar->typeflag;
		// TODO: Things like modified time and other meta data!
		if ( tar->typeflag == 'L' )
		{
			free(TAR->name);
			if ( !(TAR->name = (char*) malloc(size + 1)) )
				Panic("initrd tar malloc failure");
			memcpy(TAR->name, TAR->data, size);
			TAR->name[size] = '\0';
			continue;
		}
		else if ( tar->typeflag == 'g' )
		{
			// TODO: Implement pax extensions.
			continue;
		}
		else if ( tar->typeflag == 'x' )
		{
			// TODO: Implement pax extensions.
			continue;
		}
		if ( !tar->name[0] )
			continue;
		if ( !TAR->name )
		{
			if ( tar->prefix[0] )
			{
				size_t prefix_len = strnlen(tar->prefix, sizeof(tar->prefix));
				size_t name_len = strnlen(tar->name, sizeof(tar->name));
				size_t name_size = prefix_len + 1 + name_len + 1;
				if ( !(TAR->name = (char*) malloc(name_size)) )
					Panic("initrd tar malloc failure");
				memcpy(TAR->name, tar->prefix, prefix_len);
				TAR->name[prefix_len] = '/';
				memcpy(TAR->name + prefix_len + 1, tar->name, name_len);
				TAR->name[prefix_len + 1 + name_len] = '\0';
			}
			else
			{
				TAR->name = (char*) strndup(tar->name, sizeof(tar->name));
				if ( !TAR->name )
					Panic("initrd tar malloc failure");
			}
		}
		if ( !TAR->linkname )
		{
			TAR->linkname = (char*) strndup(tar->linkname, sizeof(tar->linkname));
			if ( !TAR->linkname )
				Panic("initrd tar malloc failure");
		}
		return true;
	}
}

static void ExtractTarObject(Ref<Descriptor> desc,
                             struct initrd_context* ctx,
                             TAR* TAR)
{
	if ( TAR->typeflag == '0' || TAR->typeflag == 0 )
	{
		int oflags = O_WRITE | O_CREATE | O_TRUNC;
		Ref<Descriptor> file(desc->open(&ctx->ioctx, TAR->name, oflags, TAR->mode));
		if ( !file )
			PanicF("%s: %m", TAR->name);
		if ( file->truncate(&ctx->ioctx, TAR->size) != 0 )
			PanicF("truncate: %s: %m", TAR->name);
		size_t sofar = 0;
		while ( sofar < TAR->size )
		{
			size_t left = TAR->size - sofar;
			size_t chunk = 1024 * 1024;
			size_t count = left < chunk ? left : chunk;
			ssize_t numbytes = file->write(&ctx->ioctx, TAR->data + sofar, count);
			if ( numbytes <= 0 )
				PanicF("write: %s: %m", TAR->name);
			sofar += numbytes;
		}
	}
	else if ( TAR->typeflag == '1' )
	{
		Ref<Descriptor> dest(desc->open(&ctx->ioctx, TAR->linkname, O_READ, 0));
		if ( !dest )
			PanicF("%s: %m", TAR->linkname);
		if ( desc->link(&ctx->ioctx, TAR->name, dest) != 0 )
			PanicF("link: %s -> %s: %m", TAR->linkname, TAR->name);
	}
	else if ( TAR->typeflag == '2' )
	{
		if ( desc->symlink(&ctx->ioctx, TAR->linkname, TAR->name) != 0 )
			PanicF("symlink: %s: %m", TAR->name);
	}
	else if ( TAR->typeflag == '5' )
	{
		if ( desc->mkdir(&ctx->ioctx, TAR->name, TAR->mode) && errno != EEXIST )
			PanicF("mkdir: %s: %m", TAR->name);
	}
	else
	{
		Log::PrintF("kernel: initrd: %s: Unsupported tar filetype '%c'\n",
		            TAR->name, TAR->typeflag);
	}
}

static void ExtractTar(Ref<Descriptor> desc, struct initrd_context* ctx)
{
	Process* process = CurrentProcess();
	kthread_mutex_lock(&process->id_lock);
	mode_t oldmask = process->umask;
	process->umask = 0000;
	kthread_mutex_unlock(&process->id_lock);
	TAR TAR;
	OpenTar(&TAR, ctx->initrd, ctx->initrd_size);
	while ( ReadTar(&TAR) )
		ExtractTarObject(desc, ctx, &TAR);
	CloseTar(&TAR);
	kthread_mutex_lock(&process->id_lock);
	process->umask = oldmask;
	kthread_mutex_unlock(&process->id_lock);
}

static int ExtractTo_mkdir(Ref<Descriptor> desc, ioctx_t* ctx,
                           const char* path, mode_t mode)
{
	int saved_errno = errno;
	if ( !desc->mkdir(ctx, path, mode) )
		return 0;
	if ( errno == ENOENT )
	{
		char* prev = strdup(path);
		if ( !prev )
			return -1;
		int status = ExtractTo_mkdir(desc, ctx, dirname(prev), mode | 0500);
		free(prev);
		if ( status < 0 )
			return -1;
		errno = saved_errno;
		if ( !desc->mkdir(ctx, path, mode) )
			return 0;
	}
	if ( errno == EEXIST )
		return errno = saved_errno, 0;
	return -1;
}

static void ExtractTo(Ref<Descriptor> desc,
                      struct initrd_context* ctx,
                      const char* path,
                      int extra_oflags)
{
	int oflags = O_WRITE | O_CREATE | extra_oflags;
	Ref<Descriptor> file(desc->open(&ctx->ioctx, path, oflags, 0644));
	if ( !file && errno == ENOENT )
	{
		char* prev = strdup(path);
		if ( !prev )
			PanicF("%s: strdup: %m", path);
		if ( ExtractTo_mkdir(desc, &ctx->ioctx, dirname(prev), 0755) < 0 )
			PanicF("%s: mkdir -p: %s: %m", path, prev);
		free(prev);
		file = desc->open(&ctx->ioctx, path, oflags, 0644);
	}
	if ( !file )
	{
		if ( errno == EEXIST && (oflags & O_EXCL) )
			return;
		PanicF("%s: %m", path);
	}
	if ( !(oflags & O_APPEND) )
	{
		if ( file->truncate(&ctx->ioctx, ctx->initrd_size) != 0 )
			PanicF("truncate: %s: %m", path);
	}
	size_t sofar = 0;
	while ( sofar < ctx->initrd_size )
	{
		size_t left = ctx->initrd_size - sofar;
		size_t chunk = 1024 * 1024;
		size_t count = left < chunk ? left : chunk;
		ssize_t numbytes = file->write(&ctx->ioctx, ctx->initrd + sofar, count);
		if ( numbytes <= 0 )
			PanicF("write: %s: %m", path);
		sofar += numbytes;
	}
}

static void ExtractModule(struct multiboot_mod_list* module,
                          Ref<Descriptor> desc,
                          struct initrd_context* ctx)
{
	size_t mod_size = module->mod_end - module->mod_start;
	const char* cmdline = (const char*) (uintptr_t) module->cmdline;

	// Ignore the random seed.
	if ( !strcmp(cmdline, "--random-seed") )
		return;

	// Allocate the needed kernel virtual address space.
	addralloc_t initrd_addr_alloc;
	if ( !AllocateKernelAddress(&initrd_addr_alloc, mod_size) )
		PanicF("Failed to allocate kernel address space for the initrd");

	// Map the physical frames onto our address space.
	addr_t physfrom = module->mod_start;
	addr_t mapat = initrd_addr_alloc.from;
	for ( size_t i = 0; i < mod_size; i += Page::Size() )
	{
		if ( !Memory::Map(physfrom + i, mapat + i, PROT_KREAD | PROT_KWRITE) )
			PanicF("Unable to map the initrd into virtual memory");
	}
	Memory::Flush();

	ctx->initrd = (uint8_t*) initrd_addr_alloc.from;
	ctx->initrd_size = mod_size;
	ctx->initrd_unmap_start = module->mod_start;
	ctx->initrd_unmap_end = Page::AlignDown(module->mod_end);

	const unsigned char xz_magic[] = { 0xFD, '7', 'z', 'X', 'Z', 0x00 };
	const unsigned char bzip2_magic[] = { 'B', 'Z' };
	const unsigned char gz_magic[] = { 0x1F, 0x8B };

	if ( !strncmp(cmdline, "--to ", strlen("--to ")) ||
	     !strncmp(cmdline, "--to=", strlen("--to=")) )
		ExtractTo(desc, ctx, cmdline + strlen("--to "), O_TRUNC);
	else if ( !strncmp(cmdline, "--append-to ", strlen("--append-to ")) ||
	          !strncmp(cmdline, "--append-to=", strlen("--append-to=")) )
		ExtractTo(desc, ctx, cmdline + strlen("--append-to "), O_APPEND);
	else if ( !strncmp(cmdline, "--create-to ", strlen("--create-to ")) ||
	          !strncmp(cmdline, "--create-to=", strlen("--create-to=")) )
		ExtractTo(desc, ctx, cmdline + strlen("--create-to "), O_EXCL);
	// TODO: After releasing Sortix 1.1, remove this nice error message.
	else if ( strlen("sortix-initrd-2") <= ctx->initrd_size &&
	          !memcmp(ctx->initrd, "sortix-initrd-2", strlen("sortix-initrd-2")) )
		Panic("The sortix-initrd-2 format is no longer supported");
	else if ( sizeof(struct tar) <= ctx->initrd_size &&
	          !memcmp(ctx->initrd + offsetof(struct tar, magic), "ustar", 5) )
		ExtractTar(desc, ctx);
	else if ( sizeof(xz_magic) <= ctx->initrd_size &&
	          !memcmp(ctx->initrd, xz_magic, sizeof(xz_magic)) )
		Panic("Bootloader failed to decompress an xz initrd, "
		      "or try the --to <path> option");
	else if ( sizeof(gz_magic) <= ctx->initrd_size &&
	          !memcmp(ctx->initrd, gz_magic, sizeof(gz_magic)) )
		Panic("Bootloader failed to decompress a gzip initrd, "
		      "or try the --to <path> option");
	else if ( sizeof(bzip2_magic) <= ctx->initrd_size &&
	          !memcmp(ctx->initrd, bzip2_magic, sizeof(bzip2_magic)) )
		Panic("Bootloader failed to decompress a bzip2 initrd, "
		      "or try the --to <path> option");
	else
		Panic("Unsupported initrd format, or try the --to <path> option");

	// Unmap the pages and return the physical frames for reallocation.
	for ( size_t i = 0; i < mod_size; i += Page::Size() )
		UnmapInitrdPage(ctx, mapat + i);
	Memory::Flush();

	// Free the used virtual address space.
	FreeKernelAddress(&initrd_addr_alloc);
}

void ExtractModules(struct multiboot_info* bootinfo, Ref<Descriptor> root)
{
	struct multiboot_mod_list* modules =
		(struct multiboot_mod_list*) (uintptr_t) bootinfo->mods_addr;
	struct initrd_context ctx;
	memset(&ctx, 0, sizeof(ctx));
	SetupKernelIOCtx(&ctx.ioctx);
	for ( uint32_t i = 0; i < bootinfo->mods_count; i++ )
		ExtractModule(&modules[i], root, &ctx);
}

} // namespace Sortix
