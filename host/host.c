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
 * host.c
 * Domain name system client.
 */

#include <sys/dnsconfig.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <endian.h>
#include <err.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static size_t encode_dns_header(unsigned char* msg,
                                size_t offset,
                                const struct dns_header* hdrin)
{
	struct dns_header hdr;
	if ( DNS_SIZE - offset < sizeof(hdr) )
		errx(1, "dns message too large");
	hdr.id = htobe16(hdrin->id);
	hdr.flags = htobe16(hdrin->flags);
	hdr.qdcount = htobe16(hdrin->qdcount);
	hdr.ancount = htobe16(hdrin->ancount);
	hdr.nscount = htobe16(hdrin->nscount);
	hdr.arcount = htobe16(hdrin->arcount);
	memcpy(msg + offset, &hdr, sizeof(hdr));
	return offset + sizeof(hdr);
}

static size_t encode_dns_byte(unsigned char* msg,
                              size_t offset,
                              unsigned char byte)
{
	if ( DNS_SIZE - offset < 1 )
		errx(1, "dns message too large");
	msg[offset] = byte;
	return offset + 1;
}

static size_t encode_dns_name(unsigned char* msg,
                              size_t offset,
                              const char* name)
{
	size_t index = 0;
	size_t namelen = 0;
	if ( !name[0] )
		errx(1, "'%s' is not a valid name (unexpected end of input)", name);
	while ( name[index] )
	{
		if ( !strcmp(name + index, ".") )
			break;
		if ( name[index] == '.' )
			errx(1, "'%s' is not a valid name (empty label)", name);
		size_t length = strcspn(name + index, ".");
		if ( DNS_LABEL_MAX <= length )
			errx(1, "'%s' is not a valid name (label too long)", name);
		if ( namelen++ == DNS_NAME_MAX )
			errx(1, "'%s' is not a valid name (name is too long)", name);
		offset = encode_dns_byte(msg, offset, length & 0xFF);
		for ( size_t i = 0; i < length; i++ )
		{
			if ( namelen++ == DNS_NAME_MAX )
				errx(1, "'%s' is not a valid name (name is too long)", name);
			offset = encode_dns_byte(msg, offset, name[index + i]);
		}
		index += length;
		if ( name[index] == '.' )
			index++;
	}
	if ( namelen++ == DNS_NAME_MAX )
		errx(1, "'%s' is not a valid name (name is too long)", name);
	offset = encode_dns_byte(msg, offset, 0);
	return offset;
}

static size_t encode_dns_question(unsigned char* msg,
                                  size_t offset,
                                  const char* name,
                                  const struct dns_question* qsin)
{
	offset = encode_dns_name(msg, offset, name);
	struct dns_question qs;
	if ( DNS_SIZE - offset < sizeof(qs) )
		errx(1, "dns message too large");
	qs.qtype = htobe16(qsin->qtype);
	qs.qclass = htobe16(qsin->qclass);
	memcpy(msg + offset, &qs, sizeof(qs));
	return offset + sizeof(qs);
}

static size_t decode_dns_header(const unsigned char* msg,
                                size_t offset,
                                size_t msg_size,
                                struct dns_header* hdrout)
{
	struct dns_header hdr;
	if ( msg_size - offset < sizeof(hdr) )
		errx(1, "dns message too small");
	memcpy(&hdr, msg + offset, sizeof(hdr));
	hdrout->id = be16toh(hdr.id);
	hdrout->flags = be16toh(hdr.flags);
	hdrout->qdcount = be16toh(hdr.qdcount);
	hdrout->ancount = be16toh(hdr.ancount);
	hdrout->nscount = be16toh(hdr.nscount);
	hdrout->arcount = be16toh(hdr.arcount);
	return offset + sizeof(hdr);
}

static size_t decode_dns_byte(const unsigned char* msg,
                              size_t offset,
                              size_t msg_size,
                              unsigned char* byte)
{
	if ( msg_size <= offset || msg_size - offset < 1 )
		errx(1, "dns message too small");
	*byte = msg[offset];
	return offset + 1;
}

