#define _GNU_SOURCE

#include "camera.h"

#include <errno.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

volatile sig_atomic_t camera_running = 1;

static void stop_capture(int signal_number)
{
	(void)signal_number;
	camera_running = 0;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s [video] [fb] [save-dir] [key-device]\n"
		"Defaults: %s %s %s %s\n",
		program, DEFAULT_VIDEO_DEVICE, DEFAULT_FB_DEVICE, DEFAULT_SAVE_DIR,
		DEFAULT_KEY_DEVICE);
}

int main(int argc, char **argv)
{
	struct camera_state state;
	const char *video_device;
	const char *fb_device;
	int result;

	if (argc > 5) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	memset(&state, 0, sizeof(state));
	state.video_fd = -1;
	state.fb_fd = -1;
	state.timer_fd = -1;
	video_device = argc > 1 ? argv[1] : DEFAULT_VIDEO_DEVICE;
	fb_device = argc > 2 ? argv[2] : DEFAULT_FB_DEVICE;
	state.save_dir = argc > 3 ? argv[3] : DEFAULT_SAVE_DIR;
	state.key_device = argc > 4 ? argv[4] : DEFAULT_KEY_DEVICE;
	signal(SIGINT, stop_capture);
	signal(SIGTERM, stop_capture);
	if (pthread_mutex_init(&state.frame_lock, NULL) != 0)
		camera_fail_message("initialize frame synchronization");

	camera_open(&state, video_device);
	camera_open_lcd(&state, fb_device);
	fprintf(stderr, "camera: %ux%u -> LCD %ux%u, bpp=%u, stride=%u, virtual=%ux%u, "
		"RGB=(%u/%u,%u/%u,%u/%u), framebuffer pages=%u, save every %u seconds\n",
		state.camera_width, state.camera_height, state.lcd_width, state.lcd_height,
		state.lcd_bpp, state.lcd_stride, state.lcd_variable.xres_virtual,
		state.lcd_variable.yres_virtual, state.lcd_red_offset,
		state.lcd_red_length, state.lcd_green_offset, state.lcd_green_length,
		state.lcd_blue_offset, state.lcd_blue_length,
		state.lcd_double_buffer ? 2U : 1U,
		SAVE_INTERVAL_SECONDS);

	camera_start(&state);
	result = camera_save_start(&state);
	if (result != 0) {
		fprintf(stderr, "camera: create save threads: %s\n", strerror(result));
		camera_save_stop(&state);
		camera_stop(&state);
		pthread_mutex_destroy(&state.frame_lock);
		return EXIT_FAILURE;
	}

	while (camera_running) {
		fd_set read_fds;
		struct timeval timeout = {2, 0};
		struct v4l2_buffer buffer;

		FD_ZERO(&read_fds);
		FD_SET(state.video_fd, &read_fds);
		result = select(state.video_fd + 1, &read_fds, NULL, NULL, &timeout);
		if (result < 0) {
			if (errno == EINTR)
				continue;
			camera_fail_errno("select camera frame");
		}
		if (result == 0)
			continue;
		memset(&buffer, 0, sizeof(buffer));
		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;
		if (camera_xioctl(state.video_fd, VIDIOC_DQBUF, &buffer) < 0) {
			if (errno == EAGAIN)
				continue;
			camera_fail_errno("VIDIOC_DQBUF");
		}
		if (buffer.index >= state.buffer_count)
			camera_fail_message("camera returned an invalid buffer index");
		camera_display_frame(&state, state.buffers[buffer.index].start);
		pthread_mutex_lock(&state.frame_lock);
		memcpy(state.latest_frame, state.buffers[buffer.index].start,
		       state.latest_frame_length);
		state.frame_valid = 1;
		pthread_mutex_unlock(&state.frame_lock);
		if (camera_xioctl(state.video_fd, VIDIOC_QBUF, &buffer) < 0)
			camera_fail_errno("VIDIOC_QBUF");
	}

	camera_save_stop(&state);
	camera_stop(&state);
	pthread_mutex_destroy(&state.frame_lock);
	return EXIT_SUCCESS;
}
