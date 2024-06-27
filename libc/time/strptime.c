/*
 * Copyright (c) 2023, 2024 Jonas 'Sortie' Termansen.
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
 * time/strptime.c
 * Parse date and time.
 */

#include <ctype.h>
#include <limits.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

static const char* wdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                              "Thursday", "Friday", "Saturday", NULL};
static const char* months[] = {"January", "February", "March", "April", "May",
                               "June", "July", "August", "September", "October",
                               "November", "December", NULL};

static const char* strptime_str(const char* str,
                                int* output,
                                const char* const* list)
{
	for ( int i = 0; list[i]; i++ )
	{
		size_t len = strlen(list[i]);
		if ( !strncasecmp(str, list[i], len) )
			return *output = i, str + len;
		if ( !strncasecmp(str, list[i], 3) )
			return *output = i, str + 3;
	}
	return NULL;
}

static const char* strptime_num(const char* str,
                                int* output,
                                int minimum,
                                int maximum,
                                size_t min_digits,
                                size_t max_digits,
                                int offset)
{
	int value = 0;
	size_t i = 0;
	while ( i < max_digits )
	{
		if ( str[i] < '0'|| '9' < str[i] )
		{
			if ( min_digits <= i )
				break;
			return NULL;
		}
		if ( __builtin_mul_overflow(value, 10, &value) ||
		     __builtin_add_overflow(value, str[i++] - '0', &value) )
			return NULL;
	}
	str += i;
	if ( value < minimum || maximum < value  )
		return NULL;
	if ( __builtin_add_overflow(value, offset, &value) )
		return NULL;
	return *output = value, str;
}

