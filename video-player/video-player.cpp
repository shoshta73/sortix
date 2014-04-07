#define __STDC_CONSTANT_MACROS
#define __STDC_LIMIT_MACROS

#include <assert.h>
#include <errno.h>
#include <error.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <timespec.h>
#include <unistd.h>

#include <display.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
} // extern "C"

uint32_t WINDOW_ID = 0;
uint32_t WINDOW_WIDTH = 0;
uint32_t WINDOW_HEIGHT = 0;

bool need_show = true;
bool need_exit = false;

uint32_t* framebuffer = NULL;
size_t framebuffer_size = 0;

void on_disconnect(void*)
{
	need_exit = true;
}

void on_quit(void*, uint32_t window_id)
{
	if ( window_id != WINDOW_ID )
		return;
	need_exit = true;
}

void on_resize(void*, uint32_t window_id, uint32_t width, uint32_t height)
{
	if ( window_id != WINDOW_ID )
		return;
	WINDOW_WIDTH = width;
	WINDOW_HEIGHT = height;
}

void on_keyboard(void*, uint32_t window_id, uint32_t)
{
	if ( window_id != WINDOW_ID )
		return;
}

static void DisplayVideoFrame(AVFrame* frame, struct display_connection* connection)
{
	size_t framebuffer_needed = sizeof(uint32_t) * WINDOW_WIDTH * WINDOW_HEIGHT;
	if ( framebuffer_size != framebuffer_needed )
	{
		framebuffer = (uint32_t*) realloc(framebuffer, framebuffer_size = framebuffer_needed);
		memset(framebuffer, 255, framebuffer_needed);
	}

	SwsContext* sws_ctx = sws_getContext(frame->width, frame->height,
	                                     (PixelFormat) frame->format, WINDOW_WIDTH,
	                                     WINDOW_HEIGHT, PIX_FMT_RGB32, SWS_BILINEAR,
	                                     NULL, NULL, NULL);
	assert(sws_ctx);

	uint8_t* data_arr[1] = { (uint8_t*) framebuffer };
	int stride_arr[1] = { (int) (sizeof(framebuffer[0]) * WINDOW_WIDTH) };
	sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height, data_arr,
	          stride_arr);

	sws_freeContext(sws_ctx);

	display_render_window(connection, WINDOW_ID, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, framebuffer);

	struct display_event_handlers handlers;
	memset(&handlers, 0, sizeof(handlers));
	handlers.disconnect_handler = on_disconnect;
	handlers.quit_handler = on_quit;
	handlers.resize_handler = on_resize;
	handlers.keyboard_handler = on_keyboard;
	while ( display_poll_event(connection, &handlers) == 0 );
}

