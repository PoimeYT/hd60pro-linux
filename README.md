# hd60pro-linux

A Linux driver for the **Elgato Game Capture HD60 Pro** (PCIe), reverse
engineered from Elgato's Windows driver. Elgato never shipped Linux support
for this card; this project makes it a normal Linux capture device:

- **Video**: uncompressed 1080p60 into a v4l2loopback camera, usable by OBS,
  ffmpeg, browsers and anything else that reads a webcam.
- **Audio**: HDMI audio as a PipeWire/PulseAudio input called
  "HD60 Pro Audio".
- **HDMI OUT passthrough**: the card's HDMI output works, with picture and
  sound, at the input's resolution (up to 1080p60).

It runs entirely in userspace through [VFIO](https://docs.kernel.org/driver-api/vfio.html),
so a bug cannot crash your kernel, and the IOMMU confines the card's DMA to
the buffers the driver gives it.

> Not affiliated with, endorsed by, or supported by Elgato or Corsair.
> "Elgato" and "Game Capture HD60 Pro" are trademarks of their owners and are
> used here only to say which hardware this works with.

## Status

| Feature | State |
| --- | --- |
| Card bring-up, firmware handshake | Working |
| HDMI input detection (resolution, frame rate, audio rate, colour space) | Working |
| Raw video capture, 1080p60 | Working, 60.00 fps measured |
| Audio capture (48 kHz stereo) | Working |
| HDMI OUT passthrough (picture + sound) | Working |
| HDCP on HDMI OUT for encrypted sources | Ported, untested |
| Card's built-in H.264 encoder | Not working yet (raw capture is used instead) |
| Companion app (preview, record, headphone monitoring) | Planned, see [design](docs/superpowers/specs/2026-11-07-qt-companion-app-design.md) |
| Easy setup / packages | Planned |

Tested with a Nintendo Switch 2 at 1080p60 and a Samsung monitor on HDMI OUT.
See [COMPATIBILITY.md](COMPATIBILITY.md) for cards, modes and systems.

## Requirements

- An HD60 Pro (PCI ID `12ab:0380`, subsystem vendor `1cfa`).
  [Which revision do I have?](COMPATIBILITY.md#identify-your-card)
- x86-64 Linux with the IOMMU enabled (`/sys/kernel/iommu_groups/` is not
  empty). Intel systems often need `intel_iommu=on` on the kernel command line.
- The `vfio-pci` kernel module.
- [v4l2loopback](https://github.com/umlaeute/v4l2loopback) 0.12 or newer.
- PipeWire with PulseAudio compatibility (`pactl`), for the audio input.
- A C compiler and `make`.

## Quick start (manual setup)

These steps are what the planned setup tool will automate.

1. **Find the card** and its IOMMU group:

   ```bash
   lspci -nn -d 12ab:
   ```

   ```bash
   readlink /sys/bus/pci/devices/0000:05:00.0/iommu_group
   ```

   Replace `0000:05:00.0` with the address `lspci` printed. The last part of
   the second command's output is the group number.

2. **Attach the card to `vfio-pci`** (no other Linux driver claims it):

   ```bash
   sudo modprobe vfio-pci
   ```

   ```bash
   echo vfio-pci | sudo tee /sys/bus/pci/devices/0000:05:00.0/driver_override
   ```

   ```bash
   echo 0000:05:00.0 | sudo tee /sys/bus/pci/drivers_probe
   ```

3. **Let your user open it** (replace `16` with your group number):

   ```bash
   sudo chown $USER /dev/vfio/16
   ```

4. **Allow enough locked memory** for the DMA buffers (about 32 MB; with the
   default 8 MB the driver falls back to a single shared buffer). For the
   current shell:

   ```bash
   sudo prlimit --pid $$ --memlock=unlimited:unlimited
   ```

5. **Create a loopback camera**:

   ```bash
   sudo modprobe v4l2loopback exclusive_caps=1 card_label="HD60 Pro" video_nr=60
   ```

   If v4l2loopback is already loaded (for example for OBS's virtual camera),
   add a device to it instead:

   ```bash
   sudo v4l2loopback-ctl add -n "HD60 Pro" -x 1 /dev/video60
   ```

6. **Build and run**:

   ```bash
   make -C userspace
   ```

   ```bash
   ./userspace/hd60pro run /dev/video60
   ```

The driver prints the detected input and HDMI OUT state, then keeps running
until Ctrl-C. While it runs:

- **OBS**: add *Video Capture Device (V4L2)* → *HD60 Pro*, and
  *Audio Input Capture* → *HD60 Pro Audio*.
- **Quick look**: `ffplay -f v4l2 /dev/video60`.
- **Headphones**: route "HD60 Pro Audio" to your output, for example with
  `pw-loopback -C hd60pro`.

If the card is passed through to a virtual machine, shut the VM down first;
only one of them can use it.

## Commands

```text
hd60pro [-d BDF] [-v] COMMAND
  run [DEV] [PCM]        stream to a v4l2loopback device, export audio, drive HDMI OUT
  probe                  handshake with the card and print its firmware version
  detect [SECS]          initialise the HDMI receiver and report the input format
  capture PREFIX [SECS]  save raw frames (.yuv), H.264 (.h264) and audio (.pcm)
  passthrough [SECS]     drive HDMI OUT only
  tx-status [SECS]       read the display's EDID and watch HDMI OUT hot-plug
  regs, i2c-read, i2c-write, i2c-dump, i2c-scan, gpio-get   low-level debugging
```

`-d` selects the card's PCI address (default `0000:05:00.0`), `-v` adds
detail (`-vv` for every mailbox command).

## How it works

The HD60 Pro is a small ARM computer (a YUAN MZ0380 / VATICS "Mozart" SoC
running embedded Linux from its own flash) with an MStar MST3367 HDMI
receiver and an ITE IT6612 HDMI transmitter. The host talks to the card
through a mailbox in BAR0, registers DMA buffers for raw video, H.264 and
audio, and drives both HDMI chips over the card's I2C bus. The full protocol,
with the evidence behind each value, is in [docs/PROTOCOL.md](docs/PROTOCOL.md).

```text
userspace/
  vfio.c       VFIO container/group/device, BAR mapping, IOMMU DMA buffers
  mz0380.c     mailbox, boot handshake, I2C/GPIO tunnelling
  mst3367.c    HDMI receiver init, format detection, colour conversion
  it6612.c     HDMI transmitter (passthrough) state machine, EDID, HDCP
  stream.c     buffer registration, capture start/stop, frame delivery
  v4l2out.c    v4l2loopback output
  main.c       command-line tool and the `run` service loop
tools/         PDB symbol and registry-default extractors used for the research
docs/          protocol notes and design documents
```

## Safety

The card keeps its software in on-board flash. Elgato's driver can rewrite
that flash; **this driver never sends the firmware or flash commands**
(`0x0b`, `0x0c`, `0x0e`, `0x0f`), so it cannot brick the card. If the card
reports a firmware version other than the one Elgato's driver expects, the
driver reports it and carries on.

## Contributing

Reports from other HD60 Pro revisions, other sources and other displays are
the most useful thing right now. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

GPL-2.0-or-later; see [LICENSE](LICENSE). The passthrough code includes small
data tables (video timings, colour matrices, clock values) extracted from
Elgato's Windows driver for interoperability; see
[userspace/it6612_tables.h](userspace/it6612_tables.h).
