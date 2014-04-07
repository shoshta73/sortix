/*******************************************************************************

    Copyright(C) Brian Paul 1999, 2000, 2001.

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    BRIAN PAUL BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
    AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
    CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

    gears.cpp
    Classic OpenGL test program that render colored rotating gears.

*******************************************************************************/

#include <sys/keycodes.h>
#include <sys/termmode.h>

#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <timespec.h>
#include <unistd.h>

#include <display.h>

#include <GL/gl.h>
#include <GL/osmesa.h>

struct context
{
	uint32_t window_id;
	uint32_t window_width;
	uint32_t window_height;
	bool need_exit;
	bool key_a;
	bool key_c;
	bool key_d;
	bool key_e;
	bool key_q;
	bool key_s;
	bool key_w;
	bool key_up;
	bool key_down;
	bool key_left;
	bool key_right;
	bool key_pgup;
	bool key_pgdown;
};

void on_disconnect(void* context_pointer)
{
	struct context* context = (struct context*) context_pointer;
	context->need_exit = true;
}

void on_quit(void* context_pointer, uint32_t window_id)
{
	struct context* context = (struct context*) context_pointer;
	if ( window_id != context->window_id )
		return;
	context->need_exit = true;
}

void on_resize(void* context_pointer, uint32_t window_id, uint32_t width, uint32_t height)
{
	struct context* context = (struct context*) context_pointer;
	if ( window_id != context->window_id )
		return;
	context->window_width = width;
	context->window_height = height;
}

void on_keyboard(void* context_pointer, uint32_t window_id, uint32_t codepoint)
{
	struct context* context = (struct context*) context_pointer;
	if ( window_id != context->window_id )
		return;
	int kbkey = KBKEY_DECODE(codepoint);
	if ( !kbkey )
		return;
	unsigned int abskbkey = kbkey < 0 ? - (unsigned int) kbkey :
	                                      (unsigned int) kbkey;
	switch ( abskbkey )
	{
	case KBKEY_A: context->key_a = 0 < kbkey; break;
	case KBKEY_C: context->key_c = 0 < kbkey; break;
	case KBKEY_D: context->key_d = 0 < kbkey; break;
	case KBKEY_E: context->key_e = 0 < kbkey; break;
	case KBKEY_Q: context->key_q = 0 < kbkey; break;
	case KBKEY_S: context->key_s = 0 < kbkey; break;
	case KBKEY_W: context->key_w = 0 < kbkey; break;
	case KBKEY_UP: context->key_up = 0 < kbkey; break;
	case KBKEY_DOWN: context->key_down = 0 < kbkey; break;
	case KBKEY_LEFT: context->key_left = 0 < kbkey; break;
	case KBKEY_RIGHT: context->key_right = 0 < kbkey; break;
	case KBKEY_PGUP: context->key_pgup = 0 < kbkey; break;
	case KBKEY_PGDOWN: context->key_pgdown = 0 < kbkey; break;
	default: break;
	}
}

