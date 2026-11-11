#!/usr/bin/env python3
"""Compile a device XNU driver against the recorded source-kernel ABI.

Same recipe as the base tree's build-arm-driver.py: replay the compile command
XNU recorded for IOPlatformExpert, swap in the driver source, and check that the
object really is an ARM64 Mach-O object. The differences are the component list
(which also covers this device's drivers) and the archive step.

Run it from the build tree; `unixnu build kernel` does that for every entry of
[kernel] archives in board.toml.
"""
import argparse
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# component -> sources, output directory, extra include directories
COMPONENTS = {
    "ArmDTPlatform": {
        "sources": ["drivers/ArmDTPlatform/ArmDTPlatform.cpp"],
        "out": "armdt-platform",
    },
    "ArmCpu": {
        "sources": ["drivers/ArmCpu/ArmCpu.cpp", "drivers/NativeGicV2/GicV2Core.cpp"],
        "out": "armcpu",
        "include": ["drivers/NativeGicV2"],
    },
    "NativeSdhci": {
        "sources": ["drivers/NativeSdhci/NativeSdhci.cpp", "drivers/NativeSdhci/SdhciCore.cpp",
                    "drivers/NativeSdhci/PollingHost.cpp"],
        "out": "native-sdhci",
        # The storage ABI headers are prepared from IOStorageFamily by the kernel step;
        # osfmk is where the recorded recipe resolves <mach/...> from.
        "include": ["drivers/NativeSdhci", "build/storage-abi/include", ".cache/xnu-work/osfmk"],
    },
    "NativeAES16": {
        # upstream NativeAES on the iOS 16.7.16 IOAESAccelerator ABI, with derived per-device keys for
        # hardware-key selectors; scripts/build-native-aes16.py adds its original bindings to the archive.
        "sources": ["drivers/NativeAES16/NativeAES.cpp"],
        "out": "native-aes16",
        "include": ["drivers/NativeAES16"],
    },
    "NativeAdreno418": {
        # Rung 1 of the GPU: maps the KGSL and MMSS windows, checks the GDSC state
        # and reads the revision registers. The cores hold the decoding, the ring
        # arithmetic and the command stream; the driver calls the first of those
        # only, but all of them are compiled here so a core that stops being valid
        # ARM64 code fails in this archive rather than the day it is wired.
        "sources": ["drivers/NativeAdreno418/NativeAdreno418.cpp",
                    "drivers/NativeAdreno418/AdrenoCore.cpp",
                    "drivers/NativeAdreno418/IommuCore.cpp",
                    "drivers/NativeAdreno418/CommandStream.cpp",
                    "drivers/NativeAdreno418/GpuIo.cpp",
                    "drivers/NativeAdreno418/LpaeCore.cpp",
                    "drivers/NativeAdreno418/SmmuContext.cpp",
                    "drivers/NativeAdreno418/Submission.cpp",
                    "drivers/NativeAdreno418/RpmCore.cpp",
                    "drivers/NativeAdreno418/SmdPipe.cpp",
                    "drivers/NativeAdreno418/SmdMemory.cpp",
                    "drivers/NativeAdreno418/RpmVotes.cpp",
                    "drivers/NativeAdreno418/PowerController.cpp",
                    "drivers/NativeAdreno418/SpmiCore.cpp",
                    "drivers/NativeAdreno418/PinnedGraphics.cpp",
                    "drivers/NativeAdreno418/A418Init.cpp",
                    "drivers/NativeAdreno418/SmmuRoutes.cpp",
                    "drivers/NativeAdreno418/GpuBatch.cpp",
                    "drivers/NativeAdreno418/PlatformCheck.cpp"],
        "out": "native-adreno418",
        "include": ["drivers/NativeAdreno418"],
    },
    "NativeMdss": {
        # Rung 1 of the display: maps the MDP, MMSS, DSI0 and PHY windows, proves
        # the mdss power domain is on, and logs what the firmware left scanning.
        # It publishes no framebuffer, so nothing draws into a surface nothing
        # scans out.
        "sources": ["drivers/NativeMdss/NativeMdss.cpp",
                    "drivers/NativeMdss/MdssCore.cpp"],
        "out": "native-mdss",
        "include": ["drivers/NativeMdss"],
    },
    "NativeLegacyDisplay": {
        # Not a matched driver: it exists so the kernel link resolves the probe symbol
        # that upstream's bsd_init.c calls without an XNU_NATIVE_LEGACY_DISPLAY guard.
        "sources": ["drivers/NativeLegacyDisplay/NativeLegacyDisplay.cpp"],
        "out": "native-legacy-display",
    },
    "NativeKeyStoreEvents": {
        # Upstream's AppleKeyStore stand-in (no keybag, no passcode, unlocked): keybagd --init needs
        # aks_get_system to succeed or it reboots into recovery. Its hooks and the runtime personality
        # live in the zzv overlay patch (IOStartIOKit.cpp).
        "sources": ["drivers/NativeKeyStoreEvents/NativeKeyStoreEvents.cpp"],
        "out": "native-keystore",
    },
    "NativeWatchdog": {
        # IOWatchdog stand-in for watchdogd (no SoC watchdog, no Apple PMU); personality in zzv.
        "sources": ["drivers/NativeWatchdog/NativeWatchdog.cpp"],
        "out": "native-watchdog",
    },
    "NativeHID": {
        # backboardd's HID event services (touch screen, buttons): Apple's IOHIDFamily-1915 event,
        # queue and user client sources under a minimal IOHIDEventService, ported from ios-sweet
        # (MIT); personalities in zzv. Hardware drivers feed native_hid_touch() / native_hid_key().
        "sources": ["drivers/NativeHID/NativeHID.cpp", "drivers/NativeHID/IOHIDEvent.cpp",
                    "drivers/NativeHID/IOHIDEventServiceQueue.cpp",
                    "drivers/NativeHID/IOHIDEventServiceUserClient.cpp"],
        "out": "native-hid",
        "include": ["drivers/NativeHID"],
    },
    "NativeTouch": {
        # Touchscreen (Synaptics RMI4 on BLSP1 QUP1) and the side keys (PM8994 PON + GPIOs over SPMI),
        # polled into NativeHID; personality in zzv.
        "sources": ["drivers/NativeTouch/NativeTouch.cpp"],
        "out": "native-touch",
    },
    "NativeCpuClock": {
        # Read-only CPU clock readout: cluster PLLs and muxes, and timed adds per core; personality in zzv.
        "sources": ["drivers/NativeCpuClock/NativeCpuClock.cpp"],
        "out": "native-cpu-clock",
    },
    "NativeAdreno430": {
        # Xperia Z5 Compact GPU, bring-up: microcode, ring and a CP_MEM_WRITE proof; personality in zzv.
        "sources": ["drivers/NativeAdreno430/NativeAdreno430.cpp"],
        "out": "native-adreno430",
        "include": ["drivers/NativeAdreno430"],
    },
    "NativeBattery": {
        # powerd's IOPMPowerSource: charge level and charger state from the PMI8994; personality in zzv.
        "sources": ["drivers/NativeBattery/NativeBattery.cpp"],
        "out": "native-battery",
    },
    "NativeDisplay": {
        # IOSurface (IOCoreSurfaceRoot) + IOMobileFramebuffer (AppleCLCD) on the GOP framebuffer,
        # ported from ios-sweet (MIT); personalities in zzv.
        "sources": ["drivers/NativeDisplay/NativeSurface.cpp", "drivers/NativeDisplay/NativeDisplay.cpp"],
        "out": "native-display",
        "include": ["drivers/NativeDisplay"],
    },
    "NativeRamBlock": {
        "sources": ["drivers/NativeRamBlock/NativeRamBlock.cpp"],
        "out": "native-ram-block",
        "include": ["build/storage-abi/include"],
    },
}

