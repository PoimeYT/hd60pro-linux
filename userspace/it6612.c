/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * IT6612 passthrough, transliterated from the CIT6612 class of
 * e60MZ0380.X64.SYS (HD60 Pro paths only). Object offsets are kept as in the
 * driver: B()/I() address the same fields the decompiled code uses.
 */
#include "it6612.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "it6612_tables.h"

#define B(o)	(t->st[o])
#define C(o)	((int8_t)t->st[o])
#define I(o)	(*(int32_t *)&t->st[o])
#define U16(o)	(*(uint16_t *)&t->st[o])

enum { MODE_NOSIG_C = 'C', MODE_NOSIG = 'D', MODE_E = 'E', MODE_F = 'F' };

static void msleep(unsigned ms)
{
	usleep(ms * 1000);
}

static uint8_t R98(struct it6612 *t, uint8_t reg)
{
	int v = mz_i2c_read(t->m, I2C_IT6612, reg);
	return v < 0 ? 0 : (uint8_t)v;
}

static void W98(struct it6612 *t, uint8_t reg, uint8_t val)
{
	mz_i2c_write(t->m, I2C_IT6612, reg, val);
}

static void RN98(struct it6612 *t, uint8_t reg, uint8_t *buf, unsigned len)
{
	for (unsigned i = 0; i < len; i++)
		buf[i] = R98(t, reg + i);
}

static void WN98(struct it6612 *t, uint8_t reg, const uint8_t *buf, unsigned len)
{
	for (unsigned i = 0; i < len; i++)
		W98(t, reg + i, buf[i]);
}

static uint8_t R9C(struct it6612 *t, uint8_t bank, uint8_t reg)
{
	int v = mz_bank_read(t->m, I2C_MST3367, bank, reg);
	return v < 0 ? 0 : (uint8_t)v;
}

static void W9C(struct it6612 *t, uint8_t bank, uint8_t reg, uint8_t val)
{
	mz_bank_write(t->m, I2C_MST3367, bank, reg, val);
}

static const uint8_t *mode_entry(unsigned idx)
{
	return &InputModeTable[idx * 16];
}

static uint32_t mode_pclk(unsigned idx)
{
	const uint8_t *e = mode_entry(idx);
	return e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24;
}

static uint8_t mode_flags(unsigned idx)
{
	return mode_entry(idx)[12];
}

/* ---- small register helpers ---- */

static void set_av_mute(struct it6612 *t)
{
	W98(t, 0x0f, 0);
	W98(t, 0xc1, 1);
	W98(t, 0xc6, 3);
}

static void sw_reset(struct it6612 *t)
{
	W98(t, 0x04, 0x3d);
	msleep(5);
	W98(t, 0x04, 0x1d);
	W98(t, 0x61, 0x30);
	W98(t, 0x09, 0xb2);
	W98(t, 0x0a, 0xf8);
	W98(t, 0x0b, 0x37);
	for (uint8_t r = 0xc9; r <= 0xd0; r++)
		W98(t, r, 0);
}

static void init_sys(struct it6612 *t)
{
	I(4) = 0;
	B(0x12) = 0;
	uint8_t id1 = R98(t, 1), id2 = R98(t, 2), id3 = R98(t, 3), r5 = R98(t, 5);
	if (id1 == 0xca) {
		if (id2 == 0x11 && id3 == 0x16 && (r5 & 0x20))
			B(0x12) = 1;
	} else if (id1 == 'I' && id2 == 0x12 && id3 == 0x16) {
		B(0x12) = 2;
	}
	if (B(0x12)) {
		W98(t, 5, 0);
		B(0x1f) = 1;
		B(0x20) = 0;
		sw_reset(t);
	}
}

static void abort_ddc(struct it6612 *t)
{
	uint8_t r4 = R98(t, 4), r20 = R98(t, 0x20);
	R98(t, 0x10);
	W98(t, 0x65, 2);
	W98(t, 0x20, r20 & 0xfe);
	W98(t, 0x04, r4 | 1);
	W98(t, 0x10, 1);
	for (int pass = 0; pass < 2; pass++) {
		W98(t, 0x15, 0x0f);
		for (int i = 0; i < 200; i++) {
			msleep(1);
			uint8_t s = R98(t, 0x16);
			if ((s & 0x80) || (s & 0x38))
				break;
		}
	}
	W98(t, 0x65, 0);
}

/* ---- HDCP ---- */

static void hdcp_reset_auth(struct it6612 *t)
{
	W98(t, 0x22, 0);
	W98(t, 0x20, 0);
	W98(t, 0x04, R98(t, 4) | 1);
	W98(t, 0x10, 1);
	W98(t, 0x0a, R98(t, 0x0a) & 0xf8);
	W98(t, 0x0c, 0x1c);
	W98(t, 0x0d, 0);
	W98(t, 0x0e, 1);
	abort_ddc(t);
}

static bool ddc_wait(struct it6612 *t, uint8_t *status)
{
	for (int i = 0; i < 200; i++) {
		msleep(1);
		*status = R98(t, 0x16);
		if ((*status & 0x80) || (*status & 0x38))
			return !(*status & 0x38);
	}
	return false;
}

