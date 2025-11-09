/*
 * Copyright (c) 2014, 2016, 2017, 2024, 2025 Jonas 'Sortie' Termansen.
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
 * sortix/limits.h
 * System limits.
 */

#ifndef _INCLUDE_SORTIX_LIMITS_H
#define _INCLUDE_SORTIX_LIMITS_H

#include <sys/cdefs.h>

#include <sortix/__/sigset.h>

/* After releasing Sortix 1.1, remove this undef for gcc 5.2.0 compatibility. */
#undef MB_LEN_MAX
#define MB_LEN_MAX 4

#if __USE_SORTIX || __USE_POSIX

#define WORD_BIT __INT_WIDTH__
#define LONG_BIT __LONG_WIDTH__
#if !defined(SSIZE_MAX) /* Also in <sys/types.h> */
#define SSIZE_MAX __INTPTR_MAX__ /* TODO: __SSIZE_MAX__ */
#endif

/*#undef AIO_LISTIO_MAX*/
/*#undef AIO_MAX*/
/*#undef AIO_PRIO_DELTA_MAX*/
/*#undef ARG_MAX*/
/*#undef ATEXIT_MAX*/
/*#undef CHILD_MAX*/
/*#undef DELAYTIMER_MAX*/ /* TODO: True? */
#define HOST_NAME_MAX 255
#define IOV_MAX 1024
/*#undef LOGIN_NAME_MAX*/
/*#undef MQ_OPEN_MAX*/
/*#undef MQ_PRIO_MAX*/
/*#undef OPEN_MAX*/
#if defined(__x86_64__) || defined(__i386__)
#define PAGESIZE 4096
#else
#warning "You should define PAGESIZE for this architecture"
/*#undef PAGESIZE*/
#endif
#if (__USE_SORTIX || __USE_XOPEN) && defined(PAGESIZE)
#define PAGE_SIZE PAGESIZE
#endif
/*#undef PTHREAD_DESTRUCTOR_ITERATIONS*/
/*#undef PTHREAD_KEYS_MAX*/
#define PTHREAD_STACK_MIN PAGESIZE
/*#undef PTHREAD_THREADS_MAX*/
#define RTSIG_MAX (/*SIGRTMAX*/ 127 + 1 - /*SIGRTMIN*/ 64)
/*#undef SEM_NSEMS_MAX*/
#define SEM_VALUE_MAX INT_MAX
#define SIGQUEUE_MAX 1 /* TODO: Implement real time signals */
/*#undef SS_REPL_MAX*/
/*#undef STREAM_MAX*/
#define SYMLOOP_MAX 20
#define TIMER_MAX 32
#define TTY_NAME_MAX 32
#define TZNAME_MAX 6

#define FILESIZEBITS 64
/*#undef LINK_MAX*/
/*#undef MAX_CANON*/
/*#undef NAME_MAX*/
/*#undef PATH_MAX*/
/*#undef PIPE_BUF*/ /* TODO: Implement PIPE_BUF semantics */
/*#undef POSIX_ALLOC_SIZE_MIN*/
/*#undef POSIX_REC_INCR_XFER_SIZE*/
/*#undef POSIX_REC_MAX_XFER_SIZE*/
/*#undef POSIX_REC_MIN_XFER_SIZE*/
/*#undef POSIX_REC_XFER_ALIGN*/
/*#undef SYMLINK_MAX*/
/*#undef TEXTDOMAIN_MAX*/

#define BC_BASE_MAX 99
#define BC_DIM_MAX 2048
#define BC_SCALE_MAX 99
#define BC_STRING_MAX 1000
#define CHARCLASS_NAME_MAX 14
#define COLL_WEIGHTS_MAX 2
#define EXPR_NEST_MAX INT_MAX
#define LINE_MAX 4096
#define NGROUPS_MAX 32
#define RE_DUP_MAX INT_MAX

#define _POSIX_CLOCKRES_MIN 20000000

#define _POSIX_AIO_LISTIO_MAX 2
#define _POSIX_AIO_MAX 1
#define _POSIX_ARG_MAX 4096
#define _POSIX_CHILD_MAX 25
#define _POSIX_DELAYTIMER_MAX 32
#define _POSIX_HOST_NAME_MAX 255
#define _POSIX_LINK_MAX 8
#define _POSIX_LOGIN_NAME_MAX 9
#define _POSIX_MAX_CANON 255
#define _POSIX_MAX_INPUT 255
#define _POSIX_MQ_OPEN_MAX 8
#define _POSIX_MQ_PRIO_MAX 32
#define _POSIX_NAME_MAX 14
#define _POSIX_NGROUPS_MAX 8
#define _POSIX_OPEN_MAX 20
#define _POSIX_PATH_MAX 256
#define _POSIX_PIPE_BUF 512
#define _POSIX_RE_DUP_MAX 255
#define _POSIX_RTSIG_MAX 8
#define _POSIX_SEM_NSEMS_MAX 8
#define _POSIX_SEM_VALUE_MAX 32767
#define _POSIX_SIGQUEUE_MAX 32
#define _POSIX_SSIZE_MAX 32767
#define _POSIX_SS_REPL_MAX 32
#define _POSIX_STREAM_MAX 8
#define _POSIX_SYMLINK_MAX 255
#define _POSIX_SYMLOOP_MAX 8
#define _POSIX_THREAD_DESTRUCTOR_ITERATIONS 4
#define _POSIX_THREAD_KEYS_MAX 128
#define _POSIX_THREAD_THREADS_MAX 64
#define _POSIX_TIMER_MAX 32
#define _POSIX_TTY_NAME_MAX 9
#define _POSIX_TZNAME_MAX 6
#define _POSIX2_BC_BASE_MAX 99
#define _POSIX2_BC_DIM_MAX 99
#define _POSIX2_BC_SCALE_MAX 99
#define _POSIX2_BC_STRING_MAX 1000
#define _POSIX2_CHARCLASS_NAME_MAX 14
#define _POSIX2_COLL_WEIGHTS_MAX 2
#define _POSIX2_EXPR_NEST_MAX 32
#define _POSIX2_LINE_MAX 2048
#define _POSIX2_RE_DUP_MAX 255
#if __USE_SORTIX || __USE_XOPEN
#define _XOPEN_IOV_MAX 16
#define _XOPEN_NAME_MAX 255
#define _XOPEN_PATH_MAX 1024
#endif

#define NL_ARGMAX 0 /* TODO: Implement printf/scanf %n$, minimum 9 */
#if __USE_SORTIX || __USE_XOPEN
#define NL_LANGMAX 32
#endif
#define NL_MSGMAX 32767
#define NL_SETMAX 255
#define NL_TEXTMAX 2048
#if __USE_SORTIX || __USE_XOPEN
#define NZERO 20
#endif

#endif

#if __USE_SORTIX || 202405L <= __USE_POSIX
#define GETENTROPY_MAX 256
#define NSIG_MAX __SIGSET_NUM_SIGNALS
#endif

#endif
