/*
 * Copyright (c) 2013-2016, 2020, 2022-2024 Jonas 'Sortie' Termansen.
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
 * tix-build.c
 * Compile a source tix into a tix suitable for installation.
 */

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

struct buildvar
{
	const char* variable;
	const char* value;
	bool program;
};

static struct buildvar buildvars[] =
{
	{ "AR", "ar", true },
	{ "AS", "as", true },
	{ "CC", "gcc", true },
	{ "CFLAGS", "-Os", false },
	{ "CPP", "gcc -E", true },
	{ "CPP", "", false },
	{ "CXXFILT", "c++filt", true },
	{ "CXX", "g++", true },
	{ "CXXFLAGS", "-Os", false },
	{ "LD", "ld", true },
	{ "LDFLAGS", "", false },
	{ "NM", "nm", true },
	{ "OBJCOPY", "objcopy", true },
	{ "OBJDUMP", "objdump", true },
	{ "PKG_CONFIG", "pkg-config", true },
	{ "RANLIB", "ranlib", true },
	{ "READELF", "readelf", true },
	{ "STRIP", "strip", true },
};

#define BUILDVARS_LENGTH (sizeof(buildvars) / sizeof(buildvars[0]))

enum build_step
{
	BUILD_STEP_NO_SUCH_STEP,
	BUILD_STEP_START,
	BUILD_STEP_PRE_CLEAN,
	BUILD_STEP_CONFIGURE,
	BUILD_STEP_BUILD,
	BUILD_STEP_INSTALL,
	BUILD_STEP_POST_INSTALL,
	BUILD_STEP_POST_CLEAN,
	BUILD_STEP_PACKAGE,
	BUILD_STEP_END,
};

static bool should_do_build_step(enum build_step step,
                                 enum build_step start,
                                 enum build_step end)
{
	return start <= step && step <= end;
}

#define SHOULD_DO_BUILD_STEP(step, minfo) \
        should_do_build_step((step), (minfo)->start_step, (minfo)->end_step)

static enum build_step step_of_step_name(const char* step_name)
{
	if ( !strcmp(step_name, "start") )
		return BUILD_STEP_START;
	if ( !strcmp(step_name, "clean") )
		return BUILD_STEP_PRE_CLEAN;
	if ( !strcmp(step_name, "pre-clean") )
		return BUILD_STEP_PRE_CLEAN;
	if ( !strcmp(step_name, "configure") )
		return BUILD_STEP_CONFIGURE;
	if ( !strcmp(step_name, "build") )
		return BUILD_STEP_BUILD;
	if ( !strcmp(step_name, "install") )
		return BUILD_STEP_INSTALL;
	if ( !strcmp(step_name, "post-install") )
		return BUILD_STEP_POST_INSTALL;
	if ( !strcmp(step_name, "post-clean") )
		return BUILD_STEP_POST_CLEAN;
	if ( !strcmp(step_name, "package") )
		return BUILD_STEP_PACKAGE;
	if ( !strcmp(step_name, "end") )
		return BUILD_STEP_END;
	return BUILD_STEP_NO_SUCH_STEP;
}

struct metainfo
{
	char* build;
	char* build_dir;
	char* destination;
	int generation;
	char* host;
	char* make;
	char* makeflags;
	char* package_dir;
	char* package_info_path;
	char* package_name;
	char* prefix;
	char* exec_prefix;
	char* subdir;
	char* sysroot;
	char* tar;
	char* target;
	char* tmp;
	string_array_t package_info;
	enum build_step start_step;
	enum build_step end_step;
	bool bootstrapping;
	bool cross;
	// TODO: After releasing Sortix 1.1, remove tixbuildinfo support.
	bool tixbuildinfo;
};

static const char* metainfo_get_def(const struct metainfo* minfo,
                                    const char* key,
                                    const char* old_key,
                                    const char* def)
{
	return dictionary_get_def((string_array_t*) &minfo->package_info,
	                          !minfo->tixbuildinfo ? key : old_key, def);
}

static const char* metainfo_get(const struct metainfo* minfo,
                                const char* key,
                                const char* old_key)
{
	return metainfo_get_def(minfo, key, old_key, NULL);
}

static const char* metainfo_verify(struct metainfo* minfo,
                                   const char* key,
                                   const char* old_key)
{
	const char* ret = metainfo_get(minfo, key, old_key);
	if ( !ret )
		errx(1, "error: `%s': no `%s' variable declared",
		     minfo->package_info_path, !minfo->tixbuildinfo ? key : old_key);
	return ret;
}

static bool has_in_path(const char* program)
{
	pid_t child_pid = fork();
	if ( child_pid < 0 )
		err(1, "fork: which %s", program);
	if ( child_pid )
	{
		int exitstatus;
		waitpid(child_pid, &exitstatus, 0);
		return WIFEXITED(exitstatus) && WEXITSTATUS(exitstatus) == 0;
	}
	close(0);
	close(1);
	close(2);
	if ( open("/dev/null", O_RDONLY) != 0 ||
	     open("/dev/null", O_WRONLY) != 1 ||
	     open("/dev/null", O_WRONLY) != 2 )
	{
		warn("/dev/null");
		_exit(1);
	}
	const char* argv[] = { "which", program, NULL };
	execvp(argv[0], (char**) argv);
	_exit(1);
}

static void emit_compiler_wrapper_invocation(FILE* wrapper,
                                             struct metainfo* minfo,
                                             const char* name)
{
	fprintf(wrapper, "%s", name);
	if ( minfo->sysroot )
		fprintf(wrapper, " --sysroot=\"$SYSROOT\"");
	fprintf(wrapper, " \"$@\"");
}

static void emit_compiler_sysroot_wrapper(struct metainfo* minfo,
                                          const char* bindir,
                                          const char* name)
{
	if ( !has_in_path(name) )
		return;
	char* wrapper_path = join_paths(bindir, name);
	if ( !wrapper_path )
		err(1, "malloc");
	FILE* wrapper = fopen(wrapper_path, "w");
	if ( !wrapper )
		err(1, "`%s'", wrapper_path);
	fprintf(wrapper, "#!/bin/sh\n");
	fprint_shell_variable_assignment(wrapper, "PATH", getenv("PATH"));
	if ( minfo->sysroot )
		fprint_shell_variable_assignment(wrapper, "SYSROOT", minfo->sysroot);
	fprintf(wrapper, "exec ");
	emit_compiler_wrapper_invocation(wrapper, minfo, name);
	fprintf(wrapper, "\n");
	fflush(wrapper);
	fchmod_plus_x(fileno(wrapper));
	if ( ferror(wrapper) || fflush(wrapper) == EOF )
		err(1, "%s", wrapper_path);
	fclose(wrapper);
	free(wrapper_path);
}

