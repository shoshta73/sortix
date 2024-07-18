/*
 * Copyright (c) 2017, 2020 Jonas 'Sortie' Termansen.
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
 * checksum.c
 * Compute and check cryptographic hashes.
 */

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <sha2.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char hexchars[] = "0123456789abcdef";
static uint8_t buffer[65536];

#define DIGEST_MAX_LENGTH SHA512_DIGEST_LENGTH

union ctx
{
	SHA2_CTX sha2;
};

struct hash
{
	const char* name;
	size_t digest_size;
	void (*init)(union ctx* ctx);
	void (*update)(union ctx* ctx, const uint8_t* buffer, size_t size);
	void (*final)(uint8_t digest[], union ctx* ctx);
};

#define WRAP(ctx_member, algorithm) \
static void Wrap##algorithm##Init(union ctx* ctx) \
{ \
	algorithm##Init(&ctx->ctx_member); \
} \
\
static void Wrap##algorithm##Update(union ctx* ctx, \
                                    const uint8_t* buffer, \
                                    size_t size) \
{ \
	algorithm##Update(&ctx->ctx_member, buffer, size); \
} \
\
static void Wrap##algorithm##Final(uint8_t digest[], union ctx* ctx) \
{ \
	algorithm##Final(digest, &ctx->ctx_member); \
}

WRAP(sha2, SHA224)
WRAP(sha2, SHA256)
WRAP(sha2, SHA384)
WRAP(sha2, SHA512_256)
WRAP(sha2, SHA512)

#define HASH(variable, name, algorithm) \
static struct hash variable = \
{ \
	name, \
	algorithm##_DIGEST_LENGTH, \
	Wrap##algorithm##Init, \
	Wrap##algorithm##Update, \
	Wrap##algorithm##Final, \
}

HASH(sha224, "SHA224", SHA224);
HASH(sha256, "SHA256", SHA256);
HASH(sha384, "SHA384", SHA384);
HASH(sha512_256, "SHA512/256", SHA512_256);
HASH(sha512, "SHA512", SHA512);

static struct hash* hashes[] =
{
	&sha224,
	&sha256,
	&sha384,
	&sha512_256,
	&sha512,
	NULL,
};

static struct hash* hash = NULL;
static const char* algorithm = NULL;
static const char* checklist = NULL;
static bool check = false;
static bool ignore_missing = false;
static bool quiet = false;
static bool silent = false;

int debase(char c)
{
	if ( '0' <= c && c <= '9' )
		return c - '0';
	if ( 'a' <= c && c <= 'f' )
		return c - 'a' + 10;
	if ( 'A' <= c && c <= 'F' )
		return c - 'A' + 10;
	return -1;
}

static void printhex(const uint8_t* buffer, size_t size)
{
	for ( size_t i = 0; i < size; i++ )
	{
		putchar(hexchars[buffer[i] >> 4]);
		putchar(hexchars[buffer[i] & 0xF]);
	}
}

static int digest_fd(uint8_t digest[DIGEST_MAX_LENGTH],
                     int fd,
                     const char* path)
{
	union ctx ctx;
	hash->init(&ctx);
	ssize_t amount;
	while ( 0 < (amount = read(fd, buffer, sizeof(buffer))) )
		hash->update(&ctx, buffer, amount);
	if ( amount < 0 )
	{
		warn("%s", path);
		return 1;
	}
	hash->final(digest, &ctx);
	return 0;
}

static int digest_path(uint8_t digest[DIGEST_MAX_LENGTH], const char* path)
{
	if ( !strcmp(path, "-") )
		return digest_fd(digest, 0, "-");
	int fd = open(path, O_RDONLY);
	if ( fd < 0 )
	{
		if ( errno == ENOENT && ignore_missing )
			return -1;
		warn("%s", path);
		return 1;
	}
	int result = digest_fd(digest, fd, path);
	close(fd);
	return result;
}

static int verify_path(uint8_t checksum[], const char* path)
{
	uint8_t digest[DIGEST_MAX_LENGTH];
	int status = digest_path(digest, path);
	if ( status == -1 )
		return status;
	if ( status == 0 &&
	     timingsafe_memcmp(checksum, digest, hash->digest_size) != 0 )
		status = 2;
	explicit_bzero(digest, sizeof(digest));
	if ( !silent && (!quiet || status != 0) )
		printf("%s: %s\n", path, status == 0 ? "OK" : "FAILED");
	return status;
}

struct checklist
{
	const char* file;
	uint8_t checksum[DIGEST_MAX_LENGTH];
	bool initialized;
};

static int compare_checklist_file(const void* a_ptr, const void* b_ptr)
{
	struct checklist* a = *(struct checklist**) a_ptr;
	struct checklist* b = *(struct checklist**) b_ptr;
	return strcmp(a->file, b->file);
}

