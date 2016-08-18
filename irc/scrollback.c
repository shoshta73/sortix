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
 * scrollback.c
 * Ordered messages for display.
 */

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "compat.h"
#include "network.h"
#include "scrollback.h"
#include "string.h"

void message_free(struct message* msg)
{
	free(msg->who);
	free(msg->what);
}

void scrollback_free(struct scrollback* sb)
{
	if ( sb->network )
	{
		if ( sb->scrollback_prev )
			sb->scrollback_prev->scrollback_next = sb->scrollback_next;
		else
			sb->network->scrollbacks = sb->scrollback_next;
		if ( sb->scrollback_next )
			sb->scrollback_next->scrollback_prev = sb->scrollback_prev;
		sb->scrollback_prev = NULL;
		sb->scrollback_next = NULL;
		sb->network = NULL;
	}
	for ( size_t i = 0; i < sb->messages_count; i++ )
		message_free(&sb->messages[i]);
	free(sb->messages);
	free(sb->name);
	free(sb);
}

struct scrollback* find_scrollback_network(const struct network* network)
{
	// TODO: The server hostname can be a valid nick, for instance if the
	//       hostname doesn't contain any dot characters.
	for ( struct scrollback* sb = network->scrollbacks;
	      sb;
	      sb = sb->scrollback_next )
	{
		if ( sb->name[0] == '#' )
			continue;
		if ( !strnickcmp(network->server_hostname, sb->name) )
			return sb;
	}
	return NULL;
}

struct scrollback* find_scrollback(const struct network* network,
                                   const char* name)
{
	assert(name);
	for ( struct scrollback* sb = network->scrollbacks;
	      sb;
	      sb = sb->scrollback_next )
	{
		if ( name[0] == '#' && sb->name[0] == '#' )
		{
			if ( strchannelcmp(name + 1, sb->name + 1) == 0 )
				return sb;
		}
		else if ( name[0] != '#' && sb->name[0] != '#' )
		{
			if ( strnickcmp(name + 1, sb->name + 1) == 0 )
				return sb;
		}
	}
	return NULL;
}

struct scrollback* add_scrollback(struct network* network, const char* name)
{
	struct scrollback* sb =
		(struct scrollback*) calloc(1, sizeof(struct scrollback));
	if ( !sb )
		return NULL;
	if ( !(sb->name = strdup(name)) )
		return scrollback_free(sb), (struct scrollback*) NULL;
	sb->network = network;
	sb->scrollback_next = sb->network->scrollbacks;
	if ( sb->scrollback_next )
		sb->scrollback_next->scrollback_prev = sb;
	sb->network->scrollbacks = sb;
	return sb;
}

struct scrollback* get_scrollback(struct network* network, const char* name)
{
	struct scrollback* result = find_scrollback(network, name);
	if ( result )
		return result;
	return add_scrollback(network, name);
}

bool scrollback_add_message(struct scrollback* sb,
                            enum activity activity,
                            const struct message* msg)
{
	if ( sb->messages_count == sb->messages_allocated )
	{
		size_t new_allocated = 2 * sb->messages_allocated;
		if ( new_allocated == 0 )
			new_allocated = 64;
		struct message* new_messages = (struct message*)
			reallocarray(sb->messages, new_allocated, sizeof(struct message));
		if ( !new_messages )
			return false;
		sb->messages = new_messages;
		sb->messages_allocated = new_allocated;
	}
	sb->messages[sb->messages_count++] = *msg;
	size_t who_width = strlen(msg->who); // TODO: Unicode?
	if ( sb->who_width < who_width )
		sb->who_width = who_width;
	if ( sb->activity < activity )
		sb->activity = activity;
	return true;
}

static void message_timestamp(struct message* msg)
{
	struct tm tm;
	time_t now = time(NULL);
	localtime_r(&now, &tm);
	msg->sec = tm.tm_sec;
	msg->min = tm.tm_min;
	msg->hour = tm.tm_hour;
}

bool scrollback_print(struct scrollback* sb,
                      enum activity activity,
                      const char* who,
                      const char* what)
{
	struct message msg;
	memset(&msg, 0, sizeof(msg));
	message_timestamp(&msg);
	if ( (msg.who = strdup(who)) &&
	     (msg.what = strdup(what)) &&
	     scrollback_add_message(sb, activity, &msg) )
		return true;
	message_free(&msg);
	return false;
}

bool scrollback_printf(struct scrollback* sb,
                       enum activity activity,
                       const char* who,
                       const char* whatf,
                       ...)
{
	struct message msg;
	memset(&msg, 0, sizeof(msg));
	message_timestamp(&msg);
	va_list ap;
	va_start(ap, whatf);
	int len = vasprintf(&msg.what, whatf, ap);
	va_end(ap);
	if ( (msg.who = strdup(who)) &&
	     0 <= len &&
	     scrollback_add_message(sb, activity, &msg) )
		return true;
	message_free(&msg);
	return false;
}

void scrollback_clear(struct scrollback* sb)
{
	for ( size_t i = 0; i < sb->messages_count; i++ )
	{
		free(sb->messages[i].who);
		free(sb->messages[i].what);
	}
	sb->messages_count = 0;
	sb->messages_allocated = 0;
	free(sb->messages);
	sb->messages = NULL;
}
