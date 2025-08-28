/*
 * Copyright (c) 2016, 2017, 2023 Jonas 'Sortie' Termansen.
 * Copyright (c) 2021, 2022, 2023 Juhani 'nortti' Krekelä.
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

#include <sys/dnsconfig.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <endian.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/if_ether.h>
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
	uint8_t magic[4];
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
	unsigned char options[65536 - (sizeof(struct dhcp))];
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

struct interface
{
	char name[IF_NAMESIZE];
	int if_fd;
	int sock_fd;
	struct ether_addr hwaddr;
	unsigned int linkid;
};

struct request
{
	unsigned char requests[255];
	unsigned char requests_len;
	uint32_t xid;
	struct timespec begun;
	struct timespec since_startup;
	struct sockaddr_in remote;
	socklen_t remote_len;
	unsigned char server_identifier[4];
	unsigned char yiaddr[4];
	char remote_host_str[NI_MAXHOST];
	char remote_serv_str[NI_MAXSERV];
	char yiaddr_str[INET_ADDRSTRLEN];
};

struct lease
{
	struct in_addr server;
	struct in_addr address;
	struct in_addr subnet;
	struct in_addr router;
	size_t dns_count;
	unsigned char dns[DNSCONFIG_MAX_SERVERS][4];
	uint32_t lease_time;
	struct timespec t1;
	struct timespec t2;
	struct timespec expiration;
	bool leased;
};

enum config_method { AUTO, MANUAL, NONE };

struct config_dns_servers
{
	enum config_method method;
	struct dnsconfig dnsconfig;
};

struct config_dns
{
	struct config_dns_servers servers;
};

struct config_ether_address
{
	enum config_method method;
	struct ether_addr addr;
};

struct config_ether
{
	struct config_ether_address address;
};

struct config_inet_address
{
	enum config_method method;
	struct in_addr addr;
};

struct config_inet
{
	struct config_inet_address address;
	struct config_inet_address router;
	struct config_inet_address subnet;
};

struct config
{
	struct config_dns dns;
	struct config_ether ether;
	struct config_inet inet;
};

struct config_file
{
	const char* path;
	FILE* fp;
	bool shared;
	char* line;
	size_t line_size;
	off_t line_number;
	char* token;
	char* token_start;
	char* token_saved;
};

static bool dns_servers_parse(void* ptr, const char* value)
{
	struct config_dns_servers* config = ptr;
	if ( !strcmp(value, "none") )
		config->method = NONE;
	else if ( !strcmp(value, "auto") )
		config->method = AUTO;
	else
	{
		config->method = MANUAL;
		config->dnsconfig.servers_count = 0;
		while ( value[0] )
		{
			if ( value[0] == ',' )
			{
				value++;
				continue;
			}
			char addr[INET6_ADDRSTRLEN];
			size_t length = strcspn(value, ",");
			if ( sizeof(addr) <= length )
				return false;
			memcpy(addr, value, length);
			addr[length] = '\0';
			value += length;
			struct dnsconfig_server server = {0};
			if ( inet_pton(AF_INET, addr, &server.addr.in) )
			{
				server.family = AF_INET;
				server.addrsize = sizeof(server.addr.in);
			}
			else if ( inet_pton(AF_INET6, addr, &server.addr.in6) )
			{
				server.family = AF_INET6;
				server.addrsize = sizeof(server.addr.in6);
			}
			else
				return false;
			size_t index = config->dnsconfig.servers_count++;
			if ( DNSCONFIG_MAX_SERVERS < config->dnsconfig.servers_count )
				return false;
			config->dnsconfig.servers[index] = server;
			if ( value[0] )
			{
				if ( value[0] != ',' )
					return false;
				value++;
			}
		}
	}
	return true;
}

static bool mac_parse(struct ether_addr* addr, const char* string)
{
	for ( size_t i = 0; i < 6; i++ )
	{
		int upper;
		if ( '0' <= string[i*3 + 0] && string[i*3 + 0] <= '9' )
			upper = string[i*3 + 0] - '0';
		else if ( 'a' <= string[i*3 + 0] && string[i*3 + 0] <= 'f' )
			upper = string[i*3 + 0] - 'a' + 10;
		else if ( 'A' <= string[i*3 + 0] && string[i*3 + 0] <= 'F' )
			upper = string[i*3 + 0] - 'A' + 10;
		else
			return false;
		int lower;
		if ( '0' <= string[i*3 + 1] && string[i*3 + 1] <= '9' )
			lower = string[i*3 + 1] - '0';
		else if ( 'a' <= string[i*3 + 1] && string[i*3 + 1] <= 'f' )
			lower = string[i*3 + 1] - 'a' + 10;
		else if ( 'A' <= string[i*3 + 1] && string[i*3 + 1] <= 'F' )
			lower = string[i*3 + 1] - 'A' + 10;
		else
			return false;
		if ( string[i*3 + 2] != (i + 1 != 6 ? ':' : '\0') )
			return false;
		addr->ether_addr_octet[i] = upper << 4 | lower;
	}
	return true;
}

static bool ether_address_parse(void* ptr, const char* value)
{
	struct config_ether_address* config = ptr;
	if ( !strcmp(value, "auto") )
		config->method = AUTO;
	else if ( !strcmp(value, "none") )
		config->method = NONE;
	else
	{
		if ( !mac_parse(&config->addr, value) )
			return false;
		config->method = MANUAL;
	}
	return true;
}

static bool inet_address_parse(void* ptr, const char* value)
{
	struct config_inet_address* config = ptr;
	if ( !strcmp(value, "auto") )
		config->method = AUTO;
	else if ( !strcmp(value, "none") )
		config->method = NONE;
	else
	{
		if ( inet_pton(AF_INET, value, &config->addr) != 1 )
			return false;
		config->method = MANUAL;
	}
	return true;
}

#define ARRAY_LENGTH(array) (sizeof(array) / sizeof((array)[0]))

struct configuration
{
	const char* name;
	size_t offset;
	bool (*parse)(void*, const char*);
};

#define CONFIGOFFSET(protocol, parameter) \
	(offsetof(struct config, protocol) + \
	 offsetof(struct config_##protocol, parameter))

struct configuration dns_configurations[] =
{
	{ "servers", CONFIGOFFSET(dns, servers), dns_servers_parse},
};

struct configuration ether_configurations[] =
{
	{ "address", CONFIGOFFSET(ether, address), ether_address_parse},
};

struct configuration inet_configurations[] =
{
	{ "address", CONFIGOFFSET(inet, address), inet_address_parse },
	{ "router", CONFIGOFFSET(inet, router), inet_address_parse },
	{ "subnet", CONFIGOFFSET(inet, subnet), inet_address_parse },
};

struct protocol
{
	const char* name;
	struct configuration* configurations;
	size_t configurations_count;
};

struct protocol protocols[] =
{
	{ "dns", dns_configurations, ARRAY_LENGTH(dns_configurations) },
	{ "ether", ether_configurations, ARRAY_LENGTH(ether_configurations) },
	{ "inet", inet_configurations, ARRAY_LENGTH(inet_configurations) },
};

static const struct protocol* protocol_lookup(const char* name)
{
	for ( size_t i = 0; i < ARRAY_LENGTH(protocols); i++ )
		if ( !strcmp(protocols[i].name, name) )
			return &protocols[i];
	return NULL;
}

static const struct configuration* configuration_lookup(
    const struct protocol* protocol,
    const char* name)
{
	for ( size_t i = 0; i < protocol->configurations_count; i++ )
	{
		if ( !strcmp(protocol->configurations[i].name, name) )
			return &protocol->configurations[i];
	}
	return NULL;
}

static bool config_file_read_line(struct config_file* config_file)
{
	errno = 0;
	ssize_t length = getline(&config_file->line, &config_file->line_size,
	                          config_file->fp);
	if ( length < 0 )
	{
		if ( errno )
			err(1, "%s", config_file->path);
		free(config_file->line);
		return false;
	}
	config_file->line_number++;

	// Remove leading whitespace.
	char* line = config_file->line;
	size_t start = 0;
	while ( isspace((unsigned char) line[start]) )
		start++;
	length -= start;
	memmove(line, line + start, length);
	line[length] = '\0';

	// Remove comments.
	length = strcspn(line, "#");
	line[length] = '\0';

	// Remove trailing whitespace.
	while ( length && isspace((unsigned char) line[length - 1]) )
		length--;
	line[length] = '\0';

	return true;
}

static char* config_file_read_token(struct config_file* config_file)
{
	while ( true )
	{
		if ( !config_file->line )
		{
			if ( !config_file_read_line(config_file) )
				return NULL;
			config_file->token_start = config_file->line;
		}

		if ( (config_file->token = strtok_r(config_file->token_start,
		                                    " \t\n\v\f\r",
		                                    &config_file->token_saved)) )
		{
			config_file->token_start = NULL;
			return config_file->token;
		}

		config_file->token_start = NULL;
		config_file->token_saved = NULL;
		free(config_file->line);
		config_file->line = NULL;
	}
}

static char* config_file_read_parameter(struct config_file* config_file,
                                        const char* option)
{
	char* option_copy = strdup(option);
	if ( !option_copy )
		err(1, "malloc");
	char* token = config_file_read_token(config_file);
	if ( !token )
		errx(1, "%s:%ji: error: %s expects a parameter", config_file->path,
		     (intmax_t) config_file->line_number, option_copy);
	free(option_copy);
	return token;
}

static bool match_interface(const struct interface* interface,
                            const char* specifier,
                            struct config_file* config_file)
{
	if ( !strchr(specifier, ':') )
		return strcmp(specifier, interface->name);
	else if ( !strncmp(specifier, "etherhw:", strlen("etherhw:")) )
	{
		struct ether_addr ether_hwaddr;
		const char *addr_string = specifier + strlen("etherhw:");
		if ( !mac_parse(&ether_hwaddr, addr_string) )
			errx(1, "%s:%ji: Invalid ethernet address: %s",
			     config_file->path, (intmax_t) config_file->line_number,
			     addr_string);
		return !memcmp(&ether_hwaddr, &interface->hwaddr, sizeof(ether_hwaddr));
	}
	else if ( !strncmp(specifier, "id:", strlen("id:")) )
	{
		const char* id_string = specifier + strlen("id:");
		char* end;
		errno = 0;
		unsigned long ulong = strtoul(id_string, &end, 10);
		if ( errno || !*id_string || *end || ulong > UINT_MAX )
			errx(1, "%s:%ji: Invalid interface id: %s",
			     config_file->path, (intmax_t) config_file->line_number,
			     id_string);
		return ulong == interface->linkid;
	}
	errx(1, "%s:%ji: Invalid interface specifier: %s", config_file->path,
	     (intmax_t) config_file->line_number, specifier);
}

static void config_file_load(const struct interface* interface,
                             struct config* config,
                             struct config_file* config_file)
{
	bool relevant = true;
	config_file->line_number = 0;
	const struct protocol* protocol = NULL;
	const struct protocol* found_protocol = NULL;
	const struct configuration* configuration = NULL;
	char* option;
	while ( (option = config_file_read_token(config_file)) )
	{
		if ( !strcmp(option, "if") )
		{
			if ( !config_file->shared )
				errx(1, "%s:%ji: `if` not valid in interface-specific config",
				     config_file->path, (intmax_t) config_file->line_number);
			char* value = config_file_read_parameter(config_file, option);
			relevant = match_interface(interface, value, config_file) ||
			           !interface->name[0] /* testing */;
		}
		else if ( (found_protocol = protocol_lookup(option)) )
			protocol = found_protocol;
		else if ( protocol &&
		          (!strcmp(option, "none") || !strcmp(option, "auto")) )
		{
			for ( size_t i = 0; i < protocol->configurations_count; i++ )
				if ( relevant && (configuration = protocol->configurations+i) )
					configuration->parse((char*) config + configuration->offset,
				                         option);
		}
		else if ( protocol &&
		          (configuration = configuration_lookup(protocol, option)) )
		{
			const char* value = config_file_read_parameter(config_file, option);
			if ( relevant &&
			     !configuration->parse((char*) config + configuration->offset,
			                           value) )
				errx(1, "%s:%ji: Invalid configuration value: %s %s: %s",
				     config_file->path, (intmax_t) config_file->line_number,
				     protocol->name, configuration->name, value);
		}
		else if ( protocol )
			errx(1, "%s:%ji: Unknown %s configuration or protocol: %s",
			     config_file->path, (intmax_t) config_file->line_number,
		         protocol->name, option);
		else
			errx(1, "%s:%ji: Unknown protocol: %s", config_file->path,
			     (intmax_t) config_file->line_number, option);
	}
}

