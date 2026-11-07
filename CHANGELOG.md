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