static size_t decode_dns_name(const unsigned char* msg,
                              size_t offset,
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
			errx(1, "name too long");
		offset = decode_dns_byte(msg, offset, msg_size, &b);
		if ( 0xC0 & b )
		{
			namelen--;
			size_t ptr = (b & 0x3F) << 8;
			offset = decode_dns_byte(msg, offset, msg_size, &b);
			ptr |= b;
			if ( !real_offset_set )
			{
				real_offset = offset;
				real_offset_set = true;
			}
			offset = ptr;
			continue;
		}
		size_t length = b;
		if ( DNS_LABEL_MAX <= length )
			errx(1, "label too long");
		if ( !length )
			break;
		if ( index )
			name[index++] = '.';
		for ( size_t i = 0; i < length; i++ )
		{
			if ( namelen++ == DNS_NAME_MAX )
				errx(1, "name too long");
			offset = decode_dns_byte(msg, offset, msg_size, &b);
			// TODO: Handle if b == '.'.
			name[index++] = b;
		}
	}
	name[index++] = '.';
	name[index] = '\0';
	if ( real_offset_set )
		return real_offset;
	return offset;
}

static size_t decode_dns_question(const unsigned char* msg,
                                  size_t offset,
                                  size_t msg_size,
                                  char* name,
                                  struct dns_question* qsout)
{
	offset = decode_dns_name(msg, offset, msg_size, name);
	struct dns_question qs;
	if ( msg_size <= offset || msg_size - offset < sizeof(qs) )
		errx(1, "dns message too small");
	memcpy(&qs, msg + offset, sizeof(qs));
	qsout->qtype = be16toh(qs.qtype);
	qsout->qclass = be16toh(qs.qclass);
	return offset + sizeof(qs);
}

static size_t decode_dns_record(const unsigned char* msg,
                                size_t offset,
                                size_t msg_size,
                                char* name,
                                struct dns_record* rrout)
{
	offset = decode_dns_name(msg, offset, msg_size, name);
	struct dns_record rr;
	if ( msg_size <= offset || msg_size - offset < sizeof(rr) )
		errx(1, "dns message too small");
	memcpy(&rr, msg + offset, sizeof(rr));
	rrout->type = be16toh(rr.type);
	rrout->class = be16toh(rr.class);
	rrout->ttl_high = be16toh(rr.ttl_high);
	rrout->ttl_low = be16toh(rr.ttl_low);
	rrout->rdlength = be16toh(rr.rdlength);
	return offset + sizeof(rr);
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
	int ipv = 4;

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
			case '4': ipv = 4; break;
			case '6': ipv = 6; break;
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

	char nsipstr[INET_ADDRSTRLEN];
	const char* nameserver;
	if ( argc < 3 )
	{
		struct dnsconfig dnscfg;
		if ( getdnsconfig(&dnscfg) < 0 )
			err(1, "dnsconfig");
		bool found = false;
		for ( size_t i = 0; !found && i < dnscfg.servers_count; i++ )
		{
			if ( dnscfg.servers[i].family != AF_INET )
				continue;
			inet_ntop(AF_INET, &dnscfg.servers[i].addr, nsipstr, sizeof(nsipstr));
			found = true;
		}
		if ( !found )
			errx(1, "No nameserver given and no default configured");
		nameserver = nsipstr;
	}
	else
		nameserver = argv[2];

	int port = 4 <= argc ? atoi(argv[3]) : 53;
	if ( 5 <= argc )
		errx(1, "Unexpected extra operand");

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if ( fd < 0 )
		err(1, "socket");

	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htobe16(port);
	if ( inet_pton(AF_INET, nameserver, &addr.sin_addr) < 1 )
		errx(1, "invalid ip address: %s", nameserver);

	if ( connect(fd, (const struct sockaddr*) &addr, sizeof(addr)) < 0 )
		err(1, "connect");

	unsigned char req[DNS_SIZE];
	size_t req_size = 0;
	struct dns_header hdr;
	hdr.id = 0;
	hdr.flags = DNS_HEADER_FLAGS_RD;
	hdr.qdcount = 1;
	hdr.ancount = 0;
	hdr.nscount = 0;
	hdr.arcount = 0;
	req_size = encode_dns_header(req, req_size, &hdr);
	struct dns_question qs;
	qs.qtype = ipv == 4 ? DNS_TYPE_A : DNS_TYPE_AAAA;
	qs.qclass = DNS_CLASS_IN;
	req_size = encode_dns_question(req, req_size, host, &qs);

