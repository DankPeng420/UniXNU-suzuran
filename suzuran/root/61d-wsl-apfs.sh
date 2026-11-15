#!/bin/bash
# Run inside WSL:  bash /mnt/c/Users/<you>/Desktop/suzuran-port/root/61d-wsl-apfs.sh
# apfs-fuse (2023) misses #include <cstdint> for GCC 15: force-include it. Log: root/61d-apfs.log
D=$(cd "$(dirname "$0")" && pwd)
exec > >(tee "$D/61d-apfs.log") 2>&1
set -x
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
cd ~/iosbuild/src/apfs-fuse && rm -rf build && mkdir build && cd build
cmake .. -DUSE_FUSE3=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS="-include cstdint -include cstring" \
  && make -j"$(nproc)" 2>&1 | grep -E "error|Error|Built target" \
  ; [ -x apfs-fuse ] && $SUDO install -m 0755 apfs-fuse apfsutil /usr/local/bin/
command -v apfs-fuse apfsutil && echo "=== apfs OK ===" || echo "=== apfs FAILED ==="
