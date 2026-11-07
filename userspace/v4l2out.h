/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Feed frames into a v4l2loopback device so any V4L2 application can use them. */
#ifndef HD60_V4L2OUT_H
#define HD60_V4L2OUT_H

#include <stdint.h>

struct v4l2out {
	int fd;
	unsigned width, height, stride;
	uint8_t *tmp;	/* repacking buffer when the card stride differs */
};

int v4l2out_open(struct v4l2out *o, const char *dev, unsigned width, unsigned height, unsigned fps);
/* I420 frame with the given luma stride (chroma stride = stride / 2). */
int v4l2out_write_i420(struct v4l2out *o, const uint8_t *frame, unsigned stride);
void v4l2out_close(struct v4l2out *o);

#endif
