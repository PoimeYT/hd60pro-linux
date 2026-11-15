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
| Companion app (preview, record, headphone listening) | New, see [The app](#the-app) |
| Setup at boot, driver at login (`system/install.sh`) | New, see [Install](#install-set-up-at-boot-driver-starts-at-login) |
| Packages | Planned |

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

## Install (set up at boot, driver starts at login)

```bash
./system/install.sh
```

Run it as your normal user; it asks for `sudo` for the system parts. After
that, every boot binds the card to `vfio-pci` for your user and creates the
"HD60 Pro" camera at `/dev/video60`, and every login starts the driver
(`hd60pro.service`, a systemd user service). The IOMMU still has to be on
(`intel_iommu=on` on Intel); the installer does not touch the bootloader.

The boot step only ever acts on the HD60 Pro: it finds it by PCI ID, leaves it
alone if it shares an IOMMU group with a device in use, and never unbinds or
rebinds anything else. If the system is not ready, the service is skipped
instead of restarting in a loop; `hd60pro check` says why.

```bash
systemctl --user status hd60pro.service   # state
journalctl --user -u hd60pro -b           # driver output
./system/uninstall.sh                     # remove everything again
```

## The app

`system/install.sh` also builds and installs the app when Qt 6 is present
(`qt6-base` and `qt6-multimedia` on Arch; the `-devel`/`-dev` packages on
Fedora and Debian/Ubuntu). It appears in the application menu as **HD60 Pro**.

- **Preview** of the game fills the window.
- **Record** saves to `~/Videos/HD60 Pro/` with one click. Quality presets
  and a custom mode (H.264/HEVC, bitrate, resolution, MP4/MKV) are in
  Settings. The GPU encoder is used when it works (NVENC, then VAAPI), else
  software. MP4 files are written so that a crash still leaves a playable file,
  and a recording stops cleanly if the signal is lost or the mode changes.
- **Listen** plays the game sound on your headphones or speakers, with its
  own volume. With Listen on, closing the window keeps the app in the system
  tray; "Open in the tray when I log in" in Settings makes that permanent.

The app needs `ffmpeg`, `pw-loopback` and `pactl`. It only talks to the
driver service; HDMI passthrough, the camera and the audio input work without
it. Streaming is OBS's job: use the "HD60 Pro" camera and "HD60 Pro Audio".

Tests: `cd app/tests && qmake6 && make && ./tst_logic`.

## Quick start (manual setup)

These steps are what `system/install.sh` automates; use them to try the
driver without installing anything.

> **Read this first.** Only ever bind the HD60 Pro itself to `vfio-pci`. If the
> card shares its IOMMU group with other devices (step 1 shows them), do
> **not** bind those too, do not use the ACS override patch, and do not use
> VFIO's no-IOMMU mode: each of these can take your disk, network or graphics
> card away from the running system or let the card write anywhere in memory.
> Move the card to another PCIe slot instead. Nothing below needs to be made
> permanent (no initramfs, bootloader or `/etc` changes); a reboot undoes all
> of it.

1. **Find the card** and check what shares its IOMMU group:

   ```bash
   BDF=$(lspci -D -d 12ab: | cut -d' ' -f1); echo "$BDF"
   ```

   ```bash
   ls /sys/bus/pci/devices/$BDF/iommu_group/devices/
   ```

   The first command must print exactly one address such as `0000:05:00.0`.
   The second should list only that address (PCI bridges are fine too). If it
   lists a disk, network or graphics device, stop here and move the card to
   another slot. The group number is the last part of
   `readlink /sys/bus/pci/devices/$BDF/iommu_group`.

2. **Attach the card to `vfio-pci`** (no other Linux driver claims it):

   ```bash
   sudo modprobe vfio-pci
   ```

   ```bash
   echo vfio-pci | sudo tee /sys/bus/pci/devices/$BDF/driver_override
   ```

   ```bash
   echo $BDF | sudo tee /sys/bus/pci/drivers_probe
   ```

3. **Let your user open it** (replace `16` with your group number):

   ```bash
   sudo chown $USER /dev/vfio/16
   ```

   Then let the driver confirm the setup. This only reads sysfs and never
   touches the card:

   ```bash
   ./userspace/hd60pro check
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
  check                  check the setup without touching the card
  run [DEV] [PCM]        stream to a v4l2loopback device, export audio, drive HDMI OUT
  probe                  handshake with the card and print its firmware version
  detect [SECS]          initialise the HDMI receiver and report the input format
  capture PREFIX [SECS]  save raw frames (.yuv), H.264 (.h264) and audio (.pcm)
  passthrough [SECS]     drive HDMI OUT only
  tx-status [SECS]       read the display's EDID and watch HDMI OUT hot-plug
  regs, i2c-read, i2c-write, i2c-dump, i2c-scan, gpio-get   low-level debugging
```

The card is found by its PCI ID; `-d` selects one by address when there are
several. `-v` adds detail (`-vv` for every mailbox command).

Every command first checks that the address really is an HD60 Pro, that the
IOMMU is on and that nothing else in the card's IOMMU group is in use, and
refuses to open the device otherwise. `run` and `capture` stop the card and
remove the audio input again on Ctrl-C, `kill`, a closed terminal or an
error; a leftover audio input from a run that was killed with `SIGKILL` is
removed by the next run.

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
app/           Qt 6 companion app (preview, record, listen)
system/        install/uninstall scripts and the boot-time binding
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
