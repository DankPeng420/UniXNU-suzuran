#!/usr/bin/env bash
# Build the Lumia 950 chainload payload with EDK II and stage it in the EFIESP layout.
#
#   UNIXNU_SOURCE=/path/to/UniXNU UNIXNU_EDK2=/path/to/mu_basecore \
#     bash scripts/build-payload.sh [OUT]
#
# OUT defaults to out/payload and holds what goes on the phone:
#
#   OUT/EFI/UniXNU/UniXNU.efi              the menu, started from the Lumia boot manager
#   OUT/EFI/UniXNU/unixnu.toml             its configuration
#   OUT/EFI/UniXNU/Drivers/DarwinBoot.efi  the loader; scripts/stage-kernel.py puts it in the
#                                          config and stages a kernel beside it
#
# Copy that tree over \EFI on EFIESP (or onto the microSD card). This does not touch the
# Lumia's own UEFI: that firmware stays as Lumia950XLPkg built it, and only starts our
# application, which is why nothing here can brick the phone.
#
# Requirements: an edk2 or Mu_Basecore checkout with BaseFdtLib and MipiSysTLib submodules,
# clang and lld for CLANGPDB, and the UniXNU DarwinBoot submodule.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
UNIXNU=${UNIXNU_SOURCE:-$ROOT/../unixnu}
EDK2=${UNIXNU_EDK2:-}
OUT=${1:-$ROOT/out/payload}
TARGET=${TARGET:-DEBUG}
TOOLCHAIN=${TOOLCHAIN:-CLANGPDB}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
PYTHON=${PYTHON:-python3}

if [ -z "$EDK2" ] || [ ! -f "$EDK2/MdePkg/MdePkg.dec" ]; then
  echo "error: set UNIXNU_EDK2 to an edk2 or Mu_Basecore checkout (MdePkg not found)" >&2
  exit 1
fi
DARWINBOOT=$UNIXNU/drivers/DarwinBoot
if [ ! -f "$DARWINBOOT/DarwinBootPkg/DarwinBoot.dsc" ]; then
  echo "error: $DARWINBOOT is empty; in $UNIXNU run:" >&2
  echo "  git submodule update --init --depth 1 drivers/DarwinBoot" >&2
  exit 1
fi

# The loader has to be the one this repository stages for: it reads the kernel from whichever
# volume holds it, so Payloads/ can live on MainOS while the menu stays on EFIESP.
# ci/darwinboot-payload-volume.patch carries that change until it is upstream; a checkout that
# already has it makes the apply a no-op, so building twice is safe.
if git -C "$DARWINBOOT" apply --reverse --check "$ROOT/ci/darwinboot-payload-volume.patch" >/dev/null 2>&1; then
  echo "== DarwinBoot: payload-volume change already there"
else
  git -C "$DARWINBOOT" apply "$ROOT/ci/darwinboot-payload-volume.patch"
  echo "== DarwinBoot: applied ci/darwinboot-payload-volume.patch"
fi

if git -C "$DARWINBOOT" apply --reverse --check "$ROOT/ci/darwinboot-handoff-label.patch" >/dev/null 2>&1; then
  echo "== DarwinBoot: accurate handoff label already there"
else
  git -C "$DARWINBOOT" apply "$ROOT/ci/darwinboot-handoff-label.patch"
  echo "== DarwinBoot: label distinguishes loader handoff from XNU execution"
fi

# A root ramdisk too large for EFIESP is staged beside the kernel as <kernel>.ramdisk, and the
# loader reads it from the kernel's volume when the menu passes no initrd.
if git -C "$DARWINBOOT" apply --reverse --check "$ROOT/ci/darwinboot-payload-ramdisk.patch" >/dev/null 2>&1; then
  echo "== DarwinBoot: payload ramdisk change already there"
else
  git -C "$DARWINBOOT" apply "$ROOT/ci/darwinboot-payload-ramdisk.patch"
  echo "== DarwinBoot: reads <kernel>.ramdisk beside the kernel"
fi

