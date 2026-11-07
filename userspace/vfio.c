/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "vfio.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/vfio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int read_bus_addrs(struct vfio_dev *d, const char *bdf)
{
	char path[256];
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource", bdf);
	FILE *f = fopen(path, "r");
	if (!f)
		return -errno;
	for (int i = 0; i < 6; i++) {
		unsigned long long start, end, flags;
		if (fscanf(f, "%llx %llx %llx", &start, &end, &flags) != 3)
			break;
		d->bar[i].bus_addr = start;
	}
	fclose(f);
	return 0;
}

static int group_of(const char *bdf)
{
	char path[256], link[256];
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", bdf);
	ssize_t n = readlink(path, link, sizeof(link) - 1);
	if (n < 0)
		return -errno;
	link[n] = 0;
	char *slash = strrchr(link, '/');
	return atoi(slash ? slash + 1 : link);
}

int vfio_open(struct vfio_dev *d, const char *bdf)
{
	memset(d, 0, sizeof(*d));
	d->container = d->group = d->device = d->irq_efd = -1;

	int grp = group_of(bdf);
	if (grp < 0) {
		fprintf(stderr, "vfio: no iommu group for %s\n", bdf);
		return grp;
	}
	read_bus_addrs(d, bdf);

	d->container = open("/dev/vfio/vfio", O_RDWR);
	if (d->container < 0) {
		perror("open /dev/vfio/vfio");
		return -errno;
	}
	if (ioctl(d->container, VFIO_GET_API_VERSION) != VFIO_API_VERSION ||
	    !ioctl(d->container, VFIO_CHECK_EXTENSION, VFIO_TYPE1v2_IOMMU)) {
		fprintf(stderr, "vfio: type1v2 IOMMU unsupported\n");
		return -ENOTSUP;
	}

	char gpath[64];
	snprintf(gpath, sizeof(gpath), "/dev/vfio/%d", grp);
	d->group = open(gpath, O_RDWR);
	if (d->group < 0) {
		fprintf(stderr, "open %s: %s (need ownership of the group node)\n", gpath, strerror(errno));
		return -errno;
	}
	struct vfio_group_status gs = { .argsz = sizeof(gs) };
	ioctl(d->group, VFIO_GROUP_GET_STATUS, &gs);
	if (!(gs.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		fprintf(stderr, "vfio: group %d not viable (bind every device in it to vfio-pci)\n", grp);
		return -EINVAL;
	}
	if (ioctl(d->group, VFIO_GROUP_SET_CONTAINER, &d->container) ||
	    ioctl(d->container, VFIO_SET_IOMMU, VFIO_TYPE1v2_IOMMU)) {
		perror("vfio: set container/iommu");
		return -errno;
	}
	d->device = ioctl(d->group, VFIO_GROUP_GET_DEVICE_FD, bdf);
	if (d->device < 0) {
		perror("vfio: get device fd (is a VM using it?)");
		return -errno;
	}

	struct vfio_region_info ri = { .argsz = sizeof(ri), .index = VFIO_PCI_CONFIG_REGION_INDEX };
	if (ioctl(d->device, VFIO_DEVICE_GET_REGION_INFO, &ri)) {
		perror("vfio: config region");
		return -errno;
	}
	d->cfg_offset = ri.offset;

	for (int i = 0; i < 6; i++) {
		struct vfio_region_info bi = { .argsz = sizeof(bi), .index = VFIO_PCI_BAR0_REGION_INDEX + i };
		if (ioctl(d->device, VFIO_DEVICE_GET_REGION_INFO, &bi) || !bi.size)
			continue;
		d->bar[i].size = bi.size;
		if (!(bi.flags & VFIO_REGION_INFO_FLAG_MMAP)) {
			fprintf(stderr, "vfio: BAR%d not mmappable\n", i);
			continue;
		}
		void *p = mmap(NULL, bi.size, PROT_READ | PROT_WRITE, MAP_SHARED, d->device, bi.offset);
		if (p == MAP_FAILED) {
			fprintf(stderr, "vfio: mmap BAR%d: %s\n", i, strerror(errno));
			continue;
		}
		d->bar[i].map = p;
	}

	/* Memory space + bus master. */
	uint16_t cmd;
	vfio_cfg_read(d, 4, &cmd, 2);
	cmd |= 0x6;
	vfio_cfg_write(d, 4, &cmd, 2);
	return 0;
}

void vfio_close(struct vfio_dev *d)
{
	for (int i = 0; i < 6; i++)
		if (d->bar[i].map)
			munmap((void *)d->bar[i].map, d->bar[i].size);
	if (d->irq_efd >= 0)
		close(d->irq_efd);
	if (d->device >= 0)
		close(d->device);
	if (d->group >= 0)
		close(d->group);
	if (d->container >= 0)
		close(d->container);
}

int vfio_cfg_read(struct vfio_dev *d, unsigned off, void *buf, size_t len)
{
	return pread(d->device, buf, len, d->cfg_offset + off) == (ssize_t)len ? 0 : -EIO;
}

int vfio_cfg_write(struct vfio_dev *d, unsigned off, const void *buf, size_t len)
{
	return pwrite(d->device, buf, len, d->cfg_offset + off) == (ssize_t)len ? 0 : -EIO;
}

int vfio_dma_alloc(struct vfio_dev *d, struct dma_buf *b, size_t size, uint64_t iova)
{
	size = (size + 0xfff) & ~(size_t)0xfff;
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	if (p == MAP_FAILED)
		return -errno;
	memset(p, 0, size);
	struct vfio_iommu_type1_dma_map m = {
		.argsz = sizeof(m),
		.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
		.vaddr = (uintptr_t)p,
		.iova = iova,
		.size = size,
	};
	if (ioctl(d->container, VFIO_IOMMU_MAP_DMA, &m)) {
		int e = errno;
		fprintf(stderr, "vfio: MAP_DMA %zu bytes @%#llx: %s%s\n", size, (unsigned long long)iova,
			strerror(e), e == ENOMEM ? " (raise RLIMIT_MEMLOCK or grant CAP_IPC_LOCK)" : "");
		munmap(p, size);
		return -e;
	}
	b->virt = p;
	b->iova = iova;
	b->size = size;
	return 0;
}

void vfio_dma_free(struct vfio_dev *d, struct dma_buf *b)
{
	if (!b->virt)
		return;
	struct vfio_iommu_type1_dma_unmap u = { .argsz = sizeof(u), .iova = b->iova, .size = b->size };
	ioctl(d->container, VFIO_IOMMU_UNMAP_DMA, &u);
	munmap(b->virt, b->size);
	b->virt = NULL;
}

int vfio_intx_enable(struct vfio_dev *d)
{
	d->irq_efd = eventfd(0, EFD_CLOEXEC);
	if (d->irq_efd < 0)
		return -errno;
	char buf[sizeof(struct vfio_irq_set) + sizeof(int)];
	struct vfio_irq_set *s = (void *)buf;
	s->argsz = sizeof(buf);
	s->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
	s->index = VFIO_PCI_INTX_IRQ_INDEX;
	s->start = 0;
	s->count = 1;
	memcpy(s->data, &d->irq_efd, sizeof(int));
	if (ioctl(d->device, VFIO_DEVICE_SET_IRQS, s))
		return -errno;
	return 0;
}

int vfio_intx_unmask(struct vfio_dev *d)
{
	struct vfio_irq_set s = {
		.argsz = sizeof(s),
		.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_UNMASK,
		.index = VFIO_PCI_INTX_IRQ_INDEX,
		.start = 0,
		.count = 1,
	};
	return ioctl(d->device, VFIO_DEVICE_SET_IRQS, &s) ? -errno : 0;
}
