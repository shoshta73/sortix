/*
 * Copyright (c) 2012-2016, 2023, 2025 Jonas 'Sortie' Termansen.
 * Copyright (c) 2023 Juhani 'nortti' Krekelä.
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
 * chvideomode.c
 * Menu for changing the screen resolution.
 */

#include <sys/display.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include <assert.h>
#include <display.h>
#include <errno.h>
#include <err.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define REQUEST_DISPLAYS_ID 0
#define REQUEST_DISPLAY_MODES_ID 1
#define SET_DISPLAY_MODE_ID 2

static uint32_t display_id;
static bool displays_received = false;

static size_t modes_count = 0;
static struct dispmsg_crtc_mode* modes;
static int request_display_modes_error = 0;
static bool modes_received = false;

static int set_display_mode_error = 0;
static bool set_display_mode_ack_received;

static struct termios saved;

static void restore_terminal(int sig)
{
	if ( tcsetattr(0, TCSANOW, &saved) )
		err(1, "tcsetattr");
	// Re-raise the signal. As we set SA_RESETHAND this will not run the handler
	// again but rather fall back to default.
	raise(sig);
}

static void on_displays(void* ctx, uint32_t id, uint32_t displays)
{
	(void) ctx;
	if ( id != REQUEST_DISPLAYS_ID )
		return;
	if ( displays < 1 )
		errx(1, "No displays available");
	display_id = 0; // TODO: Multimonitor support.
	displays_received = true;
}

static void on_display_modes(void* ctx, uint32_t id,
                             uint32_t display_modes_count,
                             void* aux, size_t aux_size)
{
	(void) ctx;
	assert(display_modes_count * sizeof(struct dispmsg_crtc_mode) == aux_size);
	if ( id != REQUEST_DISPLAY_MODES_ID )
		return;
	modes = malloc(aux_size);
	if ( !modes )
		err(1, "malloc");
	memcpy(modes, aux, aux_size);
	modes_count = display_modes_count;
	modes_received = true;
}

static void on_ack(void* ctx, uint32_t id, int32_t error)
{
	(void) ctx;
	switch ( id )
	{
	case REQUEST_DISPLAY_MODES_ID:
		if ( error )
		{
			modes = NULL;
			request_display_modes_error = error;
			modes_received = true;
		}
		break;
	case SET_DISPLAY_MODE_ID:
		set_display_mode_error = error;
		set_display_mode_ack_received = true;
		break;
	}
}

static void request_displays(struct display_connection* connection)
{
	display_request_displays(connection, REQUEST_DISPLAYS_ID);
	struct display_event_handlers handlers = {0};
	handlers.displays_handler = on_displays;
	while ( !displays_received )
		display_wait_event(connection, &handlers);
}

static void request_display_modes(struct display_connection* connection,
                                  uint32_t display_id)
{
	display_request_display_modes(connection, REQUEST_DISPLAY_MODES_ID,
								  display_id);
	struct display_event_handlers handlers = {0};
	handlers.display_modes_handler = on_display_modes;
	handlers.ack_handler = on_ack;
	while ( !modes_received )
		display_wait_event(connection, &handlers);
	errno = request_display_modes_error;
}

static bool request_set_display_mode(struct display_connection* connection,
                                     uint32_t display_id,
                                     struct dispmsg_crtc_mode mode)
{
	display_set_display_mode(connection, SET_DISPLAY_MODE_ID, display_id, mode);
	struct display_event_handlers handlers = {0};
	handlers.ack_handler = on_ack;
	set_display_mode_ack_received = false;
	while ( !set_display_mode_ack_received )
		display_wait_event(connection, &handlers);
	return !(errno = set_display_mode_error);
}

static bool set_current_mode(const struct tiocgdisplay* display,
                             struct dispmsg_crtc_mode mode)
{
	struct dispmsg_set_crtc_mode msg;
	msg.msgid = DISPMSG_SET_CRTC_MODE;
	msg.device = display->device;
	msg.connector = display->connector;
	msg.mode = mode;
	return dispmsg_issue(&msg, sizeof(msg)) == 0;
}

