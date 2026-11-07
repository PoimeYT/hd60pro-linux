/* SPDX-License-Identifier: GPL-2.0-or-later */
/* hd60pro: userspace (VFIO) driver and diagnostics for the Elgato HD60 Pro. */
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mst3367.h"
#include "mz0380.h"
#include "stream.h"
#include "v4l2out.h"
#include "it6612.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
	stop_requested = 1;
}

struct run_ctx {
	struct v4l2out out;
	unsigned stride;
	unsigned frames, audio;
	FILE *pcm;
	int pipe_fd;		/* PipeWire pipe-source FIFO, -1 when unused */
	char pipe_module[32];	/* pactl module index to unload */
};

static void on_run_frame(const struct frame *f, void *arg)
{
	struct run_ctx *r = arg;
	if (f->kind == FRAME_VIDEO) {
		v4l2out_write_i420(&r->out, f->data, r->stride);
		r->frames++;
	} else if (f->kind == FRAME_AUDIO) {
		r->audio++;
		if (r->pcm)
			fwrite(f->data, 1, f->len, r->pcm);
		if (r->pipe_fd >= 0 && write(r->pipe_fd, f->data, f->len) < 0 && errno != EAGAIN)
			perror("audio pipe");
	}
}

/*
 * Signal monitor thread: input detection, receiver CSC and the HDMI OUT state
 * machine, like the Windows driver's analysis thread. Publishes the current
 * mode; the streaming loop restarts capture when it changes.
 */
struct monitor {
	struct mz_dev *m;
	struct it6612 tx;
	pthread_mutex_t lock;
	unsigned seq;		/* bumped on every detection result */
	int got;
	struct video_mode mode;
};

static double mono_s(void);

static void *monitor_main(void *arg)
{
	struct monitor *mon = arg;
	const char *tx_state = NULL;
	enum colorspace cs = (enum colorspace)-1;
	while (!stop_requested) {
		struct video_mode v;
		int got = mst3367_detect(mon->m, &v);
		if (got == 1) {
			if (v.cs != cs) {
				mst3367_setup_csc(mon->m, v.cs);
				cs = v.cs;
			}
			mon->tx.tv_mode = it6612_tv_mode(v.width, v.height, v.fps);
		} else {
			cs = (enum colorspace)-1;
		}
		double t0 = mono_s();
		int before = *(int32_t *)&mon->tx.st[4];
		it6612_timer(&mon->tx, got == 1 ? &v : NULL, got == 1);
		double ms = (mono_s() - t0) * 1000;
		if (mon->m->verbose && ms > 20)
			printf("monitor: slow HDMI-out tick %.0f ms (state %d -> %d, vic %u/%u)\n", ms, before,
			       *(int32_t *)&mon->tx.st[4], mon->tx.st[0], mon->tx.st[0xc]);
		if (tx_state != it6612_state_name(&mon->tx)) {
			tx_state = it6612_state_name(&mon->tx);
			printf("HDMI out: %s\n", tx_state);
			fflush(stdout);
		}
		pthread_mutex_lock(&mon->lock);
		mon->got = got;
		mon->mode = v;
		mon->seq++;
		pthread_mutex_unlock(&mon->lock);
		usleep(500000);
	}
	return NULL;
}

#define AUDIO_FIFO "/tmp/hd60pro-audio"

/* Expose the card's audio as a PipeWire/PulseAudio input named "HD60 Pro Audio". */
static int audio_pipe_open(struct run_ctx *r)
{
	unlink(AUDIO_FIFO);
	FILE *p = popen("pactl load-module module-pipe-source source_name=hd60pro file=" AUDIO_FIFO
			" format=s16le rate=48000 channels=2 source_properties=device.description=HD60_Pro_Audio",
			"r");
	if (!p || !fgets(r->pipe_module, sizeof(r->pipe_module), p)) {
		if (p)
			pclose(p);
		fprintf(stderr, "audio: pactl load-module failed, audio not exported\n");
		return -1;
	}
	pclose(p);
	r->pipe_module[strcspn(r->pipe_module, "\n")] = 0;
	r->pipe_fd = open(AUDIO_FIFO, O_WRONLY | O_NONBLOCK);
	if (r->pipe_fd < 0) {
		perror("audio: open " AUDIO_FIFO);
		return -1;
	}
	printf("audio: PipeWire source \"HD60 Pro Audio\" (module %s)\n", r->pipe_module);
	return 0;
}

