/*
 * Copyright (c) 2014, 2015, 2022 Jonas 'Sortie' Termansen.
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
 * stdio/vasprintf.c
 * Prints a string to a newly allocated buffer.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

struct vasprintf
{
	char* buffer;
	size_t used;
	size_t size;
#ifdef __TRACE_ALLOCATION_SITES
	struct __allocation_site* allocation_site;
#endif
};

static size_t vasprintf_callback(void* ctx, const char* string, size_t length)
{
	struct vasprintf* state = (struct vasprintf*) ctx;
	size_t needed_size = state->used + length + 1;
	if ( state->size < needed_size )
	{
		// TODO: Overflow check.
		size_t new_size = 2 * state->size;
		if ( new_size < needed_size )
			new_size = needed_size;
#ifdef __TRACE_ALLOCATION_SITES
		char* new_buffer = (char*) realloc_trace(state->allocation_site,
		                                         state->buffer, new_size);
#else
		char* new_buffer = (char*) realloc(state->buffer, new_size);
#endif
		if ( !new_buffer )
		{
			free(state->buffer);
			state->buffer = NULL;
			return 0;
		}
		state->buffer = new_buffer;
		state->size = new_size;
	}
	memcpy(state->buffer + state->used, string, length);
	state->used += length;
	return length;
}

#ifdef __TRACE_ALLOCATION_SITES
int vasprintf_trace(struct __allocation_site* allocation_site,
                    char** restrict result_ptr,
                    const char* restrict format,
                    va_list list)
#else
int vasprintf(char** restrict result_ptr,
              const char* restrict format,
              va_list list)
#endif
{
	struct vasprintf state;
	state.used = 0;
	state.size = 32;
#ifdef __TRACE_ALLOCATION_SITES
	state.allocation_site = allocation_site;
	if ( !(state.buffer = (char*) malloc_trace(allocation_site, state.size)) )
#else
	if ( !(state.buffer = (char*) malloc(state.size)) )
#endif
		return -1;
	int result = vcbprintf(&state, vasprintf_callback, format, list);
	if ( !state.buffer )
		return -1;
	state.buffer[state.used] = '\0';
	return *result_ptr = state.buffer, result;
}