static struct dispmsg_crtc_mode*
get_available_modes(const struct tiocgdisplay* display,
                    size_t* modes_count_ptr)
{
	struct dispmsg_get_crtc_modes msg;
	msg.msgid = DISPMSG_GET_CRTC_MODES;
	msg.device = display->device;
	msg.connector = display->connector;
	size_t guess = 1;
	while ( true )
	{
		struct dispmsg_crtc_mode* ret =
			calloc(guess, sizeof(struct dispmsg_crtc_mode));
		if ( !ret )
			return NULL;
		msg.modes_length = guess;
		msg.modes = ret;
		if ( dispmsg_issue(&msg, sizeof(msg)) == 0 )
		{
			*modes_count_ptr = guess;
			return ret;
		}
		free(ret);
		if ( errno == ERANGE && guess < msg.modes_length )
		{
			guess = msg.modes_length;
			continue;
		}
		return NULL;
	}
}

struct filter
{
	bool include_all;
	bool include_supported;
	bool include_unsupported;
	bool include_text;
	bool include_graphics;
	size_t minbpp;
	size_t maxbpp;
	size_t minxres;
	size_t maxxres;
	size_t minyres;
	size_t maxyres;
	size_t minxchars;
	size_t maxxchars;
	size_t minychars;
	size_t maxychars;
};

static bool mode_passes_filter(struct dispmsg_crtc_mode mode,
                               struct filter* filter)
{
	if ( filter->include_all )
		return true;
	size_t width = mode.view_xres;
	size_t height = mode.view_yres;
	size_t bpp = mode.fb_format;
	bool supported = (mode.control & DISPMSG_CONTROL_VALID) ||
	                 (mode.control & DISPMSG_CONTROL_OTHER_RESOLUTIONS);
	bool unsupported = !supported;
	bool text = mode.control & DISPMSG_CONTROL_VGA;
	bool graphics = !text;
	if ( mode.control & DISPMSG_CONTROL_OTHER_RESOLUTIONS )
		return true;
	if ( unsupported && !filter->include_unsupported )
		return false;
	if ( supported && !filter->include_supported )
		return false;
	if ( text && !filter->include_text )
		return false;
	if ( graphics && !filter->include_graphics )
		return false;
	if ( graphics && (bpp < filter->minbpp || filter->maxbpp < bpp) )
		return false;
	if ( graphics && (width < filter->minxres || filter->maxxres < width) )
		return false;
	if ( graphics && (height < filter->minyres || filter->maxyres < height) )
		return false;
	// TODO: Support filtering text modes according to columns/rows.
	return true;
}

static void filter_modes(struct dispmsg_crtc_mode* modes,
                         size_t* modes_count_ptr,
                         struct filter* filter)
{
	size_t in_count = *modes_count_ptr;
	size_t out_count = 0;
	for ( size_t i = 0; i < in_count; i++ )
	{
		if ( mode_passes_filter(modes[i], filter) )
			modes[out_count++] = modes[i];
	}
	*modes_count_ptr = out_count;
}

static bool get_mode(struct dispmsg_crtc_mode* modes,
                     size_t modes_count,
                     unsigned int xres,
                     unsigned int yres,
                     unsigned int bpp,
                     struct dispmsg_crtc_mode* mode)
{
	bool found = false;
	bool found_other = false;
	size_t index;
	size_t other_index = 0;
	for ( size_t i = 0; i < modes_count; i++ )
	{
		if ( modes[i].view_xres == xres &&
			 modes[i].view_yres == yres &&
			 modes[i].fb_format == bpp )
		{
			index = i;
			found = true;
			break;
		}
		if ( modes[i].control & DISPMSG_CONTROL_OTHER_RESOLUTIONS )
		{
			found_other = true;
			other_index = i;
		}
	}
	if ( !found )
	{
		if ( found_other )
			index = other_index;
		else
			// Not in the list of pre-set resolutions and setting a custom
			// resolution is not supported.
			return false;
	}

	*mode = modes[index];
	if ( mode->control & DISPMSG_CONTROL_OTHER_RESOLUTIONS )
	{
		mode->fb_format = bpp;
		mode->view_xres = xres;
		mode->view_yres = yres;
		mode->control &= ~DISPMSG_CONTROL_OTHER_RESOLUTIONS;
		mode->control |= DISPMSG_CONTROL_VALID;
	}

	return true;
}

static bool select_mode(struct dispmsg_crtc_mode* modes,
                        size_t modes_count,
                        int mode_set_error,
                        struct dispmsg_crtc_mode* mode)
{
	if ( !isatty(0) )
		errx(1, "Interactive menu requires stdin to be a terminal");

