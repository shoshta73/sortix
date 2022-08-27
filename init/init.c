/*
 * Copyright (c) 2011-2022 Jonas 'Sortie' Termansen.
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
 * init.c
 * Start the operating system.
 */

#include <sys/display.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fstab.h>
#include <grp.h>
#include <inttypes.h>
#include <ioleast.h>
#include <limits.h>
#include <locale.h>
#include <net/if.h>
#include <poll.h>
#include <psctl.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <timespec.h>
#include <unistd.h>

// TODO: The Sortix <limits.h> doesn't expose this at the moment.
#if !defined(HOST_NAME_MAX) && defined(__sortix__)
#include <sortix/limits.h>
#endif

#include <fsmarshall.h>

#include <mount/blockdevice.h>
#include <mount/devices.h>
#include <mount/filesystem.h>
#include <mount/harddisk.h>
#include <mount/partition.h>
#include <mount/uuid.h>

struct device_match
{
	const char* path;
	struct blockdevice* bdev;
};

struct mountpoint
{
	struct fstab entry;
	char* entry_line;
	pid_t pid;
	char* absolute;
};

enum verbosity
{
	VERBOSITY_SILENT,
	VERBOSITY_QUIET,
	VERBOSITY_VERBOSE,
};

enum exit_code_meaning
{
	EXIT_CODE_MEANING_DEFAULT,
	EXIT_CODE_MEANING_POWEROFF_REBOOT,
};

enum daemon_state
{
	DAEMON_STATE_TERMINATED,
	DAEMON_STATE_SCHEDULED,
	DAEMON_STATE_WAITING,
	DAEMON_STATE_SATISFIED,
	DAEMON_STATE_STARTING,
	DAEMON_STATE_READY,
	DAEMON_STATE_RUNNING,
	DAEMON_STATE_TERMINATING,
	DAEMON_STATE_FINISHED,
	NUM_DAEMON_STATES
};

struct daemon;

struct dependency
{
	struct daemon* source;
	struct daemon* target;
	int flags;
};

#define DEPENDENCY_FLAG_REQUIRE (1 << 0)
#define DEPENDENCY_FLAG_AWAIT (1 << 1)
#define DEPENDENCY_FLAG_EXIT_CODE (1 << 2)

enum log_method
{
	LOG_METHOD_NONE,
	LOG_METHOD_APPEND,
	LOG_METHOD_ROTATE,
};

enum log_format
{
	LOG_FORMAT_NONE,
	LOG_FORMAT_SECONDS,
	LOG_FORMAT_NANOSECONDS,
	LOG_FORMAT_BASIC,
	LOG_FORMAT_FULL,
	LOG_FORMAT_SYSLOG,
};

struct log
{
	char* name;
	pid_t pid;
	enum log_method method;
	enum log_format format;
	bool control_messages;
	bool rotate_on_start;
	size_t max_rotations;
	off_t max_line_size;
	size_t skipped;
	off_t max_size;
	char* path;
	char* path_src;
	char* path_dst;
	size_t path_number_offset;
	size_t path_number_size;
	char* buffer;
	size_t buffer_used;
	size_t buffer_size;
	off_t size;
	int fd;
	int last_errno;
	bool line_terminated;
	bool line_begun;
	mode_t file_mode;
};

struct daemon
{
	char* name;
	struct daemon* next_by_state;
	struct daemon* prev_by_state;
	struct daemon* parent_of_parameter;
	struct daemon* first_by_parameter;
	struct daemon* last_by_parameter;
	struct daemon* prev_by_parameter;
	struct daemon* next_by_parameter;
	struct dependency** dependencies;
	size_t dependencies_used;
	size_t dependencies_length;
	size_t dependencies_ready;
	size_t dependencies_finished;
	size_t dependencies_failed;
	struct dependency** dependents;
	size_t dependents_used;
	size_t dependents_length;
	size_t reference_count;
	size_t pfd_readyfd_index;
	size_t pfd_outputfd_index;
	struct dependency* exit_code_from;
	char* cd;
	char* netif;
	int argc;
	char** argv;
	struct termios oldtio;
	struct log log;
	pid_t pid;
	enum exit_code_meaning exit_code_meaning;
	enum daemon_state state;
	int exit_code;
	int readyfd;
	int outputfd;
	bool configured;
	bool need_tty;
	bool was_ready;
	bool was_terminated;
	bool was_dereferenced;
};

struct dependency_config
{
	char* target;
	int flags;
};

struct daemon_config
{
	char* name;
	struct dependency_config** dependencies;
	size_t dependencies_used;
	size_t dependencies_length;
	char* cd;
	int argc;
	char** argv;
	enum exit_code_meaning exit_code_meaning;
	bool per_if;
	bool need_tty;
	enum log_method log_method;
	enum log_format log_format;
	bool log_control_messages;
	bool log_rotate_on_start;
	size_t log_rotations;
	off_t log_line_size;
	off_t log_size;
	mode_t log_file_mode;
};

static pid_t main_pid;
static pid_t forward_signal_pid = -1;

static volatile sig_atomic_t caught_exit_signal = -1;
static sigset_t handled_signals;

static struct daemon_config default_config =
{
	.log_method = LOG_METHOD_ROTATE,
	.log_format = LOG_FORMAT_NANOSECONDS,
	.log_control_messages = true,
	.log_rotate_on_start = false,
	.log_rotations = 3,
	.log_line_size = 4096,
	.log_size = 1048576,
	.log_file_mode = 0644,
};

static struct log init_log = { .fd = -1 };

static enum verbosity verbosity = VERBOSITY_QUIET;

static struct harddisk** hds = NULL;
static size_t hds_used = 0;
static size_t hds_length = 0;

static struct mountpoint* mountpoints = NULL;
static size_t mountpoints_used = 0;
static size_t mountpoints_length = 0;

static struct daemon** daemons = NULL;
static size_t daemons_used = 0;
static size_t daemons_length = 0;

static struct daemon* first_daemon_by_state[NUM_DAEMON_STATES];
static struct daemon* last_daemon_by_state[NUM_DAEMON_STATES];
static size_t count_daemon_by_state[NUM_DAEMON_STATES];

static struct pollfd* pfds = NULL;
static size_t pfds_used = 0;
static size_t pfds_length = 0;

static struct daemon** pfds_daemon = NULL;
static size_t pfds_daemon_length = 0;

static bool chain_location_made = false;
static char chain_location[] = "/tmp/fs.XXXXXX";
static bool chain_location_dev_made = false;
static char chain_location_dev[] = "/tmp/fs.XXXXXX/dev";

static void signal_handler(int signum)
{
	if ( getpid() != main_pid )
		return;

	if ( forward_signal_pid != -1 )
	{
		if ( 0 < forward_signal_pid )
			kill(forward_signal_pid, signum);
		return;
	}

	switch ( signum )
	{
	case SIGINT: caught_exit_signal = 1; break;
	case SIGTERM: caught_exit_signal = 0; break;
	case SIGQUIT: caught_exit_signal = 2; break;
	}
}

