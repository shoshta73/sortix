/*
 * Copyright (c) 2026 Jonas 'Sortie' Termansen.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF7
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * strace.c
 * Trace system calls.
 */

#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <psctl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char* const syscalls[] =
{
	[SYSCALL_ACCEPT4] = "int sys_accept4(int, void*, size_t*, int);",
	[SYSCALL_ALARMNS] = "int sys_alarmns(const struct timespec*, struct timespec*);",
	[SYSCALL_BAD_SYSCALL] = "int sys_bad_syscall(void);",
	[SYSCALL_BIND] = "int sys_bind(int, const void*, size_t);",
	[SYSCALL_CLOCK_GETTIMERES] = "int sys_clock_gettimeres(clockid_t, struct timespec*, struct timespec*);",
	[SYSCALL_CLOCK_NANOSLEEP] = "int sys_clock_nanosleep(clockid_t, int, const struct timespec*, struct timespec*);",
	[SYSCALL_CLOCK_SETTIMERES] = "int sys_clock_settimeres(clockid_t, const struct timespec*, const struct timespec*);",
	[SYSCALL_CLOSE] = "int sys_close(int);",
	[SYSCALL_CLOSEFROM] = "int sys_closefrom(int);",
	[SYSCALL_CONNECT] = "int sys_connect(int, const void*, size_t);",
	[SYSCALL_DISPMSG_ISSUE] = "int sys_dispmsg_issue(void*, size_t);",
	[SYSCALL_DUP] = "int sys_dup(int);",
	[SYSCALL_DUP2] = "int sys_dup2(int, int);",
	[SYSCALL_DUP3] = "int sys_dup3(int, int, int);",
	[SYSCALL_EXECVE] = "int sys_execve(const char*, char* const*, char* const*);",
	[SYSCALL_EXECVEAT] = "int sys_execveat(int, const char*, char* const*, char* const*, int);",
	[SYSCALL_EXIT_THREAD] = "int sys_exit_thread(int, int, const struct exit_thread*);",
	[SYSCALL_FACCESSAT] = "int sys_faccessat(int, const char*, int, int);",
	[SYSCALL_FCHDIR] = "int sys_fchdir(int);",
	[SYSCALL_FCHDIRAT] = "int sys_fchdirat(int, const char*, int);",
	[SYSCALL_FCHDIRAT_NOFLAGS] = "int sys_fchdirat_noflags(int, const char*);",
	[SYSCALL_FCHMOD] = "int sys_fchmod(int, mode_t);",
	[SYSCALL_FCHMODAT] = "int sys_fchmodat(int, const char*, mode_t, int);",
	[SYSCALL_FCHOWN] = "int sys_fchown(int, uid_t, gid_t);",
#if defined(__i386__)
	[SYSCALL_FCHOWNAT] = "int sys_fchownat_wrapper(const struct fchownat_request*);",
#else
	[SYSCALL_FCHOWNAT] = "int sys_fchownat(int, const char*, uid_t, gid_t, int);",
#endif
	[SYSCALL_FCHROOT] = "int sys_fchroot(int);",
	[SYSCALL_FCHROOTAT] = "int sys_fchrootat(int, const char*, int);",
	[SYSCALL_FCHROOTAT_NOFLAGS] = "int sys_fchrootat_noflags(int, const char*);",
	[SYSCALL_FCNTL] = "int sys_fcntl(int, int, uintptr_t);",
	[SYSCALL_FEXECVE] = "int sys_fexecve(int, char* const*, char* const*);",
	[SYSCALL_FPATHCONF] = "long sys_fpathconf(int, int);",
	[SYSCALL_FSM_FSBIND] = "int sys_fsm_fsbind(int, int, int);",
	[SYSCALL_FSM_MOUNTAT] = "int sys_fsm_mountat(int, const char*, const struct stat*, int);",
	[SYSCALL_FSTATAT] = "int sys_fstatat(int, const char*, struct stat*, int);",
	[SYSCALL_FSTAT] = "int sys_fstat(int, struct stat*);",
	[SYSCALL_FSTATVFSAT] = "int sys_fstatvfsat(int, const char*, struct statvfs*, int);",
	[SYSCALL_FSTATVFS] = "int sys_fstatvfs(int, struct statvfs*);",
	[SYSCALL_FSYNC] = "int sys_fsync(int);",
	[SYSCALL_FTRUNCATE] = "int sys_ftruncate(int, off_t);",
	[SYSCALL_FUTEX] = "int sys_futex(int*, int, int, const struct timespec*);",
	[SYSCALL_FUTIMENS] = "int sys_futimens(int, const struct timespec*);",
	[SYSCALL_GETDNSCONFIG] = "int sys_getdnsconfig(struct dnsconfig*);",
	[SYSCALL_GETEGID] = "gid_t sys_getegid(void);",
	[SYSCALL_GETENTROPY] = "int sys_getentropy(void*, size_t);",
	[SYSCALL_GETEUID] = "uid_t sys_geteuid(void);",
	[SYSCALL_GETGID] = "gid_t sys_getgid(void);",
	[SYSCALL_GETGROUPS] = "int sys_getgroups(int, gid_t*);",
	[SYSCALL_GETHOSTNAME] = "int sys_gethostname(char*, size_t);",
	[SYSCALL_GETINIT] = "pid_t sys_getinit(pid_t);",
	[SYSCALL_GETPAGESIZE] = "size_t sys_getpagesize(void);",
	[SYSCALL_GETPEERNAME] = "int sys_getpeername(int, void*, size_t*);",
	[SYSCALL_GETPGID] = "pid_t sys_getpgid(pid_t);",
	[SYSCALL_GETPID] = "pid_t sys_getpid(void);",
	[SYSCALL_GETPPID] = "pid_t sys_getppid(void);",
	[SYSCALL_GETPRIORITY] = "int sys_getpriority(int, id_t);",
	[SYSCALL_GETSID] = "pid_t sys_getsid(pid_t);",
	[SYSCALL_GETSOCKNAME] = "int sys_getsockname(int, void*, size_t*);",
	[SYSCALL_GETSOCKOPT] = "int sys_getsockopt(int, int, int, void*, size_t*);",
	[SYSCALL_GETUID] = "uid_t sys_getuid(void);",
	[SYSCALL_GETUMASK] = "mode_t sys_getumask(void);",
	[SYSCALL_IOCTL] = "int sys_ioctl(int, int, uintptr_t);",
	[SYSCALL_ISATTY] = "int sys_isatty(int);",
	[SYSCALL_KERNELINFO] = "ssize_t sys_kernelinfo(const char*, char*, size_t);",
	[SYSCALL_KILL] = "int sys_kill(pid_t, int);",
	[SYSCALL_LINKAT] = "int sys_linkat(int, const char*, int, const char*, int);",
	[SYSCALL_LISTEN] = "int sys_listen(int, int);",
	[SYSCALL_LSEEK] = "off_t sys_lseek(int, off_t, int);",
	[SYSCALL_MEMSTAT] = "int sys_memstat(size_t*, size_t*);",
	[SYSCALL_MEMUSAGE] = "int sys_memusage(const size_t*, size_t*, size_t);",
	[SYSCALL_MKDIRAT] = "int sys_mkdirat(int, const char*, mode_t);",
	[SYSCALL_MKPARTITION] = "int sys_mkpartition(int, off_t, off_t, int);",
	[SYSCALL_MKPTY] = "int sys_mkpty(int*, int*, int);",
	[SYSCALL_MMAP_WRAPPER] = "void* sys_mmap_wrapper(struct mmap_request*);",
	[SYSCALL_MPROTECT] = "int sys_mprotect(void*, size_t, int);",
	[SYSCALL_MUNMAP] = "int sys_munmap(void*, size_t);",
	[SYSCALL_OPENAT] = "int sys_openat(int, const char*, int, mode_t);",
	[SYSCALL_PATHCONFAT] = "long sys_pathconfat(int, const char*, int, int);",
	[SYSCALL_PIPE2] = "int sys_pipe2(int*, int);",
	[SYSCALL_PPOLL] = "int sys_ppoll(struct pollfd*, size_t, const struct timespec*, const sigset_t*);",
	[SYSCALL_PREAD] = "ssize_t sys_pread(int, void*, size_t, off_t);",
	[SYSCALL_PREADV] = "ssize_t sys_preadv(int, const struct iovec*, int, off_t);",
	[SYSCALL_PRLIMIT] = "int sys_prlimit(pid_t, int, const struct rlimit*, struct rlimit*);",
	[SYSCALL_PSCTL] = "int sys_psctl(pid_t, int, void*);",
	[SYSCALL_PWRITE] = "ssize_t sys_pwrite(int, const void*, size_t, off_t);",
	[SYSCALL_PWRITEV] = "ssize_t sys_pwritev(int, const struct iovec*, int, off_t);",
	[SYSCALL_RAISE] = "int sys_raise(int);",
	[SYSCALL_RDMSR] = "uint64_t sys_rdmsr(uint32_t);",
	[SYSCALL_READ] = "ssize_t sys_read(int, void*, size_t);",
	[SYSCALL_READDIRENTS] = "ssize_t sys_readdirents(int, struct dirent*, size_t);",
	[SYSCALL_READLINKAT] = "ssize_t sys_readlinkat(int, const char*, char*, size_t);",
	[SYSCALL_READV] = "ssize_t sys_readv(int, const struct iovec*, int);",
	[SYSCALL_RECV] = "ssize_t sys_recv(int, void*, size_t, int);",
	[SYSCALL_RECVMSG] = "ssize_t sys_recvmsg(int, struct msghdr*, int);",
	[SYSCALL_RENAMEAT] = "int sys_renameat(int, const char*, int, const char*);",
	[SYSCALL_SCRAM] = "void sys_scram(int, const void*);",
	[SYSCALL_SCHED_YIELD] = "int sys_sched_yield(void);",
	[SYSCALL_SEND] = "ssize_t sys_send(int, const void*, size_t, int);",
	[SYSCALL_SENDMSG] = "ssize_t sys_sendmsg(int, const struct msghdr*, int);",
	[SYSCALL_SETDNSCONFIG] = "int sys_setdnsconfig(const struct dnsconfig*);",
	[SYSCALL_SETEGID] = "int sys_setegid(gid_t);",
	[SYSCALL_SETEUID] = "int sys_seteuid(uid_t);",
	[SYSCALL_SETGID] = "int sys_setgid(gid_t);",
	[SYSCALL_SETGROUPS] = "int sys_setgroups(int, const gid_t*);",
	[SYSCALL_SETHOSTNAME] = "int sys_sethostname(const char*, size_t);",
	[SYSCALL_SETINIT] = "int sys_setinit(void);",
	[SYSCALL_SETPGID] = "int sys_setpgid(pid_t, pid_t);",
	[SYSCALL_SETPRIORITY] = "int sys_setpriority(int, id_t, int);",
	[SYSCALL_SETSID] = "pid_t sys_setsid(void);",
	[SYSCALL_SETSOCKOPT] = "int sys_setsockopt(int, int, int, const void*, size_t);",
	[SYSCALL_SETUID] = "int sys_setuid(uid_t);",
	[SYSCALL_SHUTDOWN] = "int sys_shutdown(int, int);",
	[SYSCALL_SIGACTION] = "int sys_sigaction(int, const struct sigaction*, struct sigaction*);",
	[SYSCALL_SIGALTSTACK] = "int sys_sigaltstack(const stack_t*, stack_t*);",
	[SYSCALL_SIGPENDING] = "int sys_sigpending(sigset_t*);",
	[SYSCALL_SIGPROCMASK] = "int sys_sigprocmask(int, const sigset_t*, sigset_t*);",
	[SYSCALL_SIGSUSPEND] = "int sys_sigsuspend(const sigset_t*);",
	[SYSCALL_SOCKATMARK] = "int sys_sockatmark(int);",
	[SYSCALL_SOCKET] = "int sys_socket(int, int, int);",
	[SYSCALL_SYMLINKAT] = "int sys_symlinkat(const char*, int, const char*);",
	[SYSCALL_TCDRAIN] = "int sys_tcdrain(int);",
	[SYSCALL_TCFLOW] = "int sys_tcflow(int, int);",
	[SYSCALL_TCFLUSH] = "int sys_tcflush(int, int);",
	[SYSCALL_TCGETATTR] = "int sys_tcgetattr(int, struct termios*);",
	[SYSCALL_TCGETBLOB] = "ssize_t sys_tcgetblob(int, const char*, void*, size_t);",
	[SYSCALL_TCGETPGRP] = "pid_t sys_tcgetpgrp(int);",
	[SYSCALL_TCGETSID] = "pid_t sys_tcgetsid(int);",
	[SYSCALL_TCGETWINCURPOS] = "int sys_tcgetwincurpos(int, struct wincurpos*);",
	[SYSCALL_TCGETWINSIZE] = "int sys_tcgetwinsize(int, struct winsize*);",
	[SYSCALL_TCSENDBREAK] = "int sys_tcsendbreak(int, int);",
	[SYSCALL_TCSETATTR] = "int sys_tcsetattr(int, int, const struct termios*);",
	[SYSCALL_TCSETBLOB] = "ssize_t sys_tcsetblob(int, const char*, const void*, size_t);",
	[SYSCALL_TCSETPGRP] = "int sys_tcsetpgrp(int, pid_t);",
	[SYSCALL_TKILL] = "int sys_tkill(tid_t, int);",
	[SYSCALL_TFORK] = "pid_t sys_tfork(int, struct tfork*);",
	[SYSCALL_TIMENS] = "int sys_timens(struct tmns*);",
	[SYSCALL_TIMER_CREATE] = "int sys_timer_create(clockid_t, struct sigevent*, timer_t*);",
	[SYSCALL_TIMER_DELETE] = "int sys_timer_delete(timer_t);",
	[SYSCALL_TIMER_GETOVERRUN] = "int sys_timer_getoverrun(timer_t);",
	[SYSCALL_TIMER_GETTIME] = "int sys_timer_gettime(timer_t, struct itimerspec*);",
	[SYSCALL_TIMER_SETTIME] = "int sys_timer_settime(timer_t, int, const struct itimerspec*, struct itimerspec*);",
	[SYSCALL_TRUNCATEAT] = "int sys_truncateat(int, const char*, off_t, int);",
	[SYSCALL_TRUNCATEAT_NOFLAGS] = "int sys_truncateat_noflags(int, const char*, off_t);",
	[SYSCALL_UMASK] = "mode_t sys_umask(mode_t);",
	[SYSCALL_UNLINKAT] = "int sys_unlinkat(int, const char*, int);",
	[SYSCALL_UNMOUNTAT] = "int sys_unmountat(int, const char*, int);",
	[SYSCALL_UTIMENSAT] = "int sys_utimensat(int, const char*, const struct timespec*, int);",
	[SYSCALL_WAITPID] = "pid_t sys_waitpid(pid_t, int*, int);",
	[SYSCALL_WRITE] = "ssize_t sys_write(int, const void*, size_t);",
	[SYSCALL_WRITEV] = "ssize_t sys_writev(int, const struct iovec*, int);",
	[SYSCALL_WRMSR] = "uint64_t sys_wrmsr(uint32_t, uint64_t);",
};

