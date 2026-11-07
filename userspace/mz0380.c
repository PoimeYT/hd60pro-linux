/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mz0380.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int mz_open(struct mz_dev *m, const char *bdf)
{
	memset(m, 0, sizeof(*m));
	m->i2c_bank = -1;
	int r = vfio_open(&m->vfio, bdf);
	if (r)
		return r;
	if (!m->vfio.bar[0].map || !m->vfio.bar[5].map) {
		fprintf(stderr, "mz0380: BAR0/BAR5 not mapped\n");
		return -ENODEV;
	}
	m->bar0 = (volatile uint32_t *)m->vfio.bar[0].map;
	m->bar5 = (volatile uint32_t *)m->vfio.bar[5].map;
	m->bar0_bus = m->vfio.bar[0].bus_addr;
	pthread_mutexattr_t a;
	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&m->lock, &a);
	pthread_mutexattr_destroy(&a);
	return 0;
}

void mz_lock(struct mz_dev *m)
{
	pthread_mutex_lock(&m->lock);
}

void mz_unlock(struct mz_dev *m)
{
	pthread_mutex_unlock(&m->lock);
}

void mz_close(struct mz_dev *m)
{
	vfio_close(&m->vfio);
}

/* Same sequence as CPCIBaseDevice::OnInterruptHandle. */
void mz_ack_irq(struct mz_dev *m)
{
	ep_wr(m, EP_IRQ_CLEAR, 2);
	mz_wr(m, MZ_IRQ_STATUS, 0);
	mz_wr(m, MZ_DOORBELL, MZ_DOORBELL_ACK);
}

/*
 * The Windows driver uses two completion styles: "sync" commands poll
 * CMD_STATUS bit0 for up to 50 ms, "async" ones wait for IRQ_STATUS bit 11
 * (delivered through the ISR). Polling both covers either style without
 * needing a working interrupt line.
 */
static int mz_cmd_locked(struct mz_dev *m, const uint32_t *words, int n, int timeout_ms)
{
	mz_wr(m, MZ_CMD_STATUS, 0);
	for (int i = 0; i < n; i++)
		mz_wr(m, MZ_PARAM(i), words[i]);
	mz_wr(m, MZ_DOORBELL, MZ_DOORBELL_CMD);

	uint64_t deadline = now_us() + (uint64_t)timeout_ms * 1000;
	do {
		uint32_t irq = mz_rd(m, MZ_IRQ_STATUS);
		if (irq && irq != 0xffffffff) {
			m->pending_irq |= irq & ~MZ_IRQ_CMD_DONE;
			mz_ack_irq(m);
			if (irq & MZ_IRQ_CMD_DONE)
				goto done;
		}
		if (mz_rd(m, MZ_CMD_STATUS) & 1)
			goto done;
		usleep(200);
	} while (now_us() < deadline);
	if (m->verbose)
		fprintf(stderr, "mz0380: command %#x timed out (status %08x irq %08x)\n", words[0],
			mz_rd(m, MZ_CMD_STATUS), mz_rd(m, MZ_IRQ_STATUS));
	return -ETIMEDOUT;
done:
	if (m->verbose > 1)
		fprintf(stderr, "mz0380: cmd %#x ok, result %08x %08x %08x status %08x\n", words[0],
			mz_rd(m, MZ_RESULT(0)), mz_rd(m, MZ_RESULT(1)), mz_rd(m, MZ_RESULT(2)),
			mz_rd(m, MZ_CMD_STATUS));
	return 0;
}

/*
 * MZ0380_HwBackgroundInitialize, up to the board version query. The card
 * boots its application from on-board flash; the firmware download branch
 * (taken on a version mismatch) is deliberately not reproduced.
 */
int mz_cmd(struct mz_dev *m, const uint32_t *words, int n, int timeout_ms)
{
	mz_lock(m);
	int r = mz_cmd_locked(m, words, n, timeout_ms);
	mz_unlock(m);
	return r;
}

