/*
 * Copyright (c) 2017, 2020, 2024 Jonas 'Sortie' Termansen.
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

#include <sys/stat.h>

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

struct checklist
{
	const char* file;
	uint8_t checksum[DIGEST_MAX_LENGTH];
	bool initialized;
	bool invalidated;
};

static struct checklist** cache = NULL;
static size_t cache_used = 0;
static size_t cache_length = 0;
static struct timespec cache_time;
static struct hash* hash = NULL;
static const char* algorithm = NULL;
static const char* cache_path = NULL;
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

static void fprinthex(FILE* fp, const uint8_t* buffer, size_t size)
{
	for ( size_t i = 0; i < size; i++ )
	{
		fputc(hexchars[buffer[i] >> 4], fp);
		fputc(hexchars[buffer[i] & 0xF], fp);
	}
}

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

static struct checklist* checklist_lookup(struct checklist** sorted,
                                          size_t used,
                                          const char* file)
{
	struct checklist** entry_ptr =
		bsearch(file, sorted, used,
                sizeof(struct checksum*), search_checklist_file);
	return entry_ptr ? *entry_ptr : NULL;
}

static void checklist_add(struct checklist*** checklist_ptr,
                          size_t* used_ptr,
                          size_t* length_ptr,
                          uint8_t digest[DIGEST_MAX_LENGTH],
                          const char* file)
{
	struct checklist* entry = calloc(1, sizeof(struct checklist));
	if ( !entry || !(entry->file = strdup(file)) )
		err(1, "malloc");
	memcpy(entry->checksum, digest, hash->digest_size);
	if ( *used_ptr == *length_ptr )
	{
		struct checklist** new_checklist =
			reallocarray(*checklist_ptr, *length_ptr,
			             2 * sizeof(struct checklist*));
		if ( !new_checklist )
			err(1, "malloc");
		*checklist_ptr = new_checklist;
		*length_ptr *= 2;
	}
	(*checklist_ptr)[(*used_ptr)++] = entry;
}

static void checklist_parse(char* line,
                            size_t line_length,
                            struct checklist* entry,
                            const char* path,
                            off_t line_number)
{
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
		entry->checksum[i] = higher << 4 | lower;
	}
	if ( line[2 * hash->digest_size + 0] != ' ' ||
	     line[2 * hash->digest_size + 1] != ' ' ||
	     line[2 * hash->digest_size + 2] == '\0' )
		errx(1, "%s:%ji: Improperly formatted %s checksum line",
		     path, (intmax_t) line_number, hash->name);
	entry->file = line + 2 * hash->digest_size + 2;
	if ( !strcmp(path, "-") && !strcmp(entry->file, "-") )
		errx(1, "%s:%ji: Improperly formatted %s checksum line",
		     path, (intmax_t) line_number, hash->name);
}

static void checklist_read(struct checklist*** ptr,
                           size_t* used_ptr,
                           size_t* length_ptr,
                           struct timespec* time_ptr,
                           const char* path,
                           bool allow_missing)
{
	if ( !(*ptr = malloc(sizeof(struct checklist*) * 4)) )
		err(1, "malloc");
	*used_ptr = 0;
	*length_ptr = 4;
	FILE* fp = fopen(path, "r");
	if ( !fp )
	{
		if ( allow_missing )
			return;
		err(1, "malloc");
	}
	struct stat st;
	fstat(fileno(fp), &st);
	*time_ptr = st.st_mtim;
	char* line = NULL;
	size_t line_size = 0;
	ssize_t line_length;
	off_t line_number = 0;
	struct checklist input;
	while ( 0 < (line_length = getline(&line, &line_size, fp)) )
	{
		line_number++;
		checklist_parse(line, (size_t) line_length, &input, path, line_number);
		checklist_add(ptr, used_ptr, length_ptr, input.checksum, input.file);
	}
	free(line);
	if ( ferror(fp) )
		err(1, "%s", path);
	fclose(fp);
	qsort(*ptr, *used_ptr, sizeof(struct checklist*), compare_checklist_file);
}

static void checklist_write(struct checklist** checklist,
                            size_t used,
                            const char* path)
{
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
	for ( size_t i = 0; i < used; i++ )
	{
		fprinthex(out, checklist[i]->checksum, hash->digest_size);
		fprintf(out, "  %s\n", checklist[i]->file);
	}
	if ( ferror(out) || fclose(out) == EOF )
	{
		unlink(out_path);
		err(1, "%s", out_path);
	}
	if ( rename(out_path, path) < 0 )
		err(1, "rename: %s -> %s", out_path, path);
	free(out_path);
}

static int digest_fd(uint8_t digest[DIGEST_MAX_LENGTH],
                     int fd,
                     const char* path)
{
	struct checklist* entry = NULL;
	if ( cache && (entry = checklist_lookup(cache, cache_used, path)) &&
	     !entry->invalidated )
	{
		struct stat st;
		fstat(fd, &st);
		if ( st.st_mtim.tv_sec < cache_time.tv_sec ||
		     (st.st_mtim.tv_sec == cache_time.tv_sec &&
		      st.st_mtim.tv_nsec <= cache_time.tv_nsec) )
		{
			memcpy(digest, entry->checksum, hash->digest_size);
			return 0;
		}
	}
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
	if ( cache )
	{
		if ( entry )
		{
			memcpy(entry->checksum, digest, hash->digest_size);
			entry->invalidated = false;
		}
		else
		{
			checklist_add(&cache, &cache_used, &cache_length, digest, path);
			size_t i = cache_used - 1;
			while ( i && 0 < strcmp(cache[i - 1]->file, cache[i]->file) )
			{
				struct checklist* t = cache[i - 1];
				cache[i - 1] = cache[i];
				cache[i--] = t;
			}
		}
	}
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
	struct checklist* entry = NULL;
	if ( cache && (entry = checklist_lookup(cache, cache_used, path)) &&
	     timingsafe_memcmp(checksum, entry->checksum, hash->digest_size) != 0 )
		entry->invalidated = true;
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

static int checklist_fp(FILE* fp,
                        const char* path,
                        size_t files_count,
                        const char* const* files)
{
	struct checklist* checklist = NULL;
	struct checklist** checklist_sorted = NULL;
	if ( files )
	{
		checklist = calloc(files_count, sizeof(struct checklist));
		checklist_sorted = calloc(files_count, sizeof(struct checklist*));
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
	bool any = false;
	char* line = NULL;
	size_t line_size = 0;
	ssize_t line_length;
	off_t line_number = 0;
	size_t read_failures = 0;
	size_t check_failures = 0;
	struct checklist input;
	while ( 0 < (line_length = getline(&line, &line_size, fp)) )
	{
		line_number++;
		checklist_parse(line, (size_t) line_length, &input, path, line_number);
		if ( files )
		{
			struct checklist** entry_ptr =
				bsearch(input.file, checklist_sorted, files_count,
                        sizeof(struct checksum*), search_checklist_file);
			if ( entry_ptr )
			{
				struct checklist* entry = *entry_ptr;
				if ( entry->initialized )
					errx(1, "%s:%ji: Duplicate hash found for: %s", path,
					     (intmax_t) line_number, input.file);
				memcpy(entry->checksum, input.checksum, DIGEST_MAX_LENGTH);
				entry->initialized = true;
			}
		}
		else
		{
			int status = verify_path(input.checksum, input.file);
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
	explicit_bzero(input.checksum, sizeof(input.checksum));
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
		else if ( !strcmp(arg, "--cache") )
		{
			if ( i + 1 == argc )
				errx(1, "option '--cache' requires an argument");
			cache_path = argv[i+1];
			argv[++i] = NULL;
		}
		else if ( !strncmp(arg, "--cache=", strlen("--cache=")) )
			cache_path = arg + strlen("--cache=");
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

	if ( cache_path )
	{
		if ( !strcmp(cache_path, "-") )
			errx(1, "cache cannot be the standard input");
		checklist_read(&cache, &cache_used, &cache_length, &cache_time,
		               cache_path, true);
	}

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
			int result = digest_path(digest, "-");
			if ( result == 0 )
			{
				fprinthex(stdout, digest, hash->digest_size);
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
				fprinthex(stdout, digest, hash->digest_size);
				printf("  %s\n", argv[i]);
				explicit_bzero(digest, sizeof(digest));
			}
			else if ( result == 1 )
				read_failures = true;
		}
	}

	if ( ferror(stdout) || fflush(stdout) == EOF )
		return 1;

	if ( cache_path )
	     checklist_write(cache, cache_used, cache_path);

	return read_failures ? 1 : check_failures ? 2 : 0;
}
