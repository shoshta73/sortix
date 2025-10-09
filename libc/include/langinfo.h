/*
 * Copyright (c) 2016, 2025 Jonas 'Sortie' Termansen.
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
 * langinfo.h
 * Language information constants.
 */

#ifndef _INCLUDE_LANGINFO_H
#define _INCLUDE_LANGINFO_H

#ifndef __locale_t_defined
#define __locale_t_defined
typedef struct __locale* locale_t;
#endif

#ifndef __nl_item_defined
#define __nl_item_defined
typedef int nl_item;
#endif

#define CODESET 1
#define D_T_FMT 2
#define D_FMT 3
#define T_FMT 4
#define T_FMT_AMPM 5
#define AM_STR 6
#define PM_STR 7
#define DAY_1 8
#define DAY_2 9
#define DAY_3 10
#define DAY_4 11
#define DAY_5 12
#define DAY_6 13
#define DAY_7 14
#define ABDAY_1 15
#define ABDAY_2 16
#define ABDAY_3 17
#define ABDAY_4 18
#define ABDAY_5 19
#define ABDAY_6 20
#define ABDAY_7 21
#define MON_1 22
#define MON_2 23
#define MON_3 24
#define MON_4 25
#define MON_5 26
#define MON_6 27
#define MON_7 28
#define MON_8 29
#define MON_9 30
#define MON_10 31
#define MON_11 32
#define MON_12 33
#define ABMON_1 34
#define ABMON_2 35
#define ABMON_3 36
#define ABMON_4 37
#define ABMON_5 38
#define ABMON_6 39
#define ABMON_7 40
#define ABMON_8 41
#define ABMON_9 42
#define ABMON_10 43
#define ABMON_11 44
#define ABMON_12 45
#define ERA 46
#define ERA_D_FMT 47
#define ERA_D_T_FMT 48
#define ERA_T_FMT 49
#define ALT_DIGITS 50
#define RADIXCHAR 51
#define THOUSEP 52
#define YESEXPR 53
#define NOEXPR 54
#define CRNCYSTR 55
#define ALTMON_1 56
#define ALTMON_2 57
#define ALTMON_3 58
#define ALTMON_4 59
#define ALTMON_5 60
#define ALTMON_6 61
#define ALTMON_7 62
#define ALTMON_8 63
#define ALTMON_9 64
#define ALTMON_10 65
#define ALTMON_11 66
#define ALTMON_12 67
#define ABALTMON_1 69
#define ABALTMON_2 70
#define ABALTMON_3 71
#define ABALTMON_4 72
#define ABALTMON_5 73
#define ABALTMON_6 74
#define ABALTMON_7 75
#define ABALTMON_8 76
#define ABALTMON_9 77
#define ABALTMON_10 78
#define ABALTMON_11 79
#define ABALTMON_12 80

#ifdef __cplusplus
extern "C" {
#endif

char* nl_langinfo(nl_item);
char* nl_langinfo_l(nl_item, locale_t);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
