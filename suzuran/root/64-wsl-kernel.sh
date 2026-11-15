#!/bin/bash
# Run inside WSL:  bash /mnt/c/Users/<you>/Desktop/suzuran-port/root/64-wsl-kernel.sh
# The stock WSL kernel has no HFS+. Build Microsoft's WSL kernel (same version line) with HFS+, FUSE and loop
# built in, copy it to C:\wsl\bzImage-hfs and point C:\Users\<you>\.wslconfig at it. Log: root/64-kernel.log
# Afterwards run `wsl --shutdown` in PowerShell and reopen Ubuntu.
D=$(cd "$(dirname "$0")" && pwd)
exec > >(tee "$D/64-kernel.log") 2>&1
set -x
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
$SUDO apt-get install -y build-essential flex bison libssl-dev libelf-dev bc dwarves cpio python3 git || true
cd ~/iosbuild/src
TAG="linux-msft-wsl-$(uname -r | sed 's/-microsoft.*//')"
if [ ! -d WSL2-Linux-Kernel ]; then
  git clone --depth 1 --branch "$TAG" https://github.com/microsoft/WSL2-Linux-Kernel.git \
    || git clone --depth 1 https://github.com/microsoft/WSL2-Linux-Kernel.git
fi
cd WSL2-Linux-Kernel && git log -1 --format='%h %s'
if [ -r /proc/config.gz ]; then zcat /proc/config.gz > .config; else cp Microsoft/config-wsl .config; fi
scripts/config --enable HFSPLUS_FS --enable FUSE_FS --enable BLK_DEV_LOOP --enable HFS_FS
make olddefconfig
grep -E "^CONFIG_(HFSPLUS_FS|FUSE_FS|BLK_DEV_LOOP)=" .config
make -j"$(nproc)" bzImage 2>&1 | tail -5
mkdir -p /mnt/c/wsl && cp arch/x86/boot/bzImage /mnt/c/wsl/bzImage-hfs && ls -l /mnt/c/wsl/bzImage-hfs || { echo "=== kernel FAILED ==="; exit 1; }
WIN_USER=$(cmd.exe /c "echo %USERNAME%" 2>/dev/null | tr -d '\r')
CFG=/mnt/c/Users/${WIN_USER:?set WIN_USER to your Windows user name}/.wslconfig
if [ -e "$CFG" ]; then cp "$CFG" "$CFG.bak"; echo "existing .wslconfig backed up:"; cat "$CFG"; fi
if grep -q '^\[wsl2\]' "$CFG" 2>/dev/null; then
  sed -i '/^kernel=/d' "$CFG"; sed -i 's|^\[wsl2\]|[wsl2]\nkernel=C:\\\\wsl\\\\bzImage-hfs|' "$CFG"
else
  printf '[wsl2]\nkernel=C:\\\\wsl\\\\bzImage-hfs\n' >> "$CFG"
fi
echo ".wslconfig now:"; cat "$CFG"
echo "=== kernel done: run 'wsl --shutdown' in PowerShell, then reopen Ubuntu ==="
