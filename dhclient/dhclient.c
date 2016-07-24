/*
 * Copyright (c) 2016, 2017 Jonas 'Sortie' Termansen.
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
 * dhclient.c
 * Dynamic Host Configuration Protocol client.
 */

#if defined(__sortix__)
#include <sys/dnsconfig.h>
#endif
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <assert.h>
#include <dirent.h>
#include <endian.h>
#include <err.h>
#include <fcntl.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <timespec.h>
#include <unistd.h>

#define PORT_DHCP_SERVER 67
#define PORT_DHCP_CLIENT 68

struct dhcp
{
	uint8_t op;
	uint8_t htype;
	uint8_t hlen;
	uint8_t hops;
	uint32_t xid;
	uint16_t secs;
	uint16_t flags;
	uint8_t ciaddr[4];
	uint8_t yiaddr[4];
	uint8_t siaddr[4];
	uint8_t giaddr[4];
	uint8_t chaddr[16];
	uint8_t sname[64];
	uint8_t file[128];
};

#define DHCP_OP_BOOTREQUEST 1
#define DHCP_OP_BOOTREPLY 2
#define DHCP_FLAGS_BROADCAST (1 << 15)
#define DHCP_HTYPE_ETHERNET 1
#define DHCP_HLEN_ETHERNET 6
#define DHCP_MAGIC_0 99
#define DHCP_MAGIC_1 130
#define DHCP_MAGIC_2 83
#define DHCP_MAGIC_3 99

#define OPTION_PAD 0
#define OPTION_SUBNET 1
#define OPTION_TIME_OFFSET 2
#define OPTION_ROUTERS 3
#define OPTION_DNS 6
#define OPTION_DOMAIN_NAME 12
#define OPTION_INTERFACE_MTU 26
#define OPTION_BROADCAST_ADDRESS 28
#define OPTION_NTP 42
#define OPTION_REQUESTED_IP 50
#define OPTION_LEASE_TIME 51
#define OPTION_OPTION_OVERLOAD 52
#define OPTION_DHCP_MSGTYPE 53
#define OPTION_SERVER_IDENTIFIER 54
#define OPTION_PARAMETER_REQUEST 55
#define OPTION_RENEWAL_TIME 58
#define OPTION_REBINDING_TIME 59
#define OPTION_END 255

#define DHCPDISCOVER 1
#define DHCPOFFER 2
#define DHCPREQUEST 3
#define DHCPDECLINE 4
#define DHCPACK 5
#define DHCPNAK 6
#define DHCPRELEASE 7
#define DHCPINFORM 9

struct dhcp_message
{
	struct dhcp hdr;
	unsigned char magic[4];
	unsigned char options[65536 - (sizeof(struct dhcp) + 4)];
};

enum option_state
{
	OPTION_STATE_OPTIONS,
	OPTION_STATE_FILE,
	OPTION_STATE_SNAME,
	OPTION_STATE_DONE,
};

struct option_iterate
{
	struct dhcp* hdr;
	unsigned char* options;
	size_t offset;
	size_t length;
	enum option_state state;
	bool has_sname_options;
	bool has_file_options;
};

static void option_iterate_begin(struct option_iterate* iter,
                                 struct dhcp* hdr,
                                 unsigned char* options,
                                 size_t length)
{
	memset(iter, 0, sizeof(*iter));
	iter->hdr = hdr;
	iter->options = options;
	iter->length = length;
}

static void option_iterate_begin_msg(struct option_iterate* iter,
                                     struct dhcp_message* msg,
                                     size_t length)
{
	size_t offset = offsetof(struct dhcp_message, options);
	assert(offset <= length);
	option_iterate_begin(iter, &msg->hdr, msg->options, length - offset);
}

static bool option_iterate_array(struct option_iterate* iter,
                                 unsigned char* options,
                                 size_t length,
                                 unsigned char* out_option,
                                 unsigned char* out_optlen,
                                 unsigned char** out_data)
{
	while ( iter->offset < length )
	{
		unsigned char option = options[iter->offset++];
		if ( option == OPTION_PAD )
			continue;
		if ( option == OPTION_END )
			break;
		if ( iter->offset == length )
			return false;
		unsigned char optlen = options[iter->offset++];
		if ( length - iter->offset < optlen )
			return false;
		unsigned char* data = iter->options + iter->offset;
		*out_option = option;
		*out_optlen = optlen;
		*out_data = data;
		iter->offset += optlen;
		if ( option == OPTION_OPTION_OVERLOAD )
		{
			if ( optlen != 1 )
				return false;
			if ( iter->state == OPTION_STATE_OPTIONS )
			{
				if ( data[0] & 1 << 0 )
					iter->has_sname_options = true;
				if ( data[0] & 1 << 1 )
					iter->has_file_options = true;
			}
			continue;
		}
		return true;
	}
	return false;

}

