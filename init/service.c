/*
 * Copyright (c) 2024 Jonas 'Sortie' Termansen.
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
 * service.c
 * Start and stop services.
 */

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <wchar.h>

static bool array_add(void*** array_ptr,
                      size_t* used_ptr,
                      size_t* length_ptr,
                      void* value)
{
	void** array;
	memcpy(&array, array_ptr, sizeof(array)); // Strict aliasing.

	if ( *used_ptr == *length_ptr )
	{
		size_t length = *length_ptr;
		if ( !length )
			length = 4;
		void** new_array = reallocarray(array, length, 2 * sizeof(void*));
		if ( !new_array )
			return false;
		array = new_array;
		memcpy(array_ptr, &array, sizeof(array)); // Strict aliasing.
		*length_ptr = length * 2;
	}

	memcpy(array + (*used_ptr)++, &value, sizeof(value)); // Strict aliasing.

	return true;
}

static char** tokenize(size_t* out_tokens_used, const char* string)
{
	size_t tokens_used = 0;
	size_t tokens_length = 0;
	char** tokens = malloc(sizeof(char*));
	if ( !tokens )
		return NULL;
	bool failed = false;
	bool invalid = false;
	while ( *string )
	{
		if ( isspace((unsigned char) *string) )
		{
			string++;
			continue;
		}
		if ( *string == '#' )
			break;
		char* token;
		size_t token_size;
		FILE* fp = open_memstream(&token, &token_size);
		if ( !fp )
		{
			failed = true;
			break;
		}
		bool singly = false;
		bool doubly = false;
		bool escaped = false;
		while ( *string )
		{
			char c = *string++;
			if ( !escaped && !singly && !doubly && isspace((unsigned char) c) )
				break;
			if ( !escaped && !doubly && c == '\'' )
			{
				singly = !singly;
				continue;
			}
			if ( !escaped && !singly && c == '"' )
			{
				doubly = !doubly;
				continue;
			}
			if ( !singly && !escaped && c == '\\' )
			{
				escaped = true;
				continue;
			}
			if ( escaped )
			{
				switch ( c )
				{
				case 'a': c = '\a'; break;
				case 'b': c = '\b'; break;
				case 'e': c = '\e'; break;
				case 'f': c = '\f'; break;
				case 'n': c = '\n'; break;
				case 'r': c = '\r'; break;
				case 't': c = '\t'; break;
				case 'v': c = '\v'; break;
				default: break;
				};
			}
			escaped = false;
			if ( fputc((unsigned char) c, fp) == EOF )
			{
				failed = true;
				break;
			}
		}
		if ( singly || doubly || escaped )
		{
			fclose(fp);
			free(token);
			invalid = true;
			break;
		}
		if ( fflush(fp) == EOF )
		{
			fclose(fp);
			free(token);
			failed = true;
			break;
		}
		fclose(fp);
		if ( !array_add((void***) &tokens, &tokens_used, &tokens_length,
		                token) )
		{
			free(token);
			failed = true;
			break;
		}
	}
	if ( failed || invalid )
	{
		for ( size_t i = 0; i < tokens_used; i++ )
			free(tokens[i]);
		free(tokens);
		if ( invalid )
			errno = 0;
		return NULL;
	}
	char** new_tokens = reallocarray(tokens, tokens_used, sizeof(char*));
	if ( new_tokens )
		tokens = new_tokens;
	*out_tokens_used = tokens_used;
	return tokens;
}

static char** receive(FILE* fp, size_t* out_tokens_used)
{
	char* line = NULL;
	size_t line_size;
	if ( getline(&line, &line_size, fp) < 0 )
	{
		if ( ferror(fp) )
			errx(1, "receiving reply: Unexpected end of connection");
		else
			err(1, "receiving reply");
	}
	char** result = tokenize(out_tokens_used, line);
	free(line);
	if ( !result )
	{
		if ( errno )
			errx(1, "invalid reply: %s", line);
		else
			errx(1, "failed to parse reply");
	}
	if ( !*out_tokens_used )
			errx(1, "invalid empty reply");
	if ( !strcmp(result[0], "ok") )
		return result;
	else if ( !strcmp(result[0], "error") )
		errx(1, "error: %s", 2 <= *out_tokens_used ? result[1] : "Unknown");
	else
		errx(1, "unknown reply: %s", result[0]);
}