static void emit_compiler_sysroot_cross_wrapper(struct metainfo* minfo,
                                                const char* bindir,
                                                const char* name)
{
	char* cross_name = print_string("%s-%s", minfo->host, name);
	if ( !cross_name )
		err(1, "malloc");
	emit_compiler_sysroot_wrapper(minfo, bindir, cross_name);
	free(cross_name);
}

static void emit_pkg_config_wrapper(struct metainfo* minfo, const char* bindir)
{
	// Create a pkg-config script for the build system.
	char* pkg_config_for_build_path = join_paths(bindir, "pkg-config");
	if ( !pkg_config_for_build_path )
		err(1, "malloc");
	FILE* pkg_config_for_build = fopen(pkg_config_for_build_path, "w");
	if ( !pkg_config_for_build )
		err(1, "`%s'", pkg_config_for_build_path);
	fprintf(pkg_config_for_build, "#!/bin/sh\n");
	fprint_shell_variable_assignment(pkg_config_for_build,
		"PATH", getenv("PATH"));
	fprint_shell_variable_assignment(pkg_config_for_build,
		"PKG_CONFIG", getenv("PKG_CONFIG"));
	fprint_shell_variable_assignment(pkg_config_for_build,
		"PKG_CONFIG_FOR_BUILD", getenv("PKG_CONFIG_FOR_BUILD"));
	if ( getenv("PKG_CONFIG_PATH_FOR_BUILD") )
		fprint_shell_variable_assignment(pkg_config_for_build,
			"PKG_CONFIG_PATH", getenv("PKG_CONFIG_PATH_FOR_BUILD"));
	else
		fprint_shell_variable_assignment(pkg_config_for_build,
			"PKG_CONFIG_PATH", getenv("PKG_CONFIG_PATH"));
	if ( getenv("PKG_CONFIG_SYSROOT_DIR_FOR_BUILD") )
		fprint_shell_variable_assignment(pkg_config_for_build,
			"PKG_CONFIG_SYSROOT_DIR",
			getenv("PKG_CONFIG_SYSROOT_DIR_FOR_BUILD"));
	else
		fprint_shell_variable_assignment(pkg_config_for_build,
			"PKG_CONFIG_SYSROOT_DIR", getenv("PKG_CONFIG_SYSROOT_DIR"));
	if ( getenv("PKG_CONFIG_LIBDIR_FOR_BUILD") )
		fprint_shell_variable_assignment(pkg_config_for_build,
			"PKG_CONFIG_LIBDIR", getenv("PKG_CONFIG_LIBDIR_FOR_BUILD"));
	else
		fprint_shell_variable_assignment(pkg_config_for_build,
			"PKG_CONFIG_LIBDIR", getenv("PKG_CONFIG_LIBDIR"));
	fprintf(pkg_config_for_build,
		"exec ${PKG_CONFIG_FOR_BUILD:-${PKG_CONFIG:-pkg-config}} \"$@\"\n");
	if ( ferror(pkg_config_for_build) || fflush(pkg_config_for_build) == EOF )
		err(1, "%s", pkg_config_for_build_path);
	fchmod_plus_x(fileno(pkg_config_for_build));
	fclose(pkg_config_for_build);
	free(pkg_config_for_build_path);

	// Create a pkg-config script for the host system.
	char* var_pkg_config_libdir =
		print_string("%s%s/lib/pkgconfig",
		             minfo->sysroot ? minfo->sysroot : "", minfo->exec_prefix);
	if ( !var_pkg_config_libdir )
		err(1, "malloc");
	char* var_pkg_config_path = strdup(var_pkg_config_libdir);
	if ( !var_pkg_config_path )
		err(1, "malloc");
	char* pkg_config_name = print_string("%s-pkg-config", minfo->host);
	if ( !pkg_config_name )
		err(1, "malloc");
	char* pkg_config_path = join_paths(bindir, pkg_config_name);
	if ( !pkg_config_path )
		err(1, "malloc");
	FILE* pkg_config = fopen(pkg_config_path, "w");
	if ( !pkg_config )
		err(1, "`%s'", pkg_config_path);
	fprintf(pkg_config, "#!/bin/sh\n");
	fprint_shell_variable_assignment(pkg_config, "PATH", getenv("PATH"));
	fprint_shell_variable_assignment(pkg_config,
		"PKG_CONFIG", getenv("PKG_CONFIG"));
	fprint_shell_variable_assignment(pkg_config,
		"PKG_CONFIG_PATH", var_pkg_config_path);
	fprint_shell_variable_assignment(pkg_config,
		"PKG_CONFIG_SYSROOT_DIR", minfo->sysroot);
	fprint_shell_variable_assignment(pkg_config,
		"PKG_CONFIG_LIBDIR", var_pkg_config_libdir);
	// Pass --static as Sortix only static links at the moment.
	fprintf(pkg_config, "exec ${PKG_CONFIG:-%s} --static \"$@\"\n",
	        has_in_path(pkg_config_name) ? pkg_config_name : "pkg-config");
	fflush(pkg_config);
	if ( ferror(pkg_config) || fflush(pkg_config) == EOF )
		err(1, "%s", pkg_config_path);
	fchmod_plus_x(fileno(pkg_config));
	fclose(pkg_config);
	free(pkg_config_path);
	free(var_pkg_config_libdir);
	free(var_pkg_config_path);
}

static void append_to_path(const char* directory)
{
	const char* path = getenv("PATH");
	if ( path && path[0] )
	{
		char* new_path = print_string("%s:%s", directory, path);
		if ( !new_path )
			err(1, "malloc");
		if ( setenv("PATH", new_path, 1) < 0 )
			err(1, "setenv");
		free(new_path);
	}
	else if ( setenv("PATH", directory, 1) < 0 )
		err(1, "setenv");
}

static void EmitWrappers(struct metainfo* minfo)
{
	if ( !minfo->cross )
		return;

	char* bindir = join_paths(tmp_root, "bin");
	if ( mkdir(bindir, 0777) < 0 )
		err(1, "mkdir: %s", bindir);

	emit_pkg_config_wrapper(minfo, bindir);
	emit_compiler_sysroot_cross_wrapper(minfo, bindir, "cc");
	emit_compiler_sysroot_cross_wrapper(minfo, bindir, "gcc");
	emit_compiler_sysroot_cross_wrapper(minfo, bindir, "c++");
	emit_compiler_sysroot_cross_wrapper(minfo, bindir, "g++");
	emit_compiler_sysroot_cross_wrapper(minfo, bindir, "ld");

	append_to_path(bindir);

	free(bindir);
}

static void SetNeedVariableBuildTool(struct metainfo* minfo,
                                     const char* variable,
                                     const char* value)
{
	const char* needed_vars =
		metainfo_get_def(minfo, "MAKE_NEEDED_VARS", "pkg.make.needed-vars",
		                 "true");
	char* key = minfo->tixbuildinfo ?
	            print_string("pkg.make.needed-vars.%s", variable) :
	            print_string("MAKE_NEEDED_VARS_%s", variable);
	if ( !key )
		err(1, "malloc");
	const char* needed_var = metainfo_get_def(minfo, key, key, needed_vars);
	free(key);
	if ( !parse_boolean(needed_var) )
		return;
	if ( setenv(variable, value, 1) < 0 )
		err(1, "setenv");
}

