#!/bin/bash
# Copy a staged kernel (scripts/stage-kernel.py --payload-out DIR) and its boot menu configuration to the
# phone: DIR/EFI/UniXNU/Payloads/* to /cache, the unixnu.toml to the EFIESP. Checks the copies by md5.
#   push-kernel.sh <payload-out dir> <unixnu.toml>
set -euo pipefail
. "$(dirname "$0")/common.sh"
P=$1/EFI/UniXNU/Payloads
TOML=$2
for f in kernel.macho kernel.macho.adt kernel.macho.trustcache; do test -f "$P/$f"; done
adb_root
"$ADB" shell "mkdir -p $PAYLOAD_DIR"
for f in kernel.macho kernel.macho.adt kernel.macho.trustcache; do "$ADB" push "$P/$f" "$PAYLOAD_DIR/$f" >/dev/null; done
"$ADB" push "$TOML" /data/local/tmp/unixnu.toml >/dev/null
with_esp rw "cp /data/local/tmp/unixnu.toml \$E/EFI/UniXNU/unixnu.toml; md5sum \$E/EFI/UniXNU/unixnu.toml"
"$ADB" shell "md5sum $PAYLOAD_DIR/kernel.macho $PAYLOAD_DIR/kernel.macho.adt $PAYLOAD_DIR/kernel.macho.trustcache"
echo "local:"; (cd "$P" && md5sum kernel.macho kernel.macho.adt kernel.macho.trustcache 2>/dev/null || md5 -r kernel.macho kernel.macho.adt kernel.macho.trustcache)
md5sum "$TOML" 2>/dev/null || md5 -r "$TOML"
