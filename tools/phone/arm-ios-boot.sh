#!/bin/bash
# Boot iOS once: put a BCD whose {bootmgr} bootsequence points at the UEFI entry (BootShim -> UEFI.elf ->
# \EFI\BOOT\BOOTAA64.EFI = UniXNU menu) on the EFIESP, then reboot. Windows Boot Manager removes a
# one-time bootsequence after using it, so the boot after iOS goes back to the default (Android).
#   arm-ios-boot.sh <BCD with the one-shot bootsequence>     (see docs/lumia950xl/INSTALL.md, "BCD")
set -euo pipefail
. "$(dirname "$0")/common.sh"
adb_root
"$ADB" push "$1" /data/local/tmp/bcd.oneshot >/dev/null
with_esp rw "cp /data/local/tmp/bcd.oneshot \$E/EFI/Microsoft/BOOT/BCD; md5sum \$E/EFI/Microsoft/BOOT/BCD"
"$ADB" reboot
date +%T