static void SetNeedVariableCrossTool(struct metainfo* minfo,
                                     const char* variable,
                                     const char* value)
{
	if ( !minfo->cross )
		SetNeedVariableBuildTool(minfo, variable, value);
	else
	{
		char* newvalue = print_string("%s-%s", minfo->host, value);
		if ( !newvalue )
			err(1, "malloc");
		SetNeedVariableBuildTool(minfo, variable, newvalue);
		free(newvalue);
	}
}

static void SetNeededVariables(struct metainfo* minfo)
{
	if ( minfo->bootstrapping )
	{
		for ( size_t i = 0; i < BUILDVARS_LENGTH; i++ )
			unsetenv(buildvars[i].variable);
		for ( size_t i = 0; i < BUILDVARS_LENGTH; i++ )
		{
			char* for_build =
				print_string("%s_FOR_BUILD", buildvars[i].variable);
			if ( !for_build )
				err(1, "malloc");
			const char* value = getenv(for_build);
			if ( value && setenv(buildvars[i].variable, value, 1) < 0 )
				err(1, "setenv");
			free(for_build);
		}
		return;
	}

	for ( size_t i = 0; i < BUILDVARS_LENGTH; i++ )
	{
		if ( !buildvars[i].program && !getenv(buildvars[i].variable) )
			continue;
		char* for_build = print_string("%s_FOR_BUILD", buildvars[i].variable);
		if ( !for_build )
			err(1, "malloc");
		SetNeedVariableBuildTool(minfo, for_build, buildvars[i].value);
		free(for_build);
	}
	for ( size_t i = 0; i < BUILDVARS_LENGTH; i++ )
		if ( buildvars[i].program )
			SetNeedVariableCrossTool(minfo, buildvars[i].variable,
			                         buildvars[i].value);
}

static void Configure(struct metainfo* minfo)
{
	if ( !fork_and_wait_or_recovery() )
		return;
	const char* configure_raw =
		metainfo_get_def(minfo, "CONFIGURE", "pkg.configure.cmd",
		                 "./configure");
	char* configure;
	if ( strcmp(minfo->build_dir, minfo->package_dir) == 0 )
		configure = strdup(configure_raw);
	else
		configure = join_paths(minfo->package_dir, configure_raw);
	if ( !configure )
		err(1, "malloc");
	const char* conf_extra_args =
		metainfo_get_def(minfo, "CONFIGURE_ARGS", "pkg.configure.args", "");
	const char* conf_extra_vars =
		metainfo_get_def(minfo, "CONFIGURE_VARS", "pkg.configure.vars", "");
	bool with_sysroot =
		parse_boolean(metainfo_get_def(minfo,
			"CONFIGURE_WITH_SYSROOT", "pkg.configure.with-sysroot", "false"));
	// TODO: I am unclear if this issue still affects gcc, I might have
	//       forgotten to set pkg.configure.with-sysroot-ld-bug=true there.
	const char* with_sysroot_ld_bug_default = "false";
	if ( !strcmp(minfo->package_name, "gcc") )
		with_sysroot_ld_bug_default = "true";
	bool with_sysroot_ld_bug =
		parse_boolean(metainfo_get_def(minfo, "CONFIGURE_WITH_SYSROOT_LD_BUG",
			"pkg.configure.with-sysroot-ld-bug", with_sysroot_ld_bug_default ));
	bool with_build_sysroot =
		parse_boolean(metainfo_get_def(minfo, "CONFIGURE_WITH_BUILD_SYSROOT",
			"pkg.configure.with-build-sysroot", "false"));
	if ( chdir(minfo->build_dir) != 0 )
		err(1, "chdir: `%s'", minfo->build_dir);
	if ( minfo->subdir && chdir(minfo->subdir) != 0 )
		err(1, "chdir: `%s/%s'", minfo->build_dir, minfo->subdir);
	SetNeededVariables(minfo);
	string_array_t env_vars = string_array_make();
	string_array_append_token_string(&env_vars, conf_extra_vars);
	for ( size_t i = 0; i < env_vars.length; i++ )
	{
		char* key = env_vars.strings[i];
		assert(key);
		char* assignment = strchr((char*) key, '=');
		if ( !assignment )
		{
			if ( !strncmp(key, "unset ", strlen("unset ")) )
				unsetenv(key + strlen("unset "));
			continue;
		}
		*assignment = '\0';
		char* value = assignment+1;
		setenv(key, value, 1);
	}
	const char* fixed_cmd_argv[] =
	{
		configure,
		print_string("--prefix=%s", minfo->prefix),
		print_string("--exec-prefix=%s", minfo->exec_prefix),
		print_string("--build=%s", minfo->build),
		minfo->bootstrapping ? NULL :
		print_string("--host=%s", minfo->host),
		print_string("--target=%s", minfo->target),
		NULL
	};
	string_array_t args = string_array_make();
	for ( size_t i = 0; fixed_cmd_argv[i]; i++ )
		string_array_append(&args, fixed_cmd_argv[i]);
	if ( minfo->sysroot && with_build_sysroot )
	{
		string_array_append(&args, print_string("--with-build-sysroot=%s",
		                                        minfo->sysroot));
		if ( minfo->sysroot && with_sysroot )
		{
			// TODO: Binutils has a bug where the empty string means that
			//       sysroot support is disabled and ld --sysroot won't work
			//       so set it to / here for compatibility.
			// TODO: GCC has a bug where it doesn't use the
			//       --with-build-sysroot value when --with-sysroot= when
			//       locating standard library headers.
			if ( with_sysroot_ld_bug )
				string_array_append(&args, "--with-sysroot=/");
			else
				string_array_append(&args, "--with-sysroot=");
		}
	}
	else if ( minfo->sysroot && with_sysroot )
	{
		string_array_append(&args, print_string("--with-sysroot=%s",
		                                        minfo->sysroot));
	}
	string_array_append_token_string(&args, conf_extra_args);
	string_array_append(&args, NULL);
	recovery_execvp(args.strings[0], (char* const*) args.strings);
	err(127, "`%s'", args.strings[0]);
}

static bool TestDirty(struct metainfo* minfo,
                      const char* candidate)
{
	const char* subdir = minfo->subdir ? minfo->subdir : ".";
	char* path;
	if ( asprintf(&path, "%s/%s/%s", minfo->build_dir, subdir, candidate) < 0 )
		err(1, "asprintf");
	bool result = access(path, F_OK) == 0;
	free(path);
	return result;
}