static int search_checklist_file(const void* file_ptr, const void* elem_ptr)
{
	const char* file = (const char*) file_ptr;
	struct checklist* elem = *(struct checklist**) elem_ptr;
	return strcmp(file, elem->file);
}

static int checklist_fp(FILE* fp,
                        const char* path,
                        size_t files_count,
                        const char* const* files)
{
	struct checklist* checklist = NULL;
	struct checklist** checklist_sorted = NULL;
	if ( files )
	{
		checklist = calloc(sizeof(struct checklist), files_count);
		checklist_sorted = calloc(sizeof(struct checklist*), files_count);
		if ( !checklist || !checklist_sorted )
			err(1, "malloc");
		for ( size_t i = 0; i < files_count; i++ )
		{
			checklist[i].file = files[i];
			checklist_sorted[i] = &checklist[i];
		}
		qsort(checklist_sorted, files_count, sizeof(struct checklist*),
		      compare_checklist_file);
	}
	uint8_t checksum[DIGEST_MAX_LENGTH];
	bool any = false;
	char* line = NULL;
	size_t line_size = 0;
	ssize_t line_length;
	off_t line_number = 0;
	size_t read_failures = 0;
	size_t check_failures = 0;
	while ( 0 < (line_length = getline(&line, &line_size, fp)) )
	{
		line_number++;
		if ( line[line_length - 1] != '\n' )
			errx(1, "%s:%ji: Line was not terminated with a newline",
			     path, (intmax_t) line_number);
		line[--line_length] = '\0';
		if ( (size_t) line_length < 2 * hash->digest_size )
			errx(1, "%s:%ji: Improperly formatted %s checksum line",
			     path, (intmax_t) line_number, hash->name);
		for ( size_t i = 0; i < hash->digest_size; i++ )
		{
			int higher = debase(line[i*2 + 0]);
			int lower = debase(line[i*2 + 1]);
			if ( higher == -1 || lower == -1 )
				errx(1, "%s:%ji: Improperly formatted %s checksum line",
				     path, (intmax_t) line_number, hash->name);
			checksum[i] = higher << 4 | lower;
		}
		if ( line[2 * hash->digest_size + 0] != ' ' ||
		     line[2 * hash->digest_size + 1] != ' ' ||
		     line[2 * hash->digest_size + 2] == '\0' )
			errx(1, "%s:%ji: Improperly formatted %s checksum line",
			     path, (intmax_t) line_number, hash->name);
		const char* file = line + 2 * hash->digest_size + 2;
		if ( !strcmp(path, "-") && !strcmp(file, "-") )
			errx(1, "%s:%ji: Improperly formatted %s checksum line",
			     path, (intmax_t) line_number, hash->name);
		if ( files )
		{
			struct checklist** entry_ptr =
				bsearch(file, checklist_sorted, files_count,
                        sizeof(struct checksum*), search_checklist_file);
			if ( entry_ptr )
			{
				struct checklist* entry = *entry_ptr;
				if ( entry->initialized )
					errx(1, "%s:%ji: Duplicate hash found for: %s", path,
					     (intmax_t) line_number, file);
				memcpy(entry->checksum, checksum, DIGEST_MAX_LENGTH);
				entry->initialized = true;
			}
		}
		else
		{
			int status = verify_path(checksum, file);
			if ( status == 1 )
				read_failures++;
			else if ( status == 2 )
				check_failures++;
		}
		any = true;
	}
	free(line);
	if ( ferror(fp) )
		err(1, "%s", path);
	if ( !any )
		errx(1, "%s: No properly formatted %s checksum lines found",
		     path, hash->name);
	for ( size_t i = 0; i < files_count; i++ )
	{
		const char* file = files[i];
		struct checklist* entry = &checklist[i];
		if ( !entry->initialized )
			errx(1, "%s: No hash found for: %s", path, file);
		int status = verify_path(entry->checksum, file);
		if ( status == 1 )
			read_failures++;
		else if ( status == 2 )
			check_failures++;
	}
	explicit_bzero(checksum, sizeof(checksum));
	free(checklist);
	free(checklist_sorted);
	if ( !silent && read_failures )
		warnx("WARNING: %zu listed %s could not be read",
		      read_failures, read_failures == 1 ? "file" : "files");
	if ( !silent && check_failures )
		warnx("WARNING: %zu computed %s did NOT match",
		      check_failures, check_failures == 1 ? "checksum" : "checksums");
	return read_failures ? 1 : check_failures ? 2 : 0;
}