static bool config_file_load_path(const struct interface* interface,
                                 struct config* config,
                                 const char* path,
                                 bool shared)
{
	struct config_file config_file = {0};
	config_file.path = path;
	config_file.shared = shared;
	if ( !(config_file.fp = fopen(path, "r")) )
	{
		if ( errno == ENOENT )
			return false;
		err(1, "%s", path);
	}
	config_file_load(interface, config, &config_file);
	fclose(config_file.fp);
	return true;
}

static void load_config(const struct interface* interface,
                        struct config* config,
                        const char* override_path)
{
	memset(config, 0, sizeof(struct config));

	if ( override_path )
	{
		if ( !config_file_load_path(interface, config, override_path, true) )
			err(1, "%s", override_path);
		return;
	}

	char* paths[3];
	if ( asprintf(&paths[0], "/etc/dhclient.%02x:%02x:%02x:%02x:%02x:%02x.conf",
	              interface->hwaddr.ether_addr_octet[0],
	              interface->hwaddr.ether_addr_octet[1],
	              interface->hwaddr.ether_addr_octet[2],
	              interface->hwaddr.ether_addr_octet[3],
	              interface->hwaddr.ether_addr_octet[4],
	              interface->hwaddr.ether_addr_octet[5]) < 0 )
		err(1, "malloc");
	if ( asprintf(&paths[1], "/etc/dhclient.%s.conf", interface->name) < 0 )
		err(1, "malloc");
	if ( asprintf(&paths[2], "/etc/dhclient.conf") < 0 )
		err(1, "malloc");

	bool loaded = false;
	for ( size_t i = 0; !loaded && i < ARRAY_LENGTH(paths); i++ )
	{
		bool shared = i == ARRAY_LENGTH(paths) - 1;
		loaded = config_file_load_path(interface, config, paths[i], shared);
	}

	for ( size_t i = 0; i < ARRAY_LENGTH(paths); i++ )
		free(paths[i]);
}

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
		unsigned char* data = options + iter->offset;
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

