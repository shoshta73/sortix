/*
 * Copyright (c) 2015, 2016, 2021, 2022, 2024 Jonas 'Sortie' Termansen.
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
 * pstree.c
 * Lists processes in a nice tree.
 */

#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <psctl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char* last_basename(const char* path)
{
	const char* result = path;
	for ( size_t i = 0; path[i]; i++ )
		if ( path[i] == '/' && path[i + 1] != '/' && path[i + 1] )
			result = path + i + 1;
	return result;
}

static char* get_program_path_of_pid(pid_t pid)
{
	struct psctl_program_path ctl;
	memset(&ctl, 0, sizeof(ctl));
	ctl.buffer = NULL;
	ctl.size = 0;
	if ( psctl(pid, PSCTL_PROGRAM_PATH, &ctl) < 0 )
		return NULL;
	while ( true )
	{
		char* new_buffer = (char*) realloc(ctl.buffer, ctl.size);
		if ( !new_buffer )
			return free(ctl.buffer), (char*) NULL;
		ctl.buffer = new_buffer;
		if ( psctl(pid, PSCTL_PROGRAM_PATH, &ctl) == 0 )
			return ctl.buffer;
		if ( errno != ERANGE )
			return free(ctl.buffer), (char*) NULL;
	}
}

static void pstree(pid_t pid,
                   const char* prefix,
                   bool continuation,
                   bool show_pgid,
                   bool show_pid,
                   bool show_sid,
                   bool show_init)
{
	while ( pid != -1 )
	{
		struct psctl_stat psst;
		if ( psctl(pid, PSCTL_STAT, &psst) < 0 )
		{
			if ( errno != ESRCH )
				warn("psctl: PSCTL_STAT: [%" PRIiPID "]", pid);
			return;
		}
		char* full_path = get_program_path_of_pid(pid);
		const char* path = last_basename(full_path ? full_path : "<unknown>");
		if ( !continuation )
			fputs(prefix, stdout);
		if ( prefix[0] )
		{
			if ( continuation )
				fputs("─", stdout);
			else
				fputs(" ", stdout);
			if ( continuation )
				fputs(psst.ppid_next == -1 ? "─" : "┬", stdout);
			else
				fputs(psst.ppid_next == -1 ? "└" : "│", stdout);
			fputs("─", stdout);
		}
		size_t item_length = printf("%s", path);
		if ( show_pid || show_pgid || show_sid || show_init )
		{
			size_t sep = 0;
			item_length += printf("(");
			if ( show_pid )
				item_length += (sep = printf("%" PRIiPID, pid));
			if ( show_pgid && sep )
				item_length += printf(","), sep = 0;
			if ( show_pgid )
				item_length += (sep = printf("%" PRIiPID, psst.pgid));
			if ( show_sid && sep )
				item_length += printf(","), sep = 0;
			if ( show_sid )
				item_length += (sep = printf("%" PRIiPID, psst.sid));
			if ( show_init && sep )
				item_length += printf(","), sep = 0;
			if ( show_init )
				item_length += (sep = printf("%" PRIiPID, psst.init));
			item_length += printf(")");
		}
		if ( psst.ppid_first != -1 )
		{
			char* new_prefix;
			if ( prefix[0] )
			{
				const char* drawing = psst.ppid_next != -1 ? " │ " : "   ";
				size_t drawing_length = strlen(drawing);
				size_t prefix_length = strlen(prefix);
				size_t new_prefix_length =
					prefix_length + drawing_length + item_length;
				if ( !(new_prefix = (char*) malloc(new_prefix_length + 1)) )
					err(1, "malloc");
				memcpy(new_prefix, prefix, prefix_length);
				memcpy(new_prefix + prefix_length, drawing, drawing_length);
				for ( size_t i = 0; i < item_length; i++ )
					new_prefix[prefix_length + drawing_length + i] = ' ';
				new_prefix[prefix_length + drawing_length + item_length] = '\0';
			}
			else
			{
				if ( !(new_prefix = (char*) malloc(item_length + 1)) )
					err(1, "malloc");
				for ( size_t i = 0; i < item_length; i++ )
					new_prefix[i] = ' ';
				new_prefix[item_length] = '\0';
			}
			pstree(psst.ppid_first, new_prefix, true, show_pgid, show_pid,
			       show_sid, show_init);
			free(new_prefix);
		}
		else
			printf("\n");
		free(full_path);
		continuation = false;
		pid = psst.ppid_next;
	}
}

int main(int argc, char* argv[])
{
	bool show_pgid = false;
	bool show_pid = false;
	bool show_sid = false;
	bool show_init = false;

	int opt;
	while ( (opt = getopt(argc, argv, "gips")) != -1 )
	{
		switch ( opt )
		{
		case 'g': show_pgid = true; break;
		case 'i': show_init = true; break;
		case 'p': show_pid = true; break;
		case 's': show_sid = true; break;
		default: return 1;
		}
	}

	if ( optind < argc )
		errx(1, "extra operand: %s", argv[optind]);

	pstree(1, "", true, show_pgid, show_pid, show_sid, show_init);

	return ferror(stdout) || fflush(stdout) == EOF ? 1 : 0;
}
