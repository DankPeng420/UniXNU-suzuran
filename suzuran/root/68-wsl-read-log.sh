#!/bin/bash
# READ-ONLY. Phone in TWRP, MTP disabled. Run in WSL:
#   bash /mnt/c/Users/<you>/Desktop/suzuran-port/root/68-wsl-read-log.sh
# Copies NativeSdhci's console log ring (last 65536 blocks of the 'ios' partition + header block)
# to root/out/natlog.bin and extracts the text to root/out/natlog.txt. Log: root/68-read-log.log
D=$(cd "$(dirname "$0")" && pwd)
exec > >(tee "$D/68-read-log.log") 2>&1
set -euo pipefail
ADB="/mnt/c/Program Files/platform-tools/adb.exe"
[ "$("$ADB" get-state | tr -d '\r')" = recovery ] || { echo "phone not in TWRP recovery (or MTP still on)"; exit 1; }
SEC=$("$ADB" exec-out cat /sys/class/block/mmcblk0p42/size | tr -d '\r\n ')
HDR=$((SEC - 65536 - 1))
echo "p42 $SEC sectors, log header at p42 block $HDR"
"$ADB" exec-out "dd if=/dev/block/mmcblk0p42 bs=512 skip=$HDR count=65537 2>/dev/null" > "$D/out/natlog.bin"
ls -l "$D/out/natlog.bin"
python3 - "$D/out/natlog.bin" "$D/out/natlog.txt" <<'PY'
import sys, struct
d = open(sys.argv[1], 'rb').read(); h, ring = d[:512], d[512:]
print('header magic', h[:8], 'end', struct.unpack_from('<Q', h, 8)[0], 'blocks', struct.unpack_from('<I', h, 16)[0],
      'boot', struct.unpack_from('<I', h, 20)[0], 'base', struct.unpack_from('<Q', h, 24)[0],
      'lost', struct.unpack_from('<Q', h, 32)[0], 'flushes', struct.unpack_from('<Q', h, 40)[0])
t = ring.replace(b'\0', b'').decode('utf-8', 'replace')
open(sys.argv[2], 'w').write(t)
print(len(t), 'chars of text')
for l in t.splitlines():
    if 'NativeSdhci' in l or 'panic' in l.lower(): print(l)
PY
echo "=== done ==="
