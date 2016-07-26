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
 * getaddrinfo.c
 * Wrapper program for getaddrinfo(3).
 */

#include <sys/socket.h>

#include <arpa/inet.h>
#include <err.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void compact_arguments(int* argc, char*** argv)
{
	for ( int i = 0; i < *argc; i++ )
	{
		while ( i < *argc && !(*argv)[i] )
		{
			for ( int n = i; n < *argc; n++ )
				(*argv)[n] = (*argv)[n+1];
			(*argc)--;
		}
	}
}

int main(int argc, char* argv[])
{
	const char* argv0 = argv[0];
	for ( int i = 1; i < argc; i++ )
	{
		const char* arg = argv[i];
		if ( arg[0] != '-' || !arg[1] )
			continue;
		argv[i] = NULL;
		if ( !strcmp(arg, "--") )
			break;
		if ( arg[1] != '-' )
		{
			char c;
			while ( (c = *++arg) ) switch ( c )
			{
			default:
				fprintf(stderr, "%s: unknown option -- '%c'\n", argv0, c);
				exit(1);
			}
		}
		else
		{
			fprintf(stderr, "%s: unknown option: %s\n", argv0, arg);
			exit(1);
		}
	}

	compact_arguments(&argc, &argv);

	if ( argc < 2 )
		errx(1, "No host given");
	const char* host = argv[1];
	if ( 3 < argc )
		errx(1, "Unexpected extra operand `%s'", argv[3]);
	const char* service = argv[2];

	struct addrinfo* res = NULL;
	int status = getaddrinfo(host, service, NULL, &res);
	if ( status == EAI_SYSTEM )
		err(1, "%s", host);
	if ( status )
		errx(1, "%s: %s", host, gai_strerror(status));
	for ( struct addrinfo* iter = res; iter; iter = iter->ai_next )
	{
		// TODO: Implement and use getnameinfo(3).
		if ( iter->ai_family == AF_INET )
		{
			struct sockaddr_in* sin = (struct sockaddr_in*) iter->ai_addr;
			char str[INET_ADDRSTRLEN];
			inet_ntop(iter->ai_family, &sin->sin_addr, str, sizeof(str));
			printf("%s\n", str);
		}
		else if ( iter->ai_family == AF_INET6 )
		{
			struct sockaddr_in6* sin6 = (struct sockaddr_in6*) iter->ai_addr;
			char str[INET6_ADDRSTRLEN];
			inet_ntop(iter->ai_family, &sin6->sin6_addr, str, sizeof(str));
			printf("%s\n", str);
		}
		else
			printf("(unknown address family %i)\n", iter->ai_family);
	}
	freeaddrinfo(res);

	return 0;
}