RECIPE = ROOT / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json"

MH_MAGIC_64 = 0xFEEDFACF
CPU_TYPE_ARM64 = 0x0100000C
MH_OBJECT = 1


def check_object(path):
    """The three fields a driver object must have to link into this kernel."""
    data = path.read_bytes()[:32]
    if len(data) < 32:
        raise SystemExit(f"{path}: shorter than a Mach-O header")
    magic, cputype, _subtype, filetype = struct.unpack("<IIII", data[:16])
    if magic != MH_MAGIC_64:
        raise SystemExit(f"{path}: magic 0x{magic:08x}, expected 0x{MH_MAGIC_64:08x}")
    if cputype != CPU_TYPE_ARM64:
        raise SystemExit(f"{path}: cputype 0x{cputype:08x}, expected 0x{CPU_TYPE_ARM64:08x}")
    if filetype != MH_OBJECT:
        raise SystemExit(f"{path}: filetype {filetype}, expected {MH_OBJECT}")


def build_command(record, source, destination, include_dirs):
    command = []
    arguments = record["arguments"]
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value == record["file"]:
            command.append(str(source))
        elif value == "-o":
            command.extend(["-o", str(destination)])
            index += 1
        elif value == "-Xclang" and arguments[index + 1].startswith("-W"):
            index += 1
        elif not value.startswith("-W"):
            command.append(value)
        index += 1

    command.extend([
        "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
        "-Wno-c99-designator", "-Wno-reorder-init-list",
        "-Wno-deprecated-enum-enum-conversion", "-Wno-unnecessary-virtual-specifier",
    ])
    for directory in include_dirs:
        command.extend(["-I", str(directory)])
    return command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("component", choices=sorted(COMPONENTS))
    args = parser.parse_args()

    spec = COMPONENTS[args.component]
    if not RECIPE.is_file():
        raise SystemExit(
            f"No compile record at {RECIPE}. Prepare the XNU build with Xcode and the "
            "iPhoneOS SDK first; freestanding core builds do not provide the IOKit ABI recipe."
        )

    record = json.loads(RECIPE.read_text().strip().rstrip(","))
    output = ROOT / "build" / spec["out"]
    output.mkdir(parents=True, exist_ok=True)
    include_dirs = [ROOT / directory for directory in spec.get("include", [])]

    objects = []
    for source_name in spec["sources"]:
        source = ROOT / source_name
        if not source.is_file():
            raise SystemExit(f"{source} is missing: is the module linked into the build tree?")
        if args.component == "ArmCpu" and source.name == "ArmCpu.cpp":
            generated = output / "generated/ArmCpu.cpp"
            subprocess.run([sys.executable, str(ROOT / "scripts/prepare-armcpu.py"),
                            str(source), str(generated)], check=True)
            source = generated
        destination = output / (source.stem + ".o")
        command = build_command(record, source, destination, include_dirs)
        subprocess.run(command, cwd=record["directory"], check=True)
        check_object(destination)
        objects.append(destination)

    if shutil.which("ar") is None:
        raise SystemExit("ar is not on PATH")
    archive = output / f"lib{args.component}.a"
    subprocess.run(["ar", "rcs", str(archive)] + [str(o) for o in objects], check=True)
    print(f"{args.component}: {archive}")
    print(f"  link with  -Wl,-force_load,{archive}")


if __name__ == "__main__":
    sys.exit(main())