static int checklist_path(const char* path,
                          size_t files_count,
                          const char* const* files)
{
	if ( !strcmp(path, "-") )
		return checklist_fp(stdin, "-", files_count, files);
	FILE* fp = fopen(path, "r");
	if ( !fp )
		err(1, "%s", path);
	int result = checklist_fp(fp, path, files_count, files);
	fclose(fp);
	return result;
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
	char* argv0_last_slash = strrchr(argv[0], '/');
	const char* argv0_basename =
		argv0_last_slash ? argv0_last_slash + 1 : argv[0];

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
			case 'a':
				if ( !*(algorithm = arg + 1) )
				{
					if ( i + 1 == argc )
						errx(1, "option requires an argument -- 'a'");
					algorithm = argv[i+1];
					argv[++i] = NULL;
				}
				arg = "a";
				break;
			case 'c': check = true; break;
			case 'C':
				if ( !*(checklist = arg + 1) )
				{
					if ( i + 1 == argc )
						errx(1, "option requires an argument -- 'C'");
					checklist = argv[i+1];
					argv[++i] = NULL;
				}
				arg = "C";
				break;
			case 'i': ignore_missing = true; break;
			case 'q': quiet = true; break;
			case 's': silent = true; break;
			default:
				errx(1, "unknown option -- '%c'", c);
			}
		}
		else if ( !strcmp(arg, "--algorithm") )
		{
			if ( i + 1 == argc )
				errx(1, "option '--algorithm' requires an argument");
			algorithm = argv[i+1];
			argv[++i] = NULL;
		}
		else if ( !strncmp(arg, "--algorithm=", strlen("--algorithm=")) )
			algorithm = arg + strlen("--algorithm=");
		else if ( !strcmp(arg, "--check") )
			check = true;
		else if ( !strcmp(arg, "--checklist") )
		{
			if ( i + 1 == argc )
				errx(1, "option '--checklist' requires an argument");
			checklist = argv[i+1];
			argv[++i] = NULL;
		}
		else if ( !strncmp(arg, "--checklist=", strlen("--checklist=")) )
			checklist = arg + strlen("--checklist=");
		else if ( !strcmp(arg, "--ignore-missing") )
			ignore_missing = true;
		else if ( !strcmp(arg, "--quiet") )
			quiet = true;
		else if ( !strcmp(arg, "--status") )
			silent = true;
		else
			errx(1, "unknown option: %s", arg);
	}

	compact_arguments(&argc, &argv);

	if ( check && checklist )
		errx(1, "The -c and -C options are mutually incompatible");

	if ( !(check || checklist) && (ignore_missing || quiet || silent) )
		errx(1, "The -iqs options require -c or -C");

	if ( algorithm )
	{
		for ( size_t i = 0; !hash && hashes[i]; i++ )
			if ( !strcasecmp(hashes[i]->name, algorithm) )
				hash = hashes[i];
		if ( !hash )
			errx(1, "No such hash algorithm: %s", algorithm);
	}
	else if ( !strcmp(argv0_basename, "sha224sum") )
		hash = &sha224;
	else if ( !strcmp(argv0_basename, "sha256sum") )
		hash = &sha256;
	else if ( !strcmp(argv0_basename, "sha384sum") )
		hash = &sha384;
	else if ( !strcmp(argv0_basename, "sha512sum") )
		hash = &sha512;
	else
		errx(1, "No hash algorithm was specified with -a");

	bool read_failures = false;
	bool check_failures = false;

	if ( checklist )
	{
		int result =
			checklist_path(checklist, argc - 1, (const char* const*) argv + 1);
		if ( result == 1 )
			read_failures = true;
		else if ( result == 2 )
			check_failures = true;
	}
	else if ( argc == 1 )
	{
		if ( check )
		{
			int result = checklist_fp(stdin, "-", 0, NULL);
			if ( result == 1 )
				read_failures = true;
			else if ( result == 2 )
				check_failures = true;
		}
		else
		{
			uint8_t digest[DIGEST_MAX_LENGTH];
			int result = digest_fd(digest, 0, "-");
			if ( result == 0 )
			{
				printhex(digest, hash->digest_size);
				puts("  -");
				explicit_bzero(digest, sizeof(digest));
			}
			else if ( result == 1 )
				read_failures = true;
		}
	}
	else for ( int i = 1; i < argc; i++ )
	{
		if ( check )
		{
			int result = checklist_path(argv[i], 0, NULL);
			if ( result == 1 )
				read_failures = true;
			else if ( result == 2 )
				check_failures = true;
		}
		else
		{
			uint8_t digest[DIGEST_MAX_LENGTH];
			int result = digest_path(digest, argv[i]);
			if ( result == 0 )
			{
				printhex(digest, hash->digest_size);
				printf("  %s\n", argv[i]);
				explicit_bzero(digest, sizeof(digest));
			}
			else if ( result == 1 )
				read_failures = true;
		}
	}

	if ( ferror(stdout) || fflush(stdout) == EOF )
		return 1;
	return read_failures ? 1 : check_failures ? 2 : 0;
}
