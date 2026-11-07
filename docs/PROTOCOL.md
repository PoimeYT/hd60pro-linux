# Elgato Game Capture HD60 Pro: host protocol

Reverse engineered from `e60MZ0380.X64.SYS` 1.1.0.191.4 (Windows, shipped inside
`DRVInstaller.exe` in `GameCaptureSetup_3.70.56.3056_x64.msi`). The matching
`MZ0380.X64.PDB` ships alongside it (GUID 8f63e9ab-5646-1c4f-bedf-570ecb8bb8c0),
so function names come from the PDB. Decompilation used REA + Ghidra 12.1.4;
the renamed pseudocode is in `re/analysis/named/` (generated, not committed).

Status legend: **observed** = read directly from the driver code;
**inferred** = interpretation; **unverified** = not yet tested on hardware.

## Hardware

| Item | Value |
| --- | --- |
| PCI ID | `12ab:0380` (YUAN High-Tech), subsystem `1cfa:0006` ("Rev. 1 + Ryzen fix") |
| Other IDs served by the same driver | subsys `0003`, `0005` on 0380; `0381` subsys `0010` (Rev. 3) |
| BAR0 | 1 MiB MMIO, mailbox and status (observed) |
| BAR5 | 4 KiB MMIO, the SoC's PCIe endpoint controller (observed) |
| IRQ | legacy INTx pin B (lspci) |
| SoC | YUAN MZ0380 = VATICS "Mozart" ARM SoC running Linux 2.6.28 (observed in firmware tarball) |
| HDMI receiver | MStar MST3367 at I2C 0x9C (8-bit address), banked registers (observed) |
| HDMI transmitter (passthrough) | ITE IT6612 at I2C 0x98, driven by the host driver (observed) |
| Copy-protect chip | I2C 0xA2/0xA3 (observed) |

The driver's per-device switches read a cached config struct: `+0x08` vendor,
`+0x0a` device, `+0x0c` subsystem ID, `+0x0e` subsystem vendor. For this card
`+0x0c = 0x0006` and `+0x0e/+0x0f = fa 1c`.

## Card firmware

The card boots its own Linux and application from on-board flash
(`/mnt/flash/yuan_demo_sdi`). `MZ0380.HD.HEX` / `.SD.HEX` are gzipped tarballs
of that application (`tinyvenc5/7/8` H.264 encoders, `video_capture_mgr`,
`audio_capture_mgr`, `ep.ko` PCIe endpoint driver, ...). The host only uploads
the tarball when the version the card reports differs from `MZ0380.FW.TXT`
(`01.11`); the card's `yuan_update_mgr` then writes it to flash.

**The Linux driver never sends the firmware or base-firmware commands
(0x0b/0x0c/0x0e/0x0f).** They rewrite flash; a version mismatch is reported,
not repaired.

## BAR0 mailbox (observed)

| Offset | Use |
| --- | --- |
| 0x00 | doorbell: write `0x800` to submit a command, `0x400` to acknowledge an IRQ |
| 0x04 | command id (PARAM 0) |
| 0x08... | parameters on submit; results on completion (0x08 = result 0) |
| 0x2c | command status: bit0 = done (polled commands); `0xaaaaaaaa` = firmware ready (cmd 0x0a) |
| 0x30 | IRQ status. bit11 = command done; bits 0-7, 8-15, 16-23 = data pipes, dispatched to DPCs |
| 0x40, 0x44, 0x48 | per-pipe buffer index / status words read in the packet handler |
| 0x50 | byte cleared after a surround-audio timestamp |
| 0x60... | firmware upload window |

Submitting (`MZ0380_SendVendorCommand`): clear 0x2c, write PARAM 1..n-1 at
0x08.., write the command id at 0x04 (via the param loop), write `0x800` to 0x00.
Completion is either polled (0x2c bit0, 50 x 1 ms) or signalled through the
ISR (0x30 bit11), depending on the command.

ISR (`CPCIBaseDevice::OnInterruptHandle`): read 0x30; if 0 or all-ones it is not
ours; else write BAR5+0xdc = 2, 0x30 = 0, doorbell = 0x400, then queue DPCs per byte.

## BAR5 (endpoint controller, observed)

