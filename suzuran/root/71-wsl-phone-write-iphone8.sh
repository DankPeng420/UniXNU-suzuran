#!/bin/bash
# Run inside WSL with the phone in TWRP, MTP DISABLED:  bash /mnt/c/Users/<you>/Desktop/suzuran-port/root/71-wsl-phone-write-iphone8.sh
# Writes the iPhone 8 root (~/iosbuild8/ios-root.img, from 70-wsl-iphone8.sh) over the 'ios' partition with
# 66-wsl-phone-write.sh (same checks, only partition 42, read-back sha256). Replaces the iPhone X root on the phone.
D=$(cd "$(dirname "$0")" && pwd)
IMG=~/iosbuild8/ios-root.img SHA="$D/out8/ios-root.img.sha256" exec bash "$D/66-wsl-phone-write.sh"
