/*
 * Copyright (c) 2013, 2015, 2016, 2022 Jonas 'Sortie' Termansen.
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
 * netdb/getaddrinfo.c
 * Network address and service translation.
 */

#include <sys/dnsconfig.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <endian.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <timespec.h>
#include <unistd.h>

#define DNS_SIZE 512
#define DNS_NAME_MAX 255
#define DNS_LABEL_MAX 64

struct dns_header
{
	uint16_t id;
	uint16_t flags;
	uint16_t qdcount;
	uint16_t ancount;
	uint16_t nscount;
	uint16_t arcount;
};

struct dns_question
{
	uint16_t qtype;
	uint16_t qclass;
};

struct dns_record
{
	uint16_t type;
	uint16_t class;
	uint16_t ttl_high;
	uint16_t ttl_low;
	uint16_t rdlength;
};

#define DNS_HEADER_FLAGS_RCODE_MASK (0xF << 0)
#define DNS_HEADER_FLAGS_RCODE_NO (0 << 0)
#define DNS_HEADER_FLAGS_RCODE_FORMAT (1 << 0)
#define DNS_HEADER_FLAGS_RCODE_SERVER (2 << 0)
#define DNS_HEADER_FLAGS_RCODE_NAME (3 << 0)
#define DNS_HEADER_FLAGS_RCODE_NOT_IMPLEMENTED (4 << 0)
#define DNS_HEADER_FLAGS_RCODE_REFUSED (5 << 0)
#define DNS_HEADER_FLAGS_RA (1 << 7)
#define DNS_HEADER_FLAGS_RD (1 << 8)
#define DNS_HEADER_FLAGS_TC (1 << 9)
#define DNS_HEADER_FLAGS_AA (1 << 10)
#define DNS_HEADER_FLAGS_OPCODE_MASK (0xF << 11)
#define DNS_HEADER_FLAGS_OPCODE_QUERY (0 << 11)
#define DNS_HEADER_FLAGS_OPCODE_IQUERY (1 << 11)
#define DNS_HEADER_FLAGS_OPCODE_STATUS (2 << 11)
#define DNS_HEADER_FLAGS_QR (1 << 15)

#define DNS_TYPE_A 1
#define DNS_TYPE_NS 2
#define DNS_TYPE_MD 3
#define DNS_TYPE_MF 4
#define DNS_TYPE_CNAME 5
#define DNS_TYPE_SOA 6
#define DNS_TYPE_MB 7
#define DNS_TYPE_MG 8
#define DNS_TYPE_MR 9
#define DNS_TYPE_NULL 10
#define DNS_TYPE_WKS 11
#define DNS_TYPE_PTR 12
#define DNS_TYPE_HINFO 13
#define DNS_TYPE_MINFO 14
#define DNS_TYPE_MX 15
#define DNS_TYPE_TXT 16
#define DNS_TYPE_AAAA 28

#define DNS_QTYPE_AXFR 252
#define DNS_QTYPE_MAILB 253
#define DNS_QTYPE_MAILA 254
#define DNS_QTYPE_ANY 255

#define DNS_CLASS_IN 1
#define DNS_CLASS_CS 2
#define DNS_CLASS_CH 3
#define DNS_CLASS_HS 4

#define DNS_QCLASS_ANY 255

static bool encode_dns_header(unsigned char* msg,
                              size_t* offset_ptr,
                              const struct dns_header* hdrin)
{
	size_t offset = *offset_ptr;
	struct dns_header hdr;
	if ( DNS_SIZE - offset < sizeof(hdr) )
		return false;
	hdr.id = htobe16(hdrin->id);
	hdr.flags = htobe16(hdrin->flags);
	hdr.qdcount = htobe16(hdrin->qdcount);
	hdr.ancount = htobe16(hdrin->ancount);
	hdr.nscount = htobe16(hdrin->nscount);
	hdr.arcount = htobe16(hdrin->arcount);
	memcpy(msg + offset, &hdr, sizeof(hdr));
	*offset_ptr = offset + sizeof(hdr);
	return true;
}

