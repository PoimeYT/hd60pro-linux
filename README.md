# hd60pro-linux

Use the **Elgato Game Capture HD60 Pro** (the internal PCIe card) on Linux.
Elgato never released Linux support for it; this project is a driver and a
small app, reverse engineered from Elgato's Windows driver.

What you get:

- **An app** with a live preview, a Record button and a Listen button that
  plays the game sound in your headphones.
- **A camera** called "HD60 Pro" (1080p60, uncompressed) that OBS, ffmpeg,
  browsers and video-call apps can use.
- **An audio input** called "HD60 Pro Audio".
- **HDMI OUT passthrough** to your TV or monitor, with picture and sound.

After installing, the card is set up at every boot and the driver starts when
you log in. There is nothing to run by hand.

> Not affiliated with, endorsed by, or supported by Elgato or Corsair.
> "Elgato" and "Game Capture HD60 Pro" are trademarks of their owners and are
> used here only to say which hardware this works with.

## Will it work for me?

You need:

- **The card**: an HD60 Pro, the internal PCIe one. Check with:

  ```bash
  lspci -nn -d 12ab:
  ```

  It should print one line ending in `[12ab:0380]`. Nothing printed means the
  card is not detected. The USB capture devices (HD60 S, HD60 X, Cam Link) and
  the 4K60 Pro are different hardware and are not supported.
  [Which revision do I have?](COMPATIBILITY.md#identify-your-card)
- **A 64-bit Intel or AMD PC** with virtualisation for devices (Intel VT-d or
  AMD-Vi, together called the IOMMU). Almost every PC from the last ten years
  has it; step 2 below switches it on.
- **A distribution with systemd and PipeWire**. That is the default on current
  Fedora, Ubuntu, Debian, Arch and their derivatives.

Tested on Fedora 44 and CachyOS (Arch-based) with a Nintendo Switch 2 at
1080p60. The Debian/Ubuntu instructions below follow the same steps but have
not been run by us; please [report](CONTRIBUTING.md#reporting-hardware-results)
how it goes. The card cannot capture or pass through 1440p, 4K or 120 Hz.
Details are in [COMPATIBILITY.md](COMPATIBILITY.md).

| Feature | State |
| --- | --- |
| Video capture, 1080p60 | Working |
| Audio capture (48 kHz stereo) | Working |
| HDMI OUT passthrough (picture + sound) | Working |
| App: preview, record, listen | Working, new |
| Copy-protected (HDCP) sources | Not expected to capture; passthrough untested |
| The card's built-in H.264 encoder | Not working (not needed: the PC encodes) |
| Distribution packages (AUR, RPM, deb) | Not yet: install from source as below |

## Install

### 1. Install what it needs

**Arch, CachyOS, EndeavourOS, Manjaro**

```bash
sudo pacman -S --needed base-devel git pciutils qt6-base qt6-multimedia qt6-multimedia-ffmpeg ffmpeg pipewire pipewire-pulse libpulse
```

You also need the `v4l2loopback` kernel module. CachyOS kernels include it.
On the others install it with:

```bash
sudo pacman -S --needed v4l2loopback-dkms linux-headers
```

(use the headers package that matches your kernel, for example
`linux-lts-headers` or `linux-zen-headers`).

**Fedora**

```bash
sudo dnf install gcc gcc-c++ make git pciutils qt6-qtbase-devel qt6-qtmultimedia-devel pipewire-utils pulseaudio-utils ffmpeg-free
```

`v4l2loopback` is not in Fedora itself. Enable
[RPM Fusion](https://rpmfusion.org/Configuration) and install it from there:

```bash
sudo dnf install v4l2loopback
```

RPM Fusion's full `ffmpeg` (in place of `ffmpeg-free`) is needed for
recording with an NVIDIA card's encoder.

**Debian, Ubuntu, Linux Mint, Pop!_OS** (not yet tested by us)

```bash
sudo apt install build-essential git pciutils qmake6 qt6-base-dev qt6-multimedia-dev ffmpeg pipewire-pulse pipewire-bin pulseaudio-utils v4l2loopback-dkms
```

**Other distributions** need the same things under their own names: a C and
C++ compiler with `make`, Qt 6 base and multimedia (with `qmake6`), `ffmpeg`,
PipeWire with its PulseAudio replacement, `pactl`, `pw-loopback`, and the
`v4l2loopback` kernel module. Without Qt the driver is still installed and
only the app is skipped.

### 2. Switch the IOMMU on

Check whether it already is:

```bash
ls /sys/kernel/iommu_groups | wc -l
```

A number above 0 means it is on: go to step 3. If it prints 0:

1. In your PC's firmware (BIOS/UEFI) setup, enable **VT-d** (Intel) or
   **AMD-Vi / IOMMU / SVM** (AMD).
2. **AMD**: that is normally all. **Intel**: also add `intel_iommu=on` to the
   kernel command line. How depends on your bootloader:

   - **GRUB** (Ubuntu, Debian, Mint, most Arch installs): edit
     `/etc/default/grub`, add `intel_iommu=on` inside the quotes of
     `GRUB_CMDLINE_LINUX_DEFAULT`, then run `sudo update-grub`
     (Debian/Ubuntu) or `sudo grub-mkconfig -o /boot/grub/grub.cfg` (Arch).
   - **Fedora**: `sudo grubby --update-kernel=ALL --args="intel_iommu=on"`
   - **systemd-boot**: add it to `/etc/kernel/cmdline` and regenerate the
     entries with your distribution's tool, or append it to the `options`
     line of the entry in `/boot/loader/entries/`.
   - **Limine** (CachyOS): add it inside the quotes of `KERNEL_CMDLINE` in
     `/etc/default/limine`, then run `sudo limine-update`.

3. Reboot and run the check again.

Make a copy of the file before you edit it. This is the only step that
touches your boot configuration; the installer never does.

### 3. Get the code and install

```bash
git clone https://github.com/PoimeYT/hd60pro-linux.git
cd hd60pro-linux
./system/install.sh
```

Run the installer as your normal user, not with `sudo`. It asks for your
password for the system parts, builds the driver and the app, and tells you
at the end whether anything is still missing.

Then **log out and back in** once (or reboot), so that the memory limit the
driver needs applies.

### 4. Use it

Connect your console to the card's **HDMI IN** and, if you want passthrough,
your TV or monitor to **HDMI OUT**.

- Open **HD60 Pro** from your application menu for the preview, recording and
  listening. Recordings go to `~/Videos/HD60 Pro/`.
- In **OBS**, add *Video Capture Device (V4L2)* → *HD60 Pro* and
  *Audio Input Capture* → *HD60 Pro Audio*. Streaming is OBS's job; the app
  does not stream.
- Passthrough, the camera and the audio input work whether or not the app is
  open.

## If something does not work

Start with the driver's own check. It only reads system information and never
touches the card:

```bash
hd60pro check
```

It names the first thing that is wrong and what to do about it.

| What you see | What it means |
| --- | --- |
| "The IOMMU is off" | Step 2 is not done, or you have not rebooted since. |
| "shares IOMMU group … with devices the system is using" | The slot the card is in is not isolated. Move the card to another PCIe slot (one wired directly to the CPU usually works). Do **not** follow VFIO guides that tell you to bind the other devices or to patch the kernel (ACS override). |
| "not bound to vfio-pci" | The boot step did not run: `systemctl status hd60pro-bind.service` |
| The app says "The driver is not running" | Press *Start the driver* in the app; it shows the reason if it cannot. |
| The app says "No signal" | The source is off, asleep, or plugged into HDMI OUT instead of HDMI IN. |
| No "HD60 Pro" camera in OBS | `v4l2loopback` is missing or was already loaded with other settings: `ls /dev/video60`, then reboot. |
| Picture but a slow consumer sees torn frames | The memory limit has not applied yet: log out and back in. |
| The card is passed through to a virtual machine | Only one of them can use it. Shut the VM down first. |

Driver messages: `journalctl --user -u hd60pro -b`. For bug reports see
[CONTRIBUTING.md](CONTRIBUTING.md).

## Uninstall

```bash
./system/uninstall.sh
```

This removes everything the installer added. If you added `intel_iommu=on`
yourself in step 2 you can leave it; it does no harm.

## Is this safe for my system?

The driver is an ordinary program, not a kernel module, so a bug in it cannot
crash the kernel. It is given the card through the kernel's VFIO interface,
and the IOMMU limits the card to the memory the driver hands it.

- It only ever touches the HD60 Pro. The card is found by its hardware ID,
  and both the boot step and the driver refuse any other device.
- It refuses to run when the IOMMU is off, and when the card shares an IOMMU
  group with a device your system is using. It never unbinds or rebinds
  another device.
- The installer adds a handful of small files under `/usr/local` and `/etc`
  (listed at the top of [system/install.sh](system/install.sh)). It does not
  touch the bootloader, the initramfs or udev rules.
- It never sends the commands that rewrite the card's firmware, so it cannot
  brick the card.

More detail is in [SECURITY.md](SECURITY.md).

## Trying it without installing

The driver can be built and run by hand. Nothing here is permanent; a reboot
undoes all of it. Install the dependencies and switch the IOMMU on first
(steps 1 and 2 above).

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

6. **Build and run** (as your normal user):

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

What the driver does to protect the rest of the system is described under
[Is this safe for my system?](#is-this-safe-for-my-system) and in
[SECURITY.md](SECURITY.md).

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
