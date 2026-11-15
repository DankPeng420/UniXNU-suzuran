#!/bin/bash
# READ-ONLY. Run inside WSL:  bash /mnt/c/Users/<you>/Desktop/suzuran-port/root/73-wsl-launchd-plist.sh
# Copies launchd's job list (/System/Library/xpc/launchd.plist, as stage-ios-root.py left it) out of the iPhone 8
# root image ~/iosbuild8/ios-root.img into root/out8/launchd.plist, so the addon can add a job to it.
# The image is mounted read-only; nothing touches the phone. Log: root/73-launchd-plist.log
D=$(cd "$(dirname "$0")" && pwd)
exec > >(tee "$D/73-launchd-plist.log") 2>&1
set -euo pipefail
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
IMG=~/iosbuild8/ios-root.img; M=~/iosbuild8/mnt/ro
[ -f "$IMG" ] || { echo "missing $IMG"; exit 1; }
mkdir -p "$M"
$SUDO mountpoint -q "$M" && $SUDO umount "$M" || true
$SUDO mount -t hfsplus -o loop,ro "$IMG" "$M"
trap '$SUDO umount "$M"' EXIT
$SUDO cp "$M/System/Library/xpc/launchd.plist" "$D/out8/launchd.plist"
$SUDO chown "$(id -u):$(id -g)" "$D/out8/launchd.plist"
$SUDO stat -c '%u:%g %a %s %n' "$M/System/Library/xpc/launchd.plist" | tee "$D/out8/launchd.plist.stat"
$SUDO ls -ld "$M/usr/libexec" "$M/usr/lib" "$M/System/Library/xpc" | tee -a "$D/out8/launchd.plist.stat"
sha256sum "$D/out8/launchd.plist"
echo "=== done ==="
