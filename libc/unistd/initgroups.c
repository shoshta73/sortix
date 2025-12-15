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
 * unistd/initgrouplist.c
 * Set user's supplementary groups per group(5).
 */

#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

// TODO: After releasing Sortix 1.1, remove this temporarily compatibility.
#include <sortix/limits.h>

int initgroups(const char* name, gid_t basegid)
{
	gid_t* groups = calloc(NGROUPS_MAX, sizeof(gid_t));
	if ( !groups )
		return -1;
	int ngroups = NGROUPS_MAX;
	if ( getgrouplist(name, basegid, groups, &ngroups) < 0 )
		return free(groups), -1;
	int result = setgroups(ngroups, groups);
	free(groups);
	return result;
}