static bool option_iterate(struct option_iterate* iter,
                           unsigned char* out_option,
                           unsigned char* out_optlen,
                           unsigned char** out_data)
{
	if ( iter->state == OPTION_STATE_OPTIONS )
	{
		if ( option_iterate_array(iter, iter->options, iter->length,
		                          out_option, out_optlen, out_data) )
			return true;
		iter->state = OPTION_STATE_SNAME;
		iter->offset = 0;
	}
	if ( iter->state == OPTION_STATE_SNAME )
	{
		if ( iter->has_sname_options &&
		     option_iterate_array(iter, iter->hdr->sname,
		                          sizeof(iter->hdr->sname), out_option,
		                          out_optlen, out_data) )
			return true;
		iter->state = OPTION_STATE_FILE;
		iter->offset = 0;
	}
	if ( iter->state == OPTION_STATE_FILE )
	{
		if ( iter->has_file_options &&
		     option_iterate_array(iter, iter->hdr->file,
		                          sizeof(iter->hdr->file), out_option,
		                          out_optlen, out_data) )
			return true;
		iter->state = OPTION_STATE_DONE;
		iter->offset = 0;
	}
	return false;
}

static bool option_search(struct option_iterate* iter,
                          unsigned char search_option,
                          unsigned char* out_optlen,
                          unsigned char** out_data)
{
	enum option_state saved_state = iter->state;
	size_t saved_offset = iter->offset;
	iter->state = OPTION_STATE_OPTIONS;
	iter->offset = 0;
	bool result = false;
	unsigned char option;
	unsigned char optlen;
	unsigned char* data;
	while ( option_iterate(iter, &option, &optlen, &data) )
	{
		if ( option == search_option )
		{
			result = true;
			*out_optlen = optlen;
			*out_data = data;
			break;
		}
	}
	iter->state = saved_state;
	iter->offset = saved_offset;
	return result;
}

static const unsigned char requests[] =
{
	OPTION_SUBNET,
	OPTION_TIME_OFFSET,
	OPTION_ROUTERS,
	OPTION_DNS,
	OPTION_DOMAIN_NAME,
	OPTION_INTERFACE_MTU,
	OPTION_NTP,
};

static size_t add_option_byte(unsigned char* options,
                              size_t optsmax,
                              size_t offset,
                              unsigned char byte)
{
	if ( optsmax <= offset )
		errx(1, "too many dhcp options");
	options[offset++] = byte;
	return offset;
}

static size_t add_option(unsigned char* options,
                         size_t optsmax,
                         size_t offset,
                         unsigned char option,
                         unsigned char optlen,
                         const unsigned char* data)
{
	offset = add_option_byte(options, optsmax, offset, option);
	offset = add_option_byte(options, optsmax, offset, optlen);
	for ( size_t i = 0; i < optlen; i++ )
		offset = add_option_byte(options, optsmax, offset, data[i]);
	return offset;
}

static bool check_dchp_message(struct dhcp_message* msg,
                               size_t amount,
                               unsigned char* chaddr,
                               uint32_t xid)
{
	if ( (size_t) amount < sizeof(msg->hdr) )
		return false;
	if ( (size_t) amount < sizeof(msg->hdr) + sizeof(msg->magic) )
		return false;
	if ( msg->hdr.op != DHCP_OP_BOOTREPLY )
		return false;
	if ( msg->hdr.htype != DHCP_HTYPE_ETHERNET ||
	     msg->hdr.hlen != DHCP_HLEN_ETHERNET )
		return false;
	if ( memcmp(msg->hdr.chaddr, chaddr, sizeof(msg->hdr.chaddr)) != 0 )
		return false;
	if ( msg->hdr.xid != htobe32(xid) )
		return false;
	if ( msg->magic[0] != DHCP_MAGIC_0 ||
	     msg->magic[1] != DHCP_MAGIC_1 ||
	     msg->magic[2] != DHCP_MAGIC_2 ||
	     msg->magic[3] != DHCP_MAGIC_3 )
		return false;
	return true;
}

