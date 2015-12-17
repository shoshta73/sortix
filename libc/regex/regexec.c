/*
 * Copyright (c) 2014, 2015 Jonas 'Sortie' Termansen.
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
 * regex/regexec.c
 * Regular expression execution.
 */

#include <assert.h>
#include <pthread.h>
#include <regex.h>
#include <stdbool.h>

#include <stdio.h> // TODO: DEBUG

// TODO: DEBUG
static inline bool verify_current_state(struct re* first, struct re* last, struct re* current)
{
	if ( !current )
	{
		if ( first || current )
			return printf("(error 1)\n"), false;
		return true;
	}
	if ( !current->re_is_current )
		return printf("(error 2)\n"), false;
	if ( !current->re_current_state_prev && first != current )
		return printf("(error 3)\n"), false;
	if ( !current->re_current_state_next && last != current )
		return printf("(error 4)\n"), false;
	if ( current->re_current_state_prev )
	{
		if ( first == current )
			return printf("(error 9)\n"), false;
		if ( current->re_current_state_prev->re_current_state_next != current )
			return printf("(error 5)\n"), false;
	}
	if ( current->re_current_state_next )
	{
		if ( last == current )
			return printf("(error 10)\n"), false;
		if ( current->re_current_state_next->re_current_state_prev != current )
			return printf("(error 6)\n"), false;
		if ( !current->re_is_currently_done && current->re_current_state_next->re_is_currently_done )
			return printf("(error 7)\n"), false;
	}
	for ( struct re* iter = current->re_current_state_next; iter; iter = iter->re_current_state_next )
	{
		if ( iter == current )
			return printf("(error 8)\n"), false;
	}
	if ( !current->re_current_state_next )
		return true;
	return verify_current_state(first, last, current->re_current_state_next);
}

#define QUEUE_CURRENT_STATE(new_state) \
{ \
	if ( !new_state ) \
	{ \
		match = true; \
		assert(verify_current_state(current_states, current_states_last, current_states) && "1"); \
		assert(verify_current_state(current_states, current_states_last, state) && "10"); \
		for ( struct re* re = state->re_current_state_next; \
		      re; \
		      re = re->re_current_state_next ) \
		{ \
			assert(re->re_current_state_next != re); \
			assert(!re->re_current_state_next || re->re_current_state_next->re_current_state_next != re); \
			re->re_is_current = 0; \
		} \
		state->re_current_state_next = NULL; \
		current_states_last = state; \
		assert(verify_current_state(current_states, current_states_last, current_states) && "11"); \
		assert(verify_current_state(current_states, current_states_last, state) && "12"); \
	} \
	else if ( !(new_state->re_is_current && new_state->re_is_currently_done) ) \
	{ \
		assert(verify_current_state(current_states, current_states_last, state) && "6"); \
		if ( new_state->re_is_current ) \
		{ \
			assert(verify_current_state(current_states, current_states_last, current_states) && "2"); \
			if ( new_state->re_current_state_prev ) \
				new_state->re_current_state_prev->re_current_state_next = \
					new_state->re_current_state_next; \
			else \
				current_states = new_state->re_current_state_next; \
			if ( new_state->re_current_state_next ) \
				new_state->re_current_state_next->re_current_state_prev = \
					new_state->re_current_state_prev; \
			else \
				current_states_last = new_state->re_current_state_prev; \
			assert(verify_current_state(current_states, current_states_last, current_states) && "3"); \
		} \
		assert(verify_current_state(current_states, current_states_last, current_states) && "4"); \
		new_state->re_current_state_prev = state; \
		new_state->re_current_state_next = state->re_current_state_next; \
		if ( state->re_current_state_next ) \
			state->re_current_state_next->re_current_state_prev = new_state; \
		else \
			current_states_last = new_state; \
		state->re_current_state_next = new_state; \
		new_state->re_is_currently_done = 0; \
		new_state->re_is_current = 1; \
		new_state->re_is_upcoming = 0; \
		for ( size_t m = 0; m < nmatch; m++ ) \
			new_state->re_matches[m] = state->re_matches[m]; \
		assert(verify_current_state(current_states, current_states_last, current_states) && "5"); \
		assert(verify_current_state(current_states, current_states_last, state) && "7"); \
		assert(verify_current_state(current_states, current_states_last, new_state) && "8"); \
	} \
} \

#define QUEUE_UPCOMING_STATE(new_state) \
{ \
	if ( !new_state ) \
	{ \
		consumed_char = true; \
		match = true; \
		assert(verify_current_state(current_states, current_states_last, current_states) && "1"); \
		assert(verify_current_state(current_states, current_states_last, state) && "10"); \
		for ( struct re* re = state->re_current_state_next; \
		      re; \
		      re = re->re_current_state_next ) \
		{ \
			assert(re->re_current_state_next != re); \
			assert(!re->re_current_state_next || re->re_current_state_next->re_current_state_next != re); \
			re->re_is_current = 0; \
		} \
		state->re_current_state_next = NULL; \
		current_states_last = state; \
		assert(verify_current_state(current_states, current_states_last, current_states) && "11"); \
		assert(verify_current_state(current_states, current_states_last, state) && "12"); \
	} \
	else if ( !new_state->re_is_upcoming ) \
	{ \
		if ( !upcoming_states ) \
			upcoming_states = new_state; \
		if ( upcoming_states_last ) \
			upcoming_states_last->re_upcoming_state_next = new_state; \
		upcoming_states_last = new_state; \
		new_state->re_upcoming_state_next = NULL; \
		new_state->re_is_upcoming = 1; \
		for ( size_t m = 0; m < nmatch; m++ ) \
			new_state->re_matches[m] = state->re_matches[m]; \
	} \
} \

