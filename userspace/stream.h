/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DMA buffer registration, capture start/stop and frame retrieval. */
#ifndef HD60_STREAM_H
#define HD60_STREAM_H

#include <stdio.h>

#include "mst3367.h"
#include "mz0380.h"

#define VIDEO_BUF_SIZE	0x466000	/* announced to the card by cmd 0x02 */
#define ENC_BUF_SIZE	0x34bd00	/* announced by cmds 0x04/0x05 */
#define ENC_HDR_SIZE	0x1000		/* H.264 payload offset in an encoder buffer */
#define AUDIO_BUF_SIZE	0x2000		/* per slot, 4 slots per channel */
#define AUDIO_PERIOD_BYTES 4096		/* 4 x 256 frames x 16-bit stereo (cmd 0x2a period) */

struct stream_cfg {
	unsigned bitrate;	/* CustomEncoderVideoBitRateProperty, bit/s */
	unsigned gop;		/* AnalogVideoCompressionKeyframeRateProperty */
	unsigned quality;	/* AnalogVideoCompressionQualityProperty (0..10000) */
	bool deinterlace;	/* CustomAnalogVideoDeinterlaceTypeProperty */
	unsigned audio_channels;
	bool h264;		/* bitstream_num 1; the card stalls when the encoder DMA stalls */
};

struct stream {
	struct dma_buf video[4];	/* raw I420 preview, cmd 0x02 */
	struct dma_buf enc[2][4];	/* H.264 main/sub, cmds 0x04/0x05 */
	struct dma_buf audio;		/* 8 channels x 4 slots x 0x2000, cmd 0x03 */
	unsigned nvideo, nenc;		/* distinct buffers allocated (1 or 4) */
	bool audio_shared;		/* all audio channels share channel 0's slots */
	struct stream_cfg cfg;
	struct video_mode mode;
	unsigned last_vidx, last_aidx;	/* last buffer index delivered */
	uint32_t irq_seen;		/* OR of every IRQ status seen (debug) */
	unsigned irq_count;
	FILE *irqlog;			/* debug: every IRQ status seen */
};

struct frame {
	enum { FRAME_VIDEO, FRAME_H264, FRAME_AUDIO } kind;
	const uint8_t *data;
	unsigned len;
	unsigned index;
	bool keyframe;
};

void stream_default_cfg(struct stream_cfg *c);
/* Allocates the 4-buffer ring, falling back to one shared buffer per pipe
 * when RLIMIT_MEMLOCK is too low (force_shared skips the attempt). */
int stream_alloc(struct mz_dev *m, struct stream *s, bool force_shared);
void stream_free(struct mz_dev *m, struct stream *s);
int stream_register_buffers(struct mz_dev *m, struct stream *s);
int stream_start(struct mz_dev *m, struct stream *s, const struct video_mode *mode, uint8_t colorimetry);
int stream_stop(struct mz_dev *m);
/* Polls the card; calls cb for each completed frame. Returns frames seen. */
int stream_poll(struct mz_dev *m, struct stream *s, void (*cb)(const struct frame *, void *), void *arg);

#endif