struct type
{
	const char* name;
	size_t size;
	char representation;
};

static const struct type types[] =
{
	{ "*", sizeof(void*), 'x' },
	{ "char", sizeof(char), 'd' },
	{ "short", sizeof(short), 'd' },
	{ "int", sizeof(int), 'd' },
	{ "long", sizeof(long), 'd' },
	{ "long long", sizeof(long long), 'd' },
	{ "unsigned char", sizeof(unsigned char), 'u' },
	{ "unsigned short", sizeof(unsigned short), 'u' },
	{ "unsigned int", sizeof(unsigned int), 'u' },
	{ "unsigned long", sizeof(unsigned long), 'u' },
	{ "unsigned long long", sizeof(unsigned long long), 'u' },
	{ "int8_t", sizeof(int8_t), 'd' },
	{ "int16_t", sizeof(int16_t), 'd' },
	{ "int32_t", sizeof(int32_t), 'd' },
	{ "int64_t", sizeof(int64_t), 'd' },
	{ "uint8_t", sizeof(int8_t), 'u' },
	{ "intptr_t", sizeof(intptr_t), 'd' },
	{ "intmax_t", sizeof(intmax_t), 'd' },
	{ "uint16_t", sizeof(int16_t), 'u' },
	{ "uint32_t", sizeof(int32_t), 'u' },
	{ "uint64_t", sizeof(int64_t), 'u' },
	{ "uintptr_t", sizeof(uintptr_t), 'u' },
	{ "uintmax_t", sizeof(uintmax_t), 'u' },
	{ "ssize_t", sizeof(ssize_t), 'd' },
	{ "size_t", sizeof(size_t), 'u' },
	{ "clockid_t", sizeof(clockid_t), 'd' },
	{ "gid_t", sizeof(gid_t), 'u' },
	{ "id_t", sizeof(id_t), 'u' },
	{ "mode_t", sizeof(mode_t), 'o' },
	{ "off_t", sizeof(off_t), 'd' },
	{ "pid_t", sizeof(pid_t), 'd' },
	{ "tid_t", sizeof(tid_t), 'u' },
	{ "timer_t", sizeof(timer_t), 'u' },
	{ "uid_t", sizeof(uid_t), 'u' },
};