static bool encode_dns_byte(unsigned char* msg,
                            size_t* offset_ptr,
                            unsigned char byte)
{
	size_t offset = *offset_ptr;
	if ( DNS_SIZE - offset < 1 )
		return false;
	msg[offset] = byte;
	*offset_ptr = offset + 1;
	return true;
}

// TODO: Enforce things like proper use of dashes and restrict to allowed ASCII
//       sequences (but see internationalized domain names).
// TODO: Simplify this further.
static bool is_valid_name(const char* name)
{
	size_t index = 0;
	size_t namelen = 0;
	if ( !name[0] )
		return false; /* unexpected end of input */
	while ( name[index] )
	{
		if ( !strcmp(name + index, ".") )
			break;
		if ( name[index] == '.' )
			return false; /* empty label */
		size_t length = strcspn(name + index, ".");
		if ( DNS_LABEL_MAX <= length )
			return false; /* label too long */
		if ( namelen++ == DNS_NAME_MAX )
			return false; /* name is too long */
		for ( size_t i = 0; i < length; i++ )
		{
			if ( namelen++ == DNS_NAME_MAX )
				return false; /* name is too long */
		}
		index += length;
		if ( name[index] == '.' )
			index++;
	}
	if ( namelen++ == DNS_NAME_MAX )
		return false;
	return true;
}

static bool encode_dns_name(unsigned char* msg,
                            size_t* offset_ptr,
                            const char* name)
{
	size_t index = 0;
	size_t namelen = 0;
	if ( !name[0] )
		return false;
	while ( name[index] )
	{
		if ( !strcmp(name + index, ".") )
			break;
		if ( name[index] == '.' )
			return false;
		size_t length = strcspn(name + index, ".");
		if ( DNS_LABEL_MAX <= length )
			return false;
		if ( namelen++ == DNS_NAME_MAX )
			return false;
		if ( !encode_dns_byte(msg, offset_ptr, length & 0xFF) )
			return false;
		for ( size_t i = 0; i < length; i++ )
		{
			if ( namelen++ == DNS_NAME_MAX )
				return false;
			if ( !encode_dns_byte(msg, offset_ptr, name[index + i]) )
				return false;
		}
		index += length;
		if ( name[index] == '.' )
			index++;
	}
	if ( namelen++ == DNS_NAME_MAX )
		return false;
	if ( !encode_dns_byte(msg, offset_ptr, 0) )
		return false;
	return true;
}

static bool encode_dns_question(unsigned char* msg,
                                size_t* offset_ptr,
                                const char* name,
                                const struct dns_question* qsin)
{
	if ( !encode_dns_name(msg, offset_ptr, name) )
		return false;
	size_t offset = *offset_ptr;
	struct dns_question qs;
	if ( DNS_SIZE - offset < sizeof(qs) )
		return false;
	qs.qtype = htobe16(qsin->qtype);
	qs.qclass = htobe16(qsin->qclass);
	memcpy(msg + offset, &qs, sizeof(qs));
	*offset_ptr = offset + sizeof(qs);
	return true;
}

static bool decode_dns_header(const unsigned char* msg,
                              size_t* offset_ptr,
                              size_t msg_size,
                              struct dns_header* hdrout)
{
	size_t offset = *offset_ptr;
	struct dns_header hdr;
	if ( msg_size - offset < sizeof(hdr) )
		return false;
	memcpy(&hdr, msg + offset, sizeof(hdr));
	hdrout->id = be16toh(hdr.id);
	hdrout->flags = be16toh(hdr.flags);
	hdrout->qdcount = be16toh(hdr.qdcount);
	hdrout->ancount = be16toh(hdr.ancount);
	hdrout->nscount = be16toh(hdr.nscount);
	hdrout->arcount = be16toh(hdr.arcount);
	*offset_ptr = offset + sizeof(hdr);
	return true;
}

