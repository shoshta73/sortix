/*
 * Copyright (c) 2013, 2025 Jonas 'Sortie' Termansen.
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
 * endian.h
 * Convert byte ordering of integers.
 */

#ifndef _INCLUDE_ENDIAN_H
#define _INCLUDE_ENDIAN_H

#include <sys/cdefs.h>

#include <__/endian.h>

#include <stdint.h>

/* Constans for each kind of known endian. */
#define LITTLE_ENDIAN __LITTLE_ENDIAN
#define PDP_ENDIAN __PDP_ENDIAN
#define BIG_ENDIAN __BIG_ENDIAN

/* Easy access to the current endian. */
#define BYTE_ORDER __BYTE_ORDER

/* Easy conversion of 16-bit integers. */
#define htobe16(x) __htobe16(x)
#define htole16(x) __htole16(x)
#define be16toh(x) __be16toh(x)
#define le16toh(x) __le16toh(x)

/* Easy conversion of 32-bit integers. */
#define htobe32(x) __htobe32(x)
#define htole32(x) __htole32(x)
#define be32toh(x) __be32toh(x)
#define le32toh(x) __le32toh(x)

/* Easy conversion of 64-bit integers. */
#define htobe64(x) __htobe64(x)
#define htole64(x) __htole64(x)
#define be64toh(x) __be64toh(x)
#define le64toh(x) __le64toh(x)

/* Sortix specific extensions only available in C++. */
#if defined(__cplusplus) && __USE_SORTIX

/* Create big-endian versions of the stdint.h exact size data types. */
typedef __big_uint8_t big_uint8_t;
typedef __big_uint16_t big_uint16_t;
typedef __big_uint32_t big_uint32_t;
typedef __big_uint64_t big_uint64_t;

/* Create little-endian versions of the stdint.h exact size data types. */
typedef __little_uint8_t little_uint8_t;
typedef __little_uint16_t little_uint16_t;
typedef __little_uint32_t little_uint32_t;
typedef __little_uint64_t little_uint64_t;

#endif

#endif
