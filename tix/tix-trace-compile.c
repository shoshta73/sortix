/*
 * Copyright (c) 2025 Jonas 'Sortie' Termansen.
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
 * tix-trace-compile.c
 * Trace a compilation command and enforce safe cross-compilation.
 */

#include <sys/stat.h>
#include <sys/wait.h>

#include <err.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdnoreturn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char* invocation;
static const char* sysroot;
static const char* source_dir;
static const char* build_dir;
static const char* mode;
static const char* tool;
static bool compile_only;
static bool no_cross_exec;
static int tty_fd = -1;

static bool ends_with(const char* str, const char* end)
{
	size_t len = strlen(str);
	size_t endlen = strlen(end);
	return endlen <= len && !strcmp(str + len - endlen, end);
}

// TODO: Be stricter. Ensure slashes and realpath too.
static bool is_in_directory(const char* path, const char* directory)
{
	return !strncmp(path, directory, strlen(directory));
}

static noreturn void reject(const char* path, const char* message)
{
	// TODO: Warn-only mode initially.
	warn("error: %s: %s: %s", path, message, invocation);
	dprintf(tty_fd, "\e[1;91merror: %s: %s: %s\e[m\n", path, message, invocation);
	exit(1);
}

static void enforce_input_path(const char* path)
{
	if ( !strcmp(mode, "build") )
	{
		if ( sysroot && is_in_directory(path, sysroot) )
			reject(path, "Native build but input path was inside sysroot");
	}
	else if ( !strcmp(mode, "host") )
	{
		if ( !((sysroot && is_in_directory(path, sysroot)) ||
		       (source_dir && is_in_directory(path, source_dir)) ||
		       (build_dir && is_in_directory(path, build_dir)) ||
		       path[0] != '/') )
			reject(path, "Cross-build but input was not inside "
			             "source directory, build directory, or sysroot");
	}

	if ( sysroot && is_in_directory(path, sysroot) )
		return;
	if ( is_in_directory(path, sysroot) )
		return;
	if ( (source_dir && is_in_directory(path, source_dir)) ||
	     (build_dir && is_in_directory(path, build_dir)) ||
	     path[0] != '/' )
	{
		if ( ends_with(path, ".o") ||
			 ends_with(path, ".a") ||
			 ends_with(path, ".so") )
		{
			char* marker;
			if ( asprintf(&marker, "%s.%s", path, mode) < 0 )
				err(1, "malloc");
			if ( access(marker, F_OK) < 0 )
				reject(path, "Non-source-code input has not been traced for "
				       "this compilation mode");
			dprintf(tty_fd, "allowing %s\n", marker);
			free(marker);
		}
		else
		{
			// TODO: Enforce the path is source code.
		}
	}
}

static void enforce_output_path(const char* path)
{
	if ( path[0] != '/' )
		;
	else if ( build_dir && !is_in_directory(path, build_dir) )
		reject(path, "Output path was not inside build directory");
	else if ( source_dir && !is_in_directory(path, source_dir) )
		reject(path, "Output path was not inside source directory");
}

