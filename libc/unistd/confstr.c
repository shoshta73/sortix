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
 * unistd/confstr.c
 * Get configurable string variables.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CFLAGS ""
#define LDFLAGS ""
#define LIBS ""
#define THREADS_CFLAGS ""
#define THREADS_LDFLAGS ""

size_t confstr(int name, char* buffer, size_t buffer_size)
{
	static const char* keys[] =
	{
		[_CS_PATH] = "/bin:/sbin",
#if _POSIX_V8_ILP32_OFF32
		[_CS_POSIX_V8_ILP32_OFF32_CFLAGS] = CFLAGS,,
		[_CS_POSIX_V8_ILP32_OFF32_LDFLAGS] = LDFLAGS,
		[_CS_POSIX_V8_ILP32_OFF32_LIBS] = LIBS,
#endif
#if _POSIX_V8_ILP32_OFFBIG
		[_CS_POSIX_V8_ILP32_OFFBIG_CFLAGS] = CFLAGS,
		[_CS_POSIX_V8_ILP32_OFFBIG_LDFLAGS] = LDFLAGS,
		[_CS_POSIX_V8_ILP32_OFFBIG_LIBS] = LIBS,
#endif
#if _POSIX_V8_LP64_OFF64
		[_CS_POSIX_V8_LP64_OFF64_CFLAGS] = CFLAGS,
		[_CS_POSIX_V8_LP64_OFF64_LDFLAGS] = LDFLAGS,
		[_CS_POSIX_V8_LP64_OFF64_LIBS] = LIBS,
#endif
#if _POSIX_V8_LPBIG_OFFBIG
		[_CS_POSIX_V8_LPBIG_OFFBIG_CFLAGS] = CFLAGS,
		[_CS_POSIX_V8_LPBIG_OFFBIG_LDFLAGS] = LDFLAGS,
		[_CS_POSIX_V8_LPBIG_OFFBIG_LIBS] = LIBS,
#endif
		[_CS_POSIX_V8_THREADS_CFLAGS] = THREADS_CFLAGS,
		[_CS_POSIX_V8_THREADS_LDFLAGS] = THREADS_LDFLAGS,
		[_CS_POSIX_V8_WIDTH_RESTRICTED_ENVS] =
#if _POSIX_V8_ILP32_OFF32
		"POSIX_V8_ILP32_OFF32\n"
#endif
#if _POSIX_V8_ILP32_OFFBIG
		"POSIX_V8_ILP32_OFFBIG\n"
#endif
#if _POSIX_V8_LP64_OFF64
		"POSIX_V8_LP64_OFF64\n"
#endif
#if _POSIX_V8_LPBIG_OFFBIG
		"POSIX_V8_LPBIG_OFFBIG\n"
#endif
		"",
		[_CS_V8_ENV] = "POSIXLY_CORRECT=1",
#if _POSIX_V7_ILP32_OFF32
		[_CS_POSIX_V7_ILP32_OFF32_CFLAGS] = CFLAGS,
		[_CS_POSIX_V7_ILP32_OFF32_LDFLAGS] = LDFLAGS,
		[_CS_POSIX_V7_ILP32_OFF32_LIBS] = LIBS,
#endif
#if _POSIX_V7_ILP32_OFFBIG
		[_CS_POSIX_V7_ILP32_OFFBIG_CFLAGS] = CFLAGS,
		[_CS_POSIX_V7_ILP32_OFFBIG_LDFLAGS] = LDFLAGS,
		[_CS_POSIX_V7_ILP32_OFFBIG_LIBS] = LIBS,
#endif
#if _POSIX_V7_LP64_OFF64
		[_CS_POSIX_V7_LP64_OFF64_CFLAGS] = CFLAGS,
		[_CS_POSIX_V7_LP64_OFF64_LDFLAGS] = LDFLAGS,
		[_CS_POSIX_V7_LP64_OFF64_LIBS] = LIBS,
#endif
#if _POSIX_V7_LPBIG_OFFBIG
		[_CS_POSIX_V7_LPBIG_OFFBIG_CFLAGS] = CFLAGS,
		[_CS_POSIX_V7_LPBIG_OFFBIG_LDFLAGS] = LDFLAGS,
		[_CS_POSIX_V7_LPBIG_OFFBIG_LIBS] = LIBS,
#endif
		[_CS_POSIX_V7_THREADS_CFLAGS] = THREADS_CFLAGS,
		[_CS_POSIX_V7_THREADS_LDFLAGS] = THREADS_LDFLAGS,
		[_CS_POSIX_V7_WIDTH_RESTRICTED_ENVS] =
#if _POSIX_V7_ILP32_OFF32
		"POSIX_V7_ILP32_OFF32\n"
#endif
#if _POSIX_V7_ILP32_OFFBIG
		"POSIX_V7_ILP32_OFFBIG\n"
#endif
#if _POSIX_V7_LP64_OFF64
		"POSIX_V7_LP64_OFF64\n"
#endif
#if _POSIX_V7_LPBIG_OFFBIG
		"POSIX_V7_LPBIG_OFFBIG\n"
#endif
		"",
		[_CS_V7_ENV] = "POSIXLY_CORRECT=1",
	};
	if ( name < 0 || sizeof(keys) / sizeof(keys[0]) <= (size_t) name )
		return errno = EINVAL, -1;
	const char* value = keys[name];
	if ( !value )
		return 0;
	size_t value_length = strlen(value);
	// WIDTH_RESTRICTED_ENVS is newline separated, not terminated.
	if ( value_length && value[value_length-1] == '\n' )
		value_length--;
	if ( !buffer || !buffer_size )
		return value_length + 1;
	size_t output =  value_length;
	if ( output < buffer_size - 1 )
		output =  buffer_size - 1;
	memcpy(buffer, value, output);
	buffer[output] = '\0';
	return value_length + 1;
}
