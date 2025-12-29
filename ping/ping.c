/*
 * Copyright (c) 2017, 2023 Jonas 'Sortie' Termansen.
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
 * ping.c
 * Internet Control Message Protocol Echo.
 */

#include <sys/socket.h>

#include <err.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <timespec.h>
#include <unistd.h>

#define PAYLOAD_SIZE 56

int main(int argc, char* argv[])
{
	bool ipv4 = false;
	bool ipv6 = false;

	int opt;
	while ( (opt = getopt(argc, argv, "46")) != -1 )
	{
		switch ( opt )
		{
		case '4': ipv4 = true; break;
		case '6': ipv6 = true; break;
		default: return 1;
		}
	}

	if ( argc - optind < 1 )
		errx(1, "No host given");
	if ( argc - optind > 1 )
		errx(1, "Unexpected extra operand: %s", argv[optind + 1]);
	const char* host = argv[optind];

	if ( 1 < ipv4 + ipv6 )
		errx(1, "The -4 and -6 options are mutually incompatible");

	struct addrinfo hint;
	memset(&hint, 0, sizeof(hint));
	if ( ipv4 )
		hint.ai_family = AF_INET;
	if ( ipv6 )
		hint.ai_family = AF_INET6;
	hint.ai_socktype = SOCK_DGRAM;
	hint.ai_protocol = IPPROTO_PING;

	struct addrinfo* res0 = NULL;
	int status = getaddrinfo(host, NULL, &hint, &res0);
	if ( status == EAI_SYSTEM )
		err(1, "%s", host);
	if ( status )
		errx(1, "%s: %s", host, gai_strerror(status));
	if ( !res0 )
		errx(1, "%s: %s", host, gai_strerror(EAI_NONAME));

	char host_address[NI_MAXHOST];
	int fd = -1;
	for ( struct addrinfo* res = res0; res; res = res->ai_next )
	{
		if ( (fd = socket(res->ai_family, res->ai_socktype,
		                  res->ai_protocol)) < 0 )
		{
			if ( res->ai_next )
				continue;
			err(1, "socket");
		}
		if ( connect(fd, res->ai_addr, res->ai_addrlen) < 0 )
		{
			close(fd);
			if ( res->ai_next )
				continue;
			err(1, "connect: %s", host);
		}
		if ( getnameinfo(res->ai_addr, res->ai_addrlen, host_address,
		                 sizeof(host_address), NULL, 0, NI_NUMERICHOST) < 0 )
			strlcpy(host_address, "unknown", sizeof(host_address));
		break;
	}

	freeaddrinfo(res0);

	printf("PING %s (%s) %zu bytes of data.\n",
	       host, host_address, (size_t) (PAYLOAD_SIZE + 8));

	while ( true )
	{
		unsigned char expected[PAYLOAD_SIZE];
		arc4random_buf(expected, sizeof(expected));
		struct timespec begun;
		clock_gettime(CLOCK_MONOTONIC, &begun);
		// TODO: Don't fail on network errors.
		if ( send(fd, expected, sizeof(expected), 0) < 0 )
			err(1, "send");
		struct timespec timeout = timespec_add(timespec_make(1, 0), begun);
		while ( true )
		{
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			struct timespec remaining = timespec_sub(timeout, now);
			if ( remaining.tv_sec < 0 )
				break;
			struct pollfd pfd = { 0 };
			pfd.fd = fd;
			pfd.events = POLLIN;
			if ( ppoll(&pfd, 1, &remaining, NULL) <= 0 )
				break;
			unsigned char gotten[PAYLOAD_SIZE];
			ssize_t amount = recv(fd, gotten, sizeof(gotten), 0);
			struct timespec end;
			clock_gettime(CLOCK_MONOTONIC, &end);
			// TODO: Don't fail on network errors.
			if ( amount < 0 )
				err(1, "recv");
			if ( amount == PAYLOAD_SIZE &&
			     memcmp(expected, gotten, PAYLOAD_SIZE) == 0 )
			{
				// TODO: Reverse DNS.
				uint16_t sequence = gotten[0] << 8 | gotten[1] << 0;
				struct timespec duration = timespec_sub(end, begun);
				uintmax_t ms = (uintmax_t) duration.tv_sec * (uintmax_t) 1000 +
				               (uintmax_t) ((duration.tv_nsec / 1000) / 1000);
				unsigned int us = (duration.tv_nsec / 1000) % 1000;
				printf("%zu bytes from %s (%s): icmp_seq=%u time=%ju.%03u ms\n",
				       (size_t) (PAYLOAD_SIZE + 8), host, host_address,
				       sequence, ms, us);
			}
		}
	}

	return 0;
}