static int open_local_client_socket(const char* path, int flags)
{
	size_t path_length = strlen(path);
	size_t addr_size = offsetof(struct sockaddr_un, sun_path) + path_length + 1;
	struct sockaddr_un* sockaddr = malloc(addr_size);
	if ( !sockaddr )
		return -1;
	sockaddr->sun_family = AF_LOCAL;
	strcpy(sockaddr->sun_path, path);
	int fd = socket(AF_LOCAL, SOCK_STREAM | flags, 0);
	if ( fd < 0 )
		return free(sockaddr), -1;
	if ( connect(fd, (const struct sockaddr*) sockaddr, addr_size) < 0 )
		return close(fd), free(sockaddr), -1;
	free(sockaddr);
	return fd;
}

static void rewrite(const char* path, const char* daemon, const char* flags)
{
	FILE* fp = fopen(path, "r");
	if ( !fp && errno != ENOENT )
		err(1, "%s", path);
	char* out_path;
	if ( asprintf(&out_path, "%s.XXXXXX", path) < 0 )
		err(1, "malloc");
	int out_fd = mkstemp(out_path);
	if ( out_fd < 0 )
		err(1, "mkstemp: %s.XXXXXX", path);
	FILE* out = fdopen(out_fd, "w");
	if ( !out )
	{
		unlink(out_path);
		err(1, "fdopen");
	}
	bool found = false;
	char* line = NULL;
	size_t line_size = 0;
	ssize_t line_length;
	off_t line_number = 0;
	while ( fp && 0 < (line_length = getline(&line, &line_size, fp)) )
	{
		line_number++;
		size_t tokenc;
		char** tokens = tokenize(&tokenc, line);
		if ( !tokens )
		{
			unlink(out_path);
			if ( errno )
				err(1, "%s", path);
			else
				errx(1, "%s:%ji: Syntax error", path, (intmax_t) line_number);
		}
		if ( 2 <= tokenc &&
		     !strcmp(tokens[0], "require") && !strcmp(tokens[1], daemon) )
		{
			found = true;
			if ( flags )
				fprintf(out, "require %s%s\n", daemon, flags);
		}
		else
			fputs(line, out);
	}
	free(line);
	if ( !found && flags )
		fprintf(out, "require %s%s\n", daemon, flags);
	if ( (fp && ferror(fp)) || ferror(out) || fflush(out) == EOF )
	{
		unlink(out_path);
		err(1, "%s", path);
	}
	if ( fp )
	{
		struct stat st;
		fstat(fileno(fp), &st);
		fchmod(out_fd, st.st_mode & 07777);
		fchown(out_fd, st.st_uid, st.st_gid);
		fclose(fp);
	}
	else
		fchmod(out_fd, 0666 & ~getumask());
	if ( rename(out_path, path) < 0 )
	{
		unlink(out_path);
		err(1, "rename: %s -> %s", out_path, path);
	}
	fclose(out);
}

static bool check_daemon_exists_in_dir(const char* dir, const char* daemon)
{
	char* path;
	if ( asprintf(&path, "%s/%s", dir, daemon) < 0 )
		err(1, "malloc");
	bool result = !access(path, F_OK);
	free(path);
	return result;
}


static void check_daemon_exists(const char* daemon)
{
	if ( !check_daemon_exists_in_dir("/etc/init", daemon) &&
	     !check_daemon_exists_in_dir("/share/init", daemon) )
		errx(1, "%s: Daemon does not exist", daemon);
}

static size_t string_display_length(const char* str)
{
	size_t display_length = 0;
	mbstate_t ps;
	memset(&ps, 0, sizeof(ps));
	while ( true )
	{
		wchar_t wc;
		size_t amount = mbrtowc(&wc, str, SIZE_MAX, &ps);
		if ( amount == 0 )
			break;
		if ( amount == (size_t) -1 || amount == (size_t) -2 )
		{
			display_length++;
			str++;
			memset(&ps, 0, sizeof(ps));
			continue;
		}
		int width = wcwidth(wc);
		if ( width < 0 )
			width = 0;
		if ( SIZE_MAX - display_length < (size_t) width )
			display_length = SIZE_MAX;
		else
			display_length += (size_t) width;
		str += amount;
	}
	return display_length;
}

