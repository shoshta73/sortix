/*
 * Copyright (c) 2012, 2024, 2025 Jonas 'Sortie' Termansen.
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
 * poll.h
 * Input/output multiplexing.
 */

#ifndef _INCLUDE_POLL_H
#define _INCLUDE_POLL_H

#include <sys/cdefs.h>

#include <sys/__/types.h>

#include <sortix/poll.h>

#if __USE_SORTIX || 202405L <= __USE_POSIX
#include <sortix/sigset.h>
#include <sortix/timespec.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

int poll(struct pollfd*, nfds_t, int);

#if __USE_SORTIX || 202405L <= __USE_POSIX
int ppoll(struct pollfd*, nfds_t, const struct timespec*, const sigset_t*);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
