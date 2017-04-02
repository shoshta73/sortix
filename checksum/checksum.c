/*
 * Copyright (c) 2017 Jonas 'Sortie' Termansen.
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
 * sha256sum.c
 * Compute and verify checksums.
 */

#include <err.h>
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
static bool checklists = false;
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

static void printhex(uint8_t* buffer, size_t size)
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
	// TODO: How should it be handled if fd is a directory?
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

static int digest_path(uint8_t digest[DIGEST_MAX_LENGTH],
                       const char* path)
{
	if ( !strcmp(path, "-") )
		return digest_fd(digest, 0, "-");
	int fd = open(path, O_RDONLY);
	if ( fd < 0 )
	{
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
	if ( status == 0 &&
	     timingsafe_memcmp(checksum, digest, hash->digest_size) != 0 )
		status = 2;
	explicit_bzero(digest, sizeof(digest));
	if ( !silent && (!quiet || status != 0) )
		printf("%s: %s\n", path, status == 0 ? "OK" : "FAILED");
	return status;
}

static int checklist_fp(FILE* fp,
                        const char* path,
                        size_t files_count,
                        char** files)
{
	int result = 0;
	uint8_t* checksums = NULL;
	if ( files )
	{
		checksums = calloc(1 + hash->digest_size, files_count);
		if ( !checksums )
			err(1, "malloc");
	}
	uint8_t checksum[DIGEST_MAX_LENGTH];
	bool any = false;
	char* line = NULL;
	size_t line_size = 0;
	ssize_t line_length;
	// TODO: Count line numbers for error messages.
	while ( 0 < (line_length = getline(&line, &line_size, fp)) )
	{
		if ( line[line_length-1] != '\n' )
			errx(1, "%s: Line was not terminated with a newline", path);
		line[--line_length] = '\0';
		if ( (size_t) line_length < 2 * hash->digest_size )
			errx(1, "%s: Improperly formated %s checksum line",
			     path, hash->name);
		for ( size_t i = 0; i < hash->digest_size; i++ )
		{
			int higher = debase(line[i*2 + 0]);
			int lower = debase(line[i*2 + 1]);
			if ( higher == -1 || lower == -1 )
				errx(1, "%s: Improperly formated %s checksum line",
				     path, hash->name);
			checksum[i] = higher << 4 | lower;
		}
		if ( line[2 * hash->digest_size + 0] != ' ' ||
		     line[2 * hash->digest_size + 1] != ' ' ||
		     line[2 * hash->digest_size + 2] == '\0' )
			errx(1, "%s: Improperly formated %s checksum line",
			     path, hash->name);
		const char* file = line + 2 * hash->digest_size + 2;
		if ( files )
		{
			// TODO: Inefficient lookup, note there can be multiple matches.
			for ( size_t i = 0; i < files_count; i++ )
			{
				if ( !strcmp(files[i], file) )
				{
					if ( checksums[i * (1 + hash->digest_size) + 0] )
						errx(1, "%s: Duplicate hash found for: %s", path, file);
					checksums[i * (1 + hash->digest_size) + 0] = 1;
					memcpy(&checksums[i * (hash->digest_size + 1) + 1],
					       checksum, hash->digest_size);
				}
			}
		}
		else
		{
			int status = verify_path(checksum, file);
			if ( status == 1 )
				result = 1;
			else if ( status == 2 && result == 0 )
				result = 2;
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
		if ( !checksums[i * (1 + hash->digest_size) + 0] )
			errx(1, "%s: No hash found for: %s", path, file);
		int status =
			verify_path(&checksums[i * (1 + hash->digest_size) + 1], file);
		if ( status == 1 )
			result = 1;
		else if ( status == 2 && result == 0 )
			result = 2;
	}
	explicit_bzero(checksum, sizeof(checksum));
	free(checksums); // TODO: explicit_bzero?
	return result;
}

static int checklist_path(const char* path,
                          size_t files_count,
                          char** files)
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
			case 'c': checklists = true; break;
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
			case 'q': quiet = true; break;
			case 's': silent = true; break;
			default:
				errx(1, "unknown option -- '%c'", c);
			}
		}
		else if ( !strcmp(arg, "--quiet") )
			quiet = true;
		else if ( !strcmp(arg, "--status") )
			silent = true;
		// TODO: --ignore-missing
		else
			errx(1, "unknown option: %s", arg);
	}

	compact_arguments(&argc, &argv);

	if ( checklist && checklists )
		errx(1, "The -c and -C options are mutually incompatible");

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

	int result = 0;

	if ( checklist )
		result = checklist_path(checklist, argc - 1, argv + 1);
	else if ( argc == 1 )
	{
		if ( checklists )
			result = checklist_fp(stdin, "-", 0, NULL);
		else
		{
			uint8_t digest[DIGEST_MAX_LENGTH];
			result = digest_fd(digest, 0, "-");
			if ( result == 0 )
			{
				printhex(digest, hash->digest_size);
				puts("  -");
				explicit_bzero(digest, sizeof(digest));
			}
		}
	}
	else for ( int i = 1; i < argc; i++ )
	{
		if ( checklists )
			result = checklist_path(argv[i], 0, NULL);
		else
		{
			uint8_t digest[DIGEST_MAX_LENGTH];
			result = digest_path(digest, argv[i]);
			if ( result == 0 )
			{
				printhex(digest, hash->digest_size);
				printf("  %s\n", argv[i]);
				explicit_bzero(digest, sizeof(digest));
			}
		}
	}

	if ( ferror(stdout) || fflush(stdout) == EOF )
		return 1;
	return result;
}