static bool option_search(struct option_iterate* input_iter,
                          unsigned char search_option,
                          unsigned char* out_optlen,
                          unsigned char** out_data)
{
	struct option_iterate iter = *input_iter;
	bool result = false;
	unsigned char option;
	unsigned char optlen;
	unsigned char* data;
	while ( option_iterate(&iter, &option, &optlen, &data) )
	{
		if ( option == search_option )
		{
			result = true;
			*out_optlen = optlen;
			*out_data = data;
			break;
		}
	}
	return result;
}

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

static bool send_dhcpdiscover(const struct interface* interface,
                              const struct request* request,
                              struct sockaddr_in dest)
{
	struct dhcp_message msg = {0};

	msg.hdr.op = DHCP_OP_BOOTREQUEST;
	msg.hdr.htype = DHCP_HTYPE_ETHERNET;
	msg.hdr.hlen = DHCP_HLEN_ETHERNET;
	msg.hdr.xid = htobe32(request->xid);
	msg.hdr.secs = htobe16((uint16_t) request->since_startup.tv_sec);
	msg.hdr.flags = htobe16(DHCP_FLAGS_BROADCAST);
	memset(msg.hdr.chaddr, 0, sizeof(msg.hdr.chaddr));
	memcpy(msg.hdr.chaddr, &interface->hwaddr, sizeof(interface->hwaddr));
	msg.hdr.magic[0] = DHCP_MAGIC_0;
	msg.hdr.magic[1] = DHCP_MAGIC_1;
	msg.hdr.magic[2] = DHCP_MAGIC_2;
	msg.hdr.magic[3] = DHCP_MAGIC_3;

	const size_t optsmax = sizeof(msg.options);
	size_t optsoff = 0;
	const unsigned char msgtype = DHCPDISCOVER;
	optsoff = add_option(msg.options, optsmax, optsoff, OPTION_DHCP_MSGTYPE, 1,
	                     &msgtype);
	if ( request->requests_len )
		optsoff = add_option(msg.options, optsmax, optsoff,
		                     OPTION_PARAMETER_REQUEST, request->requests_len,
		                     request->requests);
	optsoff = add_option_byte(msg.options, optsmax, optsoff, OPTION_END);

	const size_t msgsize = sizeof(msg.hdr) + optsoff;

	if ( sendto(interface->sock_fd, &msg, msgsize, 0,
	            (const struct sockaddr*) &dest, sizeof(dest)) < 0)
	{
		warn("send");
		// Drop packets and retry on transient errors and otherwise consider the
		// send attempt permanently failed for now.
		return errno == EAGAIN || errno == EWOULDBLOCK ||
		       errno == ENOMEM || errno == ENOBUFS;
	}

