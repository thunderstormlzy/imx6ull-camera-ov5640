#define _GNU_SOURCE

#include "camera.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int camera_xioctl(int fd, unsigned long request, void *argument)
{
	int result;

	do {
		result = ioctl(fd, request, argument);
	} while (result == -1 && errno == EINTR);

	return result;
}

void camera_fail_errno(const char *operation)
{
	fprintf(stderr, "camera: %s: %s\n", operation, strerror(errno));
	exit(EXIT_FAILURE);
}

void camera_fail_message(const char *message)
{
	fprintf(stderr, "camera: %s\n", message);
	exit(EXIT_FAILURE);
}

static unsigned int component_to_u16(unsigned int value, unsigned int bits)
{
	if (bits == 0)
		return 0;
	return (value * ((1U << bits) - 1U) + 127U) / 255U;
}

static uint32_t pack_pixel(const struct camera_state *state,
				   unsigned int red, unsigned int green,
				   unsigned int blue)
{
	return (component_to_u16(red, state->lcd_red_length) << state->lcd_red_offset) |
	       (component_to_u16(green, state->lcd_green_length) << state->lcd_green_offset) |
	       (component_to_u16(blue, state->lcd_blue_length) << state->lcd_blue_offset);
}

static unsigned int clamp_u8(int value)
{
	if (value < 0)
		return 0;
	if (value > 255)
		return 255;
	return (unsigned int)value;
}

static void yuv_to_rgb(int y_value, int u_value, int v_value,
			       unsigned int *red, unsigned int *green,
			       unsigned int *blue)
{
	int y = y_value - 16;
	int u = u_value - 128;
	int v = v_value - 128;
	int red_value = (298 * y + 409 * v + 128) >> 8;
	int green_value = (298 * y - 100 * u - 208 * v + 128) >> 8;
	int blue_value = (298 * y + 516 * u + 128) >> 8;

	*red = clamp_u8(red_value);
	*green = clamp_u8(green_value);
	*blue = clamp_u8(blue_value);
}

void camera_source_pixel_rgb(const struct camera_state *state,
				     const unsigned char *frame,
				     unsigned int x, unsigned int y,
				     unsigned int *red, unsigned int *green,
				     unsigned int *blue)
{
	const unsigned char *line = frame + y * state->camera_bytesperline;

	if (state->camera_pixfmt == V4L2_PIX_FMT_RGB565) {
		const uint16_t *pixel = (const uint16_t *)line + x;
		uint16_t value = *pixel;
		*red = ((value >> 11) & 0x1f) << 3;
		*green = ((value >> 5) & 0x3f) << 2;
		*blue = (value & 0x1f) << 3;
		return;
	}

	{
		const unsigned char *pair = line + (x & ~1U) * 2;
		int y_value;
		int u_value;
		int v_value;

		if (state->camera_pixfmt == V4L2_PIX_FMT_UYVY) {
			u_value = pair[0];
			y_value = pair[(x & 1U) ? 3 : 1];
			v_value = pair[2];
		} else {
			y_value = pair[(x & 1U) ? 2 : 0];
			u_value = pair[1];
			v_value = pair[3];
		}
		yuv_to_rgb(y_value, u_value, v_value, red, green, blue);
	}
}

