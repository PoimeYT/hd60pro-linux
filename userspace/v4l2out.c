/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "v4l2out.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int v4l2out_open(struct v4l2out *o, const char *dev, unsigned width, unsigned height, unsigned fps)
{
	memset(o, 0, sizeof(*o));
	o->fd = open(dev, O_RDWR);
	if (o->fd < 0) {
		fprintf(stderr, "v4l2out: open %s: %s\n", dev, strerror(errno));
		return -errno;
	}
	struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	f.fmt.pix.width = width;
	f.fmt.pix.height = height;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
	f.fmt.pix.field = V4L2_FIELD_NONE;
	f.fmt.pix.bytesperline = width;
	f.fmt.pix.sizeimage = width * height * 3 / 2;
	f.fmt.pix.colorspace = V4L2_COLORSPACE_REC709;
	if (ioctl(o->fd, VIDIOC_S_FMT, &f)) {
		fprintf(stderr, "v4l2out: S_FMT %ux%u: %s\n", width, height, strerror(errno));
		close(o->fd);
		return -errno;
	}
	struct v4l2_streamparm p = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	p.parm.output.timeperframe.numerator = 1;
	p.parm.output.timeperframe.denominator = fps ? fps : 60;
	ioctl(o->fd, VIDIOC_S_PARM, &p);	/* optional on v4l2loopback */
	o->width = width;
	o->height = height;
	o->tmp = malloc(width * height * 3 / 2);
	return o->tmp ? 0 : -ENOMEM;
}

int v4l2out_write_i420(struct v4l2out *o, const uint8_t *frame, unsigned stride)
{
	size_t size = (size_t)o->width * o->height * 3 / 2;
	const uint8_t *src = frame;
	if (stride != o->width) {
		/* Card planes are stride x height (Y) then stride/2 x height/2 (U, V). */
		uint8_t *d = o->tmp;
		for (unsigned y = 0; y < o->height; y++, d += o->width)
			memcpy(d, frame + (size_t)y * stride, o->width);
		const uint8_t *u = frame + (size_t)stride * o->height;
		const uint8_t *v = u + (size_t)(stride / 2) * (o->height / 2);
		for (unsigned p = 0; p < 2; p++) {
			const uint8_t *s = p ? v : u;
			for (unsigned y = 0; y < o->height / 2; y++, d += o->width / 2)
				memcpy(d, s + (size_t)y * (stride / 2), o->width / 2);
		}
		src = o->tmp;
	}
	ssize_t n = write(o->fd, src, size);
	return n == (ssize_t)size ? 0 : -errno;
}

void v4l2out_close(struct v4l2out *o)
{
	if (o->fd > 0)
		close(o->fd);
	free(o->tmp);
	o->tmp = NULL;
	o->fd = -1;
}
