/*
 * Copyright (c) 2014, 2022 Jonas 'Sortie' Termansen.
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
 * syslog/vsyslog.c
 * Logs an event to the system log.
 */

#include <sys/types.h>

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

// TODO: After releasing Sortix 1.1, remove this bootstrap compatibility.
#if !defined(HOST_NAME_MAX) && defined(__sortix__)
#include <sortix/limits.h>
#endif

char* __syslog_identity = NULL;
int __syslog_facility = LOG_USER;
int __syslog_mask = LOG_UPTO(LOG_DEBUG);
int __syslog_option = 0;

void vsyslog(int priority, const char* format, va_list ap)
{
	// Drop the event if it doesn't fit the current priority mask.
	if ( !(LOG_MASK(LOG_PRI(priority)) & __syslog_mask) )
		return;

	// If no facility is given we'll use the default facility from openlog.
	if ( !LOG_FAC(priority) )
		priority |= __syslog_facility;

	// Gather the log event metadata.
	int version = 1; // RFC 5424
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	struct tm tm;
	gmtime_r(&now.tv_sec, &tm);
	char timeformat[64];
	snprintf(timeformat, sizeof(timeformat),
	         "%%FT%%T.%06liZ", now.tv_nsec / 1000);
	char timestamp[64];
	strftime(timestamp, sizeof(timestamp), timeformat, &tm);
	char hostname[HOST_NAME_MAX + 1] = "-";
	gethostname(hostname, sizeof(hostname));
	const char* identity = __syslog_identity ? __syslog_identity : "-";
	char pidstr[3 * sizeof(pid_t)] = "-";
	if ( __syslog_option & LOG_PID )
		snprintf(pidstr, sizeof(pidstr), "%"PRIdPID, getpid());
	const char* msgid = "-";
	const char* structured_data = "-";

	// Transmit the event to the system log.
	flockfile(stderr);
	fprintf(stderr, "<%d>%d %s %s %s %s %s %s ",
	                 priority,
	                 version,
	                 timestamp,
	                 hostname,
	                 identity,
	                 pidstr,
	                 msgid,
	                 structured_data);
	vfprintf(stderr, format, ap);
	fputc('\n', stderr);
	funlockfile(stderr);
}