static void audio_pipe_close(struct run_ctx *r)
{
	if (r->pipe_fd >= 0)
		close(r->pipe_fd);
	r->pipe_fd = -1;
	if (r->pipe_module[0]) {
		char cmd[64];
		snprintf(cmd, sizeof(cmd), "pactl unload-module %s", r->pipe_module);
		if (system(cmd) != 0)
			fprintf(stderr, "audio: failed to unload module %s\n", r->pipe_module);
		r->pipe_module[0] = 0;
	}
}

static double mono_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

struct capture_out {
	FILE *video, *h264, *audio;
	unsigned nvideo, nh264, naudio, nkey;
	unsigned long h264_bytes;
	unsigned max_raw;
	FILE *framelog;	/* HD60_FRAMELOG: per-frame time and buffer index */
};

static void on_frame(const struct frame *f, void *arg)
{
	struct capture_out *o = arg;
	switch (f->kind) {
	case FRAME_VIDEO:
		if (o->framelog)
			fprintf(o->framelog, "%.6f %u\n", mono_s(), f->index);
		if (o->nvideo++ < o->max_raw && o->video)
			fwrite(f->data, 1, f->len, o->video);
		break;
	case FRAME_H264:
		o->nh264++;
		o->nkey += f->keyframe;
		o->h264_bytes += f->len;
		if (o->h264)
			fwrite(f->data, 1, f->len, o->h264);
		break;
	case FRAME_AUDIO:
		o->naudio++;
		if (o->audio)
			fwrite(f->data, 1, f->len, o->audio);
		break;
	}
}

static void print_mode(const struct video_mode *v)
{
	static const char *cs[] = { "RGB", "YUV422", "YUV444" };
	printf("%ux%u%c%s%u, audio %u Hz, %s\n", v->width, v->interlaced ? v->height * 2 : v->height,
	       v->interlaced ? 'i' : 'p', v->ntsc ? "~" : "@", v->fps, v->audio_rate, cs[v->cs]);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: hd60pro [-d BDF] [-v] COMMAND\n"
		"  regs                   dump BAR0 mailbox and BAR5 registers (no commands sent)\n"
		"  probe                  handshake with the card and print its firmware version\n"
		"  i2c-read ADDR REG      read one byte (8-bit address, e.g. 0x9c)\n"
		"  i2c-write ADDR REG VAL\n"
		"  i2c-dump ADDR [BANK]   dump 256 registers, optionally selecting a bank first\n"
		"  i2c-scan               probe every 8-bit address for an answering device\n"
		"  detect [SECS]          initialise the HDMI receiver and report the input format\n"
		"  capture PREFIX [SECS]  stream and save PREFIX.yuv (first frames), .h264 and .pcm\n"
		"  passthrough [SECS]     drive HDMI OUT only (no capture)\n"
		"  tx-status [SECS]       read the display EDID and watch HDMI OUT hot-plug state\n"
		"  run [DEV] [PCM]        stream to a v4l2loopback device (default /dev/video0)\n"
		"  gpio-get PIN\n");
}

static unsigned long num(const char *s)
{
	return strtoul(s, NULL, 0);
}

