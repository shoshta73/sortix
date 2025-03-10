/*
 * Copyright (c) 2013, 2015, 2016, 2021, 2024 Jonas 'Sortie' Termansen.
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
 * find.c
 * Locate files and directories.
 */

#include <sys/stat.h>
#include <sys/wait.h>

#include <assert.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <grp.h>
#include <inttypes.h>
#include <libgen.h>
#include <limits.h>
#include <pwd.h>
#include <regex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum expr_kind
{
	EXPR_PAREN,
	EXPR_NOT,
	EXPR_AND,
	EXPR_OR,
	EXPR_DELETE,
	EXPR_EXEC,
	EXPR_GROUP,
	EXPR_INUM,
	EXPR_LINKS,
	EXPR_NAME,
	EXPR_NEWER,
	EXPR_NOGROUP,
	EXPR_NOUSER,
	EXPR_PATH,
	EXPR_PERM,
	EXPR_PRINT,
	EXPR_PRUNE,
	EXPR_REGEX,
	EXPR_SIZE,
	EXPR_TIME,
	EXPR_TRUE,
	EXPR_TYPE,
	EXPR_USER,
};

struct expr_paren
{
	struct expr* expr;
};

struct expr_not
{
	struct expr* expr;
};

struct expr_and
{
	struct expr* a;
	struct expr* b;
};

struct expr_or
{
	struct expr* a;
	struct expr* b;
};

struct expr_exec
{
	int argc;
	const char* const* argv;
	bool ok;
	bool plus;
	bool dir;
};

struct expr_group
{
	gid_t gid;
};

struct expr_inum
{
	ino_t ino;
	int cmp;
};

struct expr_links
{
	nlink_t n;
	int cmp;
};

struct expr_name
{
	const char* pattern;
	int flags;
};

struct expr_newer
{
	struct timespec when;
	char t;
};

struct expr_path
{
	const char* pattern;
};

struct expr_perm
{
	mode_t mode;
	bool least;
};

struct expr_print
{
	char delim;
};

struct expr_regex
{
	regex_t re;
};

struct expr_size
{
	bool bytes;
	off_t size;
	int cmp;
};

struct expr_time
{
	time_t n;
	time_t interval;
	int cmp;
	char t;
};

struct expr_type
{
	char c;
};

struct expr_user
{
	uid_t uid;
};

struct expr
{
	enum expr_kind kind;
	union
	{
		struct expr_paren expr_paren;
		struct expr_not expr_not;
		struct expr_and expr_and;
		struct expr_or expr_or;
		struct expr_exec expr_exec;
		struct expr_group expr_group;
		struct expr_inum expr_inum;
		struct expr_links expr_links;
		struct expr_name expr_name;
		struct expr_newer expr_newer;
		struct expr_path expr_path;
		struct expr_perm expr_perm;
		struct expr_print expr_print;
		struct expr_regex expr_regex;
		struct expr_size expr_size;
		struct expr_time expr_time;
		struct expr_type expr_type;
		struct expr_user expr_user;
	};
	struct expr* parent;
};

enum symderef
{
	SYMDEREF_NONE,
	SYMDEREF_ARGUMENTS,
	SYMDEREF_ALWAYS,
};

static struct timespec startup;

static char* join_paths(const char* a, const char* b)
{
	size_t a_len = strlen(a);
	bool has_slash = (a_len && a[a_len-1] == '/') || b[0] == '/';
	char* result;
	if ( (has_slash && asprintf(&result, "%s%s", a, b) < 0) ||
	     (!has_slash && asprintf(&result, "%s/%s", a, b) < 0) )
		return NULL;
	return result;
}

static bool is_octal_string(const char* str)
{
	if ( !str[0] )
		return false;
	for ( size_t i = 0; str[i]; i++ )
		if ( !('0' <= str[i] && str[i] <= '7') )
			return false;
	return true;
}

static mode_t execute_modespec(const char* str,
                               mode_t mode,
                               mode_t type,
                               mode_t umask)
{
	if ( is_octal_string(str) )
	{
		errno = 0;
		uintmax_t input = strtoumax((char*) str, NULL, 8);
		if ( errno == ERANGE )
			return (mode_t) -1;
		if ( input & ~((uintmax_t) 07777) )
			return (mode_t) -1;
		return (mode_t) input;
	}

	size_t index = 0;
	do
	{
		mode_t who_mask = 01000;
		while ( true )
		{
			if ( str[index] == 'u' && (index++, true) )
				who_mask |= 04700;
			else if ( str[index] == 'g' && (index++, true) )
				who_mask |= 02070;
			else if ( str[index] == 'o' && (index++, true) )
				who_mask |= 00007;
			else if ( str[index] == 'a' && (index++, true) )
				who_mask |= 06777;
			else
				break;
		}
		if ( !(who_mask & 0777) )
			who_mask |= 06777 & ~umask;
		do
		{
			char op;
			switch ( (op = str[index++]) )
			{
			case '+': break;
			case '-': break;
			case '=': break;
			default: return (mode_t) -1;
			};
			mode_t operand = 0;
			if ( str[index] == 'u' || str[index] == 'g' || str[index] == 'o' )
			{
				char permcopy = str[index++];
				switch ( permcopy )
				{
				case 'u': operand = mode >> 6 & 07; break;
				case 'g': operand = mode >> 3 & 07; break;
				case 'o': operand = mode >> 0 & 07; break;
				default: __builtin_unreachable();
				};
				operand = operand << 0 | operand << 3 | operand << 6;
				switch ( permcopy )
				{
				case 'u': if ( mode & 04000) operand |= 06000; break;
				case 'g': if ( mode & 02000) operand |= 06000; break;
				};
				who_mask &= ~((mode_t) 01000);
			}
			else
			{
				bool unknown = false;
				do
				{
					switch ( str[index] )
					{
					case 'r': operand |= 00444; break;
					case 'w': operand |= 00222; break;
					case 'x': operand |= 00111; break;
					case 'X':
						if ( S_ISDIR(type) || (mode & 0111) )
							operand |= 00111;
						break;
					case 's': operand |= 06000; break;
					case 't': operand |= 00000; break;
					default: unknown = true; break;
					}
				} while ( !unknown && (index++, true) );
			}
			switch ( op )
			{
			case '+': mode |= (operand & who_mask); break;
			case '-': mode &= ~(operand & who_mask); break;
			case '=': mode = (mode & ~who_mask) | (operand & who_mask); break;
			default: __builtin_unreachable();
			}
		} while ( str[index] == '+' ||
		          str[index] == '-' ||
		          str[index] == '=' );
	} while ( str[index] == ',' && (index++, true) );
	if ( str[index] )
		return (mode_t) -1;
	return mode;
}