int regexec(const regex_t* restrict regex_const,
            const char* restrict string,
            size_t nmatch,
            regmatch_t* restrict pmatch,
            int eflags)
{
	// TODO: Sanitize eflags.

	regex_t* regex = (regex_t*) regex_const;
	pthread_mutex_lock(&regex->re_lock);

	if ( regex->re_cflags & REG_NOSUB )
		nmatch = 0;

	for ( size_t i = 0; i < nmatch; i++ )
	{
		pmatch[i].rm_so = -1;
		pmatch[i].rm_eo = -1;
	}

	if ( regex->re_nsub + 1 < nmatch )
		nmatch = regex->re_nsub + 1;

	int result = REG_NOMATCH;

	struct re* current_states = NULL;
	struct re* current_states_last = NULL;
	struct re* upcoming_states = NULL;
	struct re* upcoming_states_last = NULL;

	regex->re->re_is_current = 0;

	for ( size_t i = 0; true; i++ )
	{
		if ( !regex->re->re_is_current && result == REG_NOMATCH )
		{
			if ( current_states_last )
				current_states_last->re_current_state_next = regex->re;
			else
				current_states = regex->re;
			regex->re->re_current_state_prev = current_states_last;
			regex->re->re_current_state_next = NULL;
			current_states_last = regex->re;
			regex->re->re_is_currently_done = 0;
			regex->re->re_is_current = 1;
			regex->re->re_is_upcoming = 0;
			for ( size_t m = 0; m < nmatch; m++ )
			{
				regex->re->re_matches[m].rm_so = m == 0 ? (regoff_t) i : -1;
				regex->re->re_matches[m].rm_eo = -1;
			}
		}
		char c = string[i];
		assert(verify_current_state(current_states, current_states_last, current_states));
		for ( struct re* state = current_states;
		      state;
		      state = state->re_current_state_next )
		{
			assert(verify_current_state(current_states, current_states_last, current_states));
			assert(verify_current_state(current_states, current_states_last, state));
			bool match = false;
			bool consumed_char = false;
			if ( state->re_type == RE_TYPE_BOL )
			{
				if ( !(eflags & REG_NOTBOL) )
					QUEUE_CURRENT_STATE(state->re_next);
			}
			else if ( state->re_type == RE_TYPE_EOL )
			{
				if ( !(eflags & REG_NOTEOL) && c == '\0' )
					QUEUE_CURRENT_STATE(state->re_next);
			}
			else if ( state->re_type == RE_TYPE_CHAR )
			{
				if ( c != '\0' && state->re_char.c == c )
					QUEUE_UPCOMING_STATE(state->re_next);
			}
			else if ( state->re_type == RE_TYPE_ANY_CHAR )
			{
				if ( c != '\0' )
					QUEUE_UPCOMING_STATE(state->re_next);
			}
			else if ( state->re_type == RE_TYPE_SET )
			{
				unsigned char uc = c;
				if ( c != '\0' && (state->re_set.set[uc / 8] & (1 << (uc % 8))) )
					QUEUE_UPCOMING_STATE(state->re_next);
			}
			else if ( state->re_type == RE_TYPE_SUBEXPRESSION )
			{
				size_t index = state->re_subexpression.index;
				state->re_matches[index].rm_so = i;
				QUEUE_CURRENT_STATE(state->re_next);
			}
			else if ( state->re_type == RE_TYPE_SUBEXPRESSION_END )
			{
				size_t index = state->re_subexpression.index;
				state->re_matches[index].rm_eo = i;
				QUEUE_CURRENT_STATE(state->re_next);
			}
			else if ( state->re_type == RE_TYPE_ALTERNATIVE ||
			          state->re_type == RE_TYPE_OPTIONAL ||
			          state->re_type == RE_TYPE_LOOP )
			{
				assert(verify_current_state(current_states, current_states_last, current_states));
				QUEUE_CURRENT_STATE(state->re_split.re);
				assert(verify_current_state(current_states, current_states_last, current_states));
				QUEUE_CURRENT_STATE(state->re_next);
				assert(verify_current_state(current_states, current_states_last, current_states));
			}
			state->re_is_currently_done = 1;
			if ( match )
			{
				state->re_matches[0].rm_eo = i + consumed_char;
				for ( size_t m = 0; m < nmatch; m++ )
					pmatch[m] = state->re_matches[m];
				result = 0;
				if ( nmatch == 0 )
					break;
			}
		}

		assert(verify_current_state(current_states, current_states_last, current_states));

		for ( struct re* re = current_states; re; re = re->re_current_state_next )
			re->re_is_current = 0;

		if ( nmatch == 0 && result == 0 )
		{
			for ( struct re* re = upcoming_states; re; re = re->re_upcoming_state_next )
				re->re_is_upcoming = 0;
			break;
		}

		current_states = upcoming_states;
		if ( current_states )
			current_states->re_current_state_prev = NULL;
		current_states_last = upcoming_states_last;
		for ( struct re* re = current_states; re; re = re->re_current_state_next )
		{
			re->re_is_currently_done = 0;
			re->re_is_current = 1;
			re->re_is_upcoming = 0;
			re->re_current_state_next = re->re_upcoming_state_next;
			if ( re->re_current_state_next )
				re->re_current_state_next->re_current_state_prev = re;
		}
		upcoming_states = NULL;
		upcoming_states_last = NULL;

		assert(verify_current_state(current_states, current_states_last, current_states));

		eflags |= REG_NOTBOL;

		if ( current_states == NULL && result == 0 )
			break;

		if ( c == '\0' )
			break;
	}

	pthread_mutex_unlock(&regex->re_lock);

	return result;
}
