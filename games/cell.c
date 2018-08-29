/*
 * Copyright (c) 2018 Jonas 'Sortie' Termansen.
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
 * aquatinspitz.c
 * Aqua tin spitz!
 */

#ifndef DUMP
#include <sys/keycodes.h>
#include <sys/termmode.h>
#endif
#include <sys/types.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <error.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#ifndef DUMP
#include <timespec.h>
#endif
#include <unistd.h>

#ifdef DUMP
#include <stdio.h>
#endif

#ifndef DUMP
#include <dispd.h>
#endif

#if defined(__linux__)
#include <fcntl.h>
static uint32_t arc4random(void)
{
	static int fd = -1;
	if ( fd < 0 )
		fd = open("/dev/urandom", O_RDONLY);
	if ( fd < 0 )
		err(1, "/dev/urandom");
	uint32_t value;
	if ( read(fd, &value, sizeof(value)) != sizeof(value) )
		err(1, "read: /dev/urandom");
	return value;
}
#endif

// Utility global variables every game will need.
#ifndef DUMP
static bool game_running = true;
#else
static volatile bool game_running = true;
#endif
#ifndef DUMP
#define MAX_KEY_NUMBER 512
static bool keys_down[MAX_KEY_NUMBER];
static bool keys_pending[MAX_KEY_NUMBER];
//static struct timespec key_handled_last[MAX_KEY_NUMBER];

// Utility functions every game will need.
//bool pop_is_key_just_down(int abskbkey);
#endif

// Your game is customized from here ...

static size_t data_width = 0;
static size_t data_height = 0;
static uint32_t* data;

static inline uint32_t make_color(uint8_t r, uint8_t g, uint8_t b)
{
	return b << 0UL | g << 8UL | r << 16UL;
}

static inline uint8_t get_channel(uint32_t cell, uint32_t channel)
{
	return cell >> (8 * channel) & 0xff;
}

static inline uint32_t set_channel(uint32_t cell, uint32_t channel, uint8_t value)
{
	uint32_t mask = ~(0xFFU << (channel * 8U));
	return (cell & mask) | ((uint32_t) value << (8U * channel));
}

#if 0
static size_t measure_energy(void)
{
	size_t count = 0;
	for ( size_t y = 0; y < data_height; y++ )
	{
		for ( size_t x = 0; x < data_width; x++ )
		{
			uint32_t cell = data[y * data_width + x];
			count += get_channel(cell, 0);
			count += get_channel(cell, 1);
			count += get_channel(cell, 2);
			count += get_channel(cell, 3);
		}
	}
	return count;
	return 0;
}
#endif

