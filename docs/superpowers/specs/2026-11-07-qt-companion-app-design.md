# HD60 Pro companion app (Qt) — design

Date: 2026-11-07
Status: draft for approval

## Why

The HD60 Pro now works on Linux through the userspace driver
(`userspace/hd60pro run`): raw 1080p60 into v4l2loopback `/dev/video0`, game
audio as the PipeWire source `hd60pro` ("HD60 Pro Audio"), and HDMI OUT
passthrough. Using it today means running the driver in a terminal and
knowing OBS or ffmpeg.

Two kinds of user:

- **The owner** plays on a monitor through HDMI OUT and mainly wants the
  Switch's sound in PC headphones, without thinking about it.
- **Other people** don't know OBS and want to open an app, see the game,
  press Record and get a normal video file.

Streaming stays OBS's job.

## Goals

1. Hear the game in headphones with one toggle and a volume slider.
2. Record to a file with one button, with presets and full customisation.
3. See a live preview of the game (the main window is built around it).
4. The driver runs as a background service, optionally started at login;
   passthrough, `/dev/video0` and the audio source work without the app.
5. A one-time "Set up" step replaces the manual `sudo` commands.
6. Installable by other people on their own PCs with their own HD60 Pro,
   without knowing anything about VFIO.

## Non-goals

- Streaming (Twitch, YouTube, ...): use OBS with `/dev/video0` and the
  `hd60pro` audio source.
- Using the card's own H.264 encoder (the raw path plus PC encoding is used).
- A kernel driver; other operating systems; other capture cards.
- Distro repositories / app stores: packages are built from this project;
  getting them into Fedora, AUR or PPAs is out of scope for now.

## Anti-goals (would make it a failure even if it "works")

- Passthrough or the audio source only working while the app is open.
- Noticeable audio delay in the headphones.
- A recording that silently fails or produces an empty/unplayable file.
- Needing the terminal for normal use.

## Constraints and facts

- Driver: `userspace/hd60pro` (C, VFIO). Needs access to `/dev/vfio/<group>`
  (group 16 here) and ~32 MB `RLIMIT_MEMLOCK` (falls back to a shared buffer
  with 8 MB). Exports video to `/dev/video0` (v4l2loopback,
  `exclusive_caps=1`, label "Virtual Camera") and audio via
  `pactl load-module module-pipe-source` (needs the user's PipeWire session,
  so the driver must run as the user, not root).
- The card is also passed to a libvirt VM `win10`; the driver and the VM
  cannot use it at the same time.
- After a reboot the card must be bound to `vfio-pci` (today this comes from
  a `driver_override` left by libvirt).
- System: Fedora 44, Qt 6.11 runtime installed (devel packages not yet),
  ffmpeg with `h264_nvenc`/`hevc_nvenc` (RTX 2060), `h264_vaapi`, `libx264`,
  PipeWire 1.6 with pulse compatibility.

## Running on other people's PCs

Decided by the owner: the app and driver are for other people's machines
too, not just this one. That brings these requirements:

- **Card detection**: find HD60 Pro cards by PCI ID (`12ab:0380`, `12ab:0381`
  with subsystem vendor `1cfa`) on any slot; never assume `0000:05:00.0` or
  IOMMU group 16. With more than one card, let the user pick.
- **Only the tested variant is "supported"**: subsystem `1cfa:0006` is tested.
  `0003`/`0005` (0380) and `0381:0010` (Rev. 3, firmware 11.11) are shown as
  experimental; the driver must not assume the 0380 firmware version or
  board branch for them without checking (see `docs/PROTOCOL.md`).
- **Setup must check and explain the system state** before changing anything:
  IOMMU enabled (Intel often needs `intel_iommu=on`; the setup offers to add
  it via the distro's bootloader tool and says a reboot is needed),
  the card's IOMMU group is viable (other devices in the group are listed
  if not), v4l2loopback available (install hint per distro), PipeWire with
  pulse compatibility present, no VM or other driver holding the card.
  Each check shows a plain-language result and the fix.
