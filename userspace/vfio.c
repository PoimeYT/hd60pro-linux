/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "vfio.h"

#include <ctype.h>
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

/* ---- sysfs helpers: none of these touch the device ---- */

bool pci_bdf_valid(const char *bdf)
{
	/* DDDD:BB:DD.F, as used in sysfs paths: reject anything else. */
	static const char pat[] = "xxxx:xx:xx.x";
	if (!bdf || strlen(bdf) != sizeof(pat) - 1)
		return false;
	for (size_t i = 0; pat[i]; i++) {
		if (pat[i] == 'x' ? !isxdigit((unsigned char)bdf[i]) : bdf[i] != pat[i])
			return false;
	}
	return true;
}

int pci_attr_hex(const char *bdf, const char *attr, unsigned *out)
{
	char path[256];
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/%s", bdf, attr);
	FILE *f = fopen(path, "r");
	if (!f)
		return -errno;
	int n = fscanf(f, "%x", out);
	fclose(f);
	return n == 1 ? 0 : -EIO;
}

/* Name of the bound kernel driver, "" when the device has none. */
int pci_driver(const char *bdf, char *buf, size_t len)
{
	char path[256], link[256];
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/driver", bdf);
	ssize_t n = readlink(path, link, sizeof(link) - 1);
	if (n < 0) {
		buf[0] = 0;
		return errno == ENOENT ? 0 : -errno;
	}
	link[n] = 0;
	char *slash = strrchr(link, '/');
	snprintf(buf, len, "%s", slash ? slash + 1 : link);
	return 0;
}

/* IOMMU group number, or a negative errno (-ENOENT: the IOMMU is off). */
int pci_iommu_group(const char *bdf)
{
	char path[256], link[256];
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", bdf);
	ssize_t n = readlink(path, link, sizeof(link) - 1);
	if (n < 0)
		return -errno;
	link[n] = 0;
	char *slash = strrchr(link, '/');
	const char *num = slash ? slash + 1 : link;
	char *endp;
	long g = strtol(num, &endp, 10);
	if (endp == num || *endp || g < 0 || g > 0xffff)
		return -EINVAL;
	return (int)g;
}

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