// Calculate the game state of the next round.
static void update(void)
{
#if 0
	size_t energy = measure_energy();
#endif
	for ( size_t y = 0; y < data_height; y++ )
	{
		for ( size_t x = 0; x < data_width; x++ )
		{
			uint32_t cell = data[y * data_width + x];
			uint8_t value_d = get_channel(cell, 3);
			for ( int channel_a = 0; channel_a < 3; channel_a++ )
			{
				int channel_b = (channel_a + 1) % 3;
				int channel_c = (channel_a + 2) % 3;
				uint8_t value_a = get_channel(cell, channel_a);
				//if ( value_d && 0 < value_a && value_a < 255 )
				if ( value_d < 255 && value_a == 1 )
				{
					//value_d--;
					//value_a++;
					value_a = value_d;
					value_d = 0;
					cell = set_channel(cell, channel_a, value_a);
					cell = set_channel(cell, 3, value_d);
#if 0
					data[y * data_width + x] = cell;
					assert(energy == measure_energy());
#endif
				}
				uint8_t value_b = get_channel(cell, channel_b);
				uint8_t value_c = get_channel(cell, channel_c);
				if ( value_a &&
				     (value_b || value_c) &&
				     (value_a >= value_b && value_a >= value_c) )
				{
					uint8_t value_e = value_b > value_c ? value_b : value_c;
					uint8_t channel_e = value_b > value_c ? channel_b : channel_c;
					uint8_t max = 255 - value_d;
					uint8_t dam = max < value_e ? max : value_e;
					value_d += dam;
					value_e -= dam;
					cell = set_channel(cell, 3, value_d);
					cell = set_channel(cell, channel_e, value_e);
#if 0
					data[y * data_width + x] = cell;
					assert(energy == measure_energy());
#endif
				}
			}
			uint32_t other_number = 0;
			for ( int offset_y = -1; offset_y <= 1; offset_y++ )
			{
				size_t other_y = offset_y == -1 && y == 0 ? data_height - 1 :
				                 offset_y == 1 && y + 1 == data_height ? 0 :
				                 y + offset_y;
				for ( int offset_x = -1; offset_x <= 1; offset_x++ )
				{
					if ( offset_x == 0 && offset_y == 0 )
						continue;
					other_number++;
					size_t other_x = offset_x == -1 && x == 0 ? data_width - 1 :
					                 offset_x == 1 && x + 1 == data_width ? 0 :
					                 x + offset_x;
					uint32_t other = data[other_y * data_width + other_x];
					uint8_t cell_d = get_channel(cell, 3);
					uint8_t other_d = get_channel(other, 3);
					if ( other_d > cell_d )
					{
						uint32_t divisor = 8 - (other_number - 1);
						uint8_t available = (other_d - cell_d + (divisor - 1)) / divisor;
						uint8_t possible = 255 - cell_d;
						uint8_t portion = available < possible ? available : possible;
						cell_d += portion;
						other_d -= portion;
						cell = set_channel(cell, 3, cell_d);
						other = set_channel(other, 3, other_d);
#if 0
						data[other_y * data_width + other_x] = other;
						data[y * data_width + x] = cell;
						assert(energy == measure_energy());
#endif
					}
					for ( int channel_a = 0; channel_a < 3; channel_a++ )
					{
						uint8_t cell_a = get_channel(cell, channel_a);
						if ( !cell_a )
							continue;
						uint8_t other_a = get_channel(other, channel_a);
						for ( int channel_bi = 1; channel_bi < 3; channel_bi++ )
						{
							int channel_b = (channel_a + channel_bi) % 3;
							uint8_t other_b = get_channel(other, channel_b);
							if ( other_a && !other_b )
								continue;
							int might = cell_a + other_a;
							if ( 255 < might )
								might = 255;
							if ( 1 < cell_a && other_b < might )
							{
								uint8_t max = 255 - other_a;
								uint8_t can = cell_a - 1;
								uint8_t transfer = max < can ? max : can;
								if ( transfer )
								{
									assert(transfer <= cell_a);
									assert(transfer + other_a < 256);
									cell_a -= transfer;
									other_a += transfer;
									cell = set_channel(cell, channel_a, cell_a);
									other = set_channel(other, channel_a, other_a);
#if 0
									data[other_y * data_width + other_x] = other;
									data[y * data_width + x] = cell;
									assert(energy == measure_energy());
#endif
								}
							}
						}
						if ( 2 <= cell_a && other_d > cell_d )
						{
								uint8_t available = cell_a - 1;
								uint8_t possible = 255 - other_a;
								uint8_t portion = available < possible ? available : possible;
								cell_a -= portion;
								other_a += portion;
								cell = set_channel(cell, channel_a, cell_a);
								other = set_channel(other, channel_a, other_a);
#if 0
								data[other_y * data_width + other_x] = other;
								data[y * data_width + x] = cell;
								assert(energy == measure_energy());
#endif
						}
						else if ( 2 <= cell_a )
						{
#if 0
								uint8_t available = (other_a - cell_a + 15) / 16;
								uint8_t possible = 255 - other_a;
								uint8_t portion = available < possible ? available : possible;
								cell_a -= portion;
								other_a += portion;
								cell = set_channel(cell, channel_a, cell_a);
								other = set_channel(other, channel_a, other_a);
#if 1
								data[other_y * data_width + other_x] = other;
								data[y * data_width + x] = cell;
								assert(energy == measure_energy());
#endif
#endif
						}
#if 0
						uint8_t cell_a = get_channel(cell, channel_a);
						uint8_t other_a = get_channel(other, channel_a);
						if ( cell_a > other_a )
						{
							for ( int i = 7; i >= 0; i-- )
							{
								int mag = 1 << i;
								if ( cell_a > mag && mag <= 255 - other_a && cell_a - other_a > 2 * mag )
								{
									cell_a -= mag;
									other_a += mag;
									cell = set_channel(cell, channel_a, cell_a);
									other = set_channel(other, channel_a, other_a);
#if 1
									data[other_y * data_width + other_x] = other;
									data[y * data_width + x] = cell;
									assert(energy == measure_energy());
#endif
									break;
								}
							}
						}
#endif
					}
					data[other_y * data_width + other_x] = other;
#if 0
					assert(energy == measure_energy());
#endif
				}
			}
			data[y * data_width + x] = cell;
#if 0
			assert(energy == measure_energy());
#endif
		}
	}
}