static bool IsDirty(struct metainfo* minfo)
{
	const char* dirty_file = metainfo_get(minfo, "DIRTY_FILE", "pkg.dirty-file");
	if ( dirty_file )
		return TestDirty(minfo, dirty_file);
	return TestDirty(minfo, "config.log") ||
	       TestDirty(minfo, "Makefile") ||
	       TestDirty(minfo, "makefile");
}

static void Make(struct metainfo* minfo,
                 const char* make_target,
                 const char* destdir,
                 bool die_on_error)
{
	if ( !(die_on_error ?
	       fork_and_wait_or_recovery() :
	       fork_and_wait_or_death_def(false)) )
		return;

	char* make = strdup(minfo->make);
	const char* override_make = metainfo_get(minfo, "MAKE", "pkg.make.cmd");
	const char* make_extra_args =
		metainfo_get_def(minfo, "MAKE_ARGS", "pkg.make.args", "");
	const char* make_extra_vars =
		metainfo_get_def(minfo, "MAKE_VARS", "pkg.make.vars", "");
	if ( override_make )
	{
		free(make);
		make = strdup(override_make);
	}
	SetNeededVariables(minfo);
	if ( chdir(minfo->build_dir) != 0 )
		err(1, "chdir: `%s'", minfo->build_dir);
	if ( minfo->subdir && chdir(minfo->subdir) != 0 )
		err(1, "chdir: `%s/%s'", minfo->build_dir, minfo->subdir);
	if ( !minfo->bootstrapping && destdir )
		setenv("DESTDIR", destdir, 1);
	setenv("BUILD", minfo->build, 1);
	setenv("HOST", minfo->host, 1);
	setenv("TARGET", minfo->target, 1);
	if ( minfo->prefix )
		setenv("PREFIX", minfo->prefix, 1);
	else
		unsetenv("PREFIX");
	if ( minfo->exec_prefix )
		setenv("EXEC_PREFIX", minfo->exec_prefix, 1);
	else
		unsetenv("EXEC_PREFIX");
	if ( minfo->makeflags )
		setenv("MAKEFLAGS", minfo->makeflags, 1);
	setenv("MAKE", minfo->make, 1);
	string_array_t env_vars = string_array_make();
	string_array_append_token_string(&env_vars, make_extra_vars);
	for ( size_t i = 0; i < env_vars.length; i++ )
	{
		char* key = env_vars.strings[i];
		assert(key);
		char* assignment = strchr((char*) key, '=');
		if ( !assignment )
		{
			if ( !strncmp(key, "unset ", strlen("unset ")) )
				unsetenv(key + strlen("unset "));
			continue;
		}
		*assignment = '\0';
		char* value = assignment+1;
		setenv(key, value, 1);
	}
	const char* fixed_cmd_argv[] = { make, NULL };
	string_array_t args = string_array_make();
	for ( size_t i = 0; fixed_cmd_argv[i]; i++ )
		string_array_append(&args, fixed_cmd_argv[i]);
	string_array_append_token_string(&args, make_target);
	string_array_append_token_string(&args, make_extra_args);
	if ( !die_on_error )
		string_array_append(&args, "-k");
	string_array_append(&args, NULL);
	if ( die_on_error )
		recovery_execvp(args.strings[0], (char* const*) args.strings);
	else
		execvp(args.strings[0], (char* const*) args.strings);
	err(127, "`%s'", args.strings[0]);
}

static void Clean(struct metainfo* minfo)
{
	const char* build_system =
		metainfo_get_def(minfo, "BUILD_SYSTEM", "pkg.build-system", "none");
	const char* default_clean_target =
		!strcmp(build_system, "configure") ? "distclean" : "clean";
	const char* clean_target =
		metainfo_get_def(minfo, "MAKE_CLEAN_TARGET", "pkg.make.clean-target",
		                 default_clean_target);
	const char* ignore_clean_failure_var =
		metainfo_get_def(minfo, "MAKE_IGNORE_CLEAN_FAILURE",
		                 "pkg.make.ignore-clean-failure", "true");
	bool ignore_clean_failure = parse_boolean(ignore_clean_failure_var);

	Make(minfo, clean_target, NULL, !ignore_clean_failure);
}

static void Build(struct metainfo* minfo)
{
	const char* build_target =
		metainfo_get_def(minfo, "MAKE_BUILD_TARGET", "pkg.make.build-target",
		                 "all");

	Make(minfo, build_target, NULL, true);
}

static void CreateDestination(struct metainfo* minfo)
{
	char* tardir_rel = join_paths(tmp_root, "tix");
	if ( !tardir_rel )
		err(1, "malloc");
	// TODO: After releasing Sortix 1.1, remove generation 2 compatibility.
	const char* prefix = 3 <= minfo->generation ? minfo->prefix : "";
	char* prefixdir_rel = print_string("%s%s", tardir_rel, prefix);
	if ( !prefixdir_rel )
		err(1, "malloc");
	if ( mkdir_p(prefixdir_rel, 0755) < 0 )
		err(1, "mkdir: %s", prefixdir_rel);
	// TODO: After releasing Sortix 1.1, remove generation 2 compatibility.
	if ( minfo->generation == 2 )
	{
		char* destdir_rel = join_paths(prefixdir_rel, "data");
		char* tixdir_rel = join_paths(prefixdir_rel, "tix");
		if ( mkdir(destdir_rel, 0755) != 0 )
			err(1, "mkdir: `%s'", destdir_rel);
		if ( mkdir(tixdir_rel, 0755) != 0 )
			err(1, "mkdir: `%s'", tixdir_rel);
		free(tixdir_rel);
		free(destdir_rel);
	}
	free(prefixdir_rel);
	free(tardir_rel);
}

static void Install(struct metainfo* minfo)
{
	const char* install_target =
		metainfo_get_def(minfo, "MAKE_INSTALL_TARGET",
		                 "pkg.make.install-target", "install");
	char* tardir_rel = join_paths(tmp_root, "tix");
	// TODO: After releasing Sortix 1.1, remove generation 2 compatibility.
	char* destdir_rel = minfo->generation == 3 ? strdup(tardir_rel) :
	                    join_paths(tardir_rel, "data");
	char* destdir = realpath(destdir_rel, NULL);
	if ( !destdir )
		err(1, "realpath: %s", tardir_rel);

	Make(minfo, install_target, destdir, true);

	free(tardir_rel);
	free(destdir_rel);
	free(destdir);
}

