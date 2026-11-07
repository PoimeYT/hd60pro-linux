# Compatibility

What has been tested, what should work, and what will not. If you try a
combination that is not listed, please
[report it](CONTRIBUTING.md#reporting-hardware-results), whether it works or
not.

Legend: ✅ tested and working · 🟡 expected to work, untested ·
🧪 experimental · ❌ not supported

## Identify your card

```bash
lspci -nn -d 12ab: -vv | grep -E "12ab|Subsystem"
```

The device ID (`0380` or `0381`) and the subsystem ID after `1cfa:` identify
the revision. Elgato's Windows driver lists these:

| PCI ID | Subsystem | Elgato's name | Status |
| --- | --- | --- | --- |
| `12ab:0380` | `1cfa:0006` | HD60 Pro (Rev. 1 + Ryzen fix) | ✅ |
| `12ab:0380` | `1cfa:0003` | HD60 Pro (Rev. 1) | 🟡 same code path in Elgato's driver as `0006` |
| `12ab:0380` | `1cfa:0005` | HD60 Pro (Rev. 2, "never released") | 🟡 same code path as `0006` |
| `12ab:0381` | `1cfa:0010` | HD60 Pro (Rev. 3) | 🧪 different firmware (11.11 instead of 1.11); bring-up not yet adapted |
| `12ab:0380` | `05cf:12ab` | Prototype | ❌ |

Other Elgato cards (4K60 Pro, 4K60 Pro MK.2, 4K60 S+, HD60 S, Cam Link) use
different chips and are not supported.

## Card firmware

The card runs software from its own flash. This driver only talks to it; it
never updates it.

| Card | Firmware the driver expects | Tested |
| --- | --- | --- |
| `12ab:0380` | 1.11 | ✅ 1.11 |
| `12ab:0381` | 11.11 | 🧪 |

If your card reports something else (`hd60pro probe` prints it), please
report it.

## Input sources and modes

The HDMI receiver recognises every mode in Elgato's mode table (480i up to
1920×1200 and 2560×1080, see `userspace/mst3367.c`). Capture is configured
from the detected mode.

| Source | Mode | Status |
| --- | --- | --- |
| Nintendo Switch 2 (dock) | 1920×1080p60, RGB, 48 kHz | ✅ |
| Any source | 1080p50/30/25/24, 720p60/50 | 🟡 |
| Any source | 1080i, 576i, 480i (interlaced) | 🧪 the interlaced path is ported but untested |
| Any source | 1440p, 4K, or above 60 Hz | ❌ beyond the card's HDMI receiver |
| HDCP-encrypted source | any | 🧪 capture of protected content is not expected to work; passthrough HDCP is ported but untested |

The card cannot pass through or capture 1440p, 4K or 120 Hz: its HDMI chips
are HDMI 1.x parts whose fastest modes are around 1080p60 / 1920×1200@60.

## HDMI OUT (passthrough)

| Display | Status |
| --- | --- |
| Samsung monitor (EDID manufacturer `SAM`) | ✅ picture and sound |

Some displays drop their hot-plug signal in standby and only wake when they
receive video. The driver detects a display over DDC when hot-plug is low and
turns the output on anyway, which wakes them.

## Host systems

| Item | Tested |
| --- | --- |
| Distribution | Fedora 44 |
| Kernel | 7.2.4 |
| CPU / IOMMU | Intel, `intel_iommu=on iommu=pt` |
| v4l2loopback | 0.15.4 |
| PipeWire | 1.6.8 |
| Architecture | x86-64 only |

Expected to work on any distribution with VFIO, v4l2loopback and PipeWire.
AMD systems usually have the IOMMU enabled by default.

## Known limitations

- The card's H.264 encoder output does not work yet; capture uses the raw
  uncompressed preview stream instead (full quality, encode on the PC).
- With the default 8 MB locked-memory limit the driver uses one shared
  video buffer instead of four; it still runs at full frame rate, but a slow
  consumer can see a partly updated frame.
- The card can only be used by one thing at a time: this driver or a VM it is
  passed through to.
