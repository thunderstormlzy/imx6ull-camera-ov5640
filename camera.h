#ifndef CAMERA_H
#define CAMERA_H

#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <linux/fb.h>

#include "camera_timer_events.h"

#define CAMERA_WIDTH 640
#define CAMERA_HEIGHT 480
#define BUFFER_COUNT 4
#define SAVE_INTERVAL_SECONDS (60 * 60)
#define DEFAULT_VIDEO_DEVICE "/dev/video0"
#define DEFAULT_FB_DEVICE "/dev/fb0"
#define DEFAULT_TIMER_DEVICE "/dev/camera_timer"
#define DEFAULT_SAVE_DIR \
	"/home/lzy/linux/imx6ull/Drivers/Linux_Drivers/camera/data"
#define DEFAULT_KEY_DEVICE "/dev/key"

struct capture_buffer {
	void *start;
	size_t length;
};

struct camera_state {
	int video_fd;
	int fb_fd;
	struct capture_buffer *buffers;
	unsigned int buffer_count;
	unsigned int camera_width;
	unsigned int camera_height;
	unsigned int camera_bytesperline;
	unsigned int camera_sizeimage;
	unsigned int camera_pixfmt;
	unsigned int lcd_width;
	unsigned int lcd_height;
	unsigned int lcd_bpp;
	unsigned int lcd_stride;
	unsigned int lcd_red_offset;
	unsigned int lcd_red_length;
	unsigned int lcd_green_offset;
	unsigned int lcd_green_length;
	unsigned int lcd_blue_offset;
	unsigned int lcd_blue_length;
	unsigned char *lcd_memory;
	unsigned char *lcd_back_buffer;
	unsigned char *lcd_source_buffer;
	size_t lcd_memory_length;
	size_t lcd_frame_length;
	struct fb_var_screeninfo lcd_variable;
	unsigned int lcd_draw_page;
	int lcd_double_buffer;

	char *save_dir;
	char *key_device;

	unsigned char *latest_frame;
	size_t latest_frame_length;
	pthread_mutex_t frame_lock;
	int frame_valid;
	int timer_fd;
	pthread_t saver_thread;
	pthread_t key_thread;
	int save_thread_started;
	int key_thread_started;
};

extern volatile sig_atomic_t camera_running;

int camera_xioctl(int fd, unsigned long request, void *argument);
void camera_fail_errno(const char *operation);
void camera_fail_message(const char *message);

int camera_open(struct camera_state *state, const char *device);
int camera_open_lcd(struct camera_state *state, const char *device);
int camera_start(struct camera_state *state);
void camera_stop(struct camera_state *state);
void camera_display_frame(struct camera_state *state,
				 const unsigned char *frame);
void camera_source_pixel_rgb(const struct camera_state *state,
				     const unsigned char *frame,
				     unsigned int x, unsigned int y,
				     unsigned int *red, unsigned int *green,
				     unsigned int *blue);

int camera_save_start(struct camera_state *state);
void camera_save_stop(struct camera_state *state);

#endif
