/*
 * Copyright (c) 2016, 2017, 2018 Jonas 'Sortie' Termansen.
 * Copyright (c) 2021 Juhani 'nortti' Krekelä.
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
 * ifconfig.c
 * Configure network interface.
 */

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/if_ether.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct if_all
{
	struct if_info info;
	struct if_status status;
	struct if_config config;
};

static void link_id_print(const struct if_all* all, const void* ptr)
{
	(void) all;
	unsigned int id = *(const unsigned int*) ptr;
	printf("%u", id);
}

static void link_type_print(const struct if_all* all, const void* ptr)
{
	(void) all;
	int type = *(const int*) ptr;
	const char* type_string;
	switch ( type )
	{
	case IF_TYPE_ETHERNET: type_string = "ether"; break;
	case IF_TYPE_LOOPBACK: type_string = "loopback"; break;
	default: type_string = "unknown"; break;
	}
	fputs(type_string, stdout);
}

static void link_up_print(const struct if_all* all, const void* ptr)
{
	(void) all;
	int flags = *(const int*) ptr;
	fputs(flags & IF_STATUS_FLAGS_UP ? "yes" : "no", stdout);
}

static void link_name_print(const struct if_all* all, const void* ptr)
{
	(void) all;
	const char* name = (const char*) ptr;
	fputs(name, stdout);
}

static void ether_address_print(const struct if_all* all, const void* ptr)
{
	(void) all;
	struct ether_addr* addr = (struct ether_addr*) ptr;
	printf("%02x:%02x:%02x:%02x:%02x:%02x",
	       addr->ether_addr_octet[0], addr->ether_addr_octet[1],
	       addr->ether_addr_octet[2], addr->ether_addr_octet[3],
	       addr->ether_addr_octet[4], addr->ether_addr_octet[5]);
}