static const struct type* lookup_type(const char* type, size_t length)
{
	for ( size_t i = 0; i < length; i++ )
		if ( type[i] == '*' )
			return &types[0];
	for ( size_t i = 1; i < sizeof(types) / sizeof(types[0]); i++ )
	{
		if ( strlen(types[i].name) == length &&
		     !strncmp(type, types[i].name, length) )
			return &types[i];
	}
	return NULL;
}

static void print_type(FILE* out, const struct type* type, uintmax_t parameter)
{
	if ( type->size == 1 )
	{
		if ( type->representation == 'd' )
			fprintf(out, "%" PRId8, (int8_t) parameter);
		else if ( type->representation == 'x' )
			fprintf(out, "0x%" PRIx8, (uint8_t) parameter);
		else if ( type->representation == 'o' )
			fprintf(out, "0%" PRIo8, (uint8_t) parameter);
		else
			fprintf(out, "%" PRIu8, (uint8_t) parameter);
	}
	else if ( type->size == 2 )
	{
		if ( type->representation == 'd' )
			fprintf(out, "%" PRId16, (int16_t) parameter);
		else if ( type->representation == 'x' )
			fprintf(out, "0x%" PRIx16, (int16_t) parameter);
		else if ( type->representation == 'o' )
			fprintf(out, "0%" PRIo16, (int16_t) parameter);
		else
			fprintf(out, "%" PRIu16, (uint16_t) parameter);
	}
	else if ( type->size == 4 )
	{
		if ( type->representation == 'd' )
			fprintf(out, "%" PRId32, (int32_t) parameter);
		else if ( type->representation == 'x' )
			fprintf(out, "0x%" PRIx32, (int32_t) parameter);
		else if ( type->representation == 'o' )
			fprintf(out, "0%" PRIo32, (int32_t) parameter);
		else
			fprintf(out, "%" PRIu32, (uint32_t) parameter);
	}
	else if ( type->size == 8 )
	{
		if ( type->representation == 'd' )
			fprintf(out, "%" PRId64, (int64_t) parameter);
		else if ( type->representation == 'x' )
			fprintf(out, "0x%" PRIx64, (int64_t) parameter);
		else if ( type->representation == 'o' )
			fprintf(out, "0%" PRIo64, (int64_t) parameter);
		else
			fprintf(out, "%" PRIu64, (uint64_t) parameter);
	}
}

