/*
 * Copyright (c) 2016 Jonas 'Sortie' Termansen.
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
 * connection.c
 * IRC protocol.
 */

#include <sys/socket.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "compat.h"
#include "connection.h"

void dump_error(const char* message,
                size_t message_size,
                const struct timespec* when)
{
	// TODO: Send the error somewhere appropriate in the UI.

	fprintf(stderr, "\e[91m");
	struct tm tm;
	gmtime_r(&when->tv_sec, &tm);
	fprintf(stderr, "[%i-%02i-%02i %02i:%02i:%02i %09li] ",
	                tm.tm_year + 1900,
	                tm.tm_mon + 1,
	                tm.tm_mday,
	                tm.tm_hour,
	                tm.tm_min,
	                tm.tm_sec,
	                when->tv_nsec);
	for ( size_t i = 0; i < message_size; i++ )
	{
		if ( message[i] == '\r' )
			continue;
		else if ( message[i] == '\n' )
			continue;
		else if ( (unsigned char) message[i] < 32 )
		{
			fprintf(stderr, "\e[31m");
			fprintf(stderr, "\\x%02X", (unsigned char) message[i]);
			fprintf(stderr, "\e[91m");
		}
		fputc((unsigned char) message[i], stderr);
	}
	fprintf(stderr, "\e[m\n");
}

void dump_outgoing(const char* message,
                   size_t message_size,
                   const struct timespec* when)
{
	return; // TODO: Remove this, or adopt it for a logging mechanism.

	fprintf(stderr, "\e[92m");
	struct tm tm;
	gmtime_r(&when->tv_sec, &tm);
	fprintf(stderr, "[%i-%02i-%02i %02i:%02i:%02i %09li] ",
	                tm.tm_year + 1900,
	                tm.tm_mon + 1,
	                tm.tm_mday,
	                tm.tm_hour,
	                tm.tm_min,
	                tm.tm_sec,
	                when->tv_nsec);
	for ( size_t i = 0; i < message_size; i++ )
	{
		if ( message[i] == '\r' )
			continue;
		else if ( message[i] == '\n' )
			continue;
		else if ( (unsigned char) message[i] < 32 )
		{
			fprintf(stderr, "\e[91m");
			fprintf(stderr, "\\x%02X", (unsigned char) message[i]);
			fprintf(stderr, "\e[92m");
			continue;
		}
		fputc((unsigned char) message[i], stderr);
	}
	fprintf(stderr, "\e[m\n");
}

void dump_incoming(const char* message,
                   size_t message_size,
                   const struct timespec* when)
{
	return; // TODO: Remove this, or adopt it for a logging mechanism.

	fprintf(stderr, "\e[93m");
	struct tm tm;
	gmtime_r(&when->tv_sec, &tm);
	fprintf(stderr, "[%i-%02i-%02i %02i:%02i:%02i %09li] ",
	                tm.tm_year + 1900,
	                tm.tm_mon + 1,
	                tm.tm_mday,
	                tm.tm_hour,
	                tm.tm_min,
	                tm.tm_sec,
	                when->tv_nsec);
	for ( size_t i = 0; i < message_size; i++ )
	{
		if ( message[i] == '\r' )
			continue;
		else if ( message[i] == '\n' )
			continue;
		else if ( (unsigned char) message[i] < 32 )
		{
			fprintf(stderr, "\e[91m");
			fprintf(stderr, "\\x%02X", (unsigned char) message[i]);
			fprintf(stderr, "\e[93m");
			continue;
		}
		fputc((unsigned char) message[i], stderr);
	}
	fprintf(stderr, "\e[m\n");
}

void irc_error_vlinef(const char* format, va_list ap_orig)
{
	va_list ap;

	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);

	va_copy(ap, ap_orig);
	char* string;
	if ( 0 <= vasprintf(&string, format, ap) )
	{
		va_end(ap);
		dump_error(string, strlen(string), &now);
		free(string);
		return;
	}
	va_end(ap);

	char buffer[512];
	va_copy(ap, ap_orig);
	if ( 0 <= vsnprintf(buffer, sizeof(buffer), format, ap) )
	{
		va_end(ap);
		dump_error(buffer, strlen(buffer), &now);
		dump_error("(vasprintf failed printing that line)",
		           strlen("(vasprintf failed printing that line)"), &now);
		return;
	}
	va_end(ap);

	dump_error(format, strlen(format), &now);
	dump_error("(vsnprintf failed printing format string)",
	           strlen("(vsnprintf failed printing that format string)"), &now);
}

void irc_error_linef(const char* format, ...)
{
	va_list ap;
	va_start(ap, format);
	irc_error_vlinef(format, ap),
	va_end(ap);
}

void irc_transmit(struct irc_connection* irc_connection,
                  const char* message,
                  size_t message_size)
{
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	if ( irc_connection->connectivity_error )
		return;
	dump_outgoing(message, message_size, &now);
	int fd = irc_connection->fd;
	while ( message_size )
	{
		ssize_t amount = 0;
		amount = send(fd, message, message_size, MSG_NOSIGNAL);
		if ( amount < 0 || amount == 0 )
		{
			warn("send");
			irc_connection->connectivity_error = true;
			return;
		}
		message += amount;
		message_size -= amount;
	}
}