| Offset | Use |
| --- | --- |
| 0x30 | inbound window start: written with (BAR0 bus address + 4) before HELLO |
| 0x38 | inbound window limit: (BAR0 bus address + 0x5f) |
| 0xdc | write 2 to clear the endpoint's interrupt |

## Commands

| Id | Name | Words | Notes |
| --- | --- | --- | --- |
| 0x01 | HELLO | `[1]` | retried up to 100x, 0.5 s each, until the card answers |
| 0x02 | SET_BUF_VIDEO | `[2, 0, size, hi0, lo0, hi1, lo1, hi2, lo2, hi3, lo3]` | pipe 0 buffers; size 0x466000 on this card |
| 0x03 | SET_BUF_AUDIO | `[3, ch, 0x2000, hi, lo x4]` | 8 channels, carved from one 4 MiB common buffer, 0x8000 per channel |
| 0x04 | SET_BUF_ENC0 | like 0x02 | pipe 1 buffers, size 0x34bd00 |
| 0x05 | SET_BUF_ENC1 | like 0x02 | pipe 2 buffers, size 0x34bd00 |
| 0x06 | START | `[6, 0xffffffff]` | last step of StartFirmware |
| 0x07 | STOP | `[7, 0xffffffff]` | StopFirmware |
| 0x08 | SET_BUF_VIDEO_EX | like 0x02 | pipe 0 buffers 4-7, when allocated |
| 0x0a | GET_VERSION | `[0x0a, 0, 0]` | 0x2c = 0xaaaaaaaa when ready; result0.result1 = version |
| 0x14/0x15/0x17 | GPIO get/set/direction | `[id, 1<<pin, val<<pin]` | get returns at 0x0c |
| 0x18/0x19 | SPI get/set | `[id, reg, val]` | get returns at 0x0c |
| 0x1a | I2C read | `[0x1a, addr, reg, 0]` | byte at 0x10 |
| 0x1b | I2C write | `[0x1b, addr, reg, val]` | |
| 0x1c/0x1d | crypto chip read/write | | copy-protect handshake only |
| 0x1e | I2C read N | `[0x1e, addr \| reg<<8 \| len<<16]` | up to 32 bytes at 0x0c.. |
| 0x1f | I2C write N | `[0x1f, addr \| reg<<8 \| len<<16, data...]` | |
| 0x29 | SET_VIDEO_IN | 10 words | input timing: size, fps, crop, colour, audio-in flags |
| 0x2a | SET_AUDIO_IN | 5 words | channels, 16 bit, rate, period |
| 0x2d | SET_ENCODER | 11 words | H.264 main (0) / sub (1): fps, GOP, profile, entropy, bitrate, QP, aspect, size, crop |
| 0x31 | SET_PREVIEW | 5 words | raw preview: fps, skip, deinterlace, OSD, mirror, flip |
| 0x62 | SET_OSD | | |

Banked registers (MST3367): the bank number is written to register 0 first;
the driver caches the last bank written.

## Bring-up (MZ0380_HwBackgroundInitialize, observed)

1. Wait 256 ms; ack IRQs (BAR5+0xdc=2, 0x30=0, doorbell=0x400).
2. Program BAR5 inbound window, send HELLO; repeat until answered.
3. Ack; GET_VERSION until 0x2c reads `0xaaaaaaaa`; compare with 1.11.
4. Register DMA buffers (cmds 2, [8], 3 x 8, 4, 5).
5. GPIO setup, I2C chip detection, logos, crossbar route (initialises
   MST3367), copy-protect handshake (host-side gate only).

Signal detection then runs in a polling thread (`MZ0380_OnControlPanelAnalysisThread`),
which for this card calls `MST3367_VideoFormatDetection` and `CIT6612::HDMITxTimer`
(passthrough). A format change triggers StopFirmware + StartFirmware.

## DMA buffers (CPCIBaseDevice::OnDeviceStart, observed)

Each pipe has up to 32 buffer slots; physical addresses are passed to the card
as (hi, lo) word pairs. For this card's branch every pipe buffer is 0x655000
bytes, and the common (audio) buffer is 0x400000 bytes. The sizes announced
to the card are smaller (0x466000 video, 0x34bd00 encoder).