	return true;
}

static bool send_dhcprequest(const struct interface* interface,
                             const struct request* request,
                             struct sockaddr_in dest,
                             struct in_addr client_address)
{
	struct dhcp_message msg = {0};

	msg.hdr.op = DHCP_OP_BOOTREQUEST;
	msg.hdr.htype = DHCP_HTYPE_ETHERNET;
	msg.hdr.hlen = DHCP_HLEN_ETHERNET;
	msg.hdr.xid = htobe32(request->xid);
	msg.hdr.secs = htobe16((uint16_t) request->since_startup.tv_sec);
	msg.hdr.flags = client_address.s_addr ? 0 : htobe16(DHCP_FLAGS_BROADCAST);
	memcpy(msg.hdr.ciaddr, &client_address, sizeof(client_address));
	memset(msg.hdr.chaddr, 0, sizeof(msg.hdr.chaddr));
	memcpy(msg.hdr.chaddr, &interface->hwaddr, sizeof(interface->hwaddr));
	msg.hdr.magic[0] = DHCP_MAGIC_0;
	msg.hdr.magic[1] = DHCP_MAGIC_1;
	msg.hdr.magic[2] = DHCP_MAGIC_2;
	msg.hdr.magic[3] = DHCP_MAGIC_3;

	const size_t optsmax = sizeof(msg.options);
	size_t optsoff = 0;
	const unsigned char msgtype = DHCPREQUEST;
	optsoff = add_option(msg.options, optsmax, optsoff, OPTION_DHCP_MSGTYPE, 1,
	                     &msgtype);
	if ( request->requests_len )
		optsoff = add_option(msg.options, optsmax, optsoff,
		                     OPTION_PARAMETER_REQUEST, request->requests_len,
		                     request->requests);
	if ( !client_address.s_addr )
	{
		optsoff = add_option(msg.options, optsmax, optsoff,
			                 OPTION_SERVER_IDENTIFIER,
			                 sizeof(request->server_identifier),
			                 request->server_identifier);
		optsoff = add_option(msg.options, optsmax, optsoff, OPTION_REQUESTED_IP,
			                 sizeof(request->yiaddr),
			                 request->yiaddr);
	}
	optsoff = add_option_byte(msg.options, optsmax, optsoff, OPTION_END);

	const size_t msgsize = sizeof(msg.hdr) + optsoff;

	if ( sendto(interface->sock_fd, &msg, msgsize, 0,
	            (const struct sockaddr*) &dest, sizeof(dest)) < 0)
	{
		warn("send");
		// Drop packets and retry on transient errors and otherwise consider the
		// send attempt permanently failed for now.
		return errno == EAGAIN || errno == EWOULDBLOCK ||
		       errno == ENOMEM || errno == ENOBUFS;
	}

	return true;
}

static ssize_t receive_dhcp_message(const struct interface* interface,
                                    struct dhcp_message* msg,
                                    struct timespec* left,
                                    struct sockaddr_in* remote,
                                    socklen_t* remote_len)
{
	struct pollfd pfd = { .fd = interface->sock_fd, .events = POLLIN };
	int num_events = ppoll(&pfd, 1, left, NULL);
	if ( num_events < 0 )
		err(1, "ppoll");
	if ( num_events == 0 )
		return -1;
	*remote_len = sizeof(remote);
	ssize_t amount = recvfrom(interface->sock_fd, msg, sizeof(*msg), 0,
	                          (struct sockaddr*) remote, remote_len);
	if ( amount < 0 )
	{
		warn("recv");
		return -1;
	}
	return amount;
}

static bool check_dchp_message(const struct interface* interface,
                               const struct request* request,
                               struct dhcp_message* msg,
                               size_t amount)
{
	if ( (size_t) amount < sizeof(msg->hdr) )
		return false;
	if ( msg->hdr.op != DHCP_OP_BOOTREPLY )
		return false;
	if ( msg->hdr.htype != DHCP_HTYPE_ETHERNET ||
	     msg->hdr.hlen != DHCP_HLEN_ETHERNET )
		return false;
	unsigned char chaddr[16];
	memset(chaddr, 0, sizeof(chaddr));
	memcpy(chaddr, &interface->hwaddr, sizeof(interface->hwaddr));
	if ( memcmp(msg->hdr.chaddr, chaddr, sizeof(msg->hdr.chaddr)) != 0 )
		return false;
	if ( msg->hdr.xid != htobe32(request->xid) )
		return false;
	if ( msg->hdr.magic[0] != DHCP_MAGIC_0 ||
	     msg->hdr.magic[1] != DHCP_MAGIC_1 ||
	     msg->hdr.magic[2] != DHCP_MAGIC_2 ||
	     msg->hdr.magic[3] != DHCP_MAGIC_3 )
		return false;
	return true;
}

static bool parse_dhcpoffer(const struct interface* interface,
                            struct request* request,
                            struct dhcp_message* msg,
                            size_t amount)
{
	if ( !check_dchp_message(interface, request, msg, amount) )
		return false;

	struct option_iterate iter;
	unsigned char optlen;
	unsigned char* optdata;

	option_iterate_begin_msg(&iter, msg, amount);

	if ( !option_search(&iter, OPTION_DHCP_MSGTYPE, &optlen, &optdata) ||
	     optlen != 1 || optdata[0] != DHCPOFFER )
	{
		fprintf(stderr, "error: not DHCPOFFER\n");
		return false;
	}

	if ( !option_search(&iter, OPTION_SERVER_IDENTIFIER, &optlen, &optdata) ||
	     optlen != sizeof(request->server_identifier) )
		return false;

	memcpy(request->server_identifier, optdata,
	       sizeof(request->server_identifier));
	memcpy(request->yiaddr, msg->hdr.yiaddr, sizeof(request->yiaddr));

	return true;
}

