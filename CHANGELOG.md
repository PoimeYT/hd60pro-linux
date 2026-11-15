# Changelog

## Unreleased

First public version of the userspace driver.

- Card bring-up over VFIO: mailbox, firmware handshake (version 1.11), I2C
  and GPIO tunnelling.
- MST3367 HDMI receiver: reset, initialisation, format detection from
  Elgato's mode table, colour-space conversion.
- Raw 1080p60 capture into v4l2loopback, measured at 60.00 fps with no
  dropped frames; delivery follows the card's buffer index registers.
- 48 kHz stereo audio exported as the PipeWire source "HD60 Pro Audio".
- IT6612 HDMI OUT passthrough with EDID, infoframes, audio and HDCP
  (HDCP untested); wakes displays that drop hot-plug in standby.
- Signal detection and HDMI OUT run on a separate thread from frame delivery.
- Four-buffer DMA ring, with a single-buffer fallback when the locked-memory
  limit is low.
- Protocol documentation in docs/PROTOCOL.md.

### Safety and bug fixes

- The card is found by PCI ID instead of assuming `0000:05:00.0`; every
  command verifies the device is an HD60 Pro (sysfs and config space) before
  touching it, and unknown commands no longer open the card at all.
- New `check` command reports the IOMMU, IOMMU-group, binding and access state
  without touching the card. The "group not viable" message no longer tells
  you to bind the other devices in the group to `vfio-pci`.
- `run` and `capture` always stop the card, unload the PipeWire module and
  free the DMA buffers on SIGINT/SIGTERM/SIGHUP/SIGQUIT and on every error
  path; SIGPIPE no longer kills the driver; an error exit of `run` no longer
  hangs waiting for the monitor thread.
- Leftover `module-pipe-source` instances from a killed run are removed at
  start. The audio FIFO moved from `/tmp` to `$XDG_RUNTIME_DIR` and is checked
  to be a FIFO.
- `run` defaults to `/dev/video60` and refuses devices that are not
  v4l2loopback outputs; a failed format set no longer double-closes the file
  descriptor.
- Modes larger than the card's video buffer (2048x1536, 2048x2048) are no
  longer captured (they read past the buffer); HDMI OUT still works for them.
- I2C/GPIO results are read under the mailbox lock (the monitor thread could
  read another command's result).
- `capture` no longer crashes when an output file cannot be created.
- Bounds checks in the EDID data-block parser and on GPIO pin numbers.

### Install

- `system/install.sh` / `system/uninstall.sh`: boot-time vfio-pci binding
  (`hd60pro-bind`, HD60 Pro only, refuses shared IOMMU groups), v4l2loopback
  camera at `/dev/video60`, locked-memory limit, and a systemd user service
  that starts the driver at login.

### App

- Qt 6 companion app (`app/`): live preview, one-button recording through
  ffmpeg with presets, headphone listening through `pw-loopback` with a
  volume slider, system tray, single instance, settings dialog.
- The driver writes its state to `$XDG_RUNTIME_DIR/hd60pro.status` for the app.
