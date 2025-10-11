/*
 * Copyright (c) 2012, 2013, 2025 Jonas 'Sortie' Termansen.
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
 * stdlib/at_quick_exit.c
 * Hooks that are called upon quick_exit.
 */

#include <pthread.h>
#include <stdlib.h>

extern pthread_mutex_t __exit_lock;

int at_quick_exit(void (*hook)(void))
{
	pthread_mutex_lock(&__exit_lock);
	struct quick_exit_handler* handler =
		(struct quick_exit_handler*) malloc(sizeof(struct quick_exit_handler));
	if ( !handler )
		return pthread_mutex_unlock(&__exit_lock), -1;
	handler->hook = hook;
	handler->next = __quick_exit_handler_stack;
	__quick_exit_handler_stack = handler;
	pthread_mutex_unlock(&__exit_lock);
	return 0;
}
