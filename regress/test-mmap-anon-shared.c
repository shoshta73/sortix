/*
 * Copyright (c) 2016 Jonas 'Sortie' Termansen.
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
 * test-mmap-anon-shared.c
 * Tests whether anonymous shared memory works.
 */

#include <sys/mman.h>
#include <sys/wait.h>

#include <unistd.h>

#include "test.h"

int main(void)
{
	const char* magic = "Tests whether anonymous shared memory works";
	size_t pagesize = getpagesize();
	test_assert(strlen(magic) < pagesize);

	void* shared = mmap(NULL, pagesize, PROT_READ | PROT_WRITE,
	                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if ( shared == MAP_FAILED )
		test_error(errno, "mmap(MAP_SHARED | MAP_ANONYMOUS)");

	pid_t child = fork();
	if ( child < 0 )
		test_error(errno, "fork");

	if ( child == 0 )
	{
		strlcpy((char*) shared, magic, pagesize);
		_exit(0);
	}

	int status;
	waitpid(child, &status, 0);

	test_assert(strncmp((const char*) shared, magic, pagesize) == 0);

	if ( munmap(shared, pagesize) < 0 )
		test_error(errno, "munmap");

	return 0;
}
