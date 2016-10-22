// TODO: Rename to getty(8).

#include <sys/ioctl.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <pty.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#if !defined(TTY_NAME_MAX)
#include <sortix/limits.h>
#endif

static pid_t main_pid;
static const char* serial = NULL;
static struct termios old_tio;
static bool has_old_tio;
static int incoming_fd;
static int outgoing_fd;
static int master_fd;

static void exit_handler(void)
{
	if ( main_pid != getpid() )
		return;

	if ( has_old_tio )
	{
		if ( tcsetattr(0, TCSAFLUSH, &old_tio) )
			warn("tcsetattr");
	}
}

static void* incoming_thread(void* ctx)
{
	(void) ctx;
	char c;
	ssize_t amount = 0;
	while ( 0 < (amount = read(incoming_fd, &c, 1)) )
	{
		if ( write(master_fd, &c, 1) <= 0 )
		{
			warn("incoming write");
			break;
		}
	}
	if ( amount < 0 )
		warn("incoming read");
	exit(0);
	return NULL;
}

static void* outgoing_thread(void* ctx)
{
	(void) ctx;
	char c;
	ssize_t amount = 0;
	struct wincurpos wcp;
	bool emulate_getcursor = !serial && tcgetwincurpos(incoming_fd, &wcp) == 0;
	if ( emulate_getcursor )
	{
		const char* getcursor = "\e[6n";
		size_t i = 0;
		while ( 0 < (amount = read(master_fd, &c, 1)) )
		{
			if ( c == getcursor[i] )
			{
				i++;
				if ( !getcursor[i] )
				{
					i = 0;
					struct wincurpos wcp;
					tcgetwincurpos(incoming_fd, &wcp);
					char buf[64];
					snprintf(buf, sizeof(buf), "\e[%zu;%zuR",
					         wcp.wcp_row + 1, wcp.wcp_col + 1);
					for ( size_t n = 0; buf[n]; n++ )
					{
						if ( write(master_fd, &buf[n], 1) <= 0 )
						{
							warn("incoming write");
							break; // TODO: This break is incorrect.
						}
					}
				}
				continue;
			}
			for ( size_t j = 0; j < i; j++ )
			{
				if ( write(outgoing_fd, &getcursor[j], 1) <= 0 )
				{
					warn("outgoing write");
					break; // TODO: This break is incorrect.
				}
			}
			i = 0;
			if ( write(outgoing_fd, &c, 1) <= 0 )
			{
				warn("outgoing write");
				break;
			}
		}
	}
	else
	{
		while ( 0 < (amount = read(master_fd, &c, 1)) )
		{
			if ( write(outgoing_fd, &c, 1) <= 0 )
			{
				warn("outgoing write");
				break;
			}
		}
	}
	if ( amount < 0 )
		warn("outgoing read");
	exit(0);
	return NULL;
}

static char serial_getchar(int fd, const char* path)
{
	char c;
	ssize_t amount = read(fd, &c, 1);
	if ( amount < 0 )
		err(1, "read: %s", path);
	if ( amount == 0 )
		errx(1, "unexpected end of file: %s", path);
	return c;
}