static void gear(GLfloat inner_radius, GLfloat outer_radius, GLfloat width,
                 GLint teeth, GLfloat tooth_depth)
{
	GLint i;
	GLfloat r0, r1, r2;
	GLfloat angle, da;
	GLfloat u, v, len;

	r0 = inner_radius;
	r1 = outer_radius - tooth_depth / 2.0;
	r2 = outer_radius + tooth_depth / 2.0;

	da = 2.0 * M_PI / teeth / 4.0;

	glShadeModel(GL_FLAT);

	glNormal3f(0.0, 0.0, 1.0);

	// Draw the front face.
	glBegin(GL_QUAD_STRIP);
	for ( i = 0; i <= teeth; i++ )
	{
		angle = i * 2.0 * M_PI / teeth;
		glVertex3f(r0 * cos(angle), r0 * sin(angle), width * 0.5);
		glVertex3f(r1 * cos(angle), r1 * sin(angle), width * 0.5);
		if ( i < teeth )
		{
			glVertex3f(r0 * cos(angle), r0 * sin(angle), width * 0.5);
			glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da),
			           width * 0.5);
		}
	}
	glEnd();

	// Draw the front sides of teeth.
	glBegin(GL_QUADS);
	da = 2.0 * M_PI / teeth / 4.0;
	for ( i = 0; i < teeth; i++ )
	{
		angle = i * 2.0 * M_PI / teeth;
		glVertex3f(r1 * cos(angle), r1 * sin(angle), width * 0.5);
		glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), width * 0.5);
		glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da),
		           width * 0.5);
		glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da),
		           width * 0.5);
	}
	glEnd();

	glNormal3f(0.0, 0.0, -1.0);

	// Draw the back face.
	glBegin(GL_QUAD_STRIP);
	for ( i = 0; i <= teeth; i++ )
	{
		angle = i * 2.0 * M_PI / teeth;
		glVertex3f(r1 * cos(angle), r1 * sin(angle), -width * 0.5);
		glVertex3f(r0 * cos(angle), r0 * sin(angle), -width * 0.5);
		if ( i < teeth )
		{
			glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da),
			           -width * 0.5);
			glVertex3f(r0 * cos(angle), r0 * sin(angle), -width * 0.5);
		}
	}
	glEnd();

	// Draw the back sides of teeth.
	glBegin(GL_QUADS);
	da = 2.0 * M_PI / teeth / 4.0;
	for ( i = 0; i < teeth; i++ )
	{
		angle = i * 2.0 * M_PI / teeth;

		glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da),
		           -width * 0.5);
		glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da),
		           -width * 0.5);
		glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), -width * 0.5);
		glVertex3f(r1 * cos(angle), r1 * sin(angle), -width * 0.5);
	}
	glEnd();

	// Draw the outward faces of teeth.
	glBegin(GL_QUAD_STRIP);
	for (i = 0; i < teeth; i++)
	{
		angle = i * 2.0 * M_PI / teeth;

		glVertex3f(r1 * cos(angle), r1 * sin(angle), width * 0.5);
		glVertex3f(r1 * cos(angle), r1 * sin(angle), -width * 0.5);
		u = r2 * cos(angle + da) - r1 * cos(angle);
		v = r2 * sin(angle + da) - r1 * sin(angle);
		len = sqrt(u * u + v * v);
		u /= len;
		v /= len;
		glNormal3f(v, -u, 0.0);
		glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), width * 0.5);
		glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), -width * 0.5);
		glNormal3f(cos(angle), sin(angle), 0.0);
		glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da),
		           width * 0.5);
		glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da),
		           -width * 0.5);
		u = r1 * cos(angle + 3 * da) - r2 * cos(angle + 2 * da);
		v = r1 * sin(angle + 3 * da) - r2 * sin(angle + 2 * da);
		glNormal3f(v, -u, 0.0);
		glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da),
		           width * 0.5);
		glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da),
		           -width * 0.5);
		glNormal3f(cos(angle), sin(angle), 0.0);
	}

	glVertex3f(r1 * cos(0), r1 * sin(0), width * 0.5);
	glVertex3f(r1 * cos(0), r1 * sin(0), -width * 0.5);

	glEnd();

	glShadeModel(GL_SMOOTH);

	// Draw the inside radius cylinder.
	glBegin(GL_QUAD_STRIP);
	for ( i = 0; i <= teeth; i++ )
	{
		angle = i * 2.0 * M_PI / teeth;
		glNormal3f(-cos(angle), -sin(angle), 0.0);
		glVertex3f(r0 * cos(angle), r0 * sin(angle), -width * 0.5);
		glVertex3f(r0 * cos(angle), r0 * sin(angle), width * 0.5);
	}
	glEnd();
}

static GLfloat view_rotx = 20.0, view_roty = 30.0, view_rotz = 0.0;
static GLint gear1, gear2, gear3;
static GLfloat angle = 0.0;

static void draw(int width, int height)
{
	GLfloat h = (GLfloat) height / (GLfloat) width;

	glViewport(0, 0, (GLint) width, (GLint) height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-1.0, 1.0, -h, h, 5.0, 60.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(0.0, 0.0, -40.0);

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glPushMatrix();
	glRotatef(view_rotx, 1.0, 0.0, 0.0);
	glRotatef(view_roty, 0.0, 1.0, 0.0);
	glRotatef(view_rotz, 0.0, 0.0, 1.0);

	glPushMatrix();
	glTranslatef(-3.0, -2.0, 0.0);
	glRotatef(angle, 0.0, 0.0, 1.0);
	glCallList(gear1);
	glPopMatrix();

	glPushMatrix();
	glTranslatef(3.1, -2.0, 0.0);
	glRotatef(-2.0 * angle - 9.0, 0.0, 0.0, 1.0);
	glCallList(gear2);
	glPopMatrix();

	glPushMatrix();
	glTranslatef(-3.1, 4.2, 0.0);
	glRotatef(-2.0 * angle - 25.0, 0.0, 0.0, 1.0);
	glCallList(gear3);
	glPopMatrix();

	glPopMatrix();
}

static void init()
{
	static GLfloat pos[4] = { 5.0, 5.0, 10.0, 0.0 };
	static GLfloat red[4] = { 0.8, 0.1, 0.0, 1.0 };
	static GLfloat green[4] = { 0.0, 0.8, 0.2, 1.0 };
	static GLfloat blue[4] = { 0.2, 0.2, 1.0, 1.0 };

	glLightfv(GL_LIGHT0, GL_POSITION, pos);
	glEnable(GL_CULL_FACE);
	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glEnable(GL_DEPTH_TEST);

	// Make the gears.
	gear1 = glGenLists(1);
	glNewList(gear1, GL_COMPILE);
	glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, red);
	gear(1.0, 4.0, 1.0, 20, 0.7);
	glEndList();

	gear2 = glGenLists(1);
	glNewList(gear2, GL_COMPILE);
	glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, green);
	gear(0.5, 2.0, 2.0, 10, 0.7);
	glEndList();

	gear3 = glGenLists(1);
	glNewList(gear3, GL_COMPILE);
	glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, blue);
	gear(1.3, 2.0, 0.5, 10, 0.7);
	glEndList();

	glEnable(GL_NORMALIZE);
}

