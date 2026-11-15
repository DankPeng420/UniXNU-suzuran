#!/bin/bash
# READ-ONLY. Phone in TWRP, MTP disabled. Run in WSL:
#   bash /mnt/c/Users/<you>/Desktop/suzuran-port/root/69-wsl-check.sh
# 1) whole-disk GPT still valid (CRCs, primary == backup, entry 42 = ios)
# 2) start of p42 vs start of ios-root.img (HFS+ volume header, anything written over it)
# 3) the GPT-like header found in the last block of p42
# Log: root/69-check.log
D=$(cd "$(dirname "$0")" && pwd)
exec > >(tee "$D/69-check.log") 2>&1
set -uo pipefail
ADB="/mnt/c/Program Files/platform-tools/adb.exe"
[ "$("$ADB" get-state | tr -d '\r')" = recovery ] || { echo "phone not in TWRP recovery (or MTP still on)"; exit 1; }
echo "=== 1. disk GPT"
python3 "$D/gpt42.py" "$ADB" "$D/out" --dry
echo "=== 2. p42 start vs image"
"$ADB" exec-out "dd if=/dev/block/mmcblk0p42 bs=512 count=4096 2>/dev/null" > /tmp/p42head.bin
SEC=$("$ADB" exec-out cat /sys/class/block/mmcblk0p42/size | tr -d '\r\n ')
"$ADB" exec-out "dd if=/dev/block/mmcblk0p42 bs=512 skip=$((SEC - 34)) count=34 2>/dev/null" > /tmp/p42tail.bin
python3 - ~/iosbuild/ios-root.img /tmp/p42head.bin /tmp/p42tail.bin <<'PY'
import sys, struct
img = open(sys.argv[1], 'rb').read(4096 * 512); p = open(sys.argv[2], 'rb').read(); t = open(sys.argv[3], 'rb').read()
print('read', len(p), 'bytes of p42 head,', len(t), 'of tail')
diff = [i for i in range(len(p) // 512) if p[i*512:(i+1)*512] != img[i*512:(i+1)*512]]
print('sectors differing from the image in the first 4096:', len(diff), diff[:40])
for i in diff[:6]:
    print('  sector %d now: %s' % (i, p[i*512:i*512+48].hex()))
    print('  sector %d img: %s' % (i, img[i*512:i*512+48].hex()))
vh = p[1024:1024+512]
print('HFS volume header sig %r version %d blocksize %d totalblocks %d' % (vh[:2], struct.unpack('>H', vh[2:4])[0],
      struct.unpack('>I', vh[40:44])[0], struct.unpack('>I', vh[44:48])[0]))
print('p42 sector 1 starts:', p[512:520])
print('tail: nonzero sectors', [i for i in range(34) if any(t[i*512:(i+1)*512])], '(33 = last sector of p42)')
PY
echo "=== done"
