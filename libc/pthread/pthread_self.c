/*
 * Copyright (c) 2013, 2025 Jonas 'Sortie' Termansen.
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
 * pthread/pthread_self.c
 * Returns the identity of the current thread.
 */

#include <elf.h>
#include <pthread.h>
#include <stdalign.h>

// Emit an ELF note containing the size and alignment of struct pthread and
// initialize the main thread pthread structure.
__attribute__((constructor(1)))
static void __init_pthread(void)
{
	asm volatile (
		".pushsection .note.sortix,\"a\",@note\n\t"
		".align 4\n\t"
		".long 2f-1f\n\t" // namesz
		".long 4f-3f\n\t" // descsz
		".long %c0\n" // type
		"1:\n\t"
		".string \"Sortix\"\n" // name
		"2:\n\t"
		".align 4\n"
		"3:\n\t"
#if defined(__x86_64__)
		".quad %c1\n"
		".quad %c2\n"
#elif defined(__i386__)
		".long %c1\n"
		".long %c2\n"
#endif
		"4:\n\t"
		".align 4\n\t"
		".popsection\n\t"
		:: "n"(ELF_NOTE_SORTIX_UTHREAD_SIZE),
		   "n"(sizeof(struct pthread)),
		   "n"(alignof(struct pthread))
	);
	struct pthread* self = pthread_self();
	self->join_lock = (pthread_mutex_t) PTHREAD_NORMAL_MUTEX_INITIALIZER_NP;
	self->join_lock.lock = 1 /* LOCKED_VALUE */;
	self->join_lock.type = PTHREAD_MUTEX_NORMAL;
	self->join_lock.owner = (unsigned long) self;
	self->detach_lock = (pthread_mutex_t) PTHREAD_NORMAL_MUTEX_INITIALIZER_NP;
	self->detach_state = PTHREAD_CREATE_JOINABLE;
}

pthread_t pthread_self(void)
{
	pthread_t current_thread;
#if defined(__i386__)
	asm ( "mov %%gs:0, %0" : "=r"(current_thread));
#elif defined(__x86_64__)
	asm ( "mov %%fs:0, %0" : "=r"(current_thread));
#else
	#error "You need to implement pthread_self for your platform"
#endif
	return current_thread;
}