	int modes_count_display_length = 1;
	for ( size_t i = modes_count; 10 <= i; i /= 10 )
		modes_count_display_length++;

	size_t selection = 0;
	bool decided = false;
	while ( !decided )
	{
		fflush(stdout);

		struct winsize ws;
		if ( ioctl(1, TIOCGWINSZ, &ws) != 0 )
		{
			ws.ws_col = 80;
			ws.ws_row = 25;
		}

		size_t off = 1; // The "Please select ..." line at the top.
		if ( mode_set_error )
			off++;

		size_t entries_per_page = ws.ws_row - off;
		size_t page = selection / entries_per_page;
		size_t from = page * entries_per_page;
		size_t how_many_available = modes_count - from;
		size_t how_many = entries_per_page;
		if ( how_many_available < how_many )
			how_many = how_many_available;
		size_t lines_on_screen = off + how_many;

		printf("\e[m");
		printf("\e[2K");

		if ( mode_set_error )
			printf("Error: Could not set desired mode: %s\n",
			       strerror(mode_set_error));
		printf("Please select one of these video modes or press Q to abort.\n");

		for ( size_t i = 0; i < how_many; i++ )
		{
			size_t index = from + i;
			const char* color = index == selection ? "\e[31m" : "\e[m";
			printf("%s", color);
			printf("\e[2K");
			printf(" [%-*zu] ", modes_count_display_length, index);
			if ( modes[index].control & DISPMSG_CONTROL_VALID )
				printf("%ux%ux%u",
				       modes[index].view_xres,
				       modes[index].view_yres,
				       modes[index].fb_format);
			else if ( modes[index].control & DISPMSG_CONTROL_OTHER_RESOLUTIONS )
				printf("(enter a custom resolution)");
			else
				printf("(unknown video device feature)");
			printf("\e[m");
			if ( i + 1 < how_many )
				printf("\n");
		}

		printf("\e[J");
		fflush(stdout);

		// Block delivery of SIGTSTP during menu to avoid having to deal with
		// complex interactions between signals and terminal settings.
		sigset_t sigtstp;
		sigemptyset(&sigtstp);
		sigaddset(&sigtstp, SIGTSTP);
		sigprocmask(SIG_BLOCK, &sigtstp, NULL);

		if ( tcgetattr(0, &saved) )
			err(1, "tcgetattr");

		// Revert back to normal terminal settings before dying on a signal.
		struct sigaction sa = {0};
		sa.sa_handler = restore_terminal;
		sa.sa_flags = SA_RESETHAND; // The handler should only run once.
		sigaction(SIGINT, &sa, NULL);
		sigaction(SIGQUIT, &sa, NULL);
		sigaction(SIGTERM, &sa, NULL);

		struct termios altered = saved;
		altered.c_lflag &= ~(ECHO | ICANON);
		if ( tcsetattr(0, TCSANOW, &altered) )
			err(1, "tcsetattr");

		bool redraw = false;
		while ( !redraw && !decided )
		{
			int byte = fgetc(stdin);

			if ( byte == '\e' )
			{
				switch ( fgetc(stdin) )
				{
				case 'O': fgetc(stdin); break; // \eO is followed by one byte
				case '[':
				{
					// Sequence can have numbers separated by a semicolon before
					// the final character, so read until a non-digit
					// non-semicolon is found.
					size_t length = 1;
					while ( (byte = fgetc(stdin)) &&
					        (('0' <= byte && byte <= '9') || byte == ';' ) )
						length++;

					if ( length == 1 && byte == 'A' ) // Up key
					{
						if ( selection )
							selection--;
						else
							selection = modes_count - 1;
						redraw = true;
					}
					else if ( length == 1 && byte == 'B' ) // Down key
					{
						if ( selection + 1 == modes_count )
							selection = 0;
						else
							selection++;
						redraw = true;
					}
					break;
				}
				}
			}
			else if ( '0' <= byte && byte <= '9' )
			{
				uint32_t requested = byte - '0';
				if ( requested < modes_count )
				{
					selection = requested;
					redraw = true;
				}
			}
			else
			{
				switch ( byte )
				{
				case 'q':
				case 'Q':
					if ( tcsetattr(0, TCSANOW, &saved) )
						err(1, "tcsetattr");
					printf("\n");
					return false;
				case '\n':
					printf("\n");
					decided = true;
					break;
				}
			}
		}

		if ( redraw )
			printf("\e[%zuF", lines_on_screen - 1);

		if ( tcsetattr(0, TCSANOW, &saved) )
			err(1, "tcsetattr");
	}

