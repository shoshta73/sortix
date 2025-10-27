/*
 * Copyright (c) 2011, 2012, 2014, 2015, 2025 Jonas 'Sortie' Termansen.
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
 * stdlib/exit.c
 * Terminates the current process.
 */

#include <dirent.h>
#include <DIR.h>
#include <FILE.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void _fini(void);

__attribute__((weak)) __attribute__((visibility("hidden")))
extern void (*__fini_array_start)(void);
__attribute__((weak)) __attribute__((visibility("hidden")))
extern void (*__fini_array_end)(void);

static void __fini_array(void)
{
	void (**fini)(void) = &__fini_array_end;
	while ( fini > &__fini_array_start )
	{
		fini--;
		(*fini)();
	}
}

bool __currently_exiting = false;
FILE* __first_file = NULL;

// Only pull in the stdin/stdout/stderr objects if referenced.
static FILE* volatile dummy_file = NULL; // volatile due to constant folding bug
weak_alias(dummy_file, __stdin_used);
weak_alias(dummy_file, __stdout_used);

// Only pull in pthread_mutex_lock if pthread_create is referenced.
static void noop_lock(pthread_mutex_t* mutex) { (void) mutex; }
weak_alias(noop_lock, __pthread_mutex_lock_if_threaded);

// Only pull in pthread_mutex_lock if pthread_create is referenced.
static void noop(void) { }
weak_alias(noop, __lock_exit_lock);
weak_alias(noop, __lock_first_lock_lock);

// Only pull in on_exit execution if on_exit is referenced.
static void on_exit_noop(int status) { (void) status; }
weak_alias(on_exit_noop, __on_exit_execute);

static void exit_file(FILE* fp)
{
	if ( !fp )
		return;
	__pthread_mutex_lock_if_threaded(&fp->file_lock);
	if ( fp->fflush_indirect )
		fp->fflush_indirect(fp);
	if ( fp->close_func )
		fp->close_func(fp->user);
}

void exit(int status)
{
	// It's undefined behavior to call this function more than once: If more
	// than one thread calls the function we'll wait until the process dies.
	__lock_exit_lock();

	// It's undefined behavior to call this function more than once: If a
	// cleanup function calls this function we'll self-destruct immediately.
	if ( __currently_exiting )
		_exit(status);
	__currently_exiting = true;

	// Run the on_exit(3) (and atexit(3)) functions.
	__on_exit_execute(status);

	// Run the global destructors.
	__fini_array();
	_fini();

	// Flush all the remaining FILE objects.
	__lock_first_lock_lock();
	exit_file(__stdin_used);
	exit_file(__stdout_used);
	for ( FILE* fp = __first_file; fp; fp = fp->next )
		exit_file(fp);

	// Exit the process.
	_exit(status);
}
