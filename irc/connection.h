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
 * connection.h
 * IRC protocol.
 */

#ifndef CONNECTION_H
#define CONNECTION_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

struct irc_connection
{
	int fd;
	bool connectivity_error;
	char incoming_buffer[512];
	size_t incoming_amount;
};

__attribute__((format(printf, 1, 0)))
void irc_error_vlinef(const char* format, va_list ap);
__attribute__((format(printf, 1, 2)))
void irc_error_linef(const char* format, ...);
void irc_transmit(struct irc_connection* irc_connection,
                  const char* message,
                  size_t message_size);
void irc_transmit_message(struct irc_connection* irc_connection,
                          const char* message,
                          size_t message_size);
void irc_transmit_string(struct irc_connection* irc_connection,
                         const char* string);
__attribute__((format(printf, 2, 0)))
void irc_transmit_vformat(struct irc_connection* irc_connection,
                         const char* format,
                         va_list ap);
__attribute__((format(printf, 2, 3)))
void irc_transmit_format(struct irc_connection* irc_connection,
                         const char* format,
                         ...);
void irc_receive_more_bytes(struct irc_connection* irc_connection);
bool irc_receive_message(struct irc_connection* irc_connection,
                         char message[512],
                         struct timespec* when);
void irc_command_pass(struct irc_connection* irc_connection,
                      const char* password);
void irc_command_nick(struct irc_connection* irc_connection,
                      const char* nick);
void irc_command_user(struct irc_connection* irc_connection,
                      const char* nick,
                      const char* local_hostname,
                      const char* server_hostname,
                      const char* real_name);
void irc_command_join(struct irc_connection* irc_connection,
                      const char* channel);
void irc_command_part(struct irc_connection* irc_connection,
                      const char* channel);
void irc_command_privmsg(struct irc_connection* irc_connection,
                         const char* where,
                         const char* what);
__attribute__((format(printf, 3, 4)))
void irc_command_privmsgf(struct irc_connection* irc_connection,
                          const char* where,
                          const char* what_format,
                          ...);
void irc_command_notice(struct irc_connection* irc_connection,
                        const char* where,
                        const char* what);
__attribute__((format(printf, 3, 4)))
void irc_command_noticef(struct irc_connection* irc_connection,
                         const char* where,
                         const char* what_format,
                         ...);
void irc_command_kick(struct irc_connection* irc_connection,
                      const char* where,
                      const char* who,
                      const char* why);
void irc_command_quit(struct irc_connection* irc_connection,
                      const char* message);
void irc_command_quit_malfunction(struct irc_connection* irc_connection,
                                  const char* message);
void irc_parse_message_parameter(char* message,
                                 char* parameters[16],
                                 size_t* num_parameters_ptr);
void irc_parse_who(char* full, const char** who, const char** whomask);

#endif
