#!/bin/bash
# Run inside WSL:  bash /mnt/c/Users/<you>/Desktop/suzuran-port/root/61b-wsl-setup.sh
# Ubuntu 26.04 ("resolute") has no apfs-fuse package: build it from source (github.com/sgan81/apfs-fuse)
# and install the HFS+ tools separately. Log: root/61b-setup.log
D=$(cd "$(dirname "$0")" && pwd)
exec > >(tee "$D/61b-setup.log") 2>&1
set -x
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
for p in hfsprogs rsync python3 unzip fuse3 git cmake g++ make pkg-config libfuse3-dev zlib1g-dev libbz2-dev libattr1-dev; do
    $SUDO apt-get install -y "$p" || echo "MISSING PACKAGE: $p"
done
apt-cache search hfs | head -20
B=~/iosbuild/src; mkdir -p "$B"; cd "$B"
[ -d apfs-fuse ] || git clone --recursive https://github.com/sgan81/apfs-fuse.git
cd apfs-fuse && git submodule update --init --recursive && git log -1 --format='%h %cd' && mkdir -p build && cd build
cmake .. -DUSE_FUSE3=ON && make -j"$(nproc)" && $SUDO install -m 0755 apfs-fuse apfsutil /usr/local/bin/
command -v apfs-fuse mkfs.hfsplus fsck.hfsplus rsync unzip python3
echo "=== setup done ==="