static int hdcp_authenticate(struct it6612 *t)
{
	int i;
	for (i = 0; i < 10; i++)
		if (R98(t, 0x0e) & 0x10)
			break;
	if (i == 10)
		return 1;
	I(0x24) = 0;
	W98(t, 0x04, R98(t, 4) | 1);
	W98(t, 0x20, 0);
	W98(t, 0x22, 0);
	W98(t, 0x10, 1);
	W98(t, 0x10, 1);
	W98(t, 0x15, 9);
	abort_ddc(t);
	W98(t, 0x0f, 0);

	uint8_t st, bcaps = 0;
	for (i = 0; i < 40; i++) {	/* read Bcaps/Bstatus until HDMI mode agrees */
		msleep(15);
		W98(t, 0x10, 1);
		W98(t, 0x11, 0x74);
		W98(t, 0x12, 0x40);
		W98(t, 0x13, 3);
		W98(t, 0x15, 0);
		if (!ddc_wait(t, &st))
			goto fail;
		bcaps = R98(t, 0x45);
		R98(t, 0x44);
		R98(t, 0x43);
		bool hdmi = R98(t, 0xc0) & 1;
		if (hdmi == !!(bcaps & 0x10))
			break;
	}
	uint8_t bksv[5] = { 0 };
	W98(t, 0x10, 1);
	W98(t, 0x11, 0x74);
	W98(t, 0x12, 0);
	W98(t, 0x13, 5);
	W98(t, 0x15, 0);
	if (ddc_wait(t, &st))
		RN98(t, 0x3b, bksv, 5);
	int ones = 0;
	for (i = 0; i < 5; i++)
		ones += __builtin_popcount(bksv[i]);
	if (ones != 20)
		goto fail;

	W98(t, 0x0f, 0);
	W98(t, 0x04, R98(t, 4) & 0xfe);
	W98(t, 0x1f, 1);
	msleep(1);
	W98(t, 0x1f, 0);
	uint8_t an[8];
	RN98(t, 0x30, an, 8);
	WN98(t, 0x28, an, 8);
	W98(t, 0x22, 0);
	W98(t, 0x20, 1);
	W98(t, 0x0a, R98(t, 0x0a) & 0xf8);
	W98(t, 0x0c, 0x1c);
	W98(t, 0x0d, 0);
	W98(t, 0x0e, 1);
	I(0x24) = 0;
	W98(t, 0x10, 1);
	W98(t, 0x15, 9);
	W98(t, 0x10, 0);
	W98(t, 0x21, 1);
	for (i = 0; i < 100; i++) {
		msleep(5);
		if (R98(t, 0x46) & 0x80) {
			I(0x24) = 1;
			return 0;
		}
		if (R98(t, 7) & 1) {	/* authentication failed */
			W98(t, 0x0c, 4);
			W98(t, 0x0d, 0);
			W98(t, 0x0e, 1);
			W98(t, 0x0e, 0);
			break;
		}
	}
fail:
	I(0x24) = 0;
	return 1;
}

static bool hdcp_wanted(struct it6612 *t)
{
	return t->input_hdcp != 0;	/* dev+0x7820 set, dev+0x6634 == 0 */
}

static void enable_hdcp(struct it6612 *t, bool on)
{
	if (!on) {
		I(0x24) = 0;
		hdcp_reset_auth(t);
		return;
	}
	if (hdcp_wanted(t) && hdcp_authenticate(t) == 1) {
		set_av_mute(t);
		hdcp_reset_auth(t);
	}
}

/* ---- AV mute release (ClrAVMute / ClrAVMute1) ---- */

static void clr_av_mute1(struct it6612 *t)
{
	W98(t, 0x0f, 0);
	W98(t, 0xc1, 0);
	W98(t, 0xc6, 3);
	if (B(0xe) == MODE_E || B(0xe) == MODE_F) {
		W9C(t, 0, 0xb2, R9C(t, 0, 0xb2) | 3);
		return;
	}
	uint8_t b2;
	if (B(2) == 1) {
		uint8_t s = R9C(t, 0, 0x55);
		b2 = R9C(t, 0, 0xb2) & 0xfc;
		if ((s & 4) && !(s & 1))
			b2 |= 1;
		if (s & 8) {
			if (!(s & 2))
				b2 |= 2;
			else
				b2 &= 0xfd;
		}
	} else {
		uint8_t s = R9C(t, 0, 0x14);
		b2 = R9C(t, 0, 0xb2);
		if (s & 0x10)
			b2 = (s & 4) ? (b2 & 0xfe) : (b2 | 1);
		if (s & 0x80)
			b2 = (s & 0x20) ? (b2 & 0xfd) : (b2 | 2);
	}
	W9C(t, 0, 0xb2, b2);
}

static void clr_av_mute(struct it6612 *t)
{
	if (B(2) && B(0xe) != MODE_E && B(0xe) != MODE_F && I(0x24) != 1 && hdcp_wanted(t)) {
		I(0x24) = 0;
		hdcp_reset_auth(t);
		enable_hdcp(t, true);
		msleep(100);
		if (I(0x24) != 1) {
			set_av_mute(t);
			return;
		}
	}
	clr_av_mute1(t);
}

/* ---- receiver-side helpers (program the MST3367 output for the TX) ---- */

static bool check_signal(struct it6612 *t, uint8_t mode)
{
	B(2) = 0;
	if (mode == MODE_E || mode == MODE_F) {
		B(1) = 1;
		return false;
	}
	B(1) = 0;
	if (R9C(t, 0, 0x51) & 0x80) {
		if (R9C(t, 1, 1) & 4)
			B(2) = 1;	/* HDMI (not DVI) input */
		return (R9C(t, 0, 0x55) & 0x3c) == 0x3c;
	}
	uint8_t s = R9C(t, 0, 0x14);
	if (!(s & 0x80)) {
		uint8_t s2 = R9C(t, 0, 0x15);
		if ((s & 0x10) || !(s2 & 0x40))
			return false;
	}
	return true;
}

static void set_data_latch_timing(struct it6612 *t, uint8_t delay, uint8_t mode)
{
	if (mode == MODE_E || mode == MODE_F)
		return;
	if (delay > 0xc)
		delay -= 0xc;
	uint8_t b2 = R9C(t, 0, 0xb2) & 0xf7, b5 = R9C(t, 0, 0xb5);
	if (delay > 5) {
		b2 |= 8;
		delay -= 6;
	}
	W9C(t, 0, 0xb2, b2);
	W9C(t, 0, 0xb5, delay << 2 | (b5 & 0xc3));
}

static void set_pll_charge_pump(struct it6612 *t, unsigned idx, uint8_t mode)
{
	uint8_t v;
	if (mode == MODE_E || mode == MODE_F)
		return;
	switch (mode) {
	case 0x00: case 0x01: case '<': case '>':
		v = clock_13_27[(idx + 1) & 0xf];
		break;
	case 0x02: case 0x03:
		v = clock_75[idx & 0xf];
		break;
	case 0x0e: case 0x0f:
		v = clock_150[(idx + 1) & 7];
		break;
	case 0x10: case 0x11:
		v = clock_13_27[(idx - 7) & 0xf];
		break;
	default:
		v = clock_75[(idx - 8) & 0xf];
	}
	W9C(t, 0, 3, v);
}

