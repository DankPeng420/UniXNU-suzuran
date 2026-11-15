#!/bin/bash
# Run inside WSL (Ubuntu):  bash /mnt/c/Users/<you>/Desktop/suzuran-port/root/61-wsl-setup.sh
# Installs the Linux tools for building the iOS root image. Log: root/61-setup.log
D=$(cd "$(dirname "$0")" && pwd)
exec > >(tee "$D/61-setup.log") 2>&1
set -x
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
$SUDO apt-get update
$SUDO apt-get install -y apfs-fuse hfsprogs rsync python3 unzip fuse3 || $SUDO apt-get install -y apfs-fuse hfsprogs rsync python3 unzip
command -v apfs-fuse mkfs.hfsplus fsck.hfsplus rsync unzip python3
dpkg -l apfs-fuse hfsprogs | tail -2
ls -l /dev/fuse
uname -r
df -h ~ /mnt/c | tail -2
echo "=== setup done ==="