char* strptime(const char* restrict str,
               const char* restrict format,
               struct tm* restrict tm)
{
	bool pm = false;
	bool need_mktime = false;
	int year_high = -1, year_low = -1, dummy;
	for ( size_t i = 0; format[i]; )
	{
		if ( isspace((unsigned char) format[i]) )
		{
			do i++;
			while ( isspace((unsigned char) format[i]) );
			if ( !isspace((unsigned char) *str) )
				return NULL;
			do str++;
			while ( isspace((unsigned char) *str) );
			continue;
		}
		else if ( format[i] != '%' )
		{

			if ( format[i++] != *str++ )
				return NULL;
			continue;
		}
		i++;
		if ( format[i] == '0' || format[i] == '+'  )
			i++;
		bool has_width = '0' <= format[i] && format[i] <= '9';
		size_t width = 0;
		while ( '0' <= format[i] && format[i] <= '9' )
			width = width * 10 + (format[i++] - '0');
		// TODO: Maximum width.
		bool modifier_E = false, modifier_O = false;
		if ( format[i] == 'E' )
			modifier_E = true, i++;
		else if ( format[i] == 'O' )
			modifier_O = true, i++;
		(void) modifier_E, (void) modifier_O;
		switch ( format[i] )
		{
		case 'a':
		case 'A': str = strptime_str(str, &tm->tm_wday, wdays); break;
		case 'b':
		case 'B':
		case 'h':
			str = strptime_str(str, &tm->tm_mon, months);
			need_mktime = true;
			break;
		case 'c': str = strptime(str, "%a %b %e %H:%M:%S %Y", tm); break;
		case 'C': str = strptime_num(str, &year_high, 0, 99, 1, 2, 0); break;
		case 'd':
		case 'e':
			str = strptime_num(str, &tm->tm_mday, 1, 31, 1, 2, 0);
			need_mktime = true;
			break;
		case 'D': str = strptime(str, "%m/%d/%y", tm); break;
		case 'F': str = strptime(str, "%Y-%m-%d", tm); break;
		case 'g': str = strptime_num(str, &dummy, 0, 99, 1, 2, 0); break;
		case 'G':
			// POSIX divergence: Avoid year 10k problem by allowing more than
			// four characters by default.
			if ( !has_width )
				width = SIZE_MAX;
			// TODO: Allow leading + and - per POSIX.
			str = strptime_num(str, &dummy, INT_MIN, INT_MAX, 1, width, -1900);
			break;
		case 'H': str = strptime_num(str, &tm->tm_hour, 0, 23, 1, 2, 0); break;
		case 'I':
			str = strptime_num(str, &tm->tm_hour, 1, 12, 1, 2, 0);
			if ( tm->tm_hour == 12 )
				tm->tm_hour = 0;
			break;
		case 'j': str = strptime_num(str, &tm->tm_yday, 1, 366, 3, 3, -1); break;
		case 'm':
			str = strptime_num(str, &tm->tm_mon, 1, 12, 1, 2, -1);
			need_mktime = true;
			break;
		case 'M': str = strptime_num(str, &tm->tm_min, 0, 59, 1, 2, 0); break;
		case 'n':
		case 't':
			if ( !isspace((unsigned char) *str) )
				return NULL;
			do str++;
			while ( isspace((unsigned char) *str) );
			break;
		case 'p':
			if ( !strncasecmp(str, "am", 2) )
				str += 2, pm = false;
			else if ( !strncasecmp(str, "pm", 2) )
				str += 2, pm = true;
			else
				return NULL;
			break;
		case 'r': str = strptime(str, "%I:%M:%S %p", tm); break;
		case 'R': str = strptime(str, "%H:%M", tm); break;
		case 's':
			// TODO: What timezone should be used to for this conversion? Use
			//       the timezone in %z and %Z if set?
			if ( !(str[0] == '-' || ('0' <= str[0] && str[0] <= '9')) )
				return 0;
			char* end;
			intmax_t value = strtoimax(str, &end, 10);
			time_t timestamp = (time_t) value;
			if ( value != timestamp )
				return NULL;
			localtime_r(&timestamp, tm);
			str = (const char*) end;
			break;
		case 'S': str = strptime_num(str, &tm->tm_sec, 0, 60, 1, 2, 0); break;
		case 'T': str = strptime(str, "%H:%M:%S", tm); break;
		case 'u':
			str = strptime_num(str, &tm->tm_wday, 1, 7, 1, 1, 0);
			if ( tm->tm_wday == 7 )
				tm->tm_wday = 0;
			break;
		case 'V': str = strptime_num(str, &dummy, 0, 53, 1, 2, 0); break;
		case 'w': str = strptime_num(str, &tm->tm_wday, 0, 6, 1, 1, 0); break;
		case 'W': str = strptime_num(str, &dummy, 0, 53, 1, 2, 0); break;
		case 'x': str = strptime(str, "%m/%d/%Y", tm); break;
		case 'X': str = strptime(str, "%H:%M:%S", tm); break;
		case 'y': str = strptime_num(str, &year_low, 0, 99, 1, 2, 0); break;
		case 'Y':
			// POSIX divergence: Avoid year 10k problem by allowing more than
			// four characters by default.
			if ( !has_width )
				width = SIZE_MAX;
			// TODO: Allow leading + and - per POSIX.
			str = strptime_num(str, &tm->tm_year, INT_MIN, INT_MAX, 1, width,
			                   -1900);
			need_mktime = true;
			break;
		case 'z':
			// TODO: More exact.
			if ( *str != '-' || *str != '+' )
				return NULL;
			int hours, minutes;
			if ( !(str = strptime_num(str, &hours, -12, 12, 2, 2, 0)) ||
			     !(str = strptime_num(str, &minutes, 0, 59, 2, 2, 0)) )
				return NULL;
			tm->tm_isdst = 0;
			// TODO: What is done with this timezone information?
			break;
		case 'Z':
			// TODO: Other timezones.
			if ( strncmp(str, "UTC", 3) != 0 )
				return NULL;
			str += 3;
			tm->tm_isdst = 0;
			// TODO: What is done with this timezone information?
			break;
		case '%':
			if ( *str++ != '%' )
				return NULL;
			break;
		default: NULL;
		}
		if ( !str )
			return NULL;
		i++;
	}
	if ( str )
	{
		if ( pm )
			tm->tm_hour += 12;
		if ( year_high != -1 || year_low != -1 )
		{
			if ( year_high == -1 )
				year_high = year_low < 70 ? 20 : 19;
			else if ( year_low == -1 )
				year_low = 0;
			tm->tm_year = year_high * 100 + year_low - 1900;
			need_mktime = true;
		}
		if ( need_mktime )
		{
			struct tm copy = *tm;
			mktime(&copy);
			tm->tm_wday = copy.tm_wday;
			tm->tm_yday = copy.tm_yday;
		}
	}
	return (char*) str;
}