static void PostInstall(struct metainfo* minfo)
{
	const char* post_install_cmd =
		metainfo_get(minfo, "POST_INSTALL", "pkg.post-install.cmd");
	if ( !post_install_cmd )
		return;

	if ( !fork_and_wait_or_recovery() )
		return;

	char* tardir_rel = join_paths(tmp_root, "tix");
	// TODO: After releasing Sortix 1.1, remove generation 2 compatibility.
	char* destdir_rel = minfo->generation == 3 ? strdup(tardir_rel) :
	                    join_paths(tardir_rel, "data");
	char* destdir = realpath(destdir_rel, NULL);
	if ( !destdir )
		err(1, "realpath: %s", destdir_rel);

	SetNeededVariables(minfo);
	if ( chdir(minfo->package_dir) != 0 )
		err(1, "chdir: `%s'", minfo->package_dir);
	if ( minfo->subdir && chdir(minfo->subdir) != 0 )
		err(1, "chdir: `%s/%s'", minfo->build_dir, minfo->subdir);
	setenv("TIX_BUILD_DIR", minfo->build_dir, 1);
	setenv("TIX_SOURCE_DIR", minfo->package_dir, 1);
	setenv("TIX_INSTALL_DIR", destdir, 1);
	if ( minfo->sysroot )
		setenv("TIX_SYSROOT", minfo->sysroot, 1);
	else
		unsetenv("TIX_SYSROOT");
	setenv("BUILD", minfo->build, 1);
	setenv("HOST", minfo->host, 1);
	setenv("TARGET", minfo->target, 1);
	if ( minfo->prefix )
		setenv("PREFIX", minfo->prefix, 1);
	else
		unsetenv("PREFIX");
	if ( minfo->exec_prefix )
		setenv("EXEC_PREFIX", minfo->exec_prefix, 1);
	else
		unsetenv("EXEC_PREFIX");
	const char* cmd_argv[] =
	{
		post_install_cmd,
		NULL
	};
	recovery_execvp(cmd_argv[0], (char* const*) cmd_argv);
	err(127, "%s", cmd_argv[0]);
}

static void TixInfo(struct metainfo* minfo)
{
	char* tardir_rel = join_paths(tmp_root, "tix");
	if ( !tardir_rel )
		err(1, "malloc");
	// TODO: After releasing Sortix 1.1, remove generation 2 compatibility.
	const char* prefix = 3 <= minfo->generation ? minfo->prefix : "";
	char* prefixdir_rel = print_string("%s%s", tardir_rel, prefix);
	if ( !prefixdir_rel )
		err(1, "malloc");
	char* tixdir_rel = join_paths(prefixdir_rel, "tix");
	if ( !tixdir_rel )
		err(1, "malloc");
	// TODO: After releasing Sortix 1.1, remove generation 2 compatibility.
	if ( 3 <= minfo->generation && mkdir(tixdir_rel, 0755) && errno != EEXIST )
		err(1, "%s", tixdir_rel);
	char* tixinfodir_rel = join_paths(tixdir_rel, "tixinfo");
	if ( !tixinfodir_rel )
		err(1, "malloc");
	// TODO: After releasing Sortix 1.1, remove generation 2 compatibility.
	if ( 3 <= minfo->generation &&
	     mkdir(tixinfodir_rel, 0755) && errno != EEXIST )
		err(1, "%s", tixdir_rel);
	char* tixinfo_rel = 3 <= minfo->generation ?
	                     join_paths(tixinfodir_rel, minfo->package_name) :
	                     strdup(tixinfodir_rel);

	const char* alias = metainfo_get(minfo, "ALIAS_OF", "pkg.alias-of");
	const char* runtime_deps =
		metainfo_get(minfo, "RUNTIME_DEPS", "pkg.runtime-deps");
	bool location_independent =
		parse_boolean(metainfo_get_def(minfo,
			"LOCATION_INDEPENDENT", "pkg.location-independent", "false"));
	bool is_set =
		parse_boolean(metainfo_get_def(minfo, "IS_SET", "pkg.is-set", "false"));

	FILE* tixinfo_fp = fopen(tixinfo_rel, "w");
	if ( !tixinfo_fp )
		err(1, "`%s'", tixinfo_rel);

	if ( 3 <= minfo->generation )
	{
		// TODO: Shell escape the values if needed.
		fwrite_variable(tixinfo_fp, "TIX_VERSION", "3");
		fwrite_variable(tixinfo_fp, "NAME", minfo->package_name);
		const char* edition = metainfo_get(minfo, "EDITION", "pkg.edition");
		if ( edition )
			fwrite_variable(tixinfo_fp, "EDITION", edition);
		const char* version = metainfo_get(minfo, "VERSION", "VERSION");
		if ( version )
			fwrite_variable(tixinfo_fp, "VERSION", version);
		const char* version_2 = metainfo_get(minfo, "VERSION_2", "VERSION_2");
		if ( version_2 )
			fwrite_variable(tixinfo_fp, "VERSION_2", version_2);
		fwrite_variable(tixinfo_fp, "PLATFORM", minfo->host);
		if ( alias )
			fwrite_variable(tixinfo_fp, "ALIAS_OF", alias);
		else
		{
			if ( runtime_deps )
				fwrite_variable(tixinfo_fp, "RUNTIME_DEPS", runtime_deps);
			if ( location_independent )
				fwrite_variable(tixinfo_fp, "LOCATION_INDEPENDENT", "true");
			else
				fwrite_variable(tixinfo_fp, "PREFIX", minfo->prefix);
		}
		const char* renames = metainfo_get(minfo, "RENAMES", "pkg.renames");
		if ( renames )
			fwrite_variable(tixinfo_fp, "RENAMES", renames);
		if ( is_set )
			fwrite_variable(tixinfo_fp, "IS_SET", "true");
	}
	// TODO: After releasing Sortix 1.1, remove generation 2 compatibility.
	else
	{
		fprintf(tixinfo_fp, "tix.version=1\n");
		fprintf(tixinfo_fp, "tix.class=tix\n");
		fprintf(tixinfo_fp, "tix.platform=%s\n", minfo->host);
		fprintf(tixinfo_fp, "pkg.name=%s\n", minfo->package_name);
		if ( alias )
			fprintf(tixinfo_fp, "pkg.alias-of=%s\n", alias);
		else
		{
			if ( runtime_deps )
				fprintf(tixinfo_fp, "pkg.runtime-deps=%s\n", runtime_deps);
			if ( location_independent )
				fprintf(tixinfo_fp, "pkg.location-independent=true\n");
			else
				fprintf(tixinfo_fp, "pkg.prefix=%s\n", minfo->prefix);
		}
	}

	if ( ferror(tixinfo_fp) || fflush(tixinfo_fp) == EOF )
		err(1, "write: `%s'", tixinfo_rel);

	fclose(tixinfo_fp);
	free(tardir_rel);
	free(prefixdir_rel);
	free(tixdir_rel);
	free(tixinfodir_rel);
	free(tixinfo_rel);
}