static void install_signal_handler(void)
{
	sigemptyset(&handled_signals);
	sigaddset(&handled_signals, SIGINT);
	sigaddset(&handled_signals, SIGQUIT);
	sigaddset(&handled_signals, SIGTERM);
	sigprocmask(SIG_BLOCK, &handled_signals, NULL);
	struct sigaction sa = { .sa_handler = signal_handler, .sa_flags = 0 };
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

static void uninstall_signal_handler(void)
{
	struct sigaction sa = { .sa_handler = SIG_DFL, .sa_flags = 0 };
	sa.sa_handler = SIG_DFL;
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigprocmask(SIG_UNBLOCK, &handled_signals, NULL);
}

static char* read_single_line(FILE* fp)
{
	char* ret = NULL;
	size_t ret_size = 0;
	ssize_t ret_length = getline(&ret, &ret_size, fp);
	if ( ret_length < 0 )
	{
		free(ret);
		return NULL;
	}
	if ( ret[ret_length-1] == '\n' )
		ret[--ret_length] = '\0';
	return ret;
}

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

static bool array_add(void*** array_ptr,
                      size_t* used_ptr,
                      size_t* length_ptr,
                      void* value)
{
	void** array;
	memcpy(&array, array_ptr, sizeof(array)); // Strict aliasing.

	if ( *used_ptr == *length_ptr )
	{
		size_t length = *length_ptr;
		if ( !length )
			length = 4;
		void** new_array = reallocarray(array, length, 2 * sizeof(void*));
		if ( !new_array )
			return false;
		array = new_array;
		memcpy(array_ptr, &array, sizeof(array)); // Strict aliasing.
		*length_ptr = length * 2;
	}

	memcpy(array + (*used_ptr)++, &value, sizeof(value)); // Strict aliasing.

	return true;
}

static int exit_code_to_exit_status(int exit_code)
{
	if ( WIFEXITED(exit_code) )
		return WEXITSTATUS(exit_code);
	else if ( WIFSIGNALED(exit_code) )
		return 128 + WTERMSIG(exit_code);
	else
		return 1;
}

static void log_close(struct log* log)
{
	if ( 0 <= log->fd )
		close(log->fd);
	log->fd = -1;
	free(log->buffer);
	log->buffer = NULL;
}

static void log_error(struct log* log, const char* prefix, const char* path)
{
	// TODO: warn() calls should go to the init log unless about the init log.
	// TODO: Maybe warn only once globally about /var/log being read-only?
	if ( !errno )
		warnx("%s%s", prefix, path ? path : log->path);
	else if ( errno != log->last_errno )
		warn("%s%s", prefix, path ? path : log->path);
	log->last_errno = errno;
}

static bool log_open(struct log* log)
{
	if ( log->method == LOG_METHOD_NONE )
		return true;
	int logflags = O_CREAT | O_WRONLY | O_APPEND | O_NOFOLLOW;
	if ( log->method == LOG_METHOD_APPEND && log->rotate_on_start )
		logflags |= O_TRUNC;
	if ( 0 <= log->fd )
		close(log->fd);
	log->fd = open(log->path, logflags, log->file_mode);
	if ( log->fd < 0 )
	{
		log_error(log, "", NULL);
		// Don't block daemon startup on read-only filesystems.
		return errno == EROFS;
	}
	struct stat st;
	if ( fstat(log->fd, &st) < 0 )
	{
		log_error(log, "stat: ", NULL);
		close(log->fd);
		log->fd = -1;
		return false;
	}
	if ( (st.st_mode & 07777) != log->file_mode )
	{
		if ( fchmod(log->fd, log->file_mode) < 0 )
		{
			log_error(log, "fchmod: ", NULL);
			close(log->fd);
			log->fd = -1;
			return false;
		}
	}
	log->size = st.st_size;
	log->line_terminated = true;
	return true;
}

static bool log_rotate(struct log* log)
{
	if ( log->method == LOG_METHOD_NONE )
		return true;
	if ( 0 <= log->fd )
	{
		close(log->fd);
		log->fd = -1;
	}
	for ( size_t i = log->max_rotations; 0 < i; i-- )
	{
		snprintf(log->path_dst + log->path_number_offset,
		         log->path_number_size, ".%zu", i);
		snprintf(log->path_src + log->path_number_offset,
		         log->path_number_size, "%c%zu", i-1 != 0 ? '.' : '\0', i-1);
		if ( i == log->max_rotations )
		{
			if ( !access(log->path_dst, F_OK) )
			{
				// Ensure the file system space usage has an upper bound by
				// deleting the oldest log. However if another process has the
				// log open, the kernel will keep the file contents alive. The
				// file is truncated to zero size to avoid disk space remaining
				// temporarily in use that way, although the inode itself does
				// remain open temporarily.
				// TODO: truncateat should have a flags parameter, to not follow
				// symbolic links. Otherwise a symlink in /var/log could be
				// used to truncate an arbitrary file, which is avoided here.
				int fd = open(log->path_dst, O_WRONLY | O_NOFOLLOW);
				if ( fd < 0 )
				{
					// Don't rotate logs on read-only filesystems.
					if ( errno == EROFS )
						break;
					log_error(log, "archiving: opening: ", log->path_dst);
				}
				else
				{
					if ( ftruncate(fd, 0) < 0 )
						log_error(log, "archiving: truncate: ", log->path_dst);
					close(fd);
				}
				if ( unlink(log->path_dst) < 0 )
					log_error(log, "archiving: unlink: ", log->path_dst);
			}
			else if ( errno != ENOENT )
				log_error(log, "archiving: ", log->path_dst);
		}
		if ( rename(log->path_src, log->path_dst) < 0 )
		{
			// Don't rotate logs on read-only filesystems.
			if ( errno == EROFS )
				break;
			// Ignore non-existent logs.
			if ( errno != ENOENT )
			{
				log_error(log, "archiving: ", log->path_src);
				return errno == EROFS;
			}
		}
	}
	return log_open(log);
}

static bool log_initialize(struct log* log,
                           const char* name,
                           struct daemon_config* daemon_config)
{
	memset(log, 0, sizeof(*log));
	log->fd = -1;
	log->method = daemon_config->log_method;
	log->format = daemon_config->log_format;
	log->control_messages = daemon_config->log_control_messages;
	log->rotate_on_start = daemon_config->log_rotate_on_start;
	log->max_rotations = daemon_config->log_rotations;
	log->max_line_size = daemon_config->log_line_size;
	log->max_size = daemon_config->log_size;
	if ( log->max_size < log->max_line_size )
		log->max_line_size = log->max_size;
	log->file_mode = daemon_config->log_file_mode;
	log->name = strdup(name);
	if ( !log->name )
		return false;
	if ( asprintf(&log->path, "/var/log/%s.log", name) < 0 )
		return free(log->name), false;
	// Preallocate the paths used when renaming log files so there's no error
	// conditions when cycling logs.
	if ( asprintf(&log->path_src, "%s.%i", log->path, INT_MAX) < 0 )
		return free(log->path), free(log->name), false;
	if ( asprintf(&log->path_dst, "%s.%i", log->path, INT_MAX) < 0 )
		return free(log->path_src), free(log->path), free(log->name), false;
	log->path_number_offset = strlen(log->path);
	log->path_number_size = strlen(log->path_dst) + 1 - log->path_number_offset;
	return true;
}

static bool log_begin_buffer(struct log* log)
{
	log->buffer_used = 0;
	log->buffer_size = 4096;
	log->buffer = malloc(log->buffer_size);
	if ( !log->buffer )
		return false;
	return true;
}

static void log_data_to_buffer(struct log* log, const char* data, size_t length)
{
	assert(log->buffer);
	if ( log->skipped )
	{
		log->skipped += length;
		return;
	}
	while ( length )
	{
		size_t available = log->buffer_size - log->buffer_used;
		if ( !available )
		{
			if ( 1048576 <= log->buffer_size )
			{
				errno = 0;
				log_error(log, "in-memory buffer exhausted: ", NULL);
				log->skipped += length;
				return;
			}
			size_t new_size = 2 * log->buffer_size;
			char* new_buffer = realloc(log->buffer, new_size);
			if ( !new_buffer )
			{
				log_error(log, "expanding in-memory buffer: ", NULL);
				log->skipped += length;
				return;
			}
			log->buffer = new_buffer;
			log->buffer_size = new_size;
			available = log->buffer_size - log->buffer_used;
		}
		size_t amount = length < available ? length : available;
		memcpy(log->buffer + log->buffer_used, data, amount);
		data += amount;
		length -= amount;
		log->buffer_used += amount;
	}
}

static void log_data(struct log* log, const char* data, size_t length)
{
	if ( log->method == LOG_METHOD_NONE )
		return;
	if ( log->fd < 0 && log->buffer )
	{
		log_data_to_buffer(log, data, length);
		return;
	}
	if ( log->skipped )
	{
		// TODO: Try to log a mesage about how many bytes were skipped and
		//       resume logging if that worked. For now, let's just lose data.
	}
	const off_t chunk_cut_offset = log->max_size - log->max_line_size;
	size_t sofar = 0;
	while ( sofar < length )
	{
		if ( log->fd < 0 )
		{
			log->skipped += length - sofar;
			return;
		}
		// If the data is currently line terminated, then cut if we can't add
		// another line of the maximum length, otherwise cut if the chunk is
		// full.
		if ( log->method == LOG_METHOD_ROTATE &&
		     (log->line_terminated ?
		      chunk_cut_offset :
		      log->max_size) <= log->size )
		{
			if ( !log_rotate(log) )
			{
				log->skipped += length - sofar;
				return;
			}
		}
		// Decide the size of the new chunk to write out.
		const char* next_data = data + sofar;
		size_t remaining_length = length - sofar;
		size_t next_length = remaining_length;
		if ( log->method == LOG_METHOD_ROTATE )
		{
			off_t chunk_left = log->max_size - log->size;
			next_length =
				(uintmax_t) remaining_length < (uintmax_t) chunk_left ?
				(size_t) remaining_length : (size_t) chunk_left;
			// Attempt to cut the log at a newline.
			if ( chunk_cut_offset <= log->size + (off_t) next_length )
			{
				// Find where the data becomes eligible for a line cut, and
				// search for a newline after that.
				size_t first_cut_index =
					log->size < chunk_cut_offset ?
					0 :
					(size_t) (chunk_cut_offset - log->size);
				for ( size_t i = first_cut_index; i < next_length; i++ )
				{
					if ( next_data[i] == '\n' )
					{
						next_length = i + 1;
						break;
					}
				}
			}
		}
		ssize_t amount = write(log->fd, next_data, next_length);
		if ( amount < 0 )
		{
			log_error(log, "writing: ", NULL);
			log->skipped += length - sofar;
			return;
		}
		sofar += amount;
		log->size += amount;
		log->line_terminated = next_data[amount - 1] == '\n';
		log->last_errno = 0;
	}
}

static void log_formatted(struct log* log, const char* string, size_t length)
{
	if ( log->format == LOG_FORMAT_NONE )
	{
		log_data(log, string, length);
		return;
	}
	size_t log_name_length = strlen(log->name);
	for ( size_t i = 0; i < length; )
	{
		size_t fragment = 1;
		while ( string[i + fragment - 1] != '\n' && i + fragment < length )
			fragment++;
		if ( !log->line_begun )
		{
			struct timespec now;
			clock_gettime(CLOCK_REALTIME, &now);
			struct tm tm;
			gmtime_r(&now.tv_sec, &tm);
			char hostname[HOST_NAME_MAX + 1];
			gethostname(hostname, sizeof(hostname));
			if ( log->format == LOG_FORMAT_SYSLOG )
			{
				int pri = 3 /* system daemons */ * 8 + 6 /* informational */;
				char header[64];
				snprintf(header, sizeof(header), "<%d>1 ", pri);
				log_data(log, header, strlen(header));
			}
			char timeformat[64] = "%F %T +0000";
			if ( log->format == LOG_FORMAT_SYSLOG )
				snprintf(timeformat, sizeof(timeformat),
				         "%%FT%%T.%06liZ", now.tv_nsec / 1000);
			else if ( log->format != LOG_FORMAT_SECONDS )
				snprintf(timeformat, sizeof(timeformat),
				         "%%F %%T.%09li +0000", now.tv_nsec);
			char timestamp[64];
			strftime(timestamp, sizeof(timestamp), timeformat, &tm);
			log_data(log, timestamp, strlen(timestamp));
			if ( log->format == LOG_FORMAT_FULL ||
			     log->format == LOG_FORMAT_SYSLOG )
			{
				log_data(log, " ", 1);
				log_data(log, hostname, strlen(hostname));
			}
			if ( log->format == LOG_FORMAT_BASIC ||
			     log->format == LOG_FORMAT_FULL ||
			     log->format == LOG_FORMAT_SYSLOG )
			{
				log_data(log, " ", 1);
				log_data(log, log->name, log_name_length);
			}
			if ( log->format == LOG_FORMAT_SYSLOG )
			{
				pid_t pid = 0 < log->pid ? log->pid : getpid();
				char part[64];
				snprintf(part, sizeof(part), " %ji - - ", (intmax_t) pid);
				log_data(log, part, strlen(part));
			}
			else
				log_data(log, ": ", 2);
		}
		log_data(log, string + i, fragment);
		log->line_begun = string[i + fragment - 1] != '\n';
		i += fragment;
	}
}

static size_t log_callback(void* ctx, const char* str, size_t len)
{
	log_formatted((struct log*) ctx, str, len);
	return len;
}

static bool log_begin(struct log* log)
{
	if ( log->method == LOG_METHOD_NONE )
		return true;
	bool opened;
	if ( log->method == LOG_METHOD_ROTATE && log->rotate_on_start )
		opened = log_rotate(log);
	else
		opened = log_open(log);
	if ( !opened )
		return false;
	if ( log->buffer )
	{
		log_data(log, log->buffer, log->buffer_used);
		free(log->buffer);
		log->buffer = NULL;
		log->buffer_used = 0;
		log->buffer_size = 0;
		// TODO: Warn about any skipped data.
		log->skipped = 0;
	}
	return true;
}

__attribute__((format(printf, 2, 3)))
static void log_status(const char* status, const char* format, ...)
{
	va_list ap;
	va_start(ap, format);
	vcbprintf(&init_log, log_callback, format, ap);
	va_end(ap);
	if ( verbosity == VERBOSITY_SILENT ||
	     (verbosity == VERBOSITY_QUIET &&
	      strcmp(status, "failed") != 0) )
		return;
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	struct tm tm;
	localtime_r(&now.tv_sec, &tm);
	va_start(ap, format);
	fprintf(stderr, "%04d-%02d-%02d %02d:%02d:%02d ",
		tm.tm_year + 1900,
		tm.tm_mon + 1,
		tm.tm_mday + 1,
		tm.tm_hour,
		tm.tm_min,
		tm.tm_sec);
	if ( !strcmp(status, "starting") )
		fprintf(stderr, "[      ] ");
	else if ( !strcmp(status, "started") )
		fprintf(stderr, "[  \e[92mOK\e[m  ] ");
	else if ( !strcmp(status, "finished") )
		fprintf(stderr, "[ \e[92mDONE\e[m ] ");
	else if ( !strcmp(status, "failed") )
		fprintf(stderr, "[\e[91mFAILED\e[m] ");
	else if ( !strcmp(status, "stopping") )
		fprintf(stderr, "[      ] ");
	else if ( !strcmp(status, "stopped") )
		fprintf(stderr, "[  \e[92mOK\e[m  ] ");
	else
		fprintf(stderr, "[  ??  ] ");
	vfprintf(stderr, format, ap);
	fflush(stderr);
	va_end(ap);
}

__attribute__((noreturn))
__attribute__((format(printf, 1, 2)))
static void fatal(const char* format, ...)
{
	va_list ap;
	va_start(ap, format);
	fprintf(stderr, "%s: fatal: ", program_invocation_name);
	vfprintf(stderr, format, ap);
	fprintf(stderr, "\n");
	fflush(stderr);
	va_end(ap);
	if ( getpid() == main_pid )
	{
		va_start(ap, format);
		vcbprintf(&init_log, log_callback, format, ap);
		log_formatted(&init_log, "\n", 1);
		va_end(ap);
	}
	if ( getpid() == main_pid )
		exit(2);
	_exit(2);
}

__attribute__((format(printf, 1, 2)))
static void warning(const char* format, ...)
{
	va_list ap;
	va_start(ap, format);
	fprintf(stderr, "%s: warning: ", program_invocation_name);
	vfprintf(stderr, format, ap);
	fprintf(stderr, "\n");
	fflush(stderr);
	va_end(ap);
	if ( getpid() == main_pid )
	{
		va_start(ap, format);
		vcbprintf(&init_log, log_callback, format, ap);
		log_formatted(&init_log, "\n", 1);
		va_end(ap);
	}
}

__attribute__((format(printf, 1, 2)))
static void note(const char* format, ...)
{
	va_list ap;
	va_start(ap, format);
	fprintf(stderr, "%s: ", program_invocation_name);
	vfprintf(stderr, format, ap);
	fprintf(stderr, "\n");
	fflush(stderr);
	va_end(ap);
	if ( getpid() == main_pid )
	{
		va_start(ap, format);
		vcbprintf(&init_log, log_callback, format, ap);
		log_formatted(&init_log, "\n", 1);
		va_end(ap);
	}
}

static char** tokenize(size_t* out_tokens_used, const char* string)
{
	size_t tokens_used = 0;
	size_t tokens_length = 0;
	char** tokens = malloc(sizeof(char*));
	if ( !tokens )
		return NULL;
	bool failed = false;
	bool invalid = false;
	while ( *string )
	{
		if ( isspace((unsigned char) *string) )
		{
			string++;
			continue;
		}
		if ( *string == '#' )
			break;
		char* token;
		size_t token_size;
		FILE* fp = open_memstream(&token, &token_size);
		if ( !fp )
		{
			failed = true;
			break;
		}
		bool singly = false;
		bool doubly = false;
		bool escaped = false;
		for ( char c = *string++; c; c = *string++ )
		{
			if ( !escaped && !singly && !doubly && isspace((unsigned char) c) )
				break;
			if ( !escaped && !doubly && c == '\'' )
			{
				singly = !singly;
				continue;
			}
			if ( !escaped && !singly && c == '"' )
			{
				doubly = !doubly;
				continue;
			}
			if ( !singly && !escaped && c == '\\' )
			{
				escaped = true;
				continue;
			}
			if ( escaped )
			{
				switch ( c )
				{
				case 'a': c = '\a'; break;
				case 'b': c = '\b'; break;
				case 'e': c = '\e'; break;
				case 'f': c = '\f'; break;
				case 'n': c = '\n'; break;
				case 'r': c = '\r'; break;
				case 't': c = '\t'; break;
				case 'v': c = '\v'; break;
				default: break;
				};
			}
			escaped = false;
			if ( fputc((unsigned char) c, fp) == EOF )
			{
				failed = true;
				break;
			}
		}
		if ( singly || doubly || escaped )
		{
			fclose(fp);
			free(token);
			invalid = true;
			break;
		}
		if ( fflush(fp) == EOF )
		{
			fclose(fp);
			free(token);
			failed = true;
			break;
		}
		fclose(fp);
		if ( !array_add((void***) &tokens, &tokens_used, &tokens_length,
		                token) )
		{
			free(token);
			failed = true;
			break;
		}
	}
	if ( failed || invalid )
	{
		for ( size_t i = 0; i < tokens_used; i++ )
			free(tokens[i]);
		free(tokens);
		if ( invalid )
			errno = 0;
		return NULL;
	}
	char** new_tokens = reallocarray(tokens, tokens_used, sizeof(char*));
	if ( new_tokens )
		tokens = new_tokens;
	*out_tokens_used = tokens_used;
	return tokens;
}

static void daemon_config_free(struct daemon_config* daemon_config)
{
	free(daemon_config->name);
	for ( size_t i = 0; i < daemon_config->dependencies_used; i++ )
	{
		free(daemon_config->dependencies[i]->target);
		free(daemon_config->dependencies[i]);
	}
	free(daemon_config->dependencies);
	free(daemon_config->cd);
	for ( int i = 0; i < daemon_config->argc; i++ )
		free(daemon_config->argv[i]);
	free(daemon_config->argv);
	free(daemon_config);
}

static bool daemon_config_load_search(struct daemon_config* daemon_config,
                                      size_t next_search_path_index);

static bool daemon_process_command(struct daemon_config* daemon_config,
                                   const char* path,
                                   size_t argc,
                                   const char* const* argv,
                                   off_t line_number,
                                   size_t next_search_path_index)
{
	if ( !argc )
		return true;
	if ( !strcmp(argv[0], "furthermore") )
	{
		if ( 2 <= argc )
			warning("%s:%ji: unexpected parameter to %s: %s",
			        path, (intmax_t) line_number, argv[0], argv[1]);
		// TODO: Only once per search path level.
		// TODO: How about requiring it to be the first statement?
		if ( !daemon_config_load_search(daemon_config, next_search_path_index) )
		{
			if ( errno == ENOENT )
			{
				warning("%s:%ji: 'furthermore' failed to locate next '%s' "
				        "configuration file in search path: %m",
				        path, (intmax_t) line_number, daemon_config->name);
				errno = EINVAL;
			}
			else
				warning("%s: while processing 'furthermore': %m", path);
			return false;
		}
		return true;
	}
	if ( argc == 1 )
	{
		warning("%s:%ji: expected parameter: %s",
		        path, (intmax_t) line_number, argv[0]);
		return false;
	}
	if ( !strcmp(argv[0], "cd") )
	{
		free(daemon_config->cd);
		if ( !(daemon_config->cd = strdup(argv[1])) )
		{
			warning("strdup: %m");
			return false;
		}
	}
	else if ( !strcmp(argv[0], "exec") )
	{
		for ( int i = 0; i < daemon_config->argc; i++ )
			free(daemon_config->argv[i]);
		free(daemon_config->argv);
		daemon_config->argc = 0;
		daemon_config->argv = NULL;
		if ( INT_MAX - 1 < argc - 1 )
		{
			warning("%s:%ji: too many arguments: %s",
			        path, (intmax_t) line_number, argv[0]);
			return false;
		}
		int new_argc = argc - 1;
		char** new_argv = calloc(new_argc + 1, sizeof(char*));
		if ( !new_argv )
		{
			warning("malloc: %m");
			return false;
		}
		for ( int i = 0; i < new_argc; i++ )
		{
			size_t n = 1 + (size_t) i;
			if ( !(new_argv[i] = strdup(argv[n])) )
			{
				warning("malloc: %m");
				for ( int j = 0; i < j; j++ )
					free(new_argv[j]);
				free(new_argv);
				return false;
			}
		}
		daemon_config->argc = new_argc;
		daemon_config->argv = new_argv;
	}
	else if ( !strcmp(argv[0], "exit-code-meaning") )
	{
		if ( !strcmp(argv[1], "default") )
			daemon_config->exit_code_meaning = EXIT_CODE_MEANING_DEFAULT;
		else if ( !strcmp(argv[1], "poweroff-reboot") )
			daemon_config->exit_code_meaning =
				EXIT_CODE_MEANING_POWEROFF_REBOOT;
		else
			warning("%s:%ji: unknown %s: %s",
			        path, (intmax_t) line_number, argv[0], argv[1]);
	}
	else if ( !strcmp(argv[0], "log-control-messages") )
	{
		if ( !strcmp(argv[1], "true") )
			daemon_config->log_control_messages = true;
		else if ( !strcmp(argv[1], "false") )
			daemon_config->log_control_messages = false;
		else
			warning("%s:%ji: unknown %s: %s",
			        path, (intmax_t) line_number, argv[0], argv[1]);
	}
	else if ( !strcmp(argv[0], "log-file-mode") )
	{
		char* end;
		errno = 0;
		uintmax_t value = strtoumax(argv[1], &end, 8);
		if ( argv[1] == end || errno || value != (value & 07777) )
			warning("%s:%ji: invalid %s: %s",
			        path, (intmax_t) line_number, argv[0], argv[1]);
		else
			daemon_config->log_file_mode = (mode_t) value;
	}
	else if ( !strcmp(argv[0], "log-format") )
	{
		if ( !strcmp(argv[1], "none") )
			daemon_config->log_format = LOG_FORMAT_NONE;
		else if ( !strcmp(argv[1], "seconds") )
			daemon_config->log_format = LOG_FORMAT_SECONDS;
		else if ( !strcmp(argv[1], "nanoseconds") )
			daemon_config->log_format = LOG_FORMAT_NANOSECONDS;
		else if ( !strcmp(argv[1], "basic") )
			daemon_config->log_format = LOG_FORMAT_BASIC;
		else if ( !strcmp(argv[1], "full") )
			daemon_config->log_format = LOG_FORMAT_FULL;
		else if ( !strcmp(argv[1], "syslog") )
			daemon_config->log_format = LOG_FORMAT_SYSLOG;
		else
			warning("%s:%ji: unknown %s: %s",
			        path, (intmax_t) line_number, argv[0], argv[1]);
	}
	else if ( !strcmp(argv[0], "log-line-size") )
	{
		char* end;
		errno = 0;
		intmax_t value = strtoimax(argv[1], &end, 10);
		if ( argv[1] == end || errno || value != (off_t) value || value < 0 )
			warning("%s:%ji: invalid %s: %s",
			        path, (intmax_t) line_number, argv[0], argv[1]);
		else
			daemon_config->log_line_size = (off_t) value;
	}
	else if ( !strcmp(argv[0], "log-method") )
	{
		if ( !strcmp(argv[1], "append") )
			daemon_config->log_method = LOG_METHOD_APPEND;
		else if ( !strcmp(argv[1], "rotate") )
			daemon_config->log_method = LOG_METHOD_ROTATE;
		else
			warning("%s:%ji: unknown %s: %s",
			        path, (intmax_t) line_number, argv[0], argv[1]);
	}
	else if ( !strcmp(argv[0], "log-rotate-on-start") )
	{
		if ( !strcmp(argv[1], "true") )
			daemon_config->log_rotate_on_start = true;
		else if ( !strcmp(argv[1], "false") )
			daemon_config->log_rotate_on_start = false;
		else
			warning("%s:%ji: unknown %s: %s",
			        path, (intmax_t) line_number, argv[0], argv[1]);
	}
	else if ( !strcmp(argv[0], "log-size") )
	{
		char* end;
		errno = 0;
		intmax_t value = strtoimax(argv[1], &end, 10);
		if ( argv[1] == end || errno || value != (off_t) value || value < 0 )
			warning("%s:%ji: invalid %s: %s",
			        path, (intmax_t) line_number, argv[0], argv[1]);
		else
			daemon_config->log_size = (off_t) value;
	}
	else if ( !strcmp(argv[0], "per") )
	{
		if ( !strcmp(argv[1], "if") )
			daemon_config->per_if = true;
		else
			warning("%s:%ji: unknown %s: %s",
			        path, (intmax_t) line_number, argv[0], argv[1]);
	}
	else if ( !strcmp(argv[0], "need") )
	{
		if ( !strcmp(argv[1], "tty") )
			daemon_config->need_tty = true;
		else
			warning("%s:%ji: unknown %s: %s",
			        path, (intmax_t) line_number, argv[0], argv[1]);
	}
	else if ( !strcmp(argv[0], "require") )
	{
		char* target = strdup(argv[1]);
		if ( !target )
		{
			warning("strdup: %m");
			return false;
		}
		int negated_flags = DEPENDENCY_FLAG_REQUIRE | DEPENDENCY_FLAG_AWAIT;
		int flags = negated_flags;
		for ( size_t i = 2; i < argc; i++ )
		{
			if ( !strcmp(argv[i], "optional") )
				flags &= ~DEPENDENCY_FLAG_REQUIRE;
			else if ( !strcmp(argv[i], "no-await") )
				flags &= ~DEPENDENCY_FLAG_AWAIT;
			else if ( !strcmp(argv[i], "exit-code") )
				flags |= DEPENDENCY_FLAG_EXIT_CODE;
			else
				warning("%s:%ji: %s %s: unknown flag: %s", path,
				        (intmax_t) line_number, argv[0], argv[1], argv[i]);
		}
		bool had_exit_code = false;
		// TODO: Linear time lookup.
		struct dependency_config* dependency = NULL;
		for ( size_t i = 0; i < daemon_config->dependencies_used; i++ )
		{
			struct dependency_config* dep = daemon_config->dependencies[i];
			if ( dep->flags & DEPENDENCY_FLAG_EXIT_CODE )
				had_exit_code = true;
			if ( !strcmp(dep->target, target) )
			{
				dependency = dep;
				break;
			}
		}
		if ( dependency )
		{
			dependency->flags &= flags & negated_flags;
			dependency->flags |= flags & ~negated_flags;
			free(target);
		}
		else
		{
			if ( (flags & DEPENDENCY_FLAG_EXIT_CODE) && had_exit_code )
			{
				warning("%s:%ji: %s %s: exit-code had already been set",
				        path, (intmax_t) line_number, argv[0], argv[1]);
				dependency->flags &= ~DEPENDENCY_FLAG_EXIT_CODE;
			}
			dependency = (struct dependency_config*)
				calloc(1, sizeof(struct dependency_config));
			if ( !dependency )
			{
				warning("malloc: %m");
				free(target);
				return false;
			}
			dependency->target = target;
			dependency->flags = flags;
			if ( !array_add((void***) &daemon_config->dependencies,
				            &daemon_config->dependencies_used,
				            &daemon_config->dependencies_length,
				            dependency) )
			{
				warning("malloc: %m");
				free(target);
				free(dependency);
				return false;
			}
		}
	}
	else if ( !strcmp(argv[0], "tty") )
	{
		// TODO: Implement.
	}
	else if ( !strcmp(argv[0], "unset") )
	{
		if ( !strcmp(argv[1], "cd") )
		{
			free(daemon_config->cd);
			daemon_config->cd = NULL;
		}
		else if ( !strcmp(argv[1], "exec") )
		{
			for ( int i = 0; i < daemon_config->argc; i++ )
				free(daemon_config->argv[i]);
			free(daemon_config->argv);
			daemon_config->argc = 0;
			daemon_config->argv = NULL;
		}
		else if ( !strcmp(argv[1], "exit-code-meaning") )
			daemon_config->exit_code_meaning = EXIT_CODE_MEANING_DEFAULT;
		else if ( !strcmp(argv[1], "log-control-messages") )
			daemon_config->log_control_messages =
				default_config.log_control_messages;
		else if ( !strcmp(argv[1], "log-file-mode") )
			daemon_config->log_file_mode = default_config.log_file_mode;
		else if ( !strcmp(argv[1], "log-format") )
			daemon_config->log_format = default_config.log_format;
		else if ( !strcmp(argv[1], "log-line-size") )
			daemon_config->log_line_size = default_config.log_line_size;
		else if ( !strcmp(argv[1], "log-method") )
			daemon_config->log_method = default_config.log_method;
		else if ( !strcmp(argv[1], "log-rotate-on-start") )
			daemon_config->log_rotate_on_start =
				default_config.log_rotate_on_start;
		else if ( !strcmp(argv[1], "log-size") )
			daemon_config->log_line_size = default_config.log_line_size;
		else if ( !strcmp(argv[1], "per") )
		{
			if ( argc < 3 )
				warning("%s:%ji: expected parameter: %s: %s",
					    path, (intmax_t) line_number, argv[0], argv[1]);
			else if ( !strcmp(argv[2], "if") )
				daemon_config->per_if = false;
			else
				warning("%s:%ji: %s %s: unknown: %s", path,
				        (intmax_t) line_number, argv[0], argv[1], argv[2]);
			daemon_config->per_if = false;
		}
		else if ( !strcmp(argv[1], "need") )
		{
			daemon_config->need_tty = false;
			if ( argc < 3 )
				warning("%s:%ji: expected parameter: %s: %s",
					    path, (intmax_t) line_number, argv[0], argv[1]);
			else if ( !strcmp(argv[2], "tty") )
				daemon_config->need_tty = false;
			else
				warning("%s:%ji: %s %s: unknown: %s", path,
				        (intmax_t) line_number, argv[0], argv[1], argv[2]);
			daemon_config->per_if = false;
		}
		else if ( !strcmp(argv[1], "require") )
		{
			if ( argc < 3 )
			{
				for ( size_t i = 0; i < daemon_config->dependencies_used; i++ )
				{
					free(daemon_config->dependencies[i]->target);
					free(daemon_config->dependencies[i]);
				}
				free(daemon_config->dependencies);
				daemon_config->dependencies_used = 0;
				daemon_config->dependencies = NULL;
				return true;
			}
			const char* target = argv[2];
			// TODO: Linear time lookup.
			struct dependency_config* dependency = NULL;
			size_t i;
			for ( i = 0; i < daemon_config->dependencies_used; i++ )
			{
				if ( !strcmp(daemon_config->dependencies[i]->target, target) )
				{
					dependency = daemon_config->dependencies[i];
					break;
				}
			}
			if ( !dependency )
			{
				warning("%s:%ji: dependency wasn't already required: %s",
				        path, (intmax_t) line_number, target);
				return true;
			}
			if ( argc <= 3 )
			{
				free(daemon_config->dependencies[i]->target);
				size_t last = daemon_config->dependencies_used - 1;
				if ( i != last )
				{
					daemon_config->dependencies[i] =
						daemon_config->dependencies[last];
					daemon_config->dependencies[last] = NULL;
				}
				daemon_config->dependencies_used--;
			}
			else for ( size_t i = 3; i < argc; i++ )
			{
				if ( !strcmp(argv[i], "optional") )
					dependency->flags |= DEPENDENCY_FLAG_REQUIRE;
				else if ( !strcmp(argv[i], "no-await") )
					dependency->flags |= DEPENDENCY_FLAG_AWAIT;
				else if ( !strcmp(argv[i], "exit-code") )
					dependency->flags &= ~DEPENDENCY_FLAG_EXIT_CODE;
				else
					warning("%s:%ji: %s %s %s: unknown flag: %s",
					        path, (intmax_t) line_number, argv[0], argv[1],
					        argv[2], argv[i]);
			}
		}
		else if ( !strcmp(argv[1], "tty") )
		{
			// TODO: Implement.
		}
		else
			warning("%s:%ji: unknown unset operation: %s",
			        path, (intmax_t) line_number, argv[0]);
	}
	else
		warning("%s:%ji: unknown operation: %s",
		        path, (intmax_t) line_number, argv[0]);
	return true;
}

static bool daemon_process_line(struct daemon_config* daemon_config,
                                const char* path,
                                char* line,
                                off_t line_number,
                                size_t next_search_path_index)
{
	size_t argc;
	char** argv = tokenize(&argc, line);
	if ( !argv )
	{
		if ( !errno )
			warning("%s:%ji: syntax error", path, (intmax_t) line_number);
		else
			warning("%s: %m", path);
		return false;
	}
	bool result = daemon_process_command(daemon_config, path, argc,
	                                     (const char* const*) argv, line_number,
	                                     next_search_path_index);
	for ( size_t i = 0; i < argc; i++ )
		free(argv[i]);
	free(argv);
	return result;
}

static bool daemon_config_load_from_path(struct daemon_config* daemon_config,
                                         const char* path,
                                         size_t next_search_path_index)
{
	FILE* fp = fopen(path, "r");
	if ( !fp )
	{
		if ( errno != ENOENT )
			warning("%s: Failed to load daemon configuration file: %m", path);
		return false;
	}
	char* line = NULL;
	size_t line_size = 0;
	ssize_t line_length;
	off_t line_number = 0;
	while ( 0 < (line_length = getline(&line, &line_size, fp)) )
	{
		if ( line[line_length-1] == '\n' )
			line[--line_length] = '\0';
		line_number++;
		if ( !daemon_process_line(daemon_config, path, line, line_number,
		                          next_search_path_index) )
		{
			fclose(fp);
			free(line);
			if ( errno == ENOENT )
				errno = EINVAL;
			return false;
		}
	}
	free(line);
	if ( ferror(fp) )
	{
		warning("%s: %m", path);
		fclose(fp);
		return false;
	}
	fclose(fp);
	return true;
}

static bool daemon_config_load_search(struct daemon_config* daemon_config,
                                      size_t next_search_path_index)
{
	// If the search path ever becomes arbitrarily long, consider handling the
	// 'furthermore' feature in a manner using constant stack space rather than
	// recursion.
	const char* search_paths[] =
	{
		"/etc/init",
		"/share/init",
	};
	size_t search_paths_count = sizeof(search_paths) / sizeof(search_paths[0]);
	for ( size_t i = next_search_path_index; i < search_paths_count; i++ )
	{
		const char* search_path = search_paths[i];
		char* path = join_paths(search_path, daemon_config->name);
		if ( !path )
		{
			warning("malloc: %m");
			return false;
		}
		if ( !daemon_config_load_from_path(daemon_config, path, i + 1) )
		{
			free(path);
			if ( errno == ENOENT )
				continue;
			return NULL;
		}
		free(path);
		return true;
	}
	errno = ENOENT;
	return false;
}

static void daemon_config_initialize(struct daemon_config* daemon_config)
{
	memset(daemon_config, 0, sizeof(*daemon_config));
	daemon_config->log_method = default_config.log_method;
	daemon_config->log_format = default_config.log_format;
	daemon_config->log_control_messages = default_config.log_control_messages;
	daemon_config->log_rotate_on_start = default_config.log_rotate_on_start;
	daemon_config->log_rotations = default_config.log_rotations;
	daemon_config->log_line_size = default_config.log_line_size;
	daemon_config->log_size = default_config.log_size;
	daemon_config->log_file_mode = default_config.log_file_mode;
}

static struct daemon_config* daemon_config_load(const char* name)
{
	struct daemon_config* daemon_config = malloc(sizeof(struct daemon_config));
	if ( !daemon_config )
	{
		warning("malloc: %m");
		return NULL;
	}
	daemon_config_initialize(daemon_config);
	if ( !(daemon_config->name = strdup(name)) )
	{
		warning("malloc: %m");
		daemon_config_free(daemon_config);
		return NULL;
	}
	if ( !daemon_config_load_search(daemon_config, 0) )
	{
		if ( errno == ENOENT )
			warning("failed to locate daemon configuration: %s: %m", name);
		daemon_config_free(daemon_config);
		return NULL;
	}
	return daemon_config;
}

// TODO: Replace with better data structure.
static struct daemon* add_daemon(void)
{
	struct daemon* daemon = calloc(1, sizeof(struct daemon));
	if ( !daemon )
		fatal("malloc: %m");
	if ( !array_add((void***) &daemons, &daemons_used, &daemons_length,
	                daemon) )
		fatal("malloc: %m");
	return daemon;
}

// TODO: This runs in O(n) but could be in O(log n).
static struct daemon* daemon_find_by_name(const char* name)
{
	for ( size_t i = 0; i < daemons_used; i++ )
		if ( !strcmp(daemons[i]->name, name) )
			return daemons[i];
	return NULL;
}

// TODO: This runs in O(n) but could be in O(log n).
static struct daemon* daemon_find_by_pid(pid_t pid)
{
	for ( size_t i = 0; i < daemons_used; i++ )
		if ( daemons[i]->pid == pid )
			return daemons[i];
	return NULL;
}

static bool daemon_is_failed(struct daemon* daemon)
{
	if ( daemon->was_terminated &&
	     WIFSIGNALED(daemon->exit_code) &&
	     WTERMSIG(daemon->exit_code) == SIGTERM )
		return false;
	switch ( daemon->exit_code_meaning )
	{
	case EXIT_CODE_MEANING_DEFAULT:
		return !WIFEXITED(daemon->exit_code) ||
		       WEXITSTATUS(daemon->exit_code) != 0;
	case EXIT_CODE_MEANING_POWEROFF_REBOOT:
		return !WIFEXITED(daemon->exit_code) ||
		       3 <= WEXITSTATUS(daemon->exit_code);
	}
	return true;
}

static void daemon_insert_state_list(struct daemon* daemon)
{
	assert(!daemon->prev_by_state);
	assert(!daemon->next_by_state);
	assert(first_daemon_by_state[daemon->state] != daemon);
	assert(last_daemon_by_state[daemon->state] != daemon);
	daemon->prev_by_state = last_daemon_by_state[daemon->state];
	daemon->next_by_state = NULL;
	if ( last_daemon_by_state[daemon->state] )
		last_daemon_by_state[daemon->state]->next_by_state = daemon;
	else
		first_daemon_by_state[daemon->state] = daemon;
	last_daemon_by_state[daemon->state] = daemon;
	count_daemon_by_state[daemon->state]++;
}

static void daemon_remove_state_list(struct daemon* daemon)
{
	assert(daemon->prev_by_state ||
	       daemon == first_daemon_by_state[daemon->state]);
	assert(daemon->next_by_state ||
	       daemon == last_daemon_by_state[daemon->state]);
	assert(0 < count_daemon_by_state[daemon->state]);
	if ( daemon->prev_by_state )
		daemon->prev_by_state->next_by_state = daemon->next_by_state;
	else
		first_daemon_by_state[daemon->state] = daemon->next_by_state;
	if ( daemon->next_by_state )
		daemon->next_by_state->prev_by_state = daemon->prev_by_state;
	else
		last_daemon_by_state[daemon->state] = daemon->prev_by_state;
	count_daemon_by_state[daemon->state]--;
	daemon->prev_by_state = NULL;
	daemon->next_by_state = NULL;
}

static void daemon_change_state_list(struct daemon* daemon,
                                     enum daemon_state new_state)
{
	daemon_remove_state_list(daemon);
	daemon->state = new_state;
	daemon_insert_state_list(daemon);
}

static struct daemon* daemon_create_unconfigured(const char* name)
{
	struct daemon* daemon = add_daemon();
	if ( !(daemon->name = strdup(name)) )
		fatal("malloc: %m");
	daemon->state = DAEMON_STATE_TERMINATED;
	daemon->readyfd = -1;
	daemon->outputfd = -1;
	daemon->log.fd = -1;
	daemon_insert_state_list(daemon);
	return daemon;
}

static bool daemon_add_dependency(struct daemon* daemon,
                                  struct daemon* target,
                                  int flags)
{
	struct dependency* dependency = calloc(1, sizeof(struct dependency));
	if ( !dependency )
		return false;
	dependency->source = daemon;
	dependency->target = target;
	dependency->flags = flags;
	if ( !array_add((void***) &daemon->dependencies,
	               &daemon->dependencies_used,
	               &daemon->dependencies_length,
	               dependency) )
	{
		free(dependency);
		return false;
	}
	if ( !array_add((void***) &target->dependents,
	               &target->dependents_used,
	               &target->dependents_length,
	               dependency) )
	{
		daemon->dependencies_used--;
		free(dependency);
		return false;
	}
	if ( flags & DEPENDENCY_FLAG_EXIT_CODE )
		daemon->exit_code_from = dependency;
	target->reference_count++;
	return true;
}

static void daemon_configure_sub(struct daemon* daemon,
                                 struct daemon_config* daemon_config,
                                 const char* netif)
{
	assert(!daemon->configured);
	daemon->dependencies = (struct dependency**)
		reallocarray(NULL, daemon_config->dependencies_used,
		             sizeof(struct dependency*));
	if ( !daemon->dependencies )
		fatal("malloc: %m");
	daemon->dependencies_used = 0;
	daemon->dependencies_length = daemon_config->dependencies_length;
	for ( size_t i = 0; i < daemon_config->dependencies_used; i++ )
	{
		struct dependency_config* dependency_config =
			daemon_config->dependencies[i];
		struct daemon* target = daemon_find_by_name(dependency_config->target);
		if ( !target )
			target = daemon_create_unconfigured(dependency_config->target);
		if ( target->netif )
		{
			// daemon_find_by_name cannot create daemons per if.
			warning("%s cannot depend on parameterized daemon %s",
			        daemon->name, target->name);
			continue;
		}
		if ( !daemon_add_dependency(daemon, target, dependency_config->flags) )
			fatal("malloc: %m");
	}
	if ( daemon_config->cd && !(daemon->cd = strdup(daemon_config->cd)) )
		fatal("malloc: %m");
	if ( daemon_config->argv )
	{
		daemon->argc = daemon_config->argc;
		if ( netif )
		{
			if ( INT_MAX - 1 <= daemon->argc )
			{
				errno = ENOMEM;
				fatal("malloc: %m");
			}
			daemon->argc++;
		}
		daemon->argv = calloc(daemon->argc, sizeof(char*));
		if ( !daemon->argv )
			fatal("malloc: %m");
		for ( int i = 0; i < daemon_config->argc; i++ )
			if ( !(daemon->argv[i] = strdup(daemon_config->argv[i])) )
				fatal("malloc: %m");
		if ( netif && !(daemon->argv[daemon_config->argc] = strdup(netif)) )
			fatal("malloc: %m");
	}
	daemon->exit_code_meaning = daemon_config->exit_code_meaning;
	if ( netif && !(daemon->netif = strdup(netif)) )
		fatal("malloc: %m");
	if ( !log_initialize(&daemon->log, daemon->name, daemon_config) )
		fatal("malloc: %m");
	daemon->need_tty = daemon_config->need_tty;
	daemon->configured = true;
}

static void daemon_configure(struct daemon* daemon,
                             struct daemon_config* daemon_config)
{
	if ( daemon_config->per_if )
	{
		struct if_nameindex* ifs = if_nameindex();
		if ( !ifs )
			fatal("if_nameindex: %m");
		for ( size_t i = 0; ifs[i].if_name; i++ )
		{
			const char* netif = ifs[i].if_name;
			char* parameterized_name;
			if ( asprintf(&parameterized_name, "%s.%s",
			              daemon_config->name, netif) < 0 )
				fatal("malloc: %m");
			struct daemon* parameterized =
				daemon_create_unconfigured(parameterized_name);
			free(parameterized_name);
			if ( !(parameterized->netif = strdup(netif)) )
				fatal("malloc: %m");
			char* name_clone = strdup(parameterized->name);
			if ( !name_clone )
				fatal("malloc: %m");
			int flags = DEPENDENCY_FLAG_REQUIRE | DEPENDENCY_FLAG_AWAIT;
			if ( !daemon_add_dependency(daemon, parameterized, flags) )
				fatal("malloc: %m");
			daemon_configure_sub(parameterized, daemon_config, netif);
		}
		if_freenameindex(ifs);
		daemon->configured = true;
	}
	else
		daemon_configure_sub(daemon, daemon_config, NULL);
}

static struct daemon* daemon_create(struct daemon_config* daemon_config)
{
	struct daemon* daemon = daemon_create_unconfigured(daemon_config->name);
	daemon_configure(daemon, daemon_config);
	return daemon;
}

static void schedule_daemon(struct daemon* daemon)
{
	assert(daemon->state == DAEMON_STATE_TERMINATED);
	daemon_change_state_list(daemon, DAEMON_STATE_SCHEDULED);
}

// TODO: Mutual recursion.
static void daemon_on_finished(struct daemon* daemon);
static void daemon_mark_finished(struct daemon* daemon);

static void daemon_terminate(struct daemon* daemon)
{
	assert(!daemon->was_terminated);
	daemon->was_terminated = true;
	if ( 0 < daemon->pid )
	{
		log_status("stopping", "Stopping %s.\n", daemon->name);
		// TODO: Send SIGKILL after a timeout.
		kill(daemon->pid, SIGTERM);
		daemon_change_state_list(daemon, DAEMON_STATE_TERMINATING);
	}
	else
	{
		daemon_change_state_list(daemon, DAEMON_STATE_TERMINATING);
		// TODO: Recursion.
		daemon_on_finished(daemon);
	}
}

static void daemon_on_not_referenced(struct daemon* daemon)
{
	assert(daemon->reference_count == 0);
	if ( daemon->state == DAEMON_STATE_TERMINATED ||
	     daemon->state == DAEMON_STATE_SCHEDULED ||
	     daemon->state == DAEMON_STATE_WAITING ||
	     daemon->state == DAEMON_STATE_SATISFIED )
	{
		daemon_mark_finished(daemon);
		assert(daemon->state == DAEMON_STATE_FINISHED);
	}
	else if ( daemon->state == DAEMON_STATE_STARTING ||
	          daemon->state == DAEMON_STATE_READY ||
	          daemon->state == DAEMON_STATE_RUNNING )
	{
		daemon_terminate(daemon);
		// Dependencies are dereferenced when the daemon terminates.
	}
	else
	{
		// Dependencies are dereferenced when the daemon terminates.
	}
}

static void daemon_dereference(struct daemon* daemon)
{
	assert(0 < daemon->reference_count);
	daemon->reference_count--;
	// TODO: Recursion.
	if ( !daemon->reference_count )
		daemon_on_not_referenced(daemon);
}

static void daemon_dereference_dependencies(struct daemon* daemon)
{
	assert(!daemon->was_dereferenced);
	daemon->was_dereferenced = true;
	for ( size_t i = 0; i < daemon->dependencies_used; i++ )
		daemon_dereference(daemon->dependencies[i]->target);
}

static void daemon_on_dependency_ready(struct dependency* dependency)
{
	struct daemon* daemon = dependency->source;
	if ( !(dependency->flags & DEPENDENCY_FLAG_AWAIT) )
		return;
	daemon->dependencies_ready++;
	if ( daemon->state == DAEMON_STATE_WAITING &&
	     daemon->dependencies_ready == daemon->dependencies_used )
		daemon_change_state_list(daemon, DAEMON_STATE_SATISFIED);
}

static void daemon_mark_ready(struct daemon* daemon)
{
	daemon_change_state_list(daemon, DAEMON_STATE_RUNNING);
	daemon->was_ready = true;
	for ( size_t i = 0; i < daemon->dependents_used; i++ )
		daemon_on_dependency_ready(daemon->dependents[i]);
}

static void daemon_on_ready(struct daemon* daemon)
{
	log_status("started", "Started %s.\n", daemon->name);
	daemon_mark_ready(daemon);
}

static void daemon_on_dependency_finished(struct dependency* dependency)
{
	struct daemon* daemon = dependency->source;
	struct daemon* target = dependency->target;
	daemon->dependencies_finished++;
	// TODO: This is to protect against virtual daemons where daemon_terminate
	//       has already been called from calling daemon_on_finished again. Best
	//       way to do this?
	if ( daemon->state == DAEMON_STATE_FINISHED )
		return;
	bool failed = (dependency->flags & DEPENDENCY_FLAG_REQUIRE) &&
	              daemon_is_failed(target);
	if ( failed )
		daemon->dependencies_failed++;
	if ( daemon->argv )
	{
		if ( failed )
		{
			// TODO: If still waiting for dependencies to start, fail early.
		}
	}
	else if ( daemon->exit_code_from )
	{
		if ( dependency->flags & DEPENDENCY_FLAG_EXIT_CODE )
		{
			daemon->exit_code = target->exit_code;
			daemon->exit_code_meaning = target->exit_code_meaning;
			// TODO: Recursion.
			daemon_on_finished(daemon);
		}
		// TODO: This ignores the exit code from other dependencies. Make it
		//       possible to still fail early if a key dependency fails, even
		//       if we want the exit code from a particular dependency.
	}
	else
	{
		// TODO: This gathers all the exit codes before the virtual daemon
		//       finished, maybe have a kind of virtual daemon that finished on
		//       first error?
		if ( failed )
			daemon->exit_code = WCONSTRUCT(WNATURE_EXITED, 3, 0);
		if ( daemon->dependencies_finished == daemon->dependencies_used )
		{
			// TODO: Recursion.
			daemon_on_finished(daemon);
		}
	}
}

static void daemon_mark_finished(struct daemon* daemon)
{
	assert(daemon->state != DAEMON_STATE_FINISHED);
	if ( !daemon->was_ready )
		daemon_mark_ready(daemon);
	daemon_change_state_list(daemon, DAEMON_STATE_FINISHED);
	for ( size_t i = 0; i < daemon->dependents_used; i++ )
		daemon_on_dependency_finished(daemon->dependents[i]);
	daemon_dereference_dependencies(daemon);
}

static void daemon_on_finished(struct daemon* daemon)
{
	assert(daemon->state != DAEMON_STATE_FINISHED);
	if ( daemon_is_failed(daemon) )
		log_status("failed", "%s exited unsuccessfully.\n", daemon->name);
	else if ( daemon->state == DAEMON_STATE_TERMINATING )
		log_status("stopped", "Stopped %s.\n", daemon->name);
	else
		log_status("finished", "Finished %s.\n", daemon->name);
	daemon_mark_finished(daemon);
}

static void daemon_on_startup_error(struct daemon* daemon)
{
	assert(daemon->state != DAEMON_STATE_FINISHED);
	daemon_mark_finished(daemon);
}

static void daemon_register_pollfd(struct daemon* daemon,
                                   int fd,
                                   size_t* out_index,
                                   short events)
{
	assert(pfds_used < pfds_length);
	assert(pfds_used < pfds_daemon_length);
	size_t index = pfds_used++;
	struct pollfd* pfd = pfds + index;
	memset(pfd, 0, sizeof(*pfd));
	pfd->fd = fd;
	pfd->events = events;
	pfds_daemon[index] = daemon;
	*out_index = index;
}

static void daemon_unregister_pollfd(struct daemon* daemon, size_t index)
{
	assert(pfds_used <= pfds_length);
	assert(index < pfds_used);
	assert(pfds_daemon[index] == daemon);
	// This function is relied on to not mess with any pollfds prior to the
	// index, so it doesn't break a forward iteration on the pollfds.
	size_t last_index = pfds_used - 1;
	if ( index != last_index )
	{
		memcpy(pfds + index, pfds + last_index, sizeof(*pfds));
		pfds_daemon[index] = pfds_daemon[last_index];
		if ( 0 <= pfds_daemon[index]->readyfd &&
		     pfds_daemon[index]->pfd_readyfd_index == last_index )
			pfds_daemon[index]->pfd_readyfd_index = index;
		if ( 0 <= pfds_daemon[index]->outputfd &&
		     pfds_daemon[index]->pfd_outputfd_index == last_index )
			pfds_daemon[index]->pfd_outputfd_index = index;
	}
	pfds_used--;
	memset(pfds + last_index, 0, sizeof(*pfds));
	pfds_daemon[last_index] = NULL;
}

static void daemon_schedule(struct daemon* daemon)
{
	assert(daemon->state == DAEMON_STATE_SCHEDULED);
	if ( !daemon->configured )
	{
		struct daemon_config* daemon_config = daemon_config_load(daemon->name);
		if ( !daemon_config )
		{
			log_status("failed", "Failed to load configuration for %s.\n",
			           daemon->name);
			daemon->exit_code = WCONSTRUCT(WNATURE_EXITED, 3, 0);
			daemon_on_startup_error(daemon);
			return NULL;
		}
		daemon_configure(daemon, daemon_config);
		daemon_config_free(daemon_config);
	}
	for ( size_t i = 0; i < daemon->dependencies_used; i++ )
	{
		// TODO: Require the dependency graph to be an directed acylic graph.
		struct dependency* dependency = daemon->dependencies[i];
		assert(dependency->source == daemon);
		assert(dependency->target);

		if ( dependency->target->state == DAEMON_STATE_TERMINATED )
		{
			schedule_daemon(dependency->target);
			if ( !(dependency->flags & DEPENDENCY_FLAG_AWAIT) )
				daemon->dependencies_ready++;
		}
		else if ( dependency->target->state == DAEMON_STATE_SCHEDULED ||
		          dependency->target->state == DAEMON_STATE_SATISFIED ||
		          dependency->target->state == DAEMON_STATE_STARTING )
		{
			// Daemon start is already in progress.
			if ( !(dependency->flags & DEPENDENCY_FLAG_AWAIT) )
				daemon->dependencies_ready++;
		}
		else if ( dependency->target->state == DAEMON_STATE_RUNNING )
		{
			daemon->dependencies_ready++;
		}
		else if ( dependency->target->state == DAEMON_STATE_TERMINATING )
		{
			// TODO: Bring it back up first. How?
		}
		else if ( dependency->target->state == DAEMON_STATE_FINISHED )
		{
			daemon->dependencies_ready++;
			daemon->dependencies_finished++;
			if ( (dependency->flags & DEPENDENCY_FLAG_REQUIRE) &&
			     daemon_is_failed(daemon) )
			{
				daemon->dependencies_failed++;
				// TODO: Don't start more dependencies.
				//break;
			}
		}
		else
		{
			// TODO: This is unreachable.
		}
	}
	if ( daemon->dependencies_ready < daemon->dependencies_used )
		daemon_change_state_list(daemon, DAEMON_STATE_WAITING);
	else
		daemon_change_state_list(daemon, DAEMON_STATE_SATISFIED);
}

static void daemon_start(struct daemon* daemon)
{
	assert(daemon->state == DAEMON_STATE_SATISFIED);
	if ( !daemon->argv )
	{
		daemon_on_ready(daemon);
		if ( daemon->exit_code_from )
		{
			struct daemon* target = daemon->exit_code_from->target;
			if ( target->state == DAEMON_STATE_FINISHED )
			{
				daemon->exit_code = target->exit_code;
				daemon->exit_code_meaining = target->exit_code_meaining;
				daemon_on_finished(daemon);
			}
		}
		else if ( daemon->dependencies_finished == daemon->dependencies_used )
		{
			daemon_on_finished(daemon);
		}
		return;
	}
	// TODO: Support for a virtual daemon using the exit-code flag to still fail
	//       if certain key dependencies fail.
	if ( 0 < daemon->dependencies_failed )
	{
		log_status("failed", "Failed to start %s due to failed dependencies.\n",
		           daemon->name);
		daemon->exit_code = WCONSTRUCT(WNATURE_EXITED, 3, 0);
		daemon_on_startup_error(daemon);
		return;
	}
	log_status("starting", "Starting %s...\n", daemon->name);
	uid_t uid = getuid();
	pid_t ppid = getpid();
	struct passwd* pwd = getpwuid(uid);
	if ( !pwd )
		fatal("looking up user by uid %" PRIuUID ": %m", uid);
	const char* home = pwd->pw_dir[0] ? pwd->pw_dir : "/";
	const char* shell = pwd->pw_shell[0] ? pwd->pw_shell : "sh";
	const char* cd = daemon->cd ? daemon->cd : "/";
	// TODO: This is a hack.
	if ( !strcmp(cd, "$HOME") )
		cd = home;
	int outputfds[2];
	int readyfds[2];
	if ( !daemon->need_tty )
	{
		size_t required_fds = 2;
		if ( pfds_length - pfds_used < required_fds )
		{
			size_t old_length = pfds_length ? pfds_length : required_fds;
			struct pollfd* new_pfds =
				reallocarray(pfds, old_length, 2 * sizeof(struct pollfd));
			if ( !new_pfds )
				fatal("malloc");
			pfds = new_pfds;
			pfds_length = old_length * 2;
		}
		if ( pfds_daemon_length - pfds_used < required_fds )
		{
			size_t old_length =
				pfds_daemon_length ? pfds_daemon_length : required_fds;
			struct daemon** new_pfds_daemon =
				reallocarray(pfds_daemon, old_length,
				             2 * sizeof(struct daemon*));
			if ( !new_pfds_daemon )
				fatal("malloc");
			pfds_daemon = new_pfds_daemon;
			pfds_daemon_length = old_length * 2;
		}
		if ( !log_begin(&daemon->log) )
		{
			// TODO: Should we not stop the daemon?
		}
		if ( pipe(outputfds) < 0 )
			fatal("pipe");
		daemon->outputfd = outputfds[0];
		fcntl(daemon->outputfd, F_SETFL, O_NONBLOCK);
		// Setup the pollfd for the outputfd.
		daemon_register_pollfd(daemon, daemon->outputfd,
		                       &daemon->pfd_outputfd_index, POLLIN);
		// Create the readyfd.
		if ( pipe(readyfds) < 0 )
			fatal("pipe");
		daemon->readyfd = readyfds[0];
		fcntl(daemon->readyfd, F_SETFL, O_NONBLOCK);
		// Setup the pollfd for the readyfd.
		daemon_register_pollfd(daemon, daemon->readyfd,
		                       &daemon->pfd_readyfd_index, POLLIN);
	}
	// TODO: This is not concurrency safe, build a environment array just for
	//       this daemon.
	char ppid_str[sizeof(pid_t) * 3];
	snprintf(ppid_str, sizeof(ppid_str), "%" PRIiPID, ppid);
	if ( (!daemon->need_tty && setenv("READYFD", "3", 1)) < 0 ||
	     setenv("INIT_PID", ppid_str, 1) < 0 ||
	     setenv("LOGNAME", pwd->pw_name, 1) < 0 ||
	     setenv("USER", pwd->pw_name, 1) < 0 ||
	     setenv("HOME", home, 1) < 0 ||
	     setenv("SHELL", shell, 1) < 0 )
		fatal("setenv");
	daemon->pid = daemon->log.pid = fork();
	if ( daemon->pid < 0 )
		fatal("fork: %m");
	if ( daemon->need_tty )
	{
		// TODO: Any chance of SIGTTIN?
		if ( tcgetattr(0, &daemon->oldtio) )
			fatal("tcgetattr: %m");
	}
	if ( daemon->pid == 0 )
	{
		uninstall_signal_handler();
		if ( chdir(cd) )
		{
			// TODO: Right way to send a fatal error back?
			fatal("chdir: %s: %m", cd);
		}
		if ( daemon->need_tty )
		{
			pid_t pid = getpid();
			// TODO: Support for setsid().
			if ( setpgid(0, 0) < 0 )
				fatal("setpgid: %m");
			sigset_t oldset, sigttou;
			sigemptyset(&sigttou);
			sigaddset(&sigttou, SIGTTOU);
			sigprocmask(SIG_BLOCK, &sigttou, &oldset);
			if ( tcsetpgrp(0, pid) < 0 )
				fatal("tcsetpgrp: %m");
			daemon->oldtio.c_cflag |= CREAD;
			if ( tcsetattr(0, TCSANOW, &daemon->oldtio) )
				fatal("tcgetattr: %m");
			sigprocmask(SIG_SETMASK, &oldset, NULL);
			closefrom(3);
		}
		else
		{
			close(0);
			close(1);
			close(2);
			open("/dev/null", O_RDONLY);
			// TODO: Fix daemon's logging to stdout instead of stderr.
			//open("/dev/null", O_WRONLY);
			dup2(outputfds[1], 1);
			dup2(outputfds[1], 2);
			dup2(readyfds[1], 3);
			closefrom(4);
		}
		// TODO: This is a hack.
		if ( !strcmp(daemon->argv[0], "$SHELL") )
			daemon->argv[0] = (char*) shell;
		execvp(daemon->argv[0], daemon->argv);
		// TODO: Use a pipe to send the errno back.
		warning("%s: %m", daemon->argv[0]);
		_exit(127);
	}
	if ( !daemon->need_tty )
	{
		close(outputfds[1]);
		close(readyfds[1]);
	}
	// TODO: Not thread safe.
	// TODO: Also unset other things.
	if ( !daemon->need_tty )
		unsetenv("READYFD");
	unsetenv("INIT_PID");
	unsetenv("LOGNAME");
	unsetenv("USER");
	unsetenv("HOME");
	unsetenv("SHELL");
	if ( daemon->need_tty )
		daemon_on_ready(daemon);
	else
		daemon_change_state_list(daemon, DAEMON_STATE_STARTING);
}

static bool daemon_process_ready(struct daemon* daemon)
{
	char c;
	ssize_t amount = read(daemon->readyfd, &c, sizeof(c));
	if ( amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) )
		return true;
	if ( amount < 0 )
		return false;
	else if ( amount == 0 )
		return false;
	if ( c == '\n' )
	{
		daemon_on_ready(daemon);
		return false;
	}
	return true;
}

