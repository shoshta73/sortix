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
 * database.c
 * Data structure for keeping track of channels and people.
 */

#include <sys/types.h>

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "database.h"
#include "network.h"
#include "string.h"

struct channel* find_channel(const struct network* state, const char* channel_name)
{
	assert(channel_name);

	for ( struct channel* channel = state->channels; channel; channel = channel->next_channel )
		if ( strchannelcmp(channel->name, channel_name) == 0 )
			return channel;
	return NULL;
}

struct channel* add_channel(struct network* state, const char* channel_name)
{
	assert(channel_name);

	assert(!find_channel(state, channel_name));
	struct channel* channel = (struct channel*) calloc(sizeof(struct channel), 1);
	if ( !channel )
		return NULL;
	channel->name = strdup(channel_name);
	if ( !channel->name )
		return free(channel), (struct channel*) NULL;
	channel->people = NULL;

	channel->prev_channel = NULL;
	channel->next_channel = state->channels;
	if ( state->channels )
		state->channels->prev_channel = channel;
	state->channels = channel;

	return channel;
}

struct channel* get_channel(struct network* state, const char* channel_name)
{
	assert(channel_name);

	for ( struct channel* result = find_channel(state, channel_name); result; result = NULL )
		return result;
	return add_channel(state, channel_name);
}

void remove_channel(struct network* state, struct channel* channel)
{
	while ( channel->people )
		remove_person_from_channel(state, channel->people);

	if ( channel->prev_channel )
		channel->prev_channel->next_channel = channel->next_channel;
	else
		state->channels = channel->next_channel;

	if ( channel->next_channel )
		channel->next_channel->prev_channel = channel->prev_channel;

	free(channel->name);
	free(channel);
}

struct person* find_person(const struct network* state, const char* nick)
{
	assert(nick);

	for ( struct person* person = state->people; person; person = person->next_person )
		if ( strnickcmp(person->nick, nick) == 0 )
			return person;
	return NULL;
}

struct person* add_person(struct network* state, const char* nick)
{
	assert(nick);

	assert(!find_person(state, nick));
	struct person* person = (struct person*) calloc(sizeof(struct person), 1);
	if ( !person )
		return NULL;
	person->nick = strdup(nick);
	if ( !person->nick )
		return free(person), (struct person*) NULL;

	person->prev_person = NULL;
	person->next_person = state->people;
	if ( state->people )
		state->people->prev_person = person;
	state->people = person;

	return person;
}

struct person* get_person(struct network* state, const char* nick)
{
	assert(nick);

	for ( struct person* result = find_person(state, nick); result; result = NULL )
		return result;
	return add_person(state, nick);
}

void remove_person(struct network* state, struct person* person)
{
	while ( person->channels )
		remove_person_from_channel(state, person->channels);

	if ( person->prev_person )
		person->prev_person->next_person = person->next_person;
	else
		state->people = person->next_person;

	if ( person->next_person )
		person->next_person->prev_person = person->prev_person;

	free(person->nick);
	free(person);
}

struct channel_person* find_person_in_channel(const struct network* state, const char* nick, const char* channel_name)
{
	assert(nick);
	assert(channel_name);

	struct channel* channel = find_channel(state, channel_name);
	if ( !channel )
		return NULL;
	for ( struct channel_person* channel_person = channel->people; channel_person; channel_person = channel_person->next_person_in_channel )
	{
		assert(channel_person->person);
		assert(channel_person->person->nick);
		if ( strnickcmp(channel_person->person->nick, nick) == 0 )
			return channel_person;
	}
	return NULL;
}

struct channel_person* add_person_to_channel(struct network* state, struct person* person, struct channel* channel)
{
	assert(person);
	assert(channel);

	assert(person->nick);
	assert(channel->name);

	assert(!find_person_in_channel(state, person->nick, channel->name));
	struct channel_person* channel_person = (struct channel_person*)
		calloc(sizeof(struct channel_person), 1);
	if ( !channel_person )
		return NULL;
	channel_person->channel = channel;
	channel_person->person = person;

	channel_person->prev_channel_person = NULL;
	channel_person->next_channel_person = state->channel_people;
	if ( state->channel_people )
		state->channel_people->prev_channel_person = channel_person;
	state->channel_people = channel_person;

	channel_person->prev_person_in_channel = NULL;
	channel_person->next_person_in_channel = channel->people;
	if ( channel->people )
		channel->people->prev_person_in_channel = channel_person;
	channel->people = channel_person;

	channel_person->prev_channel_for_person = NULL;
	channel_person->next_channel_for_person = person->channels;
	if ( person->channels )
		person->channels->prev_channel_for_person = channel_person;
	person->channels = channel_person;

	return channel_person;
}

struct channel_person* get_person_in_channel(struct network* state, struct person* person, struct channel* channel)
{
	for ( struct channel_person* result =
	     find_person_in_channel(state, person->nick, channel->name); result; result = NULL )
		return result;
	return add_person_to_channel(state, person, channel);
}

void remove_person_from_channel(struct network* state, struct channel_person* channel_person)
{
	if ( state->channel_people != channel_person )
		channel_person->prev_channel_person->next_channel_person = channel_person->next_channel_person;
	else
		state->channel_people = channel_person->next_channel_person;

	if ( channel_person->next_channel_person )
		channel_person->next_channel_person->prev_channel_person = channel_person->prev_channel_person;

	if ( channel_person->channel->people != channel_person )
		channel_person->prev_person_in_channel->next_person_in_channel = channel_person->next_person_in_channel;
	else
		channel_person->channel->people = channel_person->next_person_in_channel;

	if ( channel_person->next_person_in_channel )
		channel_person->next_person_in_channel->prev_person_in_channel = channel_person->prev_person_in_channel;

	if ( channel_person->person->channels != channel_person )
		channel_person->prev_channel_for_person->next_channel_for_person = channel_person->next_channel_for_person;
	else
		channel_person->person->channels = channel_person->next_channel_for_person;

	if ( channel_person->next_channel_for_person )
		channel_person->next_channel_for_person->prev_channel_for_person = channel_person->prev_channel_for_person;

	free(channel_person);
}
