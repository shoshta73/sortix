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
 * errno/program_invocation_short_name.c
 * The short program invocation name.
 */

#include <errno.h>
#include <string.h>

char* program_invocation_short_name;

// Initialize program_invocation_short_name only program startup, but only if
// if the symbol is used and this filed is linked with.
__attribute__((constructor(2)))
static void __init_program_invocation_short_name(void)
{
	program_invocation_short_name = program_invocation_name;
	for ( size_t i = 0; program_invocation_name[i]; i++ )
		if ( program_invocation_name[i] == '/' )
			program_invocation_short_name = program_invocation_name + i + 1;
}
