/*
 * Copyright (c) 2016 Jonas 'Sortie' Termansen.
 * Copyright (c) 2022 Juhani 'nortti' Krekelä.
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
 * irc.c
 * IRC client.
 */

#include <sys/socket.h>
#include <sys/utsname.h>

#include <assert.h>
#if defined(__sortix__)
#include <brand.h>
#endif
#include <err.h>
#include <locale.h>
#include <locale.h>
#include <netdb.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <pwd.h>

#include "compat.h"
#include "connection.h"
#include "database.h"
#include "network.h"
#include "scrollback.h"
#include "string.h"
#include "ui.h"

static const char* fix_where(const char* where, bool* op, bool* voice)
{
	while ( where[0] == '@' || where[0] == '+' )
	{
		if ( where[0] == '+' )
		{
			*op = true;
			*voice = true;
		}
		if ( where[0] == '@' )
		{
			*op = true;
		}
		where++;
	}
	return where;
}

// This should not happen unless the database was faulty or the network is
// faulty (perhaps malicious).
static void database_prediction_mistake(struct network* state, int line)
{
	irc_error_linef("database_prediction_mistake() at %s:%i!", __FILE__, line);
	(void) state;
}

#define database_prediction_mistake(x) database_prediction_mistake(x, __LINE__)

static void garbage_collect_people(struct network* state)
{
again:
	for ( struct person* person = state->people; person; person = person->next_person )
	{
		if ( !person->channels && !person->always_observable )
		{
			remove_person(state, person);
			goto again; // TODO: This runs in squared time.
		}
	}
}

void on_startup(struct network* state)
{
	(void) state;
}

void on_shutdown(struct network* state)
{
	(void) state;
}

void on_nick(struct network* state, const char* who, const char* whomask,
             const char* newnick)
{
	(void) whomask;
	if ( !strnickcmp(who, newnick) )
		return; // We don't care if nothing changed.

	struct person* person;

	// There shouldn't be anyone by the name newnick, let's ensure that.
	if ( (person = find_person(state, newnick)) )
	{
		database_prediction_mistake(state);
		// There, unexpectedly, is such a person that doesn't exist and it's me.
		if ( !strnickcmp(newnick, state->nick) )
			return irc_command_quit_malfunction(state->irc_connection, "network nonsense");
		// There isn't a newnick person, but we thought there was, let's forget.
		remove_person(state, person);
	}

	// Locate the person in the database to update the nick.
	if ( (person = find_person(state, who)) )
	{
		char* newnick_copy = strdup(newnick);
		if ( !newnick_copy )
			return irc_command_quit_malfunction(state->irc_connection, "strdup failure");
		free(person->nick);
		person->nick = newnick_copy;
		for ( struct channel_person* cp = person->channels;
		      cp;
		      cp = cp->next_channel_for_person )
		{
			struct scrollback* sb = get_scrollback(state, cp->channel->name);
			if ( sb )
				scrollback_printf(sb, ACTIVITY_NONTALK, "*",
				                  "%s is now known as %s", who, newnick);
		}
		struct scrollback* sb = find_scrollback(state, who);
		if ( sb )
			scrollback_printf(sb, ACTIVITY_NONTALK, "*",
			                  "%s is now known as %s", who, newnick);
	}

	// Evidently there was someone with the old nick we didn't know about, but
	// now there isn't, and there is someone with the new nick.
	else
	{
		// But we don't actually care about that person, we don't know any
		// channels in which that person resides, so that person would get
		// garbage collected. This shouldn't happen. The next time that person
		// does anything in a channel, we'll put that person in that channel.
		database_prediction_mistake(state);
	}

	// In case I changed my name, update it.
	if ( !strnickcmp(who, state->nick) )
	{
		char* newnick_copy = strdup(newnick);
		if ( !newnick_copy )
			return irc_command_quit_malfunction(state->irc_connection, "strdup failure");
		free(state->nick);
		state->nick = newnick_copy;
	}
}