static struct winsize serial_winsize(int fd, const char* path)
{
	dprintf(fd, "\e[18t");
	while ( true )
	{
		while ( serial_getchar(fd, path) != '\e' )
			continue;
		if ( serial_getchar(fd, path) != '[' )
			continue;
		unsigned int params[16];
		memset(&params, 0, sizeof(params));
		size_t current_param = 0;
		while ( true )
		{
			char c = serial_getchar(fd, path);
			if ( '0' <= c && c <= '9' )
			{
				params[current_param] = params[current_param] * 10 + c - '0';
				continue;
			}
			else if ( c == ';' )
			{
				if ( current_param < 16 )
					current_param++;
			}
			else if ( c == 't' )
			{
				if ( params[0] == 8 )
				{
					struct winsize ws;
					memset(&ws, 0, sizeof(ws));
					ws.ws_row = params[1];
					ws.ws_col = params[2];
					return ws;
				}
			}
			else
				break;
		}
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
	for ( int i = 1; i < argc; i++ )
	{
		const char* arg = argv[i];
		if ( arg[0] != '-' || !arg[1] )
			break;
		argv[i] = NULL;
		if ( !strcmp(arg, "--") )
			break;
		if ( arg[1] != '-' )
		{
			char c;
			while ( (c = *++arg) ) switch ( c )
			{
			case 's':
				if ( !*(serial = arg + 1) )
				{
					if ( i + 1 == argc )
						errx(125, "option requires an argument -- 's'");
					serial = argv[i+1];
					argv[++i] = NULL;
				}
				arg = "s";
				break;
			default:
				errx(1, "unknown option -- '%c'", c);
			}
		}
		else
			errx(1, "unknown option: %s", arg);
	}

	compact_arguments(&argc, &argv);

	main_pid = getpid();
	if ( atexit(exit_handler) )
		err(1, "atexit");

	struct winsize ws;
	memset(&ws, 0, sizeof(ws));
	if ( serial )
	{
		incoming_fd = open(serial, O_RDWR);
		if ( incoming_fd < 0 )
			err(1, "%s", serial);
		outgoing_fd = incoming_fd;
#if 0
		if ( 4 <= argc )
		{
			ws.ws_row = atoi(argv[2]);
			ws.ws_col = atoi(argv[3]);
		}
		else
#endif
			ws = serial_winsize(incoming_fd, serial);
	}
	else
	{
		if ( tcgetattr(0, &old_tio) )
			err(1, "tcgetattr");

		struct termios tio;
		memcpy(&tio, &old_tio, sizeof(struct termios));
		tio.c_iflag &= ~(BRKINT | ICRNL | IGNBRK | IGNCR | INLCR | IXANY | IXOFF | IXON);
		tio.c_oflag &= ~(OPOST); // TODO: Others.
		tio.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL | ICANON | IEXTEN | ISIG | NOFLSH);

		if ( tcsetattr(0, TCSAFLUSH, &tio) )
			err(1, "tcsetattr");
		has_old_tio = true;
		incoming_fd = 0;
		outgoing_fd = 1;

		if ( ioctl(incoming_fd, TIOCGWINSZ, &ws) < 0 )
			warn("ioctl: TIOCGWINSZ");
	}

	char path[TTY_NAME_MAX + 1];
	int slave_fd;
	if ( openpty(&master_fd, &slave_fd, path, NULL,
	             ws.ws_row && ws.ws_col ? &ws : NULL) < 0 )
		err(1, "openpty");

	pid_t child_pid = fork();
	if ( child_pid < 0 )
		err(1, "fork");

	if ( !child_pid )
	{
		if ( setsid() < 0 )
		{
			warn("setsid");
			_exit(1);
		}
		if ( ioctl(slave_fd, TIOCSCTTY) < 0 )
		{
			warn("ioctl: TIOCSCTTY");
			_exit(1);
		}
		if ( close(0) < 0 || close(1) < 0 || close(2) < 0 )
		{
			warn("close");
			_exit(1);
		}
		if ( dup2(slave_fd, 0) != 0 ||
		     dup2(slave_fd, 1) != 1 ||
		     dup2(slave_fd, 2) != 2 )
		{
			warn("dup");
			_exit(1);
		}
		if ( closefrom(3) < 0 )
		{
			warn("closefrom");
			_exit(1);
		}
		if ( argc <= 1 )
		{
			const char* program = "sh";
			execlp(program, program, (const char*) NULL);
		}
		else
		{
			execvp(argv[1], argv + 1);
		}
		_exit(127);
	}

	close(slave_fd);

	int errnum;
	pthread_t inthread;
	pthread_t outthread;
	if ( (errnum = pthread_create(&inthread, NULL, incoming_thread, NULL)) )
	{
		errno = errnum;
		err(1, "pthread_create");
	}
	if ( (errnum = pthread_create(&outthread, NULL, outgoing_thread, NULL)) )
	{
		errno = errnum;
		err(1, "pthread_create");
	}

	int status;
	waitpid(child_pid, &status, 0);
	exit(0);

	pthread_join(inthread, NULL);
	pthread_join(outthread, NULL);

	return 0;
}
