#!/bin/bash
# Run inside WSL:  bash /mnt/c/Users/<you>/Desktop/suzuran-port/root/61c-wsl-apfs.sh
# Rebuild apfs-fuse with CMake 4 (its CMakeLists asks for an old cmake_minimum_required). Log: root/61c-apfs.log
D=$(cd "$(dirname "$0")" && pwd)
exec > >(tee "$D/61c-apfs.log") 2>&1
set -x
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
cd ~/iosbuild/src/apfs-fuse && rm -rf build && mkdir build && cd build
cmake .. -DUSE_FUSE3=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 && make -j"$(nproc)" && $SUDO install -m 0755 apfs-fuse apfsutil /usr/local/bin/
command -v apfs-fuse apfsutil
echo "=== apfs done ==="