static void TixManifest(struct metainfo* minfo)
{
	if ( !fork_and_wait_or_recovery() )
		return;
	char* tardir_rel = join_paths(tmp_root, "tix");
	if ( !tardir_rel )
		err(1, "malloc");
	// TODO: After releasing Sortix 1.1, remove generation 2 compatibility.
	const char* prefix = 3 <= minfo->generation ? minfo->prefix : "";
	char* prefixdir_rel = print_string("%s%s", tardir_rel, prefix);
	if ( !prefixdir_rel )
		err(1, "malloc");
	if ( chdir(prefixdir_rel) < 0 )
		err(1, "%s", prefixdir_rel);
	if ( mkdir("tix", 0755) && errno != EEXIST )
		err(1, "%s", "tix");
	if ( mkdir("tix/manifest", 0755) && errno != EEXIST )
		err(1, "%s", "tix/manifest");
	char* command;
	if ( asprintf(&command,
	              "find . -name tix -prune -o -print | "
	              "sed -E -e 's,^\\.$,/,' -e 's,^\\./,/,' | "
	              "LC_ALL=C sort > tix/manifest/%s",
	              minfo->package_name) < 0 )
		err(1, "malloc");
	const char* cmd_argv[] = { "sh", "-c", command, NULL };
	recovery_execvp(cmd_argv[0], (char* const*) cmd_argv);
	err(127, "%s", cmd_argv[0]);
}

static void Package(struct metainfo* minfo)
{
	if ( !fork_and_wait_or_recovery() )
		return;
	char* tardir_rel = join_paths(tmp_root, "tix");
	if ( !tardir_rel )
		err(1, "malloc");
	// TODO: After releasing Sortix 1.1, remove generation 2 compatibility.
	const char* prefix = 3 <= minfo->generation ? minfo->prefix : "";
	char* prefixdir_rel = print_string("%s%s", tardir_rel, prefix);
	if ( !prefixdir_rel )
		err(1, "malloc");
	char* package_tix = print_string("%s/%s.tix.tar.xz",
		minfo->destination, minfo->package_name);
	if ( !package_tix )
		err(1, "malloc");
	printf("Creating `%s'...\n", package_tix);
	fflush(stdout);
	const char* cmd_argv[] =
	{
		minfo->tar,
		"-C", prefixdir_rel,
		"--remove-files",
		"--create",
		"--xz",
		"--numeric-owner",
		"--owner=0",
		"--group=0",
		"--file", package_tix,
		"--",
		"tix",
		NULL
	};
	string_array_t cmd = string_array_make();
	for ( size_t i = 0; cmd_argv[i]; i++ )
		if ( !string_array_append(&cmd, cmd_argv[i]) )
			err(1, "malloc");
	struct dirent** entries;
	int count = scandir(prefixdir_rel, &entries, NULL, alphasort);
	if ( count < 0 )
		err(1, "scandir: %s", prefixdir_rel);
	for ( int i = 0; i < count; i++ )
	{
		const char* name = entries[i]->d_name;
		if ( !strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, "tix") )
			continue;
		if ( !string_array_append(&cmd, name) )
			err(1, "malloc");
	}
	if ( !string_array_append(&cmd, NULL) )
		err(1, "malloc");
	recovery_execvp(cmd.strings[0], (char* const*) cmd.strings);
	err(127, "%s", cmd.strings[0]);
}

static void Compile(struct metainfo* minfo)
{
	// Detect which build system we are interfacing with.
	const char* build_system =
		metainfo_get(minfo, "BUILD_SYSTEM", "pkg.build-system");
	if ( !build_system )
		errx(1, "%s: pkg.build-system was not found", minfo->package_info_path);

	if ( !strcmp(build_system, "none") )
		return;

	// Determine whether need to do an out-of-directory build.
	const char* subdir = metainfo_get(minfo, "SUBDIR", "pkg.subdir");
	const char* use_build_dir_var =
		metainfo_get_def(minfo, "CONFIGURE_USE_BUILD_DIRECTORY",
		                 "pkg.configure.use-build-directory", "false");
	bool use_build_dir = parse_boolean(use_build_dir_var);
	if ( use_build_dir )
	{
		const char* build_rel =
			minfo->bootstrapping ? "build-bootstrap" : "build";
		minfo->build_dir = join_paths(tmp_root, build_rel);
		if ( !minfo->build_dir )
			err(1, "malloc");
		if ( mkdir(minfo->build_dir, 0777) < 0 )
			err(1, "mkdir %s", minfo->build_dir);
		if ( subdir )
			minfo->package_dir = join_paths(minfo->package_dir, subdir);
	}
	else
	{
		minfo->build_dir = strdup(minfo->package_dir);
		if ( subdir )
			minfo->subdir = strdup(subdir);
	}

	// Reset the build directory if needed.
	if ( SHOULD_DO_BUILD_STEP(BUILD_STEP_PRE_CLEAN, minfo) &&
	     !use_build_dir &&
	     IsDirty(minfo) )
		Clean(minfo);

	// Configure the build directory if needed.
	if ( strcmp(build_system, "configure") == 0 &&
	     SHOULD_DO_BUILD_STEP(BUILD_STEP_CONFIGURE, minfo) )
		Configure(minfo);

	if ( SHOULD_DO_BUILD_STEP(BUILD_STEP_BUILD, minfo) )
		Build(minfo);

	if ( SHOULD_DO_BUILD_STEP(BUILD_STEP_INSTALL, minfo) )
		Install(minfo);

	if ( SHOULD_DO_BUILD_STEP(BUILD_STEP_POST_INSTALL, minfo) )
		PostInstall(minfo);

	// Clean the build directory after the successful build.
	if ( SHOULD_DO_BUILD_STEP(BUILD_STEP_POST_CLEAN, minfo) )
		Clean(minfo);
}