static void ether_hwaddress_print(const struct if_all* all, const void* ptr)
{
	struct ether_addr hwaddr;
	memcpy(&hwaddr, ptr, sizeof(hwaddr));
	ether_address_print(all, &hwaddr);
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

static bool ether_address_parse(const struct if_all* all,
                                void* ptr,
                                const char* string)
{
	struct ether_addr* addr = (struct ether_addr*) ptr;
	if ( !strcmp(string, "default") )
	{
		memcpy(addr, all->info.addr, sizeof(*addr));
		return true;
	}
	return mac_parse(addr, string);
}

static void inet_address_print(const struct if_all* all, const void* ptr)
{
	(void) all;
	char addr[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, ptr, addr, sizeof(addr));
	fputs(addr, stdout);
}

static bool inet_address_parse(const struct if_all* all,
                               void* ptr,
                               const char* string)
{
	(void) all;
	return inet_pton(AF_INET, string, ptr) == 1;
}

#define ARRAY_LENGTH(array) (sizeof(array) / sizeof((array)[0]))

struct configuration
{
	const char* name;
	size_t offset;
	void (*print)(const struct if_all*, const void*);
	bool (*parse)(const struct if_all*, void*, const char*);
	bool hidden;
};

#define INFOOFFSET(parameter) \
	(offsetof(struct if_all, info) + \
	 offsetof(struct if_info, parameter))

#define STATUSOFFSET(parameter) \
	(offsetof(struct if_all, status) + \
	 offsetof(struct if_status, parameter))

#define CONFIGOFFSET(protocol, parameter) \
	(offsetof(struct if_all, config) + \
	 offsetof(struct if_config, protocol) + \
	 offsetof(struct if_config_##protocol, parameter))

struct configuration link_configurations[] =
{
	{ "up", STATUSOFFSET(flags), link_up_print, NULL, false },
	{ "type", INFOOFFSET(type), link_type_print, NULL, false },
	{ "id", INFOOFFSET(linkid), link_id_print, NULL, false },
	{ "name", INFOOFFSET(name), link_name_print, NULL, true },
};

struct configuration ether_configurations[] =
{
	{ "address", CONFIGOFFSET(ether, address),
	  ether_address_print, ether_address_parse, false },
	{ "hwaddress", INFOOFFSET(addr), ether_hwaddress_print, NULL, false },
};

struct configuration loopback_configurations[] =
{
};

struct configuration inet_configurations[] =
{
	{ "address", CONFIGOFFSET(inet, address),
	  inet_address_print, inet_address_parse, false },
	{ "router", CONFIGOFFSET(inet, router),
	  inet_address_print, inet_address_parse, false },
	{ "subnet", CONFIGOFFSET(inet, subnet),
	  inet_address_print, inet_address_parse, false },
};

struct protocol
{
	const char* name;
	int link_type_value;
	struct configuration* configurations;
	size_t configurations_count;
};

struct protocol protocols[] =
{
	{ "link", 0,
      link_configurations, ARRAY_LENGTH(link_configurations) },
	{ "ether", IF_TYPE_ETHERNET,
      ether_configurations, ARRAY_LENGTH(ether_configurations) },
	{ "loopback", IF_TYPE_LOOPBACK,
      loopback_configurations, ARRAY_LENGTH(loopback_configurations) },
	{ "inet", 0,
      inet_configurations, ARRAY_LENGTH(inet_configurations) },
};

static int filter_dev_netif(const struct dirent* entry)
{
	char* path;
	if ( asprintf(&path, "/dev/%s", entry->d_name) < 0 )
		err(1, "malloc");
	// TODO: Open with O_STAT or some future extension that lets us properly
	//       test whether this is a network interface before complaining we
	//       couldn't open it. Otherwise it's annoying for non-root users to get
	//       warnings about non-network-interfaces in /dev they aren't supposed
	//       to be able to open.
	int fd = open(path, O_RDONLY | O_NOFOLLOW);
	if ( fd < 0 )
	{
		struct stat st;
		if ( lstat(path, &st) < 0 )
		{
			warn("stat: %s", path);
			free(path);
			return 0;
		}
		// TODO: Determine whether this is a network interface without having
		//       access to the device. Otherwise non-root users will be warned
		//       about non-network interfaces in /dev they're not supposed to be
		//       able to access.
		if ( S_ISCHR(st.st_mode) )
			warn("%s", path);
		free(path);
		return 0;
	}
	free(path);
	int type = ioctl(fd, IOCGETTYPE);
	close(fd);
	return IOC_TYPE(type) == IOC_TYPE_NETWORK_INTERFACE;
}

enum specifier_type { ETHER, ETHERHW, INET, ID };

struct if_specifier
{
	enum specifier_type type;
	union
	{
		struct ether_addr mac_addr;
		struct in_addr ipv4_addr;
		unsigned int id;
	};
};

static bool parse_specifier(struct if_specifier* specifier, const char* string)
{
	if ( !strncmp(string, "ether:", strlen("ether:")) )
	{
		specifier->type = ETHER;
		return mac_parse(&specifier->mac_addr, string + strlen("ether:"));
	}
	else if ( !strncmp(string, "etherhw:", strlen("etherhw:")) )
	{
		specifier->type = ETHERHW;
		return mac_parse(&specifier->mac_addr, string + strlen("etherhw:"));
	}
	else if ( !strncmp(string, "inet:", strlen("inet:")) )
	{
		specifier->type = INET;
		return inet_pton(AF_INET, string + strlen("inet:"),
		                 &specifier->ipv4_addr) == 1;
	}
	else if ( !strncmp(string, "id:", strlen("id:")) )
	{
		specifier->type = ID;
		const char* idstr = string + strlen("id:");
		char* end;
		errno = 0;
		unsigned long ulong = strtoul(idstr, &end, 10);
		if ( errno || !*idstr || *end || ulong > UINT_MAX )
			return false;
		specifier->id = ulong;
		return true;
	}

	return false;
}

static int find_interface(const char* specifier_string,
                          char if_name[IF_NAMESIZE])
{
	struct if_specifier specifier;
	if ( !parse_specifier(&specifier, specifier_string) )
		errx(1, "Invalid interface specifier: %s", specifier_string);

	struct if_nameindex* ifs = if_nameindex();
	if ( !ifs )
		err(1, "if_nameindex");

	int if_fd = -1;
	for ( size_t i = 0; ifs[i].if_index || ifs[i].if_name; i++ )
	{
		const char* name = ifs[i].if_name;

		char* path;
		if ( asprintf(&path, "/dev/%s", name) < 0 )
			err(1, "malloc");
		int fd = open(path, O_RDONLY);
		if ( fd < 0 )
			err(1, "%s", path);
		free(path);

		struct if_info ifinfo;
		struct if_config ifconfig;
		if ( ioctl(fd, NIOC_GETINFO, &ifinfo) < 0 )
			err(1, "%s: ioctl: NIOC_GETINFO", name);
		if ( ioctl(fd, NIOC_GETCONFIG, &ifconfig) < 0 )
			err(1, "%s: ioctl: NIOC_GETCONFIG", name);

		bool match = false;
		switch ( specifier.type )
		{
		case ETHER:
			match = ifinfo.type == IF_TYPE_ETHERNET &&
			        !memcmp(&specifier.mac_addr, &ifconfig.ether.address,
			                sizeof(struct ether_addr));
			break;
		case ETHERHW:
			match = ifinfo.type == IF_TYPE_ETHERNET &&
			        !memcmp(&specifier.mac_addr, &ifinfo.addr,
			                sizeof(struct ether_addr));
			break;
		case INET:
			match = !memcmp(&specifier.ipv4_addr, &ifconfig.inet.address,
			                sizeof(struct in_addr));
			break;
		case ID:
			match = ifinfo.linkid == specifier.id;
			break;
		}

		// Ensure the specifier unambiguously matches an interface.
		if ( match && if_fd != -1 )
			errx(1, "Ambiguous specifier; matches at least %s and %s: %s",
			     if_name, name, specifier_string);

		if ( match )
		{
			if_fd = fd;
			strlcpy(if_name, name, IF_NAMESIZE);
		}
		else
			close(fd);

	}

	if_freenameindex(ifs);

	if ( if_fd == -1 )
		errx(1, "Specifier does not match any interfaces: %s",
		     specifier_string);

	return if_fd;
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
	bool list = false;

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
			case 'l': list = true; break;
			default:
				errx(1, "unknown option -- '%c'", c);
			}
		}
		else
			errx(1, "unknown option: %s", arg);
	}

	compact_arguments(&argc, &argv);

	int devices_count = 1;
	struct dirent** devices = NULL;
	if ( argc <= 1 )
	{
		devices_count = scandir("/dev", &devices, filter_dev_netif, alphasort);
		if ( devices_count < 0 )
			err(1, "scandir: /dev");
	}

	int result = 0;
	for ( int d = 0; d < devices_count; d++ )
	{
		char if_name[IF_NAMESIZE];
		const char* name = devices ? devices[d]->d_name : argv[1];
		int fd;
		if ( strchr(name, '/') )
			fd = open(name, O_RDONLY);
		else if ( strchr(name, ':') )
		{
			fd = find_interface(name, if_name);
			name = if_name;
		}
		else
		{
			char* path;
			if ( asprintf(&path, "/dev/%s", name) < 0 )
				err(1, "malloc");
			fd = open(path, O_RDONLY);
			free(path);
		}
		if ( fd < 0 && errno == ENOENT )
			errx(1, "%s: No such interface", name);
		if ( fd < 0 )
			err(1, "%s", name);
		struct if_all all;
		if ( ioctl(fd, NIOC_GETINFO, &all.info) < 0 )
			err(1, "%s: ioctl: NIOC_GETINFO", name);
		if ( ioctl(fd, NIOC_GETSTATUS, &all.status) < 0 )
			err(1, "%s: ioctl: NIOC_GETSTATUS", name);
		if ( ioctl(fd, NIOC_GETCONFIG, &all.config) < 0 )
			err(1, "%s: ioctl: NIOC_GETCONFIG", name);
		if ( list && argc == 1 )
		{
			puts(name);
			continue;
		}
		else if ( list && (argc == 2 || argc == 3) )
		{
			bool found = false;
			for ( size_t i = 0; i < ARRAY_LENGTH(protocols); i++ )
			{
				struct protocol* protocol = &protocols[i];
				if ( 3 <= argc && strcmp(protocol->name, argv[2]) != 0 )
					continue;
				if ( protocol->link_type_value &&
				     all.info.type != protocol->link_type_value )
				{
					if ( 3 <= argc )
						errx(1, "%s: %s: Interface does not support protocol",
						     name, argv[2]);
					continue;
				}
				if ( argc < 3 )
				{
					puts(protocol->name);
					continue;
				}
				found = true;
				for ( size_t j = 0; j < protocol->configurations_count; j++ )
					puts(protocol->configurations[j].name);
			}
			if ( 3 <= argc && !found )
				errx(1, "%s: %s: No such protocol", name, argv[2]);
			continue;
		}
		else if ( argc <= 2 )
		{
			printf("%s:\n", name);
			for ( size_t i = 0; i < ARRAY_LENGTH(protocols); i++ )
			{
				struct protocol* protocol = &protocols[i];
				if ( protocol->link_type_value &&
				     all.info.type != protocol->link_type_value )
					continue;
				putchar('\t');
				fputs(protocol->name, stdout);
				for ( size_t j = 0; j < protocol->configurations_count; j++ )
				{
					struct configuration* configuration =
						&protocol->configurations[j];
					if ( configuration->hidden )
						continue;
					putchar(' ');
					fputs(configuration->name, stdout);
					putchar(' ');
					void* ptr = ((char*) &all + configuration->offset);
					configuration->print(&all, ptr);
				}
				putchar('\n');
			}
			continue;
		}
		struct protocol* protocol = NULL;
		for ( int i = 2; i < argc; )
		{
			const char* operand = argv[i++];
			bool found = false;
			for ( size_t n = 0;
			      protocol && n < protocol->configurations_count;
			      n++ )
			{
				struct configuration* configuration =
					&protocol->configurations[n];
				if ( strcmp(operand, configuration->name) != 0 )
					continue;
				found = true;
				void* ptr = ((char*) &all + configuration->offset);
				if ( list )
				{
					configuration->print(&all, ptr);
					putchar('\n');
				}
				else
				{
					if ( !configuration->parse )
						errx(1, "%s: %s: %s: Configuration is read-only",
					         name, protocol->name, operand);
					if ( i == argc )
						errx(1, "%s: %s: %s: Expected parameter",
						     name, protocol->name, operand);
					const char* parameter = argv[i++];
					if ( !configuration->parse(&all, ptr, parameter) )
						errx(1, "%s: %s: %s: Invalid value: %s",
						     name, protocol->name, operand, parameter);
				}
			}
			for ( size_t n = 0; !found && n < ARRAY_LENGTH(protocols); n++ )
			{
				struct protocol* new_protocol = &protocols[n];
				if ( strcmp(operand, new_protocol->name) != 0 )
					continue;
				if ( new_protocol->link_type_value &&
				     all.info.type != new_protocol->link_type_value )
					errx(1, "%s: %s: Interface does not support protocol",
					     name, operand);
				found = true;
				protocol = new_protocol;
			}
			if ( !found )
			{
				if ( !protocol )
					errx(1, "%s: %s: No such protocol", name, operand);
				errx(1, "%s: %s: No such protocol or configuration of protocol "
					 "%s", name, operand, protocol->name);
			}
		}
		if ( !list && ioctl(fd, NIOC_SETCONFIG, &all.config) < 0 )
			err(1, "%s: ioctl: NIOC_SETCONFIG", name);
		close(fd);
	}

	if ( ferror(stdout) || fflush(stdout) == EOF )
		err(1, "stdout");
	return result;
}