static bool decode_dns_byte(const unsigned char* msg,
                            size_t* offset_ptr,
                            size_t msg_size,
                            unsigned char* byte)
{
	size_t offset = *offset_ptr;
	if ( msg_size <= offset || msg_size - offset < 1 )
		return false;
	*byte = msg[offset];
	*offset_ptr = offset + 1;
	return true;
}

static bool decode_dns_name(const unsigned char* msg,
                            size_t* offset_ptr,
                            size_t msg_size,
                            char* name)
{
	bool real_offset_set = false;
	size_t real_offset = 0;
	size_t index = 0;
	size_t namelen = 0;
	uint8_t b;
	while ( true )
	{
		if ( namelen++ == DNS_NAME_MAX )
			return false;
		if ( !decode_dns_byte(msg, offset_ptr, msg_size, &b) )
			return false;
		if ( 0xC0 & b )
		{
			namelen--;
			size_t ptr = (b & 0x3F) << 8;
			if ( !decode_dns_byte(msg, offset_ptr, msg_size, &b) )
				return false;
			ptr |= b;
			if ( !real_offset_set )
			{
				real_offset = *offset_ptr;
				real_offset_set = true;
			}
			*offset_ptr = ptr;
			continue;
		}
		size_t length = b;
		if ( DNS_LABEL_MAX <= length )
			return false;
		if ( !length )
			break;
		if ( index )
			name[index++] = '.';
		for ( size_t i = 0; i < length; i++ )
		{
			if ( namelen++ == DNS_NAME_MAX )
				return false;
			if ( !decode_dns_byte(msg, offset_ptr, msg_size, &b) )
				return false;
			// TODO: Handle if b == '.'.
			name[index++] = b;
		}
	}
	name[index++] = '.';
	name[index] = '\0';
	if ( real_offset_set )
		*offset_ptr = real_offset;
	return true;
}

static bool decode_dns_question(const unsigned char* msg,
                                size_t* offset_ptr,
                                size_t msg_size,
                                char* name,
                                struct dns_question* qsout)
{
	if ( !decode_dns_name(msg, offset_ptr, msg_size, name) )
		return false;
	size_t offset = *offset_ptr;
	struct dns_question qs;
	if ( msg_size <= offset || msg_size - offset < sizeof(qs) )
		return false;
	memcpy(&qs, msg + offset, sizeof(qs));
	qsout->qtype = be16toh(qs.qtype);
	qsout->qclass = be16toh(qs.qclass);
	*offset_ptr = offset + sizeof(qs);
	return true;
}

static bool decode_dns_record(const unsigned char* msg,
                              size_t* offset_ptr,
                              size_t msg_size,
                              char* name,
                              struct dns_record* rrout)
{
	if ( !decode_dns_name(msg, offset_ptr, msg_size, name) )
		return false;
	size_t offset = *offset_ptr;
	struct dns_record rr;
	if ( msg_size <= offset || msg_size - offset < sizeof(rr) )
		return false;
	memcpy(&rr, msg + offset, sizeof(rr));
	rrout->type = be16toh(rr.type);
	rrout->class = be16toh(rr.class);
	rrout->ttl_high = be16toh(rr.ttl_high);
	rrout->ttl_low = be16toh(rr.ttl_low);
	rrout->rdlength = be16toh(rr.rdlength);
	*offset_ptr = offset + sizeof(rr);
	return true;
}

static bool linkaddrinfo(struct addrinfo** restrict* res_ptr,
                         const struct addrinfo* restrict templ)
{
	struct addrinfo* link =
		(struct addrinfo*) calloc(1, sizeof(struct addrinfo));
	if ( !link )
		return false;
	link->ai_flags = templ->ai_flags;
	link->ai_family = templ->ai_family;
	link->ai_socktype = templ->ai_socktype;
	link->ai_protocol = templ->ai_protocol;
	link->ai_addrlen = templ->ai_addrlen;
	link->ai_addr = (struct sockaddr*) malloc(templ->ai_addrlen);
	if ( !link->ai_addr )
		return free(link), false;
	memcpy(link->ai_addr, templ->ai_addr, templ->ai_addrlen);
	link->ai_canonname = NULL;
	if ( templ->ai_canonname )
	{
		link->ai_canonname = strdup(templ->ai_canonname);
		if ( !link->ai_canonname )
			return free(link->ai_addr), free(link), false;
	}
	**res_ptr = link;
	*res_ptr = &link->ai_next;
	return true;
}