static bool daemon_process_output(struct daemon* daemon)
{
	char data[4096];
	ssize_t amount = read(daemon->outputfd, data, sizeof(data));
	if ( amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) )
		return true;
	if ( amount < 0 )
		return false;
	else if ( amount == 0 )
		return false;
	log_formatted(&daemon->log, data, amount);
	return true;
}

static void daemon_on_exit(struct daemon* daemon, int exit_code)
{
	assert(daemon->state != DAEMON_STATE_FINISHED);
	daemon->exit_code = exit_code;
	if ( 0 <= daemon->readyfd )
	{
		daemon_unregister_pollfd(daemon, daemon->pfd_readyfd_index);
		close(daemon->readyfd);
		daemon->readyfd = -1;
	}
	if ( 0 <= daemon->outputfd )
	{
		daemon_process_output(daemon);
		daemon_unregister_pollfd(daemon, daemon->pfd_outputfd_index);
		close(daemon->outputfd);
		daemon->outputfd = -1;
	}
	if ( 0 <= daemon->log.fd )
		log_close(&daemon->log);
	if ( daemon->need_tty )
	{
		// TODO: There is a race condition between getting the exit code from
		//       waitpid and us reclaiming it here where some other process that
		//       happened to get the right pid may own the tty.
		sigset_t oldset, sigttou;
		sigemptyset(&sigttou);
		sigaddset(&sigttou, SIGTTOU);
		sigprocmask(SIG_BLOCK, &sigttou, &oldset);
		if ( tcsetattr(0, TCSAFLUSH, &daemon->oldtio) )
			fatal("tcsetattr: %m");
		if ( tcsetpgrp(0, getpgid(0)) < 0 )
			fatal("tcsetpgrp: %m");
		sigprocmask(SIG_SETMASK, &oldset, NULL);
	}
	daemon_on_finished(daemon);
}