static void rx_adc_set_coast(struct it6612 *t, uint8_t mode)
{
	for (int i = 0; i < 3; i++) {
		const uint8_t *e = CoastTable[i];
		uint8_t v = e[2] == 0xff ? e[3] : ((R9C(t, e[0], e[1]) & ~e[2]) | e[3]);
		W9C(t, e[0], e[1], v);
	}
	uint8_t v13;
	if (mode < 0x10 || mode == 0x10 || mode == 0x11) {
		W9C(t, 0, 0x12, (mode == 0 || mode == 1 || mode == 0x10 || mode == 0x11) ? 4 : 0);
		v13 = 0x14;
	} else {
		v13 = 4;
	}
	W9C(t, 0, 0x13, v13);
}

/* GetHDMIDotClock: audio rate from the receiver's ACR, then the pixel clock. */
static uint32_t get_dot_clock(struct it6612 *t, uint32_t *audio_hz)
{
	uint16_t acr = (R9C(t, 2, 0x11) | R9C(t, 2, 0x12) << 8) & 0x7fff;
	uint32_t fs = 0xffff;
	for (int i = 0; i < 7; i++)
		if ((int)acr >= AudioPLL[i] - 0x1e && acr <= AudioPLL[i] + 0x1e) {
			fs = SampleFreqs[i];
			break;
		}
	I(0x1bc) = fs;
	if (fs == 0xffff || !B(2)) {
		*audio_hz = 0;
		I(0x18) = 0;
		return 0;
	}
	*audio_hz = fs * 100;
	uint32_t n = ((R9C(t, 2, 0x42) & 0xf) << 8 | R9C(t, 2, 0x41)) << 8 | R9C(t, 2, 0x40);
	uint32_t cts = ((R9C(t, 2, 0x45) & 0xf) << 8 | R9C(t, 2, 0x44)) << 8 | R9C(t, 2, 0x43);
	if (!cts)
		cts = 1;
	uint8_t dc = R9C(t, 2, 0x47) & 0xf;
	uint32_t clk = (uint32_t)(((uint64_t)fs * n * 0x80 + cts * 5ull) / (cts * 10ull));
	if (dc == 5)
		clk = clk * 4 / 5;
	else if (dc == 6)
		clk = clk * 2 / 3;
	else if (dc == 7)
		clk >>= 1;
	I(0x18) = clk;
	for (int i = 0; i < 16; i++) {
		uint32_t c = CEA861DotClock[i];
		if (c - c / 2000 <= clk && clk <= c + c / 2000) {
			clk = c;
			if (i < 2)
				clk = c / ((R9C(t, 2, 0x4c) & 0xf) + 1);
			B(0x1c) = CEA861DotClockRep[i];
			break;
		}
	}
	return clk;
}

static unsigned get_vertical_freq(struct it6612 *t)
{
	unsigned cnt = R9C(t, 0, 0x5a) + (R9C(t, 0, 0x59) & 0x3f) * 0x100, f = 0;
	if (cnt > 1) {
		f = (1250000 / (cnt - 1)) & 0xffff;
		if (f - 0x1eb < 0x13)
			return 5000;
		if (f - 0xf7 < 8)
			return 2500;
	}
	uint32_t clk = 0, a;
	if (B(2) != 1 || !(clk = get_dot_clock(t, &a))) {
		if (f - 0x24f < 0x13)
			return cnt < 0x826 ? 6000 : 5994;
		if (f - 0xec < 9)
			return cnt > 0x1460 ? 2397 : 2400;
		if (f - 0x123 < 0x13)
			return cnt > 0x104d ? 2997 : 3000;
	}
	if (f - 0x24f >= 0x13) {
		if (f - 0xec < 9)
			return clk > 0xb52d4 ? 2400 : 2397;
		if (f - 0x123 >= 0x13)
			return 5000;
		return clk > 0xb52d4 ? 3000 : 2997;
	}
	static const uint32_t lim[][2] = { { 1000000, 0x16a5a8 }, { 700000, 0xb52d4 }, { 500000, 0x83e5a },
					   { 260000, 0x41f37 } };
	for (int i = 0; i < 4; i++)
		if (clk > lim[i][0])
			return clk > lim[i][1] ? 6000 : 5994;
	return clk > 0x3d7e8 ? 6000 : 5994;
}

/* ---- video ---- */

static uint8_t get_current_vic(struct it6612 *t, uint8_t fallback)
{
	for (int i = 0; i <= 100; i++) {
		if (R9C(t, 2, 0x0b) & 8)
			return R9C(t, 2, 0x4b) & 0x7f;
		msleep(5);
	}
	return fallback;
}

static void set_avi_infoframe(struct it6612 *t, const uint8_t *p)
{
	W98(t, 0x0f, 1);
	static const uint8_t regs[] = { 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5e, 0x5f, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65 };
	uint8_t sum = 0;
	for (int i = 0; i < 13; i++) {
		W98(t, regs[i], p[3 + i]);
		sum += p[3 + i];
	}
	W98(t, 0x5d, (uint8_t)(0x6f - sum));	/* 0x100 - (0x82 + 2 + 0x0d) - payload */
	W98(t, 0x0f, 0);
	W98(t, 0xcd, 3);
}

static void config_info_frame(struct it6612 *t, bool hd, bool full_range)
{
	uint8_t p[16] = { 0 };
	uint8_t y;
	switch (B(0x10) & 3) {
	case 0: y = 0x10; break;
	case 1: y = 0x30; break;
	default: y = 0x50; break;
	}
	p[3] = y;
	uint8_t m = I(0x3c) ? 0x20 : 0x10;
	uint8_t b4 = m | (hd ? 0x40 : 0);
	if (!hd)
		b4 = m | ((mode_flags(I(0x1c4)) & 8) ? 0x80 : 0x40);
	p[4] = b4 | 0x08;
	p[5] = full_range ? 4 : 0;
	p[6] = B(0);
	p[7] = (mode_flags(I(0x1c4)) >> 1) & 0x37;
	set_avi_infoframe(t, p);
}