void on_quit(struct network* state, const char* who, const char* whomask,
             const char* reason)
{
	(void) whomask;
	(void) reason;
	if ( !strnickcmp(who, state->nick) )
		return; // We don't care about our own quit message.

	// Delete that person from our user database.
	struct person* person;
	if ( (person = find_person(state, who)) )
	{
		for ( struct channel_person* cp = person->channels;
		      cp;
		      cp = cp->next_channel_for_person )
		{
			struct scrollback* sb = get_scrollback(state, cp->channel->name);
			if ( sb )
				scrollback_printf(sb, ACTIVITY_NONTALK, "*", "%s has quit (%s)",
				                  who, reason);
		}
		struct scrollback* sb = find_scrollback(state, who);
		if ( sb )
			scrollback_printf(sb, ACTIVITY_NONTALK, "*", "%s has quit (%s)",
			                  who, reason);
		remove_person(state, person);
	}
	else
	{
		// Oddly, we didn't know about that person. But that's okay, the person
		// is now gone, and not knowing about that person is correct.
		database_prediction_mistake(state);
	}
}

static
void on_as_if_join(struct network* state, const char* who, const char* where)
{
	// Check if I'm joining a channel.
	if ( !strnickcmp(who, state->nick) )
	{
		// In case we already thought we were in that channel.
		if ( find_channel(state, where) )
		{
			// I'm already in that channel according to the database.
			database_prediction_mistake(state);
			assert(find_person_in_channel(state, state->nick, where));
			return;
		}

		struct channel* channel;
		if ( !(channel = add_channel(state, where)) )
			return irc_command_quit_malfunction(state->irc_connection, "add_channel failure");

		struct person* self = find_person(state, state->nick);
		assert(self);
		if ( !add_person_to_channel(state, self, channel) )
			return irc_command_quit_malfunction(state->irc_connection, "add_person_to_channel failure");

		return;
	}

	// Find the channel.
	struct channel* channel;
	if ( !(channel = find_channel(state, where)) )
	{
		database_prediction_mistake(state);

		if ( !(channel = add_channel(state, where)) )
			return irc_command_quit_malfunction(state->irc_connection, "add_channel failure");

		// I must be in that channel.
		struct person* self = find_person(state, state->nick);
		assert(self);
		if ( !add_person_to_channel(state, self, channel) )
			return irc_command_quit_malfunction(state->irc_connection, "add_person_to_channel failure");
	}

	// Find the person.
	struct person* person = get_person(state, who);
	if ( !person )
		return irc_command_quit_malfunction(state->irc_connection, "get_person failure");

	// Check if the person is already in that channel.
	if ( find_person_in_channel(state, who, channel->name) )
	{
		// We mistakenly already thought that person was in this channel.
			database_prediction_mistake(state);
		return;
	}

	// Put the person in the channel.
	if ( !add_person_to_channel(state, person, channel) )
		return irc_command_quit_malfunction(state->irc_connection, "add_person_to_channel");
}

void on_join(struct network* state, const char* who, const char* whomask,
             const char* where)
{
	(void) whomask;

	where = fix_where(where, NULL, NULL);
	if ( where[0] != '#' )
		return; // Join on non-channel doesn't make sense.

	on_as_if_join(state, who, where);

	struct scrollback* sb = get_scrollback(state, where);
	if ( sb )
	{
		int activity = ACTIVITY_NONTALK;
		if ( !strnickcmp(who, state->nick) )
			activity = ACTIVITY_NONE;
		scrollback_printf(sb, activity, "*", "%s (%s) has joined %s",
		                  who, whomask, where);
	}
}