// Render the game into the framebuffer.
#ifndef DUMP
static void render(struct dispd_window* window)
#else
static void render(void)
#endif
{
#ifndef DUMP
	struct dispd_framebuffer* window_fb = dispd_begin_render(window);
	if ( !window_fb )
	{
		error(0, 0, "unable to begin rendering dispd window");
		game_running = false;
		return;
	}

	uint32_t* fb = (uint32_t*) dispd_get_framebuffer_data(window_fb);
	size_t xres = dispd_get_framebuffer_width(window_fb);
	size_t yres = dispd_get_framebuffer_height(window_fb);
	size_t pitch = dispd_get_framebuffer_pitch(window_fb) / sizeof(uint32_t);
#else
	size_t xres = DUMP_XRES;
	size_t yres = DUMP_YRES;
#endif

	if ( data_width != xres || data_height != yres )
	{
		uint32_t* new_data =
			(uint32_t*) malloc(sizeof(uint32_t) * xres * yres);
		if ( !new_data )
			err(1, "malloc");
		for ( size_t y = 0; y < data_height; y++ )
		{
			for ( size_t x = 0; x < data_width; x++ )
			{
				uint32_t* output = new_data + y * xres + x;
				uint32_t* in = data + y * data_width + x;
				*output = *in;
			}
			for ( size_t x = data_width; x < xres; x++ )
			{
				uint32_t* output = new_data + y * xres + x;
				*output = arc4random() & 0xFFFFFF;
			}
		}
		for ( size_t y = data_height; y < yres; y++ )
		{
			for ( size_t x = 0; x < xres; x++ )
			{
				uint32_t* output = new_data + y * xres + x;
				*output = arc4random() & 0xFFFFFF;
			}
		}
		data = new_data;
		data_width = xres;
		data_height = yres;
	}

#ifndef DUMP
	for ( size_t y = 0; y < yres; y++ )
	{
		for ( size_t x = 0; x < xres; x++ )
		{
			if ( keys_down[KBKEY_X] )
				fb[y * pitch + x] = data[y * data_width + x] >> 24;
			else
				fb[y * pitch + x] = data[y * data_width + x];
		}
	}

	dispd_finish_render(window_fb);
#else
	fwrite(data, sizeof(uint32_t), xres * yres, stdout);
#endif
}

// ... to here. No need to edit stuff below.

#if 0
// Return if a keystroke is pending. For instance, if you press A on your
// keyboard and keep pressing it, a new A character will appear every time a
// small interval has passed, not just every time the code checks if A is down.
static bool pop_is_key_just_down(int abskbkey)
{
	assert(0 <= abskbkey);
	if ( MAX_KEY_NUMBER <= (size_t) abskbkey )
		return false;
	if ( keys_pending[abskbkey] )
	{
		keys_pending[abskbkey] = false;
		clock_gettime(CLOCK_MONOTONIC, &key_handled_last[abskbkey]);
		return true;
	}
	if ( !keys_down[abskbkey] )
		return false;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	struct timespec elapsed = timespec_sub(now, key_handled_last[abskbkey]);
	struct timespec repress_delay = timespec_make(0, 100 * 1000 * 1000);
	if ( timespec_lt(elapsed, repress_delay) )
		return false;
	clock_gettime(CLOCK_MONOTONIC, &key_handled_last[abskbkey]);
	return true;
}
#endif