static void init_embedded_video_mode(struct it6612 *t, uint8_t vic, uint8_t flags)
{
	if (!(flags & 2))
		return;
	unsigned i;
	for (i = 0; SyncEmbTable[i * 24] != 0xff; i++)
		if (SyncEmbTable[i * 24] == vic)
			break;
	const uint8_t *e = &SyncEmbTable[i * 24];
	if (e[0] == 0xff)
		return;
	const uint8_t *s = (!(flags & 1) || B(0x12) == 2) ? &e[1] : &e[10];
	W98(t, 0x90, s[0]);
	W98(t, 0x91, s[1]);
	W98(t, 0x95, s[2]);
	W98(t, 0x96, s[3]);
	W98(t, 0x97, s[4]);
	W98(t, 0xa0, e[6]);
	W98(t, 0xa1, e[7]);
	W98(t, 0xa2, e[8]);
	W98(t, 0xa3, e[9]);
	if (B(0x12) == 2) {
		if (flags & 1) {
			W98(t, 0x95, e[3] + 1);
			W98(t, 0x96, e[4] + 1);
		}
		W98(t, 0xa6, vic == 6 ? 0 : 0x30);
		W98(t, 0xb1, 0x0c);
		W98(t, 0xb2, 0xea);
	}
}

static void set_csc_scale(struct it6612 *t, uint8_t in, uint8_t out)
{
	uint8_t mode = 0, bits = 0;
	bool matrix = false;
	unsigned ii = in & 3, oo = out & 3;
	uint8_t dith = (in & 0x40) ? 0x40 : 0, filt = (in & 0x80) ? 0xa0 : 0;
	if (ii == 0) {		/* RGB in */
		if (oo == 1) { bits = dith | filt; mode = 2; }
		else if (oo == 2) { bits = filt; mode = 2; }
	} else if (ii == 1) {	/* YUV422 in */
		if (oo == 0) { bits = dith | filt; mode = 3; matrix = true; }
		else if (oo == 2) { bits = dith | filt; mode = 0; }
	} else if (ii == 2) {	/* YUV444 in */
		if (oo == 0) { bits = filt; mode = 3; matrix = true; }
		else if (oo == 1) { bits = dith; mode = 0; }
	}
	if (matrix) {
		/* Colorimetry from the mode table (the driver's deterministic branch). */
		bool bt709 = (mode_flags(I(0x1c4)) >> 3) & 1;
		WN98(t, 0x73, bCSCOffset_16_235, 3);
		WN98(t, 0x76, bt709 ? bCSCMtx_YUV2RGB_ITU709_16_235 : bCSCMtx_YUV2RGB_ITU601_16_235, 0x12);
	}
	W98(t, 0x72, (R98(t, 0x72) & 0x1c) | mode | bits);
	if (B(0x12) == 2) {
		uint8_t f = R98(t, 0x0f);
		W98(t, 0x0f, mode == 0 ? (f | 0x10) : (f & 0xef));
	}
}

static void setup_afe(struct it6612 *t, bool high_clock)
{
	W98(t, 0x61, 0x10);
	if (B(0x12) == 1) {
		W98(t, 0x63, 1);
		W98(t, 0x64, high_clock ? 0x56 : 0x1e);
		W98(t, 0x65, 0);
		W98(t, 0x04, R98(t, 4) & 0xc5);
		W98(t, 0x62, (high_clock ? 0x88 : 0x18) | 1);
		return;
	}
	if (!high_clock) {
		W98(t, 0x62, (R98(t, 0x62) & 0x6f) | 0x10);
		W98(t, 0x64, (R98(t, 0x64) & 0x76) | 9);
		W98(t, 0x68, R98(t, 0x68) | 0x10);
	} else {
		W98(t, 0x62, (R98(t, 0x62) & 0xef) | 0x80);
		W98(t, 0x64, (R98(t, 0x64) & 0xf6) | 0x80);
		W98(t, 0x68, (R98(t, 0x68) & 0xef) | 0x80);
	}
	W98(t, 0x04, R98(t, 4) & 0xc5);
	W98(t, 0x59, R98(t, 0x59) | 8);
}

static void init_video_mode(struct it6612 *t)
{
	W98(t, 0x04, 0x1d);
	if (I(0x28))
		set_av_mute(t);
	B(0x11) = I(0x30) ? (B(0x11) | 2) : (B(0x11) & 0xfd);
	B(0x11) = I(0x34) == 1 ? (B(0x11) | 1) : (B(0x11) & 0xfe);
	uint8_t r70 = R98(t, 0x70) & 0x23;
	if ((B(0xf) & 3) == 1)
		r70 |= 0x40;
	else if ((B(0xf) & 3) == 2)
		r70 |= 0x80;
	if (B(0x11) & 1)
		r70 |= 0x10;
	if (B(0x11) & 2)
		r70 |= 8;
	if (B(0x11) & 4)
		r70 |= 4;
	W98(t, 0x70, r70);
	if (!(B(0x11) & 2)) {
		W98(t, 0x90, 0);
		for (uint8_t r = 0x91; r < 0xa4; r++)
			W98(t, r, 0xff);
		W98(t, 0xc0, R98(t, 0x90) | 8);
		W98(t, 0xa2, 0xff);
		W98(t, 0xc0, R98(t, 0xa3) | 0xf);
	} else {
		init_embedded_video_mode(t, B(0), B(0x11));
	}
	set_csc_scale(t, B(0xf), B(0x10));
	W98(t, 0xc0, I(0x28) ? 1 : 0);	/* HDMI vs DVI output */
	if (B(0x12) == 2) {
		W98(t, 0x0f, R98(t, 0x0f) | 1);
		W98(t, 0x58, I(0x28) ? 0x10 : 0);
		W98(t, 0x0f, R98(t, 0x0f) & 0xfe);
		if (I(0x28)) {
			W98(t, 0xc1, R98(t, 0xc1) | 3);
			W98(t, 0xc6, R98(t, 0xc6) | 3);
		} else {
			W98(t, 0xc1, (R98(t, 0xc1) & 0xfe) | 2);
			W98(t, 0xc6, R98(t, 0xc6) & 0xfc);
		}
		W98(t, 0x71, (R98(t, 0x71) & 3) | 0x1c);
	}
	setup_afe(t, mode_pclk(I(0x1c4)) > 80000000);
}

/* ---- audio ---- */

