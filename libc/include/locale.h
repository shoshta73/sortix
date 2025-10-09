/*
 * Copyright (c) 2012, 2015, 2017, 2025 Jonas 'Sortie' Termansen.
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
 * locale.h
 * Category macros.
 */

#ifndef _INCLUDE_LOCALE_H
#define _INCLUDE_LOCALE_H

#include <sys/cdefs.h>

#ifndef NULL
#define __need_NULL
#include <stddef.h>
#endif

struct lconv
{
	char* decimal_point;
	char* thousands_sep;
	char* grouping;
	char* int_curr_symbol;
	char* currency_symbol;
	char* mon_decimal_point;
	char* mon_thousands_sep;
	char* mon_grouping;
	char* positive_sign;
	char* negative_sign;
	char int_frac_digits;
	char frac_digits;
	char p_cs_precedes;
	char n_cs_precedes;
	char p_sep_by_space;
	char n_sep_by_space;
	char p_sign_posn;
	char n_sign_posn;
	char int_p_cs_precedes;
	char int_n_cs_precedes;
	char int_p_sep_by_space;
	char int_n_sep_by_space;
	char int_p_sign_posn;
	char int_n_sign_posn;
};

#define LC_COLLATE 0
#define LC_CTYPE 1
#if __USE_SORTIX || __USE_POSIX
#define LC_MESSAGES 2
#endif
#define LC_MONETARY 3
#define LC_NUMERIC 4
#define LC_TIME 5
#define LC_ALL 6

#if __USE_SORTIX || __USE_POSIX
#define LC_COLLATE_MASK (1 << LC_COLLATE)
#define LC_CTYPE_MASK (1 << LC_CTYPE)
#define LC_MESSAGES_MASK (1 << LC_MESSAGES)
#define LC_MONETARY_MASK (1 << LC_MONETARY)
#define LC_NUMERIC_MASK (1 << LC_NUMERIC)
#define LC_TIME_MASK (1 << LC_TIME)
#define LC_ALL_MASK ((1 << LC_ALL) - 1)
#define LC_GLOBAL_LOCALE (&__global_locale)
#endif

#if defined(__is_sortix_libc)
struct __locale
{
	char* current[LC_ALL];
};
#endif

#if __USE_SORTIX || __USE_POSIX
#ifndef __locale_t_defined
#define __locale_t_defined
typedef struct __locale* locale_t;
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern struct __locale __global_locale;
extern __thread struct __locale* __locale;

char* setlocale(int, const char*);
struct lconv* localeconv(void);

#if __USE_SORTIX || 200809L <= __USE_POSIX
locale_t duplocale(locale_t);
void freelocale(locale_t);
locale_t newlocale(int, const char*, locale_t);
locale_t uselocale(locale_t);
#endif

#if __USE_SORTIX || 202405L <= __USE_POSIX
const char* getlocalename_l(int, locale_t);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
