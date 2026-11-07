/* SPDX-License-Identifier: GPL-2.0-or-later */
/* MStar MST3367 HDMI receiver on the HD60 Pro (I2C 0x9c via the card). */
#ifndef HD60_MST3367_H
#define HD60_MST3367_H

#include "mz0380.h"

enum colorspace { CS_RGB = 0, CS_YUV422 = 1, CS_YUV444 = 2 };

struct video_mode {
	unsigned width, height;	/* height is per field for interlaced modes */
	unsigned fps;		/* integer rate; 59.94 is 60 with ntsc = 1 */
	bool ntsc;		/* 1000/1001 rate (MST3367_GET_FREQV) */
	bool interlaced;
	unsigned audio_rate;
	enum colorspace cs;
	uint8_t colorimetry;	/* AVI C1C0: 0 none, 1 BT.601, 2 BT.709, 3 extended */
	uint8_t quant;		/* AVI Q1Q0 (bank2 0x4a bits 2-3) */
	bool hdcp;		/* source is HDCP encrypted (driver dev+0x7820) */
	/* raw measurements */
	unsigned htotal, vtotal, hactive, vfreq10;
};

int mst3367_init(struct mz_dev *m);
/* Returns 1 with *mode filled when a stable signal is present, 0 when not. */
int mst3367_detect(struct mz_dev *m, struct video_mode *mode);
int mst3367_setup_csc(struct mz_dev *m, enum colorspace cs);
bool mode_equal(const struct video_mode *a, const struct video_mode *b);

#endif