	*mode = modes[selection];
	if ( mode->control & DISPMSG_CONTROL_OTHER_RESOLUTIONS )
	{
		uintmax_t req_bpp, req_width, req_height;
		while ( true )
		{
			printf("Enter video mode [WIDTHxHEIGHTxBPP]: ");
			fflush(stdout);
			if ( scanf("%jux%jux%ju", &req_width, &req_height, &req_bpp) != 3 )
			{
				fgetc(stdin);
				fflush(stdin);
				continue;
			}
			fgetc(stdin);
			break;
		}
		mode->fb_format = req_bpp;
		mode->view_xres = req_width;
		mode->view_yres = req_height;
		mode->control &= ~DISPMSG_CONTROL_OTHER_RESOLUTIONS;
		mode->control |= DISPMSG_CONTROL_VALID;
	}

	return true;
}

static size_t parse_size_t(const char* str)
{
	char* endptr;
	errno = 0;
	uintmax_t parsed = strtoumax(str, &endptr, 10);
	if ( !*str || *endptr )
		errx(1, "Invalid integer argument: %s", str);
	if ( errno == ERANGE || (size_t) parsed != parsed )
		errx(1, "Integer argument too large: %s", str);
	return (size_t) parsed;
}

static bool parse_bool(const char* str)
{
	if ( !strcmp(str, "0") || !strcmp(str, "false") )
		return false;
	if ( !strcmp(str, "1") || !strcmp(str, "true") )
		return true;
	errx(1, "Invalid boolean argument: %s", str);
}

enum longopt
{
	OPT_SHOW_ALL = 128,
	OPT_SHOW_SUPPORTED,
	OPT_SHOW_UNSUPPORTED,
	OPT_SHOW_TEXT,
	OPT_SHOW_GRAPHICS,
	OPT_BPP,
	OPT_MIN_BPP,
	OPT_MAX_BPP,
	OPT_WIDTH,
	OPT_MIN_WIDTH,
	OPT_MAX_WIDTH,
	OPT_HEIGHT,
	OPT_MIN_HEIGHT,
	OPT_MAX_HEIGHT,
};