static void ready(void)
{
	const char* readyfd_env = getenv("READYFD");
	if ( !readyfd_env )
		return;
	int readyfd = atoi(readyfd_env);
	char c = '\n';
	write(readyfd, &c, 1);
	close(readyfd);
	unsetenv("READYFD");
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
	setvbuf(stdout, NULL, _IOLBF, 0);

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

#if defined(__sortix__)
	if ( argc <= 1 )
	{
		printf("Usage: %s <interface>\n", argv[0]);
		return 0;
	}
	const char* if_name = argv[1];
	if ( 3 <= argc )
		errx(1, "unexpected extra operand `%s'", argv[2]);

	int dev_fd = open("/dev", O_RDONLY | O_DIRECTORY);
	if ( dev_fd < 0 )
		err(1, "/dev");
	int if_fd = openat(dev_fd, if_name, O_RDWR);
	if ( if_fd < 0 )
		err(1, "%s", if_name);
	close(dev_fd);

	int type = ioctl(if_fd, IOCGETTYPE);
	if ( type < 0 )
		err(1, "%s: ioctl: IOCGETTYPE", if_name);
	if ( IOC_TYPE(type) != IOC_TYPE_NETWORK_INTERFACE )
		errx(1, "%s: Not a network interface", if_name);

	struct if_info info;
	if ( ioctl(if_fd, NIOC_GETINFO, &info) < 0 )
		err(1, "%s: ioctl: NIOC_GETINFO", if_name);
	if ( info.type == IF_TYPE_LOOPBACK )
	{
		printf("%s: Loopback interface does not need to be configured\n",
		       if_name);
		return 0;
	}
	if ( info.type != IF_TYPE_ETHERNET )
		errx(1, "%s: ioctl: NIOC_GETINFO: unknown device type", if_name);
	if ( info.addrlen != 6 )
		errx(1, "%s: ioctl: NIOC_GETINFO: bogus address length", if_name);
	// TODO: struct ether_addr
	unsigned char ethaddr[6];
	memcpy(ethaddr, info.addr, 6);

#else
	unsigned char ethaddr[6] = { 00, 25, 22, 04, 95, 83 };
#endif

	int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if ( fd < 0 )
		err(1, "socket");

#if defined(__sortix__)
	if ( setsockopt(fd, SOL_SOCKET, SO_BINDTOINDEX, &info.linkid,
	                sizeof(info.linkid)) < 0 )
		err(1, "setsockopt: SO_BINDTOINDEX");
#endif

	int enable = 1;
	if ( setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable)) < 0 )
		err(1, "setsockopt: SO_BROADCAST");

	struct sockaddr_in local;
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_port = htobe16(PORT_DHCP_CLIENT);
	local.sin_addr.s_addr = htobe32(INADDR_ANY);

	if ( bind(fd, (const struct sockaddr*) &local, sizeof(local)) < 0 )
		err(1, "bind");

	struct sockaddr_in dest;
	memset(&dest, 0, sizeof(dest));
	dest.sin_family = AF_INET;
	dest.sin_port = htobe16(PORT_DHCP_SERVER);
	dest.sin_addr.s_addr = htobe32(INADDR_BROADCAST);

	struct sockaddr_in remote;
	socklen_t remote_len = sizeof(remote);

#if defined(__sortix__)
	printf("%s: Waiting for interface to come up\n", if_name);
	// TODO: This can block indefinitely. Run ready() if this step times out.
	if ( ioctl(if_fd, NIOC_WAITLINKSTATUS, 1) < 0 )
		err(1, "%s: ioctl: NIOC_WAITLINKSTATUS", if_name);
	printf("%s: Interface is up\n", if_name);
#endif

	// TODO: "The client SHOULD wait a random time between one and ten seconds
	//        to desynchronize the use of DHCP at startup."
	//        Ten seconds seems excessive in today's world, but a small random
	//        delay seems reasonable. There are randomness in retransmissions so
	//        maybe this doesn't benefit so much.

#if defined(__sortix__)
	uint32_t xid = arc4random();
#else
	uint32_t xid = rand();