static
void on_as_if_part(struct network* state, const char* who, const char* where)
{
	where = fix_where(where, NULL, NULL);

	// Check if I'm parting a channel.
	if ( !strnickcmp(who, state->nick) )
	{
		// Find the channel.
		struct channel* channel;
		if ( !(channel = find_channel(state, where)) )
		{
			// I'm parting a channel I didn't know I was in. Do nothing, as now
			// my wrong belief became right.
			database_prediction_mistake(state);
			return;
		}

		// Forget about the channel as I'm no longer there to observe it.
		remove_channel(state, channel);
		// Some people may no longer be observable.
		garbage_collect_people(state);

		return;
	}

	// Find the channel.
	struct channel* channel;
	if ( !(channel = find_channel(state, where)) )
	{
		// Someone parted a channel I didn't know I was in.
		database_prediction_mistake(state);

		if ( !add_channel(state, where) )
			return irc_command_quit_malfunction(state->irc_connection, "add_channel failure");

		// I must be in that channel.
		struct person* self = find_person(state, state->nick);
		assert(self);
		if ( !add_person_to_channel(state, self, channel) )
			return irc_command_quit_malfunction(state->irc_connection, "add_person_to_channel failure");
	}

	// Find the person.
	struct person* person = find_person(state, who);
	if ( !person )
	{
		// I didn't know about the person that just left a channel I'm in. But
		// since we don't know about that person in any other channel, we'll
		// garbage collect them here.
		database_prediction_mistake(state);
		return;
	}

	struct channel_person* channel_person = find_person_in_channel(state, who, where);
	if ( !channel_person )
	{
		// I didn't think that person was in this channel. But that's true now,
		// so I don't need to do anything.
		database_prediction_mistake(state);
		return;
	}

	// Remove the person from the channel.
	remove_person_from_channel(state, channel_person);

	// If the person no longer shares any channels with us, we won't be notified
	// of quits or renames, and thus can't track that person's identity, so we
	// garbage collect the person until it comes back in side. Some people are
	// always obserable, such as ourselves or people that have engaged in a
	// private message to us.
	if ( !person->channels && !person->always_observable )
		remove_person(state, person);
}

void on_part(struct network* state, const char* who, const char* whomask,
             const char* where)
{
	(void) whomask;

	where = fix_where(where, NULL, NULL);
	if ( where[0] != '#' )
		return; // Part on non-channel doesn't make sense.

	on_as_if_part(state, who, where);

	struct scrollback* sb = get_scrollback(state, where);
	if ( sb )
		scrollback_printf(sb, ACTIVITY_NONTALK, "*", "%s (%s) has left %s",
		                  who, whomask, where);
}

static
void on_evidently_exists(struct network* state, const char* who,
                         const char* whomask, const char* where)
{
	(void) whomask;

	if ( where[0] != '#' )
	{
		// Find the person.
		struct person* person = find_person(state, who);
		if ( !person )
		{
			if ( !(person = add_person(state, who)) )
				return irc_command_quit_malfunction(state->irc_connection, "get_person failure");
		}

		// The person has now privately messaged us, so assume this person is
		// now always observable.
		person->always_observable = true;

		return;
	}

	// Find the channel.
	struct channel* channel;
	if ( !(channel = find_channel(state, where)) )
	{
		database_prediction_mistake(state);

		if ( !add_channel(state, where) )
			return irc_command_quit_malfunction(state->irc_connection, "add_channel failure");

		// I must be in that channel.
		struct person* self = find_person(state, state->nick);
		assert(self);
		if ( !add_person_to_channel(state, self, channel) )
			return irc_command_quit_malfunction(state->irc_connection, "add_person_to_channel failure");
	}

	// TODO: The following code makes the assumption that you must be in a
	//       channel to do particular things and that's a wrong assumption.
	return;

	// Find the person.
	struct person* person = find_person(state, who);
	if ( !person )
	{
		database_prediction_mistake(state);

		if ( !(person = add_person(state, who)) )
			return irc_command_quit_malfunction(state->irc_connection, "get_person failure");
	}

	// Check if the person is already in that channel.
	if ( !find_person_in_channel(state, who, channel->name) )
	{
		// We mistakenly already thought that person was in this channel.
		database_prediction_mistake(state);

		// Put the person in the channel.
		if ( !add_person_to_channel(state, person, channel) )
			return irc_command_quit_malfunction(state->irc_connection, "add_person_to_channel");
	}
}