static const struct timespec* pick_time(const struct stat* ts, char t)
{
	switch ( t )
	{
	case 'a': return &ts->st_atim;
	case 'c': return &ts->st_ctim;
	case 'm': return &ts->st_mtim;
	default: __builtin_unreachable();
	}
}

static time_t seconds_age(const struct timespec* ts)
{
	time_t seconds = startup.tv_sec - ts->tv_sec;
	if ( startup.tv_nsec < ts->tv_nsec )
		seconds--;
	if ( seconds < 0 )
		seconds = 0;
	return seconds;
}

const int SUCCESS = 1 << 0;
const int PRUNED = 1 << 1;

static int evaluate(const struct expr* expr,
                    int dirfd,
                    const char* name,
                    const char* relpath,
                    const char* path,
                    struct stat* st,
                    size_t depth,
                    size_t mindepth)
{
	const struct expr* original_expr = expr;
	int result = SUCCESS;
	if ( depth < mindepth )
		return result;
	while ( true )
	{
		bool value;
		if ( expr->kind == EXPR_PAREN )
		{
			assert(expr->expr_paren.expr->parent == expr);
			expr = expr->expr_paren.expr;
			continue;
		}
		else if ( expr->kind == EXPR_NOT )
		{
			assert(expr->expr_not.expr->parent == expr);
			expr = expr->expr_not.expr;
			continue;
		}
		else if ( expr->kind == EXPR_AND )
		{
			assert(expr->expr_and.a->parent == expr);
			assert(expr->expr_and.b->parent == expr);
			expr = expr->expr_and.a;
			continue;
		}
		else if ( expr->kind == EXPR_OR )
		{
			assert(expr->expr_or.a->parent == expr);
			assert(expr->expr_or.b->parent == expr);
			expr = expr->expr_or.a;
			continue;
		}
		else if ( expr->kind == EXPR_DELETE )
		{
			int flags = S_ISDIR(st->st_mode) ? AT_REMOVEDIR : 0;
			if ( !(value = !unlinkat(dirfd, relpath, flags)) )
			{
				warn("-delete: %s", path);
				result &= ~SUCCESS;
			}
		}
		else if ( expr->kind == EXPR_EXEC )
		{
			// Flush all buffered output before the -ok prompt, and
			// before fork() to avoid duplicate output
			if ( fflush(NULL) != 0 )
			{
				warn("fflush");
				result &= ~SUCCESS;
			}
			bool is_ok = true;
			if ( expr->expr_exec.ok )
			{
				fprintf(stderr, "< %s ... %s > ? ", expr->expr_exec.argv[0],
				        path);
				int ic;
				is_ok = (ic = getchar()) == 'y' && (ic = getchar()) == '\n';
				while ( ic != EOF && ic != '\n' )
					ic = getchar();
				if ( ic != '\n' )
					fputc('\n', stderr);
			}
			pid_t pid;
			if ( !is_ok )
				value = false;
			else if ( (pid = fork()) < 0 )
			{
				warn("fork");
				value = false;
			}
			else if ( !pid )
			{
				const char* param = expr->expr_exec.dir ? relpath : path;
				size_t param_size = strlen(param);
				int argc = expr->expr_exec.argc;
				char** argv = calloc(argc + 1, sizeof(char*));
				if ( !argv )
					err(1, "malloc");
				for ( int i = 0; i < argc; i++ )
				{
					const char* arg = expr->expr_exec.argv[i];
					size_t occurences = 0;
					size_t len;
					for ( len = 0; arg[len]; len++ )
					{
						if ( arg[len] == '{' && arg[len + 1] == '}' )
						{
							occurences++;
							len++;
						}
					}
					if ( occurences )
					{
						size_t size = len - 2 * occurences + 1;
						size_t param_sizes;
						if ( __builtin_mul_overflow(param_size, occurences,
						                            &param_sizes) ||
						     __builtin_add_overflow(param_sizes, size, &size) )
						{
							errno = ENOMEM;
							err(1, "malloc");
						}
						char* new_arg = malloc(size);
						if ( !new_arg )
							err(1, "malloc");
						size_t o = 0;
						for ( size_t n = 0; n < len; n++ )
						{
							if ( arg[n] == '{' && arg[n + 1] == '}' )
							{
								memcpy(new_arg + o, param, param_size);
								o += param_size;
								n++;
							}
							else
								new_arg[o++] = arg[n];
						}
						new_arg[o] = '\0';
						arg = new_arg;
					}
					argv[i] = (char*) arg;
				}
				if ( is_ok )
				{
					close(0);
					if ( open("/dev/null", O_RDONLY) != 0 )
						err(1, "/dev/null");
				}
				argv[argc] = NULL;
				if ( expr->expr_exec.dir &&
				     dirfd != AT_FDCWD &&
				     fchdir(dirfd) < 0 )
					err(1, "chdir into directory containing: %s", path);
				execvp(argv[0], argv);
				err(1, "%s", argv[0]);
			}
			else
			{
				int status;
				waitpid(pid, &status, 0);
				bool success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
				if ( expr->expr_exec.plus )
				{
					// TODO: Merge multiple paths into single invocations.
					value = true;
					if ( !success )
						result &= ~SUCCESS;
				}
				else
					value = success;
			}
		}
		else if ( expr->kind == EXPR_GROUP )
			value = st->st_gid == expr->expr_group.gid;
		else if ( expr->kind == EXPR_INUM)
			value = expr->expr_inum.cmp < 0 ?
			        st->st_ino < expr->expr_inum.ino:
			        expr->expr_inum.cmp > 0 ?
			        st->st_ino > expr->expr_inum.ino :
			        st->st_ino == expr->expr_inum.ino;
		else if ( expr->kind == EXPR_LINKS )
			value = expr->expr_links.cmp < 0 ?
			        st->st_nlink < expr->expr_links.n :
			        expr->expr_links.cmp > 0 ?
			        st->st_nlink > expr->expr_links.n :
			        st->st_nlink == expr->expr_links.n;
		else if ( expr->kind == EXPR_NAME )
			value = fnmatch(expr->expr_name.pattern, name,
			                expr->expr_name.flags) == 0;
		else if ( expr->kind == EXPR_NEWER )
		{
			const struct timespec* ts = pick_time(st, expr->expr_newer.t);
			value = expr->expr_newer.when.tv_sec < ts->tv_sec ||
			        (expr->expr_newer.when.tv_sec == ts->tv_sec &&
			         expr->expr_newer.when.tv_nsec < ts->tv_nsec);
		}
		else if ( expr->kind == EXPR_NOGROUP )
		{
			errno = 0;
			value = !getgrgid(st->st_gid);
			if ( errno )
				err(1, "getgrgid");
		}
		else if ( expr->kind == EXPR_NOUSER )
		{
			errno = 0;
			value = !getpwuid(st->st_uid);
			if ( errno )
				err(1, "getpwuid");
		}
		else if ( expr->kind == EXPR_PATH )
			value = fnmatch(expr->expr_path.pattern, path, 0) == 0;
		else if ( expr->kind == EXPR_PERM )
			value = (st->st_mode & (expr->expr_perm.least ?
			                        expr->expr_perm.mode :
			                        07777)) == expr->expr_perm.mode;
		else if ( expr->kind == EXPR_PRINT )
		{
			if ( printf("%s%c", path, expr->expr_print.delim) < 0 )
				err(1, "stdout");
			value = true;
		}
		else if ( expr->kind == EXPR_PRUNE )
		{
			result |= PRUNED;
			value = true;
		}
		else if ( expr->kind == EXPR_REGEX )
		{
			regmatch_t match;
			value = !regexec(&expr->expr_regex.re, path, 1, &match, 0) &&
			        match.rm_so == 1 && (size_t) match.rm_eo == strlen(path);
		}
		else if ( expr->kind == EXPR_SIZE )
		{
			off_t size = expr->expr_size.bytes ?
			             st->st_size :
			             st->st_size / 512 + (st->st_size & 511 ? 1 : 0);
			value = expr->expr_size.cmp < 0 ? size < expr->expr_size.size :
			        expr->expr_size.cmp > 0 ? size > expr->expr_size.size :
			        size == expr->expr_size.size;
		}
		else if ( expr->kind == EXPR_TIME )
		{
			const struct timespec* ts = pick_time(st, expr->expr_time.t);
			time_t age = seconds_age(ts) / expr->expr_time.interval;
			value = expr->expr_time.cmp < 0 ? age < expr->expr_time.n :
			        expr->expr_time.cmp > 0 ? age > expr->expr_time.n :
			        age == expr->expr_time.n;
		}
		else if ( expr->kind == EXPR_TRUE )
			value = true;
		else if ( expr->kind == EXPR_TYPE )
			value = expr->expr_type.c == 'b' ? S_ISBLK(st->st_mode) :
			        expr->expr_type.c == 'c' ? S_ISCHR(st->st_mode) :
			        expr->expr_type.c == 'd' ? S_ISDIR(st->st_mode) :
			        expr->expr_type.c == 'f' ? S_ISREG(st->st_mode) :
			        expr->expr_type.c == 'l' ? S_ISLNK(st->st_mode) :
			        expr->expr_type.c == 'p' ? S_ISFIFO(st->st_mode) :
			        expr->expr_type.c == 's' ? S_ISSOCK(st->st_mode) : 0;
		else if ( expr->kind == EXPR_USER )
			value = st->st_uid == expr->expr_user.uid;
		else
			value = false;
		// Continue evaluating the parent expression.
		while ( true )
		{
			if ( expr == original_expr )
				return result;
			const struct expr* parent = expr->parent;
			if ( parent->kind == EXPR_PAREN )
				expr = parent;
			else if ( parent->kind == EXPR_NOT )
			{
				value = !value;
				expr = parent;
			}
			else if ( parent->kind == EXPR_AND )
			{
				if ( value && expr == parent->expr_and.a )
				{
					expr = parent->expr_and.b;
					break;
				}
				else
					expr = parent;
			}
			else if ( parent->kind == EXPR_OR )
			{
				if ( !value && expr == parent->expr_or.a )
				{
					expr = parent->expr_or.b;
					break;
				}
				else
					expr = parent;
			}
			else
				__builtin_unreachable();
		}
	}
}

