# Security

## Reporting a vulnerability

Please report security problems privately through GitHub's
[private vulnerability reporting](https://github.com/PoimeYT/hd60pro-linux/security/advisories/new)
rather than in a public issue. Include the commit or version, what an
attacker could do, and how to reproduce it.

## What the driver is trusted with

The driver runs as your user, not root. To work it needs:

- **Ownership of the card's VFIO group node** (`/dev/vfio/<group>`). This
  gives the process full control of the HD60 Pro, but the IOMMU limits the
  card's DMA to the memory the driver maps for it.
- **A raised locked-memory limit** (about 32 MB) for those DMA buffers.
- **PipeWire/PulseAudio access** to create the "HD60 Pro Audio" input
  (`pactl load-module module-pipe-source`, FIFO at `/tmp/hd60pro-audio`).

Do not run it with the IOMMU disabled (no-IOMMU VFIO): the card would then be
able to DMA anywhere in memory.

The card itself runs Elgato/YUAN's embedded Linux, which this project does
not control or modify. Treat it like any other PCIe device with DMA access.
