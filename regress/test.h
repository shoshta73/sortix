/*
 * Copyright (c) 2014, 2017 Jonas 'Sortie' Termansen.
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
 * test.h
 * Shared test utility functions.
 */

#ifndef TEST_H
#define TEST_H

#include <err.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute((noreturn, unused)) static inline
void test_error(int errnum, const char* format, ...)
{
	fprintf(stderr, "%s: ", program_invocation_name);

	va_list list;
	va_start(list, format);
	vfprintf(stderr, format, list);
	va_end(list);

	if ( errnum )
		fprintf(stderr, ": %s", strerror(errnum));
	fprintf(stderr, "\n");

	exit(1);
}

__attribute((unused))
static inline void test_assertion(bool assertion,
                                  const char* file,
                                  unsigned int line,
                                  const char* assertion_string,
                                  int errnum)
{
	if ( !assertion )
		test_error(errnum, "assertion failure: %s:%u: %s", file, line,
		           assertion_string);
}

__attribute((unused))
static inline void test_assertionp(int errnum,
                                   const char* file,
                                   unsigned int line,
                                   const char* assertion_string)
{
	test_assertion(errnum == 0, file, line, assertion_string, errnum);
}

#define test_assert(x) test_assertion((x), __FILE__, __LINE__, #x, errno)
#define test_assertc(x, errnum) \
        test_assertion((x), __FILE__, __LINE__, #x, errnum)
#define test_assertp(x) test_assertionp((x), __FILE__, __LINE__, #x)
#define test_assertx(x) test_assertion((x), __FILE__, __LINE__, #x, 0)

#endif
