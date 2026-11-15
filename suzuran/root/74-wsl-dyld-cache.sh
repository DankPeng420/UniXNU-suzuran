#!/bin/bash
# Run inside WSL:  bash /mnt/c/Users/<you>/Desktop/suzuran-port/root/74-wsl-dyld-cache.sh
# Copies the iPhone 8 (iOS 16.7.16 20H392) dyld shared cache out of the SystemOS cryptex image to root/dsc/, so Claude
# can put names on the addresses of a crash (backboardd's, far 0x30). Reads the IPSW only; nothing touches the phone.
# Log: root/74-dyld-cache.log
D=$(cd "$(dirname "$0")" && pwd)
exec > >(tee "$D/74-dyld-cache.log") 2>&1
set -euo pipefail
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
IPSW="$D/../ipsw/iPhone_4.7_P3_16.7.16_20H392_Restore.ipsw"
B=~/iosbuild8; W=$B/ipsw; M=$B/mnt-dsc
SYSD=087-86580-047.dmg
mkdir -p "$W" "$M" "$D/dsc"
[ -f "$W/$SYSD" ] || unzip -o -j "$IPSW" "$SYSD" -d "$W"
ls -l "$W/$SYSD"
$SUDO mountpoint -q "$M" && $SUDO umount "$M" || true
$SUDO "$(command -v apfs-fuse)" "$W/$SYSD" "$M"
trap '$SUDO umount "$M" || true' EXIT
C="$M/root/System/Library/Caches/com.apple.dyld"
$SUDO ls -l "$C"
for f in $($SUDO ls "$C"); do
    case "$f" in dyld_shared_cache_arm64|dyld_shared_cache_arm64.*) ;; *) continue ;; esac
    echo "$(date +%T) copy $f"
    $SUDO cat "$C/$f" > "$D/dsc/$f"
done
ls -l "$D/dsc"
echo "=== dyld cache done ==="