// TODO. This can just be directly inlined to main as the mainloop.
static void schedule(void)
{
	int default_daemon_exit_code = -1;

	while ( first_daemon_by_state[DAEMON_STATE_SCHEDULED] ||
	        //first_daemon_by_state[DAEMON_STATE_WAITING] || // TODO?
	        first_daemon_by_state[DAEMON_STATE_SATISFIED] ||
	        first_daemon_by_state[DAEMON_STATE_STARTING] ||
	        first_daemon_by_state[DAEMON_STATE_READY] ||
	        first_daemon_by_state[DAEMON_STATE_RUNNING] ||
	        first_daemon_by_state[DAEMON_STATE_TERMINATING] )
	{
		if ( caught_exit_signal != -1 && default_daemon_exit_code == -1)
		{
			struct daemon* default_daemon = daemon_find_by_name("default");
			if ( caught_exit_signal == 0 )
				log_status("stopped", "Powering off...\n");
			else if ( caught_exit_signal == 1 )
				log_status("stopped", "Rebooting...\n");
			else if ( caught_exit_signal == 2 )
				log_status("stopped", "Halting...\n");
			else
				log_status("stopped", "Exiting %i...\n", caught_exit_signal);
			if ( default_daemon->state != DAEMON_STATE_FINISHED )
				daemon_mark_finished(default_daemon);
			default_daemon_exit_code =
				WCONSTRUCT(WNATURE_EXITED, caught_exit_signal, 0);
		}
		caught_exit_signal = -1;

		while ( first_daemon_by_state[DAEMON_STATE_SCHEDULED] )
		{
			struct daemon* daemon = first_daemon_by_state[DAEMON_STATE_SCHEDULED];
			daemon_schedule(daemon);
		}
		while ( first_daemon_by_state[DAEMON_STATE_SATISFIED] )
		{
			struct daemon* daemon =
				first_daemon_by_state[DAEMON_STATE_SATISFIED];
			// TODO: Error handling.
			daemon_start(daemon);
		}
		struct timespec timeout = timespec_make(-1, 0);
		sigset_t mask;
		sigemptyset(&mask);
		sigaddset(&mask, SIGCHLD);
		sigset_t oldset;
		sigprocmask(SIG_BLOCK, &mask, &oldset);
		sigset_t unhandled_signals;
		signotset(&unhandled_signals, &handled_signals);
		sigset_t pollset;
		sigandset(&pollset, &oldset, &unhandled_signals);
		int exit_code;
		pid_t pid;
		while ( 0 < (pid = waitpid(-1, &exit_code, WNOHANG)) )
		{
			struct daemon* daemon = daemon_find_by_pid(pid);
			if ( daemon )
				daemon_on_exit(daemon, exit_code);
			timeout = timespec_make(0, 0);
		}
		// Set a dummy SIGCHLD handler to ensure we get EINTR during ppoll(2).
		struct sigaction sa = { 0 };
		sa.sa_handler = signal_handler;
		sa.sa_flags = 0;
		struct sigaction old_sa;
		sigaction(SIGCHLD, &sa, &old_sa);
		// Await either an event, a timeout, or SIGCHLD.
		int nevents = ppoll(pfds, pfds_used, &timeout, &pollset);
		sigaction(SIGCHLD, &old_sa, NULL);
		sigprocmask(SIG_SETMASK, &oldset, NULL);
		if ( nevents < 0 && errno != EINTR )
			fatal("ppoll: %m");
		for ( size_t i = 0; i < pfds_used; i++ )
		{
			if ( nevents <= 0 )
				break;
			struct pollfd* pfd = pfds + i;
			if ( !pfd->revents )
				continue;
			nevents--;
			struct daemon* daemon = pfds_daemon[i];
			if ( 0 <= daemon->readyfd && pfd->fd == daemon->readyfd )
			{
				// TODO: POLLHUP? POLLERR? POLLNVAL?
				if ( pfd->revents & (POLLIN | POLLHUP) )
				{
					if ( !daemon_process_ready(daemon) )
					{
						daemon_unregister_pollfd(daemon,
						                         daemon->pfd_readyfd_index);
						close(daemon->readyfd);
						daemon->readyfd = -1;
						i--; // Process this index again (something new there).
					}
				}
			}
			else if ( 0 <= daemon->outputfd && pfd->fd == daemon->outputfd )
			{
				// TODO: POLLHUP? POLLERR? POLLNVAL?
				if ( pfd->revents & (POLLIN | POLLHUP) )
				{
					if ( !daemon_process_output(daemon) )
					{
						daemon_unregister_pollfd(daemon,
						                         daemon->pfd_outputfd_index);
						close(daemon->outputfd);
						daemon->outputfd = -1;
						i--; // Process this index again (something new there).
					}
				}
			}
			else
			{
				assert(false);
			}
		}
	}

	// Collect child processes reparented to us that we don't know about and
	// attempt to politely shut them down with SIGTERM and SIGKILL after a
	// timeout.
	sigset_t saved_mask, sigchld_mask;
	sigemptyset(&sigchld_mask);
	sigaddset(&sigchld_mask, SIGCHLD);
	sigprocmask(SIG_BLOCK, &sigchld_mask, &saved_mask);
	struct sigaction sa = { .sa_handler = signal_handler };
	struct sigaction old_sa;
	sigaction(SIGCHLD, &sa, &old_sa);
	struct timespec timeout = timespec_make(30, 0);
	struct timespec begun;
	clock_gettime(CLOCK_MONOTONIC, &begun);
	bool sent_sigterm = false;
	while ( true )
	{
		int exit_code;
		for ( pid_t pid = 1; 0 < pid; pid = waitpid(-1, &exit_code, WNOHANG) );

		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		struct timespec elapsed = timespec_sub(now, begun);

		struct psctl_stat psst;
		if ( psctl(getpid(), PSCTL_STAT, &psst) < 0 )
			fatal("psctl: %m");
		bool any_unknown = false;
		for ( pid_t pid = psst.ppid_first; pid != -1; pid = psst.ppid_next )
		{
			if ( psctl(pid, PSCTL_STAT, &psst) < 0 )
			{
				warn("psctl: %ji", (intmax_t) pid);
				break;
			}
			bool known = false;
			for ( size_t i = 0; !known && i < mountpoints_used; i++ )
				if ( mountpoints[i].pid == pid )
					known = true;
			if ( !known )
			{
				any_unknown = true;
				if ( !sent_sigterm )
					kill(pid, SIGTERM);
				// TODO: Hostile processes can try to escape by spawning more
				//       processes, a kernel feature is needed to recursively
				//       send a signal to all descendants atomically, although
				//       want to avoid known safe processes (mountpoints) and
				//       still catch processes reparented to us. Otherwise
				//       retrying until we succeed is the best we can do.
				else if ( timespec_le(timeout, elapsed) )
					kill(pid, SIGKILL);
			}
		}

		sent_sigterm = true;

		if ( !any_unknown )
			break;

		// Wait for the timeout to happen, or for another process to exit by
		// the poll failing with EINTR because a pending SIGCHLD was delivered
		// when the saved signal mask is restored.
		struct timespec left = timespec_sub(timeout, elapsed);
		if ( left.tv_sec < 0 || (left.tv_sec == 0 && left.tv_nsec == 0) )
			left = timespec_make(1, 0);
		struct pollfd pfd = { .fd = -1 };
		ppoll(&pfd, 1, &left, &saved_mask);
	}
	sigaction(SIGCHLD, &old_sa, NULL);
	sigprocmask(SIG_SETMASK, &saved_mask, NULL);

	if ( default_daemon_exit_code != -1 )
		daemon_find_by_name("default")->exit_code = default_daemon_exit_code;
}

