#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Removes everything system/install.sh installed. Run as your normal user.
# The card stays on vfio-pci until the next reboot, which is harmless.
set -uo pipefail

if [ "$(id -u)" = 0 ]; then
	echo "Run this as your normal user, not with sudo." >&2
	exit 1
fi
uid=$(id -u)

systemctl --user disable --now hd60pro.service 2>/dev/null
sudo systemctl disable hd60pro-bind.service 2>/dev/null
sudo rm -f /usr/local/bin/hd60pro \
	/usr/local/libexec/hd60pro-bind \
	/etc/systemd/system/hd60pro-bind.service \
	/etc/modules-load.d/hd60pro.conf \
	/etc/modprobe.d/hd60pro.conf \
	"/etc/systemd/system/user@$uid.service.d/hd60pro-memlock.conf" \
	/etc/systemd/user/hd60pro.service \
	/usr/local/bin/hd60pro-app \
	/usr/local/share/applications/hd60pro.desktop \
	/usr/local/share/icons/hicolor/scalable/apps/hd60pro.svg
rm -f "${XDG_CONFIG_HOME:-$HOME/.config}/autostart/hd60pro-app.desktop"
sudo rmdir "/etc/systemd/system/user@$uid.service.d" 2>/dev/null
sudo systemctl daemon-reload
systemctl --user daemon-reload
echo "Removed. Reboot (or log out and in) to drop the card binding and the memory limit."