#endif

	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	unsigned int retransmissions = 0;

	unsigned char chaddr[16];
	memset(chaddr, 0, sizeof(chaddr));
	memcpy(chaddr, ethaddr, 6);

	static struct dhcp_message msg;
	unsigned char* opts = msg.options;
	size_t msgsize;
	size_t optsoff;
	size_t optsmax = sizeof(msg.options);
	unsigned char option;
	unsigned char optlen = 0;
	unsigned char* optdata = NULL;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	struct timespec begun = now;
	struct timespec last_sent;
	struct timespec timeout;

	last_sent = timespec_make(-1, 0);
	timeout = timespec_make(0, 0);

	unsigned char yiaddr[4];
	unsigned char server_identifier[4];
	char remote_host_str[NI_MAXHOST];
	char remote_serv_str[NI_MAXSERV];
	char yiaddr_str[INET_ADDRSTRLEN];
	while ( true )
	{
		clock_gettime(CLOCK_MONOTONIC, &now);
		struct timespec since_sent = timespec_sub(now, last_sent);
		if ( timespec_ge(since_sent, timeout) )
		{
			struct timespec since_begun = timespec_sub(now, begun);

			memset(&msg, 0, sizeof(msg));
			msg.hdr.op = DHCP_OP_BOOTREQUEST;
			msg.hdr.htype = DHCP_HTYPE_ETHERNET;
			msg.hdr.hlen = DHCP_HLEN_ETHERNET;
			msg.hdr.xid = htobe32(xid);
			msg.hdr.secs = htobe16((uint16_t) since_begun.tv_sec);
			msg.hdr.flags = htobe16(DHCP_FLAGS_BROADCAST);
			memcpy(msg.hdr.chaddr, chaddr, sizeof(chaddr));
			msg.magic[0] = DHCP_MAGIC_0;
			msg.magic[1] = DHCP_MAGIC_1;
			msg.magic[2] = DHCP_MAGIC_2;
			msg.magic[3] = DHCP_MAGIC_3;

			optsoff = 0;
			option = DHCPDISCOVER;
			optsoff = add_option(opts, optsmax, optsoff, OPTION_DHCP_MSGTYPE,
					             1, &option);
			optsoff = add_option(opts, optsmax, optsoff,
			                     OPTION_PARAMETER_REQUEST, sizeof(requests),
			                     requests);
			// TODO: Maybe send hostname.
			optsoff = add_option_byte(opts, optsmax, optsoff, OPTION_END);

			if ( retransmissions == 0 )
				printf("%s: Broadcasting DHCPDISCOVER\n", if_name);
			else
				printf("%s: Broadcasting DHCPDISCOVER (attempt %i)\n", if_name,
			           retransmissions + 1);

			msgsize = sizeof(msg.hdr) + sizeof(msg.magic) + optsoff;
			// TODO: This can fail temporarily like with ENOBUFS.
			if ( sendto(fd, &msg, msgsize, 0, (const struct sockaddr*) &dest,
			            sizeof(dest)) < 0 )
				err(1, "send");

			last_sent = now;
			timeout = timespec_make(1 << retransmissions,
			                        arc4random_uniform(1000000000));
			if ( retransmissions < 6 )
				retransmissions++;
			else
			{
				fprintf(stderr, "%s: error: DHCPDISCOVER timed out\n", if_name);
				ready();
				// TODO: Technically restart the initialization.
				retransmissions = 0;
			}
		}
		struct timespec left =
			timespec_sub(timespec_add(last_sent, timeout), now);
		struct pollfd pfd = { 0 };
		pfd.fd = fd;
		pfd.events = POLLIN;
		int num_events = ppoll(&pfd, 1, &left, NULL);
		if ( num_events < 0 )
			err(1, "ppoll");
		if ( num_events == 0 )
			continue;
		ssize_t amount = recvfrom(fd, &msg, sizeof(msg), 0,
		                          (struct sockaddr*) &remote, &remote_len);
		if ( amount < 0 )
			err(1, "recv");
		// TODO: Check the remote port is correct?
		if ( !check_dchp_message(&msg, amount, chaddr, xid) )
			continue;
		struct option_iterate iter;
		option_iterate_begin_msg(&iter, &msg, amount);
		if ( !option_search(&iter, OPTION_DHCP_MSGTYPE, &optlen, &optdata) ||
		     optlen != 1 ||
		     optdata[0] != DHCPOFFER )
			continue;
		if ( !option_search(&iter, OPTION_SERVER_IDENTIFIER, &optlen,
		                    &optdata) || optlen != 4 )
			continue;
		memcpy(server_identifier, optdata, 4);
		memcpy(yiaddr, msg.hdr.yiaddr, 4);
		getnameinfo((const struct sockaddr*) &remote, remote_len,
		            remote_host_str, sizeof(remote_host_str),
		            remote_serv_str, sizeof(remote_serv_str),
		            NI_NUMERICHOST | NI_NUMERICSERV);
		inet_ntop(AF_INET, yiaddr, yiaddr_str, sizeof(yiaddr_str));
		printf("%s: DHCPOFFER of %s from %s:%s\n",
		       if_name, yiaddr_str, remote_host_str, remote_serv_str);
		break;
	}

	last_sent = timespec_make(-1, 0);
	timeout = timespec_make(0, 0);
	retransmissions = 0;

	unsigned char subnet[4];
	unsigned char router[4];