int main(int argc, char* argv[])
{
	(void) argc;
	(void) argv;

	struct display_connection* connection = display_connect_default();
	if ( !connection && errno == ECONNREFUSED )
		display_spawn(argc, argv);
	if ( !connection )
		error(1, errno, "Could not connect to display server");

	struct context context;
	memset(&context, 0, sizeof(context));
	context.window_id = 0;
	context.window_width = 600;
	context.window_height = 600;

	display_create_window(connection, context.window_id);
	display_resize_window(connection, context.window_id, context.window_width, context.window_height);
	display_title_window(connection, context.window_id, "Gears");

	uint32_t width = context.window_width;
	uint32_t height = context.window_height;

	uint32_t* framebuffer = new uint32_t[context.window_width * context.window_height];
	memset(framebuffer, 0, sizeof(framebuffer[0]) * context.window_width * context.window_height);

	OSMesaContext gl_ctx = OSMesaCreateContext(OSMESA_BGRA, NULL);

	struct timespec start_ts;
	struct timespec last_ts = timespec_make(0, 0);
	clock_gettime(CLOCK_MONOTONIC, &start_ts);

	bool first_frame = true;
	while ( !context.need_exit )
	{
		struct timespec now_ts;
		clock_gettime(CLOCK_MONOTONIC, &now_ts);
		struct timespec current_ts = timespec_sub(now_ts, start_ts);
		float current_time = current_ts.tv_sec + current_ts.tv_nsec / 1000000000.0;
		struct timespec delta_ts = timespec_sub(current_ts, last_ts);
		float delta_time = delta_ts.tv_sec + delta_ts.tv_nsec / 1000000000.0;

		if ( width != context.window_width || height != context.window_height )
		{
			delete[] framebuffer;
			framebuffer = new uint32_t[context.window_width * context.window_height];
			memset(framebuffer, 0, sizeof(framebuffer[0]) * context.window_width * context.window_height);
			width = context.window_width;
			height = context.window_height;
		}

		if ( !OSMesaMakeCurrent(gl_ctx, (uint8_t*) framebuffer, GL_UNSIGNED_BYTE,
		                        width, height) )
			error(1, errno, "`OSMesaMakeCurrent'");

		angle = current_time * 100;

		if ( context.key_a || context.key_left )
			view_roty += 90.0 * delta_time;
		if ( context.key_d || context.key_right )
			view_roty -= 90.0 * delta_time;
		if ( context.key_w || context.key_up )
			view_rotx += 90.0 * delta_time;
		if ( context.key_s || context.key_down )
			view_rotx -= 90.0 * delta_time;
		if ( context.key_q || context.key_pgup )
			view_rotz += 90.0 * delta_time;
		if ( context.key_e || context.key_pgdown )
			view_rotz -= 90.0 * delta_time;
		if ( context.key_c )
			context.need_exit = true;

		if ( first_frame )
			init();

		draw(width, height);

		display_render_window(connection, context.window_id, 0, 0, context.window_width, context.window_height, framebuffer);

		if ( first_frame )
			display_show_window(connection, context.window_id);

		last_ts = current_ts;

		first_frame = false;

		struct display_event_handlers handlers;
		memset(&handlers, 0, sizeof(handlers));
		handlers.context = &context;
		handlers.disconnect_handler = on_disconnect;
		handlers.quit_handler = on_quit;
		handlers.resize_handler = on_resize;
		handlers.keyboard_handler = on_keyboard;
		while ( display_poll_event(connection, &handlers) == 0 );
	}

	delete[] framebuffer;

	OSMesaDestroyContext(gl_ctx);

	display_disconnect(connection);

	return 0;
}
