/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Streaming, reconstructed from MZ0380_HwBackgroundInitialize (buffer
 * registration), MZ0380_StartFirmware / MZ0380_StopFirmware and
 * MZ0380_HwProcessAnalogPCIPacket for the HD60 Pro (subsys 1cfa:0006) with
 * the INF's registry defaults. Only channel 0 exists on this board.
 */
#include "stream.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define IOVA_VIDEO	0x10000000ull
#define IOVA_ENC	0x20000000ull
#define IOVA_AUDIO	0x30000000ull
#define IOVA_STRIDE	0x01000000ull

void stream_default_cfg(struct stream_cfg *c)
{
	c->bitrate = 0x400000;	/* INF: CustomEncoderVideoBitRatePropertyA */
	c->gop = 32;		/* INF: AnalogVideoCompressionKeyframeRatePropertyA */
	c->quality = 6000;	/* INF: AnalogVideoCompressionQualityPropertyA */
	c->deinterlace = true;	/* registry default 1 */
	c->audio_channels = 2;
	c->h264 = false;
}

/*
 * ring: four video buffers and per-channel audio slots (~22 MB pinned).
 * Otherwise every slot of a pipe points at one buffer (~8 MB), which fits the
 * default 8 MB RLIMIT_MEMLOCK but can tear and drops frames under load.
 * The encoder pipes are only written when H.264 is enabled, so they share
 * one buffer unless it is.
 */
static int alloc_layout(struct mz_dev *m, struct stream *s, bool ring)
{
	s->nvideo = ring ? 4 : 1;
	s->nenc = s->cfg.h264 && ring ? 4 : 1;
	int r;
	for (unsigned i = 0; i < s->nvideo; i++)
		if ((r = vfio_dma_alloc(&m->vfio, &s->video[i], VIDEO_BUF_SIZE, IOVA_VIDEO + i * IOVA_STRIDE)))
			return r;
	unsigned nencpipes = s->nenc == 4 ? 2 : 1;
	for (unsigned e = 0; e < nencpipes; e++)
		for (unsigned i = 0; i < s->nenc; i++)
			if ((r = vfio_dma_alloc(&m->vfio, &s->enc[e][i], ENC_BUF_SIZE,
						IOVA_ENC + (e * 4 + i) * IOVA_STRIDE)))
				return r;
	if (nencpipes == 1)
		s->enc[1][0] = s->enc[0][0];	/* sub encoder reuses the main buffer */
	s->audio_shared = !ring;
	if ((r = vfio_dma_alloc(&m->vfio, &s->audio, ring ? 32 * AUDIO_BUF_SIZE : 4 * AUDIO_BUF_SIZE, IOVA_AUDIO)))
		return r;
	return 0;
}

int stream_alloc(struct mz_dev *m, struct stream *s, bool force_shared)
{
	memset(s, 0, sizeof(*s));
	stream_default_cfg(&s->cfg);
	if (!force_shared) {
		if (!alloc_layout(m, s, true))
			return 0;
		stream_free(m, s);
		struct stream_cfg cfg = s->cfg;
		memset(s, 0, sizeof(*s));
		s->cfg = cfg;
		fprintf(stderr, "stream: not enough lockable memory for the 4-buffer ring, using one shared buffer "
				"(raise RLIMIT_MEMLOCK to ~32 MB to rule out torn frames)\n");
	}
	return alloc_layout(m, s, false);
}

void stream_free(struct mz_dev *m, struct stream *s)
{
	for (unsigned i = 0; i < 4; i++) {
		vfio_dma_free(&m->vfio, &s->video[i]);
		vfio_dma_free(&m->vfio, &s->enc[0][i]);
		if (s->nenc == 4)
			vfio_dma_free(&m->vfio, &s->enc[1][i]);
		memset(&s->enc[1][i], 0, sizeof(s->enc[1][i]));
	}
	vfio_dma_free(&m->vfio, &s->audio);
}