void irc_transmit_message(struct irc_connection* irc_connection,
                          const char* message,
                          size_t message_size)
{
	assert(2 <= message_size);
	assert(message[message_size - 2] == '\r');
	assert(message[message_size - 1] == '\n');

	char buffer[512];
	if ( 512 < message_size )
	{
		memcpy(buffer, message, 510);
		buffer[510] = '\r';
		buffer[511] = '\n';
		message = buffer;
		message_size = 512;
	}

	irc_transmit(irc_connection, message, message_size);

	explicit_bzero(buffer, sizeof(buffer));
}

void irc_receive_more_bytes(struct irc_connection* irc_connection)
{
	if ( irc_connection->connectivity_error )
		return;
	int fd = irc_connection->fd;
	char* buffer = irc_connection->incoming_buffer;
	size_t buffer_size = sizeof(irc_connection->incoming_buffer);
	size_t buffer_used = irc_connection->incoming_amount;
	size_t buffer_free = buffer_size - buffer_used;
	if ( buffer_free == 0 )
		return;
	// TODO: Use non-blocking IO for transmitting as well so O_NONBLOCK can
	//       always be used.
	// TODO: Use MSG_DONTWAIT when supported in Sortix.
	int flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	ssize_t amount = recv(fd, buffer + buffer_used, buffer_free, 0);
	fcntl(fd, F_SETFL, flags);
	if ( amount < 0 )
	{
		if ( errno == EAGAIN || errno == EWOULDBLOCK )
			return;
		warn("recv");
		irc_connection->connectivity_error = true;
	}
	else if ( amount == 0 )
	{
		// TODO: Gracefully close the connection.
		irc_connection->connectivity_error = true;
	}
	else
	{
		irc_connection->incoming_amount += amount;
	}
}

void irc_receive_pop_bytes(struct irc_connection* irc_connection,
                            char* buffer,
                            size_t count)
{
	assert(count <= irc_connection->incoming_amount);
	memcpy(buffer, irc_connection->incoming_buffer, count);
	explicit_bzero(irc_connection->incoming_buffer, count);
	memmove(irc_connection->incoming_buffer,
	        irc_connection->incoming_buffer + count,
	        irc_connection->incoming_amount - count);
	irc_connection->incoming_amount -= count;
	explicit_bzero(irc_connection->incoming_buffer + irc_connection->incoming_amount,
	               count);
}

bool irc_receive_message(struct irc_connection* irc_connection,
                         char message[512],
                         struct timespec* when)
{
	if ( irc_connection->connectivity_error )
		return false;
	size_t message_usable = 0;
	while ( message_usable < irc_connection->incoming_amount &&
	        irc_connection->incoming_buffer[message_usable] != '\r' &&
	        irc_connection->incoming_buffer[message_usable] != '\n' )
		message_usable++;
	if ( message_usable < irc_connection->incoming_amount &&
	     irc_connection->incoming_buffer[message_usable] == '\r')
	{
		message_usable++;
		if ( message_usable < irc_connection->incoming_amount &&
		     irc_connection->incoming_buffer[message_usable] == '\n' )
		{
			message_usable++;
			irc_receive_pop_bytes(irc_connection, message, message_usable);
			assert(message[message_usable-2] == '\r');
			assert(message[message_usable-1] == '\n');
			message[message_usable-2] = '\0';
			message[message_usable-1] = '\0';
			// TODO: This is not always when the message did arrive.
			struct timespec now;
			clock_gettime(CLOCK_REALTIME, &now);
			dump_incoming(message, message_usable-2, &now);
			*when = now;
			return true;
		}
		else if ( message_usable < irc_connection->incoming_amount )
		{
			// TODO: Handle bad newline sequence.
			warnx("recv: bad IRC newline");
			irc_connection->connectivity_error = true;
			return false;
		}
	}
	else if ( message_usable < irc_connection->incoming_amount &&
	     irc_connection->incoming_buffer[message_usable] == '\n' )
	{
		// TODO: Handle bad newline sequence.
		warnx("recv: bad IRC newline");
		irc_connection->connectivity_error = true;
		return false;
	}
	if ( message_usable == 512 )
	{
		// TODO: Handle untruncated lines from the server.
		warnx("recv: overlong IRC line from server");
		irc_connection->connectivity_error = true;
		return false;
	}
	return false;
}

void irc_transmit_string(struct irc_connection* irc_connection,
                         const char* string)
{
	char message[512];
	strncpy(message, string, 510);
	message[510] = '\0';
	message[511] = '\0';
	size_t string_truncated_length = strlen(message);
	for ( size_t i = 0; i < string_truncated_length; i++ )
	{
		if ( message[i] == '\r' )
			message[i] = ' ';
		if ( message[i] == '\n' )
			message[i] = ' ';
	}
	message[string_truncated_length + 0] = '\r';
	message[string_truncated_length + 1] = '\n';
	size_t message_length = strnlen(message, 512);
	irc_transmit_message(irc_connection, message, message_length);
	explicit_bzero(message, sizeof(message));
}