static void set_ncts(struct it6612 *t, uint32_t pclk, uint32_t fs)
{
	unsigned vf = get_vertical_freq(t);
	bool frac = vf % 100 != 0;	/* 59.94 etc */
	uint32_t n;
	switch (fs) {
	case 32000:
		n = frac ? 0x2d80 : 0x1000;
		break;
	case 44100: case 88200: case 176400:
		n = frac ? (pclk > 145000000 ? 0x22d6 : 0x45ac) : 0x1880;
		n *= fs / 44100;
		break;
	case 48000: case 96000: case 192000:
		n = frac ? (pclk > 145000000 ? 0x16c0 : 0x2d80) : 0x1800;
		n *= fs / 48000;
		break;
	default:
		n = (fs << 8) / 2000;
	}
	W98(t, 0x0f, 1);
	W98(t, 0x33, n);
	W98(t, 0x34, n >> 8);
	W98(t, 0x35, (n >> 16) & 0xf);
	W98(t, 0x0f, 0);
	W98(t, 0xc5, 0);
	W98(t, 0x58, (R98(t, 0x58) & 0xf7) | 4);
}

static void set_audio_format(struct it6612 *t, uint8_t chans, uint8_t enable, uint32_t fs, uint8_t bits)
{
	uint8_t cs;
	switch (fs) {
	case 88200: cs = 8; break;
	case 32000: cs = 3; break;
	case 44100: cs = 0; break;
	case 48000: cs = 2; break;
	case 96000: cs = 10; break;
	case 176400: cs = 0xc; break;
	case 192000: cs = 0xe; break;
	default: cs = 1;
	}
	uint8_t fifo, layout;
	if (chans >= 7) { fifo = 0xf; layout = 4; }
	else if (chans >= 5) { fifo = 0xb; layout = 3; }
	else if (chans >= 3) { fifo = 9; layout = 2; }
	else { fifo = 8; layout = 1; }
	if (enable & 0x10)
		enable &= 0xf0;
	enable &= 0x1f;
	uint8_t wl;
	switch (bits) {
	case 16: wl = 2; break;
	case 18: wl = 4; enable |= 0x40; break;
	case 20: wl = 10; enable |= 0x80; break;
	case 24: wl = 0xb; enable |= 0xc0; break;
	default: return;
	}
	W98(t, 0x0f, 0);
	W98(t, 0x04, R98(t, 4) | 4);
	W98(t, 0xe2, 0xe4);
	W98(t, 0xe3, (B(0x20) & 0xf) | (enable & 0x10));
	W98(t, 0xe4, fifo);
	W98(t, 0xe0, enable);
	W98(t, 0xe1, B(0x1f));
	W98(t, 0x0f, 1);
	W98(t, 0x91, chans == 1);
	W98(t, 0x92, 0);
	W98(t, 0x93, layout);
	W98(t, 0x94, 0x21);
	W98(t, 0x95, 0x43);
	W98(t, 0x96, 0x65);
	W98(t, 0x97, 0x87);
	W98(t, 0x98, cs);
	W98(t, 0x99, (uint8_t)(~cs << 4) | wl);
	W98(t, 0x0f, 0);
	W98(t, 0x04, R98(t, 4) & 4);
}

static void config_audio_infoframe(struct it6612 *t)
{
	W98(t, 0x0f, 1);
	W98(t, 0x68, 1);
	W98(t, 0x6b, 0);
	W98(t, 0x6c, 0);
	W98(t, 0x6d, 0x70);
	W98(t, 0x0f, 0);
	W98(t, 0xce, 3);
}

static void init_audio_mode(struct it6612 *t)
{
	uint8_t en;
	switch (B(0x1e)) {
	case 3: case 4: en = 3; break;
	case 5: case 6: en = 7; break;
	case 7: case 8: en = 0xf; break;
	default: en = 1;
	}
	set_ncts(t, mode_pclk(I(0x1c4)), I(0x14));
	set_audio_format(t, B(0x1e), en, I(0x14), B(0x1d));
	config_audio_infoframe(t);
	W98(t, 0x04, R98(t, 4) & 0xfb);
}

static void select_audio_format(struct it6612 *t, uint8_t idx)
{
	if (B(0xcd)) {
		B(0x45) = idx;
		I(0x14) = 192000;
		B(0x1d) = 24;
		B(0x1e) = (B(0xce + 3 * idx) & 7) + 1;
		uint8_t rates = B(0xcf + 3 * idx);
		if (rates & 0x40)
			I(0x14) = 192000;
		else if (rates & 0x10)
			I(0x14) = 96000;
		else if (rates & 4)
			I(0x14) = 48000;
	}
	if (B(2) == 1) {
		uint32_t fs;
		get_dot_clock(t, &fs);
		I(0x14) = fs;
	} else {
		I(0x14) = 48000;
	}
	B(0x1d) = 24;
}

/* ---- EDID ---- */

static int read_edid(struct it6612 *t, uint8_t *buf, uint8_t segment, uint8_t offset, int len)
{
	if (R98(t, 6) & 4)
		abort_ddc(t);
	W98(t, 0x10, 1);
	W98(t, 0x15, 9);
	W98(t, 0x0f, 0);
	while (len > 0) {
		int n = len > 32 ? 32 : len;
		W98(t, 0x10, 1);
		W98(t, 0x15, 9);
		W98(t, 0x65, 2);
		W98(t, 0x10, 1);
		W98(t, 0x11, 0xa0);
		W98(t, 0x12, offset);
		W98(t, 0x13, n);
		W98(t, 0x14, segment);
		W98(t, 0x15, 3);
		offset += n;
		len -= n;
		uint8_t st;
		if (!ddc_wait(t, &st)) {
			W98(t, 0x65, 0);
			return 1;
		}
		while (n-- > 0)
			*buf++ = R98(t, 0x17);
	}
	W98(t, 0x65, 0);
	return 0;
}