int mz_boot(struct mz_dev *m)
{
	usleep(256 * 1000);
	mz_ack_irq(m);

	int ok = 0;
	for (int i = 0; i < 100 && !ok; i++) {
		/* Tell the endpoint where its BAR0 lives (inbound window). */
		ep_wr(m, EP_INBOUND_START, (uint32_t)(m->bar0_bus + 4));
		ep_wr(m, EP_INBOUND_LIMIT, (uint32_t)(m->bar0_bus + 0x5f));
		uint32_t hello[] = { MZ_CMD_HELLO };
		ok = mz_cmd(m, hello, 1, 500) == 0;
		if (!ok && m->verbose)
			fprintf(stderr, "mz0380: waiting for card firmware (%d)\n", i);
	}
	mz_ack_irq(m);
	if (!ok) {
		fprintf(stderr, "mz0380: card did not answer HELLO\n");
		return -ETIMEDOUT;
	}

	for (int i = 0; i < 100; i++) {
		uint32_t ver[] = { MZ_CMD_GET_VERSION, 0, 0 };
		if (mz_cmd(m, ver, 3, 5000) == 0 && mz_rd(m, MZ_CMD_STATUS) == MZ_STATUS_READY) {
			usleep(100 * 1000);
			m->board_major = mz_rd(m, MZ_RESULT(0));
			m->board_minor = mz_rd(m, MZ_RESULT(1));
			return 0;
		}
		usleep(10 * 1000);
	}
	fprintf(stderr, "mz0380: firmware never reported ready\n");
	return -ETIMEDOUT;
}

int mz_i2c_read(struct mz_dev *m, uint8_t addr, uint8_t reg)
{
	uint32_t w[] = { MZ_CMD_I2C_READ, addr, reg, 0 };
	if (mz_cmd(m, w, 4, 50))
		return -ETIMEDOUT;
	return mz_rd(m, MZ_RESULT(2)) & 0xff;
}

int mz_i2c_write(struct mz_dev *m, uint8_t addr, uint8_t reg, uint8_t val)
{
	uint32_t w[] = { MZ_CMD_I2C_WRITE, addr, reg, val };
	return mz_cmd(m, w, 4, 50);
}

int mz_i2c_read_n(struct mz_dev *m, uint8_t addr, uint8_t reg, uint8_t *buf, unsigned len)
{
	if (len > 32)
		return -EINVAL;
	uint32_t w[] = { MZ_CMD_I2C_READ_N, addr | (uint32_t)reg << 8 | len << 16 };
	if (mz_cmd(m, w, 2, 50))
		return -ETIMEDOUT;
	uint32_t tmp[8];
	for (int i = 0; i < 8; i++)
		tmp[i] = mz_rd(m, MZ_RESULT(1 + i));
	memcpy(buf, tmp, len);
	return 0;
}

int mz_i2c_write_n(struct mz_dev *m, uint8_t addr, uint8_t reg, const uint8_t *buf, unsigned len)
{
	if (len > 32)
		return -EINVAL;
	uint32_t w[10] = { MZ_CMD_I2C_WRITE_N, addr | (uint32_t)reg << 8 | len << 16 };
	memcpy(&w[2], buf, len);
	return mz_cmd(m, w, 2 + (len + 3) / 4, 50);
}

static int select_bank(struct mz_dev *m, uint8_t addr, uint8_t bank)
{
	if (m->i2c_bank == bank)
		return 0;
	int r = mz_i2c_write(m, addr, 0, bank);
	m->i2c_bank = r ? -1 : bank;
	return r;
}

int mz_bank_read(struct mz_dev *m, uint8_t addr, uint8_t bank, uint8_t reg)
{
	mz_lock(m);
	int r = select_bank(m, addr, bank);
	if (!r)
		r = mz_i2c_read(m, addr, reg);
	mz_unlock(m);
	return r;
}

int mz_bank_write(struct mz_dev *m, uint8_t addr, uint8_t bank, uint8_t reg, uint8_t val)
{
	mz_lock(m);
	int r = select_bank(m, addr, bank);
	if (!r)
		r = mz_i2c_write(m, addr, reg, val);
	mz_unlock(m);
	return r;
}

int mz_gpio_get(struct mz_dev *m, unsigned pin)
{
	uint32_t w[] = { MZ_CMD_GPIO_GET, 1u << pin, 0 };
	if (mz_cmd(m, w, 3, 50))
		return -ETIMEDOUT;
	return (mz_rd(m, MZ_RESULT(1)) >> pin) & 1;
}

int mz_gpio_set(struct mz_dev *m, unsigned pin, unsigned val)
{
	uint32_t w[] = { MZ_CMD_GPIO_SET, 1u << pin, (val & 1) << pin };
	return mz_cmd(m, w, 3, 50);
}

int mz_gpio_dir(struct mz_dev *m, unsigned pin, unsigned out)
{
	uint32_t w[] = { MZ_CMD_GPIO_DIR, 1u << pin, (out & 1) << pin };
	return mz_cmd(m, w, 3, 50);
}