bool PlayVideo(const char* path, struct display_connection* connection)
{
	bool ret = false;
	int av_error;
	AVFormatContext* format_ctx = NULL;
	int video_stream_id;
	int audio_stream_id;
	AVStream* video_stream = NULL;
	AVStream* audio_stream = NULL;
	AVCodec* video_codec = NULL;
	AVCodec* audio_codec = NULL;
	AVCodecContext* video_codec_ctx = NULL;
	AVCodecContext* audio_codec_ctx = NULL;
	AVFrame* video_frame = NULL;
	AVFrame* audio_frame = NULL;
	AVPacket packet;

	if ( (av_error = avformat_open_input(&format_ctx, path, NULL, NULL)) < 0 )
	{
		error(0, 0, "%s: cannot open: %i\n", path, av_error);
		goto cleanup_done;
	}

	if ( (av_error = avformat_find_stream_info(format_ctx, NULL)) < 0 )
	{
		error(0, 0, "%s: avformat_find_stream_info: %i\n", path, av_error);
		goto cleanup_input;
	}

	video_stream_id = av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1,
	                                      -1, &video_codec, 0);
	audio_stream_id = av_find_best_stream(format_ctx, AVMEDIA_TYPE_AUDIO, -1,
	                                      -1, &audio_codec, 0);

	if ( 0 <= video_stream_id )
		video_stream = format_ctx->streams[video_stream_id];
	if ( 0 <= audio_stream_id )
		audio_stream = format_ctx->streams[audio_stream_id];

	if ( !video_stream )
	{
		error(0, 0, "%s: no video stream found\n", path);
		goto cleanup_input;
	}

	if ( video_codec && !(video_codec_ctx = avcodec_alloc_context3(video_codec)))
		goto cleanup_input;
	if ( audio_codec && !(audio_codec_ctx = avcodec_alloc_context3(audio_codec)))
		goto cleanup_video_codec_ctx;

	if ( video_codec_ctx )
	{
		video_codec_ctx->extradata = video_stream->codec->extradata;
		video_codec_ctx->extradata_size = video_stream->codec->extradata_size;
		if ( (av_error = avcodec_open2(video_codec_ctx, NULL, NULL)) < 0 )
			goto cleanup_audio_codec_ctx;
	}
	if ( audio_codec_ctx )
	{
		audio_codec_ctx->extradata = audio_stream->codec->extradata;
		audio_codec_ctx->extradata_size = audio_stream->codec->extradata_size;
		if ( (av_error = avcodec_open2(audio_codec_ctx, NULL, NULL)) < 0 )
			goto cleanup_audio_codec_ctx;
	}

	if ( !(video_frame = avcodec_alloc_frame()) )
		goto cleanup_audio_codec_ctx;
	if ( !(audio_frame = avcodec_alloc_frame()) )
		goto cleanup_video_frame;

	struct timespec next_frame_at;
	clock_gettime(CLOCK_MONOTONIC, &next_frame_at);

	while ( !need_exit && 0 <= (av_error = av_read_frame(format_ctx, &packet)) )
	{
		int stream_index = packet.stream_index;
		int packet_off = 0;
		while ( stream_index == video_stream->index && packet_off < packet.size )
		{
			packet.data += packet_off; packet.size -= packet_off;
			int got_frame;
			int bytes_used = avcodec_decode_video2(video_codec_ctx, video_frame,
			                                       &got_frame, &packet);
			packet.data -= packet_off; packet.size += packet_off;

			if ( (av_error = bytes_used) < 0 )
				goto break_decode_loop;
			if ( !got_frame )
				break;
			packet_off += bytes_used;

			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			while ( timespec_le(now, next_frame_at) )
			{
				struct timespec left = timespec_sub(next_frame_at, now);
				clock_nanosleep(CLOCK_MONOTONIC, 0, &left, NULL);
				clock_gettime(CLOCK_MONOTONIC, &now);
			}

			DisplayVideoFrame(video_frame, connection);

			uintmax_t usecs = video_codec_ctx->ticks_per_frame * 1000000 *
			                  video_codec_ctx->time_base.num /
			                  video_codec_ctx->time_base.den;
			next_frame_at = timespec_add(next_frame_at, timespec_make(0, usecs * 1000));
		}
		while ( stream_index == audio_stream->index && packet_off < packet.size )
		{
			// TODO: Add sound support when an backend is available.
			packet_off = packet.size;
		}
	}
break_decode_loop:

	// TODO: Determine whether the are here because of EOF or whether an error
	//       occured and we need to print an error.
	// TODO: Do we need to clean up the last packet or does the av_read_frame
	//       function do that for us upon error?
	ret = true;

goto cleanup_audio_frame;
cleanup_audio_frame:
	if ( audio_frame )
#if 55 <= LIBAVCODEC_VERSION_MAJOR
		avcodec_free_frame(&audio_frame);
#else
		av_free(audio_frame);
#endif
cleanup_video_frame:
	if ( video_frame )
#if 55 <= LIBAVCODEC_VERSION_MAJOR
		avcodec_free_frame(&video_frame);
#else
		av_free(video_frame);
#endif
cleanup_audio_codec_ctx:
	if ( audio_codec_ctx )
	{
		audio_codec_ctx->extradata = NULL;
		avcodec_close(audio_codec_ctx);
		av_free(audio_codec_ctx);
	}
cleanup_video_codec_ctx:
	if ( video_codec_ctx )
	{
		video_codec_ctx->extradata = NULL;
		avcodec_close(video_codec_ctx);
		av_free(video_codec_ctx);
	}
cleanup_input:
	avformat_close_input(&format_ctx);
cleanup_done:
	return ret;
}

int main(int argc, char* argv[])
{
	struct display_connection* connection = display_connect_default();
	if ( !connection && errno == ECONNREFUSED )
		display_spawn(argc, argv);
	if ( !connection )
		error(1, errno, "Could not connect to display server");

	av_register_all();

	WINDOW_WIDTH = 800;
	WINDOW_HEIGHT = 450;

	display_create_window(connection, WINDOW_ID);
	display_resize_window(connection, WINDOW_ID, WINDOW_WIDTH, WINDOW_HEIGHT);
	display_show_window(connection, WINDOW_ID);

	for ( int i = 1; i < argc; i++ )
	{
		display_title_window(connection, WINDOW_ID, argv[i]);
		if ( !PlayVideo(argv[i], connection) )
			return 1;
	}

	display_disconnect(connection);

	return 0;
}