	ssize_t amount = send(fd, req, req_size, 0);
	if ( amount < 0 )
		err(1, "send");

	// TODO: Use recvfrom to get the server ip.
	unsigned char resp[DNS_SIZE];
	ssize_t resp_size = recv(fd, resp, sizeof(resp), 0);
	if ( resp_size < 0 )
		err(1, "recv");

	// TODO: Verify the response came from the correct ip.

	size_t offset = 0;
	offset = decode_dns_header(resp, offset, resp_size, &hdr);

	// TODO: Verify the response has the correct id.

#if 0
	printf("id = %u\n", hdr.id);
	printf("flags = 0x%X\n", hdr.flags);
	printf("qdcount = %u\n", hdr.qdcount);
	printf("ancount = %u\n", hdr.ancount);
	printf("nscount = %u\n", hdr.nscount);
	printf("arcount = %u\n", hdr.arcount);
#endif

	uint16_t rcode = hdr.flags & DNS_HEADER_FLAGS_RCODE_MASK;
	if ( rcode == DNS_HEADER_FLAGS_RCODE_FORMAT )
		errx(1, "format error");
	else if ( rcode == DNS_HEADER_FLAGS_RCODE_SERVER )
		errx(1, "server error");
	else if ( rcode == DNS_HEADER_FLAGS_RCODE_NAME )
		errx(1, "no such name");
	else if ( rcode == DNS_HEADER_FLAGS_RCODE_NOT_IMPLEMENTED )
		errx(1, "not implemented error");
	else if ( rcode == DNS_HEADER_FLAGS_RCODE_REFUSED )
		errx(1, "refused");
	else if ( rcode != DNS_HEADER_FLAGS_RCODE_NO )
		errx(1, "unknown error (rcode=0x%X)", rcode);

	if ( hdr.flags & DNS_HEADER_FLAGS_TC )
		errx(1, "truncated");

	// TODO: Check query bit.

	for ( uint16_t i = 0; i < hdr.qdcount; i++ )
	{
		char name[DNS_NAME_MAX + 1];
		offset = decode_dns_question(resp, offset, resp_size, name, &qs);
		//printf("%s type=%u class=%u\n", name, qs.qtype, qs.qclass);
	}

	for ( uint16_t i = 0; i < hdr.ancount; i++ )
	{
		char name[DNS_NAME_MAX + 1];
		struct dns_record rr;
		offset = decode_dns_record(resp, offset, resp_size, name, &rr);
		uint32_t ttl = (uint32_t) rr.ttl_high << 16 | rr.ttl_low;
		printf("%s type=%u class=%u ttl=%u ", name, rr.type, rr.class, ttl);
		if ( rr.class == DNS_CLASS_IN && rr.type == DNS_TYPE_A )
		{
			unsigned char ip[4];
			for ( size_t i = 0; i < 4; i++ )
				offset = decode_dns_byte(resp, offset, resp_size, &ip[i]);
			printf("%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
		}
		else if ( rr.class == DNS_CLASS_IN && rr.type == DNS_TYPE_AAAA )
		{
			unsigned char ip[16];
			for ( size_t i = 0; i < 16; i++ )
				offset = decode_dns_byte(resp, offset, resp_size, &ip[i]);
			for ( size_t i = 0; i < 16; i++ )
			{
				if ( i && !(i & 1) )
					putchar(':');
				printf("%02x", ip[i]);
			}
		}
		else if ( rr.type == DNS_TYPE_CNAME )
		{
			char cname[DNS_NAME_MAX + 1];
			offset = decode_dns_name(resp, offset, resp_size, cname);
			printf("CNAME %s", cname);
		}
		else
		{
			printf("0x");
			fflush(stdout);
			for ( size_t i = 0; i < rr.rdlength; i++ )
			{
				unsigned char b;
				offset = decode_dns_byte(resp, offset, resp_size, &b);
				if ( isprint(b) && b != '\'' )
					printf("'%c'", b);
				else
					printf("%02X", b);
				fflush(stdout);
			}
		}
		printf("\n");
	}

	return 0;
}
