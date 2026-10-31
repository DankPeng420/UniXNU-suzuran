#!/bin/bash
# Pull the kernel console log NativeSdhci keeps at the end of the "ios" partition (header + 32 MiB ring,
# written once a second while iOS runs) for scripts/read-natlog.py.
#   pull-natlog.sh <out.bin>
set -euo pipefail
. "$(dirname "$0")/common.sh"
adb_root
N=$("$ADB" shell "blockdev --getsz $IOS_DEV" | tr -d '\r')
SKIP=$((N - 65537))
"$ADB" exec-out "dd if=$IOS_DEV bs=512 skip=$SKIP count=65537 2>/dev/null" > "$1"
ls -l "$1"