# The iOS 16 IPSWs ship version-1 trust caches (22-byte entries); the validator took only v2.
if git -C "$DARWINBOOT" apply --reverse --check "$ROOT/ci/darwinboot-trustcache-v1.patch" >/dev/null 2>&1; then
  echo "== DarwinBoot: version-1 trust caches already accepted"
else
  git -C "$DARWINBOOT" apply "$ROOT/ci/darwinboot-trustcache-v1.patch"
  echo "== DarwinBoot: accepts version-1 trust caches"
fi

echo "== UniXNU and UniXNUDxe"
UNIXNU_EDK2=$EDK2 JOBS=$JOBS PYTHON_COMMAND=$PYTHON "$UNIXNU/package/build.sh" -b "$TARGET" -t "$TOOLCHAIN"

echo "== DarwinBoot"
# DarwinBootPkg has its own DSC, so it builds without a firmware volume to be part of. The
# CONF files and BaseTools wrappers are what package/build.sh sets up for UniXNUPkg; the
# toolchain rejects a build started without them.
# DarwinBoot declares UniXNUPkg for the loader protocol GUID, so the package tree joins the
# packages path as well; without it the build stops at DarwinBoot.inf with
# "UniXNUPkg/UniXNUPkg.dec is not found in packages path".
export WORKSPACE=$DARWINBOOT
export PACKAGES_PATH=$WORKSPACE:$UNIXNU/package:$EDK2
export EDK_TOOLS_PATH=$EDK2/BaseTools
export CONF_PATH=$WORKSPACE/Build/Conf
mkdir -p "$CONF_PATH"
for f in target tools_def build_rule; do
  if [ ! -f "$CONF_PATH/$f.txt" ] || [ "$EDK_TOOLS_PATH/Conf/$f.template" -nt "$CONF_PATH/$f.txt" ]; then
    cp "$EDK_TOOLS_PATH/Conf/$f.template" "$CONF_PATH/$f.txt"
  fi
done
if [ -z "${CLANG_BIN:-}" ]; then
  CLANG=$(command -v clang) || { echo "error: clang not found" >&2; exit 1; }
  CLANG_BIN=$(dirname "$CLANG")/
fi
export CLANG_BIN
export PATH=$EDK_TOOLS_PATH/BinWrappers/PosixLike:$EDK_TOOLS_PATH/Source/C/bin:$PATH
# -m builds only the loader: the DSC also lists the validation application, which is a test.
build -p DarwinBootPkg/DarwinBoot.dsc -a AARCH64 -b "$TARGET" -t "$TOOLCHAIN" -n "$JOBS" \
  -m DarwinBootPkg/Driver/DarwinBoot.inf

echo "== stage"
# The menu goes in twice on purpose. UniXNU reads its config from \EFI\UniXNU, but nothing in
# the Lumia firmware runs an application from there by name: BootSplashApp boots the first
# \EFI\BOOT\BOOTAA64.EFI it can load from any volume it has mounted and only falls back to
# the boot manager menu when that fails, so the removable-media name is the entry point.
"$PYTHON" "$UNIXNU/package/Scripts/stage-esp.py" -o "$OUT" "$ROOT/payload" \
  --put "$UNIXNU/package/Build/Out/AARCH64-$TARGET/UniXNU.efi=EFI/UniXNU/UniXNU.efi" \
  --put "$UNIXNU/package/Build/Out/AARCH64-$TARGET/UniXNU.efi=EFI/BOOT/BOOTAA64.EFI" \
  --put "$WORKSPACE/build/DarwinBoot/${TARGET}_$TOOLCHAIN/AARCH64/DarwinBoot.efi=EFI/UniXNU/Drivers/DarwinBoot.efi"

echo "== check"
"$PYTHON" "$ROOT/scripts/check-payload.py" --config "$OUT/EFI/UniXNU/unixnu.toml" --payload-dir "$OUT"

echo "== output"
if command -v sha256sum >/dev/null 2>&1; then
  find "$OUT" -type f -print0 | sort -z | xargs -0 sha256sum
else
  find "$OUT" -type f -print0 | sort -z | xargs -0 shasum -a 256
fi
