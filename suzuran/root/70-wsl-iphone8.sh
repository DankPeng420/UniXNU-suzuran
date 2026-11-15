#!/bin/bash
# Run inside WSL:  bash /mnt/c/Users/<you>/Desktop/suzuran-port/root/70-wsl-iphone8.sh
# iPhone 8 identity: builds ~/iosbuild8/ios-root.img from the 4.7-inch P3 IPSW (iPhone 8 / SE 2 / SE 3, iOS 16.7.16
# 20H392): OS 087-86684-044, SystemOS 087-86580-047, AppOS 087-86715-047 (same as iPhone X), same steps as 62 + 63 + 65,
# plus the D201AP (iPhone10,4) device tree. The iPhone X build in ~/iosbuild is left alone. Nothing touches the phone.
# Members are checked against the CRC32s in the IPSW's own zip directory (no published SHA-256 for these).
# Results for Claude: root/out8/ (d201ap.adt, root.trustcache, ios-root.img.sha256). Log: root/70-iphone8.log
D=$(cd "$(dirname "$0")" && pwd)
exec > >(tee "$D/70-iphone8.log") 2>&1
set -euo pipefail
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
IPSW="$D/../ipsw/iPhone_4.7_P3_16.7.16_20H392_Restore.ipsw"
[ -f "$IPSW" ] || { echo "missing $IPSW"; exit 1; }
B=~/iosbuild8; W=$B/ipsw; OUT=$B/ios-root.img; M=$B/mnt
SIZE=${SIZE:-20G}
OSD=087-86684-044.dmg; SYSD=087-86580-047.dmg; APPD=087-86715-047.dmg; DT=DeviceTree.d201ap.im4p
mkdir -p "$W" "$D/out8"
df -h ~ | tail -1
echo "=== 1. extract $(date +%T)"
unzip -o -j "$IPSW" $OSD $SYSD $APPD "Firmware/$OSD.trustcache" "Firmware/$SYSD.trustcache" "Firmware/$APPD.trustcache" \
    "Firmware/all_flash/$DT" -d "$W"
python3 - "$IPSW" "$W" <<'PY'
import sys, zipfile, zlib, os
z = zipfile.ZipFile(sys.argv[1]); bad = 0
for i in z.infolist():
    p = os.path.join(sys.argv[2], os.path.basename(i.filename))
    if i.is_dir() or not os.path.exists(p) or os.path.getsize(p) != i.file_size: continue
    c = 0
    with open(p, 'rb') as f:
        while True:
            b = f.read(1 << 24)
            if not b: break
            c = zlib.crc32(b, c)
    ok = (c & 0xffffffff) == i.CRC; bad += not ok
    print(f"{'OK ' if ok else 'BAD'} {i.CRC:08x} {i.file_size:>12} {os.path.basename(i.filename)}")
sys.exit(1 if bad else 0)
PY
echo "=== 2. build root $(date +%T)"
[ ! -e "$OUT" ] || { echo "refusing to overwrite $OUT (delete it first)"; exit 1; }
mkdir -p "$M"
for m in root os sysos appos; do $SUDO mountpoint -q "$M/$m" 2>/dev/null && $SUDO umount "$M/$m" || true; done
mounts=()
cleanup(){ for m in "${mounts[@]}"; do $SUDO mountpoint -q "$m" && $SUDO umount "$m" || true; done; }
trap cleanup EXIT
amount(){ mkdir -p "$M/$2"; ln -sf "$W/$1" "$M/$2.dmg"; $SUDO "$(command -v apfs-fuse)" "$M/$2.dmg" "$M/$2"; mounts+=("$M/$2"); }
date
# preflight: the WSL kernel must have HFS+ and FUSE
grep -qw hfsplus /proc/filesystems || $SUDO modprobe hfsplus 2>/dev/null || true
grep -qw hfsplus /proc/filesystems || { echo "NO HFSPLUS IN THIS WSL KERNEL ($(uname -r))"; exit 2; }
ls -l /dev/fuse
command -v apfs-fuse mkfs.hfsplus fsck.hfsplus rsync
amount $OSD os
amount $SYSD sysos
amount $APPD appos
$SUDO ls "$M/os" "$M/os/root" | head -20
truncate -s "$SIZE" "$OUT"
mkfs.hfsplus -s -J -v iOSRoot "$OUT"
mkdir -p "$M/root"
$SUDO mount -t hfsplus -o loop,force "$OUT" "$M/root"
mounts=("$M/root" "${mounts[@]}")
copy(){ echo "$(date +%T) copy $1 -> $2"; $SUDO mkdir -p "$2"; $SUDO rsync -aH --numeric-ids "$1/" "$2/"; }
copy "$M/os/root" "$M/root"
copy "$M/sysos/root" "$M/root/private/preboot/Cryptexes/OS"
copy "$M/appos/root" "$M/root/private/preboot/Cryptexes/App"
echo "launchd: $($SUDO head -c 4 "$M/root/sbin/launchd" | od -An -tx1)  (expect cf fa ed fe = decompressed Mach-O)"
$SUDO stat -c '%u:%g %A %s' "$M/root/sbin/launchd"
$SUDO python3 "$D/stage-ios-root.py" "$M/root"
df -h "$M/root"
$SUDO umount "$M/root"; mounts=("${mounts[@]:1}")
$SUDO fsck.hfsplus -f -y "$OUT" | grep -v 'Illegal name' || true
$SUDO fsck.hfsplus -f -n "$OUT" || true
python3 "$D/make-trustcache-bundle.py" "$B/root.trustcache" "$W/$OSD.trustcache" "$W/$SYSD.trustcache+$W/$APPD.trustcache"
ls -l "$OUT" "$B/root.trustcache"
sha256sum "$OUT" | tee "$B/ios-root.img.sha256"
echo "=== 3. device tree $(date +%T)"
L=~/iosbuild/src/apfs-fuse/3rdparty/lzfse
[ -x "$L/build/bin/lzfse" ] || make -C "$L" -j"$(nproc)" >/dev/null
python3 - "$W/$DT" "$B/d201ap.payload" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
def der(pos):
    tag, n = d[pos], d[pos + 1]; pos += 2
    if n & 0x80:
        c = n & 0x7f; n = int.from_bytes(d[pos:pos + c], 'big'); pos += c
    return tag, pos, pos + n
tag, pos, _ = der(0); assert tag == 0x30
fields = []
for _ in range(4):
    t, s, e = der(pos); fields.append((t, d[s:e])); pos = e
assert fields[0] == (0x16, b'IM4P') and fields[3][0] == 0x04, fields[:2]
open(sys.argv[2], 'wb').write(fields[3][1]); print('type', fields[1][1], 'payload', len(fields[3][1]))
PY
if head -c 3 "$B/d201ap.payload" | grep -q bvx; then "$L/build/bin/lzfse" -decode -i "$B/d201ap.payload" -o "$B/d201ap.adt"
else cp "$B/d201ap.payload" "$B/d201ap.adt"; fi
cp "$B/d201ap.adt" "$B/root.trustcache" "$B/ios-root.img.sha256" "$D/out8/"
ls -l "$D/out8"
echo "=== iphone8 done $(date +%T) ==="