static void write_random_seed(void)
{
	const char* will_not = "next boot will not have fresh randomness";
	const char* path = "/boot/random.seed";
	int fd = open(path, O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
	if ( fd < 0 )
	{
		if ( errno != ENOENT && errno != EROFS )
			warning("%s: %s: %m", will_not, path);
		return;
	}
	if ( fchown(fd, 0, 0) < 0 )
	{
		warning("%s: chown: %s: %m", will_not, path);
		close(fd);
		return;
	}
	if ( fchmod(fd, 0600) < 0 )
	{
		warning("%s: chown: %s: %m", will_not, path);
		close(fd);
		return;
	}
	// Write out randomness, but mix in some fresh kernel randomness in case the
	// randomness used to seed arc4random didn't have enough entropy, there may
	// be more now.
	unsigned char buf[256];
	arc4random_buf(buf, sizeof(buf));
	unsigned char newbuf[256];
	getentropy(newbuf, sizeof(newbuf));
	for ( size_t i = 0; i < 256; i++ )
		buf[i] ^= newbuf[i];
	size_t done = writeall(fd, buf, sizeof(buf));
	explicit_bzero(buf, sizeof(buf));
	if ( done < sizeof(buf) )
	{
		warning("%s: write: %s: %m", will_not, path);
		close(fd);
		return;
	}
	if ( ftruncate(fd, sizeof(buf)) < 0  )
	{
		warning("%s: truncate: %s: %m", will_not, path);
		close(fd);
		return;
	}
	close(fd);
}

static void prepare_filesystem(const char* path, struct blockdevice* bdev)
{
	enum filesystem_error fserr = blockdevice_inspect_filesystem(&bdev->fs, bdev);
	if ( fserr == FILESYSTEM_ERROR_ABSENT ||
	     fserr == FILESYSTEM_ERROR_UNRECOGNIZED )
		return;
	if ( fserr != FILESYSTEM_ERROR_NONE )
		return warning("probing: %s: %s", path, filesystem_error_string(fserr));
}

static bool prepare_block_device(void* ctx, const char* path)
{
	(void) ctx;
	struct harddisk* hd = harddisk_openat(AT_FDCWD, path, O_RDONLY);
	if ( !hd )
	{
		int true_errno = errno;
		struct stat st;
		if ( lstat(path, &st) == 0 && !S_ISBLK(st.st_mode) )
			return true;
		errno = true_errno;
		fatal("%s: %m", path);
	}
	if ( !harddisk_inspect_blockdevice(hd) )
	{
		if ( errno == ENOTBLK || errno == ENOMEDIUM )
			return true;
		if ( errno == EINVAL )
			return warning("%s: %m", path), true;
		fatal("%s: %m", path);
	}
	if ( hds_used == hds_length )
	{
		size_t new_half_length = hds_length ? hds_length : 8;
		struct harddisk** new_hds = (struct harddisk**)
			reallocarray(hds, new_half_length, sizeof(struct harddisk*) * 2);
		if ( !new_hds )
			fatal("realloc: %m");
		hds = new_hds;
		hds_length = 2 * new_half_length;
	}
	hds[hds_used++] = hd;
	struct blockdevice* bdev = &hd->bdev;
	enum partition_error parterr = blockdevice_get_partition_table(&bdev->pt, bdev);
	if ( parterr == PARTITION_ERROR_ABSENT ||
	     parterr == PARTITION_ERROR_UNRECOGNIZED )
	{
		prepare_filesystem(path, bdev);
		return true;
	}
	else if ( parterr == PARTITION_ERROR_ERRNO )
	{
		if ( errno == EIO || errno == EINVAL )
			warning("%s: %s", path, partition_error_string(parterr));
		else
			fatal("%s: %s", path, partition_error_string(parterr));
		return true;
	}
	else if ( parterr != PARTITION_ERROR_NONE )
	{
		warning("%s: %s", path, partition_error_string(parterr));
		return true;
	}
	for ( size_t i = 0; i < bdev->pt->partitions_count; i++ )
	{
		struct partition* p = bdev->pt->partitions[i];
		assert(p->path);
		struct stat st;
		if ( stat(p->path, &st) == 0 )
		{
			// TODO: Check the existing partition has the right offset and
			//       length, but definitely do not recreate it if it already
			//       exists properly.
		}
		else if ( errno == ENOENT )
		{
			int mountfd = open(p->path, O_RDONLY | O_CREAT | O_EXCL);
			if ( mountfd < 0 )
				fatal("%s:˙%m", p->path);
			int partfd = mkpartition(hd->fd, p->start, p->length);
			if ( partfd < 0 )
				fatal("mkpartition: %s:˙%m", p->path);
			if ( fsm_fsbind(partfd, mountfd, 0) < 0 )
				fatal("fsbind: %s:˙%m", p->path);
			close(partfd);
			close(mountfd);
		}
		else
		{
			fatal("stat: %s: %m", p->path);
		}
		prepare_filesystem(p->path, &p->bdev);
	}
	return true;
}

static void prepare_block_devices(void)
{
	static bool done = false;
	if ( done )
		return;
	done = true;

	if ( !devices_iterate_path(prepare_block_device, NULL) )
		fatal("iterating devices: %m");
}

static void search_by_uuid(const char* uuid_string,
                           void (*cb)(void*, struct device_match*),
                           void* ctx)
{
	unsigned char uuid[16];
	uuid_from_string(uuid, uuid_string);
	for ( size_t i = 0; i < hds_used; i++ )
	{
		struct blockdevice* bdev = &hds[i]->bdev;
		if ( bdev->fs )
		{
			struct filesystem* fs = bdev->fs;
			if ( !(fs->flags & FILESYSTEM_FLAG_UUID) )
				continue;
			if ( memcmp(uuid, fs->uuid, 16) != 0 )
				continue;
			struct device_match match;
			match.path = hds[i]->path;
			match.bdev = bdev;
			cb(ctx, &match);
		}
		else if ( bdev->pt )
		{
			for ( size_t j = 0; j < bdev->pt->partitions_count; j++ )
			{
				struct partition* p = bdev->pt->partitions[j];
				if ( !p->bdev.fs )
					continue;
				struct filesystem* fs = p->bdev.fs;
				if ( !(fs->flags & FILESYSTEM_FLAG_UUID) )
					continue;
				if ( memcmp(uuid, fs->uuid, 16) != 0 )
					continue;
				struct device_match match;
				match.path = p->path;
				match.bdev = &p->bdev;
				cb(ctx, &match);
			}
		}
	}
}

static void ensure_single_device_match(void* ctx, struct device_match* match)
{
	struct device_match* result = (struct device_match*) ctx;
	if ( result->path )
	{
		if ( result->bdev )
			note("duplicate match: %s", result->path);
		result->bdev = NULL;
		note("duplicate match: %s", match->path);
		return;
	}
	*result = *match;
}

static int sort_mountpoint(const void* a_ptr, const void* b_ptr)
{
	const struct mountpoint* a = (const struct mountpoint*) a_ptr;
	const struct mountpoint* b = (const struct mountpoint*) b_ptr;
	return strcmp(a->entry.fs_file, b->entry.fs_file);
}

static void load_fstab(void)
{
	FILE* fp = fopen("/etc/fstab", "r");
	if ( !fp )
	{
		if ( errno == ENOENT )
			return;
		fatal("/etc/fstab: %m");
	}
	char* line = NULL;
	size_t line_size;
	ssize_t line_length;
	while ( 0 < (line_length = getline(&line, &line_size, fp)) )
	{
		if ( line[line_length - 1] == '\n' )
			line[--line_length] = '\0';
		struct fstab fstabent;
		if ( !scanfsent(line, &fstabent) )
			continue;
		if ( mountpoints_used == mountpoints_length )
		{
			size_t new_length = 2 * mountpoints_length;
			if ( !new_length )
				new_length = 16;
			struct mountpoint* new_mountpoints = (struct mountpoint*)
				reallocarray(mountpoints, new_length, sizeof(struct mountpoint));
			if ( !new_mountpoints )
				fatal("malloc: %m");
			mountpoints = new_mountpoints;
			mountpoints_length = new_length;
		}
		struct mountpoint* mountpoint = &mountpoints[mountpoints_used++];
		memcpy(&mountpoint->entry, &fstabent, sizeof(fstabent));
		mountpoint->entry_line = line;
		mountpoint->pid = -1;
		if ( !(mountpoint->absolute = strdup(mountpoint->entry.fs_file)) )
			fatal("malloc: %m");
		line = NULL;
		line_size = 0;
	}
	if ( ferror(fp) )
		fatal("/etc/fstab: %m");
	free(line);
	fclose(fp);
	qsort(mountpoints, mountpoints_used, sizeof(struct mountpoint),
	      sort_mountpoint);
}

static void set_hostname(void)
{
	FILE* fp = fopen("/etc/hostname", "r");
	if ( !fp && errno == ENOENT )
		return;
	if ( !fp )
		return warning("unable to set hostname: /etc/hostname: %m");
	char* hostname = read_single_line(fp);
	fclose(fp);
	if ( !hostname )
		return warning("unable to set hostname: /etc/hostname: %m");
	int ret = sethostname(hostname, strlen(hostname) + 1);
	if ( ret < 0 )
		warning("unable to set hostname: `%s': %m", hostname);
	free(hostname);
}

static void set_kblayout(void)
{
	int tty_fd = open("/dev/tty", O_RDWR);
	if ( !tty_fd )
		return warning("unable to set keyboard layout: /dev/tty: %m");
	bool unsupported = tcgetblob(tty_fd, "kblayout", NULL, 0) < 0 &&
	                   (errno == ENOTTY || errno == ENOENT);
	close(tty_fd);
	if ( unsupported )
		return;
	FILE* fp = fopen("/etc/kblayout", "r");
	if ( !fp && errno == ENOENT )
		return;
	if ( !fp )
		return warning("unable to set keyboard layout: /etc/kblayout: %m");
	char* kblayout = read_single_line(fp);
	fclose(fp);
	if ( !kblayout )
		return warning("unable to set keyboard layout: /etc/kblayout: %m");
	pid_t child_pid = fork();
	if ( child_pid < 0 )
	{
		free(kblayout);
		warning("unable to set keyboard layout: fork: %m");
		return;
	}
	if ( !child_pid )
	{
		uninstall_signal_handler();
		execlp("chkblayout", "chkblayout", "--", kblayout, (const char*) NULL);
		warning("setting keyboard layout: chkblayout: %m");
		_exit(127);
	}
	int status;
	waitpid(child_pid, &status, 0);
	free(kblayout);
}

static void set_videomode(void)
{
	int tty_fd = open("/dev/tty", O_RDWR);
	if ( !tty_fd )
		return warning("unable to set video mode: /dev/tty: %m");
	struct tiocgdisplay display;
	struct tiocgdisplays gdisplays;
	memset(&gdisplays, 0, sizeof(gdisplays));
	gdisplays.count = 1;
	gdisplays.displays = &display;
	bool unsupported = ioctl(tty_fd, TIOCGDISPLAYS, &gdisplays) < 0 ||
	                   gdisplays.count == 0;
	close(tty_fd);
	if ( unsupported )
		return;
	FILE* fp = fopen("/etc/videomode", "r");
	if ( !fp && errno == ENOENT )
		return;
	if ( !fp )
		return warning("unable to set video mode: /etc/videomode: %m");
	char* videomode = read_single_line(fp);
	fclose(fp);
	if ( !videomode )
		return warning("unable to set video mode: /etc/videomode: %m");
	unsigned int xres = 0;
	unsigned int yres = 0;
	unsigned int bpp = 0;
	if ( sscanf(videomode, "%ux%ux%u", &xres, &yres, &bpp) != 3 )
	{
		warning("/etc/videomode: Invalid video mode `%s'", videomode);
		free(videomode);
		return;
	}
	free(videomode);
	struct dispmsg_get_crtc_mode get_mode;
	memset(&get_mode, 0, sizeof(get_mode));
	get_mode.msgid = DISPMSG_GET_CRTC_MODE;
	get_mode.device = display.device;
	get_mode.connector = display.connector;
	// Don't set the resolution if it's already correct.
	if ( dispmsg_issue(&get_mode, sizeof(get_mode)) == 0 )
	{
		if ( get_mode.mode.control & DISPMSG_CONTROL_VALID &&
		     !(get_mode.mode.control & DISPMSG_CONTROL_FALLBACK) &&
		     get_mode.mode.fb_format == bpp &&
		     get_mode.mode.view_xres == xres &&
		     get_mode.mode.view_yres == yres )
			return;
	}
	struct dispmsg_set_crtc_mode set_mode;
	memset(&set_mode, 0, sizeof(set_mode));
	set_mode.msgid = DISPMSG_SET_CRTC_MODE;
	set_mode.device = 0;
	set_mode.connector = 0;
	set_mode.mode.driver_index = 0;
	set_mode.mode.magic = 0;
	set_mode.mode.control = DISPMSG_CONTROL_VALID;
	set_mode.mode.fb_format = bpp;
	set_mode.mode.view_xres = xres;
	set_mode.mode.view_yres = yres;
	set_mode.mode.fb_location = 0;
	set_mode.mode.pitch = xres * (bpp / 8);
	set_mode.mode.surf_off_x = 0;
	set_mode.mode.surf_off_y = 0;
	set_mode.mode.start_x = 0;
	set_mode.mode.start_y = 0;
	set_mode.mode.end_x = 0;
	set_mode.mode.end_y = 0;
	set_mode.mode.desktop_height = yres;
	if ( dispmsg_issue(&set_mode, sizeof(set_mode)) < 0 )
		warning("/etc/videomode: Failed to set video mode `%ux%ux%u': %m",
		        xres, yres, bpp);
}

static int no_dot_nor_dot_dot(const struct dirent* entry, void* ctx)
{
	(void) ctx;
	return !strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ? 0 : 1;
}

struct clean_tmp
{
	struct clean_tmp* parent;
	DIR* dir;
	char* path;
	struct dirent** entries;
	int num_entries;
	int current_entry;
};

static void clean_tmp(const char* tmp_path)
{
	struct clean_tmp* state = calloc(1, sizeof(struct clean_tmp));
	if ( !state )
	{
		warning("malloc: %m");
		return;
	}
	state->path = strdup(tmp_path);
	if ( !state->path )
	{
		warning("malloc: %m");
		free(state);
		return;
	}
	state->dir = opendir(state->path);
	if ( !state->dir )
	{
		warning("%s: %m", state->path);
		free(state->path);
		free(state);
		return;
	}
	while ( state )
	{
		if ( !state->entries )
		{
			state->num_entries = dscandir_r(state->dir, &state->entries,
			                                no_dot_nor_dot_dot, NULL,
			                                alphasort_r, NULL);
			if ( state->num_entries < 0 )
				warning("%s: %m", state->path);
		}
		if ( state->num_entries <= state->current_entry )
		{
			closedir(state->dir);
			for ( int i = 0; i < state->num_entries; i++ )
				free(state->entries[i]);
			free(state->entries);
			free(state->path);
			struct clean_tmp* new_state = state->parent;
			free(state);
			state = new_state;
			if ( state )
			{
				struct dirent* entry = state->entries[state->current_entry];
				const char* name = entry->d_name;
				int fd = dirfd(state->dir);
				if ( unlinkat(fd, name, AT_REMOVEDIR) < 0 )
					warning("%s/%s: %m", state->path, name);
				state->current_entry++;
			}
			continue;
		}
		struct dirent* entry = state->entries[state->current_entry];
		const char* name = entry->d_name;
		int fd = dirfd(state->dir);
		if ( unlinkat(fd, name, AT_REMOVEFILE | AT_REMOVEDIR) < 0 )
		{
			if ( errno == ENOTEMPTY )
			{
				struct clean_tmp* new_state =
					calloc(1, sizeof(struct clean_tmp));
				if ( !new_state )
				{
					warning("%s/%s: malloc: %m", state->path, name);
					state->current_entry++;
					continue;
				}
				new_state->path = join_paths(state->path, name);
				if ( !new_state->path )
				{
					warning("%s/%s: malloc: %m", state->path, name);
					free(new_state);
					state->current_entry++;
					continue;
				}
				int flags = O_DIRECTORY | O_RDONLY | O_NOFOLLOW;
				int newfd = openat(fd, new_state->path, flags);
				if ( newfd < 0 )
				{
					warning("%s: %m", new_state->path);
					free(new_state->path);
					free(new_state);
					state->current_entry++;
					continue;
				}
				new_state->dir = fdopendir(newfd);
				if ( !new_state->dir )
				{
					warning("%s: %m", new_state->path);
					close(newfd);
					free(new_state->path);
					free(new_state);
					state->current_entry++;
					continue;
				}
				new_state->parent = state;
				state = new_state;
				continue;
			}
			else
				warning("%s/%s: %m", state->path, name);
		}
		state->current_entry++;
	}
}

static bool is_chain_init_mountpoint(const struct mountpoint* mountpoint)
{
	return !strcmp(mountpoint->entry.fs_file, "/");
}

static struct filesystem* mountpoint_lookup(const struct mountpoint* mountpoint)
{
	const char* path = mountpoint->entry.fs_file;
	const char* spec = mountpoint->entry.fs_spec;
	if ( strncmp(spec, "UUID=", strlen("UUID=")) == 0 )
	{
		const char* uuid = spec + strlen("UUID=");
		if ( !uuid_validate(uuid) )
		{
			warning("%s: `%s' is not a valid uuid", path, uuid);
			return NULL;
		}
		struct device_match match;
		memset(&match, 0, sizeof(match));
		search_by_uuid(uuid, ensure_single_device_match, &match);
		if ( !match.path )
		{
			warning("%s: No devices matching uuid %s were found", path, uuid);
			return NULL;
		}
		if ( !match.bdev )
		{
			warning("%s: Don't know which particular device to boot with uuid "
			        "%s", path, uuid);
			return NULL;
		}
		assert(match.bdev->fs);
		return match.bdev->fs;
	}
	// TODO: Lookup by device name.
	// TODO: Use this function in the chain init case too.
	warning("%s: Don't know how to resolve `%s' to a filesystem", path, spec);
	return NULL;
}

static bool mountpoint_mount(struct mountpoint* mountpoint)
{
	struct filesystem* fs = mountpoint_lookup(mountpoint);
	if ( !fs )
		return false;
	// TODO: It would be ideal to get an exclusive lock so that no other
	//       processes have currently mounted that filesystem.
	struct blockdevice* bdev = fs->bdev;
	const char* bdev_path = bdev->p ? bdev->p->path : bdev->hd->path;
	assert(bdev_path);
	do if ( fs->flags & (FILESYSTEM_FLAG_FSCK_SHOULD | FILESYSTEM_FLAG_FSCK_MUST) )
	{
		assert(fs->fsck);
		if ( fs->flags & FILESYSTEM_FLAG_FSCK_MUST )
			note("%s: Repairing filesystem due to inconsistency...", bdev_path);
		else
			note("%s: Checking filesystem consistency...", bdev_path);
		pid_t child_pid = fork();
		if ( child_pid < 0 )
		{
			if ( fs->flags & FILESYSTEM_FLAG_FSCK_MUST )
			{
				warning("%s: Mandatory repair failed: fork: %m", bdev_path);
				// TODO: Try to mount as read-only if supported.
				return false;
			}
			warning("%s: Skipping filesystem check: fork: %m:", bdev_path);
			break;
		}
		if ( child_pid == 0 )
		{
			uninstall_signal_handler();
			execlp(fs->fsck, fs->fsck, "-fp", "--", bdev_path, (const char*) NULL);
			note("%s: Failed to load filesystem checker: %s: %m", bdev_path, fs->fsck);
			_exit(127);
		}
		int code;
		if ( waitpid(child_pid, &code, 0) < 0 )
			fatal("waitpid: %m");
		if ( WIFEXITED(code) &&
		     (WEXITSTATUS(code) == 0 || WEXITSTATUS(code) == 1) )
		{
			// Successfully checked filesystem.
		}
		else if ( fs->flags & FILESYSTEM_FLAG_FSCK_MUST )
		{
			if ( WIFSIGNALED(code) )
				warning("%s: Mandatory repair failed: %s: %s", bdev_path,
				        fs->fsck, strsignal(WTERMSIG(code)));
			else if ( !WIFEXITED(code) )
				warning("%s: Mandatory repair failed: %s: %s", bdev_path,
				        fs->fsck, "Unexpected unusual termination");
			else if ( WEXITSTATUS(code) == 127 )
				warning("%s: Mandatory repair failed: %s: %s", bdev_path,
				        fs->fsck, "Filesystem checker is absent");
			else if ( WEXITSTATUS(code) & 2 )
				warning("%s: Mandatory repair: %s: %s", bdev_path,
				        fs->fsck, "System reboot is necessary");
			else
				warning("%s: Mandatory repair failed: %s: %s", bdev_path,
				        fs->fsck, "Filesystem checker was unsuccessful");
			// TODO: Try to mount as read-only if supported.
			return false;
		}
		else
		{
			bool ignore = false;
			if ( WIFSIGNALED(code) )
				warning("%s: Filesystem check failed: %s: %s", bdev_path,
				        fs->fsck, strsignal(WTERMSIG(code)));
			else if ( !WIFEXITED(code) )
				warning("%s: Filesystem check failed: %s: %s", bdev_path,
				        fs->fsck, "Unexpected unusual termination");
			else if ( WEXITSTATUS(code) == 127 )
			{
				warning("%s: Skipping filesystem check: %s: %s", bdev_path,
				        fs->fsck, "Filesystem checker is absent");
				ignore = true;
			}
			else if ( WEXITSTATUS(code) & 2 )
				warning("%s: Filesystem check: %s: %s", bdev_path,
				        fs->fsck, "System reboot is necessary");
			else
				warning("%s: Filesystem check failed: %s: %s", bdev_path,
				        fs->fsck, "Filesystem checker was unsuccessful");
			if ( !ignore )
				return false;
		}
	} while ( 0 );
	if ( !fs->driver )
	{
		warning("%s: Don't know how to mount a %s filesystem",
		        bdev_path, fs->fstype_name);
		return false;
	}
	const char* pretend_where = mountpoint->entry.fs_file;
	const char* where = mountpoint->absolute;
	struct stat st;
	if ( stat(where, &st) < 0 )
	{
		warning("stat: %s: %m", where);
		return false;
	}
	if ( (mountpoint->pid = fork()) < 0 )
	{
		warning("%s: Unable to mount: fork: %m", bdev_path);
		return false;
	}
	// TODO: This design is broken. The filesystem should tell us when it is
	//       ready instead of having to poll like this.
	if ( mountpoint->pid == 0 )
	{
		uninstall_signal_handler();
		execlp(fs->driver, fs->driver, "--foreground", bdev_path, where,
		       "--pretend-mount-path", pretend_where, (const char*) NULL);
		warning("%s: Failed to load filesystem driver: %s: %m", bdev_path, fs->driver);
		_exit(127);
	}
	// TODO: Use daemon readiness notification method (READYFD).
	while ( true )
	{
		struct stat newst;
		if ( stat(where, &newst) < 0 )
		{
			warning("stat: %s: %m", where);
			if ( unmount(where, 0) < 0 && errno != ENOMOUNT )
				warning("unmount: %s: %m", where);
			else if ( errno == ENOMOUNT )
				kill(mountpoint->pid, SIGQUIT);
			int code;
			waitpid(mountpoint->pid, &code, 0);
			mountpoint->pid = -1;
			return false;
		}
		if ( newst.st_dev != st.st_dev || newst.st_ino != st.st_ino )
			break;
		int code;
		pid_t child = waitpid(mountpoint->pid, &code, WNOHANG);
		if ( child < 0 )
			fatal("waitpid: %m");
		if ( child != 0 )
		{
			mountpoint->pid = -1;
			if ( WIFSIGNALED(code) )
				warning("%s: Mount failed: %s: %s", bdev_path, fs->driver,
				        strsignal(WTERMSIG(code)));
			else if ( !WIFEXITED(code) )
				warning("%s: Mount failed: %s: %s", bdev_path, fs->driver,
				        "Unexpected unusual termination");
			else if ( WEXITSTATUS(code) == 127 )
				warning("%s: Mount failed: %s: %s", bdev_path, fs->driver,
				        "Filesystem driver is absent");
			else if ( WEXITSTATUS(code) == 0 )
				warning("%s: Mount failed: %s: Unexpected successful exit",
				        bdev_path, fs->driver);
			else
				warning("%s: Mount failed: %s: Exited with status %i", bdev_path,
				        fs->driver, WEXITSTATUS(code));
			return false;
		}
		struct timespec delay = timespec_make(0, 50L * 1000L * 1000L);
		nanosleep(&delay, NULL);
	}
	return true;
}

static void mountpoints_mount(bool is_chain_init)
{
	for ( size_t i = 0; i < mountpoints_used; i++ )
	{
		struct mountpoint* mountpoint = &mountpoints[i];
		if ( is_chain_init_mountpoint(mountpoint) != is_chain_init )
			continue;
		mountpoint_mount(mountpoint);
	}
}

static void mountpoints_unmount(void)
{
	for ( size_t n = mountpoints_used; n != 0; n-- )
	{
		size_t i = n - 1;
		struct mountpoint* mountpoint = &mountpoints[i];
		if ( mountpoint->pid < 0 )
			continue;
		if ( unmount(mountpoint->absolute, 0) < 0 && errno != ENOMOUNT )
			warning("unmount: %s: %m", mountpoint->entry.fs_file);
		else if ( errno == ENOMOUNT )
			kill(mountpoint->pid, SIGTERM);
		int code;
		if ( waitpid(mountpoint->pid, &code, 0) < 0 )
			note("waitpid: %m");
		mountpoint->pid = -1;
	}
}

// This function must be usable as an atexit handler, which means it is
// undefined behavior for it to invoke exit(), including through calls to fatal
// in any function transitively called by this function.
static void niht(void)
{
	if ( getpid() != main_pid )
		return;

	// TODO: Unify with new daemon system? At least it needs to recursively kill
	//       all processes. Ideally fatal wouldn't be called for daemons.

	// TODO: Don't do this unless all the mountpoints were mounted (not for
	//       chain init).
	write_random_seed();

	// Stop logging when unmounting the filesystems.
	cbprintf(&init_log, log_callback, "Finished operating system.\n");
	log_close(&init_log);

	if ( chain_location_dev_made )
	{
		unmount(chain_location_dev, 0);
		chain_location_dev_made = false;
	}

	mountpoints_unmount();

	if ( chain_location_made )
	{
		rmdir(chain_location);
		chain_location_made = false;
	}
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

int main(int argc, char* argv[])
{
	main_pid = getpid();

	setlocale(LC_ALL, "");

	const char* target_name = "default";

	for ( int i = 1; i < argc; i++ )
	{
		const char* arg = argv[i];
		if ( arg[0] != '-' || !arg[1] )
			continue;
		argv[i] = NULL;
		if ( !strcmp(arg, "--") )
			break;
		if ( arg[1] != '-' )
		{
			char c;
			while ( (c = *++arg) ) switch ( c )
			{
			case 'q': verbosity = VERBOSITY_QUIET; break;
			case 's': verbosity = VERBOSITY_SILENT; break;
			case 'v': verbosity = VERBOSITY_VERBOSE; break;
			default:
				errx(2, "unknown option -- '%c'", c);
			}
		}
		else if ( !strcmp(arg, "--quiet") )
			verbosity = VERBOSITY_QUIET;
		else if ( !strcmp(arg, "--silent") )
			verbosity = VERBOSITY_SILENT;
		else if ( !strcmp(arg, "--verbose") )
			verbosity = VERBOSITY_VERBOSE;
		else if ( !strncmp(arg, "--target=", strlen("--target=")) )
			target_name = arg + strlen("--target=");
		else if ( !strcmp(arg, "--target") )
		{
			if ( i + 1 == argc )
				errx(2, "option '--target' requires an argument");
			target_name = argv[i+1];
			argv[++i] = NULL;
		}
		else
			errx(2, "unknown option: %s", arg);
	}

	compact_arguments(&argc, &argv);

	// Prevent recursive init without care.
	if ( getenv("INIT_PID") )
		fatal("System is already managed by an init process");

	// Register handler that shuts down the system when init exits.
	if ( atexit(niht) != 0 )
		fatal("atexit: %m");

	// Handle signals but block them until the safe points where we handle them.
	// All child processes have to uninstall the signal handler and unblock the
	// signals or they keep blocking the signals.
	install_signal_handler();

	// The default daemon brings up the operating system.
	// TODO: After releasing Sortix 1.1, remove this compatibility since a
	// sysmerge from 1.0 will not have a /etc/init directory.
	struct daemon_config* default_daemon_config =
		!strcmp(target_name, "merge") ? NULL : daemon_config_load("default");

	// Daemons inherit their default settings from the default daemon. Load its
	// configuration (if it exists) even if another default target has been set.
	if ( default_daemon_config )
	{
		default_config.log_method = default_daemon_config->log_method;
		default_config.log_format = default_daemon_config->log_format;
		default_config.log_control_messages =
			default_daemon_config->log_control_messages;
		default_config.log_rotate_on_start =
			default_daemon_config->log_rotate_on_start;
		default_config.log_rotations = default_daemon_config->log_rotations;
		default_config.log_line_size = default_daemon_config->log_line_size;
		default_config.log_size = default_daemon_config->log_size;
	}

	// If another daemon has been specified as the boot target, create a fake
	// default daemon that depends on the specified boot target daemon.
	if ( strcmp(target_name, "default") != 0 )
	{
		if ( default_daemon_config )
			daemon_config_free(default_daemon_config);
		default_daemon_config = malloc(sizeof(struct daemon_config));
		if ( !default_daemon_config )
			fatal("malloc: %m");
		daemon_config_initialize(default_daemon_config);
		if ( !(default_daemon_config->name = strdup("default")) )
			fatal("malloc: %m");
		struct dependency_config* dependency_config =
			calloc(1, sizeof(struct dependency_config));
		if ( !dependency_config )
			fatal("malloc: %m");
		if ( !(dependency_config->target = strdup(target_name)) )
			fatal("malloc: %m");
		dependency_config->flags = DEPENDENCY_FLAG_REQUIRE |
		                           DEPENDENCY_FLAG_AWAIT |
		                           DEPENDENCY_FLAG_EXIT_CODE;
		if ( !array_add((void***) &default_daemon_config->dependencies,
		                &default_daemon_config->dependencies_used,
		                &default_daemon_config->dependencies_length,
		                dependency_config) )
			fatal("malloc: %m");
	}
	else if ( !default_daemon_config )
		fatal("Failed to load /etc/init/default: %m");

	// Instantiate the default daemon from its configuration.
	struct daemon* default_daemon = daemon_create(default_daemon_config);
	daemon_config_free(default_daemon_config);

	// The default daemon should depend on exactly one top level daemon.
	const char* first_requirement =
		1 <= default_daemon->dependencies_used ?
		default_daemon->dependencies[0]->target->name :
		"";

	// Log to memory until the log directory has been mounted.
	if ( !log_initialize(&init_log, "init", &default_config) )
		fatal("malloc: %m");
	if ( !log_begin_buffer(&init_log) )
		fatal("malloc: %m");
	init_log.pid = getpid();
	cbprintf(&init_log, log_callback, "Initializing operating system...\n");

	// Make sure that we have a /tmp directory.
	umask(0000);
	mkdir("/tmp", 01777);
	clean_tmp("/tmp");

	// Make sure that we have a /var/run directory.
	umask(0000);
	mkdir("/var", 0755);
	mkdir("/var/run", 0755);
	clean_tmp("/var/run");

	// Set the default file creation mask.
	umask(0022);

	// Set up the PATH variable.
	if ( setenv("PATH", "/bin:/sbin", 1) < 0 )
		fatal("setenv: %m");

	// Load partition tables and create all the block devices.
	prepare_block_devices();

	// Load the filesystem table.
	load_fstab();

	// If the default daemon's top level dependency is a chain boot target, then
	// chain boot the actual root filesystem.
	// TODO: Document this special kind of daemon.
	if ( !strcmp(first_requirement, "chain") ||
	     !strcmp(first_requirement, "chain-merge") )
	{
		int next_argc = argc - 1;
		char** next_argv = argv + 1;
		// Create a temporary directory where the real root filesystem will be
		// mounted.
		if ( !mkdtemp(chain_location) )
			fatal("mkdtemp: /tmp/fs.XXXXXX: %m");
		chain_location_made = true;
		// Rewrite the filesystem table to mount inside the temporary directory.
		bool found_root = false;
		for ( size_t i = 0; i < mountpoints_used; i++ )
		{
			struct mountpoint* mountpoint = &mountpoints[i];
			if ( !strcmp(mountpoint->entry.fs_file, "/") )
				found_root = true;
			char* absolute = join_paths(chain_location, mountpoint->absolute);
			free(mountpoint->absolute);
			mountpoint->absolute = absolute;
		}
		if ( !found_root )
			fatal("/etc/fstab: Root filesystem not found in filesystem table");
		// Mount the filesystem table entries marked for chain boot.
		mountpoints_mount(true);
		// Additionally bind the /dev filesystem inside the root filesystem.
		snprintf(chain_location_dev, sizeof(chain_location_dev), "%s/dev",
			     chain_location);
		if ( mkdir(chain_location_dev, 0755) < 0 &&
		     errno != EEXIST && errno != EROFS )
			fatal("mkdir: %s: %m", chain_location_dev);
		int old_dev_fd = open("/dev", O_DIRECTORY | O_RDONLY);
		if ( old_dev_fd < 0 )
			fatal("%s: %m", "/dev");
		int new_dev_fd = open(chain_location_dev, O_DIRECTORY | O_RDONLY);
		if ( new_dev_fd < 0 )
			fatal("%s: %m", chain_location_dev);
		if ( fsm_fsbind(old_dev_fd, new_dev_fd, 0) < 0 )
			fatal("mount: `%s' onto `%s': %m", "/dev", chain_location_dev);
		close(new_dev_fd);
		close(old_dev_fd);
		// TODO: Forward the early init log to the chain init.
		// Run the chain booted operating system.
		pid_t child_pid = fork();
		if ( child_pid < 0 )
			fatal("fork: %m");
		if ( !child_pid )
		{
			uninstall_signal_handler();
			if ( chroot(chain_location) < 0 )
				fatal("chroot: %s: %m", chain_location);
			if ( chdir("/") < 0 )
				fatal("chdir: %s: %m", chain_location);
			unsetenv("INIT_PID");
			const char* program = next_argv[0];
			char verbose_opt[] = {'-', "sqv"[verbosity], '\0'};
			// Chain boot the operating system upgrade if needed.
			if ( !strcmp(first_requirement, "chain-merge") )
			{
				program = "/sysmerge/sbin/init";
				// TODO: Concat next_argv onto this argv_next, so the arguments
				//       can be passed to the final init.
				next_argv =
					(char*[]) { (char*) program, "--target=merge",
					            verbose_opt, NULL };
			}
			else if ( next_argc < 1 )
			{
				program = "/sbin/init";
				next_argv = (char*[]) { "init", verbose_opt, NULL };
			}
			execvp(program, (char* const*) next_argv);
			fatal("Failed to chain load init: %s: %m", next_argv[0]);
		}
		forward_signal_pid = child_pid;
		sigprocmask(SIG_UNBLOCK, &handled_signals, NULL);
		int status;
		while ( waitpid(child_pid, &status, 0) < 0 )
		{
			if ( errno != EINTR )
				fatal("waitpid: %m");
		}
		sigprocmask(SIG_BLOCK, &handled_signals, NULL);
		forward_signal_pid = -1; // Racy with waitpid.
		if ( WIFEXITED(status) )
			return WEXITSTATUS(status);
		else if ( WIFSIGNALED(status) )
			fatal("Chain booted init failed with signal: %s",
			      strsignal(WTERMSIG(status)));
		else
			fatal("Chain booted init failed unusually");
	}

	// Mount the filesystems, except for the filesystems that would have been
	// mounted by the chain init.
	mountpoints_mount(false);

	// TODO: After releasing Sortix 1.1, remove this compatibility since a
	// sysmerge from 1.0 will not have a /var/log directory.
	if ( !strcmp(first_requirement, "merge") &&
	     access("/var/log", F_OK) < 0 )
		mkdir("/var/log", 0755);

	// Logging works now that the filesystems have been mounted. Reopen the init
	// log and write the contents buffered up in memory.
	log_begin(&init_log);

	// Update the random seed in case the system fails before it can be written
	// out during the system shutdown.
	write_random_seed();

	set_hostname();
	set_kblayout();
	set_videomode();

	// Run the operating system upgrade if requested.
	// TODO: Document this special kind of daemon.
	if ( !strcmp(first_requirement, "merge") )
	{
		pid_t child_pid = fork();
		if ( child_pid < 0 )
			fatal("fork: %m");
		if ( !child_pid )
		{
			uninstall_signal_handler();
			const char* argv[] = { "sysmerge", "--booting", NULL };
			execv("/sysmerge/sbin/sysmerge", (char* const*) argv);
			fatal("Failed to load system upgrade: %s: %m", argv[0]);
		}
		forward_signal_pid = child_pid;
		sigprocmask(SIG_UNBLOCK, &handled_signals, NULL);
		int status;
		while ( waitpid(child_pid, &status, 0) < 0 )
		{
			if ( errno != EINTR )
				fatal("waitpid: %m");
		}
		sigprocmask(SIG_BLOCK, &handled_signals, NULL);
		forward_signal_pid = -1; // Racy with waitpid.
		if ( WIFEXITED(status) && WEXITSTATUS(status) != 0 )
			fatal("Automatic upgrade failed: Exit status %i",
			      WEXITSTATUS(status));
		else if ( WIFSIGNALED(status) )
			fatal("Automatic upgrade failed: %s", strsignal(WTERMSIG(status)));
		else if ( !WIFEXITED(status) )
			fatal("Automatic upgrade failed: Unexpected unusual termination");
		// Soft reinit into the freshly upgraded operating system.
		niht();
		unsetenv("INIT_PID");
		// TODO: Use next_argv here.
		const char* argv[] = { "init", NULL };
		execv("/sbin/init", (char* const*) argv);
		fatal("Failed to load init during reinit: %s: %m", argv[0]);
	}

	// TODO: Use the arguments to specify additional things the default daemon
	//       should depend on, as well as a denylist of things not to start
	//       even if in default's dependencies. The easiest thing is probably to
	//       be able to inject require and unset require lines into default.

	// Request the default daemon be run.
	schedule_daemon(default_daemon);

	// Run the operating system.
	schedule();

	// Exit with the exit code of the default daemon.
	return exit_code_to_exit_status(default_daemon->exit_code);
}
