/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ITE IT6612 HDMI transmitter (HDMI OUT passthrough) on the HD60 Pro. */
#ifndef HD60_IT6612_H
#define HD60_IT6612_H

#include <stdint.h>

#include "mst3367.h"
#include "mz0380.h"

/*
 * The Windows driver keeps this state in a CIT6612 object; the port keeps
 * the same byte layout (st[]) so offsets match the decompiled code.
 */
struct it6612 {
	struct mz_dev *m;
	uint8_t st[0x2b0];
	/* inputs the driver reads from its device extension */
	unsigned in_width;	/* ext+0x2ec */
	unsigned colorimetry;	/* ext+0x31c, AVI C1C0 from the receiver */
	unsigned quant_range;	/* ext+0x320 */
	int input_hdcp;		/* dev+0x7820, source is HDCP encrypted */
	uint8_t tv_mode;	/* ext+0x1bdc, from it6612_tv_mode() */
	unsigned ddc_probe_ticks;	/* port addition: DDC presence check while HPD is low */
	int verbose;
};

int it6612_init(struct it6612 *t, struct mz_dev *m);
/* CIT6612_GetTvMode: map the receiver's mode to the transmitter mode id. */
uint8_t it6612_tv_mode(unsigned width, unsigned height, unsigned fps);
/* Call periodically (the driver calls it from its analysis thread). */
void it6612_timer(struct it6612 *t, const struct video_mode *mode, bool signal);
const char *it6612_state_name(const struct it6612 *t);
/* Read one 128-byte EDID block from the display on HDMI OUT (DDC). 0 on success. */
int it6612_read_edid(struct it6612 *t, uint8_t *buf, unsigned block);

#endif
