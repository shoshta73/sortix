/*
 * Copyright (c) 2011, 2012, 2013, 2014, 2015, 2025 Jonas 'Sortie' Termansen.
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
 * stdio.h
 * Standard buffered input/output.
 */

#ifndef _INCLUDE_STDIO_H
#define _INCLUDE_STDIO_H

#include <sys/cdefs.h>

#include <sys/__/types.h>

#include <sortix/seek.h>

#ifndef _VA_LIST_DEFINED
#define __need___va_list
#include <stdarg.h>
typedef __gnuc_va_list va_list;
#define _VA_LIST_DEFINED
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if __USE_SORTIX || 200112L <= __USE_POSIX
#ifndef __off_t_defined
#define __off_t_defined
typedef __off_t off_t;
#endif
#endif

#ifndef __size_t_defined
#define __size_t_defined
#define __need_size_t
#include <stddef.h>
#endif

#if __USE_SORTIX || __USE_POSIX
#ifndef __ssize_t_defined
#define __ssize_t_defined
typedef __ssize_t ssize_t;
#endif
#endif

#ifndef NULL
#define __need_NULL
#include <stddef.h>
#endif

#ifndef __FILE_defined
#define __FILE_defined
typedef struct __FILE FILE;
#endif

#if __USE_SORTIX || 200809L <= __USE_POSIX
typedef off_t fpos_t;
#else
typedef __off_t fpos_t;
#endif

#if __USE_SORTIX || __USE_POSIX
#define L_ctermid (5 /*/dev/*/ + 32 /*TTY_NAME_MAX*/ + 1)
/* L_tmpnam will not be implemented. */
#endif

/* The possibilities for the third argument to `setvbuf'. */
#define _IOFBF 0 /* Fully buffered. */
#define _IOLBF 1 /* Line buffered. */
#define _IONBF 2 /* No buffering. */

#define EOF (-1)

/* FILENAME_MAX, FOPEN_MAX are not defined because Sortix doesn't have these
   restrictions. */
#if __USE_SORTIX || __USE_POSIX
/* TMP_MAX are not defined because Sortix doesn't have these restrictions. */
#endif

#if __USE_SORTIX || __USE_POSIX
/* P_tmpdir will not be implemented. */
#endif

/* Size of <stdio.h> buffers. */
#define BUFSIZ 8192

/* Constants used by `fparsemode'. */
#if __USE_SORTIX
#define FILE_MODE_READ (1 << 0)
#define FILE_MODE_WRITE (1 << 1)
#define FILE_MODE_APPEND (1 << 2)
#define FILE_MODE_CREATE (1 << 3)
#define FILE_MODE_TRUNCATE (1 << 4)
#define FILE_MODE_BINARY (1 << 5)
#define FILE_MODE_EXCL (1 << 6)
#define FILE_MODE_CLOEXEC (1 << 7)
#endif

extern FILE* const stdin;
extern FILE* const stdout;
extern FILE* const stderr;

#define stdin stdin
#define stdout stdout
#define stderr stderr

/* Functions from C89. */
void clearerr(FILE*);
int fclose(FILE*);
int feof(FILE*);
int ferror(FILE*);
int fflush(FILE*);
int fgetc(FILE*);
int fgetpos(FILE* __restrict, fpos_t* __restrict);
char* fgets(char* __restrict, int, FILE* __restrict);
FILE* fopen(const char* __restrict, const char* __restrict);
int fprintf(FILE* __restrict, const char* __restrict, ...)
	__attribute__((__format__ (printf, 2, 3)));
int fputc(int, FILE*);
int fputs(const char* __restrict, FILE* __restrict);
size_t fread(void* __restrict, size_t, size_t, FILE* __restrict);
FILE* freopen(const char* __restrict, const char *__restrict, FILE* __restrict);
int fscanf(FILE* __restrict, const char* __restrict, ... )
	__attribute__((__format__ (scanf, 2, 3)));
