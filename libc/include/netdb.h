/*
 * Copyright (c) 2013, 2014 Jonas 'Sortie' Termansen.
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
 * netdb.h
 * Definitions for network database operations.
 */

#ifndef _INCLUDE_NETDB_H
#define _INCLUDE_NETDB_H

#include <sys/cdefs.h>

#include <sys/__/types.h>

#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __in_port_t_defined
#define __in_port_t_defined
typedef uint16_t in_port_t;
#endif

#ifndef __in_addr_t_defined
#define __in_addr_t_defined
typedef uint32_t in_addr_t;
#endif

#ifndef __socklen_t_defined
#define __socklen_t_defined
typedef __socklen_t socklen_t;
#endif

/* struct hostent will not be implemented. */

struct netent
{
	char* n_name;
	char** n_aliases;
	int n_addrtype;
	uint32_t n_net;
};

struct protoent
{
	char* p_name;
	char** p_aliases;
	int p_proto;
};

struct servent
{
	char* s_name;
	char** s_aliases;
	char* s_proto;
	int s_port;
};

struct addrinfo
{
	int ai_flags;
	int ai_family;
	int ai_socktype;
	int ai_protocol;
	socklen_t ai_addrlen;
	struct sockaddr* ai_addr;
	char* ai_canonname;
	struct addrinfo* ai_next;
};

/* TODO: Figure out how this relates to Sortix. */
#define IPPORT_RESERVED 1024

#define AI_PASSIVE (1<<0)
#define AI_CANONNAME (1<<1)
#define AI_NUMERICHOST (1<<2)
#define AI_NUMERICSERV (1<<3)
#define AI_V4MAPPED (1<<4)
#define AI_ALL (1<<5)
#define AI_ADDRCONFIG (1<<6)

#define NI_NOFQDN (1<<0)
#define NI_NUMERICHOST (1<<1)
#define NI_NAMEREQD (1<<2)
#define NI_NUMERICSERV (1<<3)
#define NI_NUMERICSCOPE (1<<4)
#define NI_DGRAM (1<<5)

#define EAI_AGAIN 1
#define EAI_BADFLAGS 2
#define EAI_FAIL 3
#define EAI_FAMILY 4
#define EAI_MEMORY 5
#define EAI_NONAME 6
#define EAI_SERVICE 7
#define EAI_SOCKTYPE 8
#define EAI_SYSTEM 9
#define EAI_OVERFLOW 10

/* These are not standardized, but are provided on other platforms and existing
   sofware uses them, so let's just provide ourselves. */
#define NI_MAXHOST 1025
#define NI_MAXSERV 32

/* endhostent will not be implemented. */
__attribute__((__warning__("endnetent() is IPv4-only and does nothing on this system")))
void endnetent(void);
__attribute__((__warning__("endprotoent() is not implemented yet")))
void endprotoent(void);
__attribute__((__warning__("endservent() is not implemented yet")))
void endservent(void);
void freeaddrinfo(struct addrinfo*);
const char* gai_strerror(int);
int getaddrinfo(const char* __restrict, const char* __restrict,
                const struct addrinfo* __restrict, struct addrinfo** __restrict);
/* gethostent will not be implemented. */
int getnameinfo(const struct sockaddr* __restrict, socklen_t, char* __restrict,
                socklen_t, char* __restrict, socklen_t, int);
__attribute__((__warning__("getnetbyaddr() is IPv4-only and does nothing on this system")))
struct netent* getnetbyaddr(uint32_t, int);
__attribute__((__warning__("getnetbyname() is IPv4-only and does nothing on this system")))
struct netent* getnetbyname(const char*);
__attribute__((__warning__("getnetent() is IPv4-only and does nothing on this system")))
struct netent* getnetent(void);
__attribute__((__warning__("getprotobyname() is not implemented yet")))
struct protoent* getprotobyname(const char*);
__attribute__((__warning__("getprotobynumber() is not implemented yet")))
struct protoent* getprotobynumber(int);
__attribute__((__warning__("getprotoent() is not implemented yet")))
struct protoent* getprotoent(void);
__attribute__((__warning__("getservbyname() is not implemented yet")))
struct servent* getservbyname(const char*, const char*);
__attribute__((__warning__("getservbyport() is not implemented yet")))
struct servent* getservbyport(int, const char*);
__attribute__((__warning__("getservent() is not implemented yet")))
struct servent* getservent(void);
/* sethostent will not be implemented. */
__attribute__((__warning__("setnetent() is IPv4-only and does nothing on this system")))
void setnetent(int);
__attribute__((__warning__("setprotoent() is not implemented yet")))
void setprotoent(int);
__attribute__((__warning__("setservent() is not implemented yet")))
void setservent(int);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