int getaddrinfo(const char* restrict node,
                const char* restrict servname,
                const struct addrinfo* restrict hints,
                struct addrinfo** restrict res)
{
	int flags = 0;
	int family = AF_UNSPEC;
	int socktype = 0;
	int protocol = 0;

	if ( hints )
	{
		flags = hints->ai_flags;
		family = hints->ai_family;
		socktype = hints->ai_socktype;
		protocol = hints->ai_protocol;
	}

	// TODO: Implement missing flags.
	// TODO: Revisit AI_ADDRCONFIG when IPv6 is implemented.
	int supported = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV |
	                AI_CANONNAME | AI_ADDRCONFIG;
	if ( flags & ~supported )
		return EAI_BADFLAGS;

	// TODO: IPv6 support.
	if ( family != AF_UNSPEC && family != AF_INET )
		return EAI_FAMILY;
	family = AF_INET;

	if ( socktype == 0 )
		socktype = SOCK_STREAM;

	in_port_t port = 0;
	if ( servname )
	{
		int errval = flags & AI_NUMERICSERV ? EAI_NONAME : EAI_SERVICE;
		if ( isspace((unsigned char) servname[0]) )
			return errval;
		const char* end;
		long portl = strtol(servname, (char**) &end, 10);
		if ( end[0] )
			return errval;
		if ( (in_port_t) portl != portl )
			return errval;
		port = portl;
	}

	struct addrinfo** res_orig = res;
	*res = NULL;

	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	struct sockaddr_in6 sin6;
	memset(&sin6, 0, sizeof(sin6));

	if ( !node || !strcasecmp(node, "localhost") )
	{
		if ( !node && !servname )
			return EAI_NONAME;
		bool any = false;
		if ( family == AF_UNSPEC || family == AF_INET )
		{
			sin.sin_family = AF_INET;
			sin.sin_port = htobe16(port);
			if ( flags & AI_PASSIVE )
				sin.sin_addr.s_addr = htobe32(INADDR_ANY);
			else
				sin.sin_addr.s_addr = htobe32(INADDR_LOOPBACK);
			struct addrinfo templ;
			memset(&templ, 0, sizeof(templ));
			templ.ai_family = sin.sin_family;
			templ.ai_socktype = socktype;
			templ.ai_protocol = protocol;
			templ.ai_addrlen = sizeof(sin);
			templ.ai_addr = (struct sockaddr*) &sin;
			if ( flags & AI_CANONNAME )
				templ.ai_canonname = "localhost";
			if ( !linkaddrinfo(&res, &templ) )
				return freeaddrinfo(*res_orig), EAI_MEMORY;
			any = true;
		}
		if ( family == AF_UNSPEC || family == AF_INET6 )
		{
			sin6.sin6_family = AF_INET6;
			sin6.sin6_port = htobe16(port);
			sin6.sin6_flowinfo = 0;
			if ( flags & AI_PASSIVE )
				sin6.sin6_addr = in6addr_any;
			else
				sin6.sin6_addr = in6addr_loopback;
			sin6.sin6_scope_id = 0;
			struct addrinfo templ;
			memset(&templ, 0, sizeof(templ));
			templ.ai_family = sin6.sin6_family;
			templ.ai_socktype = socktype;
			templ.ai_protocol = protocol;
			templ.ai_addrlen = sizeof(sin6);
			templ.ai_addr = (struct sockaddr*) &sin6;
			if ( flags & AI_CANONNAME )
				templ.ai_canonname = "localhost";
			if ( !linkaddrinfo(&res, &templ) )
				return freeaddrinfo(*res_orig), EAI_MEMORY;
			any = true;
		}
		if ( any )
			return 0;
		return EAI_NONAME;
	}

