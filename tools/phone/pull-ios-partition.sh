#!/bin/bash
# Copy the whole "ios" partition (HFS+) off the phone, read-only, gzip -1 (Android's zstd rejects stdin).
# For scripts/read-ioslog.sh. Takes ~20 minutes for 17 GiB over USB 2.
#   pull-ios-partition.sh <out.img.gz>
set -euo pipefail
. "$(dirname "$0")/common.sh"
adb_root
start=$(date +%s)
"$ADB" exec-out "dd if=$IOS_DEV bs=4194304 2>/dev/null | gzip -1" > "$1"
echo "$(wc -c < "$1") bytes in $(( $(date +%s) - start )) s"
gzip -t "$1" && echo "gzip ok"