#if defined(__sortix__)
	size_t dns_count = 0;
	unsigned char dns[DNSCONFIG_MAX_SERVERS][4];
#endif
	uint32_t lease_time = 0;
	while ( true )
	{
		clock_gettime(CLOCK_MONOTONIC, &now);
		struct timespec since_sent = timespec_sub(now, last_sent);
		if ( timespec_le(timeout, since_sent) )
		{
			struct timespec since_begun = timespec_sub(now, begun);

			memset(&msg, 0, sizeof(msg));
			msg.hdr.op = DHCP_OP_BOOTREQUEST;
			msg.hdr.htype = DHCP_HTYPE_ETHERNET;
			msg.hdr.hlen = DHCP_HLEN_ETHERNET;
			msg.hdr.xid = htobe32(xid);
			msg.hdr.secs = htobe16((uint16_t) since_begun.tv_sec);
			msg.hdr.flags = htobe16(DHCP_FLAGS_BROADCAST);
			memcpy(msg.hdr.chaddr, chaddr, sizeof(chaddr));
			msg.magic[0] = DHCP_MAGIC_0;
			msg.magic[1] = DHCP_MAGIC_1;
			msg.magic[2] = DHCP_MAGIC_2;
			msg.magic[3] = DHCP_MAGIC_3;

			optsoff = 0;
			option = DHCPREQUEST;
			optsoff = add_option(opts, optsmax, optsoff, OPTION_DHCP_MSGTYPE,
			                     1, &option);
			optsoff = add_option(opts, optsmax, optsoff,
			                     OPTION_PARAMETER_REQUEST, sizeof(requests),
			                     requests);
			optsoff = add_option(opts, optsmax, optsoff,
			                     OPTION_SERVER_IDENTIFIER,
			                     sizeof(server_identifier), server_identifier);
			optsoff = add_option(opts, optsmax, optsoff, OPTION_REQUESTED_IP,
					             sizeof(yiaddr), yiaddr);
			// TODO: Maybe send hostname.
			optsoff = add_option_byte(opts, optsmax, optsoff, OPTION_END);

			if ( retransmissions == 0 )
				printf("%s: Broadcasting DHCPREQUEST\n", if_name);
			else
				printf("%s: Broadcasting DHCPREQUEST (attempt %i)\n", if_name,
				       retransmissions + 1);

			msgsize = sizeof(msg.hdr) + sizeof(msg.magic) + optsoff;
			if ( sendto(fd, &msg, msgsize, 0, (const struct sockaddr*) &dest,
			            sizeof(dest)) < 0 )
				err(1, "send");

			last_sent = now;
			timeout = timespec_make(1 << retransmissions,
			                        arc4random_uniform(1000000000));
			if ( retransmissions < 6 )
				retransmissions++;
			else
			{
				fprintf(stderr, "%s: error: DHCPDISCOVER timed out\n", if_name);
				ready();
				// TODO: Technically restart the initialization.
				retransmissions = 0;
			}
		}
		struct timespec left =
			timespec_sub(timespec_add(last_sent, timeout), now);
		struct pollfd pfd = { 0 };
		pfd.fd = fd;
		pfd.events = POLLIN;
		int num_events = ppoll(&pfd, 1, &left, NULL);
		if ( num_events < 0 )
			err(1, "ppoll");
		if ( num_events == 0 )
			continue;
		struct sockaddr_in peer;
		socklen_t peer_len = sizeof(peer);
		ssize_t amount = recvfrom(fd, &msg, sizeof(msg), 0,
		                          (struct sockaddr*) &peer, &peer_len);
		if ( amount < 0 )
			err(1, "recv");
		if ( memcmp(&peer, &remote, remote_len) != 0 )
			continue;
		if ( !check_dchp_message(&msg, amount, chaddr, xid) )
			continue;
		struct option_iterate iter;
		option_iterate_begin_msg(&iter, &msg, amount);
		// TODO: Log proper errors for the below conditions.
		if ( !option_search(&iter, OPTION_DHCP_MSGTYPE, &optlen, &optdata) ||
		     optlen != 1 ||
		     optdata[0] != DHCPACK )
			continue;
		if ( !option_search(&iter, OPTION_SERVER_IDENTIFIER, &optlen,
		                    &optdata) || optlen != 4 )
			continue;
		memcpy(server_identifier, optdata, 4);
		if ( !option_search(&iter, OPTION_SUBNET, &optlen, &optdata) ||
		     optlen != 4 )
			continue;
		memcpy(subnet, optdata, 4);
		if ( !option_search(&iter, OPTION_ROUTERS, &optlen, &optdata) ||
		     optlen < 4 )
			continue;
		memcpy(router, optdata, 4);
		if ( !option_search(&iter, OPTION_LEASE_TIME, &optlen, &optdata) ||
		     optlen != 4 )
			continue;
		lease_time = (uint32_t) optdata[0] << 24 |
		             (uint32_t) optdata[1] << 16 |
		             (uint32_t) optdata[2] << 8 |
		             (uint32_t) optdata[3] << 0;
		// TODO: Verify yiaddr is what we requested (bait and switch).
		memcpy(yiaddr, msg.hdr.yiaddr, 4);
#if defined(__sortix__)
		if ( option_search(&iter, OPTION_DNS, &optlen, &optdata) )
		{
			size_t offset = 0;
			for ( ; dns_count < DNSCONFIG_MAX_SERVERS && 4 <= optlen - offset;
			        dns_count++ )
			{
				dns[dns_count][0] = optdata[offset++];
				dns[dns_count][1] = optdata[offset++];
				dns[dns_count][2] = optdata[offset++];
				dns[dns_count][3] = optdata[offset++];
			}
		}
#endif
		printf("%s: DHCPACK of %s from %s:%s\n",
		       if_name, yiaddr_str, remote_host_str, remote_serv_str);
		break;
	}

	char router_str[INET_ADDRSTRLEN];
	char subnet_str[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, router, router_str, sizeof(router_str));
	inet_ntop(AF_INET, subnet, subnet_str, sizeof(subnet_str));
	printf("%s: Leased %s from %s:%s for %u seconds\n",
	       if_name,
	       yiaddr_str,
	       remote_host_str,
	       remote_serv_str,
	       lease_time);
	printf("%s: Router is %s\n", if_name, router_str);
	printf("%s: Subnet is %s\n", if_name, subnet_str);
	if ( dns_count == 0 )
		printf("%s: No DNS servers were offered\n", if_name);
	else for ( size_t i = 0; i < dns_count; i++ )
	{
		char dns_str[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, dns[i], dns_str, sizeof(dns_str));
		printf("%s: DNS server %zu is %s\n", if_name, i + 1, dns_str);
	}