static bool parse_dhcpack(const struct interface* interface,
                          const struct config* config,
                          struct request* request,
                          struct lease* lease,
                          struct dhcp_message* msg,
                          size_t amount)
{
	if ( !check_dchp_message(interface, request, msg, amount) )
		return false;

	struct option_iterate iter;
	unsigned char optlen;
	unsigned char* optdata;

	option_iterate_begin_msg(&iter, msg, amount);

	if ( !option_search(&iter, OPTION_DHCP_MSGTYPE, &optlen, &optdata) ||
	     optlen != 1 || optdata[0] != DHCPACK )
	{
		fprintf(stderr, "error: not DHCPACK\n");
		return false;
	}

	if ( !option_search(&iter, OPTION_SERVER_IDENTIFIER, &optlen, &optdata) ||
	     optlen != sizeof(request->server_identifier) )
	{
		fprintf(stderr, "error: DHCPACK missing server identifier\n");
		return false;
	}

	if ( memcmp(request->yiaddr, msg->hdr.yiaddr, sizeof(request->yiaddr)) )
	{
		fprintf(stderr, "error: Served bait-and-switched the address\n");
		return false;
	}

	if ( config->inet.subnet.method == AUTO )
	{
		if ( !option_search(&iter, OPTION_SUBNET, &optlen, &optdata) ||
		     optlen != sizeof(lease->subnet) )
		{
			fprintf(stderr, "error: DHCPACK missing subnet mask\n");
			return false;
		}
		memcpy(&lease->subnet, optdata, sizeof(lease->subnet));
	}

	if ( config->inet.router.method == AUTO )
	{
		if ( !option_search(&iter, OPTION_ROUTERS, &optlen, &optdata) ||
		     optlen < sizeof(lease->router) )
		{
			fprintf(stderr, "error: DHCPACK missing router information\n");
			return false;
		}
		memcpy(&lease->router, optdata, sizeof(lease->router));
	}

	if ( !option_search(&iter, OPTION_LEASE_TIME, &optlen, &optdata) ||
	     optlen != 4 )
	{
		fprintf(stderr, "error: DHCPACK missing lease time\n");
		return false;
	}
	lease->lease_time = (uint32_t) optdata[0] << 24 |
	                    (uint32_t) optdata[1] << 16 |
	                    (uint32_t) optdata[2] << 8 |
	                    (uint32_t) optdata[3] << 0;
	if ( !lease->lease_time )
	{
		fprintf(stderr, "error: DHCPACK has zero lease time\n");
		return false;
	}

	memcpy(request->server_identifier, optdata,
	       sizeof(request->server_identifier));
	memcpy(&lease->address, msg->hdr.yiaddr, sizeof(lease->address));

	if ( config->dns.servers.method == AUTO )
	{
		if ( option_search(&iter, OPTION_DNS, &optlen, &optdata) )
		{
			size_t offset = 0;
			for ( lease->dns_count = 0;
			      lease->dns_count < DNSCONFIG_MAX_SERVERS &&
			      4 <= optlen - offset;
			      lease->dns_count++ )
			{
				lease->dns[lease->dns_count][0] = optdata[offset++];
				lease->dns[lease->dns_count][1] = optdata[offset++];
				lease->dns[lease->dns_count][2] = optdata[offset++];
				lease->dns[lease->dns_count][3] = optdata[offset++];
			}
		}
	}

	return true;
}

static bool find_dhcp_server(const struct interface* interface,
                             struct request* request)
{
	struct sockaddr_in dest = {0};
	dest.sin_family = AF_INET;
	dest.sin_port = htobe16(PORT_DHCP_SERVER);
	dest.sin_addr.s_addr = htobe32(INADDR_BROADCAST);

	unsigned int retransmissions = 0;

	struct timespec last_sent = timespec_make(-1, 0);
	struct timespec timeout = timespec_make(0, 0);

	while ( true )
	{
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		struct timespec since_sent = timespec_sub(now, last_sent);
		if ( timespec_ge(since_sent, timeout) )
		{
			if ( retransmissions == 0 )
				fprintf(stderr, "Broadcasting DHCPDISCOVER\n");
			else
				fprintf(stderr, "Broadcasting DHCPDISCOVER (attempt %i)\n",
				       retransmissions + 1);

			if ( !send_dhcpdiscover(interface, request, dest) )
				return false;

			last_sent = now;
			timeout = timespec_make(1 << retransmissions,
			                        arc4random_uniform(1000000000));
			if ( retransmissions < 6 )
				retransmissions++;
			else
			{
				fprintf(stderr, "error: DHCPDISCOVER timed out\n");
				return false;
			}
		}

		struct timespec left =
			timespec_sub(timespec_add(last_sent, timeout), now);
		struct dhcp_message msg = {0};
		ssize_t amount = receive_dhcp_message(interface, &msg, &left,
		                                      &request->remote,
		                                      &request->remote_len);
		if ( amount < 0 )
			continue;
		if ( !parse_dhcpoffer(interface, request, &msg, amount) )
			continue;

		getnameinfo((const struct sockaddr*) &request->remote,
		            request->remote_len,
		            request->remote_host_str, sizeof(request->remote_host_str),
		            request->remote_serv_str, sizeof(request->remote_serv_str),
		            NI_NUMERICHOST | NI_NUMERICSERV);
		inet_ntop(AF_INET, request->yiaddr, request->yiaddr_str,
		          INET_ADDRSTRLEN);
		fprintf(stderr, "DHCPOFFER of %s from %s:%s\n", request->yiaddr_str,
		        request->remote_host_str, request->remote_serv_str);
		return true;
	}
}