static void Bootstrap(struct metainfo* minfo)
{
	struct metainfo newinfo = { 0 };

	newinfo.build = minfo->build;
	newinfo.build_dir = NULL;
	newinfo.destination = NULL;
	newinfo.generation = minfo->generation;
	newinfo.host = minfo->build;
	newinfo.make = minfo->make;
	newinfo.makeflags = minfo->makeflags;
	newinfo.package_dir = minfo->package_dir;
	newinfo.package_info_path = minfo->package_info_path;
	newinfo.package_name = minfo->package_name;
	newinfo.prefix = join_paths(tmp_root, "bootstrap");
	if ( !newinfo.prefix )
		err(1, "malloc");
	if ( mkdir(newinfo.prefix, 0777) < 0 )
		err(1, "mkdir: %s", newinfo.prefix);
	newinfo.exec_prefix = newinfo.prefix;
	newinfo.sysroot = NULL;
	newinfo.tar = minfo->tar;
	newinfo.target = minfo->host;
	newinfo.tmp = minfo->tmp;
	const char* bootstrap_prefix =
		minfo->tixbuildinfo ? "bootstrap." : "BOOTSTRAP_";
	for ( size_t i = 0; i < minfo->package_info.length; i++ )
	{
		const char* string = minfo->package_info.strings[i];
		if ( minfo->tixbuildinfo ?
		     !strncmp(string, "pkg.", strlen("pkg.")) :
		     strncmp(string, bootstrap_prefix, strlen(bootstrap_prefix)) != 0 )
			continue;
		if ( !strncmp(string, bootstrap_prefix, strlen(bootstrap_prefix)) )
		{
			const char* rest = string + strlen(bootstrap_prefix);
			char* newstring = minfo->tixbuildinfo ?
			                  print_string("pkg.%s", rest) :
			                  strdup(rest);
			if ( !newstring )
				err(1, "malloc");
			if ( !string_array_append(&newinfo.package_info, newstring) )
				err(1, "malloc");
			free(newstring);
		}
		else
		{
			if ( !string_array_append(&newinfo.package_info, string) )
				err(1, "malloc");
		}
	}
	newinfo.start_step = BUILD_STEP_PRE_CLEAN;
	newinfo.end_step = BUILD_STEP_POST_CLEAN;
	newinfo.bootstrapping = true;
	newinfo.cross = false;

	Compile(&newinfo);

	char* bindir = join_paths(newinfo.prefix, "bin");
	if ( !bindir )
		err(1, "malloc");
	if ( access(bindir, F_OK) == 0 )
		append_to_path(bindir);
	free(bindir);

	char* sbindir = join_paths(newinfo.prefix, "sbin");
	if ( !sbindir )
		err(1, "malloc");
	if ( access(sbindir, F_OK) == 0 )
		append_to_path(sbindir);
	free(sbindir);

	string_array_reset(&newinfo.package_info);
	free(newinfo.prefix);
}

static void BuildPackage(struct metainfo* minfo)
{
	// Whether this is just an alias for another package.
	const char* alias = metainfo_get(minfo, "ALIAS_OF", "pkg.alias-of");

	// Determine if the package is location independent.
	bool location_independent =
		parse_boolean(metainfo_get_def(minfo, "LOCATION_INDEPENDENT",
		                               "pkg.location-independent", "false"));
	if ( !alias && !location_independent && !minfo->prefix )
		errx(1, "error: %s is not location independent and you need to "
		        "specify the intended destination prefix using --prefix",
		        minfo->package_name);

	CreateDestination(minfo);

	// Possibly build a native version of the package to aid cross-compilation.
	// This is an anti-feature needed for broken packages that don't properly
	// handle this case entirely themselves. There's a few packages that need
	// the exact same version around natively in order to cross-compile.
	const char* use_bootstrap_var =
		metainfo_get_def(minfo, "USE_BOOTSTRAP", "pkg.use-bootstrap", "false");
	bool use_bootstrap = parse_boolean(use_bootstrap_var);
	if ( !alias && use_bootstrap && strcmp(minfo->build, minfo->host) != 0 &&
	     SHOULD_DO_BUILD_STEP(BUILD_STEP_CONFIGURE, minfo) )
		Bootstrap(minfo);

	EmitWrappers(minfo);

	if ( !alias )
		Compile(minfo);

	if ( SHOULD_DO_BUILD_STEP(BUILD_STEP_PACKAGE, minfo) )
	{
		TixInfo(minfo);
		// TODO: After releasing Sortix 1.1, remove generation 2 compatibility.
		if ( 3 <= minfo->generation )
			TixManifest(minfo);
		Package(minfo);
	}
}

static void VerifySourceTixInformation(struct metainfo* minfo)
{
	if ( minfo->tixbuildinfo )
	{
		const char* pipath = minfo->package_info_path;
		string_array_t* pinfo = &minfo->package_info;
		const char* tix_version =
			VerifyInfoVariable(pinfo, "tix.version", pipath);
		if ( atoi(tix_version) != 1 )
			errx(1, "error: `%s': tix version `%s' not supported", pipath,
				    tix_version);
		const char* tix_class = VerifyInfoVariable(pinfo, "tix.class", pipath);
		if ( !strcmp(tix_class, "tix") )
			errx(1, "error: `%s': this object is a binary tix and is already "
				    "compiled.\n", pipath);
		if ( strcmp(tix_class, "srctix") )
			errx(1, "error: `%s': tix class `%s' is not `srctix': this object "
				    "is not suitable for compilation.", pipath, tix_class);
	}
	metainfo_verify(minfo, "NAME", "pkg.name");
	if ( !metainfo_get(minfo, "ALIAS_OF", "pkg.alias-of") )
		metainfo_verify(minfo, "BUILD_SYSTEM", "pkg.build-system");
}

// TODO: The MAKEFLAGS variable is actually not in the same format as the token
//       string language. It appears that GNU make doesn't escape " characters,
//       but instead consider them normal characters. This should work as
//       expected, though, as long as the MAKEFLAGS variable doesn't contain any
//       quote characters.
static void PurifyMakeflags(void)
{
	const char* makeflags_environment = getenv("MAKEFLAGS");
	if ( !makeflags_environment )
		return;
	string_array_t makeflags = string_array_make();
	string_array_append_token_string(&makeflags, makeflags_environment);
	// Discard all the environmental variables in MAKEFLAGS.
	for ( size_t i = 0; i < makeflags.length; i++ )
	{
		char* flag = makeflags.strings[i];
		assert(flag);
		if ( flag[0] == '-' )
			continue;
		if ( !strchr(flag, '=') )
			continue;
		free(flag);
		for ( size_t n = i + 1; n < makeflags.length; n++ )
			makeflags.strings[n-1] = makeflags.strings[n];
		makeflags.length--;
		i--;
	}
	char* new_makeflags_environment = token_string_of_string_array(&makeflags);
	assert(new_makeflags_environment);
	setenv("MAKEFLAGS", new_makeflags_environment, 1);
	free(new_makeflags_environment);
	string_array_reset(&makeflags);
}

static char* FindPortFile(char* package_dir)
{
	char* path = print_string("%s.port", package_dir);
	if ( !path )
		err(1, "malloc");
	if ( !access(path, F_OK) )
		return path;
	free(path);
	path = join_paths(package_dir, "tix.port");
	if ( !path )
		err(1, "malloc");
	if ( !access(path, F_OK) )
		return path;
	free(path);
	return NULL;
}

static char* FindTixBuildInfo(char* package_dir)
{
	char* path = join_paths(package_dir, "tixbuildinfo");
	if ( !path )
		err(1, "malloc");
	if ( !access(path, F_OK) )
		return path;
	free(path);
	return NULL;
}