void on_privmsg(struct network* state, const char* who, const char* whomask,
                const char* where, const char* what)
{
	bool where_op = false;
	bool where_voice = false;
	where = fix_where(where, &where_op, &where_voice);

	on_evidently_exists(state, who, whomask, where);

	// TODO: Does this happen? How about if we message ourself? Do we want this
	//       in an IRC client?
	if ( !strnickcmp(who, state->nick) )
		return; // We don't care about our own messages.

	if ( !strcmp(what, "\x01VERSION\x01") )
	{
		struct utsname un;
		uname(&un);
#if defined(__sortix__)
		irc_command_noticef(state->irc_connection, who,
		                    "VERSION %s irc %s %s",
		                    BRAND_DISTRIBUTION_NAME, VERSIONSTR,
		                    BRAND_RELEASE_TAGLINE);
#else
		irc_command_noticef(state->irc_connection, who,
		                    "VERSION Sortix irc %s on %s %s",
		                    VERSIONSTR, un.sysname, un.release);
#endif
	}

	const char* sbname = where[0] == '#' ? where : who;
	struct scrollback* sb = get_scrollback(state, sbname);
	if ( sb )
	{
		// TODO: \x01ACTION foo\x01 support.
		// TODO: Highlights and such.
		scrollback_print(sb, ACTIVITY_TALK, who, what);
	}
}

void on_notice(struct network* state, const char* who, const char* whomask,
               const char* where, const char* what)
{
	bool where_op = false;
	bool where_voice = false;
	where = fix_where(where, &where_op, &where_voice);

	on_evidently_exists(state, who, whomask, where);

	if ( !strnickcmp(who, state->nick) )
		return; // We don't care about our own messages.

	const char* sbname = where[0] == '#' ? where : who;
	struct scrollback* sb = get_scrollback(state, sbname);
	if ( sb )
	{
		// TODO: \x01ACTION foo\x01 support.
		// TODO: Highlights and such.
		// TODO: Print as -who-.
		scrollback_print(sb, ACTIVITY_TALK, who, what);
	}
}

void on_topic(struct network* state, const char* who, const char* whomask,
              const char* where, const char* topic)
{
	where = fix_where(where, NULL, NULL);

	// Find the channel.
	struct channel* channel;
	if ( !(channel = find_channel(state, where)) )
	{
		database_prediction_mistake(state);

		if ( !(channel = add_channel(state, where)) )
			return irc_command_quit_malfunction(state->irc_connection, "add_channel failure");

		// I must be in that channel.
		struct person* self = find_person(state, state->nick);
		assert(self);
		if ( !add_person_to_channel(state, self, channel) )
			return irc_command_quit_malfunction(state->irc_connection, "add_person_to_channel failure");
	}

	(void) who;
	(void) whomask;

	free(channel->topic);
	channel->topic = strdup(topic);

	struct scrollback* sb = get_scrollback(state, where);
	if ( sb )
		scrollback_printf(sb, ACTIVITY_NONTALK, "*",
		                  "%s has changed the topic to: %s", who, topic);
}

void on_kick(struct network* state, const char* who, const char* whomask,
             const char* where, const char* target, const char* reason)
{
	where = fix_where(where, NULL, NULL);

	on_evidently_exists(state, who, whomask, where);

	on_as_if_part(state, target, where);

	struct scrollback* sb = get_scrollback(state, where);
	if ( sb )
		scrollback_printf(sb, ACTIVITY_NONTALK, "*", "%s has kicked %s (%s)",
		                  who, target, reason);
}

void on_mode(struct network* state, const char* who, const char* whomask,
             const char* where, const char* mode, const char* target)
{
	(void) whomask;

	where = fix_where(where, NULL, NULL);

	on_evidently_exists(state, who, whomask, where);

	struct channel_person* cp = find_person_in_channel(state, target, where);
	if ( cp )
	{
		bool set = true;
		for ( size_t i = 0; mode[i]; i++ )
		{
			switch ( mode[i] )
			{
			case '-': set = false; break;
			case '+': set = true; break;
			case 'o': cp->is_operator = set; break;
			case 'v': cp->is_voiced = set; break;
			}
		}
	}

	struct scrollback* sb = get_scrollback(state, where);
	if ( sb )
		scrollback_printf(sb, ACTIVITY_NONTALK, "*", "%s sets mode %s on %s",
		                  who, mode, target);
}

