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
 * nc.c
 * Command line network client and server.
 */

#include <sys/socket.h>

#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fd;
static char outgoing[65536];
static char incoming[65536];

static void* write_thread(void* ctx)
{
	(void) ctx;
	ssize_t amount;
	while ( 0 < (amount = read(0, outgoing, sizeof(outgoing))) )
	{
		ssize_t sofar = 0;
		while ( sofar < amount )
		{
			// TODO: How to handle EPIPE. Is MSG_NOSIGNAL desirable?
			int flags = MSG_NOSIGNAL;
			ssize_t done = send(fd, outgoing + sofar, amount - sofar, flags);
			if ( done <= 0 )
				err(1, "send");
			sofar += done;
		}
	}
	if ( amount < 0 )
		err(1, "stdin: read");
	if ( shutdown(fd, SHUT_WR) < 0 )
		err(1, "shutdown");
	sleep(1);
	return NULL;
}

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
	bool flag_ipv4 = false;
	bool flag_ipv6 = false;
	bool flag_listen = false;
	bool flag_udp = false;
	bool flag_verbose = false;

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
			case '4': flag_ipv4 = true; break;
			case '6': flag_ipv6 = true; break;
			case 'l': flag_listen = true; break;
			case 'u': flag_udp = true; break;
			case 'v': flag_verbose = true; break;
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

	if ( 2 <= flag_ipv4 + flag_ipv6 )
		errx(1, "The -4 and -6 options are incompatible");

	struct addrinfo hints =
	{
		.ai_flags = flag_listen ? AI_PASSIVE : 0,
		.ai_family = flag_ipv6 ? AF_INET6 : flag_ipv4 ? AF_INET : AF_UNSPEC,
		.ai_socktype = flag_udp ? SOCK_DGRAM : SOCK_STREAM,
		.ai_protocol = 0,
	};
	struct addrinfo* res0 = NULL;
	int status = getaddrinfo(host, service, &hints, &res0);
	if ( status == EAI_SYSTEM )
		err(1, "%s", host);
	if ( status )
		errx(1, "%s: %s", host, gai_strerror(status));
	if ( !res0 )
		errx(1, "%s: %s", host, gai_strerror(EAI_NONAME));

	for ( struct addrinfo* res = res0; res; res = res->ai_next )
	{
		if ( (fd = socket(res->ai_family, res->ai_socktype,
		                  res->ai_protocol)) < 0 )
		{
			if ( res->ai_next )
				continue;
			err(1, "socket");
		}
		if ( flag_listen )
		{
			if ( bind(fd, res->ai_addr, res->ai_addrlen) < 0 )
			{
				close(fd);
				if ( res->ai_next )
					continue;
				err(1, "bind: %s: %s", host, service);
			}
			if ( listen(fd, 1) < 0 )
				err(1, "listen: %s: %s", host, service);
			if ( flag_verbose )
			{
				char host[NI_MAXHOST];
				char serv[NI_MAXSERV];
				if ( getnameinfo(res->ai_addr, res->ai_addrlen,
				                 host, sizeof(host), serv, sizeof(serv),
				                 NI_NUMERICHOST | NI_NUMERICSERV) < 0 )
				{
					strlcpy(host, "unknown", sizeof(host));
					strlcpy(serv, "unknown", sizeof(serv));
				}
				fprintf(stderr, "Listening on %s:%s\n", host, serv);
			}
			// TODO: UDP support.
			struct sockaddr_storage addr;
			socklen_t addrlen = sizeof(addr);
			int nfd = accept(fd, (struct sockaddr*) &addr, &addrlen);
			if ( nfd < 0 )
				err(1, "accept: %s: %s", host, service);
			close(fd);
			fd = nfd;
			if ( flag_verbose )
			{
				char host[NI_MAXHOST];
				char serv[NI_MAXSERV];
				if ( getnameinfo((const struct sockaddr*) &addr, addrlen,
				                 host, sizeof(host), serv, sizeof(serv),
				                 NI_NUMERICHOST | NI_NUMERICSERV) < 0 )
				{
					strlcpy(host, "unknown", sizeof(host));
					strlcpy(serv, "unknown", sizeof(serv));
				}
				fprintf(stderr, "Connection from %s:%s\n", host, serv);
			}
		}
		else
		{
			if ( connect(fd, res->ai_addr, res->ai_addrlen) < 0 )
			{
				close(fd);
				if ( res->ai_next )
					continue;
				err(1, "connect: %s: %s", host, service);
			}
			if ( flag_verbose )
			{
				char host[NI_MAXHOST];
				char serv[NI_MAXSERV];
				if ( getnameinfo(res->ai_addr, res->ai_addrlen,
				                 host, sizeof(host), serv, sizeof(serv),
				                 NI_NUMERICHOST | NI_NUMERICSERV) < 0 )
				{
					strlcpy(host, "unknown", sizeof(host));
					strlcpy(serv, "unknown", sizeof(serv));
				}
				fprintf(stderr, "Connected to %s:%s\n", host, serv);
			}
		}
		break;
	}

	freeaddrinfo(res0);

	pthread_t write_pthread;
	int errnum;
	if ( (errnum = pthread_create(&write_pthread, NULL, write_thread, NULL)) )
	{
		errno = errnum;
		err(1, "pthread_create");
	}

	ssize_t amount;
	while ( 0 < (amount = recv(fd, incoming, sizeof(incoming), 0)) )
	{
		ssize_t sofar = 0;
		while ( sofar < amount )
		{
			// TODO: How to handle if this causes SIGPIPE?
			ssize_t done = write(1, incoming + sofar, amount - sofar);
			if ( done <= 0 )
				err(1, "stdout: write");
			sofar += done;
		}
	}
	if ( amount < 0 )
		err(1, "recv");

	pthread_join(write_pthread, NULL);

	close(fd);

	return 0;
}