static void pad(const char* string, size_t padding)
{
	fputs(string, stdout);
	for ( size_t length = string_display_length(string);
	      length < padding; padding-- )
		putchar(' ');
}

static void format_statuses(char** tokens, size_t count)
{
	size_t daemon_length = 0;
	size_t state_length = 10;
	for ( size_t i = 0; i < count; i++ )
	{
		if ( !strncmp(tokens[i], "daemon=", strlen("daemon=")) )
		{
			const char* arg = tokens[i] + strlen("daemon=");
			size_t length = string_display_length(arg);
			if ( daemon_length < length )
				daemon_length = length;
		}
		else if ( !strncmp(tokens[i], "state=", strlen("state=")) )
		{
			const char* arg = tokens[i] + strlen("state=");
			size_t length = string_display_length(arg);
			if ( state_length < length )
				state_length = length;
		}
	}
	size_t i = 0;
	while ( i < count )
	{
		size_t next = i;
		while ( next < count && strcmp(tokens[next], ",") )
			next++;
		const char* daemon = NULL;
		const char* state = NULL;
		for ( size_t n = i; n < next; n++ )
		{
			if ( !strncmp(tokens[n], "state=", strlen("state=")) )
				state = tokens[n] + strlen("state=");
			else if ( !strncmp(tokens[n], "daemon=", strlen("daemon=")) )
				daemon = tokens[n] + strlen("daemon=");
		}
		if ( !state || !daemon )
			errx(1, "missing information in reply");
		pad(daemon, daemon_length + 2);
		pad(state, state_length);
		for ( size_t n = i; n < next; n++ )
		{
			if ( strncmp(tokens[n], "state=", strlen("state=")) &&
			     strncmp(tokens[n], "daemon=", strlen("daemon=")) )
				printf("  %s", tokens[n]);
		}
		putchar('\n');
		i = next;
		if ( i < count && !strcmp(tokens[i], ",") )
			i++;
	}
}

