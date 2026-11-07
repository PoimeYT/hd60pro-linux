/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Minimal VFIO (type1 IOMMU) helper for a single PCI function. */
#ifndef HD60_VFIO_H
#define HD60_VFIO_H

#include <stddef.h>
#include <stdint.h>

struct vfio_bar {
	volatile uint8_t *map;
	uint64_t size;
	uint64_t bus_addr;	/* address the device decodes (from sysfs) */
};

struct vfio_dev {
	int container;
	int group;
	int device;
	uint64_t cfg_offset;	/* config space region offset within device fd */
	struct vfio_bar bar[6];
	int irq_efd;		/* eventfd for INTx, -1 when unused */
};

struct dma_buf {
	void *virt;
	uint64_t iova;
	size_t size;
};

int vfio_open(struct vfio_dev *d, const char *bdf);
void vfio_close(struct vfio_dev *d);
int vfio_cfg_read(struct vfio_dev *d, unsigned off, void *buf, size_t len);
int vfio_cfg_write(struct vfio_dev *d, unsigned off, const void *buf, size_t len);
int vfio_dma_alloc(struct vfio_dev *d, struct dma_buf *b, size_t size, uint64_t iova);
void vfio_dma_free(struct vfio_dev *d, struct dma_buf *b);
int vfio_intx_enable(struct vfio_dev *d);
int vfio_intx_unmask(struct vfio_dev *d);

#endif