static void parse_cea_edid(struct it6612 *t, const uint8_t *e)
{
	if (e[0] != 2 || e[1] != 3)
		return;
	uint8_t end = e[2];
	B(0x4a) = e[3];
	if (e[3] & 0x40)
		I(0x2c) = 1;
	B(0x4b) = 0;
	B(0x4c) = 0xff;
	unsigned p = 4;
	while (p < end && p < 128) {
		uint8_t len = e[p] & 0x1f, tag = e[p] >> 5;
		switch (tag) {
		case 1: {	/* audio */
			p++;
			B(0x45) = 0;
			B(0xcd) = len / 3;
			for (int i = 0; i < B(0xcd); i++) {
				B(0xce + 3 * i) = e[p];
				B(0xcf + 3 * i) = e[p + 1];
				B(0xd0 + 3 * i) = e[p + 2];
				p += 3;
			}
			break;
		}
		case 2:		/* video */
			p++;
			B(0x4c) = 0xff;
			for (int i = 0; i < len; i++, p++) {
				uint8_t vic = e[p] & 0x7f;
				for (unsigned k = 0; InputModeTable[k * 16] != 0x22; k++)
					if (InputModeTable[k * 16 + 4] == vic) {
						B(0x4d + B(0x4b)) = vic;
						if (e[p] & 0x80) {
							B(0x4c) = B(0x4b);
							B(0x44) = B(0x4b);
						}
						B(0x4b)++;
						break;
					}
			}
			break;
		case 3: {	/* vendor specific */
			unsigned q = p + 1;
			if (e[q] == 3) {
				I(0x130) = e[q] | e[q + 1] << 8 | e[q + 2] << 16;
				U16(0x134) = 0xffff;
				if (I(0x130) == 0x0c03) {
					I(0x28) = 1;	/* HDMI sink */
					if (len > 4)
						U16(0x134) = e[q + 3] << 8 | e[q + 4];
				} else {
					I(0x28) = 0;
				}
			}
			p = q + len;
			break;
		}
		case 4:		/* speaker allocation */
			B(0x136) = e[p + 1];
			B(0x137) = e[p + 2];
			B(0x138) = e[p + 3];
			p += 4;
			break;
		default:
			p += len + 1;
		}
	}
	B(0x48) = 1;
}