int main(int argc, char* argv[])
{
	const char* init_socket = getenv("INIT_SOCKET");
	if ( !init_socket )
		init_socket = "/var/run/init";

	bool exit_code = false;
	bool list = false;
	bool no_await = false;
	bool optional = true;
	bool raw = false;
	const char* source = "local";

	const struct option longopts[] =
	{
		{"exit-code", no_argument, NULL, 256},
		{"list", no_argument, NULL, 'l'},
		{"no-await", no_argument, NULL, 257},
		{"no-optional", no_argument, NULL, 258},
		{"source", required_argument, NULL, 's'},
		{"raw", no_argument, NULL, 'r'},
		{0, 0, 0, 0}
	};
	const char* opts = "lrs:";
	int opt;
	while ( (opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1 )
	{
		switch ( opt )
		{
		case 'l': list = true; break;
		case 'r': raw = true; break;
		case 's': source = optarg; break;
		case 256: exit_code = true; break;
		case 257: no_await = true; break;
		case 258: optional = false; break;
		default: return 2;
		}
	}

	int fd = open_local_client_socket(init_socket, 0);
	if ( fd < 0 )
		err(1, "%s", init_socket);
	FILE* fp = fdopen(fd, "r+");
	if ( !fp )
		err(1, "fdopen");

	if ( raw )
	{
		for ( int i = optind; i < argc; i++ )
		{
			if ( fprintf(fp, "%s%c", argv[i], i + 1 == argc ? '\n' : ' ') < 0 )
				err(1, "%s", init_socket);
		}
		size_t tokens_count;
		char** tokens = receive(fp, &tokens_count);
		for ( size_t i = 0; i < tokens_count; i++ )
			printf("%s%c", tokens[i], i + 1 == tokens_count ? '\n' : ' ');
		return 0;
	}

	char flags[sizeof(" optional no-await exit-code")];
	snprintf(flags, sizeof(flags), "%s%s%s",
	         optional ? " optional" : "",
	         no_await ? " no-await" : "",
	         exit_code ? " exit-code" : "");
	char* source_path;
	if ( asprintf(&source_path, "/etc/init/%s", source) < 0 )
		err(1, "malloc");

	bool no_command = list;

	if ( no_command && 0 < argc - optind  )
		errx(1, "unexpected extra operand: %s", argv[optind]);
	else if ( !no_command && argc - optind < 2 )
		errx(1, "usage: <daemon> <command>");

	const char* daemon = !no_command ? argv[optind++] : NULL;
	const char* command = !no_command ? argv[optind++] : NULL;

	if ( command && strcmp(command, "signal") && 0 < argc - optind )
		errx(1, "unexpected extra operand: %s", argv[optind]);

	if ( list )
		fprintf(fp, "list\n");
	else if ( !strcmp(command, "enable") )
	{
		check_daemon_exists(daemon);
		rewrite(source_path, daemon, flags);
		fprintf(fp, "require %s %s %s\n", source, daemon, flags);
	}
	else if ( !strcmp(command, "disable") )
	{
		rewrite(source_path, daemon, NULL);
		fprintf(fp, "unrequire %s %s\n", source, daemon);
	}
	else if ( !strcmp(command, "start") )
		fprintf(fp, "require %s %s %s\n", source, daemon, flags);
	else if ( !strcmp(command, "stop") )
		fprintf(fp, "unrequire %s %s\n", source, daemon);
	else if ( !strcmp(command, "restart") )
		fprintf(fp, "restart %s\n", daemon);
	else if ( !strcmp(command, "reload") )
		fprintf(fp, "reload %s\n", daemon);
	else if ( !strcmp(command, "reconfigure") )
		fprintf(fp, "reconfigure %s\n", daemon);
	else if ( !strcmp(command, "terminate") )
		fprintf(fp, "terminate %s\n", daemon);
	else if ( !strcmp(command, "kill") )
		fprintf(fp, "kill %s\n", daemon);
	else if ( !strcmp(command, "signal") )
	{
		if ( argc - optind < 1 )
			errx(1, "expected signal name");
		const char* signal = argv[optind++];
		if ( 0 < argc - optind )
			errx(1, "unexpected extra operand: %s", argv[optind]);
		fprintf(fp, "signal %s %s\n", daemon, signal);
	}
	else if ( !strcmp(command, "status") )
		fprintf(fp, "status %s\n", daemon);
	else if ( !strcmp(command, "state") )
		fprintf(fp, "status %s\n", daemon);
	else if ( !strcmp(command, "pid") )
		fprintf(fp, "status %s\n", daemon);
	else if ( !strcmp(command, "exit-code") )
		fprintf(fp, "status %s\n", daemon);
	else if ( !strcmp(command, "requirements") ||
	          !strcmp(command, "dependents") ||
	          !strcmp(command, "edges") )
		fprintf(fp, "%s %s\n", command, daemon);
	else
		errx(1, "unknown command: %s", command);

	if ( ferror(fp) || fflush(fp) == EOF )
		err(1, "%s", init_socket);

	size_t tokens_count;
	char** tokens = receive(fp, &tokens_count);

	if ( list )
		format_statuses(tokens + 1, tokens_count - 1);
	else if ( !strcmp(command, "status") )
		format_statuses(tokens + 1, tokens_count - 1);
	else if ( !strcmp(command, "state") )
	{
		for ( size_t i = 1; i < tokens_count; i++ )
		{
			if ( !strncmp(tokens[i], "state=", strlen("state=")) )
			{
				puts(tokens[i] + strlen("state="));
				break;
			}
		}
	}
	else if ( !strcmp(command, "pid") )
	{
		for ( size_t i = 1; i < tokens_count; i++ )
		{
			if ( !strncmp(tokens[i], "pid=", strlen("pid=")) )
			{
				const char* arg = tokens[i] + strlen("pid=");
				if ( strcmp(arg, "0") )
					puts(arg);
				break;
			}
		}
	}
	else if ( !strcmp(command, "exit-code") )
	{
		for ( size_t i = 1; i < tokens_count; i++ )
		{
			if ( !strncmp(tokens[i], "exit=", strlen("exit=")) )
			{
				const char* arg = tokens[i] + strlen("exit=");
				if ( strcmp(arg, "n/a") )
					puts(arg);
				break;
			}
		}
	}
	else if ( !strcmp(command, "requirements") ||
	          !strcmp(command, "dependents") ||
	          !strcmp(command, "edges") )
	{
		for ( size_t i = 1; i < tokens_count; i++ )
		{
			if ( !strcmp(tokens[i], ",") )
				continue;
			size_t eol = i + 1 == tokens_count || !strcmp(tokens[i + 1], ",");
			printf("%s%c", tokens[i], eol ? '\n' : ' ');
		}
	}

	return 0;
}