int fseek(FILE*, long, int);
int fsetpos(FILE*, const fpos_t*);
long ftell(FILE*);
size_t fwrite(const void* __restrict, size_t, size_t, FILE* __restrict);
int getc(FILE*);
int getchar(void);
#if __USE_C < 2011
/* gets will not be implemented. */
#endif
void perror(const char*);
int printf(const char* __restrict, ...)
	__attribute__((__format__ (printf, 1, 2)));
int putc(int, FILE*);
int putchar(int);
int puts(const char*);
int remove(const char*);
int rename(const char*, const char*);
void rewind(FILE*);
void setbuf(FILE* __restrict, char* __restrict);
int setvbuf(FILE* __restrict, char* __restrict, int, size_t);
#if !defined(__is_sortix_libc) /* not a warning inside libc */
__attribute__((__warning__("sprintf() is dangerous, use snprintf()")))
#endif
int sprintf(char* __restrict, const char* __restrict, ...)
	__attribute__((__format__ (printf, 2, 3)));
int scanf(const char* __restrict, ...)
	__attribute__((__format__ (scanf, 1, 2)));
int sscanf(const char* __restrict, const char* __restrict, ...)
	__attribute__((__format__ (scanf, 2, 3)));
FILE* tmpfile(void);
int ungetc(int, FILE*);
int vfprintf(FILE* __restrict, const char* __restrict, __gnuc_va_list)
	__attribute__((__format__ (printf, 2, 0)));
int vprintf(const char* __restrict, __gnuc_va_list)
	__attribute__((__format__ (printf, 1, 0)));
#if !defined(__is_sortix_libc) /* not a warning inside libc */
__attribute__((__warning__("vsprintf() is dangerous, use vsnprintf()")))
#endif
int vsprintf(char* __restrict, const char* __restrict, __gnuc_va_list)
	__attribute__((__format__ (printf, 2, 0)));

/* Functions from C99. */
#if __USE_SORTIX || 1999 <= __USE_C
int snprintf(char* __restrict, size_t, const char* __restrict, ...)
	__attribute__((__format__ (printf, 3, 4)));
int vfscanf(FILE* __restrict, const char* __restrict, __gnuc_va_list)
	__attribute__((__format__ (scanf, 2, 0)));
int vscanf(const char* __restrict, __gnuc_va_list)
	__attribute__((__format__ (scanf, 1, 0)));
int vsnprintf(char* __restrict, size_t, const char* __restrict, __gnuc_va_list)
	__attribute__((__format__ (printf, 3, 0)));
int vsscanf(const char* __restrict, const char* __restrict, __gnuc_va_list)
	__attribute__((__format__ (scanf, 2, 0)));
#endif

/* Functions from early POSIX. */
#if __USE_SORTIX || __USE_POSIX
int fileno(FILE*);
void flockfile(FILE*);
FILE* fdopen(int, const char*);
int ftrylockfile(FILE*);
void funlockfile(FILE*);
int getc_unlocked(FILE*);
int getchar_unlocked(void);
int putc_unlocked(int, FILE*);
int putchar_unlocked(int);
/* tmpnam will not be implemented. */
#endif

#if __USE_SORTIX || __USE_POSIX
char* ctermid(char*);
#endif

#if __USE_XOPEN
/* tempnam will not be implemented. */
#endif

/* Functions from less early POSIX. */
#if __USE_SORTIX || 199209L <= __USE_POSIX
int pclose(FILE*);
FILE* popen(const char*, const char*);
#endif

/* Functions from POSIX 2001. */
#if __USE_SORTIX || 200112L <= __USE_POSIX
int fseeko(FILE*, off_t, int);
off_t ftello(FILE*);
#endif

/* Functions from POSIX 2008. */
#if __USE_SORTIX || 200809L <= __USE_POSIX
int dprintf(int, const char* __restrict, ...)
	__attribute__((__format__ (printf, 2, 3)));
