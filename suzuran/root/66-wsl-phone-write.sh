#!/bin/bash
# Run inside WSL with the phone in TWRP, MTP DISABLED, USB connected:
#   bash /mnt/c/Users/<you>/Desktop/suzuran-port/root/66-wsl-phone-write.sh
# 1) model check, unmount /data, size check of partition 42
# 2) gpt42.py: entry 42 userdata -> 'ios' / Apple HFS+ (writes only the 2 GPT headers + 2 entry sectors)
# 3) streams ~/iosbuild/ios-root.img into /dev/block/mmcblk0p42   4) reads back, compares sha256
# Log: root/66-write.log
D=$(cd "$(dirname "$0")" && pwd)
exec > >(tee "$D/66-write.log") 2>&1
set -euo pipefail
ADB="/mnt/c/Program Files/platform-tools/adb.exe"
# iPhone 8 build:  IMG=~/iosbuild8/ios-root.img SHA=root/out8/ios-root.img.sha256 (see 71-wsl-phone-write-iphone8.sh)
IMG=${IMG:-~/iosbuild/ios-root.img}
WANT=$(cut -d' ' -f1 "${SHA:-$D/out/ios-root.img.sha256}")
SIZE=$(stat -c %s "$IMG"); BS=4194304
echo "image $SIZE bytes, expected sha256 $WANT"
[ $((SIZE % BS)) -eq 0 ] || { echo "image size not a multiple of 4 MiB"; exit 1; }
N=$((SIZE / BS))
"$ADB" devices
[ "$("$ADB" get-state | tr -d '\r')" = recovery ] || { echo "phone not in TWRP recovery (or MTP still on)"; exit 1; }
"$ADB" push "$(wslpath -w "$D/ios-prep.sh")" /tmp/ios-prep.sh
OUT=$("$ADB" shell sh /tmp/ios-prep.sh | tr -d "\r" || true); echo "$OUT"
echo "$OUT" | grep -q PREP_OK || { echo "=== prep refused, nothing written ==="; exit 1; }
SEC=$("$ADB" exec-out cat /sys/class/block/mmcblk0p42/size | tr -d '\r\n ')
echo "p42 $SEC sectors = $((SEC * 512)) bytes"
[ $((SEC * 512)) -ge "$SIZE" ] || { echo "=== p42 too small, nothing written ==="; exit 1; }
echo "=== GPT entry 42 ==="
python3 "$D/gpt42.py" "$ADB" "$D/out" | tee /tmp/gpt42.out
grep -q GPT_OK /tmp/gpt42.out || { echo "=== GPT step failed ==="; exit 1; }
echo "=== writing $N x 4 MiB (takes ~10-20 min) $(date) ==="
"$ADB" push "$(wslpath -w "$IMG")" /dev/block/mmcblk0p42
"$ADB" shell sync
echo "=== verifying $(date) ==="
GOT=$("$ADB" shell "dd if=/dev/block/mmcblk0p42 bs=$BS count=$N 2>/dev/null | sha256sum" | tr -d '\r' | cut -d' ' -f1 || true)
if ! [[ "$GOT" =~ ^[0-9a-f]{64}$ ]]; then
  echo "no sha256sum on phone ($GOT), hashing read-back on PC"
  GOT=$("$ADB" exec-out "dd if=/dev/block/mmcblk0p42 bs=$BS count=$N 2>/dev/null" | sha256sum | cut -d' ' -f1)
fi
echo "want $WANT"; echo "got  $GOT"
[ "$GOT" = "$WANT" ] && echo "=== WRITE VERIFIED OK $(date) ===" || { echo "=== HASH MISMATCH ==="; exit 1; }
