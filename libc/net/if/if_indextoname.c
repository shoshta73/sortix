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
 * net/if/if_indextoname.c
 * Get name of network interface by index.
 */

#include <errno.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>

// TODO: This is better done with a dedicated system call.
char* if_indextoname(unsigned int ifindex, char* str)
{
	struct if_nameindex* ifs = if_nameindex();
	if ( !ifs )
		return NULL;
	for ( size_t i = 0; ifs[i].if_index; i++ )
	{
		if ( ifs[i].if_index == ifindex )
		{
			strlcpy(str, ifs[i].if_name, IF_NAMESIZE);
			if_freenameindex(ifs);
			return str;
		}
	}
	if_freenameindex(ifs);
	return errno = ENXIO, (char*) NULL;
}