FILE* fmemopen(void* __restrict, size_t, const char* __restrict);
ssize_t getdelim(char** __restrict, size_t* __restrict, int, FILE* __restrict);
FILE* open_memstream(char**, size_t*);
ssize_t getline(char** __restrict, size_t* __restrict, FILE* __restrict);
int renameat(int, const char*, int, const char*);
int vdprintf(int, const char* __restrict, __gnuc_va_list)
	__attribute__((__format__ (printf, 2, 0)));
#endif

/* Functions from POSIX 2024. */
#if __USE_SORTIX || 202405L <= __USE_POSIX
int asprintf(char** __restrict, const char* __restrict, ...)
	__attribute__((__format__ (printf, 2, 3)));
int vasprintf(char** __restrict, const char* __restrict, __gnuc_va_list)
	__attribute__((__format__ (printf, 2, 0)));
#endif

/* Functions copied from elsewhere. */
#if __USE_SORTIX
void clearerr_unlocked(FILE*);
int feof_unlocked(FILE*);
int ferror_unlocked(FILE*);
int fflush_unlocked(FILE*);
int fileno_unlocked(FILE*);
int fgetc_unlocked(FILE*);
char* fgets_unlocked(char* __restrict, int, FILE* __restrict);
int fputc_unlocked(int, FILE*);
int fputs_unlocked(const char* __restrict, FILE* __restrict);
size_t fread_unlocked(void* __restrict, size_t, size_t, FILE* __restrict);
size_t fwrite_unlocked(const void* __restrict, size_t, size_t,
                       FILE* __restrict);
#endif

/* Functions that are Sortix extensions. */
#if __USE_SORTIX
int fparsemode(const char*);
int fpipe(FILE*[2]);
int fprintf_unlocked(FILE* __restrict, const char* __restrict, ...)
	__attribute__((__format__ (printf, 2, 3)));
int fscanf_unlocked(FILE* __restrict, const char* __restrict, ... )
	__attribute__((__format__ (scanf, 2, 3)));
int fseeko_unlocked(FILE*, off_t, int);
off_t ftello_unlocked(FILE*);
int removeat(int, const char*);
int setvbuf_unlocked(FILE* __restrict, char* __restrict, int, size_t);
char* sortix_gets(void);
int sortix_puts(const char*);
int ungetc_unlocked(int, FILE*);
int vfprintf_unlocked(FILE* __restrict, const char* __restrict, __gnuc_va_list)
	__attribute__((__format__ (printf, 2, 0)));
int vfscanf_unlocked(FILE* __restrict, const char* __restrict, __gnuc_va_list)
	__attribute__((__format__ (scanf, 2, 0)));
#endif

/* Functions that are Sortix extensions used for libc internal purposes. */
#if __USE_SORTIX
int fflush_stop_reading(FILE*);
int fflush_stop_reading_unlocked(FILE*);
int fflush_stop_writing(FILE*);
int fflush_stop_writing_unlocked(FILE*);
void fdeletefile(FILE*);
void fregister(FILE*);
void fresetfile(FILE*);
void funregister(FILE*);
FILE* fnewfile(void);
int fshutdown(FILE*);
#endif

/* The backends for printf and scanf. */
#if __USE_SORTIX
int cbprintf(void*, size_t (*)(void*, const char*, size_t), const char*, ...)
	__attribute__((__format__ (printf, 3, 4)));
int cbscanf(void*,
            int (*)(void*),
            int (*)(int, void*),
            const char* __restrict,
            ...)
	__attribute__((__format__ (scanf, 4, 5)));
int vcbprintf(void*,
              size_t (*)(void*, const char*, size_t),
              const char*,
              __gnuc_va_list)
	__attribute__((__format__ (printf, 3, 0)));
int vcbscanf(void*,
             int (*)(void*),
             int (*)(int, void*),
             const char* __restrict,
             __gnuc_va_list)
	__attribute__((__format__ (scanf, 4, 0)));
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#if defined(__is_sortix_libc)
#include <FILE.h>
#endif

#endif