int main(int argc, char* argv[])
{
	if ( argc <= 1 )
		errx(1, "expected program and options");
	enum
	{
		OPT_SYSROOT = 256,
		OPT_SOURCE_DIR = 257,
		OPT_BUILD_DIR = 258,
		OPT_BUILD = 259,
		OPT_HOST = 260,
		OPT_TARGET = 261,
		OPT_TOOL = 262,
		OPT_NO_CROSS_EXEC = 263,
	};
	const struct option longopts[] =
	{
		{"sysroot", required_argument, NULL, OPT_SYSROOT},
		{"source-dir", required_argument, NULL, OPT_SOURCE_DIR},
		{"build-dir", required_argument, NULL, OPT_BUILD_DIR},
		{"build", no_argument, NULL, OPT_BUILD},
		{"host", no_argument, NULL, OPT_HOST},
		{"target", no_argument, NULL, OPT_TARGET},
		{"tool", required_argument, NULL, OPT_TOOL},
		{"no-cross-exec", no_argument, NULL, OPT_NO_CROSS_EXEC},
		{0, 0, 0, 0}
	};
	const char* opts = "";
	int opt;
	while ( (opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1 )
	{
		switch ( opt )
		{
		case OPT_SYSROOT: sysroot = optarg; break;
		case OPT_SOURCE_DIR: source_dir = optarg; break;
		case OPT_BUILD_DIR: build_dir = optarg; break;
		case OPT_BUILD: mode = "build"; break;
		case OPT_HOST: mode = "host"; break;
		case OPT_TARGET: mode = "target"; break;
		case OPT_TOOL: tool = optarg; break;
		case OPT_NO_CROSS_EXEC: no_cross_exec = true; break;
		default: return 1;
		}
	}
	// TODO: Enforce pkg-config(1) -> cc option flow.
	if ( !tool )
		errx(1, "--tool must be set");
	if ( strcmp(tool, "compile") != 0 &&
	     strcmp(tool, "archive") != 0 &&
	     strcmp(tool, "assemble") != 0 )
		errx(1, "unsupported tool: %s", tool);
	if ( !mode )
		errx(1, "--build or --host or --target must be set");
	if ( !strcmp(mode, "host") && !sysroot )
		errx(1, "cross-compiling without --sysroot set");
	if ( !(invocation = strdup(argv[0])) )
		err(1, "malloc");
	if ( !isatty(2) )
		tty_fd = open("/dev/tty", O_WRONLY | O_CLOEXEC);
	else
		tty_fd = 2;
	for ( int i = 1; i < argc; i++ )
	{
		// TODO: Quote.
		char* new_invocation;
		if ( asprintf(&new_invocation, "%s %s", invocation, argv[i]) < 0 )
			err(1, "malloc");
		free(invocation);
		invocation = new_invocation;
	}
	// TODO: Allow TMPDIR too.
	// TODO: INCLUDE_PATH, LIBRARY_PATH
	// TODO: Possibly a shadow directory for markers and trace logs and state.
	const char* program = argv[optind];
	const char* source_file = NULL;
	const char* output = NULL;
	bool ended = false;
	int ar_state = 0;
	for ( int i = optind + 1; i < argc; i++ )
	{
		// TODO: Strict argument parsing and required arguments.
		if ( ended || argv[i][0] != '-' )
		{
			if ( !strcmp(tool, "archive") )
			{
				int state = ar_state++;
				if ( state < 2 )
				{
					if ( state == 1 )
						output = argv[i];
					continue;
				}
			}
			enforce_input_path(argv[i]);
			// TODO: What happens if cc -c foo.c bar.c?
			source_file = argv[i];
		}
		else if ( !strcmp(argv[i], "--") )
			ended = true;
		else if ( (!strcmp(tool, "compile") || !strcmp(tool, "assemble")) &&
		          !strncmp(argv[i], "-o", 2) )
		{
			const char* arg;
			if ( !strcmp(argv[i], "-o") )
				arg = argv[++i];
			else
				arg = argv[i] + 2;
			output = arg;
		}
		else if ( !strcmp(tool, "compile") && !strncmp(argv[i], "-I", 2) )
		{
			const char* arg;
			if ( !strcmp(argv[i], "-o") )
				arg = argv[++i];
			else
				arg = argv[i] + 2;
			enforce_input_path(arg);
		}
		else if ( !strcmp(tool, "compile") && !strncmp(argv[i], "-L", 2) )
		{
			const char* arg;
			if ( !strcmp(argv[i], "-L") )
				arg = argv[++i];
			else
				arg = argv[i] + 2;
			enforce_input_path(arg);
		}
		else if ( !strcmp(tool, "compile") && !strcmp(argv[i], "-c") )
			compile_only = true;
		// TODO: Verify --sysroot option.
		else
		{
			// Ignore unknown options.
		}
		// TODO: For each input object, enforce it has a marker for the same
		//       compilation mode.
	}
	if ( !output && !strcmp(tool, "compile") )
	{
		if ( compile_only )
		{
			if ( !source_file )
				err(1, "-c but no source file specified");
			char* copy = strdup(source_file);
			if ( !copy )
				err(1, "malloc");
			char* last_dot = strrchr(copy, '.');
			if ( !last_dot )
				err(1, "no file extension on %s", source_file);
			*last_dot = '\0';
			char* new_output;
			if ( asprintf(&new_output, "%s.o", copy) < 0 )
				err(1, "malloc");
			free(copy);
			// TODO: new_output leaks
			output = new_output;
		}
		else
			output = "a.out";
	}
	if ( !output )
		err(1, "no output file was specified");
	enforce_output_path(output);
	// TODO: Method for cleaning up markers, and alternate methods for markers.
	// TODO: Hey, we can use the suid/guid file mode bits to mark cross-compiled
	//       programs so they cannot be executed (with a bit of possible kernel
	//       help). On Linux, we can use extended attributes.
	char* marker;
	if ( asprintf(&marker, "%s.%s", output, mode) < 0 )
		err(1, "mode");
	int marker_fd = creat(marker, 0666);
	if ( marker_fd < 0 )
		err(1, "%s", marker);
	close(marker_fd);
	if ( !strcmp(mode, "build") )
		dprintf(tty_fd, "\e[33mmade %s: %s\e[m\n", marker, invocation);
	else
		dprintf(tty_fd, "\e[32mmade %s: %s\e[m\n", marker, invocation);
	free(marker);
	bool post_step = false;
	if ( !strcmp(mode, "host") && no_cross_exec )
	{
		// A mechanism for restoring the executable bit? But for most ports
		// using install(1) that will be automatic. We may need to trace cp.
		mode_t mask = umask(0);
		umask(mask | 0111);
	}
	pid_t child = post_step ? fork() : -1;
	if ( child < 0 )
	{
		execvp(program, argv + optind);
		err(127, "%s", program);
	}
	int status;
	waitpid(child, &status, 0);
	if ( WIFEXITED(status) )
		return WEXITSTATUS(status);
	else if ( WTERMSIG(status) )
		return 128 + WTERMSIG(status);
	else
		return 1;
}