static void put_addr(uint32_t *w, uint64_t iova)
{
	w[0] = iova >> 32;
	w[1] = (uint32_t)iova;
}

static int set_bufs(struct mz_dev *m, uint32_t cmd, uint32_t size, struct dma_buf *bufs, unsigned n)
{
	uint32_t w[11] = { cmd, 0, size };
	for (unsigned i = 0; i < 4; i++)
		put_addr(&w[3 + 2 * i], bufs[i % n].iova);
	return mz_cmd(m, w, 11, 5000);
}

int stream_register_buffers(struct mz_dev *m, struct stream *s)
{
	int r = set_bufs(m, MZ_CMD_SET_BUF_VIDEO, VIDEO_BUF_SIZE, s->video, s->nvideo);
	if (r)
		return r;
	bool shared_audio = s->audio_shared;
	for (uint32_t ch = 0; ch < 8; ch++) {
		uint64_t base = s->audio.iova + (shared_audio ? 0 : ch * 4 * AUDIO_BUF_SIZE);
		uint32_t w[11] = { MZ_CMD_SET_BUF_AUDIO, ch, AUDIO_BUF_SIZE };
		for (unsigned i = 0; i < 4; i++)
			put_addr(&w[3 + 2 * i], base + i * AUDIO_BUF_SIZE);
		if ((r = mz_cmd(m, w, 11, 5000)))
			return r;
	}
	if ((r = set_bufs(m, MZ_CMD_SET_BUF_ENC0, ENC_BUF_SIZE, s->enc[0], s->nenc)))
		return r;
	return set_bufs(m, MZ_CMD_SET_BUF_ENC1, ENC_BUF_SIZE, s->enc[1], s->nenc);
}

/* Aspect-ratio reduction used for the encoder command (StartFirmware). */
static void reduce(unsigned *a, unsigned *b)
{
	static const unsigned f[] = { 4, 2, 3, 5 };
	for (unsigned i = 0; i < 4; i++)
		if (*a % f[i] == 0 && *b % f[i] == 0) {
			*a /= f[i];
			*b /= f[i];
		}
}

