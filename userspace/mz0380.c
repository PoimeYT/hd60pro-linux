/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mz0380.h"

#include <dirent.h>
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

/* ---- card identification and system checks (sysfs only, no device access) ---- */

static bool is_hd60pro(const char *bdf)
{
	unsigned ven, dev, sven;
	if (pci_attr_hex(bdf, "vendor", &ven) || pci_attr_hex(bdf, "device", &dev) ||
	    pci_attr_hex(bdf, "subsystem_vendor", &sven))
		return false;
	return ven == 0x12ab && (dev == 0x0380 || dev == 0x0381) && sven == 0x1cfa;
}

int mz_find_card(char *bdf, size_t len)
{
	DIR *dir = opendir("/sys/bus/pci/devices");
	if (!dir)
		return -errno;
	int n = 0;
	struct dirent *e;
	while ((e = readdir(dir))) {
		if (!pci_bdf_valid(e->d_name) || !is_hd60pro(e->d_name))
			continue;
		/* Keep the lowest address so the choice does not depend on readdir order. */
		if (!n || strcmp(e->d_name, bdf) < 0)
			snprintf(bdf, len, "%s", e->d_name);
		n++;
	}
	closedir(dir);
	return n;
}

/* Devices that may share the card's IOMMU group without blocking VFIO. */
static bool group_peer_ok(const char *bdf, char *drv, size_t len)
{
	unsigned class = 0;
	pci_driver(bdf, drv, len);
	pci_attr_hex(bdf, "class", &class);
	return !drv[0] || !strcmp(drv, "vfio-pci") || !strcmp(drv, "pci-stub") || !strcmp(drv, "pcieport") ||
	       (class >> 8) == 0x0604;
}

int mz_preflight(const char *bdf, FILE *report)
{
	if (!pci_bdf_valid(bdf)) {
		fprintf(stderr, "\"%s\" is not a PCI address (expected e.g. 0000:05:00.0)\n", bdf);
		return -EINVAL;
	}
	unsigned ven = 0, dev = 0, sven = 0, sdev = 0;
	if (pci_attr_hex(bdf, "vendor", &ven) || pci_attr_hex(bdf, "device", &dev)) {
		fprintf(stderr, "no PCI device at %s\n", bdf);
		return -ENODEV;
	}
	pci_attr_hex(bdf, "subsystem_vendor", &sven);
	pci_attr_hex(bdf, "subsystem_device", &sdev);
	if (!is_hd60pro(bdf)) {
		fprintf(stderr,
			"%s is not an HD60 Pro (it is %04x:%04x, subsystem %04x:%04x): refusing to touch it.\n"
			"Run without -d to find the card automatically.\n", bdf, ven, dev, sven, sdev);
		return -ENODEV;
	}
	if (report)
		fprintf(report, "card:      %s (%04x:%04x, subsystem %04x:%04x)\n", bdf, ven, dev, sven, sdev);
	if (dev == 0x0381 || sdev != 0x0006)
		fprintf(stderr, "note: only subsystem 1cfa:0006 of 12ab:0380 is tested; this revision is experimental\n");

	int grp = pci_iommu_group(bdf);
	if (grp < 0) {
		fprintf(stderr,
			"The IOMMU is off (the card has no IOMMU group), so the driver cannot run.\n"
			"Enable VT-d/AMD-Vi in the firmware setup; Intel systems usually also need\n"
			"intel_iommu=on on the kernel command line, then a reboot.\n"
			"Do not work around this with VFIO's no-IOMMU mode: the card could then\n"
			"write anywhere in memory.\n");
		return -ENOTSUP;
	}
	if (report)
		fprintf(report, "IOMMU:     on, card is in group %d\n", grp);

	/* Everything else in the group must be idle, or VFIO refuses the group. */
	char path[256];
	snprintf(path, sizeof(path), "/sys/kernel/iommu_groups/%d/devices", grp);
	DIR *dir = opendir(path);
	int busy = 0;
	struct dirent *e;
	while (dir && (e = readdir(dir))) {
		char drv[64];
		if (!pci_bdf_valid(e->d_name) || !strcmp(e->d_name, bdf) || group_peer_ok(e->d_name, drv, sizeof(drv)))
			continue;
		if (!busy++)
			fprintf(stderr, "The card shares IOMMU group %d with devices the system is using:\n", grp);
		unsigned class = 0;
		pci_attr_hex(e->d_name, "class", &class);
		fprintf(stderr, "  %s  class %06x  driver %s\n", e->d_name, class, drv);
	}
	if (dir)
		closedir(dir);
	if (busy) {
		fprintf(stderr,
			"The driver cannot run like this. Do NOT bind those devices to vfio-pci and do not\n"
			"use the ACS override patch: that would take them away from the running system\n"
			"(disks, network, graphics) or remove the protection the IOMMU gives.\n"
			"Move the card to another PCIe slot (one wired to the CPU usually has its own group).\n");
		return -EBUSY;
	}
	if (report)
		fprintf(report, "group:     nothing else in use in group %d\n", grp);

	char drv[64];
	pci_driver(bdf, drv, sizeof(drv));
	if (strcmp(drv, "vfio-pci")) {
		if (drv[0])
			fprintf(stderr, "The card is bound to the \"%s\" driver, not vfio-pci.\n", drv);
		else
			fprintf(stderr, "The card is not bound to vfio-pci yet (see \"Quick start\" in the README, step 2).\n");
		return -ENOENT;
	}
	if (report)
		fprintf(report, "binding:   vfio-pci\n");

	snprintf(path, sizeof(path), "/dev/vfio/%d", grp);
	if (access(path, R_OK | W_OK)) {
		int err = errno;
		fprintf(stderr, "No access to %s: %s (README step 3: sudo chown $USER %s)\n", path, strerror(err), path);
		return -err;
	}
	if (report)
		fprintf(report, "access:    %s is readable and writable\n", path);
	return 0;
}

