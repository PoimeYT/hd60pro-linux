#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Installs the driver so the card is ready after every boot and the driver
# starts when you log in. Run it as your normal user from the repository; it
# asks for sudo for the system parts. system/uninstall.sh removes everything.
#
# What it installs (and nothing else: no bootloader, initramfs or udev changes):
#   /usr/local/bin/hd60pro                           the driver
#   /usr/local/libexec/hd60pro-bind                  boot-time vfio-pci binding
#   /etc/systemd/system/hd60pro-bind.service         runs it at boot
#   /etc/modules-load.d/hd60pro.conf                 loads v4l2loopback at boot
#   /etc/modprobe.d/hd60pro.conf                     "HD60 Pro" camera on /dev/video60
#   /etc/systemd/system/user@UID.service.d/hd60pro-memlock.conf   locked-memory limit
#   /etc/systemd/user/hd60pro.service                starts the driver at login
#   /usr/local/bin/hd60pro-app                       the app (when Qt 6 is installed)
#   /usr/local/share/applications/hd60pro.desktop    its menu entry
#   /usr/local/share/icons/hicolor/scalable/apps/hd60pro.svg
set -euo pipefail

if [ "$(id -u)" = 0 ]; then
	echo "Run this as your normal user, not with sudo: it needs to know whose session gets the card." >&2
	exit 1
fi
here=$(cd "$(dirname "$0")" && pwd)
repo=$(dirname "$here")
user=$(id -un)
uid=$(id -u)

echo "== Building the driver"
make -C "$repo/userspace"

have_app=0
if command -v qmake6 >/dev/null 2>&1; then
	echo "== Building the app"
	(cd "$repo/app" && qmake6 hd60pro-app.pro -o Makefile >/dev/null && make -j"$(nproc)") && have_app=1
	[ "$have_app" = 1 ] || echo "note: the app did not build; installing the driver only."
else
	echo "note: Qt 6 (qmake6) is not installed, so the app is skipped; the driver works without it."
fi

echo "== Installing system files (sudo)"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/hd60pro-bind.service" <<UNIT
[Unit]
Description=Hand the Elgato HD60 Pro to vfio-pci for user $user
After=systemd-modules-load.service

[Service]
Type=oneshot
RemainAfterExit=yes
# Never let a stuck bind hold up boot. Logins do not wait for this unit:
# the user service skips cleanly (ExecCondition) when the card is not bound.
TimeoutStartSec=30
ExecStart=/usr/local/libexec/hd60pro-bind $user

[Install]
WantedBy=multi-user.target
UNIT

cat > "$tmp/modules-load.conf" <<'UNIT'
# hd60pro-linux: loopback camera the driver feeds
v4l2loopback
UNIT

cat > "$tmp/modprobe.conf" <<'UNIT'
# hd60pro-linux: one loopback camera called "HD60 Pro" at /dev/video60
options v4l2loopback exclusive_caps=1 card_label="HD60 Pro" video_nr=60
UNIT

cat > "$tmp/memlock.conf" <<'UNIT'
# hd60pro-linux: the driver pins about 32 MB of DMA buffers
[Service]
LimitMEMLOCK=64M
UNIT

cat > "$tmp/hd60pro.service" <<'UNIT'
[Unit]
Description=Elgato HD60 Pro capture driver
After=pipewire-pulse.socket pipewire-pulse.service
StartLimitIntervalSec=120
StartLimitBurst=5

[Service]
# Skip quietly (no restart loop) when the system is not ready for the card:
# IOMMU off, card not bound, shared IOMMU group. "hd60pro check" says why.
ExecCondition=/usr/local/bin/hd60pro check
ExecStart=/usr/local/bin/hd60pro run /dev/video60
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
UNIT

if [ -e /etc/modprobe.d/hd60pro.conf ] || ! grep -rqs '^options v4l2loopback' /etc/modprobe.d; then
	loopback_opts=1
else
	loopback_opts=0
	echo "note: /etc/modprobe.d already sets v4l2loopback options; leaving them alone."
	echo "      Make sure they create a device at /dev/video60."
fi

sudo install -D -m 755 "$repo/userspace/hd60pro" /usr/local/bin/hd60pro
sudo install -D -m 755 "$here/hd60pro-bind" /usr/local/libexec/hd60pro-bind
sudo install -D -m 644 "$tmp/hd60pro-bind.service" /etc/systemd/system/hd60pro-bind.service
sudo install -D -m 644 "$tmp/modules-load.conf" /etc/modules-load.d/hd60pro.conf
if [ "$loopback_opts" = 1 ]; then
	sudo install -D -m 644 "$tmp/modprobe.conf" /etc/modprobe.d/hd60pro.conf
fi
sudo install -D -m 644 "$tmp/memlock.conf" "/etc/systemd/system/user@$uid.service.d/hd60pro-memlock.conf"
sudo install -D -m 644 "$tmp/hd60pro.service" /etc/systemd/user/hd60pro.service
if [ "$have_app" = 1 ]; then
	sudo install -D -m 755 "$repo/app/hd60pro-app" /usr/local/bin/hd60pro-app
	sudo install -D -m 644 "$repo/app/resources/hd60pro.desktop" /usr/local/share/applications/hd60pro.desktop
	sudo install -D -m 644 "$repo/app/resources/hd60pro.svg" /usr/local/share/icons/hicolor/scalable/apps/hd60pro.svg
	sudo gtk-update-icon-cache -q -t /usr/local/share/icons/hicolor 2>/dev/null || true
	sudo update-desktop-database -q /usr/local/share/applications 2>/dev/null || true
fi
sudo systemctl daemon-reload
sudo systemctl enable hd60pro-bind.service

echo "== Setting up now (the same steps run at every boot)"
if ! modinfo v4l2loopback >/dev/null 2>&1; then
	echo "note: the v4l2loopback kernel module is not installed; install it (v4l2loopback-dkms) for video."
elif [ ! -d /sys/module/v4l2loopback ]; then
	sudo modprobe v4l2loopback || echo "note: could not load v4l2loopback now"
elif [ ! -e /dev/video60 ]; then
	echo "note: v4l2loopback is already loaded without /dev/video60; it appears after the next reboot."
fi
sudo systemctl restart hd60pro-bind.service ||
	echo "note: the card could not be bound yet (see the message above or: journalctl -u hd60pro-bind -b)"

systemctl --user daemon-reload
systemctl --user enable hd60pro.service
# Run the driver that was just installed (a no-op skip when the system is not ready).
systemctl --user stop hd60pro-test.service 2>/dev/null || true
systemctl --user restart hd60pro.service || true

echo
if [ ! -e /sys/kernel/iommu_groups/0 ]; then
	echo "Installed. The IOMMU is off in this boot, so the card cannot be used yet."
	echo "After a reboot with intel_iommu=on everything starts by itself at login."
else
	echo "Installed. The driver is running and starts by itself at every login."
	echo "(After the very first install, log out and in once so the locked-memory limit applies.)"
	[ "$have_app" = 1 ] && echo "The app is in your application menu as \"HD60 Pro\" (or run: hd60pro-app)."
fi
echo "Status:  systemctl --user status hd60pro.service    Logs:  journalctl --user -u hd60pro -b"
echo "Remove:  $here/uninstall.sh"
