/*
 * Copyright (c) 2016, 2020, 2026 Jonas 'Sortie' Termansen.
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
 * arpa/inet/inet_ntop.c
 * Convert network address to string.
 */

#include <sys/socket.h>

#include <errno.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

const char* inet_ntop(int af,
                      const void* restrict src,
                      char* restrict dst,
                      socklen_t size)
{
	if ( af == AF_INET )
	{
		const unsigned char* ip = (const unsigned char*) src;
		int len = snprintf(dst, size, "%u.%u.%u.%u",
		                   ip[0], ip[1], ip[2], ip[3]);
		if ( len < 0 )
			return NULL;
		if ( size <= (size_t) len )
			return errno = ENOSPC, (const char*) NULL;
		return dst;
	}
	else if ( af == AF_INET6 )
	{
		const unsigned char* ip = (const unsigned char*) src;
		unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
		if ( !memcmp(ip, mapped, 12) )
		{
			int len = snprintf(dst, size, "::ffff:%u.%u.%u.%u",
				               ip[12], ip[13], ip[14], ip[15]);
			if ( len < 0 )
				return NULL;
			if ( size <= (size_t) len )
				return errno = ENOSPC, (const char*) NULL;
		}
		size_t longest_zeroes_offset = 0;
		size_t longest_zeroes_length = 0;
		size_t current_zeroes_offset = 0;
		size_t current_zeroes_length = 0;
		for ( size_t i = 0; i < 8; i++ )
		{
			const unsigned char* data = ip + i * 2;
			if ( !data[0] && !data[1] )
			{
				current_zeroes_length++;
				if ( longest_zeroes_length < current_zeroes_length )
				{
					longest_zeroes_offset = current_zeroes_offset;
					longest_zeroes_length = current_zeroes_length;
				}
			}
			else
			{
				current_zeroes_offset = i + 1;
				current_zeroes_length = 0;
			}
		}
		char buffer[INET6_ADDRSTRLEN];
		size_t offset = 0;
		for ( size_t i = 0; i < 8; i++ )
		{
			const unsigned char* data = ip + i * 2;
			if ( i == longest_zeroes_offset && 2 <= longest_zeroes_length )
			{
				buffer[offset++] = ':';
				buffer[offset++] = ':';
				i += longest_zeroes_length - 1;
			}
			else
			{
				if ( offset && buffer[offset - 1] != ':' )
					buffer[offset++] = ':';
				offset += snprintf(buffer + offset, sizeof(buffer) - offset,
				                   "%x", data[0] << 8 | data[1]);
			}
		}
		buffer[offset] = '\0';
		if ( size <= (size_t) offset )
			return errno = ENOSPC, (const char*) NULL;
		memcpy(dst, buffer, offset + 1);
		return dst;
	}
	else
		return errno = EAFNOSUPPORT, NULL;
}