int mz_open(struct mz_dev *m, const char *bdf)
{
	memset(m, 0, sizeof(*m));
	m->i2c_bank = -1;
	m->vfio.container = m->vfio.group = m->vfio.device = m->vfio.irq_efd = -1;
	int r = mz_preflight(bdf, NULL);
	if (r)
		return r;
	r = vfio_open(&m->vfio, bdf);
	if (r)
		return r;
	/* Same identity check through the device itself, before any register access. */
	uint16_t id[2] = { 0, 0 };
	if (vfio_cfg_read(&m->vfio, 0, id, 4) || id[0] != 0x12ab || (id[1] != 0x0380 && id[1] != 0x0381)) {
		fprintf(stderr, "mz0380: %s answers as %04x:%04x, not an HD60 Pro\n", bdf, id[0], id[1]);
		vfio_close(&m->vfio);
		return -ENODEV;
	}
	if (!m->vfio.bar[0].map || !m->vfio.bar[5].map || m->vfio.bar[0].size < MZ_FW_WINDOW ||
	    m->vfio.bar[5].size < 0x100) {
		fprintf(stderr, "mz0380: BAR0/BAR5 not mapped\n");
		vfio_close(&m->vfio);
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
	m->bar0 = m->bar5 = NULL;
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
	/* The result words are only valid until the next command: read them under the lock. */
	mz_lock(m);
	int r = mz_cmd(m, w, 4, 50) ? -ETIMEDOUT : (int)(mz_rd(m, MZ_RESULT(2)) & 0xff);
	mz_unlock(m);
	return r;
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
	mz_lock(m);
	if (mz_cmd(m, w, 2, 50)) {
		mz_unlock(m);
		return -ETIMEDOUT;
	}
	uint32_t tmp[8];
	for (int i = 0; i < 8; i++)
		tmp[i] = mz_rd(m, MZ_RESULT(1 + i));
	mz_unlock(m);
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
	if (pin > 31)
		return -EINVAL;
	uint32_t w[] = { MZ_CMD_GPIO_GET, 1u << pin, 0 };
	mz_lock(m);
	int r = mz_cmd(m, w, 3, 50) ? -ETIMEDOUT : (int)((mz_rd(m, MZ_RESULT(1)) >> pin) & 1);
	mz_unlock(m);
	return r;
}

int mz_gpio_set(struct mz_dev *m, unsigned pin, unsigned val)
{
	if (pin > 31)
		return -EINVAL;
	uint32_t w[] = { MZ_CMD_GPIO_SET, 1u << pin, (val & 1) << pin };
	return mz_cmd(m, w, 3, 50);
}

int mz_gpio_dir(struct mz_dev *m, unsigned pin, unsigned out)
{
	if (pin > 31)
		return -EINVAL;
	uint32_t w[] = { MZ_CMD_GPIO_DIR, 1u << pin, (out & 1) << pin };
	return mz_cmd(m, w, 3, 50);
}
