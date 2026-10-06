#define _GNU_SOURCE

#include "camera.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void notify_save(struct camera_state *state, char event)
{
	ssize_t result;

	if (state->timer_fd < 0)
		return;
	do {
		result = write(state->timer_fd, &event, 1);
	} while (result < 0 && errno == EINTR);
}

static void write_all(int fd, const void *data, size_t length)
{
	const unsigned char *bytes = data;

	while (length > 0) {
		ssize_t written = write(fd, bytes, length);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			camera_fail_errno("write image");
		}
		bytes += written;
		length -= (size_t)written;
	}
}

static void put_le16(unsigned char *data, uint16_t value)
{
	data[0] = value & 0xff;
	data[1] = value >> 8;
}

static void put_le32(unsigned char *data, uint32_t value)
{
	data[0] = value & 0xff;
	data[1] = (value >> 8) & 0xff;
	data[2] = (value >> 16) & 0xff;
	data[3] = (value >> 24) & 0xff;
}

static int save_bmp(const struct camera_state *state,
			    const unsigned char *frame, const char *path)
{
	unsigned int row_bytes = state->camera_width * 3;
	unsigned int padding = (4 - row_bytes % 4) % 4;
	uint32_t image_size = (row_bytes + padding) * state->camera_height;
	unsigned char header[54];
	unsigned char zero_padding[3] = {0, 0, 0};
	int fd;
	unsigned int y;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;
	memset(header, 0, sizeof(header));
	header[0] = 'B';
	header[1] = 'M';
	put_le32(header + 2, 54 + image_size);
	put_le32(header + 10, 54);
	put_le32(header + 14, 40);
	put_le32(header + 18, state->camera_width);
	put_le32(header + 22, state->camera_height);
	put_le16(header + 26, 1);
	put_le16(header + 28, 24);
	put_le32(header + 34, image_size);
	write_all(fd, header, sizeof(header));

	for (y = 0; y < state->camera_height; y++) {
		unsigned int source_y = state->camera_height - y - 1;
		unsigned int x;
		unsigned char pixel[3];

		for (x = 0; x < state->camera_width; x++) {
			unsigned int red;
			unsigned int green;
			unsigned int blue;

			camera_source_pixel_rgb(state, frame, x, source_y,
						&red, &green, &blue);
			pixel[0] = blue;
			pixel[1] = green;
			pixel[2] = red;
			write_all(fd, pixel, sizeof(pixel));
		}
		write_all(fd, zero_padding, padding);
	}
	close(fd);
	return 0;
}

static int make_image_path(const struct camera_state *state,
				   char *path, size_t path_length)
{
	struct timespec current_time;
	struct tm local_time;

	if (mkdir(state->save_dir, 0755) < 0 && errno != EEXIST)
		return -1;
	if (clock_gettime(CLOCK_REALTIME, &current_time) < 0 ||
	    localtime_r(&current_time.tv_sec, &local_time) == NULL)
		return -1;
	if (snprintf(path, path_length,
			"%s/camera_%04d%02d%02d_%02d%02d%02d_%03ld.bmp",
			state->save_dir, local_time.tm_year + 1900, local_time.tm_mon + 1,
			local_time.tm_mday, local_time.tm_hour, local_time.tm_min,
			local_time.tm_sec, current_time.tv_nsec / 1000000) >=
			(int)path_length)
		return -1;
	return 0;
}

static void *saver_main(void *argument)
{
	struct camera_state *state = argument;

	while (camera_running) {
		char event;
		ssize_t event_length;
		unsigned char *frame_copy = NULL;
		size_t frame_length = 0;
		char image_path[512];

		/* camera_timer 的 read 会在没有定时或按键事件时休眠。 */
		event_length = read(state->timer_fd, &event, sizeof(event));
		if (event_length < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (event_length != (ssize_t)sizeof(event))
			break;
		if (!camera_running)
			break;

		/* 定时器在内核中自动重新计时；这里只处理收到的事件。 */
		pthread_mutex_lock(&state->frame_lock);
		if (state->frame_valid) {
			frame_length = state->latest_frame_length;
			frame_copy = malloc(frame_length);
			if (frame_copy)
				memcpy(frame_copy, state->latest_frame, frame_length);
		}
		pthread_mutex_unlock(&state->frame_lock);

		if (frame_copy &&
		    make_image_path(state, image_path, sizeof(image_path)) == 0 &&
		    save_bmp(state, frame_copy, image_path) == 0)
			fprintf(stderr, "camera: saved %s\n", image_path);
		free(frame_copy);
	}
	return NULL;
}

static void *key_main(void *argument)
{
	struct camera_state *state = argument;
	unsigned char key_value;
	int key_fd = open(state->key_device, O_RDONLY);

	if (key_fd < 0) {
		fprintf(stderr, "camera: key device %s unavailable: %s\n",
			state->key_device, strerror(errno));
		return NULL;
	}
	while (camera_running) {
		ssize_t result = read(key_fd, &key_value, sizeof(key_value));
		if (result < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (result == (ssize_t)sizeof(key_value) && key_value != 0) {
			notify_save(state, CAMERA_TIMER_EVENT_KEY);
		}
	}
	close(key_fd);
	return NULL;
}

int camera_save_start(struct camera_state *state)
{
	int result;

	state->timer_fd = open(DEFAULT_TIMER_DEVICE, O_RDWR);
	if (state->timer_fd < 0)
		return errno;

	result = pthread_create(&state->saver_thread, NULL,
				saver_main, state);
	if (result != 0) {
		camera_save_stop(state);
		return result;
	}
	state->save_thread_started = 1;
	result = pthread_create(&state->key_thread, NULL, key_main, state);
	if (result == 0)
		state->key_thread_started = 1;
	else
		fprintf(stderr, "camera: KEY0 monitor disabled: %s\n", strerror(result));
	return 0;
}

void camera_save_stop(struct camera_state *state)
{
	camera_running = 0;
	/* 先取消可能阻塞在 read() 的按键线程，再等待保存线程结束。 */
	if (state->key_thread_started)
		pthread_cancel(state->key_thread);
	if (state->key_thread_started)
		pthread_join(state->key_thread, NULL);
	if (state->save_thread_started) {
		notify_save(state, CAMERA_TIMER_EVENT_QUIT);
		pthread_join(state->saver_thread, NULL);
	}
	state->save_thread_started = 0;
	state->key_thread_started = 0;
	if (state->timer_fd >= 0)
		close(state->timer_fd);
	state->timer_fd = -1;
}