int main(int argc, char* argv[])
{
	bool inherit = false;
	const char* pid_str = NULL;

	const char* output_path = NULL;

	int opt;
	while ( (opt = getopt(argc, argv, "fo:p:")) != -1 )
	{
		switch ( opt )
		{
		case 'f': inherit = true; break;
		case 'o': output_path = optarg; break;
		case 'p': pid_str = optarg; break;
		default: return 125;
		}
	}

	if ( pid_str && optind != argc )
		errx(1, "cannot both use -p and executing a program: %s", argv[optind]);
	else if ( !pid_str && optind == argc )
		errx(1, "expected a program");

	FILE* out = output_path ? fopen(output_path, "w") : stderr;
	if ( !out )
		err(1, "%s", output_path);

	int sync_fds[2];
	if ( pipe(sync_fds) < 0 )
		err(125, "pipe");

	sigset_t all_signals, old_set;
	pid_t child;
	if ( pid_str )
	{
		// TODO: Support for attaching to multiple processes.
		intmax_t value;
		errno = 0;
		char* end;
		if ( (value = strtoimax(pid_str, (char**) &end, 10)) < 0 ||
		     *end || errno || (pid_t) value != value )
			errx(125, "Invalid process id: %s", pid_str);
		child = (pid_t) value;
	}
	else
	{
		// TODO: Fine tune.
		sigfillset(&all_signals);
		sigprocmask(SIG_BLOCK, &all_signals, &old_set);

		child = fork();
		if ( child < 0 )
			err(125, "fork");
		if ( !child )
		{
			close(sync_fds[1]);
			char c;
			read(sync_fds[0], &c, 1);
			sigprocmask(SIG_SETMASK, &old_set, NULL);
			execvp(argv[optind], argv + optind);
			err(127, "%s", argv[optind]);
		}
	}

	// NOTE: The psctl(2) PSCTL_STRACE interface is NOT a stable and official
	//       supported kernel interface at this time. It can only safely be
	//       used within the base system where strace(1) and kernel(7) match.
	struct psctl_strace req = { 0 };
	req.flags = PSCTL_STRACE_INHERIT_THREAD;
	if ( inherit )
		req.flags = PSCTL_STRACE_INHERIT_PROCESS;
	if ( psctl(child, PSCTL_STRACE, &req) < 0 )
	{
		kill(child, SIGKILL);
		err(125, "psctl");
	}

	if ( !pid_str )
	{
		close(sync_fds[0]);
		close(sync_fds[1]);

		if ( !output_path )
			sigprocmask(SIG_SETMASK, &old_set, NULL);
	}

	FILE* fp = fdopen(req.fd, "r");
	if ( !fp )
		err(125, "fdopen");

	// TODO: We can rely on a bounded size here.
	char* line = NULL;
	size_t line_size = 0;
	ssize_t line_length;
	bool executed = pid_str != NULL;
	bool expecting_return = false;
	const char* signature = NULL;
	while ( 0 < (line_length = getline(&line, &line_size, fp)) )
	{
		if ( line[line_length - 1] == '\n' )
			line[--line_length] = '\0';
		intmax_t process_id;
		size_t thread_id;
		size_t syscall;
		int offset;
		uintmax_t result;
		int errnum;
		if ( sscanf(line, "%ji %zi %zi(%n", &process_id, &thread_id, &syscall,
		            &offset) == 3 )
		{
			// TODO: Improve the situation if switching threads.
			if ( !executed && syscall == SYSCALL_EXECVEAT )
				executed = true;
			if ( !executed )
				continue;
			if ( expecting_return )
				fprintf(out, " = noreturn\n");
			expecting_return = true;
			if ( sizeof(syscalls) / sizeof(syscalls[0]) <= syscall )
				errx(125, "too high syscall: %zu: %s", syscall, line);
			signature = syscalls[syscall];
			if ( !signature )
				errx(125, "unknown syscall: %zu: %s", syscall, line);
			if ( inherit )
				fprintf(out, "[pid %jd tid 0x%zx] ", process_id, thread_id);
			const char* name = strstr(signature, "sys_") + 4;
			assert(name);
			size_t name_len = strcspn(name, "(");
			assert(name[name_len] == '(');
			fwrite(name, 1, name_len, out);
			fputc('(', out);
			uintmax_t parameter;
			int len;
			const char* template = name + name_len + 1;
			while ( *template != ')' )
			{
				if ( *template == ' ' )
				{
					template++;
					continue;
				}
				if ( !strcmp(template, "void);") )
					break;
				size_t type_len = strcspn(template, ",)");
				const struct type* type = lookup_type(template, type_len);
				if ( !type )
					err(125, "unknown type: %s\n", template);
				// TODO: On 32-bit, this can't handle a 64-bit value split
				//       into two 32-bit registers.
				if ( sscanf(line + offset, " 0x%jx%n", &parameter, &len) != 1 )
					err(1, "parse failed: '%s'", line + offset);
				if ( !strcmp(type->name, "*") )
				{
					fputs("(", out);
					fwrite(template, 1, type_len, out);
					fputs(") ", out);
				}
				print_type(out, type, parameter);
				template += type_len;
				offset += len;
				if ( *template == ',' )
				{
					if ( line[offset] != ',' )
						break;
					fprintf(out, ", ");
					template++;
					offset++;
				}

			}
			fprintf(out, ")");
		}
		else if ( sscanf(line, "%ji %zi = 0x%jx (%d)",
		                 &process_id, &thread_id, &result, &errnum) == 4 )
		{
			// TODO: Improve the situation if switching threads.
			if ( !executed || !signature )
				continue;
			expecting_return = false;
			size_t type_len = (strstr(signature, "sys_") - 1) - signature;
			const struct type* type = lookup_type(signature, type_len);
			if ( !type )
				err(125, "unknown type: %s\n", signature);
			fputs(" = ", out);
			print_type(out, type, result);
			if ( errnum &&
			     ((type->size == 4 && (int32_t) result == -1) ||
			      (type->size == 8 && (int64_t) result == -1)) )
				fprintf(out, " (%s)", strerror(errnum));
			fputc('\n', out);
		}
	}
	if ( ferror(fp) )
		err(125, "getline");
	free(line);
	if ( expecting_return )
		fprintf(out, " = noreturn\n");

	if ( !pid_str )
	{
		// Exit in the exact same manner as the child without dumping core.
		int status;
		waitpid(child, &status, 0);
		exit_thread(status, EXIT_THREAD_PROCESS, NULL);
		__builtin_unreachable();
	}

	return 0;
}
