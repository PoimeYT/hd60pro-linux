# Contributing

Thanks for helping. The project was built against one card, one source and
one display, so the most valuable contributions right now are **reports from
other hardware**, followed by fixes and features.

## Reporting hardware results

Open an issue with the *Hardware report* template and include:

1. Card IDs: `lspci -nn -d 12ab: -vv | grep -E "12ab|Subsystem"`
2. Distribution, kernel (`uname -r`), CPU vendor, and whether the IOMMU is on
   (`ls /sys/kernel/iommu_groups | wc -l`).
3. Output of `./userspace/hd60pro probe` (firmware version).
4. Output of `./userspace/hd60pro -v detect 10` with your source connected.
5. What you tested (capture, audio, HDMI OUT), the source and its mode, the
   display on HDMI OUT, and what happened.

"It works" reports are just as useful as failures:
[COMPATIBILITY.md](COMPATIBILITY.md) is built from them.

## Bugs

Use the *Bug report* template. Run the failing command with `-v` (or `-vv`,
which logs every mailbox command) and attach the output.

## Development

```bash
make -C userspace
```

The driver is plain C (C11, GNU extensions) with no dependencies beyond
libc and pthreads.

- **Style**: kernel-like: tabs, `snake_case`, short functions, comments
  that explain *why*. Build with `-Wall -Wextra` and keep it warning-free.
- **Every source file** starts with
  `/* SPDX-License-Identifier: GPL-2.0-or-later */`.
- **Hardware behaviour must be evidenced.** When code reproduces something
  Elgato's driver does, say where it came from (the function name from the
  driver's symbols, e.g. `MST3367_HwInitialize`) in a comment, and record new
  protocol findings in [docs/PROTOCOL.md](docs/PROTOCOL.md) marked as
  observed, inferred or verified on hardware.
- **Port additions** (behaviour the Windows driver does not have, like the
  DDC presence probe for HDMI OUT) are marked as such in comments.

## Rules that protect users and the project

- **Never send the firmware or flash commands** `0x0b`, `0x0c`, `0x0e`,
  `0x0f`. They rewrite the card's flash and can brick it. Pull requests that
  send them will not be merged.
- **Never commit Elgato's files**: the installer, `.sys` drivers, `.pdb`
  symbols, `.HEX` firmware, logos, or decompiled output. `.gitignore` excludes
  the usual names; please check `git status` before committing. Small data
  tables needed for interoperability (like `userspace/it6612_tables.h`) are the
  exception, and each must say where it came from.
- **Don't add workarounds that weaken the IOMMU**, such as no-IOMMU VFIO mode.

## Reverse-engineering workflow

To research the Windows driver yourself, get Elgato's installer
(`GameCaptureSetup_*.msi`) from Elgato; this repository does not distribute
it.

1. Extract it (`7z x` the MSI, then `7z x DRVInstaller.exe`). The HD60 Pro
   driver is `e60MZ0380.X64.SYS`; the matching `MZ0380.X64.PDB` ships next
   to it.
2. `tools/pdbsyms.py MZ0380.X64.pdb e60MZ0380.X64.SYS > syms.json` recovers
   function and global names from the PDB.
3. Decompile with Ghidra (or similar) and rename functions from `syms.json`.
4. `tools/regdefaults.py` extracts registry setting names, defaults and
   device-extension offsets from an `objdump -d` listing of
   `CDevice::OnDeviceStart`.

Keep the extracted files outside the repository (the default `re/` folder is
ignored).

## Pull requests

- Keep each PR to one change, and describe how you tested it on hardware
  (card revision, source, mode).
- Update COMPATIBILITY.md when you test something new, and docs/PROTOCOL.md
  when you learn something new about the card.
- By contributing you agree your contribution is licensed under
  GPL-2.0-or-later.