void camera_display_frame(struct camera_state *state,
				  const unsigned char *frame)
{
	unsigned int display_y;
	unsigned int source_y = 0;
	unsigned int y_error = 0;
	unsigned int source_row;
	unsigned int bytes_per_pixel = state->lcd_bpp / 8;
	size_t source_stride = state->camera_width * bytes_per_pixel;
	size_t lcd_frame_length = state->lcd_frame_length;
	unsigned char *target_buffer;
	uint32_t crtc = 0;

	/* 双缓冲时只修改当前显示页之外的另一页，避免边扫描边改图。 */
	if (state->lcd_double_buffer)
		target_buffer = state->lcd_memory +
			state->lcd_draw_page * state->lcd_frame_length;
	else
		target_buffer = state->lcd_back_buffer;

	/* 每个摄像头像素只做一次 YUV 到 RGB 转换。 */
	for (source_row = 0; source_row < state->camera_height; source_row++) {
		unsigned int source_x;
		unsigned char *line = state->lcd_source_buffer +
			source_row * source_stride;

		for (source_x = 0; source_x < state->camera_width; source_x++) {
			unsigned int red;
			unsigned int green;
			unsigned int blue;
			uint32_t packed;

			camera_source_pixel_rgb(state, frame, source_x, source_row,
						&red, &green, &blue);
			packed = pack_pixel(state, red, green, blue);
			if (state->lcd_bpp == 16)
				((uint16_t *)line)[source_x] = (uint16_t)packed;
			else
				((uint32_t *)line)[source_x] = packed;
		}
	}

	/* 先完整生成一帧；双缓冲时目标就是下一页显存。 */
	for (display_y = 0; display_y < state->lcd_height; display_y++) {
		unsigned int display_x;
		unsigned int source_x = 0;
		unsigned int x_error = 0;
		unsigned char *target_line = target_buffer +
			display_y * state->lcd_stride;
		const unsigned char *source_line = state->lcd_source_buffer +
			source_y * source_stride;

		/* 缩放阶段只复制已经转换好的 LCD 像素，不再重复进行颜色转换。 */
		for (display_x = 0; display_x < state->lcd_width; display_x++) {
			if (state->lcd_bpp == 16)
				((uint16_t *)target_line)[display_x] =
					((const uint16_t *)source_line)[source_x];
			else
				((uint32_t *)target_line)[display_x] =
					((const uint32_t *)source_line)[source_x];

			x_error += state->camera_width;
			while (x_error >= state->lcd_width) {
				x_error -= state->lcd_width;
				source_x++;
			}
		}
		y_error += state->camera_height;
		while (y_error >= state->lcd_height) {
			y_error -= state->lcd_height;
			source_y++;
		}
	}
	if (state->lcd_double_buffer) {
		/* mxsfb 在下一次 VSYNC 到来时切换到指定页。 */
		state->lcd_variable.yoffset =
			state->lcd_draw_page * state->lcd_height;
		state->lcd_variable.activate = FB_ACTIVATE_VBL;
		if (camera_xioctl(state->fb_fd, FBIOPAN_DISPLAY,
				  &state->lcd_variable) == 0) {
			state->lcd_draw_page = 1U - state->lcd_draw_page;
			return;
		}

		/* 页面切换失败时退回复制模式，保证旧驱动仍可显示。 */
		fprintf(stderr, "camera: framebuffer page flip failed: %s, using copy mode\n",
			strerror(errno));
		memcpy(state->lcd_back_buffer, target_buffer, lcd_frame_length);
		state->lcd_double_buffer = 0;
	}

	/* 单页 framebuffer 只能等待 VSYNC 后复制，作为兼容回退路径。 */
	(void)camera_xioctl(state->fb_fd, FBIO_WAITFORVSYNC, &crtc);
	memcpy(state->lcd_memory, state->lcd_back_buffer, lcd_frame_length);
}

int camera_open(struct camera_state *state, const char *device)
{
	struct v4l2_capability capability;
	struct v4l2_format format;
	struct v4l2_requestbuffers request;
	unsigned int index;

	state->video_fd = open(device, O_RDWR | O_NONBLOCK, 0);
	if (state->video_fd < 0)
		camera_fail_errno("open video device");
	memset(&capability, 0, sizeof(capability));
	if (camera_xioctl(state->video_fd, VIDIOC_QUERYCAP, &capability) < 0)
		camera_fail_errno("VIDIOC_QUERYCAP");
	if (!(capability.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
	    !(capability.capabilities & V4L2_CAP_STREAMING))
		camera_fail_message("video device lacks capture or streaming support");

	memset(&format, 0, sizeof(format));
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	format.fmt.pix.width = CAMERA_WIDTH;
	format.fmt.pix.height = CAMERA_HEIGHT;
	format.fmt.pix.field = V4L2_FIELD_NONE;
	format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	if (camera_xioctl(state->video_fd, VIDIOC_S_FMT, &format) < 0)
		camera_fail_errno("VIDIOC_S_FMT");
	if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV &&
	    format.fmt.pix.pixelformat != V4L2_PIX_FMT_UYVY &&
	    format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565)
		camera_fail_message("camera returned an unsupported pixel format");

	state->camera_width = format.fmt.pix.width;
	state->camera_height = format.fmt.pix.height;
	state->camera_bytesperline = format.fmt.pix.bytesperline;
	state->camera_sizeimage = format.fmt.pix.sizeimage;
	state->camera_pixfmt = format.fmt.pix.pixelformat;
	memset(&request, 0, sizeof(request));
	request.count = BUFFER_COUNT;
	request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	request.memory = V4L2_MEMORY_MMAP;
	if (camera_xioctl(state->video_fd, VIDIOC_REQBUFS, &request) < 0)
		camera_fail_errno("VIDIOC_REQBUFS");
	if (request.count < 2)
		camera_fail_message("camera returned too few capture buffers");
	state->buffers = calloc(request.count, sizeof(*state->buffers));
	if (!state->buffers)
		camera_fail_errno("allocate capture buffers");
	state->buffer_count = request.count;
	for (index = 0; index < state->buffer_count; index++) {
		struct v4l2_buffer buffer;

		memset(&buffer, 0, sizeof(buffer));
		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.index = index;
		if (camera_xioctl(state->video_fd, VIDIOC_QUERYBUF, &buffer) < 0)
			camera_fail_errno("VIDIOC_QUERYBUF");
		state->buffers[index].length = buffer.length;
		state->buffers[index].start = mmap(NULL, buffer.length,
				PROT_READ | PROT_WRITE, MAP_SHARED,
				state->video_fd, buffer.m.offset);
		if (state->buffers[index].start == MAP_FAILED)
			camera_fail_errno("mmap capture buffer");
	}
	state->latest_frame_length = state->camera_bytesperline * state->camera_height;
	if (state->latest_frame_length > state->buffers[0].length)
		camera_fail_message("capture buffer is smaller than the negotiated frame");
	state->latest_frame = malloc(state->latest_frame_length);
	if (!state->latest_frame)
		camera_fail_errno("allocate latest frame");
	return 0;
}