static void dump_regs(struct mz_dev *m)
{
	printf("BAR0 @ %#llx (%llu KiB), BAR5 @ %#llx\n", (unsigned long long)m->bar0_bus,
	       (unsigned long long)m->vfio.bar[0].size / 1024,
	       (unsigned long long)m->vfio.bar[5].bus_addr);
	for (unsigned off = 0; off < 0x60; off += 16)
		printf("  BAR0+%02x: %08x %08x %08x %08x\n", off, mz_rd(m, off), mz_rd(m, off + 4),
		       mz_rd(m, off + 8), mz_rd(m, off + 12));
	for (unsigned off = 0; off < 0x100; off += 16)
		printf("  BAR5+%02x: %08x %08x %08x %08x\n", off, m->bar5[off / 4], m->bar5[off / 4 + 1],
		       m->bar5[off / 4 + 2], m->bar5[off / 4 + 3]);
}

int main(int argc, char **argv)
{
	const char *bdf = "0000:05:00.0";
	int verbose = 0, c;
	while ((c = getopt(argc, argv, "d:vh")) != -1) {
		switch (c) {
		case 'd': bdf = optarg; break;
		case 'v': verbose++; break;
		default: usage(); return 2;
		}
	}
	if (optind >= argc) {
		usage();
		return 2;
	}
	const char *cmd = argv[optind];
	char **arg = argv + optind + 1;
	int nargs = argc - optind - 1;

	struct mz_dev m;
	if (mz_open(&m, bdf))
		return 1;
	m.verbose = verbose;
	int rc = 0;

	if (!strcmp(cmd, "regs")) {
		dump_regs(&m);
		goto out;
	}

	if (mz_boot(&m)) {
		rc = 1;
		goto out;
	}
	printf("card firmware version %u.%02u\n", m.board_major, m.board_minor);

	if (!strcmp(cmd, "probe")) {
		int a = mz_bank_read(&m, I2C_MST3367, 0, 0x01);
		int b = mz_bank_read(&m, I2C_MST3367, 0, 0x02);
		printf("MST3367 bank0 reg01=%02x reg02=%02x\n", a & 0xff, b & 0xff);
		printf("IT6612 reg00..03:");
		for (int r = 0; r < 4; r++)
			printf(" %02x", mz_i2c_read(&m, I2C_IT6612, r) & 0xff);
		printf("\n");
	} else if (!strcmp(cmd, "i2c-read") && nargs == 2) {
		int v = mz_i2c_read(&m, num(arg[0]), num(arg[1]));
		if (v < 0)
			rc = 1;
		else
			printf("%02x\n", v);
	} else if (!strcmp(cmd, "i2c-write") && nargs == 3) {
		rc = mz_i2c_write(&m, num(arg[0]), num(arg[1]), num(arg[2])) ? 1 : 0;
	} else if (!strcmp(cmd, "i2c-dump") && (nargs == 1 || nargs == 2)) {
		uint8_t addr = num(arg[0]);
		if (nargs == 2)
			mz_i2c_write(&m, addr, 0, num(arg[1]));
		for (int row = 0; row < 256; row += 16) {
			uint8_t buf[16];
			printf("%02x:", row);
			if (mz_i2c_read_n(&m, addr, row, buf, 16)) {
				printf(" (timeout)\n");
				continue;
			}
			for (int i = 0; i < 16; i++)
				printf(" %02x", buf[i]);
			printf("\n");
		}
	} else if (!strcmp(cmd, "i2c-scan")) {
		/* The card firmware does not report NAKs; look for non-0xff answers. */
		for (int a = 2; a < 256; a += 2) {
			int v0 = mz_i2c_read(&m, a, 0), v1 = mz_i2c_read(&m, a, 1);
			if ((v0 >= 0 && v0 != 0xff) || (v1 >= 0 && v1 != 0xff))
				printf("  %02x: reg0=%02x reg1=%02x\n", a, v0 & 0xff, v1 & 0xff);
		}
	} else if (!strcmp(cmd, "rx-reset")) {
		/* GPIO setup from HwBackgroundInitialize, reset from MST3367_HwInitialize. */
		static const uint8_t dir[][2] = { {0, 1}, {1, 0}, {2, 0}, {6, 1}, {8, 0}, {9, 0}, {10, 0}, {11, 0} };
		for (unsigned i = 0; i < sizeof(dir) / sizeof(dir[0]); i++)
			mz_gpio_dir(&m, dir[i][0], dir[i][1]);
		mz_gpio_set(&m, 9, 1);
		usleep(50000);
		mz_gpio_set(&m, 9, 0);
		usleep(50000);
		mz_gpio_set(&m, 9, 1);
		usleep(50000);
		mz_gpio_set(&m, 8, 0);
		usleep(50000);
		for (int bank = 0; bank < 3; bank++) {
			printf("MST3367 bank %d:", bank);
			for (int r = 0; r < 16; r++)
				printf(" %02x", mz_bank_read(&m, I2C_MST3367, bank, r) & 0xff);
			printf("\n");
		}
		printf("IT6612 reg00..07:");
		for (int r = 0; r < 8; r++)
			printf(" %02x", mz_i2c_read(&m, I2C_IT6612, r) & 0xff);
		printf("\n");
	} else if (!strcmp(cmd, "detect")) {
		unsigned secs = nargs ? num(arg[0]) : 10;
		mst3367_init(&m);
		struct video_mode cur = { 0 };
		int had = -1;
		for (unsigned i = 0; i < secs * 2; i++) {
			struct video_mode v;
			int r = mst3367_detect(&m, &v);
			if (r != had || (r == 1 && !mode_equal(&v, &cur))) {
				if (r == 1) {
					printf("signal: ");
					print_mode(&v);
					mst3367_setup_csc(&m, v.cs);
				} else {
					printf("no signal\n");
				}
				fflush(stdout);
				had = r;
				cur = v;
			}
			usleep(500000);
		}
	} else if (!strcmp(cmd, "capture") && nargs >= 1) {
		/* capture PREFIX [SECS]: writes PREFIX.yuv (I420), PREFIX.h264, PREFIX.pcm */
		unsigned secs = nargs > 1 ? num(arg[1]) : 5;
		struct stream s;
		if (stream_alloc(&m, &s, false) || stream_register_buffers(&m, &s)) {
			fprintf(stderr, "buffer setup failed\n");
			rc = 1;
			goto out;
		}
		mst3367_init(&m);
		struct video_mode v;
		int got = 0;
		for (int i = 0; i < 20 && got != 1; i++) {
			got = mst3367_detect(&m, &v);
			if (got != 1)
				usleep(250000);
		}
		if (got != 1) {
			fprintf(stderr, "no input signal\n");
			rc = 1;
			goto out;
		}
		printf("input: ");
		print_mode(&v);
		mst3367_setup_csc(&m, v.cs);
		if (stream_start(&m, &s, &v, v.colorimetry)) {
			rc = 1;
			goto out;
		}
		char path[512];
		struct capture_out o = { .max_raw = 3 };
		if (getenv("HD60_FRAMELOG"))
			o.framelog = fopen(getenv("HD60_FRAMELOG"), "w");
		if (getenv("HD60_IRQLOG"))
			s.irqlog = fopen(getenv("HD60_IRQLOG"), "w");
		snprintf(path, sizeof(path), "%s.yuv", arg[0]);
		o.video = fopen(path, "wb");
		snprintf(path, sizeof(path), "%s.h264", arg[0]);
		o.h264 = fopen(path, "wb");
		snprintf(path, sizeof(path), "%s.pcm", arg[0]);
		o.audio = fopen(path, "wb");
		for (unsigned t = 0; t < secs * 2000; t++) {
			stream_poll(&m, &s, on_frame, &o);
			usleep(500);
			if (t % 500 == 0) {
				const uint32_t *vb = s.video[0].virt;
				uint32_t sum = 0;
				for (size_t i = 0; i < 1920 * 1080 / 4; i += 97)
					sum = sum * 31 + vb[i];
				printf("    t=%.2fs video sum %08x head %08x %08x idx40 %08x\n", t / 2000.0, sum, vb[0], vb[1],
				       mz_rd(&m, 0x40));
				if (getenv("HD60_CLEAR"))
					memset(s.video[0].virt, 0, 1920 * 1080 * 3 / 2);
			}
			if (t % 2000 == 1999) {
				printf("  %us: raw %u, h264 %u (%u key, %lu KiB), audio %u, idx %08x %08x %08x %08x\n",
				       t / 2000 + 1, o.nvideo, o.nh264, o.nkey, o.h264_bytes / 1024, o.naudio,
				       mz_rd(&m, 0x40), mz_rd(&m, 0x44), mz_rd(&m, 0x48), mz_rd(&m, 0x4c));
				fflush(stdout);
			}
		}
		stream_stop(&m);
		printf("irq: %u interrupts, bits seen %08x\n", s.irq_count, s.irq_seen);
		const struct dma_buf *chk[] = { &s.video[0], &s.enc[0][0], &s.audio };
		const char *nm[] = { "video", "enc", "audio" };
		for (int b = 0; b < 3; b++) {
			const uint8_t *p = chk[b]->virt;
			size_t nz = 0;
			for (size_t i = 0; i < chk[b]->size; i++)
				nz += p[i] != 0;
			printf("%s buffer: %zu/%zu nonzero bytes, head:", nm[b], nz, chk[b]->size);
			for (int i = 0; i < 32; i++)
				printf(" %02x", p[i]);
			printf("\n");
		}
		snprintf(path, sizeof(path), "%s.last.yuv", arg[0]);
		FILE *lf = fopen(path, "wb");
		fwrite(s.video[0].virt, 1, 1920 * 1080 * 3 / 2, lf);
		fclose(lf);
		fclose(o.video);
		fclose(o.h264);
		fclose(o.audio);
		stream_free(&m, &s);
	} else if (!strcmp(cmd, "tx-status")) {
		/* tx-status [SECS]: watch the IT6612 status registers and try a DDC EDID read */
		unsigned secs = nargs ? num(arg[0]) : 20;
		struct it6612 tx = { .verbose = 1 };
		if (it6612_init(&tx, &m)) {
			rc = 1;
			goto out;
		}
		uint8_t edid[128];
		int er = it6612_read_edid(&tx, edid, 0);
		printf("EDID block 0 via DDC: %s", er ? "failed" : "read:");
		if (!er)
			for (int i = 0; i < 16; i++)
				printf(" %02x", edid[i]);
		printf("\n");
		int last = -1;
		for (unsigned i = 0; i < secs * 5; i++) {
			int st = mz_i2c_read(&m, I2C_IT6612, 0x0e), i6 = mz_i2c_read(&m, I2C_IT6612, 6);
			int v = st << 8 | i6;
			if (v != last) {
				printf("%5.1fs  sys_status=%02x (HPD %d, RxSense %d)  int6=%02x\n", i / 5.0, st, !!(st & 0x40),
				       !!(st & 0x10), i6);
				fflush(stdout);
				last = v;
			}
			usleep(200000);
		}
	} else if (!strcmp(cmd, "passthrough")) {
		/* passthrough [SECS]: drive HDMI OUT only, no capture */
		unsigned secs = nargs ? num(arg[0]) : 30;
		signal(SIGINT, on_signal);
		mst3367_init(&m);
		struct it6612 tx = { .verbose = verbose };
		if (it6612_init(&tx, &m)) {
			rc = 1;
			goto out;
		}
		const char *state = NULL;
		for (unsigned i = 0; i < secs * 4 && !stop_requested; i++) {
			struct video_mode v;
			int got = mst3367_detect(&m, &v);
			if (got == 1) {
				mst3367_setup_csc(&m, v.cs);
				tx.tv_mode = it6612_tv_mode(v.width, v.height, v.fps);
			}
			double t0 = mono_s();
			it6612_timer(&tx, got == 1 ? &v : NULL, got == 1);
			if (verbose)
				printf("tick: timer %.1f ms, input hdcp %d, tx hdcp ok %d\n", (mono_s() - t0) * 1000, v.hdcp,
				       *(int32_t *)&tx.st[0x24]);
			if (state != it6612_state_name(&tx)) {
				state = it6612_state_name(&tx);
				printf("HDMI out: %s (input %s, tx mode %#x, VIC %u)\n", state, got == 1 ? "locked" : "none",
				       tx.tv_mode, tx.st[0]);
				fflush(stdout);
			}
			usleep(250000);
		}
	} else if (!strcmp(cmd, "run")) {
		/* run [V4L2DEV] [PCMFILE]: stream to a v4l2loopback device until interrupted */
		const char *dev = nargs > 0 ? arg[0] : "/dev/video0";
		struct run_ctx r = { .out.fd = -1 };
		r.pipe_fd = -1;
		if (nargs > 1 && !(r.pcm = fopen(arg[1], "wb")))
			perror(arg[1]);
		audio_pipe_open(&r);
		struct stream s;
		if (stream_alloc(&m, &s, false) || stream_register_buffers(&m, &s)) {
			fprintf(stderr, "buffer setup failed\n");
			rc = 1;
			goto out;
		}
		signal(SIGINT, on_signal);
		signal(SIGTERM, on_signal);
		mst3367_init(&m);
		struct monitor mon = { .m = &m, .tx.verbose = verbose };
		pthread_mutex_init(&mon.lock, NULL);
		it6612_init(&mon.tx, &m);
		pthread_t mon_thread;
		pthread_create(&mon_thread, NULL, monitor_main, &mon);
		struct video_mode cur = { 0 };
		bool streaming = false;
		unsigned seen_seq = 0;
		double last_report = mono_s();
		while (!stop_requested) {
			double now = mono_s();
			pthread_mutex_lock(&mon.lock);
			unsigned seq = mon.seq;
			int got = mon.got;
			struct video_mode v = mon.mode;
			pthread_mutex_unlock(&mon.lock);
			if (seq != seen_seq) {
				seen_seq = seq;
				if (got != 1 && streaming) {
					printf("signal lost\n");
					stream_stop(&m);
					v4l2out_close(&r.out);
					streaming = false;
				} else if (got == 1 && (!streaming || !mode_equal(&v, &cur))) {
					if (streaming) {
						stream_stop(&m);
						v4l2out_close(&r.out);
					}
					printf("signal: ");
					print_mode(&v);
					fflush(stdout);
					unsigned h = v.interlaced ? v.height * 2 : v.height;
					r.stride = (v.width + 15) & ~15u;
					if (v4l2out_open(&r.out, dev, v.width, h, v.fps) ||
					    stream_start(&m, &s, &v, v.colorimetry)) {
						rc = 1;
						break;
					}
					cur = v;
					streaming = true;
				}
			}
			if (streaming)
				stream_poll(&m, &s, on_run_frame, &r);
			usleep(streaming ? 500 : 50000);
			if (now - last_report >= 5) {
				printf("  %.1f fps video, %.1f audio periods/s\n", r.frames / (now - last_report),
				       r.audio / (now - last_report));
				fflush(stdout);
				r.frames = r.audio = 0;
				last_report = now;
			}
		}
		pthread_join(mon_thread, NULL);
		if (streaming)
			stream_stop(&m);
		v4l2out_close(&r.out);
		if (r.pcm)
			fclose(r.pcm);
		audio_pipe_close(&r);
		stream_free(&m, &s);
	} else if (!strcmp(cmd, "gpio-get") && nargs == 1) {
		printf("%d\n", mz_gpio_get(&m, num(arg[0])));
	} else {
		usage();
		rc = 2;
	}
out:
	mz_close(&m);
	return rc;
}