int vfio_open(struct vfio_dev *d, const char *bdf)
{
	int r;
	memset(d, 0, sizeof(*d));
	d->container = d->group = d->device = d->irq_efd = -1;

	if (!pci_bdf_valid(bdf)) {
		fprintf(stderr, "vfio: \"%s\" is not a PCI address (expected e.g. 0000:05:00.0)\n", bdf ? bdf : "");
		return -EINVAL;
	}
	int grp = pci_iommu_group(bdf);
	if (grp < 0) {
		fprintf(stderr, "vfio: no iommu group for %s\n", bdf);
		return grp;
	}
	if ((r = read_bus_addrs(d, bdf))) {
		fprintf(stderr, "vfio: cannot read the BAR addresses of %s: %s\n", bdf, strerror(-r));
		return r;
	}

	d->container = open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);
	if (d->container < 0) {
		r = -errno;
		fprintf(stderr, "open /dev/vfio/vfio: %s\n", strerror(-r));
		goto fail;
	}
	/* Type1 only: this refuses VFIO's no-IOMMU mode, where the card could DMA anywhere. */
	if (ioctl(d->container, VFIO_GET_API_VERSION) != VFIO_API_VERSION ||
	    ioctl(d->container, VFIO_CHECK_EXTENSION, VFIO_TYPE1v2_IOMMU) <= 0) {
		fprintf(stderr, "vfio: type1v2 IOMMU unsupported\n");
		r = -ENOTSUP;
		goto fail;
	}

	char gpath[64];
	snprintf(gpath, sizeof(gpath), "/dev/vfio/%d", grp);
	d->group = open(gpath, O_RDWR | O_CLOEXEC);
	if (d->group < 0) {
		r = -errno;
		fprintf(stderr, "open %s: %s%s\n", gpath, strerror(-r),
			r == -EBUSY ? " (a virtual machine or another hd60pro is using the card)" :
			r == -EACCES ? " (your user needs read/write access to this node)" :
			r == -ENOENT ? " (the card is not bound to vfio-pci)" : "");
		goto fail;
	}
	struct vfio_group_status gs = { .argsz = sizeof(gs) };
	if (ioctl(d->group, VFIO_GROUP_GET_STATUS, &gs)) {
		r = -errno;
		fprintf(stderr, "vfio: group status: %s\n", strerror(-r));
		goto fail;
	}
	if (!(gs.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		fprintf(stderr,
			"vfio: IOMMU group %d cannot be used: another device in it is in use by the system.\n"
			"      Do NOT bind those other devices to vfio-pci (that takes disks, network or\n"
			"      graphics away from the running system). Run \"hd60pro check\" to see them;\n"
			"      the fix is to move the card to a different PCIe slot.\n", grp);
		r = -EINVAL;
		goto fail;
	}
	if (ioctl(d->group, VFIO_GROUP_SET_CONTAINER, &d->container) ||
	    ioctl(d->container, VFIO_SET_IOMMU, VFIO_TYPE1v2_IOMMU)) {
		r = -errno;
		fprintf(stderr, "vfio: set container/iommu: %s\n", strerror(-r));
		goto fail;
	}
	d->device = ioctl(d->group, VFIO_GROUP_GET_DEVICE_FD, bdf);
	if (d->device < 0) {
		r = -errno;
		fprintf(stderr, "vfio: get device fd: %s (is a VM using it?)\n", strerror(-r));
		goto fail;
	}

	struct vfio_region_info ri = { .argsz = sizeof(ri), .index = VFIO_PCI_CONFIG_REGION_INDEX };
	if (ioctl(d->device, VFIO_DEVICE_GET_REGION_INFO, &ri)) {
		r = -errno;
		fprintf(stderr, "vfio: config region: %s\n", strerror(-r));
		goto fail;
	}
	d->cfg_offset = ri.offset;

	for (int i = 0; i < 6; i++) {
		struct vfio_region_info bi = { .argsz = sizeof(bi), .index = VFIO_PCI_BAR0_REGION_INDEX + i };
		if (ioctl(d->device, VFIO_DEVICE_GET_REGION_INFO, &bi) || !bi.size)
			continue;
		if (!(bi.flags & VFIO_REGION_INFO_FLAG_MMAP)) {
			fprintf(stderr, "vfio: BAR%d not mmappable\n", i);
			continue;
		}
		void *p = mmap(NULL, bi.size, PROT_READ | PROT_WRITE, MAP_SHARED, d->device, bi.offset);
		if (p == MAP_FAILED) {
			fprintf(stderr, "vfio: mmap BAR%d: %s\n", i, strerror(errno));
			continue;
		}
		d->bar[i].size = bi.size;
		d->bar[i].map = p;
	}

	/* Memory space + bus master. */
	uint16_t cmd;
	if ((r = vfio_cfg_read(d, 4, &cmd, 2)))
		goto fail;
	cmd |= 0x6;
	if ((r = vfio_cfg_write(d, 4, &cmd, 2)))
		goto fail;
	return 0;
fail:
	vfio_close(d);
	return r;
}

void vfio_close(struct vfio_dev *d)
{
	if (d->device >= 0) {
		/* Stop the card mastering the bus before its DMA mappings go away. */
		uint16_t cmd;
		if (!vfio_cfg_read(d, 4, &cmd, 2)) {
			cmd &= ~0x4;
			vfio_cfg_write(d, 4, &cmd, 2);
		}
	}
	for (int i = 0; i < 6; i++) {
		if (d->bar[i].map)
			munmap((void *)d->bar[i].map, d->bar[i].size);
		d->bar[i].map = NULL;
	}
	if (d->irq_efd >= 0)
		close(d->irq_efd);
	if (d->device >= 0)
		close(d->device);
	if (d->group >= 0)
		close(d->group);
	if (d->container >= 0)
		close(d->container);
	d->container = d->group = d->device = d->irq_efd = -1;
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
	b->virt = NULL;
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
	if (ioctl(d->device, VFIO_DEVICE_SET_IRQS, s)) {
		int e = errno;
		close(d->irq_efd);
		d->irq_efd = -1;
		return -e;
	}
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
