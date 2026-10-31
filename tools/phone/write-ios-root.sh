#!/bin/bash
# Write the iOS root image (scripts/mkiosroot-root.sh) to the phone's "ios" partition and verify it.
#   write-ios-root.sh <ios-root.sparseimage> [--no-verify]
# The stream goes through `adb shell -T` (no pty): with `adb exec-in` toybox gzip saw a TTY on stdin and
# lost data. Verification re-reads the written range on the phone (~11 MB/s: about 25 minutes).
set -euo pipefail
. "$(dirname "$0")/common.sh"
IMG=$1
RAW=${IMG%.sparseimage}.raw
[ -f "$RAW.cdr" ] || hdiutil convert "$IMG" -format UDTO -o "$RAW" >/dev/null
RAW=$RAW.cdr
SIZE=$(wc -c < "$RAW" | tr -d ' ')
adb_root
PART=$("$ADB" shell "blockdev --getsize64 $IOS_DEV" | tr -d '\r')
[ "$SIZE" -le "$PART" ] || { echo "image ($SIZE bytes) is larger than $IOS_DEV ($PART bytes)"; exit 1; }
echo "writing $SIZE bytes to $IOS_DEV ($PART bytes)"
gzip -1 -c "$RAW" | "$ADB" shell -T "gzip -dc | dd of=$IOS_DEV bs=4194304 2>/dev/null; sync"
[ "${2:-}" = --no-verify ] && exit 0
LOCAL=$(shasum -a 256 "$RAW" | cut -d' ' -f1)
PHONE=$("$ADB" shell "head -c $SIZE $IOS_DEV | sha256sum" </dev/null | cut -d' ' -f1)
echo "local $LOCAL"; echo "phone $PHONE"
[ "$LOCAL" = "$PHONE" ] && echo "verified" || { echo "MISMATCH"; exit 1; }
