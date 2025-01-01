/*
 * Copyright (c) 2023, 2025 Jonas 'Sortie' Termansen.
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
 * glob/glob.c
 * Search for paths matching a pattern.
 */

#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <regex.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int strcoll_indirect(const void* a_ptr, const void* b_ptr)
{
	const char* a = *(const char* const*) a_ptr;
	const char* b = *(const char* const*) b_ptr;
	return strcoll(a, b);
}

struct segment
{
	char* prefix;
	size_t prefix_length;
	DIR* dir;
	bool trivial;
	bool leading_period;
	bool match_directory;
	bool done;
	union
	{
		struct
		{
			size_t start;
			size_t length;
		};
		regex_t regex;
	};
};

int glob(const char* restrict pattern,
         int flags,
         int (*errfunc)(const char*, int),
         glob_t* restrict gl)
{
	if ( !(flags & GLOB_DOOFFS) )
		gl->gl_offs = 0;
	if ( !(flags & GLOB_APPEND) )
	{
		gl->gl_pathv = NULL;
		gl->gl_pathc = 0;
	}
	if ( gl->gl_offs == SIZE_MAX )
		return GLOB_NOSPACE;
	size_t initial_pathc = gl->gl_pathc;
	// Reserve room for at least one string and the trailing null to prevent
	// the possibly of late errors in the GLOB_NOCHECK case.
	size_t pathl;
	if ( __builtin_add_overflow(gl->gl_offs, gl->gl_pathc, &pathl) ||
	     __builtin_add_overflow(pathl, 2, &pathl) )
		return GLOB_NOSPACE;
	char** new_pathv = reallocarray(gl->gl_pathv, pathl, sizeof(char*));
	if ( !new_pathv )
		return GLOB_NOSPACE;
	gl->gl_pathv = new_pathv;
	size_t paths_length = gl->gl_pathc + 1;
	// Initialize the leading NULL pointers per GLOB_DOOFFS.
	if ( !(flags & GLOB_APPEND) )
	{
		for ( size_t i = 0; i < gl->gl_offs; i++ )
			gl->gl_pathv[i] = NULL;
	}
	// Parse the pattern into segments where trivial segments are fixed path
	// components that can be directly opened and non-trivial segments require
	// searching a directory for entries that match the pattern.
	struct segment* segments = NULL;
	size_t segments_count = 0;
	size_t segments_length = 0;
	int result = 0;
	for ( size_t offset = 0; pattern[offset] && !result; )
	{
		// Combine multiple trivial path components into a trivial segment, but
		// each non-trivial path component must be its own segment.
		size_t segment_length = 0;
		bool is_trivial = true;
		for ( size_t i = 0; pattern[offset + i]; i++ )
		{
			if ( pattern[offset + i] == '*' ||
			     pattern[offset + i] == '?' ||
			     pattern[offset + i] == '[' )
			{
				if ( segment_length )
					break;
				is_trivial = false;
			}
			if ( pattern[offset + i] == '/' || !pattern[offset + i + 1] )
			{
				segment_length = i + 1;
				if ( !is_trivial )
					break;
			}
		}
		// Grow the list of segments as needed.
		if ( segments_count == segments_length )
		{
			size_t old_length = segments_length ? segments_length : 1;
			struct segment* new_segments =
				reallocarray(segments, old_length, 2 * sizeof(struct segment));
			if ( !new_segments )
			{
				result = GLOB_NOSPACE;
				break;
			}
			segments = new_segments;
			segments_length = 2 * old_length;
		}
		struct segment* segment = &segments[segments_count++];
		segment->match_directory = pattern[offset + segment_length - 1] == '/';
		// Trivial segments just contain the pattern indices to directly open.
		if ( (segment->trivial = is_trivial) )
		{
			segment->start = offset;
			segment->length = segment_length;
		}
		// Non-trivial segments are translated to a regular expression that is
		// compiled right now so it can be efficiently reused during the search.
		else
		{
			// Match a leading period only if the pattern explicitly starts with
			// a period. POSIX requires that leading periods aren't matched by
			// the * and ? and [ operators, but also are not matched by negated
			// patterns like [^a]. It's unspecified whether [.] would match a
			// leading period. Although regular expressions can express such
			// patterns, it's difficult to translate, and it's much easier to
			// just special case the leading period like this.
			segment->leading_period = pattern[offset] == '.';
			char* re = NULL;
			size_t re_size;
			FILE* fp = open_memstream(&re, &re_size);
			bool escaped = false;
			fputc('^', fp);
			// Translate the pattern to an extended regular expression.
			for ( size_t i = 0; fp && i < segment_length; i++ )
			{
				unsigned char c = pattern[offset + i];
				if ( !escaped && c == '*' )
					fputs(".*", fp);
				else if ( !escaped && c == '?' )
					fputs(".", fp);
				else if ( !escaped && c == '[' )
				{
					// The whole character range is passed directly to regcomp
					// so the correct end just has to be found, taking the edge
					// cases into account. POSIX requires using ! instead of ^
					// for character range negations. As an extension, ^ is
					// also just passed directly to regcomp and works.
					const char* expr = pattern + offset + i;
					size_t max = segment_length - i;
					size_t len = 1;
					if ( len < max && (expr[len] == '!' || expr[len] == '^') )
						len++;
					if ( len < max && expr[len] == ']' )
						len++;
					while ( len < max && expr[len] != ']' )
					{
						if ( 2 <= max - len && expr[len] == '[' &&
						     (expr[len + 1] == '.' || expr[len + 1] == '=' ||
						      expr[len + 1] == ':' ) )
						{
							char t = expr[len + 1];
							len += 2;
							while ( 2 <= max - len &&
							        !(expr[len] == t && expr[len + 1] == ']') )
								len++;
							len += max - len < 2 ? max - len : 2;
						}
						else
							len++;
					}
					if ( len < max && expr[len] == ']' )
					{
						for ( size_t n = 0; n <= len; n++ )
						{
							if ( n == 1 && expr[n] == '!' )
								fputc('^', fp);
							else
								fputc((unsigned char) expr[n], fp);
						}
						i += len;
					}
					else
						fputs("\\[", fp);
				}
				else if ( !escaped && c == '\\' && !(flags & GLOB_NOESCAPE) )
					escaped = true;
				else if ( c != '/' )
				{
					if ( c == '\\' || c == '(' || c == ')' || c == '{' ||
					     c == '}' || c == '.' || c == '*' || c == '[' ||
					     c == ']' || c == '^' || c == '$' || c == '+' ||
					     c == '?' || c == '|' )
						fputc('\\', fp);
					fputc(c, fp);
					escaped = false;
				}
			}
			fputc('$', fp);
			if ( !fp || ferror(fp) || fflush(fp) == EOF )
			{
				if ( fp )
					fclose(fp);
				free(re);
				result = GLOB_NOSPACE;
				segments_count--;
				break;
			}
			fclose(fp);
			// Compile and reuse the regular expression for this segment.
			int ret = regcomp(&segment->regex, re, REG_EXTENDED);
			free(re);
			if ( ret )
			{
				result = GLOB_NOSPACE;
				segments_count--;
				break;
			}
		}
		offset += segment_length;
	}
	// Start the search with the first segment.
	if ( !result && segments_count )
	{
		segments[0].prefix = NULL;
		segments[0].prefix_length = 0;
		segments[0].dir = NULL;
		segments[0].done = false;
		// If the first segment is non-trivial then the current working
		// directory needs to be opened and searched.
		if ( !segments[0].trivial && !(segments[0].dir = opendir(".")) )
		{
			if ( errno == ENOMEM )
				result = GLOB_NOSPACE;
			else if ( (errfunc && errfunc(".", errno)) || (flags & GLOB_ERR) )
				result = GLOB_ABORTED;
			else
				segments[0].done = true;
		}
	}
	// Search the filesystem depth first for paths matching the pattern. The
	// segments array is used for the hierarchical state to avoid recursion.
	// Each active segment has a directory currently being searched and yields
	// paths to be explored by the subsequent segment. The last segment adds
	// paths to the output array if they match the pattern. The search is
	// complete when the outermost segment is done or has failed.
	size_t current_segment = 0;
	while ( segments_count &&
	        (current_segment || !(segments[0].done || result) ))
	{
		struct segment* segment = &segments[current_segment];
		// Pop to the the parent segment if the directory has been searched or
		// if an error has happened and the search is aborting.
		if ( segment->done || result )
		{
			free(segment->prefix);
			segment->prefix = NULL;
			if ( segment->dir )
				closedir(segment->dir);
			current_segment--;
			continue;
		}
		char* name;
		size_t name_length;
		unsigned char type = DT_UNKNOWN;
		// A trivial segment yields only the singular path it can match.
		if ( segment->trivial )
		{
			name = strndup(pattern + segment->start, segment->length);
			name_length = segment->length;
			segment->done = true;
		}
		// Search the directory for entries matching the pattern.
		else
		{
			errno = 0;
			struct dirent* entry = readdir(segment->dir);
			if ( !entry )
			{
				const char* path = segment->prefix ? segment->prefix : ".";
				if ( errno == ENOMEM )
					result = GLOB_NOSPACE;
				else if ( errno && ((errfunc && errfunc(path, errno)) ||
				                    (flags & GLOB_ERR)) )
					result = GLOB_ABORTED;
				segment->done = true;
				continue;
			}
			// Skip known non-directories when a directory needs to be found.
			if ( (current_segment + 1 < segments_count ||
			      segment->match_directory) &&
			     entry->d_type != DT_UNKNOWN &&
			     entry->d_type != DT_DIR &&
			     entry->d_type != DT_LNK )
				continue;
			if ( !strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") )
				continue;
			if ( entry->d_name[0] == '.' && !segment->leading_period )
				continue;
			if ( regexec(&segment->regex, entry->d_name, 0, NULL, 0) )
				continue;
			name = strdup(entry->d_name);
			name_length = strlen(entry->d_name);
			type = entry->d_type;
		}
		if ( !name )
		{
			result = GLOB_NOSPACE;
			continue;
		}
		// Append the segment's prefix with the name but keep an extra byte for
		// a possible trailing slash and of course the terminating nul byte.
		size_t size = 0;
		if ( __builtin_add_overflow(segment->prefix_length, name_length,
		                            &size) ||
		     __builtin_add_overflow(size, 1 + 1, &size) )
		{
			free(name);
			result = GLOB_NOSPACE;
			continue;
		}
		char* path = malloc(size);
		if ( !path )
		{
			free(name);
			free(path);
			result = GLOB_NOSPACE;
			continue;
		}
		if ( segment->prefix_length )
			memcpy(path, segment->prefix, segment->prefix_length);
		memcpy(path + segment->prefix_length, name, name_length);
		path[segment->prefix_length + name_length] = '\0';
		int fd = segment->dir ? dirfd(segment->dir) : AT_FDCWD;
		// If this is not the last segment, push to the next segment to search
		// the directory just found.
		if ( current_segment + 1 < segments_count )
		{
			struct segment* next_segment = &segments[current_segment + 1];
			int mode = next_segment->trivial ? O_SEARCH : O_RDONLY;
			int subdirfd = openat(fd, name, mode | O_DIRECTORY | O_CLOEXEC);
			free(name);
			next_segment->dir = subdirfd < 0 ? NULL : fdopendir(subdirfd);
			if ( !next_segment->dir )
			{
				if ( 0 <= subdirfd )
					close(subdirfd);
				if ( errno != ENOENT && errno != ENOTDIR &&
				     ((errfunc && errfunc(path, errno)) || (flags & GLOB_ERR)) )
					result = GLOB_ABORTED;
				free(path);
				continue;
			}
			next_segment->prefix = path;
			next_segment->prefix_length = size - 2;
			// Add a trailing slash to the searched directory entries.
			if ( !segment->trivial )
			{
				next_segment->prefix[next_segment->prefix_length++] = '/';
				next_segment->prefix[next_segment->prefix_length] = '\0';
			}
			next_segment->done = false;
			current_segment++;
			continue;
		}
		// The last segment just needs to output paths if they exist.
		else
		{
			bool want_slash = (flags & GLOB_MARK) || segment->match_directory;
			bool exists = true, is_dir = false;
			// The path is known to already exist for non-trivial segments since
			// it was returned by readdir, but we may need to check if the path
			// is a directory if readdir didn't tell us already.
			if ( !segment->trivial &&
			     (!want_slash || (type != DT_UNKNOWN && type != DT_LNK)) )
				is_dir = type == DT_DIR;
			// Just check if the path exists if we don't add slashes to dirs.
			else if ( !want_slash )
				exists = !faccessat(fd, name, F_OK, AT_SYMLINK_NOFOLLOW);
			// Otherwise use the slower stat operation to obtain the inode type.
			else
			{
				struct stat st;
				exists = !fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW);
				if ( want_slash && S_ISLNK(st.st_mode) )
					fstatat(fd, name, &st, 0);
				is_dir = S_ISDIR(st.st_mode);
			}
			free(name);
			if ( segment->match_directory && !is_dir )
			{
				free(path);
				continue;
			}
			if ( want_slash && is_dir && path[size - 3] != '/' )
				path[size - 2] = '/', path[size - 1] = '\0';
			if ( !exists )
			{
				if ( errno != ENOENT &&
				     ((errfunc && errfunc(path, errno)) || (flags & GLOB_ERR)) )
					result = GLOB_ABORTED;
				free(path);
				continue;
			}
			// Grow the output array as needed.
			if ( gl->gl_pathc == paths_length )
			{
				size_t old_pathc = gl->gl_pathc ? gl->gl_pathc : 1;
				if ( __builtin_mul_overflow(2, old_pathc, &pathl) ||
				     __builtin_add_overflow(gl->gl_offs, pathl, &pathl) ||
					 __builtin_add_overflow(1, pathl, &pathl) ||
				     !(new_pathv = reallocarray(gl->gl_pathv, pathl,
				                                sizeof(char*))) )
				{
					free(path);
					result = GLOB_NOSPACE;
					continue;
				}
				gl->gl_pathv = new_pathv;
				paths_length = old_pathc * 2;
			}
			gl->gl_pathv[gl->gl_offs + gl->gl_pathc++] = path;
		}
	}
	// Clean up the segments and free the compiled regular expressions.
	if ( segments_count && segments[0].dir )
		closedir(segments[0].dir);
	for ( size_t i = 0; i < segments_count; i++ )
		if ( !segments[i].trivial )
			regfree(&segments[i].regex);
	free(segments);
	// Output the input pattern if nothing matched when GLOB_NOCHECK.
	if ( !result && gl->gl_pathc == initial_pathc )
	{
		if ( (flags & GLOB_NOCHECK) )
		{
			if ( (gl->gl_pathv[gl->gl_offs] = strdup(pattern)) )
				gl->gl_pathc = 1;
			else
				result = GLOB_NOSPACE;
		}
		else
			result = GLOB_NOMATCH;
	}
	// Sort the new entries per LC_COLLATE per POSIX.
	if ( !(flags & GLOB_NOSORT) )
		qsort(gl->gl_pathv + gl->gl_offs + initial_pathc,
		      gl->gl_pathc - initial_pathc, sizeof(char*),
		      strcoll_indirect);
	gl->gl_pathv[gl->gl_offs + gl->gl_pathc] = NULL;
	return result;
}