void on_332(struct network* state, const char* where, const char* topic)
{
	where = fix_where(where, NULL, NULL);

	// Find the channel.
	struct channel* channel;
	if ( !(channel = find_channel(state, where)) )
	{
		database_prediction_mistake(state);

		if ( !(channel = add_channel(state, where)) )
			return irc_command_quit_malfunction(state->irc_connection, "add_channel failure");

		// I must be in that channel.
		struct person* self = find_person(state, state->nick);
		assert(self);
		if ( !add_person_to_channel(state, self, channel) )
			return irc_command_quit_malfunction(state->irc_connection, "add_person_to_channel failure");
	}

	free(channel->topic);
	channel->topic = strdup(topic);

	struct scrollback* sb = get_scrollback(state, where);
	if ( sb )
		scrollback_printf(sb, ACTIVITY_NONE, "*", "Topic for %s is: %s",
		                  where, topic);
}

void on_353(struct network* state, const char* wheretype, const char* where,
            const char* list)
{
	(void) wheretype;
	where = fix_where(where, NULL, NULL);

	// Find the channel.
	struct channel* channel;
	if ( !(channel = find_channel(state, where)) )
	{
		database_prediction_mistake(state);

		if ( !add_channel(state, where) )
			return irc_command_quit_malfunction(state->irc_connection, "add_channel failure");

		// I must be in that channel.
		struct person* self = find_person(state, state->nick);
		assert(self);
		if ( !add_person_to_channel(state, self, channel) )
			return irc_command_quit_malfunction(state->irc_connection, "add_person_to_channel failure");
	}

	char names[512];
	strlcpy(names, list, sizeof(names));
	char* names_input = names;
	char* names_next = NULL;
	char* name;
	while ( (name = strtok_r(names_input, " ", &names_next)) )
	{
		bool is_operator = false;
		bool is_voiced = false;
		if ( name[0] == '@' )
			name++, is_operator = true;
		else if ( name[0] == '+' )
			name++, is_voiced = true;

		struct channel_person* channel_person =
			get_person_in_channel(state, get_person(state, name), channel);

		channel_person->is_operator = is_operator;
		channel_person->is_voiced = is_voiced;

		names_input = NULL;
	}
}