int stream_start(struct mz_dev *m, struct stream *s, const struct video_mode *mode, uint8_t colorimetry)
{
	const struct stream_cfg *c = &s->cfg;
	s->mode = *mode;
	unsigned cx = mode->width, cy = mode->height, fps = mode->fps;
	bool interlaced = mode->interlaced;
	if (cx == 1440 && (cy == 240 || cy == 288) && !interlaced)
		cx = 720;
	if (interlaced) {	/* the card is configured with frame height and frame rate */
		cy *= 2;
		fps /= 2;
	}

	for (unsigned off = 0x50; off < 0x60; off += 4)
		mz_wr(m, off, 0);

	/* --- 0x29: video input --- */
	unsigned fw;		/* which encoder application ("tinyvenc5/7") the card uses */
	unsigned long px = (unsigned long)fps * cx * cy;
	if (px <= 0x41eb000 || interlaced || px >= 0x83d6001)
		fw = 5;
	else
		fw = 7;
	unsigned vi = interlaced ? 7 : 6;
	unsigned c424 = 0, c430 = 1, c42c = 1, c434 = 1;
	if (colorimetry == 1 || (colorimetry == 0 && cx * cy < 480000)) {
		/* NTSC-rate standard (AnalogVideoDecoderStandard & 0xa07) */
		bool std60 = fps >= 51 || (fps < 31 && fps > 25);
		c434 = std60 ? 6 : 5;
		c42c = 6;
		c430 = std60 ? 6 : 5;
		if (colorimetry == 1)
			c424 = 1;
	}
	if (!c424)
		c424 = 2;	/* CustomAnalogVideoRGBColorConvertColorRangeProperty == 0 */
	unsigned stride = (cx + 15) & ~15u;
	unsigned fk = 1;	/* INF: CustomAnalogVideoUStreamPatchProperty = 1 */
	uint32_t vin[10] = {
		MZ_CMD_SET_VIDEO_IN,
		vi << 24 | fw << 16 | fps << 8 | 0,
		cy << 16 | cx,
		mode->ntsc ? 1 : 0,			/* mirror 0, flip 0, 1000/1001 */
		c424 << 24 | c430 << 16 | c42c << 8 | c434,
		0,					/* ext sync start */
		cy << 16 | stride,			/* ext sync size */
		10 << 16 | 0 << 8 | (c->h264 ? 1 : 0),	/* OSD size 10, OSD off, bitstream_num */
		(uint32_t)colorimetry << 26 | fk << 8,
		0x80 << 24 | 0x80 << 16,		/* no-signal colour Y=0 U=V=0x80 */
	};
	int r = mz_cmd(m, vin, 10, 5000);
	if (r) {
		fprintf(stderr, "stream: SET_VIDEO_IN failed\n");
		return r;
	}

	/* --- 0x2a: audio input --- */
	unsigned chs = c->audio_channels == 8 ? 8 : 2;
	uint32_t ain[5] = {
		MZ_CMD_SET_AUDIO_IN,
		(chs | 0x1000) << 8 | 0,
		mode->audio_rate ? mode->audio_rate : 48000,
		4 << 16 | 0x100,			/* period: 4 x 256 samples */
		1 << 8 | 1,				/* interrupt mode, AIC on */
	};
	if ((r = mz_cmd(m, ain, 5, 5000))) {
		fprintf(stderr, "stream: SET_AUDIO_IN failed\n");
		return r;
	}

	/* --- 0x2d: H.264 encoders (main, then sub) --- */
	unsigned qp = c->quality / 250;
	if (qp > 0x28)
		qp = 0x28;
	qp = 0x30 - qp;
	for (unsigned sub = 0; sub < 2; sub++) {
		unsigned ax = 0, ay = 0;	/* CustomEncoderVideoAspectRatioProperty = 0 */
		reduce(&ax, &ay);
		uint32_t enc[11] = {
			MZ_CMD_SET_ENCODER,
			0x3fff,
			(c->gop << 8 | fps) << 16 | sub << 8 | 0,
			(0 | 0x100) << 16 | 0 << 8 | qp,	/* CBR mode 0, baseline/profile 0 */
			c->bitrate,
			7u << 24 | 0 << 16 | 0 << 8 | 0,	/* entropy 0, avg 0, skip 0 */
			ax << 16 | ay,
			0,					/* encoder resolution = input */
			0, 0,					/* crop */
			5 << 16 | 0x33,				/* QP 5..51 */
		};
		if ((r = mz_cmd(m, enc, 11, 5000))) {
			fprintf(stderr, "stream: SET_ENCODER(%u) failed\n", sub);
			return r;
		}
	}

	/* --- 0x31: raw preview --- */
	uint32_t pv[5] = {
		MZ_CMD_SET_PREVIEW,
		0x1f,
		0 << 24 | 0 << 16 | fps << 8 | 0,	/* avg 0, skip 0 */
		c->deinterlace ? 1 : 0,
		0,					/* no mirror/flip */
	};
	if ((r = mz_cmd(m, pv, 5, 5000))) {
		fprintf(stderr, "stream: SET_PREVIEW failed\n");
		return r;
	}

	/* Start from whatever the index registers hold from a previous session. */
	s->last_vidx = mz_rd(m, MZ_VIDEO_INDEX) & 3;
	s->last_aidx = mz_rd(m, MZ_AUDIO_INDEX) & 3;
	uint32_t go[2] = { MZ_CMD_START, 0xffffffff };
	if ((r = mz_cmd(m, go, 2, 5000)))
		fprintf(stderr, "stream: START failed\n");
	return r;
}

int stream_stop(struct mz_dev *m)
{
	uint32_t w[2] = { MZ_CMD_STOP, 0xffffffff };
	return mz_cmd(m, w, 2, 5000);
}

