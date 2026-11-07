/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host interface of the YUAN MZ0380 (VATICS "Mozart" SoC) used on the
 * Elgato Game Capture HD60 Pro (PCI 12ab:0380 / 12ab:0381).
 *
 * Reconstructed from the Windows driver e60MZ0380.X64.SYS 1.1.0.191;
 * see docs/PROTOCOL.md for the evidence behind each value.
 */
#ifndef HD60_MZ0380_H
#define HD60_MZ0380_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "vfio.h"

/* BAR0: mailbox / status window shared with the card's ep.ko */
#define MZ_DOORBELL		0x00	/* write CMD to submit, ACK to ack an IRQ */
#define   MZ_DOORBELL_CMD	0x800
#define   MZ_DOORBELL_ACK	0x400
#define MZ_PARAM(i)		(0x04 + 4 * (i))	/* PARAM(0) is the command id */
#define MZ_RESULT(i)		(0x08 + 4 * (i))	/* RESULT(0) overlaps PARAM(1) */
#define MZ_CMD_STATUS		0x2c	/* bit0: synchronous command done */
#define   MZ_STATUS_READY	0xaaaaaaaa
#define MZ_IRQ_STATUS		0x30
#define   MZ_IRQ_CMD_DONE	(1u << 11)
#define MZ_VIDEO_INDEX		0x40	/* per-pipe completed buffer index */
#define MZ_INDEX_1		0x44
#define MZ_INDEX_2		0x48
#define MZ_AUDIO_INDEX		0x4c	/* audio slot index, 4 bits per channel */
#define MZ_FW_WINDOW		0x60	/* firmware upload window (not used) */

/* BAR5: PCIe endpoint controller of the SoC */
#define EP_INBOUND_START	0x30
#define EP_INBOUND_LIMIT	0x38
#define EP_IRQ_CLEAR		0xdc

/* Mailbox command ids */
enum mz_cmd {
	MZ_CMD_HELLO		= 0x01,
	MZ_CMD_SET_BUF_VIDEO	= 0x02,
	MZ_CMD_SET_BUF_AUDIO	= 0x03,
	MZ_CMD_SET_BUF_ENC0	= 0x04,
	MZ_CMD_SET_BUF_ENC1	= 0x05,
	MZ_CMD_START		= 0x06,
	MZ_CMD_STOP		= 0x07,
	MZ_CMD_SET_BUF_VIDEO_EX	= 0x08,
	MZ_CMD_GET_VERSION	= 0x0a,
	MZ_CMD_FW_BEGIN		= 0x0b,	/* firmware update, never sent */
	MZ_CMD_FW_COMMIT	= 0x0c,
	MZ_CMD_BASEFW_BEGIN	= 0x0e,	/* flash update, never sent */
	MZ_CMD_BASEFW_COMMIT	= 0x0f,
	MZ_CMD_GPIO_GET		= 0x14,
	MZ_CMD_GPIO_SET		= 0x15,
	MZ_CMD_GPIO_DIR		= 0x17,
	MZ_CMD_SPI_GET		= 0x18,
	MZ_CMD_SPI_SET		= 0x19,
	MZ_CMD_I2C_READ		= 0x1a,
	MZ_CMD_I2C_WRITE	= 0x1b,
	MZ_CMD_CRYPT_READ	= 0x1c,
	MZ_CMD_CRYPT_WRITE	= 0x1d,
	MZ_CMD_I2C_READ_N	= 0x1e,
	MZ_CMD_I2C_WRITE_N	= 0x1f,
	MZ_CMD_I2C_COMBO	= 0x20,
	MZ_CMD_SET_VIDEO_IN	= 0x29,
	MZ_CMD_SET_AUDIO_IN	= 0x2a,
	MZ_CMD_SET_ENCODER	= 0x2d,
	MZ_CMD_SET_PREVIEW	= 0x31,
	MZ_CMD_SET_OSD		= 0x62,
};

/* 8-bit I2C addresses on the card's bus */
#define I2C_MST3367		0x9c	/* HDMI receiver, register bank in reg 0 */
#define I2C_IT6612		0x98	/* HDMI transmitter (passthrough) */
#define I2C_CRYPT		0xa2

struct mz_dev {
	struct vfio_dev vfio;
	volatile uint32_t *bar0;
	volatile uint32_t *bar5;
	uint64_t bar0_bus;
	int i2c_bank;		/* bank last written to MST3367 reg 0, -1 unknown */
	int verbose;
	uint32_t pending_irq;	/* data-pipe IRQ bits seen while waiting for a command */
	pthread_mutex_t lock;	/* serialises the mailbox (recursive) */
	unsigned board_major, board_minor;
};

static inline uint32_t mz_rd(struct mz_dev *m, unsigned off)
{
	return m->bar0[off / 4];
}

static inline void mz_wr(struct mz_dev *m, unsigned off, uint32_t v)
{
	m->bar0[off / 4] = v;
	__sync_synchronize();
}

static inline void ep_wr(struct mz_dev *m, unsigned off, uint32_t v)
{
	m->bar5[off / 4] = v;
	__sync_synchronize();
}

int mz_open(struct mz_dev *m, const char *bdf);
void mz_close(struct mz_dev *m);
void mz_ack_irq(struct mz_dev *m);
void mz_lock(struct mz_dev *m);
void mz_unlock(struct mz_dev *m);

/* Send words[0..n) (words[0] = command id). Returns 0 on completion. */
int mz_cmd(struct mz_dev *m, const uint32_t *words, int n, int timeout_ms);

int mz_boot(struct mz_dev *m);

int mz_i2c_read(struct mz_dev *m, uint8_t addr, uint8_t reg);
int mz_i2c_write(struct mz_dev *m, uint8_t addr, uint8_t reg, uint8_t val);
int mz_i2c_read_n(struct mz_dev *m, uint8_t addr, uint8_t reg, uint8_t *buf, unsigned len);
int mz_i2c_write_n(struct mz_dev *m, uint8_t addr, uint8_t reg, const uint8_t *buf, unsigned len);
/* Banked access used for the MST3367 (bank select = write to reg 0). */
int mz_bank_read(struct mz_dev *m, uint8_t addr, uint8_t bank, uint8_t reg);
int mz_bank_write(struct mz_dev *m, uint8_t addr, uint8_t bank, uint8_t reg, uint8_t val);
int mz_gpio_get(struct mz_dev *m, unsigned pin);
int mz_gpio_set(struct mz_dev *m, unsigned pin, unsigned val);
int mz_gpio_dir(struct mz_dev *m, unsigned pin, unsigned out);

#endif