	if ( (family == AF_UNSPEC || family == AF_INET) )
	{
		if ( inet_pton(AF_INET, node, &sin.sin_addr) == 1 )
		{
			sin.sin_family = AF_INET;
			sin.sin_port = htobe16(port);
			struct addrinfo templ;
			memset(&templ, 0, sizeof(templ));
			templ.ai_family = sin.sin_family;
			templ.ai_socktype = socktype;
			templ.ai_protocol = protocol;
			templ.ai_addrlen = sizeof(sin);
			templ.ai_addr = (struct sockaddr*) &sin;
			if ( !linkaddrinfo(&res, &templ) )
				return freeaddrinfo(*res_orig), EAI_MEMORY;
			return 0;
		}
	}

	if ( (family == AF_UNSPEC || family == AF_INET6) )
	{
		if ( inet_pton(AF_INET6, node, &sin6.sin6_addr) == 1 )
		{
			sin6.sin6_family = AF_INET6;
			sin6.sin6_port = htobe16(port);
			sin6.sin6_flowinfo = 0;
			sin6.sin6_scope_id = 0;
			struct addrinfo templ;
			memset(&templ, 0, sizeof(templ));
			templ.ai_family = sin6.sin6_family;
			templ.ai_socktype = socktype;
			templ.ai_protocol = protocol;
			templ.ai_addrlen = sizeof(sin6);
			templ.ai_addr = (struct sockaddr*) &sin6;
			if ( !linkaddrinfo(&res, &templ) )
				return freeaddrinfo(*res_orig), EAI_MEMORY;
			return 0;
		}
	}

	if ( flags & AI_NUMERICHOST )
		return EAI_NONAME;

	if ( !is_valid_name(node) )
		return EAI_NONAME;

	size_t encoded_size = 0;
	unsigned char encoded[DNS_SIZE];
	if ( !encode_dns_name(encoded, &encoded_size, node) )
		return EAI_NONAME;
	size_t decoded_size = 0;
	char target[DNS_NAME_MAX + 1];
	if ( !decode_dns_name(encoded, &decoded_size, encoded_size, target) )
		return EAI_NONAME;

	int cname_retries = 0;
retry_cname:
	if ( 5 < cname_retries++ )
		return EAI_NONAME;

	struct dnsconfig dnsconfig;
	if ( getdnsconfig(&dnsconfig) < 0 )
		return EAI_SYSTEM;

	// TODO: Potentially do a blocking wait for DNS configuration to come up (or
	//       for the automatic configuration to time out) if it isn't ready yet.
	if ( dnsconfig.servers_count == 0 )
		return EAI_NONAME;

	// TODO: Send requests to all the servers rather than picking the first one
	//       a socket can be made for.
	int fd = -1;
	size_t server_index = 0;
	struct dnsconfig_server* server = NULL;
	for ( ; server_index < dnsconfig.servers_count; server_index++ )
	{
		server = &dnsconfig.servers[server_index];
		if ( 0 <= (fd = socket(server->family, SOCK_DGRAM, 0)) )
			break;
	}
	if ( server_index == dnsconfig.servers_count )
		return EAI_SYSTEM;

	struct sockaddr* addr;
	size_t addr_size;
	if ( server->family == AF_INET )
	{
		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_port = htobe16(53);
		memcpy(&sin.sin_addr, &server->addr, sizeof(sin.sin_addr));
		addr = (struct sockaddr*) &sin;
		addr_size = sizeof(sin);
	}
	else if ( server->family == AF_INET6 )
	{
		memset(&sin6, 0, sizeof(sin6));
		sin6.sin6_family = AF_INET6;
		sin6.sin6_port = htobe16(53);
		memcpy(&sin6.sin6_addr, &server->addr, sizeof(sin6.sin6_addr));
		addr = (struct sockaddr*) &sin6;
		addr_size = sizeof(sin6);
	}
	else
		return EAI_FAMILY;

