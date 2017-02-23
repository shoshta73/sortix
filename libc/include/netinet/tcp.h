/*
 * Copyright (c) 2014, 2018 Jonas 'Sortie' Termansen.
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
 * netinet/tcp.h
 * Transmission Control Protocol.
 */

#ifndef INCLUDE_NETINET_TCP_H
#define INCLUDE_NETINET_TCP_H

#include <sys/cdefs.h>

#if __USE_SORTIX
#include <netinet/in.h>
#endif

#if __USE_SORTIX
typedef uint32_t tcp_seq; /* TCP sequence number. */

/* Control Bits in struct tcphdr th_flags. */
#define TH_FIN (1 << 0) /* No more data from sender. */
#define TH_SYN (1 << 1) /* Synchronize sequence numbers. */
#define TH_RST (1 << 2) /* Reset the connection. */
#define TH_PUSH (1 << 3) /* Push Function. */
#define TH_ACK (1 << 4) /* Acknowledgment field significant. */
#define TH_URG (1 << 5) /* Urgent Pointer field significant. */

struct tcphdr
{
	in_port_t th_sport; /* Source Port. */
	in_port_t th_dport; /* Destination Port. */
	tcp_seq th_seq; /* Sequence Number. */
	tcp_seq th_ack; /* Acknowledgment Number. */
	__extension__ union
	{
		__extension__ struct
		{
		#if __BYTE_ORDER == __LITTLE_ENDIAN
			uint8_t th_x2:4; /* Reserved. */
			uint8_t th_off:4; /* Data offset. */
		#elif __BYTE_ORDER == __BIG_ENDIAN
			uint8_t th_off:4; /* Data offset. */
			uint8_t th_x2:4; /* Reserved. */
		#else
			#warning "You need to add support for your endian"
		#endif
		};
		uint8_t th_offset;
	};
	uint8_t th_flags; /* Control Bits. */
	uint16_t th_win; /* Window. */
	uint16_t th_sum; /* Checksum. */
	uint16_t th_urp; /* Urgent Pointer. */
};

#define TCP_OFFSET_ENCODE(x) (((x) & 0xF) << 4) /* Encode th_offset. */
#define TCP_OFFSET_DECODE(x) (((x) >> 4) & 0xF) /* Decode th_offset. */

#define TCP_MSS 536 /* Default Maximum Segment Size. */

#define TCPOPT_EOL 0 /* End of Option List. */
#define TCPOPT_NOP 1 /* No-Operation. */

#define TCPOPT_MAXSEG 2 /* Maximum Segment Size. */
#define TCPOLEN_MAXSEG 4 /* Length of Maximum Segment Size. */

/* Maximum header size: 16 * 4 bytes */
#define TCP_MAXHLEN 64

/* Maximum total length of options. */
#define TCP_MAXOLEN (TCP_MAXHLEN - sizeof(struct tcphdr))

#define TCP_MAXWIN 65535 /* Maximum window size. */
#endif

/* Options at the IPPROTO_TCP socket level. */
#define TCP_NODELAY 1 /* TODO: Describe. */
#if __USE_SORTIX
#define TCP_MAXSEG 2 /* TODO: Describe. */
#define TCP_NOPUSH 3 /* TODO: Describe. */
#endif

#endif