## Verified on hardware (2026-11-07, userspace/hd60pro)

- BAR5+0x30/0x38 already held `81100004`/`8110005f` after Windows had run:
  the inbound-window formula is right.
- HELLO answers with CMD_STATUS = `dddddddd`; GET_VERSION with `aaaaaaaa`
  and version 1.11 (matches `MZ0380.FW.TXT`).
- The card converts 8-bit I2C addresses to 7-bit itself (0x9c -> 0x4e).
- MST3367 and IT6612 only answer after the GPIO9 reset pulse (1, 0, 1, 50 ms
  each) and GPIO8 = 0 from `MST3367_HwInitialize`. IT6612 then reads
  `54 49 12 16` ("IT", 0x1612); MST3367 returns a valid HDCP KSV.
- With nothing on HDMI IN, MST3367 bank0 0x55 = 0x03 (no sync), as expected.

## MST3367 (HDMI receiver)

Bank select = write the bank to register 0 (bank 0x80 is also used). Status:
bank0 0x55 bits 2-5 all set = locked. Measurements: active width bank2
0x29:0x28 (+1 if odd), vertical total bank0 0x5b:0x5c, H count 0x57:0x58
(1600000/n), V count 0x59:0x5a (1250000/n = 0.1 Hz units), interlace 0x5f
bit3, deep colour bank2 0x47. Modes come from a table keyed on (width, vtotal,
vfreq); see `userspace/mst3367.c`. Audio rate bank2 0x9b; AVI colour space
bank2 0x48 bits 5-6 when bank2 0x0b bit3 is set. CSC: 24 bytes of big-endian
coefficients at bank0 0x93 (RGB->YUV or YUV pass-through table).

## Data path (observed)

All capture IRQs arrive as IRQ_STATUS bit0 on this board; the packet handler
then checks every pipe:

| Pipe | DMA slots (driver struct) | Index register | Ready test | Payload |
| --- | --- | --- | --- | --- |
| raw video | +0x1190, cmd 0x02/0x08 | BAR0+0x40 & 3 (& 7 with 8 buffers) | buffer byte 4 bit0 | I420, stride ALIGN(width,16), Y then U then V |
| H.264 main | +0x1290, cmd 0x04 | BAR0+0x44 & 3 | header dword4 = length != 0 | Annex-B at buffer+0x1000; dword5 bit0 = keyframe |
| H.264 sub | +0x1390, cmd 0x05 | BAR0+0x48 & 3 | as above | as above |
| audio | common buffer, cmd 0x03 | BAR0+0x4c (2 bits per channel) | IRQ bits 16-25 | PCM 16-bit |

After consuming an H.264 frame the host zeroes header dwords 0 and 4. Bytes
BAR0+0x50.. are per-channel handshake flags (card sets 1, host writes 2/0).

## Card-side flow (ep.ko, video_capture_mgr, tinyvenc7)

- `ep.ko` copies each host command into `ep_command` and notifies a sysfs
  node. Simple commands (1, 0x0a, GPIO, I2C) are answered by ep.ko itself;
  0x29 is handed to `video_capture_mgr`, which launches `tinyvenc5` or
  `tinyvenc7` (selected by the `fw` byte of 0x29: 7 for >= 1080p50-class
  pixel rates) with the 0x29 fields as command-line options. Later commands
  (0x2d, 0x31, 0x06) go to tinyvenc through `epint_1080p`.