static int parse_edid(struct it6612 *t)
{
	uint8_t *blk = &B(0x13c);
	B(0x48) = 0;
	if (read_edid(t, blk, 0, 0, 128))
		return 0;
	uint8_t sum = 0;
	for (int i = 0; i < 128; i++)
		sum += blk[i];
	static const uint8_t hdr[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
	if (sum || memcmp(blk, hdr, 8) || (!(blk[0x14] & 0x80) && !(R98(t, 0x0e) & 0x20)))
		return 0;
	if (t->verbose)
		printf("IT6612: EDID OK, manufacturer %c%c%c, %u extension(s)\n", '@' + ((blk[8] >> 2) & 0x1f),
		       '@' + (((blk[8] & 3) << 3) | (blk[9] >> 5)), '@' + (blk[9] & 0x1f), blk[0x7e]);
	unsigned ext = blk[0x7e];
	if (!ext)
		return 1;
	if (ext > 4)
		ext = 4;
	bool found = false;
	for (unsigned n = 1; n <= ext; n++) {
		if (read_edid(t, blk, n / 2, (n & 1) << 7, 128))
			continue;
		if (!found && blk[0] == 2 && blk[1] == 3) {
			parse_cea_edid(t, blk);
			B(0x49) = I(0x130) == 0x0c03;
			found = B(0x49);
		}
	}
	return 1;
}

/* ---- state machine (RXInterrupt_Handler / RXVideo_Handler / SelectVideoMode) ---- */

static void select_video_mode(struct it6612 *t)
{
	unsigned i;
	for (i = 0;; i++) {
		const uint8_t *e = mode_entry(i);
		if (e[4] == B(0))
			break;
		if (e[4] == 0xff || mode_pclk(i) == 0) {
			i = 0;
			break;
		}
	}
	I(0x1c4) = i;
	B(0xd) = B(0xe);
	B(0xc) = B(0);
	const uint8_t *e = mode_entry(i);
	I(0x40) = e[0] | e[1] << 8 | e[2] << 16 | e[3] << 24;
	I(0x3c) = mode_flags(i) & 1;
	select_audio_format(t, B(0x45));
	I(4) = 4;
}

static void irq_handler(struct it6612 *t)
{
	if (!(R98(t, 0x0e) & 0x80))
		return;
	uint8_t clr0 = 0, clr1 = 0, keep = 0;
	uint8_t s6 = R98(t, 6);
	if (s6 & 0x80)
		I(8) = 3;
	if (s6 & 0x10) {
		W98(t, 0x10, 1);
		W98(t, 0x15, 9);
	}
	if (s6 & 4) {
		abort_ddc(t);
		if (I(0x24)) {	/* hdmitx_hdcp_ResumeAuthentication */
			set_av_mute(t);
			hdcp_authenticate(t);
			clr_av_mute(t);
		}
	}
	if (s6 & 1) {	/* hot plug change */
		clr0 = 1;
		if (!(R98(t, 0x0e) & 0x40)) {
			W98(t, 0x04, 0x1d);
			W98(t, 0x61, 0x30);
			I(4) = 0;
		} else {
			I(4) = 1;
		}
	}
	uint8_t s7 = R98(t, 7);
	if (s7 & 4)
		clr0 |= 0x10;
	if (s7 & 2) {
		clr0 |= 8;
		W98(t, 0x0a, R98(t, 0x0a) | 2);
		I(0x24) = 1;
	}
	if (s7 & 1) {
		clr0 |= 4;
		abort_ddc(t);
		I(0x24) = 0;
	}
	if (s7 & 0x80)
		clr1 = 2;
	if (s7 & 0x40)
		clr1 |= 1;
	if (s7 & 0x20)
		clr0 |= 0x80;
	if (s7 & 0x10)
		clr0 |= 0x40;
	if (s7 & 8)
		clr0 |= 0x20;
	uint8_t s8 = R98(t, 8);
	if (s8 & 0x40)
		keep = 2;
	if (s8 & 0x20)
		clr1 |= 0x80;
	if (s8 & 0x10) {	/* receiver sense change */
		clr1 |= 0x40;
		if (!(R98(t, 0x0e) & 0x10)) {
			if (I(4) != 0 && I(4) != 1)
				I(4) = 3;
		} else {
			if (I(4) == 3)
				I(4) = 5;
			if (I(4) == 6)
				init_audio_mode(t);
		}
	}
	if (s8 & 8)
		clr1 |= 0x20;
	if (s8 & 4)
		clr1 |= 0x10;
	if (s8 & 2)
		clr1 |= 8;
	if (s8 & 1)
		clr1 |= 4;
	if (clr0)
		W98(t, 0x0c, clr0);
	if (clr1)
		W98(t, 0x0d, clr1);
	uint8_t e = R98(t, 0x0e);
	W98(t, 0x0e, keep | (e & 0xfd) | 1);
	W98(t, 0x0e, keep | (e & 0xfc));
}

static void video_handler(struct it6612 *t)
{
	switch (I(4)) {
	case 0: {	/* no sink */
		if (B(0x48)) {
			B(0x48) = 0;
			I(0x24) = 0;
			hdcp_reset_auth(t);
		}
		uint32_t fs;
		get_dot_clock(t, &fs);
		return;
	}
	case 1:		/* sink plugged in: read its EDID */
		I(0x40) = 0x22;
		I(0x28) = 0;
		I(0x2c) = 0;
		if (!parse_edid(t)) {
			I(4) = 0;
			B(0x48) = 0;
			return;
		}
		I(0x2c) = 0;
		if (!B(0x49)) {
			I(0x28) = 0;
		} else {
			B(0x10) = 0;
			I(0x28) = 1;
			if (B(0x4a) & 0x40)
				I(0x2c) = 1;
		}
		I(0x38) = 0;
		I(4) = 2;
		return;
	case 2:
		B(0x10) = 0;
		select_video_mode(t);
		return;
	case 3:
	case 4:
		break;
	case 5:		/* sink powered: enable audio/HDCP and unmute */
		set_av_mute(t);
		W98(t, 0x0f, 0);
		W98(t, 0x61, 0);
		if (I(0x28) && I(0x2c))
			init_audio_mode(t);
		if (B(0xe) != MODE_NOSIG_C && B(0xe) != MODE_NOSIG && B(0xe) != 0xff && B(0xe) != MODE_E &&
		    B(0xe) != MODE_F) {
			I(0x24) = 0;
			hdcp_reset_auth(t);
			if (B(2) == 1) {
				enable_hdcp(t, true);
				msleep(100);
			}
		}
		if (B(0xe) != MODE_NOSIG_C && B(0xe) != MODE_NOSIG && B(0xe) != 0xff)
			clr_av_mute(t);
		I(4) = 6;
		return;
	default:
		return;
	}
	/* states 3, 4: (re)program the output for the current mode */
	select_video_mode(t);
	if (!I(0x28))
		B(0x10) = 0;
	init_video_mode(t);
	bool hd = t->colorimetry == 0 ? t->in_width > 720 : t->colorimetry == 2;
	config_info_frame(t, hd, t->quant_range == 2);
	W98(t, 0x04, R98(t, 4) & 0xf7);
	if (!(R98(t, 0x0e) & 0x10)) {
		if (B(0xe) == MODE_NOSIG)
			return;
		int i;
		for (i = 0; i < 50; i++) {
			if (R98(t, 0x0e) & 0x10)
				break;
			if (!check_signal(t, B(0xe))) {
				msleep(100);
				I(4) = 3;
				return;
			}
			msleep(10);
		}
		if (i == 50) {
			I(4) = 3;
			return;
		}
	}
	I(4) = 5;
}

static void reset_no_signal(struct it6612 *t)
{
	B(2) = 0;
	B(1) = 0;
	if (I(0x30) == 1 || I(0x34) == 1) {
		I(0x34) = 0;
		I(0x30) = 0;
		init_video_mode(t);
	}
	B(0xe) = MODE_NOSIG;
	B(0xd) = MODE_NOSIG;
	B(0xc) = 0xff;
	set_av_mute(t);
	irq_handler(t);
}

int it6612_init(struct it6612 *t, struct mz_dev *m)
{
	struct it6612 keep = *t;
	memset(t, 0, sizeof(*t));
	t->m = m;
	t->verbose = keep.verbose;
	B(0x11) = 0;
	B(0xf) = 1;	/* input from the MST3367 bus: YUV 4:2:2 */
	B(0x10) = 1;	/* output colour mode; set to RGB once a sink is known */
	I(0x40) = 0x22;
	init_sys(t);
	I(0x1c0) = 1;
	if (!B(0x12)) {
		fprintf(stderr, "IT6612: transmitter not found\n");
		return -1;
	}
	if (R98(t, 9) != 0xb2) {
		B(0x11) = 0;
		B(0xf) = 1;
		B(0x10) = 1;
		I(0x40) = 0x22;
		init_sys(t);
		I(0x1c0) = 1;
	}
	B(2) = 0;
	B(1) = 0;
	if (I(0x30) == 1 || I(0x34) == 1) {
		I(0x34) = 0;
		I(0x30) = 0;
		init_video_mode(t);
	}
	B(0xe) = MODE_NOSIG;
	B(0xd) = MODE_NOSIG;
	B(0xc) = 0xff;
	set_av_mute(t);
	irq_handler(t);
	return 0;
}

uint8_t it6612_tv_mode(unsigned w, unsigned h, unsigned fps)
{
	/* CIT6612_GetTvMode for HDMI input (crossbar 0) */
	struct { uint16_t w, h; uint8_t fps, mode; } tab[] = {
		{ 720, 240, 60, 0x00 }, { 720, 288, 50, 0x01 }, { 720, 480, 60, 0x10 }, { 720, 576, 50, 0x11 },
		{ 720, 400, 85, 0x1c }, { 832, 624, 75, 0x42 }, { 1280, 720, 59, 0x02 }, { 1280, 720, 60, 0x02 },
		{ 1280, 720, 50, 0x03 }, { 1280, 768, 60, 0x28 }, { 1280, 800, 60, 0x35 }, { 1280, 960, 60, 0x2a },
		{ 1280, 1024, 50, 0x42 }, { 1280, 1024, 59, 0x15 }, { 1280, 1024, 60, 0x15 }, { 1920, 540, 60, 0x07 },
		{ 1920, 540, 50, 0x08 }, { 1920, 1080, 24, 0x0b }, { 1920, 1080, 30, 0x09 }, { 1920, 1080, 25, 0x0a },
		{ 1920, 1080, 60, 0x0e }, { 1920, 1080, 50, 0x0f }, { 640, 480, 60, 0x12 }, { 640, 480, 72, 0x19 },
		{ 640, 480, 75, 0x1a }, { 640, 480, 85, 0x1b }, { 800, 600, 56, 0x1d }, { 800, 600, 60, 0x13 },
		{ 800, 600, 72, 0x1e }, { 800, 600, 75, 0x1f }, { 800, 600, 85, 0x20 }, { 1024, 600, 60, 0x21 },
		{ 1024, 768, 50, 0x22 }, { 1024, 768, 60, 0x14 }, { 1024, 768, 70, 0x23 }, { 1024, 768, 75, 0x25 },
		{ 1024, 768, 85, 0x26 }, { 1064, 600, 60, 0x42 }, { 1360, 768, 60, 0x2c }, { 1360, 768, 61, 0x2c },
		{ 1152, 864, 75, 0x27 }, { 1400, 1050, 60, 0x2e }, { 1440, 900, 60, 0x2d }, { 1440, 540, 50, 0x3a },
	};
	if (!w)
		return 0x44;
	for (unsigned i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
		if (tab[i].w == w && tab[i].h == h && tab[i].fps == fps)
			return tab[i].mode;
	return fps ? 0x42 : 0x44;
}

void it6612_timer(struct it6612 *t, const struct video_mode *v, bool signal)
{
	if (!B(0x12))
		return;
	uint8_t mode = MODE_NOSIG;
	if (signal && v) {
		t->in_width = v->width;
		t->colorimetry = v->colorimetry;
		t->quant_range = v->quant;
		t->input_hdcp = v->hdcp;
		mode = t->tv_mode;
	}
	if (R98(t, 9) != 0xb2) {	/* transmitter lost its configuration */
		B(0x11) = 0;
		B(0xf) = 1;
		B(0x10) = 1;
		I(0x40) = 0x22;
		init_sys(t);
		I(0x1c0) = 1;
		reset_no_signal(t);
		return;
	}
	if (mode == MODE_NOSIG_C || mode == MODE_NOSIG) {
		reset_no_signal(t);
		return;
	}
	if (B(0xe) != mode) {
		if (!check_signal(t, mode))
			return;
		set_av_mute(t);
		B(0xe) = mode;
		set_data_latch_timing(t, 0, mode);
		set_pll_charge_pump(t, 0, mode);
		rx_adc_set_coast(t, mode);
		bool hd = t->colorimetry == 0 ? t->in_width > 720 : t->colorimetry == 2;
		config_info_frame(t, hd, t->quant_range == 2);
		W9C(t, 0, 0x60, R9C(t, 0, 0x60) & 0xfb);
		uint32_t fs;
		get_dot_clock(t, &fs);
	}
	static const struct { uint8_t mode, vic; } vic_map[] = {
		{ 0x00, 6 }, { '<', 6 }, { 'E', 6 }, { 0x01, 0x15 }, { '>', 0x15 }, { 'F', 0x15 }, { 0x02, 4 },
		{ 0x03, 0x13 }, { 0x04, 0x3e }, { 0x05, 0x3d }, { 0x06, 0x3c }, { 0x07, 5 }, { 0x08, 0x14 },
		{ 0x09, 0x22 }, { 0x0a, 0x21 }, { 0x0c, 0x21 }, { 0x0b, 0x20 }, { 0x0d, 0x20 }, { 0x0e, 0x10 },
		{ 0x0f, 0x1f }, { 0x10, 2 }, { 0x11, 0x11 }, { 0x12, 1 },
	};
	uint8_t vic = 0x20;
	for (unsigned i = 0; i < sizeof(vic_map) / sizeof(vic_map[0]); i++)
		if (vic_map[i].mode == mode)
			vic = vic_map[i].vic;
	B(0) = B(2) ? get_current_vic(t, vic) : vic;
	if (mode == MODE_F || mode == MODE_E) {
		if (!I(0x30) || !I(0x34)) {
			I(0x34) = 1;
			I(0x30) = 1;
			init_video_mode(t);
		}
	} else if (I(0x30) == 1 || I(0x34) == 1) {
		I(0x34) = 0;
		I(0x30) = 0;
		init_video_mode(t);
	}
	/*
	 * The driver only leaves state 0 on a hot-plug interrupt; a display that
	 * already asserts HPD when we start never generates one. Use the level.
	 */
	if (I(4) == 0 && (R98(t, 0x0e) & 0x40))
		I(4) = 1;
	/*
	 * Some displays (seen on a Samsung) drop HPD in standby and only wake on a
	 * TMDS signal. If HPD is low, periodically check whether a display answers
	 * on DDC and treat that as connected.
	 */
	if (I(4) == 0 && ++t->ddc_probe_ticks >= 4) {
		t->ddc_probe_ticks = 0;
		uint8_t hdr[8];
		static const uint8_t magic[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
		if (!read_edid(t, hdr, 0, 0, 8) && !memcmp(hdr, magic, 8)) {
			if (t->verbose)
				printf("IT6612: HPD low but a display answers on DDC, enabling output\n");
			I(4) = 1;
		}
	}
	for (int n = 0; n <= 20; n++) {
		irq_handler(t);
		video_handler(t);
		/* No sink: further passes repeat the same reads (the driver runs this on
		 * its own thread; here it would stall frame delivery). */
		if (I(4) == 0)
			break;
		if (I(4) == 6) {
			if (B(0xe) == B(0xd) && B(0) == B(0xc)) {
				if (t->input_hdcp && B(2) == 1 && I(0x24) == 0) {
					if (t->verbose)
						printf("IT6612: HDCP (re)authentication\n");
					I(0x24) = 0;
					hdcp_reset_auth(t);
					enable_hdcp(t, true);
				}
			} else {
				I(4) = 4;
			}
			break;
		}
	}
}

int it6612_read_edid(struct it6612 *t, uint8_t *buf, unsigned block)
{
	return read_edid(t, buf, block / 2, (block & 1) << 7, 128);
}

const char *it6612_state_name(const struct it6612 *t)
{
	static const char *names[] = { "no display", "display plugged", "reading EDID", "waiting for display",
				       "setting mode", "enabling output", "passthrough on" };
	int s = *(const int32_t *)&t->st[4];
	return s >= 0 && s <= 6 ? names[s] : "?";
}