static bool handle_message(struct network* state, const char* orig_message)
{
	char message[512];
	memcpy(message, orig_message, sizeof(message));

	char* parameters[16];
	size_t num_parameters;
	irc_parse_message_parameter(message, parameters, &num_parameters);

	if ( 2 <= num_parameters && !strcmp(parameters[0], "PING") )
	{
		irc_transmit_format(state->irc_connection, "PONG :%s", parameters[1]);
		return true;
	}

	if ( num_parameters != 1 )
		return false;

	irc_parse_message_parameter(parameters[0], parameters, &num_parameters);

	if ( num_parameters < 1 )
		return false;

	if ( !strcmp(parameters[1], "332") )
	{
		if ( num_parameters < 5 )
			return false;
		const char* where = parameters[3];
		const char* topic = parameters[4];
		on_332(state, where, topic);
		return true;
	}

	if ( !strcmp(parameters[1], "333") )
	{
		// TODO: Topic set by.
		return true;
	}

	if ( !strcmp(parameters[1], "353") )
	{
		if ( num_parameters < 6 )
			return false;
		const char* wheretype = parameters[3];
		const char* where = parameters[4];
		const char* list = parameters[5];
		on_353(state, wheretype, where, list);
		return true;
	}

	if ( !strcmp(parameters[1], "366") )
	{
		// TODO: End of /NAMES list.
		return true;
	}

	const char* who;
	const char* whomask;
	irc_parse_who(parameters[0], &who, &whomask);

	if ( num_parameters < 2 )
		return false;

	if ( num_parameters < 3 )
		return false;

	if ( !strcmp(parameters[1], "NICK") )
	{
		const char* new_nick = parameters[2];
		on_nick(state, who, whomask, new_nick);
		return true;
	}

	if ( !strcmp(parameters[1], "QUIT") )
	{
		const char* reason = parameters[2];
		on_quit(state, who, whomask, reason);
		return true;
	}

	const char* where = parameters[2];
	if ( !strnickcmp(where, state->nick) )
		where = who;

	if ( !strcmp(parameters[1], "JOIN") )
	{
		on_join(state, who, whomask, where);
		return true;
	}

	if ( !strcmp(parameters[1], "PART") )
	{
		on_part(state, who, whomask, where);
		return true;
	}

	if ( num_parameters < 4 )
		return false;

	if ( !strcmp(parameters[1], "PRIVMSG") )
	{
		if ( strchr(who, '.') )
			return false; // Network message.
		const char* what = parameters[3];
		on_privmsg(state, who, whomask, where, what);
		return true;
	}

	if ( !strcmp(parameters[1], "NOTICE") )
	{
		if ( strchr(who, '.') )
			return false; // Network message.
		const char* what = parameters[3];
		on_notice(state, who, whomask, where, what);
		return true;
	}

	if ( !strcmp(parameters[1], "TOPIC") )
	{
		const char* topic = parameters[3];
		on_topic(state, who, whomask, where, topic);
		return true;
	}

	if ( num_parameters < 5 )
		return false;

	if ( !strcmp(parameters[1], "KICK") )
	{
		const char* target = parameters[3];
		const char* reason = parameters[4];
		on_kick(state, who, whomask, where, target, reason);
		return true;
	}

	if ( !strcmp(parameters[1], "MODE") )
	{
		const char* mode = parameters[3];
		const char* target = parameters[4];
		on_mode(state, who, whomask, where, mode, target);
		return true;
	}

	return false;
}

static void on_message(struct network* state, const char* message)
{
	if ( !handle_message(state, message) )
	{
		struct scrollback* sb = find_scrollback_network(state);
		scrollback_print(sb, ACTIVITY_NONTALK, state->server_hostname, message);
	}
}

static void mainloop(struct network* state)
{
	struct ui ui;
	ui_initialize(&ui, state);

	if ( state->password )
	{
		irc_command_pass(state->irc_connection, state->password);
		explicit_bzero(state->password, strlen(state->password));
		free(state->password);
	}
	irc_command_nick(state->irc_connection, state->nick);
	irc_command_user(state->irc_connection, state->nick, "localhost",
	                 state->server_hostname, state->real_name);

	struct person* self = add_person(state, state->nick);
	if ( !self )
	{
		irc_command_quit_malfunction(state->irc_connection, "add_person failure");
		return;
	}
	self->always_observable = true;

	if ( state->autojoin )
	{
		irc_command_join(state->irc_connection, state->autojoin);
		struct scrollback* sb = get_scrollback(state, state->autojoin);
		if ( sb )
			ui.current = sb;
	}

	on_startup(state);

	while ( true )
	{
		ui_render(&ui);

		if ( state->irc_connection->connectivity_error )
		{
			irc_error_linef("Exiting main loop due to transmit error");
			break;
		}

		struct pollfd pfds[2];
		memset(pfds, 0, sizeof(pfds));
		pfds[0].fd = 0;
		pfds[0].events = POLLIN;
		pfds[1].fd = state->irc_connection->fd;
		pfds[1].events = POLLIN;

		int status = poll(pfds, 2, -1);
		if ( status < 0 )
			err(1, "poll");

		if ( pfds[0].revents & POLLIN )
		{
			char buffer[512];
			ssize_t amount = read(0, buffer, sizeof(buffer));
			if ( amount < 0 )
				err(1, "read: stdin");
			for ( ssize_t i = 0; i < amount; i++ )
				ui_input_char(&ui, buffer[i]);
		}
		if ( pfds[1].revents & POLLIN )
		{
			irc_receive_more_bytes(state->irc_connection);
			char message[512];
			struct timespec now; // TODO: Use this?
			while ( irc_receive_message(state->irc_connection, message, &now) )
				on_message(state, message);
		}
	}

	on_shutdown(state);

	irc_command_quit(state->irc_connection, NULL);

	ui_destroy(&ui);
}