	if ( connect(fd, addr, addr_size) < 0 )
		return close(fd), EAI_SYSTEM;

	uint16_t id = arc4random() & 0xFFFF;

	unsigned char req[DNS_SIZE];
	size_t req_size = 0;
	struct dns_header hdr;
	hdr.id = id;
	hdr.flags = DNS_HEADER_FLAGS_RD;
	hdr.qdcount = 1;
	hdr.ancount = 0;
	hdr.nscount = 0;
	hdr.arcount = 0;
	if ( !encode_dns_header(req, &req_size, &hdr) )
		return close(fd), EAI_OVERFLOW;
	struct dns_question qs;
	qs.qtype = 0;
	if ( family == AF_INET )
		qs.qtype = DNS_TYPE_A;
	else if ( family == AF_INET6 )
		qs.qtype = DNS_TYPE_AAAA;
	qs.qclass = DNS_CLASS_IN;
	if ( !encode_dns_question(req, &req_size, node, &qs) )
		return close(fd), EAI_OVERFLOW;

	struct timespec last_sent = timespec_nul();
	struct timespec timeout = timespec_nul();

	unsigned char resp[DNS_SIZE];
	ssize_t resp_size;
	size_t offset;
	unsigned int retransmissions = 0;
	while ( true )
	{
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		struct timespec since_sent = timespec_sub(now, last_sent);
		if ( timespec_le(timeout, since_sent) )
		{
			if ( 2 <= retransmissions )
				return close(fd), EAI_AGAIN;
			ssize_t amount = send(fd, req, req_size, 0);
			if ( amount < 0 )
				return close(fd), EAI_SYSTEM;
			timeout = timespec_make(2, 500000000);
			last_sent = now;
			retransmissions++;
		}
		struct timespec left =
			timespec_sub(timespec_add(last_sent, timeout), now);
		struct pollfd pfd = { 0 };
		pfd.fd = fd;
		pfd.events = POLLIN;
		int num_events = ppoll(&pfd, 1, &left, NULL);
		if ( num_events < 0 )
			return close(fd), EAI_SYSTEM;
		if ( num_events == 0 )
			continue;
		resp_size = recv(fd, resp, sizeof(resp), 0);
		if ( resp_size < 0 )
			return close(fd), EAI_SYSTEM;
		offset = 0;
		if ( !decode_dns_header(resp, &offset, resp_size, &hdr) )
			continue;
		if ( hdr.id != id )
			continue;
		break;
	}

	// TODO: Return the correct errors below. It may be the best behavior to
	//       simply drop any responses with errors.
	uint16_t rcode = hdr.flags & DNS_HEADER_FLAGS_RCODE_MASK;
	if ( rcode == DNS_HEADER_FLAGS_RCODE_FORMAT )
		return close(fd), EAI_FAIL;
	else if ( rcode == DNS_HEADER_FLAGS_RCODE_SERVER )
		return close(fd), EAI_FAIL;
	else if ( rcode == DNS_HEADER_FLAGS_RCODE_NAME )
		return close(fd), EAI_NONAME;
	else if ( rcode == DNS_HEADER_FLAGS_RCODE_NOT_IMPLEMENTED )
		return close(fd), EAI_FAIL;
	else if ( rcode == DNS_HEADER_FLAGS_RCODE_REFUSED )
		return close(fd), EAI_FAIL;
	else if ( rcode != DNS_HEADER_FLAGS_RCODE_NO )
		return close(fd), EAI_FAIL;

	// TODO: Reconnect to server using TCP to get full response.
	if ( hdr.flags & DNS_HEADER_FLAGS_TC )
		return close(fd), EAI_FAIL;

	// TODO: Check query bit.

	for ( uint16_t i = 0; i < hdr.qdcount; i++ )
	{
		char name[DNS_NAME_MAX + 1];
		if ( !decode_dns_question(resp, &offset, resp_size, name, &qs) )
			return close(fd), EAI_OVERFLOW;
	}

