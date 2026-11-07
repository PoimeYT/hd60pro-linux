# HD60 Pro companion app Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the HD60 Pro usable without a terminal on anyone's PC: a background driver service, a one-time privileged setup, and a Qt app for preview, headphone listening and one-button recording.

**Architecture:** The C driver (`userspace/hd60pro`) gains card auto-detection, a variant check, a dedicated loopback device and a JSON status socket, and runs as a systemd user service. A bash script (`system/hd60pro-setup`) checks and changes host configuration (vfio binding, device access, memlock, loopback device) and can undo it. The Qt 6 app (`app/`) only talks to the driver through the socket and `systemctl --user`, and uses external tools for media: QtMultimedia reads the loopback for preview, `pw-loopback` plays the audio source into headphones, `ffmpeg` records.

**Tech Stack:** C11 + pthreads (driver), bash (setup), C++17 + Qt 6 Widgets/Multimedia/Network/Test, CMake ≥ 3.21, systemd user units, polkit/pkexec, PipeWire (`pw-loopback`, `pw-dump`, `wpctl`, `pactl`), ffmpeg/ffprobe.

**Spec:** `docs/superpowers/specs/2026-11-07-qt-companion-app-design.md`

## Global Constraints

- Never send mailbox commands 0x0b, 0x0c, 0x0e or 0x0f (flash). No task adds a new mailbox command.
- Ship nothing from Elgato: no MSI, driver, firmware, PDB, logo, or decompiled output. The app icon is our own drawing.
- Card IDs: PCI `12ab:0380` or `12ab:0381` with subsystem vendor `1cfa`. Never hard-code `0000:05:00.0` or IOMMU group 16 outside tests.
- Only subsystem `1cfa:0006` is "supported"; `0003`/`0005` are "likely" (same code path); `0381:0010` (firmware 11.11) is "experimental".
- The driver runs as the logged-in user (it needs the user's PipeWire session), never as root.
- Audio source name `hd60pro`, description "HD60 Pro Audio". Dedicated video device label "HD60 Pro" at `/dev/video60` with `exclusive_caps=1`; the existing "Virtual Camera" stays untouched.
- Status socket `$XDG_RUNTIME_DIR/hd60pro.sock`, newline-delimited JSON.
- Recording files: `~/Videos/HD60 Pro/HD60 Pro YYYY-MM-DD HH-MM-SS.mp4` (`.mkv` when MKV is chosen); folder configurable.
- Presets: Best quality 40 Mbps H.264 + AAC 320k; Balanced 15 Mbps + AAC 192k; Small for sharing 6 Mbps + AAC 128k at 1080p; Custom (encoder, H.264/HEVC, bitrate or quality, resolution, fps, container, audio bitrate).
- Encoder auto-pick order: NVENC, then VAAPI, then x264.
- MP4 is written fragmented (`-movflags +frag_keyframe+empty_moov+default_base_moof`) so a crash leaves a playable file.
- Recording errors are always shown to the user, never swallowed.
- Headphone delay target: under ~60 ms end to end (`pw-loopback -l 20`).
- No streaming features; no Flatpak.
- Build prerequisite (the user installs it, not the plan): `sudo dnf install qt6-qtbase-devel qt6-qtmultimedia-devel` (Fedora), `qt6-base qt6-multimedia` (Arch), `qt6-base-dev qt6-multimedia-dev` (Debian/Ubuntu).
- All new files carry `SPDX-License-Identifier: GPL-2.0-or-later`.
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **The card is held by the `win10` VM, or libvirt reset `/dev/vfio/<group>` to root:root after the VM shut down.** Expected: the app says "In use by a virtual machine (win10)" or "No access to the card: Fix access", the service retries quietly every 5 s, and nothing crash-loops. Pinned by `test_open_error_codes` (Task 3), `test_check_card_free_vm` and `test_fix_access` (Task 4), `DriverClientTest::errorFileShown` (Task 5).
2. **The signal is lost or the mode changes in the middle of a recording.** Expected: the recording stops cleanly, the file stays playable, and the user sees "Recording stopped: input signal lost. Saved to …". Pinned by `RecorderTest::stopsOnSignalLoss` (Task 6).
3. **The output folder can't be written, or the disk fills up.** Expected: Record refuses to start and says why; a failure mid-recording reports ffmpeg's last error lines and keeps the partial file. Pinned by `RecorderTest::unwritableFolder` and `RecorderTest::ffmpegFailsMidway` (Task 6).
4. **The headphone device disappears while Listen is on.** Expected: audio carries on on the default output (WirePlumber moves the stream), and if `pw-loopback` exits it is restarted, up to 5 times a minute, with the reason shown. Pinned by `ListenerTest::restartsAfterExit` (Task 7).
5. **A second copy of the app is started (for example from the menu while the first sits in the tray).** Expected: the first window comes to the front and no second `pw-loopback` starts. Pinned by `SingleInstanceTest::secondInstanceActivatesFirst` (Task 8).

---

## File structure

```
userspace/
  card.c / card.h          card discovery in sysfs + variant table            (Task 1)
  v4l2find.c / v4l2find.h  find /dev/videoN by its label                       (Task 2)
  status.c / status.h      JSON status socket + error file                     (Task 3)
  main.c                   -d auto, list-cards, run defaults, status wiring     (Tasks 1-3)
  Makefile                 new objects, `make check`                            (Task 1)
  tests/check.h            tiny assertion macros + temp-sysfs helpers           (Task 1)
  tests/test_card.c, test_v4l2find.c, test_status.c
system/
  hd60pro.service.in       systemd user unit                                    (Task 4)
  hd60pro-setup            check / apply / fix-access / undo                    (Task 4)
  org.hd60pro.setup.policy polkit action for pkexec                             (Task 4)
  tests/test-setup.sh      runs the script against a fake root                  (Task 4)
app/
  CMakeLists.txt
  src/main.cpp
  src/settings.{h,cpp}        typed QSettings wrapper                           (Task 5)
  src/driverclient.{h,cpp}    socket client, service control                    (Task 5)
  src/encoders.{h,cpp}        encoder probe                                     (Task 6)
  src/recorder.{h,cpp}        ffmpeg args, process, validation                  (Task 6)
  src/listener.{h,cpp}        pw-loopback, volume, sink list                    (Task 7)
  src/singleinstance.{h,cpp}                                                    (Task 8)
  src/mainwindow.{h,cpp}      layout A, tray, status line                       (Task 8)
  src/settingsdialog.{h,cpp}  recording/listen/startup settings                 (Task 8)
  src/setupdialog.{h,cpp}     runs hd60pro-setup                                (Task 8)
  tests/tst_*.cpp             QtTest, one file per class
  resources/hd60pro.svg, hd60pro.desktop, hd60pro-autostart.desktop
CMakeLists.txt             top level: driver + app + install rules              (Task 9)
packaging/hd60pro-linux.spec                                                    (Task 9)
```

---

### Task 1: Driver: find the card and check its variant

**Files:**
- Create: `userspace/card.c`, `userspace/card.h`, `userspace/tests/check.h`, `userspace/tests/test_card.c`
- Modify: `userspace/main.c` (option parsing at `main()` start, new `list-cards` command, variant/firmware check after `mz_boot`), `userspace/Makefile`, `userspace/mz0380.c` (`mz_open` returns the negative errno from `vfio_open`)

**Interfaces:**
- Produces:
  ```c
  enum card_support { CARD_SUPPORTED, CARD_LIKELY, CARD_EXPERIMENTAL };
  struct card_variant { uint16_t device, subsys; const char *name; uint8_t fw_major, fw_minor; enum card_support support; };
  struct card_info { char bdf[16]; uint16_t device, subsys_vendor, subsys_device;
                     int iommu_group; /* -1 if none */ char driver[32]; /* "" if unbound */
                     const struct card_variant *variant; /* NULL if unknown subsystem */ };
  const char *card_sysfs_root(void);          /* $HD60_SYSFS_ROOT or "/sys" */
  int card_find(const char *sysfs_root, struct card_info *out, int max); /* count, sorted by bdf */
  const struct card_variant *card_variant(uint16_t device, uint16_t subsys);
  const char *card_support_name(enum card_support s); /* "supported" | "likely" | "experimental" */
  ```
- Driver CLI: `-d auto` (the default) picks the first card `card_find` returns; `-x` allows experimental variants and firmware mismatches; `hd60pro list-cards [--json]` runs before `mz_open`.
- Exit code 3 = "won't fix itself by retrying" (used by the service in Task 4).

Variant table (copy exactly):

| device | subsys | name | fw | support |
| --- | --- | --- | --- | --- |
| 0x0380 | 0x0006 | HD60 Pro (Rev. 1 + Ryzen fix) | 1.11 | SUPPORTED |
| 0x0380 | 0x0003 | HD60 Pro (Rev. 1) | 1.11 | LIKELY |
| 0x0380 | 0x0005 | HD60 Pro (Rev. 2) | 1.11 | LIKELY |
| 0x0381 | 0x0010 | HD60 Pro (Rev. 3) | 11.11 | EXPERIMENTAL |

- [ ] **Step 1: Write the failing test**

`tests/check.h` provides `CHECK(cond)`, `CHECK_STR(a, b)` (prints file:line on failure, counts failures, `return failures != 0` from `main`), plus `char *tmp_root(void)` (mkdtemp under `$TMPDIR`), `void put(const char *root, const char *rel, const char *content)` (mkdir -p + write) and `void link_to(const char *root, const char *rel, const char *target)`.

`tests/test_card.c`:
```c
static void fake_card(const char *root, const char *bdf, const char *dev, const char *sv, const char *sd,
                      const char *group, const char *driver); /* writes vendor 0x12ab, device, subsystem_*,
                      iommu_group -> ../../../kernel/iommu_groups/<group>, driver -> .../drivers/<driver> */

void test_finds_cards_sorted(void) {
	char *r = tmp_root();
	fake_card(r, "0000:0a:00.0", "0x0381", "0x1cfa", "0x0010", "21", NULL);
	fake_card(r, "0000:05:00.0", "0x0380", "0x1cfa", "0x0006", "16", "vfio-pci");
	put(r, "bus/pci/devices/0000:00:02.0/vendor", "0x8086\n");         /* not ours */
	fake_card(r, "0000:06:00.0", "0x0380", "0x05cf", "0x12ab", "17", NULL); /* prototype: wrong subsys vendor */
	struct card_info c[4];
	CHECK(card_find(r, c, 4) == 2);
	CHECK_STR(c[0].bdf, "0000:05:00.0");
	CHECK(c[0].iommu_group == 16);
	CHECK_STR(c[0].driver, "vfio-pci");
	CHECK(c[0].variant && c[0].variant->support == CARD_SUPPORTED);
	CHECK_STR(c[1].bdf, "0000:0a:00.0");
	CHECK_STR(c[1].driver, "");
	CHECK(c[1].variant->support == CARD_EXPERIMENTAL && c[1].variant->fw_major == 11);
}
void test_no_cards(void) { CHECK(card_find(tmp_root(), NULL, 0) == 0); }
void test_variant_lookup(void) {
	CHECK(card_variant(0x0380, 0x0003)->support == CARD_LIKELY);
	CHECK(card_variant(0x0380, 0x1234) == NULL);
	CHECK_STR(card_support_name(CARD_EXPERIMENTAL), "experimental");
}
```

- [ ] **Step 2: Add the `check` target and run it to verify it fails**

Makefile: `TESTS = tests/test_card` (later tasks append), each linked against the objects it needs; `check: $(TESTS)` runs each and fails on the first non-zero exit. Add `card.o` to `SRCS`.

Run: `make -C userspace check`
Expected: build failure, `card.h: No such file or directory`.

- [ ] **Step 3: Implement `card.c`**

Walk `<root>/bus/pci/devices/*`, read the hex attributes with `strtoul(..., 16)`, `readlink` `iommu_group` and `driver` and take the basename, `qsort` by `bdf`. `card_find` with `out == NULL` only counts.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `make -C userspace check`
Expected: `test_card: ok`, exit 0.

- [ ] **Step 5: Wire it into `main.c`**

- `-d` defaults to `"auto"`. On `auto`: 0 cards → print "No HD60 Pro found (looked for 12ab:0380/0381 with subsystem 1cfa:*)" and exit 3; several cards → use the first and print the others with "use -d BDF to pick".
- After `mz_boot`: look up the variant. `NULL` or `CARD_EXPERIMENTAL` without `-x` → print why, exit 3. `CARD_LIKELY` → print a one-line warning and continue. If `board_major.board_minor` differs from the variant's firmware → exit 3 unless `-x`.
- `list-cards` prints `BDF  VARIANT  SUPPORT  GROUP  DRIVER`, one per line; `--json` prints `[{"bdf":…,"device":"0380","subsystem":"0006","variant":…,"support":…,"iommu_group":16,"driver":"vfio-pci"}]` (`variant` is `null` for unknown subsystems).
- Update `usage()`.

- [ ] **Step 6: Verify on the hardware**

Run: `./userspace/hd60pro list-cards && ./userspace/hd60pro probe`
Expected: one line `0000:05:00.0  HD60 Pro (Rev. 1 + Ryzen fix)  supported  16  vfio-pci`, then `card firmware version 1.11` with no `-d`.

- [ ] **Step 7: Commit**

```bash
git add userspace/card.c userspace/card.h userspace/tests userspace/main.c userspace/mz0380.c userspace/Makefile
git commit -m "driver: find HD60 Pro cards by PCI ID and check the variant"
```

---

### Task 2: Driver: use the dedicated "HD60 Pro" video device

**Files:**
- Create: `userspace/v4l2find.c`, `userspace/v4l2find.h`, `userspace/tests/test_v4l2find.c`
- Modify: `userspace/main.c` (`run` default device), `userspace/Makefile`

**Interfaces:**
- Consumes: `card_sysfs_root()` (Task 1), `tests/check.h`.
- Produces: `int v4l2_find_by_name(const char *sysfs_root, const char *name, char *path, size_t len);` returns 0 and writes `/dev/videoN` for the lowest N whose `class/video4linux/videoN/name` equals `name` once the trailing newline is stripped; -1 if none.
- `run` with no device argument uses `v4l2_find_by_name(card_sysfs_root(), "HD60 Pro", …)`. If there is none it prints "No \"HD60 Pro\" video device: run the setup (hd60pro-setup apply) or pass a device" and exits 3. An explicit device argument still works, as it does today.

- [ ] **Step 1: Write the failing test**

```c
void test_finds_by_label(void) {
	char *r = tmp_root(); char p[64];
	put(r, "class/video4linux/video0/name", "Virtual Camera\n");
	put(r, "class/video4linux/video60/name", "HD60 Pro\n");
	put(r, "class/video4linux/video7/name", "HD60 Pro Something\n");
	CHECK(v4l2_find_by_name(r, "HD60 Pro", p, sizeof p) == 0);
	CHECK_STR(p, "/dev/video60");
	CHECK(v4l2_find_by_name(r, "Nope", p, sizeof p) == -1);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `make -C userspace check`
Expected: build failure, `v4l2find.h: No such file or directory`.

- [ ] **Step 3: Implement `v4l2find.c` and the `run` default**

- [ ] **Step 4: Run the tests to verify they pass**

Run: `make -C userspace check`
Expected: all tests ok.

- [ ] **Step 5: Commit**

```bash
git add userspace/v4l2find.c userspace/v4l2find.h userspace/tests/test_v4l2find.c userspace/main.c userspace/Makefile
git commit -m "driver: default to the video device labelled HD60 Pro"
```

---

### Task 3: Driver: status socket and error reporting

**Files:**
- Create: `userspace/status.c`, `userspace/status.h`, `userspace/tests/test_status.c`
- Modify: `userspace/main.c` (`struct monitor` gains `const char *tx_state; bool tx_on;` set under `lock`; the `run` loop publishes; open errors are mapped), `userspace/it6612.c/.h` (`bool it6612_output_on(const struct it6612 *t)`, true in state 6), `userspace/Makefile`

**Interfaces:**
- Consumes: `struct video_mode` (`mst3367.h`), `struct card_info` (Task 1).
- Produces:
  ```c
  struct status_state {
  	const struct card_info *card; unsigned fw_major, fw_minor;
  	char video_device[32]; bool ring;          /* false = shared-buffer fallback (low memlock) */
  	bool signal; struct video_mode mode;      /* mode valid when signal */
  	const char *hdmi_out; bool hdmi_out_on; double fps;
  };
  int  status_format(char *buf, size_t n, const struct status_state *st);           /* one line, ends in \n */
  int  status_format_error(char *buf, size_t n, const char *code, const char *message);
  struct status_srv { int listen_fd; int clients[8]; int nclients; char last[1024]; char path[108]; };
  int  status_open(struct status_srv *s, const char *path); /* unlinks a stale socket; non-blocking; mode 0600 */
  void status_poll(struct status_srv *s);                     /* accepts clients, sends them `last` */
  void status_publish(struct status_srv *s, const char *line);/* stores in `last`, sends to all; drops dead clients */
  void status_close(struct status_srv *s);
  int  status_default_path(char *buf, size_t n);              /* $XDG_RUNTIME_DIR/hd60pro.sock; -1 if unset */
  void status_write_error_file(const char *code, const char *message); /* $XDG_RUNTIME_DIR/hd60pro.error */
  void status_clear_error_file(void);
  const char *status_open_error_code(int neg_errno, int *exit_code); /* maps mz_open's result, table below */
  ```
- Wire format, consumed by Task 5 (keys exactly as written; `signal` is `null` without input):
  ```json
  {"event":"state","card":"0000:05:00.0","variant":"HD60 Pro (Rev. 1 + Ryzen fix)","support":"supported","firmware":"1.11","video_device":"/dev/video60","audio_source":"hd60pro","buffers":"ring","signal":{"width":1920,"height":1080,"fps":60,"interlaced":false,"audio_rate":48000},"hdmi_out":"passthrough on","hdmi_out_on":true,"fps":60.0}
  {"event":"error","code":"card_busy","message":"…"}
  ```
- Error codes and exit codes:

| code | when | exit |
| --- | --- | --- |
| `no_card` | `card_find` found none | 3 |
| `not_bound` | `mz_open` → `-ENOENT` (card not on vfio-pci) | 3 |
| `no_access` | `-EACCES` / `-EPERM` | 3 |
| `card_busy` | `-EBUSY` (VM or another process has the group) | 1 |
| `unsupported_variant` | the Task 1 variant/firmware refusal | 3 |
| `no_video_device` | the Task 2 failure | 3 |
| `buffer_setup` | `stream_alloc`/`register` failed | 1 |

Each failure calls `status_write_error_file` and prints the message. A successful start of `run` calls `status_clear_error_file`. The `run` loop calls `status_poll` every iteration and `status_publish` when the detection `seq` changes, when `tx_state` changes, and every 5 s along with the fps report. `ring` is `s.nvideo == 4`.

- [ ] **Step 1: Write the failing tests**

```c
void test_format_state(void) {
	struct card_info c = { .bdf = "0000:05:00.0", .variant = card_variant(0x0380, 0x0006) };
	struct status_state st = { .card = &c, .fw_major = 1, .fw_minor = 11, .video_device = "/dev/video60", .ring = true,
		.signal = true, .mode = { .width = 1920, .height = 1080, .fps = 60, .audio_rate = 48000 },
		.hdmi_out = "passthrough on", .hdmi_out_on = true, .fps = 59.94 };
	char b[1024]; status_format(b, sizeof b, &st);
	CHECK(strstr(b, "\"event\":\"state\""));
	CHECK(strstr(b, "\"signal\":{\"width\":1920,\"height\":1080,\"fps\":60,\"interlaced\":false,\"audio_rate\":48000}"));
	CHECK(strstr(b, "\"fps\":59.94"));
	CHECK(b[strlen(b) - 1] == '\n');
	st.signal = false; status_format(b, sizeof b, &st);
	CHECK(strstr(b, "\"signal\":null"));
}
void test_error_is_escaped(void) {
	char b[256]; status_format_error(b, sizeof b, "card_busy", "used by \"qemu\"\\x");
	CHECK_STR(b, "{\"event\":\"error\",\"code\":\"card_busy\",\"message\":\"used by \\\"qemu\\\"\\\\x\"}\n");
}
void test_socket_roundtrip(void) {
	/* status_open on <tmp>/s.sock; publish "A\n"; connect a client; status_poll; client reads "A\n";
	   publish "B\n"; client reads "B\n"; close the client; publish "C\n" must not SIGPIPE; nclients == 0 */
}
void test_stale_socket_replaced(void) { /* create a regular file at the path; status_open succeeds */ }
void test_error_file(void) {
	/* setenv XDG_RUNTIME_DIR=<tmp>; status_write_error_file("no_access","x") → file holds the error line;
	   status_clear_error_file → file gone */
}
void test_open_error_codes(void) {
	int ex;
	CHECK_STR(status_open_error_code(-EBUSY, &ex), "card_busy"); CHECK(ex == 1);
	CHECK_STR(status_open_error_code(-EACCES, &ex), "no_access"); CHECK(ex == 3);
	CHECK_STR(status_open_error_code(-EPERM, &ex), "no_access"); CHECK(ex == 3);
	CHECK_STR(status_open_error_code(-ENOENT, &ex), "not_bound"); CHECK(ex == 3);
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `make -C userspace check`
Expected: build failure, `status.h: No such file or directory`.

- [ ] **Step 3: Implement `status.c`** (send with `MSG_NOSIGNAL | MSG_DONTWAIT`; a client whose send fails is closed and removed) **and `int it6612_output_on`**

- [ ] **Step 4: Run the tests to verify they pass**

Run: `make -C userspace check`
Expected: all tests ok.

- [ ] **Step 5: Wire it into `main.c`** as described in Interfaces.

- [ ] **Step 6: Verify on the hardware**

Run (driver running in another terminal): `socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/hd60pro.sock | head -2`
Expected: two `state` lines with `"signal":{"width":1920,…}` and `"hdmi_out_on":true`. Then start the driver a second time while the first holds the card: expected exit 1, `card_busy` in `$XDG_RUNTIME_DIR/hd60pro.error`.

- [ ] **Step 7: Commit**

```bash
git add userspace/status.c userspace/status.h userspace/tests/test_status.c userspace/main.c userspace/it6612.c userspace/it6612.h userspace/Makefile
git commit -m "driver: publish status on a Unix socket and report start-up errors"
```

---

### Task 4: Service unit and `hd60pro-setup`

**Files:**
- Create: `system/hd60pro.service.in`, `system/hd60pro-setup`, `system/org.hd60pro.setup.policy`, `system/tests/test-setup.sh`
- Modify: `.github/workflows/build.yml` (run `make -C userspace check` and `system/tests/test-setup.sh`)

**Interfaces:**
- Consumes: the driver CLI (`-d`, `run`, `list-cards --json`) and the exit codes (Tasks 1-3).
- Produces for Task 8:
  - `hd60pro-setup check --json` (no root) prints one JSON object: `{"cards":[…list-cards objects…],"checks":[{"id":"…","ok":true|false,"message":"…","fix":"…"}]}`. Exit 0 if every check is ok, 1 otherwise.
  - `hd60pro-setup apply --user NAME [--card BDF] [--enable-iommu]` (root) prints human-readable lines, then exactly these three at the end: `CHANGED: <comma list or none>`, `RELOGIN: yes|no`, `REBOOT: yes|no`.
  - `hd60pro-setup fix-access --user NAME` (root) re-applies the group node's ownership (`udevadm trigger --action=add --subsystem-match=vfio`, then `chgrp`/`chmod` as a fallback).
  - `hd60pro-setup undo` (root) reverses everything recorded in the state file, then deletes it.
- Test hooks: `HD60_ROOT` prefixes every path (`/sys`, `/proc`, `/etc`, `/var/lib`, `/dev`). With `HD60_DRYRUN=1` every external command that would change the system prints `RUN: <command>` instead of running. `HD60_LIST_CARDS`, `HD60_PACTL` and `HD60_VIRSH` override those tools (default: `hd60pro list-cards --json`, `pactl`, `virsh -c qemu:///system`).

Unit `hd60pro.service.in` (`@BINDIR@` filled in by CMake in Task 9):
```ini
[Unit]
Description=Elgato HD60 Pro driver
After=pipewire-pulse.service
Wants=pipewire-pulse.service

[Service]
Environment=HD60_CARD=auto
EnvironmentFile=-%h/.config/hd60pro/driver.env
ExecStart=@BINDIR@/hd60pro -d ${HD60_CARD} run
Restart=on-failure
RestartSec=5
RestartPreventExitStatus=3

[Install]
WantedBy=default.target
```
No `LimitMEMLOCK=` in the user unit: raising it above the manager's hard limit makes the unit fail to start. The user manager's limit is raised by the drop-in below, and until the user logs in again the driver falls back to shared buffers (`"buffers":"shared"`).

Checks, in this order (`id`: passes when → `fix` text):
- `card`: at least one card → "No HD60 Pro found. Check it is seated and shows in `lspci -d 12ab:`".
- `iommu`: `/sys/kernel/iommu_groups` is not empty → Intel (`GenuineIntel` in `/proc/cpuinfo`): "IOMMU is off. Set up can add intel_iommu=on to the kernel command line (reboot needed)"; AMD: "Enable IOMMU/AMD-Vi in the BIOS".
- `group`: every other device in the card's group is a PCI bridge (class `0x0604xx`), unbound, or on `vfio-pci` → otherwise list each one (`BDF class driver`) and say "these must move to another slot, or need ACS".
- `vfio_binding`: the card's driver is `vfio-pci` and `/etc/udev/rules.d/70-hd60pro.rules` exists → "Set up will bind the card to vfio-pci".
- `access`: `/dev/vfio/<group>` is readable and writable by the calling user → "Set up will give you access" or, if the rule already exists, "Fix access".
- `memlock`: the soft "Max locked memory" in `/proc/self/limits` is ≥ 64 MiB or `unlimited` → drop-in missing: "Set up will raise it"; drop-in present: "Log out and back in".
- `loopback`: a `class/video4linux/*/name` reads `HD60 Pro` → no `v4l2loopback` module dir under `/lib/modules/$(uname -r)` and no `/sys/module/v4l2loopback`: per-distro hint (Fedora: "sudo dnf install v4l2loopback (RPM Fusion)"; Arch: "sudo pacman -S v4l2loopback-dkms"; Debian/Ubuntu: "sudo apt install v4l2loopback-dkms", picked from `/etc/os-release` `ID`/`ID_LIKE`); otherwise "Set up will create it".
- `pipewire`: `$HD60_PACTL info` succeeds and contains `PipeWire` → "Install PipeWire with PulseAudio support (pipewire-pulseaudio / pipewire-pulse)".
- `card_free`: no process other than `hd60pro` has `/dev/vfio/<group>` open (scan `/proc/[0-9]*/fd`, show `comm`) and no running libvirt domain's XML has a `<hostdev>` with the card's bus/slot/function → "In use by <name> (a virtual machine): shut it down first".

`apply` writes these, and records each in `/var/lib/hd60pro/setup-state` as one `kind<TAB>value` line:
1. `/etc/udev/rules.d/70-hd60pro.rules`:
   ```
   # hd60pro-linux: bind HD60 Pro cards to vfio-pci, let group hd60pro open them
   ACTION=="add", SUBSYSTEM=="pci", ATTR{vendor}=="0x12ab", ATTR{device}=="0x0380|0x0381", ATTR{subsystem_vendor}=="0x1cfa", ATTR{driver_override}="vfio-pci"
   SUBSYSTEM=="vfio", KERNEL=="<group>", GROUP="hd60pro", MODE="0660", TAG+="uaccess"
   ```
2. `/etc/modules-load.d/hd60pro.conf`: `vfio-pci` and `v4l2loopback`.
3. System group `hd60pro` (`groupadd -r`, only if missing) and `usermod -aG hd60pro NAME` (sets RELOGIN).
4. `/etc/systemd/system/user@.service.d/hd60pro-memlock.conf`: `[Service]` / `LimitMEMLOCK=64M` (sets RELOGIN).
5. `/etc/systemd/system/hd60pro-loopback.service`: oneshot, `After=systemd-modules-load.service`, `ExecCondition=/bin/sh -c '! grep -qxs "HD60 Pro" /sys/class/video4linux/*/name'`, `ExecStart=/usr/bin/v4l2loopback-ctl add -n "HD60 Pro" -x 1 /dev/video60`, `WantedBy=multi-user.target`; then `systemctl daemon-reload` and `systemctl enable --now hd60pro-loopback.service`.
6. Bind now, if the card is not on `vfio-pci` and `card_free` passed: `modprobe vfio-pci`, write `vfio-pci` to `driver_override`, unbind from its current driver if it has one, write the BDF to `/sys/bus/pci/drivers_probe`; then `udevadm control --reload` and `udevadm trigger --action=add --subsystem-match=vfio`.
7. With `--enable-iommu` on Intel only: `grubby --update-kernel=ALL --args="intel_iommu=on iommu=pt"` when `grubby` exists (sets REBOOT); otherwise print the GRUB/systemd-boot instructions and change nothing.
8. With `--card BDF`: write `HD60_CARD=BDF` to `~NAME/.config/hd60pro/driver.env`, owned by NAME.

`apply` run twice changes nothing the second time (`CHANGED: none`). `undo` removes the files, removes NAME from the group, removes the kernel args with `grubby --remove-args`, disables the loopback unit and deletes the `/dev/video60` loopback device. It does not unbind the card (a VM may want it on vfio-pci).

- [ ] **Step 1: Write the failing tests**

`system/tests/test-setup.sh`: plain bash with `assert_eq`, `assert_file_contains`, `assert_output_contains`. Each test builds a fake root in `mktemp -d` (fake sysfs card like Task 1, `/proc/cpuinfo`, `/proc/self/limits`, `/etc/os-release`) and stub scripts for `HD60_LIST_CARDS`, `HD60_PACTL` and `HD60_VIRSH`.
- `test_check_all_ok`: everything present → exit 0, every `"ok":true`.
- `test_check_iommu_off_intel`: empty `kernel/iommu_groups`, `GenuineIntel` → the `iommu` check fails and its fix mentions `intel_iommu=on`.
- `test_check_group_not_viable`: the group also has a GPU (class `0x030000`, driver `nvidia`) → the `group` check fails and its message names it.
- `test_check_loopback_hint_fedora`: no module, `ID=fedora` → the fix contains `dnf install v4l2loopback`.
- `test_check_card_free_vm`: the virsh stub lists `win10` with a hostdev at bus `0x05` slot `0x00` function `0x0` → the `card_free` message contains `win10`.
- `test_apply_writes_and_records`: `HD60_DRYRUN=1 apply --user alice` → the rule file contains `KERNEL=="16"`, the memlock drop-in exists, the state file lists every written file, the output has `RUN: usermod -aG hd60pro alice` and ends with `RELOGIN: yes`.
- `test_apply_idempotent`: a second apply prints `CHANGED: none`.
- `test_apply_enable_iommu`: `--enable-iommu` on Intel with a grubby stub → `RUN: grubby --update-kernel=ALL --args=intel_iommu=on iommu=pt` and `REBOOT: yes`.
- `test_fix_access`: prints `RUN: udevadm trigger --action=add --subsystem-match=vfio`.
- `test_undo_reverts`: apply then undo → none of the recorded files remain and the state file is gone.
- `test_refuses_without_root_outside_tests`: `apply` without `HD60_ROOT` while not root → exit 1, "run with pkexec or sudo".

- [ ] **Step 2: Run them to verify they fail**

Run: `bash system/tests/test-setup.sh`
Expected: FAIL, `system/hd60pro-setup: No such file or directory`.

- [ ] **Step 3: Implement `system/hd60pro-setup`** (`set -euo pipefail`, one function per check and per apply step, `jq` not required: JSON is built with a small `json_str` escape helper) **and the polkit policy** (action `org.hd60pro.setup`, `auth_admin_keep`, `org.freedesktop.policykit.exec.path` = `@LIBEXECDIR@/hd60pro-setup`, message "Set up the Elgato HD60 Pro (device access and drivers)").

- [ ] **Step 4: Run them to verify they pass**

Run: `bash system/tests/test-setup.sh`
Expected: every test prints `ok`, exit 0.

- [ ] **Step 5: Run the read-only check on this machine**

Run: `HD60_LIST_CARDS="./userspace/hd60pro list-cards --json" system/hd60pro-setup check`
Expected: plain-language results. Here `loopback` fails until setup runs, `vfio_binding` fails (no rule yet), and the rest pass. Do not run `apply` on the owner's machine in this task: Task 10 does that with the owner's go-ahead.

- [ ] **Step 6: Commit**

```bash
git add system .github/workflows/build.yml
git commit -m "Add the driver's systemd user unit and the hd60pro-setup script"
```

---

### Task 5: App skeleton, settings and driver client

**Files:**
- Create: `app/CMakeLists.txt`, `app/src/main.cpp`, `app/src/settings.{h,cpp}`, `app/src/driverclient.{h,cpp}`, `app/tests/tst_settings.cpp`, `app/tests/tst_driverclient.cpp`

**Interfaces:**
- Consumes: the wire format and error file from Task 3; the unit name `hd60pro.service` from Task 4.
- Produces:
  ```cpp
  struct DriverState {
      bool connected = false;
      QString card, variant, support, firmware, videoDevice, audioSource, buffers, hdmiOut;
      bool signal = false; int width = 0, height = 0, fps = 0, audioRate = 0; bool interlaced = false;
      bool hdmiOutOn = false; double measuredFps = 0;
  };
  struct DriverError { QString code, message; };
  std::optional<DriverState> parseStateLine(const QByteArray &line);   // nullopt unless event=="state"
  std::optional<DriverError> parseErrorLine(const QByteArray &line);   // nullopt unless event=="error"

  class DriverClient : public QObject {          // QLocalSocket to $XDG_RUNTIME_DIR/hd60pro.sock
      Q_OBJECT
  public:
      explicit DriverClient(QString socketPath = {}, QString errorFile = {}, QObject *parent = nullptr);
      DriverState state() const;
      void start();                                // connect; retry every 1000 ms while disconnected
      void startService();                         // systemctl --user start hd60pro.service
      void setStartAtLogin(bool on);               // systemctl --user enable|disable hd60pro.service
  signals:
      void stateChanged(const DriverState &s);
      void signalLost();                           // signal went true -> false, or the mode changed
      void driverError(const DriverError &e);      // from the socket, or from the error file while disconnected
      void connectionChanged(bool connected);
  };

  class Settings {                                 // QSettings("hd60pro", "hd60pro-app")
  public:
      enum class Preset { Best, Balanced, Small, Custom };
      Preset preset() const; void setPreset(Preset);
      QString outputFolder() const;                // default QStandardPaths::MoviesLocation + "/HD60 Pro"
      void setOutputFolder(const QString &);
      // Custom values: encoder ("auto","nvenc","vaapi","x264"), codec ("h264","hevc"),
      // rateMode ("bitrate","quality"), videoKbps, quality (int), width/height (0 = source),
      // fps (0 = source), container ("mp4","mkv"), audioKbps: each with a getter/setter
      QString listenSink() const;                  // "" = default output
      int listenVolume() const;                    // 0..150 percent, default 100
      bool listenEnabled() const; bool listenAtLogin() const;
      bool startDriverAtLogin() const; bool previewEnabled() const; // default true, true
      // + setters for each
  };
  ```
- Builds `hd60pro-app` and links Qt6::Widgets, Multimedia, MultimediaWidgets, Network. A static library `hd60pro_core` holds everything except `main.cpp`, so the tests link it.

- [ ] **Step 1: Write the failing tests**

`tst_settings.cpp` (sets `QStandardPaths::setTestModeEnabled(true)`):
- `defaults`: `preset()==Preset::Balanced`, `outputFolder()` ends with `/HD60 Pro`, `listenVolume()==100`, `container()=="mp4"`, `encoder()=="auto"`.
- `roundTrip`: set every value, construct a new `Settings`, read the same values back.

`tst_driverclient.cpp`:
- `parsesState`: the exact state line from Task 3 → `width==1920`, `audioRate==48000`, `hdmiOutOn`, `measuredFps==60.0`, `videoDevice=="/dev/video60"`.
- `parsesNoSignal`: `"signal":null` → `signal==false`.
- `ignoresGarbage`: `"not json"` and `{"event":"other"}` → nullopt from both parsers.
- `connectsAndReconnects`: a `QLocalServer` at a temp path sends a state line → `stateChanged` fires; the server closes → `connectionChanged(false)`; the server listens again → reconnects within 2 s.
- `signalLostEmitted`: a state with signal, then one without → `signalLost` once; a mode change from 1920×1080 to 1280×720 → `signalLost` once more.
- `errorFileShown`: no server, the error file holds a `card_busy` line → `driverError` with `code=="card_busy"` within 2 s.

- [ ] **Step 2: Configure and verify it fails**

Run: `cmake -S app -B build/app -DCMAKE_BUILD_TYPE=Debug && cmake --build build/app && ctest --test-dir build/app --output-on-failure`
Expected: the build fails on the missing `settings.h` / `driverclient.h`.

- [ ] **Step 3: Implement `settings.cpp`, `driverclient.cpp`** (parse with `QJsonDocument`; `startService`/`setStartAtLogin` use `QProcess::startDetached("systemctl", {"--user", …})`) **and a `main.cpp` that only creates a `QApplication`**

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/app && ctest --test-dir build/app --output-on-failure`
Expected: `100% tests passed`.

- [ ] **Step 5: Commit**

```bash
git add app
git commit -m "app: CMake skeleton, settings and driver status client"
```

---

### Task 6: Recording

**Files:**
- Create: `app/src/encoders.{h,cpp}`, `app/src/recorder.{h,cpp}`, `app/tests/tst_encoders.cpp`, `app/tests/tst_recorder.cpp`, `app/tests/fake-ffmpeg.sh`

**Interfaces:**
- Consumes: `Settings` and `DriverClient::signalLost` (Task 5).
- Produces:
  ```cpp
  enum class Encoder { Nvenc, Vaapi, X264 };
  struct EncoderSupport { bool nvenc = false, vaapi = false, x264 = false, x265 = false, hevcNvenc = false, hevcVaapi = false;
                          QString vaapiDevice; /* first /dev/dri/renderD* that worked */ };
  EncoderSupport probeEncoders(const QString &ffmpeg = "ffmpeg");   // slow: call off the GUI thread
  Encoder pickEncoder(const EncoderSupport &s, const QString &wanted /* "auto"|"nvenc"|"vaapi"|"x264" */);

  struct RecordingOptions {
      Encoder encoder = Encoder::X264; QString codec = "h264"; QString vaapiDevice;
      bool useQuality = false; int videoKbps = 15000; int quality = 23;
      int width = 0, height = 0, fps = 0;          // 0 = keep the source's
      QString container = "mp4"; int audioKbps = 192;
  };
  RecordingOptions optionsFor(const Settings &s, const EncoderSupport &e); // applies preset or custom
  QString defaultFileName(const QDateTime &t, const QString &container);  // "HD60 Pro 2026-11-07 21-05-09.mp4"
  QStringList buildFfmpegArgs(const RecordingOptions &o, const QString &videoDev,
                              const QString &audioSource, const QString &outFile);

  class Recorder : public QObject {
      Q_OBJECT
  public:
      explicit Recorder(QString ffmpeg = "ffmpeg", QString ffprobe = "ffprobe", QObject *parent = nullptr);
      bool start(const RecordingOptions &o, const QString &videoDev, const QString &audioSource,
                 const QString &folder);         // false + error() if it cannot start
      void stop();                               // writes "q" to stdin; kills after 5 s
      void stopForSignalLoss();                  // stop(), and finished() carries the signal-loss message
      bool isRecording() const; qint64 elapsedMs() const; QString currentFile() const;
  signals:
      void started(const QString &file);
      void finished(const QString &file, const QString &message); // message empty when all went well
      void error(const QString &message);        // user-facing; includes the last 5 ffmpeg stderr lines
  };
  ```

The argument order `buildFfmpegArgs` must produce. There is no `-nostdin`, because stop sends `q` on stdin:
`-hide_banner -loglevel warning [-vaapi_device DEV] -thread_queue_size 512 -f v4l2 -i <videoDev> -thread_queue_size 512 -f pulse -i <audioSource> -map 0:v -map 1:a <video codec args> [-r FPS] -c:a aac -b:a <A>k [mp4: -movflags +frag_keyframe+empty_moov+default_base_moof] -n <outFile>`

Video codec args per encoder (`<B>` = kbps, `<Q>` = quality, `<G>` = 2 × fps, or 120 for the source rate):

| encoder | bitrate mode | quality mode | scale (when width/height set) |
| --- | --- | --- | --- |
| Nvenc | `-c:v h264_nvenc\|hevc_nvenc -preset p5 -tune hq -rc cbr -b:v <B>k -maxrate <B>k -bufsize <2B>k -g <G>` | `-rc vbr -cq <Q> -b:v 0` in place of the cbr/b:v/maxrate/bufsize | `-vf scale=W:H` |
| Vaapi | `-vf format=nv12,hwupload -c:v h264_vaapi\|hevc_vaapi -b:v <B>k -maxrate <B>k -g <G>` | `-rc_mode CQP -qp <Q>` in place of b:v/maxrate | the filter becomes `format=nv12,hwupload,scale_vaapi=w=W:h=H` |
| X264 | `-c:v libx264\|libx265 -preset veryfast -b:v <B>k -maxrate <B>k -bufsize <2B>k -pix_fmt yuv420p -g <G>` | `-crf <Q>` in place of b:v/maxrate/bufsize | `-vf scale=W:H` |

Probe: for each encoder run `ffmpeg -hide_banner -loglevel error -f lavfi -i color=black:s=256x256:r=30 -frames:v 1 [VAAPI: -vaapi_device D -vf format=nv12,hwupload] -c:v <enc> -f null -` and count it as available on exit 0. NVENC shows up in `-encoders` even without an NVIDIA GPU, so only a test encode proves it works.

`start` checks, in order: the folder exists or `QDir::mkpath` succeeds, and a temp file can be created in it; if not → `error("Can't write to <folder>: <reason>. Choose another folder in Settings.")` and return false. Then it starts ffmpeg; a failure to start the process → `error("ffmpeg is not installed …")`.
On process exit: if `stop()` was not called → `error("Recording failed: <stderr tail>")`, and the partial file is kept and named in the message. After any exit, run `ffprobe -v error -show_entries format=duration -of csv=p=0 FILE`. A duration under 0.5 s or a failed probe → `error("The recording is empty or unplayable: <file>")` (the file is kept). Otherwise emit `finished(file, msg)`, where `msg` is `"Recording stopped: input signal lost. Saved to <file>"` after `stopForSignalLoss()` and empty otherwise.

- [ ] **Step 1: Write the failing tests**

`tst_encoders.cpp`:
- `pickOrder`: all available + "auto" → Nvenc; no nvenc → Vaapi; none → X264; "x264" wanted with nvenc available → X264; "nvenc" wanted but unavailable → falls back by the auto order.
- `fileName`: `defaultFileName(QDateTime(QDate(2026,11,7), QTime(21,5,9)), "mp4") == "HD60 Pro 2026-11-07 21-05-09.mp4"`.
- `presets`: `optionsFor` with Best → `videoKbps==40000 && audioKbps==320`, Balanced → `15000/192`, Small → `6000/128` with `width==1920 && height==1080`.

`tst_recorder.cpp`:
- `argsNvencMp4`: `buildFfmpegArgs` on Balanced/Nvenc contains the consecutive runs `{"-f","v4l2","-i","/dev/video60"}`, `{"-f","pulse","-i","hd60pro"}`, `{"-c:v","h264_nvenc"}`, `{"-b:v","15000k"}`, `{"-movflags","+frag_keyframe+empty_moov+default_base_moof"}` and ends with the output path.
- `argsVaapiScaleQuality`: Vaapi, quality 24, 1280×720 → contains `{"-vf","format=nv12,hwupload,scale_vaapi=w=1280:h=720"}`, `{"-rc_mode","CQP","-qp","24"}`, `-vaapi_device` comes before the first `-i`, and there is no `-b:v`.
- `argsX264HevcMkv`: X264 + hevc + mkv → `libx265`, no `-movflags`.
- `unwritableFolder`: folder = a path under a read-only temp dir → `start` returns false, `error` mentions the folder, and no process was started.
- `ffmpegFailsMidway`: `fake-ffmpeg.sh` writes 10 bytes to the output, prints `No space left on device` to stderr and exits 1 after 200 ms → `error` contains `No space left on device` and the file name.
- `stopsOnSignalLoss`: `fake-ffmpeg.sh` in "until q" mode and a fake ffprobe printing `3.2` → `stopForSignalLoss()` → `finished` with a message starting `Recording stopped: input signal lost`.
- `emptyFileReported`: the fake ffprobe exits 1 → `error` contains `empty or unplayable`.
- `recordsLive` (`QSKIP` unless `HD60_LIVE=1`): real ffmpeg, `pickEncoder(probeEncoders(), "auto")`, Balanced, `/dev/video60` + `hd60pro`, stop after 5 s → `finished` with an empty message, and ffprobe reports 4.5–5.5 s.

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build build/app && ctest --test-dir build/app -R "encoders|recorder" --output-on-failure`
Expected: the build fails on the missing headers.

- [ ] **Step 3: Implement `encoders.cpp` and `recorder.cpp`**

- [ ] **Step 4: Run them to verify they pass**

Run: `cmake --build build/app && ctest --test-dir build/app --output-on-failure`
Expected: all pass.

- [ ] **Step 5: Real ffmpeg smoke test (driver running)**

Run: `HD60_LIVE=1 ctest --test-dir build/app -R recorder --output-on-failure`
Expected: `recordsLive` passes, and the file in the temp folder is 1920×1080 h264_nvenc + aac.

- [ ] **Step 6: Commit**

```bash
git add app/src/encoders.* app/src/recorder.* app/tests
git commit -m "app: recording through ffmpeg with presets and encoder auto-pick"
```

---

### Task 7: Listening in headphones

**Files:**
- Create: `app/src/listener.{h,cpp}`, `app/tests/tst_listener.cpp`, `app/tests/fake-pw-loopback.sh`

**Interfaces:**
- Consumes: `Settings` (Task 5).
- Produces:
  ```cpp
  struct AudioSink { QString name, description; };
  QList<AudioSink> parseSinks(const QByteArray &pactlJson);              // `pactl -f json list sinks`
  int findNodeId(const QByteArray &pwDumpJson, const QString &nodeName); // -1 if absent
  QStringList loopbackArgs(const QString &sink /* "" = default */);

  class Listener : public QObject {
      Q_OBJECT
  public:
      explicit Listener(QString pwLoopback = "pw-loopback", QObject *parent = nullptr);
      void setEnabled(bool on); bool isEnabled() const;
      void setSink(const QString &name);       // restarts the loopback if it is running
      void setVolume(int percent);             // 0..150; `wpctl set-volume <id> <percent/100>`
      QList<AudioSink> sinks() const;          // runs pactl
  signals:
      void enabledChanged(bool on);
      void problem(const QString &message);    // e.g. "Listening stopped: pw-loopback keeps exiting"
  };
  ```
- `loopbackArgs("")` is exactly `{"-n","hd60pro-listen","-C","hd60pro","-l","20","-o","node.name=hd60pro-listen-out node.description=\"HD60 Pro Listen\" media.role=Game"}`. A non-empty sink adds `"-P", sink`.
- Volume: after the process starts, poll `pw-dump` every 200 ms (up to 3 s) for `node.name == "hd60pro-listen-out"`, then `wpctl set-volume <id> <v>`. Later volume changes reuse that id.
- Restart policy: if the process exits while enabled, restart it after 1 s. After 5 exits within 60 s, stop, set enabled to false and emit `problem` with the last stderr line.

- [ ] **Step 1: Write the failing tests**
- `argsDefaultAndSink`: as in Interfaces.
- `parsesSinks`: a 2-sink `pactl -f json` sample (fields `name`, `description`) → 2 entries with the right descriptions.
- `findsNodeId`: a `pw-dump` sample with `{"id":87,"type":"PipeWire:Interface:Node","info":{"props":{"node.name":"hd60pro-listen-out"}}}` among other objects → 87; a missing name → -1.
- `restartsAfterExit`: the fake loopback exits after 100 ms → it is started again (count its start-ups through a file the fake writes) and `problem` is not emitted after 2 start-ups.
- `givesUpAfterRepeatedExits`: the fake exits at once → after 5 start-ups `problem` is emitted and `isEnabled()==false` (the restart delay can be injected: `Listener::setRestartDelayForTest(int ms)`).

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build build/app && ctest --test-dir build/app -R listener --output-on-failure`
Expected: the build fails on the missing `listener.h`.

- [ ] **Step 3: Implement `listener.cpp`**

- [ ] **Step 4: Run them to verify they pass**

Run: `ctest --test-dir build/app --output-on-failure`
Expected: all pass.

- [ ] **Step 5: Ear test (driver running, game audio playing)**

Run: `pw-loopback -n hd60pro-listen -C hd60pro -l 20 -o 'node.name=hd60pro-listen-out media.role=Game'` (the same arguments as `loopbackArgs("")`), then `pw-top` in another terminal.
Expected: the game is audible in the headphones; `pw-top` shows `hd60pro-listen-out` with QUANT/latency around 20 ms; no audible delay against the TV.

- [ ] **Step 6: Commit**

```bash
git add app/src/listener.* app/tests
git commit -m "app: listen to the card in headphones through pw-loopback"
```

---

### Task 8: Main window, tray, settings and setup dialogs

**Files:**
- Create: `app/src/singleinstance.{h,cpp}`, `app/src/mainwindow.{h,cpp}`, `app/src/settingsdialog.{h,cpp}`, `app/src/setupdialog.{h,cpp}`, `app/resources/hd60pro.svg`, `app/resources/resources.qrc`, `app/tests/tst_singleinstance.cpp`, `app/tests/tst_setupdialog.cpp`
- Modify: `app/src/main.cpp`, `app/CMakeLists.txt`

**Interfaces:**
- Consumes: `DriverClient`, `DriverState`, `Settings` (Task 5); `Recorder`, `probeEncoders`, `optionsFor` (Task 6); `Listener` (Task 7); the `hd60pro-setup` CLI and its output lines (Task 4).
- Produces:
  ```cpp
  class SingleInstance : public QObject {   // QLocalServer "hd60pro-app-<uid>"
      Q_OBJECT
  public:
      explicit SingleInstance(QString key = {}, QObject *parent = nullptr);
      bool isPrimary();                       // false: told the primary to show itself; the caller exits
  signals:
      void activated();                       // in the primary, when another instance starts
  };
  struct SetupCheck { QString id; bool ok; QString message, fix; };
  struct SetupReport { QList<SetupCheck> checks; QJsonArray cards; };
  SetupReport parseCheckJson(const QByteArray &json);
  struct ApplyResult { QStringList changed; bool relogin = false, reboot = false; };
  ApplyResult parseApplyOutput(const QString &out);
  QString statusLine(const DriverState &s, bool connected); // see below
  ```
- `main.cpp`: `--tray` starts hidden in the tray with Listen on (used by the autostart entry). With a `SingleInstance` secondary, exit 0. The primary connects `DriverClient`, and starts the service if it isn't connected within 1.5 s.

**Layout A** (MainWindow):
- The preview fills the window: a `QVideoWidget` fed by a `QCamera` on the `QCameraDevice` whose `description()` is "HD60 Pro". It is re-picked on `QMediaDevices::videoInputsChanged` and on `DriverClient::stateChanged`. When the camera is missing or the preview is off, a centred label shows, depending on state: "Driver not running (Start)" / "No signal: is the console on?" / "Preview off".
- A bar underneath: **Record** button + `mm:ss` timer (red while recording) | **Listen** toggle + volume slider (0–150 %) | **Settings** button.
- A status line underneath built by `statusLine`, for example `1920×1080p60 · 48 kHz · HDMI OUT: passthrough on · Driver: running (60.0 fps)`. Without signal: `No signal · HDMI OUT: <state> · Driver: running`. Disconnected: `Driver: not running`. When `support != "supported"`, add ` · <variant> (experimental)`. When `buffers == "shared"`, add ` · Low memory-lock limit: log out and in after setup`.
- `DriverClient::driverError` shows a banner over the preview. `card_busy` → the app runs `hd60pro-setup check --json`. If the `card_free` check failed, the banner uses that check's message, which names the VM (for example "In use by win10 (a virtual machine): shut it down first"). Otherwise it says "The card is in use by another program. Close it; the driver will pick the card up by itself." `no_access` → the message plus a **Fix access** button (`pkexec hd60pro-setup fix-access --user $USER`). `not_bound`, `no_video_device`, `no_card` → a **Set up** button.
- `DriverClient::signalLost` while recording → `Recorder::stopForSignalLoss()`. `Recorder::error` / `finished` with a message → a `QMessageBox` (warning/information) with an **Open folder** button.
- Close: if Listen is on, hide to the tray (show a tray message once: "HD60 Pro keeps playing in the tray"); otherwise quit. The tray menu has Show, Listen (checkable), Record/Stop and Quit.

**SettingsDialog:** Recording (preset combo; Custom reveals encoder [only probed encoders enabled], codec, rate mode + bitrate/quality, resolution [Source/1080p/720p], fps [Source/60/30], container, audio bitrate; folder picker), Listening (output device from `Listener::sinks()` with "Default output" first), Startup ("Start driver at login" → `DriverClient::setStartAtLogin`; "Listen at login" → writes/removes `~/.config/autostart/hd60pro.desktop` from the template `resources/hd60pro-autostart.desktop`, which has `Exec=hd60pro-app --tray`; "Show preview"). A **Card** combo appears only when the last setup report lists more than one card; choosing one writes `~/.config/hd60pro/driver.env` and restarts the service. A **Set up…** button opens SetupDialog.

**SetupDialog:** runs `hd60pro-setup check --json` (from `HD60_SETUP_PATH` or the installed libexec path baked in by CMake as `HD60_SETUP_DEFAULT`) and lists each check with ✓/✗, the message and the fix. An "Also enable IOMMU (Intel, needs a reboot)" checkbox shows only when the `iommu` check failed. **Set up** runs `pkexec <setup> apply --user $USER [--card BDF] [--enable-iommu]` and shows its output live. At the end it shows "Done. Log out and back in to finish." / "Restart the computer to finish." from `parseApplyOutput`, then re-runs `check`. **Undo setup** runs `pkexec <setup> undo` after a confirmation. A pkexec exit of 126/127 (cancelled or not authorised) shows "Setup was cancelled" with no error.

- [ ] **Step 1: Write the failing tests**
- `tst_singleinstance.cpp` `secondInstanceActivatesFirst`: two `SingleInstance` objects with the same test key → the first `isPrimary()==true`, the second `false`, and the first emits `activated` within 1 s.
- `tst_setupdialog.cpp`:
  - `parsesCheckJson`: a sample with 2 checks and 1 card → the right ids, `ok` flags and fix texts.
  - `parsesApplyOutput`: `"…\nCHANGED: udev-rule, memlock\nRELOGIN: yes\nREBOOT: no\n"` → `changed=={"udev-rule","memlock"}`, `relogin`, `!reboot`.
  - `statusLines`: the four `statusLine` cases above give exactly those strings.

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build build/app && ctest --test-dir build/app --output-on-failure`
Expected: the build fails on the missing headers.

- [ ] **Step 3: Implement the four classes, the icon** (our own simple SVG: a rounded card outline with an HDMI port and a red record dot) **and `main.cpp`**

- [ ] **Step 4: Run the tests to verify they pass**

Run: `ctest --test-dir build/app --output-on-failure`
Expected: all pass.

- [ ] **Step 5: Launch and look**

Run: `build/app/hd60pro-app` with the driver running (`HD60_SETUP_PATH=$PWD/system/hd60pro-setup`).
Expected: the live game in the preview, a correct status line, Listen audible, Record making a playable file in `~/Videos/HD60 Pro/`, close-to-tray while Listen is on, and a second launch raising the window.

- [ ] **Step 6: Commit**

```bash
git add app
git commit -m "app: main window with preview, record, listen, tray and setup"
```

---

### Task 9: Install rules and packaging

**Files:**
- Create: `CMakeLists.txt` (top level), `app/resources/hd60pro.desktop`, `packaging/hd60pro-linux.spec`
- Modify: `app/CMakeLists.txt` (install targets), `README.md` (Install and App sections), `CHANGELOG.md`, `.github/workflows/build.yml` (add a CMake + Qt job on `ubuntu-latest` with `qt6-base-dev qt6-multimedia-dev`, running `ctest`)

**Interfaces:**
- Consumes: every earlier artifact.
- Produces the installed layout (GNUInstallDirs):

| file | destination |
| --- | --- |
| `hd60pro` (built from `userspace/*.c` by CMake as well; the Makefile stays for quick builds) | `${BINDIR}` |
| `hd60pro-app` | `${BINDIR}` |
| `system/hd60pro-setup` | `${LIBEXECDIR}/hd60pro-setup` |
| `hd60pro.service` (configured from `.in` with `@BINDIR@`) | `${LIBDIR}/systemd/user/` (`${prefix}/lib/systemd/user` when the prefix is `/usr`) |
| `org.hd60pro.setup.policy` (configured, `@LIBEXECDIR@`) | `${DATADIR}/polkit-1/actions/` |
| `hd60pro.desktop` | `${DATADIR}/applications/` |
| `hd60pro.svg` | `${DATADIR}/icons/hicolor/scalable/apps/` |
| `hd60pro-autostart.desktop` | `${DATADIR}/hd60pro/` |

- Option `-DHD60_BUILD_APP=OFF` builds only the driver (no Qt needed).
- The spec: `Name: hd60pro-linux`, `License: GPL-2.0-or-later`; `BuildRequires: cmake gcc gcc-c++ qt6-qtbase-devel qt6-qtmultimedia-devel`; `Requires: pipewire-utils pulseaudio-utils polkit systemd /usr/bin/ffmpeg /usr/bin/ffprobe` (satisfied by Fedora's `ffmpeg-free` or RPM Fusion's `ffmpeg`); `Recommends: v4l2loopback` (RPM Fusion); `%post`/`%preun` use `%systemd_user_post` / `%systemd_user_preun hd60pro.service`. The description notes that NVENC needs RPM Fusion's `ffmpeg`.
- README: install from source (`cmake -B build -DCMAKE_INSTALL_PREFIX=/usr && cmake --build build && sudo cmake --install build`), then open "HD60 Pro" from the menu and press **Set up**. Add the per-distro dependency lists from Global Constraints plus v4l2loopback (Fedora via RPM Fusion; Arch `v4l2loopback-dkms`; Debian/Ubuntu `v4l2loopback-dkms`) and an RPM build line (`rpmbuild -ba packaging/hd60pro-linux.spec` after `git archive`). State that streaming goes through OBS with the "HD60 Pro" camera and the "HD60 Pro Audio" source.

- [ ] **Step 1: Write the install check**

`packaging/check-install.sh`: `cmake -S . -B build/pkg -DCMAKE_INSTALL_PREFIX=/usr && cmake --build build/pkg && DESTDIR=$tmp cmake --install build/pkg`, then assert every destination in the table exists under `$tmp`, that `hd60pro.service` contains `ExecStart=/usr/bin/hd60pro`, and that the policy contains `/usr/libexec/hd60pro-setup`. Also run `desktop-file-validate` on both desktop files if it is installed.

- [ ] **Step 2: Run it to verify it fails**

Run: `bash packaging/check-install.sh`
Expected: FAIL, no top-level `CMakeLists.txt`.

- [ ] **Step 3: Write the top-level CMake, install rules, desktop file and spec**

- [ ] **Step 4: Run it to verify it passes, and build the RPM**

Run: `bash packaging/check-install.sh && git archive --prefix=hd60pro-linux-0.2.0/ -o ~/rpmbuild/SOURCES/hd60pro-linux-0.2.0.tar.gz HEAD && rpmbuild -ba packaging/hd60pro-linux.spec`
Expected: `install layout ok`, and `rpmbuild` writes `hd60pro-linux-0.2.0-1.fc44.x86_64.rpm`. `rpm -qlp` on it shows no file from `re/` and no Elgato files.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt app packaging README.md CHANGELOG.md .github/workflows/build.yml
git commit -m "Install rules, desktop entry, RPM spec and install docs"
```

---

### Task 10: End-to-end check on the hardware

This task changes the owner's system configuration. Ask the owner before Step 2, and stop the hand-started driver (`run` in a terminal) first.

- [ ] **Step 1: Install**

Run: `sudo dnf install ~/rpmbuild/RPMS/x86_64/hd60pro-linux-0.2.0-1.fc44.x86_64.rpm`
Expected: installs cleanly; "HD60 Pro" appears in the app menu.

- [ ] **Step 2: Set up from the app**

Open the app → Settings → **Set up…** → **Set up** (polkit prompt).
Expected: the checks turn ✓ except memlock, and it says "Log out and back in to finish". `/dev/video60` "HD60 Pro" exists. "Virtual Camera" `/dev/video0` is unchanged.

- [ ] **Step 3: Log out and in, then check each goal**

| check | expected |
| --- | --- |
| `systemctl --user status hd60pro` with the app closed | active; HDMI OUT shows the Switch on the TV (anti-goal: passthrough only with the app open) |
| `pactl list short sources \| grep hd60pro` with the app closed | the source is there |
| app status line | `1920×1080p60 · 48 kHz · HDMI OUT: passthrough on · Driver: running (60.0 fps)`, no low-memlock note |
| Listen on, volume 50 % | game audio in the headphones, no perceptible delay against the TV |
| Record 30 s on each preset, then `ffprobe` | 30 s ±1 s, 1920×1080, h264_nvenc for auto, the expected bitrate within ±20 % |
| unplug the Switch while recording | a message says the recording stopped; the file plays |
| start the `win10` VM, then look at the app | "in use by a virtual machine" banner; the service is not crash-looping (`journalctl --user -u hd60pro -n 20`) |
| shut the VM down | the driver comes back by itself, or **Fix access** fixes it in one click |
| close the window with Listen on, launch again from the menu | the tray keeps the audio going; the second launch raises the same window |
| OBS: add "HD60 Pro" camera + "HD60 Pro Audio" while the app previews | both work at the same time |
| reboot with "Listen at login" on | headphones carry the game audio after login with no clicks |

- [ ] **Step 4: Record the results**

Update `COMPATIBILITY.md` (the Fedora 44 row: app and service tested) and `CHANGELOG.md`.

```bash
git add COMPATIBILITY.md CHANGELOG.md
git commit -m "Record end-to-end test of the app and service on Fedora 44"
```