int camera_open_lcd(struct camera_state *state, const char *device)
{
	struct fb_fix_screeninfo fixed;
	struct fb_var_screeninfo variable;
	size_t lcd_frame_length;
	size_t source_frame_length;

	state->fb_fd = open(device, O_RDWR, 0);
	if (state->fb_fd < 0)
		camera_fail_errno("open framebuffer");
	if (camera_xioctl(state->fb_fd, FBIOGET_FSCREENINFO, &fixed) < 0 ||
	    camera_xioctl(state->fb_fd, FBIOGET_VSCREENINFO, &variable) < 0)
		camera_fail_errno("read framebuffer parameters");
	if (variable.bits_per_pixel != 16 && variable.bits_per_pixel != 32)
		camera_fail_message("LCD framebuffer must use 16 or 32 bits per pixel");
	state->lcd_width = variable.xres;
	state->lcd_height = variable.yres;
	state->lcd_bpp = variable.bits_per_pixel;
	state->lcd_stride = fixed.line_length;
	state->lcd_red_offset = variable.red.offset;
	state->lcd_red_length = variable.red.length;
	state->lcd_green_offset = variable.green.offset;
	state->lcd_green_length = variable.green.length;
	state->lcd_blue_offset = variable.blue.offset;
	state->lcd_blue_length = variable.blue.length;
	state->lcd_memory_length = fixed.smem_len;
	lcd_frame_length = state->lcd_stride * state->lcd_height;
	state->lcd_frame_length = lcd_frame_length;
	state->lcd_variable = variable;
	if (lcd_frame_length > state->lcd_memory_length)
		camera_fail_message("LCD memory is smaller than the visible frame");
	state->lcd_double_buffer =
		variable.yres_virtual >= state->lcd_height * 2 &&
		state->lcd_memory_length >= lcd_frame_length * 2;
	state->lcd_draw_page = state->lcd_double_buffer ? 1U : 0U;
	state->lcd_memory = mmap(NULL, state->lcd_memory_length,
			PROT_READ | PROT_WRITE, MAP_SHARED, state->fb_fd, 0);
	if (state->lcd_memory == MAP_FAILED)
		camera_fail_errno("mmap framebuffer");
	state->lcd_back_buffer = malloc(lcd_frame_length);
	if (!state->lcd_back_buffer)
		camera_fail_errno("allocate LCD back buffer");
	source_frame_length = state->camera_width * state->camera_height *
		(state->lcd_bpp / 8);
	state->lcd_source_buffer = malloc(source_frame_length);
	if (!state->lcd_source_buffer)
		camera_fail_errno("allocate LCD source buffer");
	/* 初始画面清零，避免两页显存中残留旧画面。 */
	memset(state->lcd_memory, 0, state->lcd_memory_length);
	memset(state->lcd_back_buffer, 0, lcd_frame_length);
	return 0;
}

int camera_start(struct camera_state *state)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	unsigned int index;

	for (index = 0; index < state->buffer_count; index++) {
		struct v4l2_buffer buffer;

		memset(&buffer, 0, sizeof(buffer));
		buffer.type = type;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.index = index;
		if (camera_xioctl(state->video_fd, VIDIOC_QBUF, &buffer) < 0)
			camera_fail_errno("VIDIOC_QBUF");
	}
	if (camera_xioctl(state->video_fd, VIDIOC_STREAMON, &type) < 0)
		camera_fail_errno("VIDIOC_STREAMON");
	return 0;
}

void camera_stop(struct camera_state *state)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	unsigned int index;

	if (state->video_fd >= 0)
		camera_xioctl(state->video_fd, VIDIOC_STREAMOFF, &type);
	for (index = 0; index < state->buffer_count; index++)
		if (state->buffers[index].start && state->buffers[index].start != MAP_FAILED)
			munmap(state->buffers[index].start, state->buffers[index].length);
	free(state->buffers);
	free(state->latest_frame);
	free(state->lcd_back_buffer);
	free(state->lcd_source_buffer);
	if (state->lcd_memory && state->lcd_memory != MAP_FAILED)
		munmap(state->lcd_memory, state->lcd_memory_length);
	if (state->video_fd >= 0)
		close(state->video_fd);
	if (state->fb_fd >= 0)
		close(state->fb_fd);
}