	bool any = false;
	bool found_cname = false;

	for ( uint16_t i = 0; i < hdr.ancount; i++ )
	{
		char name[DNS_NAME_MAX + 1];
		struct dns_record rr;
		if ( !decode_dns_record(resp, &offset, resp_size, name, &rr) )
			return close(fd), freeaddrinfo(*res_orig), EAI_OVERFLOW;
		bool match = strcmp(name, target) == 0;
		// TODO: Support aliases.
		if ( rr.class == DNS_CLASS_IN && rr.type == DNS_TYPE_A )
		{
			unsigned char ip[4];
			for ( size_t i = 0; i < 4; i++ )
				if ( !decode_dns_byte(resp, &offset, resp_size, &ip[i]) )
					return close(fd), freeaddrinfo(*res_orig), EAI_OVERFLOW;
			if ( match && (family == AF_UNSPEC || family == AF_INET) )
			{
				memset(&sin, 0, sizeof(sin));
				sin.sin_family = AF_INET;
				sin.sin_port = htobe16(port);
				memcpy(&sin.sin_addr, ip, sizeof(sin.sin_addr));
				struct addrinfo templ;
				memset(&templ, 0, sizeof(templ));
				templ.ai_family = sin.sin_family;
				templ.ai_socktype = socktype;
				templ.ai_protocol = protocol;
				templ.ai_addrlen = sizeof(sin);
				templ.ai_addr = (struct sockaddr*) &sin;
				if ( !linkaddrinfo(&res, &templ) )
					return close(fd), freeaddrinfo(*res_orig), EAI_MEMORY;
				any = true;
			}
		}
		else if ( rr.class == DNS_CLASS_IN && rr.type == DNS_TYPE_AAAA )
		{
			unsigned char ip[16];
			for ( size_t i = 0; i < 16; i++ )
				if ( !decode_dns_byte(resp, &offset, resp_size, &ip[i]) )
					return close(fd), freeaddrinfo(*res_orig), EAI_OVERFLOW;
			if ( match && (family == AF_UNSPEC || family == AF_INET6) )
			{
				sin6.sin6_family = AF_INET6;
				sin6.sin6_port = htobe16(port);
				sin6.sin6_flowinfo = 0;
				memcpy(&sin6.sin6_addr, ip, sizeof(sin6.sin6_addr));
				sin6.sin6_scope_id = 0;
				struct addrinfo templ;
				memset(&templ, 0, sizeof(templ));
				templ.ai_family = sin6.sin6_family;
				templ.ai_socktype = socktype;
				templ.ai_protocol = protocol;
				templ.ai_addrlen = sizeof(sin6);
				templ.ai_addr = (struct sockaddr*) &sin6;
				if ( !linkaddrinfo(&res, &templ) )
					return close(fd), freeaddrinfo(*res_orig), EAI_MEMORY;
				any = true;
			}
		}
		else if ( rr.type == DNS_TYPE_CNAME )
		{
			char cname[DNS_NAME_MAX + 1];
			if ( !decode_dns_name(resp, &offset, resp_size, cname) )
				return close(fd), freeaddrinfo(*res_orig), EAI_OVERFLOW;
			if ( match )
			{
				// TODO: Report CNAME to caller.
				memcpy(target, cname, sizeof(target));
				found_cname = true;
			}
		}
		else
		{
			for ( size_t i = 0; i < rr.rdlength; i++ )
			{
				unsigned char b;
				if ( !decode_dns_byte(resp, &offset, resp_size, &b) )
					return close(fd), freeaddrinfo(*res_orig), EAI_OVERFLOW;
			}
		}
	}

	close(fd);

	if ( !any )
	{
		if ( found_cname )
		{
			freeaddrinfo(*res_orig);
			res = res_orig;
			*res = NULL;
			goto retry_cname;

		}
		return EAI_NONAME;
	}

	return 0;
}