__attribute__((format(printf, 2, 0)))
void irc_transmit_vformat(struct irc_connection* irc_connection,
                         const char* format,
                         va_list ap)
{
	char* string = NULL;
	if ( vasprintf(&string, format, ap) < 0 )
	{
		// TODO: Hmm, what do we do here.
		warn("vasprintf");
		// TODO: Should the error condition be set?
		return;
	}
	irc_transmit_string(irc_connection, string);
	explicit_bzero(string, strlen(string));
	free(string);
}

__attribute__((format(printf, 2, 3)))
void irc_transmit_format(struct irc_connection* irc_connection,
                         const char* format,
                         ...)
{
	va_list ap;
	va_start(ap, format);
	irc_transmit_vformat(irc_connection, format, ap);
	va_end(ap);
}

void irc_command_pass(struct irc_connection* irc_connection,
                      const char* password)
{
	irc_transmit_format(irc_connection, "PASS :%s", password);
}

void irc_command_nick(struct irc_connection* irc_connection,
                      const char* nick)
{
	irc_transmit_format(irc_connection, "NICK :%s", nick);
}

void irc_command_user(struct irc_connection* irc_connection,
                      const char* nick,
                      const char* local_hostname,
                      const char* server_hostname,
                      const char* real_name)
{
	// TODO: What if there are spaces in some of these fields?
	irc_transmit_format(irc_connection, "USER %s %s %s :%s",
	                    nick, local_hostname, server_hostname, real_name);
}

void irc_command_join(struct irc_connection* irc_connection,
                      const char* channel)
{
	irc_transmit_format(irc_connection, "JOIN :%s", channel);
}

void irc_command_part(struct irc_connection* irc_connection,
                      const char* channel)
{
	irc_transmit_format(irc_connection, "PART :%s", channel);
}

void irc_command_privmsg(struct irc_connection* irc_connection,
                         const char* where,
                         const char* what)

{
	// TODO: Ensure where is valid.
	irc_transmit_format(irc_connection, "PRIVMSG %s :%s", where, what);
}

void irc_command_privmsgf(struct irc_connection* irc_connection,
                          const char* where,
                          const char* what_format,
                          ...)

{
	va_list ap;
	va_start(ap, what_format);
	char msg[512];
	vsnprintf(msg, sizeof(msg), what_format, ap);
	irc_command_privmsg(irc_connection, where, msg);
	va_end(ap);
}

void irc_command_notice(struct irc_connection* irc_connection,
                        const char* where,
                        const char* what)

{
	// TODO: Ensure where is valid.
	irc_transmit_format(irc_connection, "NOTICE %s :%s", where, what);
}

void irc_command_noticef(struct irc_connection* irc_connection,
                         const char* where,
                         const char* what_format,
                         ...)

{
	va_list ap;
	va_start(ap, what_format);
	char msg[512];
	vsnprintf(msg, sizeof(msg), what_format, ap);
	irc_command_notice(irc_connection, where, msg);
	va_end(ap);
}

void irc_command_kick(struct irc_connection* irc_connection,
                      const char* where,
                      const char* who,
                      const char* why)
{
	// TODO: Ensure where and who are valid.
	if ( why )
		irc_transmit_format(irc_connection, "KICK %s %s :%s", where, who, why);
	else
		irc_transmit_format(irc_connection, "KICK %s %s", where, who);
}

void irc_command_quit(struct irc_connection* irc_connection,
                      const char* message)
{
	if ( message )
		irc_transmit_format(irc_connection, "QUIT :%s", message);
	else
		irc_transmit_string(irc_connection, "QUIT");
	shutdown(irc_connection->fd, SHUT_WR);
}

void irc_command_quit_malfunction(struct irc_connection* irc_connection,
                                  const char* message)
{
	if ( message )
		irc_transmit_format(irc_connection, "QUIT :%s", message);
	else
		irc_transmit_string(irc_connection, "QUIT");
	shutdown(irc_connection->fd, SHUT_RDWR);
}

void irc_parse_message_parameter(char* message,
                                 char* parameters[16],
                                 size_t* num_parameters_ptr)
{
	size_t num_parameters = 0;
	while ( message[0] != '\0' )
	{
		if ( message[0] == ':' || num_parameters == (16-1) -1 )
		{
			message++;
			parameters[num_parameters++] = message;
			break;
		}

		parameters[num_parameters++] = message;

		size_t usable = 0;
		while ( message[usable] != '\0' && message[usable] != ' ' )
			usable++;

		char lc = message[usable];
		message[usable] = '\0';

		if ( lc != '\0' )
			message += usable + 1;
		else
			message += usable;
	}
	*num_parameters_ptr = num_parameters;
}

void irc_parse_who(char* full, const char** who, const char** whomask)
{
	size_t bangpos = strcspn(full, "!");
	if ( full[bangpos] == '!' )
	{
		full[bangpos] = '\0';
		*who = full;
		*whomask = full + bangpos + 1;
	}
	else
	{
		*who = full;
		*whomask = "";
	}
}
