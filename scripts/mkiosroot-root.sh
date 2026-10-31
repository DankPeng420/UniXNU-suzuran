#!/bin/sh
# Run as root. The merged iOS 16.7.16 HFS+ root for p55, a macOS port of ios-sweet/ios-fuxi mkiosroot.sh:
# OS + SystemOS + AppOS copied decompressed into one case-sensitive journaled HFS+ volume (the kernel
# has only the zlib decmpfs type; the IPSW files are LZBITMAP), then stage-ios-root.py (cryptex hard
# links incl. the dyld cache, logd stores, software keybag, userland tweaks: stage-ios-root.py).
#
# Usage (as root): IPSW=<dir with the three 20H392 .dmg> OUT=<output dir> mkiosroot-root.sh
# The .dmg names are the iPhone X (iPhone10,3/iPhone10,6) iOS 16.7.16 IPSW's OS, SystemOS and AppOS
# images; `unixnu fetch` downloads and unpacks that IPSW (unixnu/out/ipsw/20H392).
set -eu
IPSW=${IPSW:?set IPSW to the unpacked 20H392 IPSW directory}
OUT=${OUT:?set OUT to an output directory with ~18 GB free}
TOOLS=$(cd "$(dirname "$0")" && pwd)
W=$OUT/mnt
IMG=$OUT/ios-root.sparseimage
[ ! -e "$IMG" ] || { echo "refusing to overwrite $IMG"; exit 1; }
mkdir -p "$W/os" "$W/sysos" "$W/appos" "$W/root"
cleanup() { for m in root appos sysos os; do hdiutil detach -quiet "$W/$m" 2>/dev/null || true; done; }
trap cleanup EXIT
att() { hdiutil attach -readonly -nobrowse -owners on -noverify -noautofsck -mountpoint "$W/$2" "$1" | tail -1; }
att "$IPSW/087-86698-044.dmg" os
att "$IPSW/087-86593-047.dmg" sysos
att "$IPSW/087-86715-047.dmg" appos
hdiutil create -size 17500m -type SPARSE -layout NONE -fs "Case-sensitive Journaled HFS+" -volname iOSRoot "$OUT/ios-root"
hdiutil attach -nobrowse -owners on -noverify -mountpoint "$W/root" "$IMG" | tail -1
mkdir -p "$W/root/.fseventsd" && touch "$W/root/.fseventsd/no_log" "$W/root/.metadata_never_index"
copy() { echo "$(date +%T) copy $1 -> $2"; mkdir -p "$2"; ditto --nopreserveHFSCompression --noqtn "$1" "$2"; }
copy "$W/os" "$W/root"
copy "$W/sysos" "$W/root/private/preboot/Cryptexes/OS"
copy "$W/appos" "$W/root/private/preboot/Cryptexes/App"
echo "$(date +%T) compressed files left: $(find -x "$W/root" -flags +compressed | wc -l)"
echo "launchd owner: $(stat -f '%u:%g %Sp' "$W/root/sbin/launchd")"
/usr/bin/python3 "$TOOLS/stage-ios-root.py" "$W/root"
rm -rf "$W/root/.fseventsd" "$W/root/.metadata_never_index" "$W/root/.Spotlight-V100" "$W/root/.Trashes"
df -h "$W/root" | tail -1
hdiutil detach "$W/root"
DEV=$(hdiutil attach -nomount -noverify "$IMG" | awk '{print $1}' | head -1)
fsck_hfs -fn "$DEV" || true
hdiutil detach "$DEV"
chown "${SUDO_UID:-0}:${SUDO_GID:-0}" "$IMG"
echo "$(date +%T) done"