- Card apps report a finished frame by writing `channel_done`: ep.ko stores
  the buffer index in BAR0+0x40/0x44/0x48 (video) or 0x4c (audio), sets the
  IRQ bit and raises the interrupt, but only when `msi_enable` is 1. The host
  re-arms it with doorbell 0x400 (the card's clear-interrupt ISR).
- tinyvenc7's capture callback drops frames while the measured input width
  differs from the configured one, or while no-signal is set. With
  `bitstream_num` = 1 it then encodes and DMAs H.264 synchronously before
  notifying; if that DMA does not complete the whole loop stops after one
  frame. **With `bitstream_num` = 0 (0x29 word7 byte0) raw preview streams
  at 60 fps** (verified).
- cmd 0x29 fields as named by video_capture_mgr: word1 = ch, fps, fw,
  "interlace"(vi); word2 = width, height; word3 = m, flip, mirror; word4 =
  colour info x4; word5 = x/y start; word6 = input frame width/height;
  word7 = bitstream_num, osd_enabled, osd_size, is_nosg; word8 = vanc_lines,
  fast_kill, -, is_slave; word9 = nosg back colour, Y, U, V.

### Lost notifications (and the fix)

The card raises an interrupt only while `msi_enable` is 1; an event that
arrives while the previous interrupt is un-acked is parked in
`pending_irqs` and only written out with the *next* event. Video and audio
are reported by different card processes, so a video frame finishing at the
same moment as an audio period loses its own IRQ bit, and by the time the
pending bit is flushed BAR0+0x40 already names the following buffer. Driving
delivery from the IRQ bits dropped ~25% of frames (median frame interval
16.7 ms, but 33 ms gaps). The index registers are updated as soon as a
buffer is complete, so the port polls BAR0+0x40 (video) and BAR0+0x4c
(audio) and delivers every slot since the last one: 60.00 fps, no skips.

## Verified streaming (Switch 2, 1080p60)

`hd60pro run /dev/video0` feeds v4l2loopback at ~58-60 fps; ffmpeg reads
it as a normal camera. Luma range of a captured frame 18..227. Audio
interrupts arrive at 45.6/s (48 kHz stereo, 4096-byte periods) but the
buffers stayed silent during the test (menu without sound).

Audio verified with Mario Kart 8 Deluxe: 16-bit stereo 48 kHz, 4096-byte
periods in 0x2000-byte slots. IRQ bits 16..19 identify the completed slot
(several may be pending in one IRQ); BAR0+0x4c only holds the latest index.
`run` exports it as the PipeWire source `hd60pro` ("HD60 Pro Audio") through
`module-pipe-source`.

## IT6612 passthrough (ported, `userspace/it6612.c`)

Transliterated from the CIT6612 class: InitSys (chip ID `CA 11 16` or
`49 12 16`), HDMITX_SWReset, the HDMITxTimer state machine (0 no sink,
1 sink plugged -> EDID read over the transmitter's DDC master, 2 select
mode, 3/4 program video, 5 audio/HDCP/unmute, 6 running),
SetDataLatchTiming / SetYPbPrPLLChargePump / RxAdcSetCoast on the MST3367
output side, CSC (YUV 4:2:2 bus -> RGB), AVI/audio infoframes, N/CTS, and
HDCP 1.x authentication when the source is encrypted. Tables (CEA mode list,
embedded-sync timings, clocks, CSC matrices) are copied into
`it6612_tables.h`. The timer is called with mode 0x44 when the receiver has
no lock, otherwise with `CIT6612_GetTvMode(width, height, fps)` (1080p60 =
0x0e -> VIC 16). Runs on its own thread in `hd60pro run`.

Verified with a Samsung monitor: EDID read over the IT6612 DDC master, VIC 16
output, picture and sound on HDMI OUT. Two port additions beyond the
Windows logic, both in `it6612_timer`:

- The driver leaves the "no sink" state only on a hot-plug *change*
  interrupt; a display already asserting HPD at start never produced one.
  The port also checks the HPD level (SYS_STATUS 0x0e bit 6).
- The Samsung drops HPD in standby and only wakes on a TMDS signal, so once
  the output stopped (e.g. driver restart) it never came back. While HPD is
  low the port probes the EDID header over DDC every ~2 s and treats an
  answer as a connected display.

## Passthrough limits

Passthrough is MST3367 (receive) -> IT6612 (re-transmit), both programmed by
the host over the card's I2C bus; the card firmware does not control them.
The MST3367 mode table tops out around 2560x1080@60 / 1920x1200@60
(~185 MHz pixel clock); 1440p120 (~500 MHz) and 1080p120 (297 MHz) are
beyond these HDMI 1.x parts, so no firmware or driver change can enable them.

## Open questions

- Raw preview pixel format and stride (HwProcessAnalogRawVideoFrame).
- H.264 frame length and timestamp layout in the encoder buffers.
- Exact field packing of 0x29/0x2a/0x2d/0x31 for common modes.
- Whether INTx is required, or polling 0x30 is enough.
