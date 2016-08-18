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

#ifndef SCROLLBACK_H
#define SCROLLBACK_H

#include <stdbool.h>
#include <stddef.h>

struct network;

enum activity
{
	ACTIVITY_NONE,
	ACTIVITY_NONTALK,
	ACTIVITY_TALK,
	ACTIVITY_HIGHLIGHT,
};

struct message
{
	int hour;
	int min;
	int sec;
	char* who;
	char* what;
};

struct scrollback
{
	struct network* network;
	struct scrollback* scrollback_prev;
	struct scrollback* scrollback_next;
	char* name;
	struct message* messages;
	size_t messages_count;
	size_t messages_allocated;
	size_t who_width;
	enum activity activity;
};

void message_free(struct message* msg);
void scrollback_free(struct scrollback* sb);
struct scrollback* find_scrollback_network(const struct network* network);
struct scrollback* find_scrollback(const struct network* network,
                                   const char* name);
struct scrollback* add_scrollback(struct network* network,
                                  const char* name);
struct scrollback* get_scrollback(struct network* network,
                                  const char* name);
bool scrollback_add_message(struct scrollback* sb,
                            enum activity activity,
                            const struct message* msg);
bool scrollback_print(struct scrollback* sb,
                      enum activity activity,
                      const char* who,
                      const char* what);
__attribute__((format(printf, 4, 5)))
bool scrollback_printf(struct scrollback* sb,
                       enum activity activity,
                       const char* who,
                       const char* whatf,
                       ...);
void scrollback_clear(struct scrollback* sb);

#endif
