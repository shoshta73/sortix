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
 * unistd/getgrouplist.c
 * Get user's supplementary groups from group(5).
 */

#include <errno.h>
#include <grp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int getgrouplist(const char* name, gid_t basegid, gid_t* groups, int* ngroups)
{
	int maximum = *ngroups;
	*ngroups = 0;
	int count = 0;
	if ( groups && count < maximum )
		groups[count] = basegid;
	count++;
	FILE* gr = opengr();
	if ( !gr )
		return -1;
	size_t buffer_size = 64;
	char* buffer = malloc(buffer_size);
	if ( !buffer )
		return fclose(gr), -1;
	while ( true )
	{
		struct group grp_buf;
		struct group* grp;
		int errnum = fgetgrent_r(gr, &grp_buf, buffer, buffer_size, &grp);
		if ( errnum == ERANGE )
		{
			char* new_buffer = reallocarray(buffer, buffer_size, 2);
			if ( !new_buffer )
				return free(buffer), fclose(gr), -1;
			buffer = new_buffer;
			buffer_size *= 2;
			continue;
		}
		else if ( errnum )
			return free(buffer), fclose(gr), -1;
		if ( !grp )
			break;
		if ( grp->gr_gid == basegid )
			continue;
		bool in_group = false;
		for ( size_t i = 0; !in_group && grp->gr_mem[i]; i++ )
			if ( !strcmp(name, grp->gr_mem[i]) )
				in_group = true;
		if ( in_group )
		{
			if ( groups && count < maximum )
				groups[count] = basegid;
			count++;
		}
	}
	free(buffer);
	fclose(gr);
	*ngroups = maximum;
	if ( maximum < count )
		return errno = 0, -1;
	return 0;
}