static bool acquire_lease(const struct interface* interface,
                          const struct config* config,
                          struct request* request,
                          struct lease* lease)
{
	// Don't unicast during the REBINDING state.
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	bool unicast = lease->leased && timespec_lt(now, lease->t2);

	struct sockaddr_in dest = {0};
	dest.sin_family = AF_INET;
	dest.sin_port = htobe16(PORT_DHCP_SERVER);
	dest.sin_addr.s_addr = htobe32(INADDR_BROADCAST);

	if ( lease->leased )
	{
		memcpy(request->yiaddr, &lease->address, sizeof(request->yiaddr));
		request->remote.sin_family = AF_INET;
		request->remote.sin_addr = lease->server;
		request->remote.sin_port = htobe16(PORT_DHCP_SERVER);
		request->remote_len = sizeof(request->remote);
	}

	if ( unicast )
		dest.sin_addr.s_addr = lease->server.s_addr;

	inet_ntop(AF_INET, request->yiaddr, request->yiaddr_str, INET_ADDRSTRLEN);
	getnameinfo((const struct sockaddr*) &request->remote,
	            sizeof(request->remote),
	            request->remote_host_str, sizeof(request->remote_host_str),
	            request->remote_serv_str, sizeof(request->remote_serv_str),
	            NI_NUMERICHOST | NI_NUMERICSERV);
	fprintf(stderr, "%s %s from %s:%s\n",
	        lease->leased ? "Renewing" : "Requesting", request->yiaddr_str,
	        request->remote_host_str, request->remote_serv_str);

	unsigned int retransmissions = 0;

	struct timespec last_sent = timespec_make(-1, 0);
	struct timespec timeout = timespec_make(0, 0);

	while ( true )
	{
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		struct timespec since_sent = timespec_sub(now, last_sent);
		if ( timespec_le(timeout, since_sent) )
		{
			const char* action = unicast ? "Sending" : "Broadcasting";
			if ( retransmissions == 0 )
				fprintf(stderr, "%s DHCPREQUEST", action);
			else
				fprintf(stderr, "%s DHCPREQUEST (attempt %i)",
				        action, retransmissions + 1);
			if ( unicast )
				fprintf(stderr, " to %s:%s\n",
				        request->remote_host_str, request->remote_serv_str);
			else
				fputc('\n', stderr);

			if ( !send_dhcprequest(interface, request, dest, lease->address) )
				return false;

			last_sent = now;
			timeout = timespec_make(1 << retransmissions,
			                        arc4random_uniform(1000000000));
			if ( retransmissions < 6 )
				retransmissions++;
			else
			{
				fprintf(stderr, "error: DHCPREQUEST timed out\n");
				return false;
			}
		}

		struct timespec left =
			timespec_sub(timespec_add(last_sent, timeout), now);
		struct dhcp_message msg = {0};
		struct sockaddr_in peer;
		socklen_t peer_len;
		ssize_t amount = receive_dhcp_message(interface, &msg, &left, &peer,
		                                      &peer_len);
		if ( amount < 0 )
			continue;

		if ( peer_len != request->remote_len ||
		     memcmp(&peer, &request->remote, peer_len) != 0 )
			continue;

		// TODO: Handle DHCPNACK gracefully, unassign the allocated address.
		if ( !parse_dhcpack(interface, config, request, lease, &msg, amount) )
			continue;

		fprintf(stderr, "DHCPACK of %s from %s:%s\n", request->yiaddr_str,
		        request->remote_host_str, request->remote_serv_str);

		memcpy(&lease->server, request->server_identifier,
		       sizeof(lease->server));
		lease->expiration = timespec_add(request->begun,
		                                 timespec_make(lease->lease_time, 0));
		// The lease isn't expired in the main loop during T2 renewal, which may
		// take 2^6 (64) seconds and T2 has 15% of the lease time to renew, so
		// that means it needs at least 64 / 0.15 = 427 seconds (7 min) to avoid
		// using the lease after it has expired. Round up to a nice 10 minutes.
		if ( 10 * 60 <= lease->lease_time )
		{
			struct timespec d1 = timespec_make(lease->lease_time * 0.5,
			                                   arc4random_uniform(1000000000));
			struct timespec d2 = timespec_make(lease->lease_time * 0.85,
			                                   arc4random_uniform(1000000000));
			lease->t1 = timespec_add(request->begun, d1);
			lease->t2 = timespec_add(request->begun, d2);
		}
		else
		{
			fprintf(stderr, "warning: Lease time of %u seconds is too short "
			        "for renewal to work properly", lease->lease_time);
			lease->t1 = lease->expiration;
			lease->t2 = lease->expiration;
		}
		lease->leased = true;
		return true;
	}
}

static void configure_interface(const struct interface* interface,
                                const struct config* config,
                                const struct lease* lease)
{
	if ( config->inet.address.method != NONE ||
	     config->inet.router.method != NONE ||
	     config->inet.subnet.method != NONE )
	{
		struct if_config_inet inet_cfg = {0};

		if ( ioctl(interface->if_fd, NIOC_GETCONFIG_INET, &inet_cfg) < 0 )
			err(1, "%s: ioctl: NIOC_GETCONFIG_INET", interface->name);

		if ( config->inet.address.method == AUTO )
			inet_cfg.address = lease->address;
		else if ( config->inet.address.method == MANUAL )
			inet_cfg.address = config->inet.address.addr;

		if ( config->inet.router.method == AUTO )
			inet_cfg.router = lease->router;
		else if ( config->inet.router.method == MANUAL )
			inet_cfg.router = config->inet.router.addr;

		if ( config->inet.subnet.method == AUTO )
			inet_cfg.subnet = lease->subnet;
		else if ( config->inet.subnet.method == MANUAL )
			inet_cfg.subnet = config->inet.subnet.addr;

		if ( ioctl(interface->if_fd, NIOC_SETCONFIG_INET, &inet_cfg) < 0 )
			err(1, "%s: ioctl: NIOC_SETCONFIG_INET", interface->name);
		fprintf(stderr, "Configured network interface %s\n", interface->name);
	}

	if ( config->dns.servers.method != NONE )
	{
		struct dnsconfig dnsconfig = {0};
		if ( config->dns.servers.method == AUTO )
		{
			dnsconfig.servers_count = lease->dns_count;
			for ( size_t i = 0; i < lease->dns_count; i++ )
			{
				dnsconfig.servers[i].family = AF_INET;
				dnsconfig.servers[i].addrsize =
					sizeof(dnsconfig.servers[i].addr.in);
				memcpy(&dnsconfig.servers[i].addr, lease->dns[i],
					sizeof(dnsconfig.servers[i].addr.in));
			}
		}
		else if ( config->dns.servers.method == MANUAL )
			dnsconfig = config->dns.servers.dnsconfig;

		if ( setdnsconfig(&dnsconfig) < 0 )
			err(1, "setdnsconfig");
		fprintf(stderr, "Configured DNS\n");
	}
}