static void bar0_wr8(struct mz_dev *m, unsigned off, uint8_t v)
{
	((volatile uint8_t *)m->bar0)[off] = v;
	__sync_synchronize();
}

int stream_poll(struct mz_dev *m, struct stream *s, void (*cb)(const struct frame *, void *), void *arg)
{
	mz_lock(m);
	uint32_t irq = mz_rd(m, MZ_IRQ_STATUS);
	if (irq == 0xffffffff) {
		mz_unlock(m);
		return -ENODEV;
	}
	if (irq)
		mz_ack_irq(m);
	irq |= m->pending_irq;
	m->pending_irq = 0;
	uint32_t vidx_now = mz_rd(m, MZ_VIDEO_INDEX);
	mz_unlock(m);
	if (irq && s->irqlog) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		fprintf(s->irqlog, "%ld.%06ld %08x %u\n", (long)ts.tv_sec, ts.tv_nsec / 1000, irq, vidx_now & 3);
	}
	if (irq) {
		s->irq_seen |= irq;
		s->irq_count++;
	}

	int n = 0;
	unsigned cx = s->mode.width, cy = s->mode.height * (s->mode.interlaced ? 2 : 1);
	/*
	 * Raw video: the card writes the finished buffer index to BAR0+0x40
	 * before raising its interrupt, but an interrupt raised while another one
	 * is un-acked is held back and merged with the next event, so frames can
	 * arrive without their IRQ bit. Follow the index register instead and
	 * deliver every slot since the last one handed out.
	 */
	uint32_t vidx = mz_rd(m, MZ_VIDEO_INDEX) & 3;
	if (vidx != s->last_vidx) {
		unsigned count = (vidx - s->last_vidx) & 3;
		if (s->nvideo == 1)
			count = 1;	/* one shared buffer: only the newest frame exists */
		for (unsigned k = count; k > 0; k--) {
			unsigned slot = (vidx - (k - 1)) & 3;
			uint8_t *vb = s->video[slot % s->nvideo].virt;
			if (!(vb[4] & 1))
				continue;
			struct frame f = { FRAME_VIDEO, vb, ((cx + 15) & ~15u) * cy * 3 / 2, slot, false };
			cb(&f, arg);
			n++;
		}
		s->last_vidx = vidx;
	}
	if (irq & 1) {
		for (unsigned e = 0; e < 2; e++) {
			uint32_t eidx = mz_rd(m, e ? MZ_INDEX_2 : MZ_INDEX_1) & 3;
			volatile uint32_t *hdr = s->enc[e][eidx % s->nenc].virt;
			uint32_t len = hdr[4];
			if (!len)
				continue;
			if (len > ENC_BUF_SIZE - ENC_HDR_SIZE)
				len = ENC_BUF_SIZE - ENC_HDR_SIZE;
			struct frame f = { FRAME_H264, (const uint8_t *)hdr + ENC_HDR_SIZE, len, eidx | e << 8,
					   hdr[5] & 1 };
			cb(&f, arg);
			hdr[0] = 0;
			hdr[4] = 0;
			bar0_wr8(m, e ? 0x58 : 0x50, 0);
			n++;
		}
	}
	/*
	 * Audio: same scheme with BAR0+0x4c (channel 0 in bits 0..1). Each slot
	 * holds one 4096-byte period.
	 */
	uint32_t aidx = mz_rd(m, MZ_AUDIO_INDEX) & 3;
	if (aidx != s->last_aidx) {
		unsigned count = (aidx - s->last_aidx) & 3;
		for (unsigned k = count; k > 0; k--) {
			unsigned slot = (aidx - (k - 1)) & 3;
			struct frame f = { FRAME_AUDIO, (uint8_t *)s->audio.virt + slot * AUDIO_BUF_SIZE,
					   AUDIO_PERIOD_BYTES, slot, false };
			cb(&f, arg);
			n++;
		}
		s->last_aidx = aidx;
	}
	return n;
}