// Stat through a symlink if any, and if that fails, stat the symlink itself.
static int fstatat_symlink(int dirfd,
                           const char* path,
                           struct stat* st,
                           int flags)
{
	if ( !fstatat(dirfd, path, st, flags) )
		return 0;
	if ( flags & AT_SYMLINK_NOFOLLOW )
		return -1;
	int old_errno = errno;
	if ( fstatat(dirfd, path, st, flags | AT_SYMLINK_NOFOLLOW) < 0 ||
	     !S_ISLNK(st->st_mode) )
	{
		errno = old_errno;
		return -1;
	}
	return 0;
}

// Like scandir on an existing DIR, but doesn't sort and omits . and .. entries.
static int list_directory(DIR* dir, struct dirent*** entries_out)
{
	struct dirent** entries = malloc(sizeof(struct dirent*));
	if ( !entries )
		return 0;
	size_t count = 0;
	size_t length = 1;
	struct dirent* entry;
	while ( (errno = 0, entry = readdir(dir)) )
	{
		if ( !strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") )
			continue;
		size_t name_length = strlen(entry->d_name);
		size_t entry_size = offsetof(struct dirent, d_name) + name_length + 1;
		struct dirent* new_entry = calloc(1, entry_size);
		if ( !new_entry )
			break;
		memcpy(new_entry, entry, entry_size);
		if ( count == length )
		{
			size_t size;
			if ( __builtin_mul_overflow(length, 2 * sizeof(struct dirent*),
			                            &size) )
			{
				errno = ENOMEM;
				break;
			}
			struct dirent** new_entries = realloc(entries, size);
			if ( !new_entries )
			{
				free(new_entry);
				break;
			}
			entries = new_entries;
			length *= 2;
		}
		if ( count == (unsigned int) INT_MAX )
		{
			errno = EFBIG;
			break;
		}
		entries[count++] = new_entry;
	}
	if ( errno )
	{
		for ( size_t i = 0; i < count; i++ )
			free(entries[i]);
		free(entries);
		return -1;
	}
	*entries_out = entries;
	return (int) count;
}

// Partially perform a path traversal with only a single path element left.
// This is different from dirname + baseline as it preserves trailing slashes.
static int open_parent_directory(int dirfd, const char* path, size_t* offset)
{
	size_t last_element = SIZE_MAX;
	for ( size_t i = 0; path[i]; i++ )
		if ( path[i] == '/' && path[i + 1] && path[i + 1] != '/' )
			last_element = i + 1;
	if ( last_element == SIZE_MAX && path[0] != '/' )
		return *offset = 0, dirfd;
	char* dir = strndup(path, last_element);
	if ( !dir )
		return -1;
	int fd = openat(dirfd, dir, O_RDONLY | O_DIRECTORY);
	free(dir);
	if ( fd < 0 )
		return -1;
	if ( last_element == SIZE_MAX )
		last_element = 0;
	return *offset = last_element, fd;
}

struct state
{
	struct state* parent;
	int dirfd;
	const char* name;
	const char* relpath;
	const char* path;
	unsigned char type;
	size_t depth;
	int flags;
	struct stat st;
	int fd;
	DIR* dir;
	struct dirent** entries;
	int num_entries;
	int i;
	bool failed_open;
	bool has_stat;
};

static bool find(const struct expr* expr,
                 struct state* state,
                 bool depth,
                 enum symderef symderef,
                 bool xdev,
                 bool mount,
                 size_t mindepth,
                 size_t maxdepth)
{
	// Ensure -execdir and such always have a single relative path element.
	size_t slash_at = strcspn(state->relpath, "/");
	if ( state->relpath[slash_at + strspn(state->relpath + slash_at, "/")] ||
	     (state->dirfd == AT_FDCWD && state->relpath[0] == '/') )
	{
		size_t offset;
		int new_dirfd =
			open_parent_directory(state->dirfd, state->relpath, &offset);
		if ( new_dirfd == -1 )
			err(1, "%s", state->path);
		struct state new_state = {0};
		new_state.dirfd = new_dirfd;
		new_state.name = state->name;
		new_state.relpath = state->relpath + offset;
		new_state.path = state->path;
		new_state.type = state->type;
		bool result = find(expr, &new_state, depth, symderef, xdev, mount,
		                   mindepth, maxdepth);
		if ( new_dirfd != state->dirfd )
			close(new_dirfd);
		return result;
	}

	// Walk the filesystem with recursive state in the heap, handling one
	// filesystem entity per loop iteration, continuing with the parent
	// directory afterwards.
	state->flags = SUCCESS;
	while ( true )
	{
		// If back to a parent directory, continue reading it.
		if ( state->dir )
			goto loop;

		// Stat the filesystem entity; unless it's a known directory, in which
		// case the directory is opened instead and fstat'd afterwards (but
		// we come back here if the directory opening fails).
		if ( !state->has_stat &&
		     (state->type != DT_DIR ||
		      state->failed_open ||
		      maxdepth <= state->depth) )
		{
			if ( fstatat_symlink(state->dirfd, state->relpath, &state->st,
			                     symderef == SYMDEREF_NONE ?
			                     AT_SYMLINK_NOFOLLOW : 0) < 0 )
			{
				warn("%s", state->path);
				state->flags &= ~SUCCESS;
				goto next;
			}
			state->has_stat = true;
			if ( (xdev || mount) && state->parent &&
				 state->st.st_dev != state->parent->st.st_dev )
				goto next;
		}

		// Evaluate non-directories and directories that couldn't be opened.
		if ( (state->has_stat ?
		     !S_ISDIR(state->st.st_mode) :
		     state->type != DT_DIR) ||
		     state->failed_open ||
		     maxdepth <= state->depth )
		{
			int old_errno = errno;
			bool success =
				evaluate(expr, state->dirfd, state->name, state->relpath,
				         state->path, &state->st, state->depth, mindepth)
				& SUCCESS;
			if ( state->failed_open )
			{
				errno = old_errno;
				warn("%s", state->path);
				success = false;
			}
			if ( !success )
				state->flags &= ~SUCCESS;
			goto next;
		}

		// Open directories.
		if ( !state->dir &&
		     (state->has_stat ?
		      S_ISDIR(state->st.st_mode) :
			  state->type == DT_DIR) )
		{
			state->fd = openat(state->dirfd, state->relpath,
			                   O_RDONLY | O_CLOEXEC | O_DIRECTORY |
			                   (symderef == SYMDEREF_NONE ? O_NOFOLLOW : 0));
			if ( state->fd < 0 )
			{
				state->failed_open = true;
				continue;
			}
			if ( symderef == SYMDEREF_ARGUMENTS )
				symderef = SYMDEREF_NONE;
			fstat(state->fd, &state->st);
			state->has_stat = true;
			bool is_mount_point =
				state->parent && state->st.st_dev != state->parent->st.st_dev;
			if ( mount && is_mount_point )
			{
				close(state->fd);
				goto next;
			}
			for ( struct state* s = state->parent; s; s = s->parent )
			{
				if ( state->st.st_dev == s->st.st_dev &&
				     state->st.st_ino == s->st.st_ino )
				{
					warnx("Filesystem loop detected: %s is %s", state->path,
					      s->path);
					state->flags &= ~SUCCESS;
					goto next;
				}
			}
			if ( !depth )
				state->flags = evaluate(expr, state->dirfd, state->name,
				                        state->relpath, state->path,
				                        &state->st, state->depth, mindepth);
			state->dir = fdopendir(state->fd);
			if ( !state->dir )
			{
				warn("fdopendir: %s", state->path);
				close(state->fd);
				state->flags &= ~SUCCESS;
				goto next;
			}
			if ( !(state->flags & PRUNED) && !(xdev && is_mount_point) )
			{
				state->num_entries =
					list_directory(state->dir, &state->entries);
				if ( state->num_entries < 0 )
				{
					warn("readdir: %s", state->path);
					state->flags &= ~SUCCESS;
					goto next;
				}
			}
		}

		// Recurse on a directory entry if any.
		if ( state->dir )
		{
		loop:
			if ( state->i < state->num_entries )
			{
				struct dirent* entry = state->entries[state->i++];
				char* new_path = join_paths(state->path, entry->d_name);
				if ( !new_path )
					err(1, "malloc");
				struct state* new_state = calloc(1, sizeof(struct state));
				if ( !new_state )
					err(1, "malloc");
				new_state->parent = state;
				new_state->dirfd = state->fd;
				new_state->name = entry->d_name;
				new_state->relpath = entry->d_name;
				new_state->path = new_path;
				new_state->type = entry->d_type;
				new_state->depth = state->depth + 1;
				new_state->flags = SUCCESS;
				state = new_state;
				continue;
			}
			if ( depth &&
			     !(evaluate(expr, state->dirfd, state->name, state->relpath,
			                state->path, &state->st, state->depth, mindepth)
			       & SUCCESS) )
				state->flags &= ~SUCCESS;
		}

	next:
		// Clean up and continue with the parent directory if any.
		if ( state->dir )
		{
			for ( int i = 0; i < state->num_entries; i++ )
				free(state->entries[i]);
			free(state->entries);
			state->entries = NULL;
			state->num_entries = 0;
			closedir(state->dir);
			state->dir = NULL;
		}
		struct state* parent = state->parent;
		if ( !parent )
			break;
		if ( !(state->flags & SUCCESS) )
			parent->flags &= ~SUCCESS;
		free((char*) state->path);
		free(state);
		state = parent;
	}
	return state->flags & SUCCESS;
}

static void compact_arguments(int* argc, char*** argv)
{
	for ( int i = 0; i < *argc; i++ )
	{
		while ( i < *argc && !(*argv)[i] )
		{
			for ( int n = i; n < *argc; n++ )
				(*argv)[n] = (*argv)[n+1];
			(*argc)--;
		}
	}
}

struct parse_state
{
	struct parse_state* outer;
	struct expr** insert_and;
	struct expr** insert_or;
};

int main(int argc, char* argv[])
{
	bool depth = false;
	bool ere = false;
	bool mount = false;
	enum symderef symderef = SYMDEREF_NONE;
	bool xdev = false;
	size_t mindepth = 0;
	size_t maxdepth = SIZE_MAX;

	for ( int i = 1; i < argc; i++ )
	{
		const char* arg = argv[i];
		if ( arg[0] != '-' || !arg[1] )
			continue;
		// Handle predicates whose prefix are valid short options.
		if ( !strcmp(arg, "-depth") ||
		     !strcmp(arg, "-delete") ||
		     !strcmp(arg, "-xdev") )
			break;
		argv[i] = NULL;
		if ( !strcmp(arg, "--") )
			break;
		if ( arg[1] != '-' )
		{
			const char* orig_arg = arg;
			char c;
			bool do_break = false;
			while ( (c = *++arg) ) switch ( c )
			{
			case 'd': depth = true; break;
			case 'E': ere = true; break;
			case 'H': symderef = SYMDEREF_ARGUMENTS; break;
			case 'L': symderef = SYMDEREF_ALWAYS; break;
			case 'P': symderef = SYMDEREF_NONE; break;
			case 'x': xdev = true; break;
			default:
				if ( orig_arg + 1 == arg )
				{
					argv[i] = (char*) orig_arg;
					arg = "?";
					do_break = true;
					break;
				}
				errx(1, "unknown option -- '%c'", c);
			}
			if ( do_break )
				break;
		}
		else
			errx(1, "unknown option: %s", arg);
	}

	compact_arguments(&argc, &argv);

	// Locate the start of the expression in argv.
	int predicates_offset = 1;
	while ( predicates_offset < argc &&
	        !(argv[predicates_offset][0] == '-' &&
	          argv[predicates_offset][1]) &&
	        strcmp(argv[predicates_offset], "!") != 0 &&
	        strcmp(argv[predicates_offset], "(") != 0 )
		predicates_offset++;

	struct expr* root = NULL;
	struct expr** insert_at = &root;
	struct expr* insert_at_parent = NULL;
	struct parse_state outermost_parse_state = { 0 };
	struct parse_state* parse_state = &outermost_parse_state;
	parse_state->outer = NULL;
	parse_state->insert_and = &root;
	parse_state->insert_or = &root;

	bool found_action = false;
	bool found_delete = false;
	bool found_prune = false;

	// Parse the expression.
	for ( int i = predicates_offset; i < argc; i++ )
	{
		const char* arg = argv[i];
		struct expr* subexpr = calloc(1, sizeof(struct expr));
		if ( !subexpr )
			err(1, "malloc");
		struct expr** next_insert_at = NULL;
		if ( !strcmp(arg, "(") )
		{
			subexpr->kind = EXPR_PAREN;
			subexpr->expr_not.expr = NULL;
			struct parse_state* new_parse_state =
				calloc(1, sizeof(struct parse_state));
			if ( !new_parse_state )
				err(1, "malloc");
			new_parse_state->outer = parse_state;
			new_parse_state->insert_and = &subexpr->expr_paren.expr;
			new_parse_state->insert_or = &subexpr->expr_paren.expr;
			parse_state = new_parse_state;
			next_insert_at = &subexpr->expr_paren.expr;
		}
		else if ( !strcmp(arg, ")") )
		{
			if ( !parse_state->outer )
				errx(1, "unbalanced closing parenthesis");
			if ( insert_at )
				errx(1, "expected subexpression before )");
			struct parse_state* old_state = parse_state;
			parse_state = parse_state->outer;
			free(old_state);
			free(subexpr);
			continue;
		}
		else if ( !strcmp(arg, "!") )
		{
			subexpr->kind = EXPR_NOT;
			subexpr->expr_not.expr = NULL;
			next_insert_at = &subexpr->expr_not.expr;
		}
		else if ( !strcmp(arg, "-a") )
		{
			if ( insert_at || !*parse_state->insert_and )
				errx(1, "expected subexpression before -a");
			subexpr->kind = EXPR_AND;
			subexpr->expr_and.a = *parse_state->insert_and;
			subexpr->expr_and.b = NULL;
			subexpr->parent = subexpr->expr_and.a->parent;
			subexpr->expr_and.a->parent = subexpr;
			*parse_state->insert_and = subexpr;
			insert_at = &subexpr->expr_and.b;
			insert_at_parent = subexpr;
			parse_state->insert_and = &subexpr->expr_and.b;
			continue;
		}
		else if ( !strcmp(arg, "-o") )
		{
			if ( insert_at || !*parse_state->insert_or )
				errx(1, "expected subexpression before -o");
			subexpr->kind = EXPR_OR;
			subexpr->expr_or.a = *parse_state->insert_or;
			subexpr->expr_or.b = NULL;
			subexpr->parent = subexpr->expr_or.a->parent;
			subexpr->expr_or.a->parent = subexpr;
			*parse_state->insert_or = subexpr;
			insert_at = &subexpr->expr_or.b;
			insert_at_parent = subexpr;
			parse_state->insert_and = &subexpr->expr_or.b;
			parse_state->insert_or = &subexpr->expr_or.b;
			continue;
		}
		else if ( !strcmp(arg, "-anewer") ||
		          !strcmp(arg, "-newer") ||
		          !strcmp(arg, "-cnewer") )
		{
			if ( i + 1 == argc )
				errx(1, "missing parameter to %s", arg);
			char t = !strcmp(arg, "-newer") ? 'm' : arg[1];
			const char* param = argv[++i];
			struct stat st;
			if ( stat(param, &st) < 0 )
				err(1, "%s", param);
			subexpr->kind = EXPR_NEWER;
			subexpr->expr_newer.when = *pick_time(&st, t);
			subexpr->expr_newer.t = t;
		}
		else if ( !strcmp(arg, "-atime") || !strcmp(arg, "-amin") ||
		          !strcmp(arg, "-ctime") || !strcmp(arg, "-cmin") ||
		          !strcmp(arg, "-mtime") || !strcmp(arg, "-mmin") )
		{
			if ( i + 1 == argc )
				errx(1, "missing parameter to %s", arg);
			char t = arg[1];
			time_t interval = !strcmp(arg + 2, "time") ? 86400 : 60;
			const char* param = argv[++i];
			int cmp = param[0] == '-' ? (param++, -1) :
			          param[0] == '+' ? (param++, 1) : 0;
			char* end;
			errno = 0;
			intmax_t value = strtoimax(param, &end, 10);
			if ( errno || *end || (time_t) value != value )
				errx(1, "invalid parameter to %s: %s", arg, param);
			time_t n = (time_t) value;
			subexpr->kind = EXPR_TIME;
			subexpr->expr_time.n = n;
			subexpr->expr_time.interval = interval;
			subexpr->expr_time.cmp = cmp;
			subexpr->expr_time.t = t;
		}
		else if ( !strcmp(arg, "-depth") )
		{
			depth = true;
			subexpr->kind = EXPR_TRUE;
		}
		else if ( !strcmp(arg, "-delete") )
		{
			found_action = true;
			found_delete = true;
			depth = true;
			subexpr->kind = EXPR_DELETE;
		}
		else if ( !strcmp(arg, "-exec") || !strcmp(arg, "-execdir") ||
		          !strcmp(arg, "-ok") || !strcmp(arg, "-okdir") )
		{
			found_action = true;
			int count = 0;
			bool was_braces = false;
			while ( i + 1 + count < argc &&
			        strcmp(argv[i + 1 + count], ";") != 0 &&
			        !(was_braces && !strcmp(argv[i + 1 + count], "+")) )
			{
				was_braces = !strcmp(argv[i + 1 + count], "{}");
				count++;
			}
			if ( !count || i + count + 1 == argc )
				errx(1, "missing parameter to %s", arg);
			subexpr->kind = EXPR_EXEC;
			subexpr->expr_exec.argc = count;
			subexpr->expr_exec.argv = (const char* const*) argv + i + 1;
			subexpr->expr_exec.ok = strstr(arg, "ok");
			subexpr->expr_exec.dir = strstr(arg, "dir");
			subexpr->expr_exec.plus = !strcmp(argv[i + count + 1], "+");
			i += 1 + count;
		}
		else if ( !strcmp(arg, "-group") )
		{
			if ( i + 1 == argc )
				errx(1, "missing parameter to %s", arg);
			const char* param = argv[++i];
			char* end;
			uintmax_t value = strtoumax(param, &end, 10);
			gid_t gid;
			if ( errno || *end || (gid_t) value != value )
			{
				errno = 0;
				struct group* grp = getgrnam(param);
				if ( !grp )
				{
					if ( errno )
						err(1, "%s: %s", arg, param);
					else
						errx(1, "%s: No such group: %s", arg, param);
				}
				gid = grp->gr_gid;
			}
			else
				gid = value;
			subexpr->kind = EXPR_GROUP;
			subexpr->expr_group.gid = gid;
		}
		else if ( !strcmp(arg, "-inum") )
		{
			if ( i + 1 == argc )
				errx(1, "missing parameter to %s", arg);
			const char* param = argv[++i];
			int cmp = param[0] == '-' ? (param++, -1) :
			          param[0] == '+' ? (param++, 1) : 0;
			char* end;
			errno = 0;
			uintmax_t value = strtoumax(param, &end, 10);
			if ( errno || *end || (ino_t) value != value )
				errx(1, "invalid parameter to %s: %s", arg, param);
			subexpr->kind = EXPR_INUM;
			subexpr->expr_inum.ino = (ino_t) value;
			subexpr->expr_inum.cmp = cmp;
		}
		else if ( !strcmp(arg, "-links") )
		{
			if ( i + 1 == argc )
				errx(1, "missing parameter to %s", arg);
			const char* param = argv[++i];
			int cmp = param[0] == '-' ? (param++, -1) :
			          param[0] == '+' ? (param++, 1) : 0;
			char* end;
			errno = 0;
			uintmax_t value = strtoumax(param, &end, 10);
			if ( errno || *end || (nlink_t) value != value )
				errx(1, "invalid parameter to %s: %s", arg, param);
			subexpr->kind = EXPR_LINKS;
			subexpr->expr_links.n = (nlink_t) value;
			subexpr->expr_links.cmp = cmp;
		}
		else if ( !strcmp(arg, "-maxdepth") )
		{
			if ( i + 1 == argc )
				errx(1, "missing parameter to %s", arg);
			const char* param = argv[++i];
			char* end;
			errno = 0;
			uintmax_t value = strtoumax(param, &end, 10);
			if ( errno || *end || (size_t) value != value )
				errx(1, "invalid parameter to %s: %s", arg, param);
			subexpr->kind = EXPR_TRUE;
			maxdepth = (size_t) value;
		}
		else if ( !strcmp(arg, "-mindepth") )
		{
			if ( i + 1 == argc )
				errx(1, "missing parameter to %s", arg);
			const char* param = argv[++i];
			char* end;
			errno = 0;
			uintmax_t value = strtoumax(param, &end, 10);
			if ( errno || *end || (size_t) value != value )
				errx(1, "invalid parameter to %s: %s", arg, param);
			subexpr->kind = EXPR_TRUE;
			mindepth = (size_t) value;
		}
		else if ( !strcmp(arg, "-mount") )
		{
			mount = true;
			subexpr->kind = EXPR_TRUE;
		}
		else if ( !strcmp(arg, "-name") || !strcmp(arg, "-iname") )
		{
			if ( i + 1 == argc )
				errx(1, "missing parameter to %s", arg);
			const char* param = argv[++i];
			subexpr->kind = EXPR_NAME;
			subexpr->expr_name.pattern = param;
			subexpr->expr_name.flags =
				!strcmp(arg, "-iname") ? FNM_CASEFOLD : 0;
		}
		else if ( !strcmp(arg, "-nogroup") )
			subexpr->kind = EXPR_NOGROUP;
		else if ( !strcmp(arg, "-nouser") )
			subexpr->kind = EXPR_NOUSER;
		else if ( !strcmp(arg, "-path") )
		{
			if ( i + 1 == argc )
				errx(1, "missing parameter to %s", arg);
			const char* param = argv[++i];
			subexpr->kind = EXPR_PATH;
			subexpr->expr_path.pattern = param;
		}
		else if ( !strcmp(arg, "-perm") )
		{
			if ( i + 1 == argc )
				errx(1, "missing parameter to %s", arg);
			const char* param = argv[++i];
			size_t offset = 0;
			bool least = param[0] == '-';
			if ( least )
				offset++;
			if ( !param[offset] )
				errx(1, "invalid parameter to %s: %s", arg, param);
			mode_t mode = execute_modespec(param + offset, 0, S_IFREG, 07777);
			if ( mode == (mode_t) -1 )
				errx(1, "invalid parameter to %s: %s", arg, param);
			subexpr->kind = EXPR_PERM;
			subexpr->expr_perm.mode = mode;
			subexpr->expr_perm.least = least;
		}
		else if ( !strcmp(arg, "-print") || !strcmp(arg, "-print0") )
		{
			found_action = true;
			subexpr->kind = EXPR_PRINT;
			subexpr->expr_print.delim = !strcmp(arg, "-print") ? '\n' : '\0';
		}
		else if ( !strcmp(arg, "-prune") )
		{
			found_prune = true;
			subexpr->kind = EXPR_PRUNE;
		}
		else if ( !strcmp(arg, "-regex") )
		{
			if ( i + 1 == argc )
				errx(1, "missing parameter to %s", arg);
			const char* param = argv[++i];
			subexpr->kind = EXPR_REGEX;
			int error = regcomp(&subexpr->expr_regex.re, param + 1,
				                ere ? REG_EXTENDED : 0);
			if ( error )
			{
				size_t size = regerror(error, NULL, NULL, 0);
				char* error_string = malloc(size);
				if ( !error_string )
					errx(1, "-regex: invalid regular expression: %s", param);
				regerror(error, NULL, error_string, size);
				errx(1, "-regex: %s: %s", error_string, param);
			}
		}
		else if ( !strcmp(arg, "-size") )
		{
			if ( i + 1 == argc )
				errx(1, "missing parameter to %s", arg);
			const char* param = argv[++i];
			int cmp = param[0] == '-' ? (param++, -1) :
			          param[0] == '+' ? (param++, 1) : 0;
			char* end;
			errno = 0;
			intmax_t value = strtoimax(param, &end, 10);
			if ( errno || (*end && *end != 'c') || (off_t) value != value ||
			     value < 0 )
				errx(1, "invalid parameter to %s: %s", arg, param);
			subexpr->kind = EXPR_SIZE;
			subexpr->expr_size.bytes = *end == 'c';
			subexpr->expr_size.size = (off_t) value;
			subexpr->expr_size.cmp = cmp;
		}
		else if ( !strcmp(arg, "-type") )
		{
			if ( i + 1 == argc )
				errx(1, "missing parameter to %s", arg);
			const char* param = argv[++i];
			if ( !param[0] || param[1] )
				errx(1, "invalid parameter to %s: %s", arg, param);
			switch ( param[0] )
			{
			case 'b': break;
			case 'c': break;
			case 'd': break;
			case 'f': break;
			case 'l': break;
			case 'p': break;
			case 's': break;
			default:
				errx(1, "invalid parameter to %s: %s", arg, param);
			}
			subexpr->kind = EXPR_TYPE;
			subexpr->expr_type.c = param[0];
		}
		else if ( !strcmp(arg, "-user") )
		{
			if ( i + 1 == argc )
				errx(1, "missing parameter to %s", arg);
			const char* param = argv[++i];
			char* end;
			uintmax_t value = strtoumax(param, &end, 10);
			uid_t uid;
			if ( errno || *end || (uid_t) value != value )
			{
				errno = 0;
				struct passwd* pwd = getpwnam(param);
				if ( !pwd )
				{
					if ( errno )
						err(1, "%s: %s", arg, param);
					else
						errx(1, "%s: No such user: %s", arg, param);
				}
				uid = pwd->pw_uid;
			}
			else
				uid = value;
			subexpr->kind = EXPR_USER;
			subexpr->expr_user.uid = uid;
		}
		else if ( !strcmp(arg, "-xdev") )
		{
			xdev = true;
			subexpr->kind = EXPR_TRUE;
		}
		else
			errx(1, "unknown primary: %s", arg);

		if ( insert_at )
		{
			*insert_at = subexpr;
			subexpr->parent = insert_at_parent;
		}
		else
		{
			struct expr* and_expr = calloc(1, sizeof(struct expr));
			if ( !and_expr )
				err(1, "malloc");
			and_expr->kind = EXPR_AND;
			and_expr->expr_and.a = *parse_state->insert_and;
			and_expr->expr_and.b = subexpr;
			and_expr->parent = and_expr->expr_and.a->parent;
			and_expr->expr_and.a->parent = and_expr;
			and_expr->expr_and.b->parent = and_expr;
			*parse_state->insert_and = and_expr;
			parse_state->insert_and = &and_expr->expr_and.b;
		}
		insert_at = next_insert_at;
		insert_at_parent = next_insert_at ? subexpr : NULL;
	}

	if ( parse_state->outer )
		errx(1, "unbalanced opening parenthesis");
	if ( insert_at && insert_at != &root )
		errx(1, "expected another subexpression");

	assert(!root || !root->parent);

	if ( !found_action )
	{
		struct expr* print_expr = calloc(1, sizeof(struct expr));
		if ( !print_expr )
			err(1, "malloc");
		print_expr->kind = EXPR_PRINT;
		print_expr->expr_print.delim = '\n';
		if ( !root )
			root = print_expr;
		else
		{
			struct expr* and_expr = calloc(1, sizeof(struct expr));
			if ( !and_expr )
				err(1, "malloc");
			and_expr->kind = EXPR_AND;
			and_expr->expr_and.a = root;
			and_expr->expr_and.b = print_expr;
			and_expr->parent = root->parent;
			and_expr->expr_and.a->parent = and_expr;
			and_expr->expr_and.b->parent = and_expr;
			root = and_expr;
		}
	}

	if ( found_delete && symderef != SYMDEREF_NONE )
		errx(1, "-delete is not allowed when following symbolic links");

	if ( depth && found_prune )
		warnx("warning: -prune is ignored when -depth");

	assert(!root->parent);

	clock_gettime(CLOCK_REALTIME, &startup);

	bool result = true;

	struct state state;
	if ( predicates_offset == 1 )
	{
		memset(&state, 0, sizeof(state));
		state.dirfd = AT_FDCWD;
		state.name = ".";
		state.relpath = ".";
		state.path = ".";
		state.type = DT_UNKNOWN;
		result = find(root, &state, depth, symderef, xdev, mount, mindepth,
		              maxdepth);
	}
	else
	{
		for ( int i = 1; i < predicates_offset; i++ )
		{
			const char* arg = argv[i];
			char* argdup = strdup(arg);
			if ( !argdup )
				err(1, "malloc");
			memset(&state, 0, sizeof(state));
			state.dirfd = AT_FDCWD;
			state.name = basename(argdup);
			state.relpath = arg;
			state.path = arg;
			state.type = DT_UNKNOWN;
			if ( !find(root, &state, depth, symderef, xdev, mount, mindepth,
			           maxdepth) )
				result = false;
			free(argdup);
		}
	}

	if ( ferror(stdout) || fflush(stdout) == EOF )
		err(1, "stdout");

	return result ? 0 : 1;
}