static void activate_lease(const struct interface* interface,
                           const struct config* config,
                           const struct lease* lease)
{
	char address_str[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &lease->address, address_str, sizeof(address_str));
	fprintf(stderr, "Leased %s for %u seconds\n",
		   address_str, lease->lease_time);
	if ( config->inet.router.method == AUTO )
	{
		char router_str[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &lease->router, router_str, sizeof(router_str));
		fprintf(stderr, "Router is %s\n", router_str);
	}
	if ( config->inet.subnet.method == AUTO )
	{
		char subnet_str[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &lease->subnet, subnet_str, sizeof(subnet_str));
		fprintf(stderr, "Subnet is %s\n", subnet_str);
	}
	if ( config->dns.servers.method == AUTO )
	{
		if ( lease->dns_count == 0 )
			fprintf(stderr, "No DNS servers were offered\n");
		else for ( size_t i = 0; i < lease->dns_count; i++ )
		{
			char dns_str[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, lease->dns[i], dns_str, sizeof(dns_str));
			fprintf(stderr, "DNS server %zu is %s\n", i + 1, dns_str);
		}
	}
	configure_interface(interface, config, lease);
}

static void deactivate_lease(const struct interface* interface,
                             const struct config* config,
                             struct lease* lease)
{
	char address_str[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &lease->address, address_str, sizeof(address_str));
	fprintf(stderr, "Lease of %s has expired after %u seconds\n",
		   address_str, lease->lease_time);
	struct if_config_inet inet_cfg = {0};
	if ( ioctl(interface->if_fd, NIOC_GETCONFIG_INET, &inet_cfg) < 0 )
		err(1, "%s: ioctl: NIOC_GETCONFIG_INET", interface->name);
	if ( config->inet.address.method == AUTO )
		inet_cfg.address.s_addr = htobe32(INADDR_ANY);
	if ( config->inet.router.method == AUTO )
		inet_cfg.router.s_addr = htobe32(INADDR_ANY);
	if ( config->inet.subnet.method == AUTO )
		inet_cfg.subnet.s_addr = htobe32(INADDR_ANY);
	if ( ioctl(interface->if_fd, NIOC_SETCONFIG_INET, &inet_cfg) < 0 )
		err(1, "%s: ioctl: NIOC_SETCONFIG_INET", interface->name);
	fprintf(stderr, "Unconfigured network interface %s\n", interface->name);
	lease->expiration = timespec_nul();
	lease->address.s_addr = htobe32(INADDR_ANY);
	lease->server.s_addr = htobe32(INADDR_ANY);
	lease->leased = false;
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

static void wait_for_link(struct interface* interface, int* timeout_ptr)
{
	fprintf(stderr, "Waiting for interface %s to come up\n",
	        interface->name);
	while ( true )
	{
		struct pollfd pfd =
		{
			.fd = interface->if_fd,
			.events = POLLOUT
		};
		int num_events = poll(&pfd, 1, *timeout_ptr);
		if ( num_events < 0 )
			err(1, "poll");
		else if ( num_events == 1 )
			break;
		// Signal readiness if waiting for the link to go up times out.
		if ( 0 <= *timeout_ptr )
		{
			fprintf(stderr, "Link has not come up yet on %s\n",
			        interface->name);
			ready();
			*timeout_ptr = -1;
		}
	}
	fprintf(stderr, "Interface %s is up\n", interface->name);
}

int main(int argc, char* argv[])
{
	struct interface interface = {0};

	const char* file = NULL;
	bool test = false;

	int opt;
	while ( (opt = getopt(argc, argv, "f:t")) != -1 )
	{
		switch ( opt )
		{
		case 'f': file = optarg; break;
		case 't': test = true; break;
		default: return 1;
		}
	}

	int args_min = test ? 0 : 1;
	int args_max = 1;
	if ( argc - optind < args_min || args_max < argc - optind )
	{
		printf("Usage: %s <interface>\n", argv[0]);
		return 1;
	}

	if ( 1 <= argc - optind )
	{
		const char* path = argv[optind];
		int dev_fd = open("/dev", O_RDONLY | O_DIRECTORY);
		if ( dev_fd < 0 )
			err(1, "/dev");
		interface.if_fd = openat(dev_fd, path, test ? O_RDONLY : O_RDWR);
		if ( interface.if_fd < 0 )
			err(1, "%s", path);
		close(dev_fd);

		int type = ioctl(interface.if_fd, IOCGETTYPE);
		if ( type < 0 )
			err(1, "%s: ioctl: IOCGETTYPE", path);
		if ( IOC_TYPE(type) != IOC_TYPE_NETWORK_INTERFACE )
			errx(1, "%s: Not a network interface", path);

		struct if_info info;
		if ( ioctl(interface.if_fd, NIOC_GETINFO, &info) < 0 )
			err(1, "%s: ioctl: NIOC_GETINFO", path);
		if ( info.type == IF_TYPE_LOOPBACK )
			errx(0, "%s: Loopback interface doesn't need to be configured", path);
		if ( info.type != IF_TYPE_ETHERNET )
			errx(1, "%s: ioctl: NIOC_GETINFO: Unknown device type", path);
		if ( info.addrlen != 6 )
			errx(1, "%s: ioctl: NIOC_GETINFO: Invalid address length", path);
		memcpy(interface.name, info.name, IF_NAMESIZE);
		memcpy(&interface.hwaddr, info.addr, sizeof(interface.hwaddr));
		interface.linkid = info.linkid;
	}

	struct config config;
	load_config(&interface, &config, file);

	if ( test )
		return 0;

	interface.sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if ( interface.sock_fd < 0 )
		err(1, "socket");

	if ( setsockopt(interface.sock_fd, SOL_SOCKET, SO_BINDTOINDEX,
	                &interface.linkid, sizeof(interface.linkid)) < 0 )
		err(1, "setsockopt: SO_BINDTOINDEX");

	int enable = 1;
	if ( setsockopt(interface.sock_fd, SOL_SOCKET, SO_BROADCAST, &enable,
	                sizeof(enable)) < 0 )
		err(1, "setsockopt: SO_BROADCAST");

	struct sockaddr_in local = {0};
	local.sin_family = AF_INET;
	local.sin_port = htobe16(PORT_DHCP_CLIENT);
	local.sin_addr.s_addr = htobe32(INADDR_ANY);
	if ( bind(interface.sock_fd, (const struct sockaddr*) &local,
	          sizeof(local)) < 0 )
	{
		if ( errno == EADDRINUSE )
			errx(0, "%s: Interface is already managed: bind: 0.0.0.0:%u",
			     interface.name, PORT_DHCP_CLIENT);
		err(1, "%s: bind: 0.0.0.0:%u", interface.name, PORT_DHCP_CLIENT);
	}

	if ( config.ether.address.method == AUTO ||
	     config.ether.address.method == MANUAL )
	{
		struct if_config_ether ether_cfg = {0};

		if ( ioctl(interface.if_fd, NIOC_GETCONFIG_ETHER, &ether_cfg) < 0 )
			err(1, "%s: ioctl: NIOC_GETCONFIG_ETHER", interface.name);

		if ( config.ether.address.method == AUTO )
			ether_cfg.address = interface.hwaddr;
		else if ( config.ether.address.method == MANUAL )
			ether_cfg.address = config.ether.address.addr;

		if ( ioctl(interface.if_fd, NIOC_SETCONFIG_ETHER, &ether_cfg) < 0 )
			err(1, "%s: ioctl: NIOC_SETCONFIG_ETHER", interface.name);
		fprintf(stderr, "Configured ethernet on interface %s\n",
		        interface.name);
	}

	bool dhcp_needed = config.inet.address.method == AUTO ||
	                   config.inet.router.method == AUTO ||
	                   config.inet.subnet.method == AUTO ||
	                   config.dns.servers.method == AUTO;
	// TODO: Implement DHCPINFORM mode.
	if ( dhcp_needed && config.inet.address.method != AUTO )
		errx(1,
		     "%s: IP address must be configured automatically if using DHCP",
		     interface.name);
	if ( !dhcp_needed )
	{
		configure_interface(&interface, &config, NULL);
		return 0;
	}

	// TODO: Allow the link up timeout to be configurable.
	int link_up_timeout = 10 * 1000; // Documented in dhclient(8).

	struct timespec startup;
	clock_gettime(CLOCK_MONOTONIC, &startup);

	bool first = true;
	bool link_up = false;
	bool success = false;
	struct lease lease = {0};
	while ( true )
	{
		if ( !first )
			ready();

		if ( errno == ENETDOWN )
			link_up = false;

		if ( !first && !success )
		{
			fprintf(stderr, "Negotiation failed, waiting before restarting\n");
			struct timespec delay =
				timespec_make(1, arc4random_uniform(1000000000));
			nanosleep(&delay, NULL);
		}

		first = false;
		success = false;

		if ( !link_up )
		{
			wait_for_link(&interface, &link_up_timeout);
			link_up = true;
		}

		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if ( lease.leased && timespec_le(lease.expiration, now) )
			deactivate_lease(&interface, &config, &lease);

		struct request request = {0};
		if ( config.inet.router.method == AUTO )
			request.requests[request.requests_len++] = OPTION_ROUTERS;
		if ( config.inet.subnet.method == AUTO )
			request.requests[request.requests_len++] = OPTION_SUBNET;
		if ( config.dns.servers.method == AUTO )
			request.requests[request.requests_len++] = OPTION_DNS;
		request.xid = arc4random();
		request.begun = now;
		request.since_startup = timespec_sub(now, startup);

		if ( !lease.leased && !find_dhcp_server(&interface, &request) )
			continue;

		if ( !lease.leased || timespec_le(lease.t1, now) )
		{
			if ( acquire_lease(&interface, &config, &request, &lease) )
			{
				activate_lease(&interface, &config, &lease);
				ready();
			}
			if ( !lease.leased )
				continue;
		}
		success = true;

		struct timespec wakeup;
		if ( timespec_lt(now, lease.t1) )
			wakeup = lease.t1;
		else if ( timespec_lt(now, lease.t2) )
			wakeup = lease.t2;
		else
			wakeup = lease.expiration;
		// TODO: Use poll to wake on incoming datagrams which are discarded.
		//       Otherwise they'll be received with errors on renewal and
		//       rejected and legimate packets might be dropped until the
		//       receive queue drains.
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wakeup, NULL);
	}
}