#if defined(__sortix__)
	struct if_config_inet inet_cfg;
	memcpy(&inet_cfg.address, yiaddr, sizeof(inet_cfg.address));
	memcpy(&inet_cfg.router, router, sizeof(inet_cfg.router));
	memcpy(&inet_cfg.subnet, subnet, sizeof(inet_cfg.subnet));
	if ( ioctl(if_fd, NIOC_SETCONFIG_INET, &inet_cfg) < 0 )
		err(1, "%s: ioctl: NIOC_SETCONFIG_INET", if_name);
	// TODO: This configuration should set a bit that marks the interface as
	//       configured (and not in the configuration phase) and wake anything
	//       waiting for the interface to come up.
	printf("%s: Configured network interface\n", if_name);

	struct dnsconfig dnsconfig;
	memset(&dnsconfig, 0, sizeof(dnsconfig));
	dnsconfig.servers_count = dns_count;
	for ( size_t i = 0; i < dns_count; i++ )
	{
		dnsconfig.servers[i].family = AF_INET;
		dnsconfig.servers[i].addrsize = 4;
		memcpy(&dnsconfig.servers[i].addr, dns[i], 4);
	}
	if ( setdnsconfig(&dnsconfig) < 0 )
		err(1, "setdnsconfig");
	printf("%s: Configured DNS\n", if_name);
#endif

	ready();

	while ( true )
	{
		sleep(lease_time);
		printf("%s: Lease for %s has expired after %u seconds\n",
		       if_name, yiaddr_str, lease_time);
		// TODO: Attempt to renew lease and expire it and such.
		fprintf(stderr, "error: Lease renewal is not implemented");
	}

	close(fd);
#if defined(__sortix__)
	close(if_fd);
#endif
}