int main(int argc, char* argv[])
{
	setlocale(LC_ALL, "");

	const char* host = NULL;
	const char* nick = NULL;
	const char* real_name = NULL;
	const char* service = "6667";
	const char* password = NULL;
	const char* autojoin = NULL;

	int c;
	while ( 0 <= (c = getopt(argc, argv, "h:j:n:N:p:P:")) )
	{
		switch ( c )
		{
		case 'h': host = optarg; break;
		case 'j': autojoin = optarg; break;
		case 'n': nick = optarg; break;
		case 'N': real_name = optarg; break;
		case 'p': service = optarg; break;
		case 'P': password = optarg; break;
		default: errx(1, "invalid option -- '%c'", optopt);
		}
	}

	if ( !nick )
	{
		struct passwd* pwd = getpwuid(getuid());
		if ( !pwd )
			errx(1, "no -n nick option was passed");
		nick = pwd->pw_name;
		if ( !real_name )
		{
			// TODO: How should gecos be properly parsed?
			size_t commapos = strcspn(pwd->pw_gecos, ",");
			pwd->pw_gecos[commapos] = '\0';
			if ( pwd->pw_gecos[0] )
				real_name = pwd->pw_gecos;
		}
	}

	if ( !real_name )
		real_name = nick;
	if ( !host )
		errx(1, "no -h host option was passed");
	if ( !service )
		errx(1, "no -p port/service option was passed");

	struct network state;
	memset(&state, 0, sizeof(state));
	state.nick = strdup(nick);
	if ( !state.nick )
		err(1, "strdup");
	state.real_name = strdup(real_name);
	if ( !state.real_name )
		err(1, "strdup");
	if ( password )
	{
		state.password = strdup(password);
		if ( !state.password )
			err(1, "strdup");
		explicit_bzero((char*) password, strlen(password));
	}
	state.server_hostname = strdup(host);
	if ( !state.server_hostname )
		err(1, "strdup");
	state.autojoin = autojoin;

	struct addrinfo addrinfo_hints;
	memset(&addrinfo_hints, 0, sizeof(addrinfo_hints));
	addrinfo_hints.ai_flags = 0;
	addrinfo_hints.ai_family = AF_UNSPEC;
	addrinfo_hints.ai_socktype = SOCK_STREAM;
	addrinfo_hints.ai_protocol = 0;

	struct addrinfo* addrinfo;
	int ret;
	if ( (ret = getaddrinfo(host, service, &addrinfo_hints, &addrinfo)) != 0 )
		errx(1, "could not resolve: %s: %s: %s",
		     host, service, gai_strerror(ret));

	int fd = -1;
	for ( struct addrinfo* info = addrinfo; info; info = info->ai_next )
	{
		if ( (fd = socket(info->ai_family, info->ai_socktype | SOCK_CLOEXEC,
		                  info->ai_protocol)) < 0 )
		{
			warn("socket");
			continue;
		}

		if ( connect(fd, info->ai_addr, info->ai_addrlen) < 0 )
		{
			warn("connect");
			close(fd);
			continue;
		}
		break;
	}
	if ( fd < 0 )
		errx(1, "unable to connect, exiting.");

	freeaddrinfo(addrinfo);

	struct irc_connection irc_connection;
	memset(&irc_connection, 0, sizeof(irc_connection));
	irc_connection.fd = fd;
	state.irc_connection = &irc_connection;

	if ( !add_scrollback(&state, state.server_hostname) )
		err(1, "add_scrollback: %s", state.server_hostname);

	mainloop(&state);

	close(irc_connection.fd);

	return 0;
}