// Read input from the keyboard.
static void input(void)
{
#ifndef DUMP
	// Read the keyboard input from the user.
	unsigned termmode = TERMMODE_KBKEY | TERMMODE_SIGNAL | TERMMODE_NONBLOCK;
	if ( settermmode(0, termmode) )
		error(1, errno, "settermmode");
	uint32_t codepoint;
	ssize_t numbytes;
	while ( 0 < (numbytes = read(0, &codepoint, sizeof(codepoint))) )
	{
		int kbkey = KBKEY_DECODE(codepoint);
		if( !kbkey )
			continue;
		int abskbkey = (kbkey < 0) ? -kbkey : kbkey;
		if ( MAX_KEY_NUMBER <= (size_t) abskbkey )
			continue;
		bool is_key_down_event = 0 < kbkey;
		if ( !keys_down[abskbkey] && is_key_down_event )
			keys_pending[abskbkey] = true;
		keys_down[abskbkey] = is_key_down_event;
	}
#endif
}

// Run the game until no longer needed.
#ifndef DUMP
static void mainloop(struct dispd_window* window)
#else
static void mainloop(void)
#endif
{
	//struct timespec last_frame_time;
	//clock_gettime(CLOCK_MONOTONIC, &last_frame_time);

#ifndef DUMP
	render(window);
#else
	render();
#endif

	while ( game_running )
	{
		//struct timespec current_frame_time;
		//clock_gettime(CLOCK_MONOTONIC, &current_frame_time);

		//struct timespec deltatime_ts =
		//	timespec_sub(current_frame_time, last_frame_time);
		//float deltatime = deltatime_ts.tv_sec + deltatime_ts.tv_nsec / 1E9f;

		input();
		//update(deltatime);
		update();
#ifndef DUMP
		render(window);
#else
		render();
#endif

		//last_frame_time = current_frame_time;
	}
}

// Reset the terminal state when the process terminates.
static struct termios saved_tio;

static void restore_terminal_on_exit(void)
{
	tcsetattr(0, TCSAFLUSH, &saved_tio);
}

static void restore_terminal_on_signal(int signum)
{
	if ( signum == SIGTSTP )
	{
		struct termios tio;
		tcgetattr(0, &tio);
		tcsetattr(0, TCSAFLUSH, &saved_tio);
		raise(SIGSTOP);
		tcgetattr(0, &saved_tio);
		tcsetattr(0, TCSAFLUSH, &tio);
		return;
	}
	tcsetattr(0, TCSAFLUSH, &saved_tio);
	raise(signum);
}

#ifdef DUMP
void on_signal(int signum)
{
	(void) signum;
	game_running = false;
}
#endif

// Create a display context, run the game, and then cleanly exit.
int main(int argc, char* argv[])
{
	if ( !isatty(0) )
		error(1, errno, "standard input");
	if ( tcgetattr(0, &saved_tio) < 0 )
		error(1, errno, "tcsetattr: standard input");
	if ( atexit(restore_terminal_on_exit) != 0 )
		error(1, errno, "atexit");
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = restore_terminal_on_signal;
	sigaction(SIGTSTP, &sa, NULL);
	sa.sa_flags = SA_RESETHAND;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

#ifndef DUMP
	if ( !dispd_initialize(&argc, &argv) )
		error(1, 0, "couldn't initialize dispd library");
	struct dispd_session* session = dispd_attach_default_session();
	if ( !session )
		error(1, 0, "couldn't attach to dispd default session");
	if ( !dispd_session_setup_game_rgba(session) )
		error(1, 0, "couldn't setup dispd rgba session");
	struct dispd_window* window = dispd_create_window_game_rgba(session);
	if ( !window )
		error(1, 0, "couldn't create dispd rgba window");

	mainloop(window);

	dispd_destroy_window(window);
	dispd_detach_session(session);
#else
	signal(SIGINT, on_signal);
	signal(SIGQUIT, on_signal);
	(void) argc;
	(void) argv;
	mainloop();
#endif

	return 0;
}