int main(int argc, char* argv[])
{
	PurifyMakeflags();

	bool print_build = false;
	bool print_host = false;
	bool print_target = false;

	struct metainfo minfo;
	memset(&minfo, 0, sizeof(minfo));
	minfo.build = NULL;
	minfo.destination = strdup(".");
	minfo.host = NULL;
	char* generation_string = strdup(DEFAULT_GENERATION);
	minfo.makeflags = strdup_null(getenv_def("MAKEFLAGS", NULL));
	minfo.make = strdup(getenv_def("MAKE", "make"));
	minfo.prefix = strdup("");
	minfo.exec_prefix = NULL;
	minfo.sysroot = NULL;
	minfo.target = NULL;
	minfo.tar = strdup("tar");
	char* tmp = strdup(getenv_def("TMPDIR", "/tmp"));
	char* start_step_string = strdup("start");
	char* end_step_string = strdup("end");
	char* source_port = NULL;

	const char* argv0 = argv[0];
	for ( int i = 0; i < argc; i++ )
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
			default:
				errx(1, "unknown option -- '%c'", c);
			}
		}
		else if ( !strcmp("--print-build", arg) )
			print_build = true;
		else if ( !strcmp("--print-host", arg) )
			print_host = true;
		else if ( !strcmp("--print-target", arg) )
			print_target = true;
		else if ( GET_OPTION_VARIABLE("--build", &minfo.build) ) { }
		else if ( GET_OPTION_VARIABLE("--destination", &minfo.destination) ) { }
		else if ( GET_OPTION_VARIABLE("--end", &end_step_string) ) { }
		else if ( GET_OPTION_VARIABLE("--exec-prefix", &minfo.exec_prefix) ) { }
		else if ( GET_OPTION_VARIABLE("--generation", &generation_string) ) { }
		else if ( GET_OPTION_VARIABLE("--host", &minfo.host) ) { }
		else if ( GET_OPTION_VARIABLE("--make", &minfo.make) ) { }
		else if ( GET_OPTION_VARIABLE("--makeflags", &minfo.makeflags) ) { }
		else if ( GET_OPTION_VARIABLE("--prefix", &minfo.prefix) ) { }
		// TODO: After releasing Sortix 1.1, remove this option.
		else if ( GET_OPTION_VARIABLE("--source-package", &source_port) ) { }
		else if ( GET_OPTION_VARIABLE("--source-port", &source_port) ) { }
		else if ( GET_OPTION_VARIABLE("--start", &start_step_string) ) { }
		else if ( GET_OPTION_VARIABLE("--sysroot", &minfo.sysroot) ) { }
		else if ( GET_OPTION_VARIABLE("--tar", &minfo.tar) ) { }
		else if ( GET_OPTION_VARIABLE("--target", &minfo.target) ) { }
		else if ( GET_OPTION_VARIABLE("--tmp", &minfo.tmp) ) { }
		else
			errx(1, "unknown option: %s", arg);
	}

	compact_arguments(&argc, &argv);

	minfo.generation = atoi(generation_string);
	free(generation_string);
	// TODO: After releasing Sortix 1.1, remove generation 2 compatibility.
	if ( minfo.generation != 2 && minfo.generation != 3 )
		errx(1, "Unsupported generation: %i", minfo.generation);

	if ( !(minfo.start_step = step_of_step_name(start_step_string)) )
	{
		fprintf(stderr, "%s: no such step `%s'\n", argv0, start_step_string);
		exit(1);
	}

	if ( !(minfo.end_step = step_of_step_name(end_step_string)) )
	{
		fprintf(stderr, "%s: no such step `%s'\n", argv0, end_step_string);
		exit(1);
	}

	if ( minfo.build && !minfo.build[0] )
		free(minfo.build), minfo.build = NULL;
	if ( minfo.host && !minfo.host[0] )
		free(minfo.host), minfo.host = NULL;
	if ( minfo.target && !minfo.target[0] )
		free(minfo.target), minfo.target = NULL;

	if ( !minfo.build && !(minfo.build = GetBuildTriplet()) )
		err(1, "unable to determine build, use --build");
	if ( !minfo.host )
		minfo.host = strdup(minfo.build);
	if ( !minfo.target )
		minfo.target = strdup(minfo.host);

	minfo.cross = strcmp(minfo.build, minfo.host) != 0 || minfo.sysroot;

	if ( print_build || print_host || print_target )
	{
		if ( print_build )
			printf("%s\n", minfo.build);
		if ( print_host )
			printf("%s\n", minfo.host);
		if ( print_target )
			printf("%s\n", minfo.target);
		if ( ferror(stdout) || fflush(stdout) == EOF )
			err(1, "stdout");
		return 0;
	}

	if ( minfo.prefix && !strcmp(minfo.prefix, "/") )
		minfo.prefix[0] = '\0';

	if ( minfo.prefix && !minfo.exec_prefix )
		minfo.exec_prefix = strdup(minfo.prefix);

	if ( argc < 2 )
	{
		fprintf(stderr, "%s: no package specified\n", argv0);
		exit(1);
	}

	if ( 2 < argc )
	{
		fprintf(stderr, "%s: unexpected extra operand `%s'\n", argv0, argv[2]);
		exit(1);
	}

	initialize_tmp(tmp, "tixbuild");

	const char* srctix = argv[1];
	minfo.package_dir = realpath(srctix, NULL);
	if ( !minfo.package_dir )
		err(1, "%s", srctix);

	if ( !IsDirectory(minfo.package_dir) )
		err(1, "`%s'", minfo.package_dir);

	if ( (minfo.package_info_path = FindPortFile(minfo.package_dir)) )
	{
		minfo.tixbuildinfo = false;
		minfo.package_info = string_array_make();
		string_array_t* package_info = &minfo.package_info;
		int ret = variables_append_file_path(package_info,
		                                     minfo.package_info_path);
		if ( ret == -1 )
			err(1, "`%s'", minfo.package_info_path);
		else if ( ret == -2 )
			errx(1, "`%s': Syntax error", minfo.package_info_path);
	}
	else if ( (minfo.package_info_path = FindTixBuildInfo(minfo.package_dir)) )
	{
		minfo.tixbuildinfo = true;
		minfo.package_info = string_array_make();
		string_array_t* package_info = &minfo.package_info;
		if ( variables_append_file_path(package_info,
		                                minfo.package_info_path) < 0 )
			err(1, "`%s'", minfo.package_info_path);
	}
	else
		err(1, "Failed to find: %s.port or %s/tix.port or %s/tixbuildinfo",
		    minfo.package_dir, minfo.package_dir, minfo.package_dir);

	VerifySourceTixInformation(&minfo);
	minfo.package_name = strdup(metainfo_get(&minfo, "NAME", "pkg.name"));

	const char* pkg_source_port =
		metainfo_get(&minfo, "SOURCE_PORT", "pkg.source-package");
	if ( pkg_source_port && !source_port )
	{
		// TODO: Change this default location to match tix-port(8)?
		source_port = print_string("%s/../%s", srctix, pkg_source_port);
		if ( !source_port )
			err(1, "malloc");
	}

	if ( source_port )
	{
		free(minfo.package_dir);
		minfo.package_dir = realpath(source_port, NULL);
		if ( !minfo.package_dir )
			err(1, "%s: looking for source port: %s", srctix, source_port);
	}

	BuildPackage(&minfo);

	return 0;
}