- **Packaging**: CMake project installing the driver, app, systemd user unit,
  setup script, desktop entry and icon; a Fedora RPM spec; install
  instructions for Arch and Debian/Ubuntu (dependencies differ:
  v4l2loopback is out-of-tree on some distros). Flatpak is excluded: the
  setup has to change host configuration (udev, vfio binding, limits).
- **Nothing from Elgato is shipped**: the driver needs no Elgato files at
  runtime; installers must not bundle the MSI, driver, firmware or logos.
- **Uninstall/undo**: the setup records what it changed and offers to undo it
  (remove udev rule, vfio binding, memlock drop-in, extra loopback device).

## Decided by the owner

- Qt app; driver can optionally start at login.
- Layout A: preview fills the window, a bar underneath with Record +
  timer, Listen toggle + volume, Settings; a status line with input format,
  audio rate, HDMI OUT state and driver state.
- Recording quality is customisable (presets plus custom).
- No streaming.

## Builder's calls (made in this conversation, owner may change)

- **C++17 with Qt 6** (Widgets + Multimedia), CMake, in `app/` next to
  `userspace/`.
- **Driver as a systemd user service** (`hd60pro.service`, `Restart=on-failure`),
  enabled/disabled by the "Start driver at login" setting. The app starts it
  if it is not running and never needs to stay open for it.
- **Status from the driver over a Unix socket** in `$XDG_RUNTIME_DIR`
  (`hd60pro.sock`): newline-delimited JSON events (signal mode, audio rate,
  HDMI OUT state, fps). The driver gains this; it already has the data.
- **Preview** reads `/dev/video0` with QtMultimedia (`QCamera` +
  `QVideoWidget`); v4l2loopback allows several readers, so OBS can use it at
  the same time.
- **Listening** uses a PipeWire loopback from `hd60pro` to the chosen output
  (`pw-loopback` with a low latency target), volume on that stream. Target
  delay under ~60 ms end to end.
- **Recording** spawns `ffmpeg` reading `/dev/video0` (v4l2) and the
  `hd60pro` source (pulse), so encoder support matches what ffmpeg has.
  Encoder auto-pick: NVENC, then VAAPI, then x264.
  - Presets: *Best quality* (~40 Mbps H.264, AAC 320k, MKV/MP4),
    *Balanced* (~15 Mbps), *Small for sharing* (~6 Mbps, 1080p, aimed at
    a few-minute clip fitting common upload limits), *Custom* (encoder,
    codec H.264/HEVC, bitrate or quality, resolution, fps, container,
    audio bitrate).
  - Files go to `~/Videos/HD60 Pro/` named `HD60 Pro YYYY-MM-DD HH-MM-SS.mp4`
    by default; folder configurable.
  - MP4 written with fragmented/fast-start options so a crash leaves a
    playable file; recording errors are shown to the user, never swallowed.
- **Close behaviour**: closing the window while Listen is on keeps the app in
  the system tray (Listen keeps working); otherwise it quits. Quit is in the
  tray menu.
- **Settings** (QSettings): recording preset/custom values and folder,
  listen output device and volume, start driver at login, preview on/off.
- **Set up** button runs one privileged script via `pkexec` that:
  binds the card to `vfio-pci` at boot, gives the user access to the card's
  VFIO group node (udev rule), and raises the memlock limit for the user's
  systemd manager. It reports what it changed and whether a re-login is
  needed.
- If the `win10` VM holds the card, the app says so instead of failing
  silently.
- **Dedicated camera device**: setup adds a second v4l2loopback device
  labelled "HD60 Pro" for the driver, so the existing "Virtual Camera" stays
  free for OBS's own virtual camera.
- **Listen at login**: a setting that autostarts the app into the tray with
  Listen on, so headphone audio needs no clicks after boot.

## Left to the builder

- Exact socket message format and reconnection logic.
- Exact ffmpeg arguments per encoder and preset.
- How the setup script identifies the card's IOMMU group and writes the
  udev/systemd configuration.
- Tray icon art, window sizing and styling (keep it clean and readable).
- How missing build dependencies (Qt devel packages) are documented.
