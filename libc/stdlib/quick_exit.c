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
 * stdlib/quick_exit.c
 * Terminates the current process but only runs at_quick_exit handlers.
 */

#include <dirent.h>
#include <DIR.h>
#include <FILE.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

struct quick_exit_handler* __quick_exit_handler_stack = NULL;

extern pthread_mutex_t __exit_lock;
extern bool __currently_exiting;

void quick_exit(int status)
{
	// It's undefined behavior to call this function more than once: If more
	// than one thread calls the function we'll wait until the process dies.
	pthread_mutex_lock(&__exit_lock);

	// It's undefined behavior to call this function more than once: If a
	// cleanup function calls this function we'll self-destruct immediately.
	if ( __currently_exiting )
		_exit(status);
	__currently_exiting = true;

	while ( __quick_exit_handler_stack )
	{
		__quick_exit_handler_stack->hook();
		__quick_exit_handler_stack = __quick_exit_handler_stack->next;
	}

	_exit(status);
}
