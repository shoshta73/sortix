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
 * database.h
 * Data structure for keeping track of channels and people.
 */

#ifndef DATABASE_H
#define DATABASE_H

struct channel;
struct channel_person;
struct network;
struct person;

struct person
{
	struct person* prev_person;
	struct person* next_person;
	char* nick;
	struct channel_person* channels;
	bool always_observable; // myself or having private messaged me
};

struct channel_person
{
	struct channel_person* prev_channel_person;
	struct channel_person* next_channel_person;
	struct channel_person* prev_person_in_channel;
	struct channel_person* next_person_in_channel;
	struct channel_person* prev_channel_for_person;
	struct channel_person* next_channel_for_person;
	struct channel* channel;
	struct person* person;
	bool is_operator;
	bool is_voiced;
};

struct channel
{
	struct channel* prev_channel;
	struct channel* next_channel;
	char* name;
	char* topic;
	struct channel_person* people;
};

struct channel* find_channel(const struct network* state, const char* channel_name);
struct channel* add_channel(struct network* state, const char* channel_name);
struct channel* get_channel(struct network* state, const char* channel_name);
void remove_channel(struct network* state, struct channel* channel);
struct person* find_person(const struct network* state, const char* nick);
struct person* add_person(struct network* state, const char* nick);
struct person* get_person(struct network* state, const char* nick);
void remove_person(struct network* state, struct person* person);
struct channel_person* find_person_in_channel(const struct network* state, const char* nick, const char* channel_name);
struct channel_person* add_person_to_channel(struct network* state, struct person* person, struct channel* channel);
struct channel_person* get_person_in_channel(struct network* state, struct person* person, struct channel* channel);
void remove_person_from_channel(struct network* state, struct channel_person* channel_person);

#endif