int main(int argc, char* argv[])
{
	struct filter filter;

	filter.include_all = false;
	filter.include_supported = true;
	filter.include_unsupported = false;
	filter.include_text = true;
	filter.include_graphics = true;
	// TODO: HACK: The kernel log printing requires either text mode or 32-bit
	// graphics. For now, just filter away anything but 32-bit graphics.
	filter.minbpp = 32;
	filter.maxbpp = 32;
	filter.minxres = 0;
	filter.maxxres = SIZE_MAX;
	filter.minyres = 0;
	filter.maxyres = SIZE_MAX;
	filter.minxchars = 0;
	filter.maxxchars = SIZE_MAX;
	filter.minychars = 0;
	filter.maxychars = SIZE_MAX;

	const struct option longopts[] =
	{
		{"show-all", required_argument, NULL, OPT_SHOW_ALL},
		{"show-supported", required_argument, NULL, OPT_SHOW_SUPPORTED},
		{"show-unsupported", required_argument, NULL, OPT_SHOW_UNSUPPORTED},
		{"show-text", required_argument, NULL, OPT_SHOW_TEXT},
		{"show-graphics", required_argument, NULL, OPT_SHOW_GRAPHICS},
		{"bpp", required_argument, NULL, OPT_BPP},
		{"min-bpp", required_argument, NULL, OPT_MIN_BPP},
		{"max-bpp", required_argument, NULL, OPT_MAX_BPP},
		{"width", required_argument, NULL, OPT_WIDTH},
		{"min-width", required_argument, NULL, OPT_MIN_WIDTH},
		{"max-width", required_argument, NULL, OPT_MAX_WIDTH},
		{"height", required_argument, NULL, OPT_HEIGHT},
		{"min-height", required_argument, NULL, OPT_MIN_HEIGHT},
		{"max-height", required_argument, NULL, OPT_MAX_HEIGHT},
		{0, 0, 0, 0}
	};
	const char* opts = "";
	int opt;
	while ( (opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1 )
	{
		switch (opt)
		{
		case OPT_SHOW_ALL: filter.include_all = parse_bool(optarg); break;
		case OPT_SHOW_SUPPORTED:
			filter.include_supported = parse_bool(optarg); break;
		case OPT_SHOW_UNSUPPORTED:
			filter.include_unsupported = parse_bool(optarg); break;
		case OPT_SHOW_TEXT: filter.include_text = parse_bool(optarg); break;
		case OPT_SHOW_GRAPHICS:
			filter.include_graphics = parse_bool(optarg); break;
		case OPT_BPP:
			filter.minbpp = filter.maxbpp = parse_size_t(optarg); break;
		case OPT_MIN_BPP: filter.minbpp = parse_size_t(optarg); break;
		case OPT_MAX_BPP: filter.maxbpp = parse_size_t(optarg); break;
		case OPT_WIDTH:
			filter.minxres = filter.maxxres = parse_size_t(optarg); break;
		case OPT_MIN_WIDTH: filter.minxres = parse_size_t(optarg); break;
		case OPT_MAX_WIDTH: filter.maxxres = parse_size_t(optarg); break;
		case OPT_HEIGHT:
			filter.minyres = filter.maxyres = parse_size_t(optarg); break;
		case OPT_MIN_HEIGHT: filter.minyres = parse_size_t(optarg); break;
		case OPT_MAX_HEIGHT: filter.maxyres = parse_size_t(optarg); break;
		default: return 1;
		}
	}

	bool use_display = getenv("DISPLAY_SOCKET");

	struct display_connection* connection = NULL;
	struct tiocgdisplay display;
	if ( use_display )
	{
		connection = display_connect_default();
		if ( !connection )
			err(1, "Could not connect to display server");
		request_displays(connection);
		request_display_modes(connection, display_id);
	}
	else
	{
		struct tiocgdisplays gdisplays = {0};
		// TODO: Multimonitor support.
		gdisplays.count = 1;
		gdisplays.displays = &display;
		if ( ioctl(1, TIOCGDISPLAYS, &gdisplays) < 0 || gdisplays.count == 0 )
		{
			fprintf(stderr, "No displays associated with this terminal.\n");
			exit(13);
		}

		modes = get_available_modes(&display, &modes_count);
	}

	if ( !modes )
		err(1, "Unable to detect available video modes");

	if ( !modes_count )
	{
		fprintf(stderr, "No video modes are currently available.\n");
		fprintf(stderr, "Try make sure a device driver exists and is "
		                "activated.\n");
		exit(11);
	}

	filter_modes(modes, &modes_count, &filter);
	if ( !modes_count )
	{
		fprintf(stderr, "No video mode remains after filtering away unwanted "
		                "modes.\n");
		fprintf(stderr, "Try make sure the desired device driver is loaded and "
		                "is configured correctly.\n");
		exit(12);
	}

	if ( 1 < argc - optind )
		errx(1, "Unexpected extra operand");
	else if ( argc - optind == 1 )
	{
		unsigned int xres, yres, bpp;
		if ( sscanf(argv[optind], "%ux%ux%u", &xres, &yres, &bpp) != 3 )
			errx(1, "Invalid video mode: %s", argv[optind]);

		struct dispmsg_crtc_mode mode;
		if ( !get_mode(modes, modes_count, xres, yres, bpp, &mode) )
			errx(1, "No such available resolution: %s", argv[optind]);

		bool mode_set;
		if ( use_display )
			mode_set = request_set_display_mode(connection, display_id, mode);
		else
			mode_set = set_current_mode(&display, mode);
		if ( !mode_set )
			err(1, "Failed to set video mode %jux%jux%ju",
			    (uintmax_t) mode.view_xres,
			    (uintmax_t) mode.view_yres,
			    (uintmax_t) mode.fb_format);
	}
	else
	{
		int mode_set_error = 0;
		bool mode_set = false;
		while ( !mode_set )
		{
			struct dispmsg_crtc_mode mode;
			if ( !select_mode(modes, modes_count, mode_set_error, &mode) )
				exit(10);

			if ( use_display )
				mode_set = request_set_display_mode(connection, display_id,
				                                    mode);
			else
				mode_set = set_current_mode(&display, mode);
			if ( !mode_set )
			{
				mode_set_error = errno;
				warn("Failed to set video mode %jux%jux%ju",
				     (uintmax_t) mode.view_xres,
				     (uintmax_t) mode.view_yres,
				     (uintmax_t) mode.fb_format);
			}
		}
	}

	if ( use_display )
		display_disconnect(connection);

	return 0;
}
