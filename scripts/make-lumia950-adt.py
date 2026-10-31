#!/usr/bin/env python3
"""Generate the MSM8992 Apple device tree (ADT) for the Lumia 950.

This is the board script named by `[payload.adt] tool` in board.toml. It writes
the sidecar DarwinBoot copies next to the kernel as `kernel.macho.adt` and the
tree ArmDTPlatform parses.

Two things are borrowed rather than invented:

  * The node encoding is Apple's, byte for byte the one in
    `scripts/qemu-entry/make-dt.py`: u32 property count, u32 child count, then
    for each property a 32-byte NUL-padded name, a u32 length and the value
    padded to four bytes, then the children.
  * The cell layout is that same fixture's: two address cells and two size
    cells at the root and under `arm-io`, with each address and size stored as
    a native-endian u64. That fixture is the only ADT this project has watched
    a real XNU platform expert accept (`scripts/test-armdt-platform.py`), so it
    is the layout to follow until a kernel here says otherwise.

What the tree says about the hardware is taken from the register map in
README.md and from the MSM8992 descriptions used by mainline Linux. What is
*not* known is recorded rather than papered over:

  * `intc` describes the real GICv2 (distributor `0xf9000000`, CPU interface
    `0xf9002000`). The hash-qualified ArmCpu GICv2 integration is now written,
    with standalone QEMU IRQ tests. XNU and handset validation remain pending.
  * `psci` says `arm,psci-1.0` because that is the compatible XNU's ARM
    platform expects, but MSM8992 implements a subset over HVC with no
    SYSTEM_OFF and no SYSTEM_RESET. See README.md, blocker 4.
  * The display node matches NativeMdss's register-identity driver, not a
    framebuffer provider or a working iOS display.
  * The GPU node is emitted because a driver now reads it (`NativeAdreno418`,
    rung 1). Its window and the MMSS window beside it are from Qualcomm's
    msm-3.10 device tree, not from mainline: see drivers/NativeAdreno418/
    BoardConstants.h for the sources of every value.
  * `reg` windows are described, but whether the firmware left them where the
    SoC map says is a question for the driver, not for this file.

The tree is deterministic except for `random-seed`, which is fresh on every run
(the payload contract wants that).
"""
from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
import uuid
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# MSM8992 (Snapdragon 808) constants. README.md, "The ADT node" and the
# register table, carries the sources for the SDHCI windows.
DRAM_BASE = 0
TIMEBASE_FREQUENCY = 19_200_000
GIC_DISTRIBUTOR = (0xF9000000, 0x1000)
GIC_CPU_INTERFACE = (0xF9002000, 0x1000)
SDHCI_WINDOWS = ((0xF9824900, 0x1A0), (0xF9824000, 0x800))
SDHCI_INTERRUPTS = (123, 138)
# The GPU. Qualcomm's msm-3.10 device tree for this SoC (msm8992-gpu.dtsi and
# msm-gdsc.dtsi) is the source: kgsl-3d0@0xfdb00000, GIC SPI 33, and the OxiLi
# power domains in the MMSS clock controller at 0xfd8c0000. The driver maps both
# windows, so both are reg entries here.
KGSL_WINDOWS = ((0xFDB00000, 0x40000), (0xFD8C0000, 0x10000))
KGSL_INTERRUPT = 33
GPU_CHIP_ID = 0x04010800
# The display. Same tree: msm8992-mdss.dtsi gives mdp_phys, the DSI0 windows and
# mmss_misc_phys; gdsc_mdss lives in the MMSS clock controller at 0xfd8c2304,
# which is why the second window is 64 KiB and not the 4 KiB of the GPU node.
# The driver maps all five by index, so the order here is the driver's order.
MDSS_WINDOWS = (
    (0xFD900000, 0x90000),   # mdp_phys
    (0xFD8C0000, 0x10000),   # MMSS clock controller (gdsc_mdss)
    (0xFD994000, 0x260),     # dsi_ctrl
    (0xFD994500, 0x280),     # dsi_phy
    (0xFD828000, 0x108),     # mmss_misc_phys
)
MDSS_INTERRUPT = 83
# The canvas iOS renders, and its scale: the iPhone X's own 1125x2436 at 3x. A 2x canvas would draw
# far fewer pixels (CoreAnimation renders in software, and the framebuffer shows the canvas at
# 886x1920 anyway), but boot 18 (750x1624 at 2x) showed what stands in the way: the icons are cached
# only at 3x, and rendering new ones crashes (IconServices' CIContext falls back to an OpenGL ES
# context, whose GLContext::init calls through a null pointer: no GPU driver), so Settings and apps
# die; 375x812 points at 2x also has no status bar visual provider (rdar://45025538). Revisit once
# CoreImage renders on the CPU; the points must then be a real iPhone's (414x896 at 2x = 828x1792).
CANVAS = (1125, 2436)
CANVAS_SCALE = 3
# MPIDR affinities from Lumia950XLPkg's MSM8992 MADT: four A53s, then two A57s.
DEFAULT_CPU_REGS = "0,1,2,3,0x100,0x101"
CPU_COMPATIBLES = {
    0: "arm,cortex-a53", 1: "arm,cortex-a53", 2: "arm,cortex-a53", 3: "arm,cortex-a53",
    0x100: "arm,cortex-a57", 0x101: "arm,cortex-a57",
    # MSM8994 (Lumia 950 XL) has two more A57s in the same cluster.
    0x102: "arm,cortex-a57", 0x103: "arm,cortex-a57",
}
NVRAM_BANK_SIZE = 8192
NVRAM_COMMON_DATA = 2048

CHRP_SIGNATURE = 0x70
CHRP_BLOCK = 0x10

OSENVIRONMENTS = ("main", "recoveryos", "ramdisk")
CRYPTO_HASH_METHODS = ("sha2-384", "sha1")


def node(name: str, properties=(), children=()) -> bytes:
    """Encode one Apple device tree node."""
    if not 0 < len(name.encode()) < 32:
        raise ValueError(f"node name {name!r} does not fit the 32-byte field")
    encoded = [("name", name.encode() + b"\0"), *properties]
    seen = set()
    for key, _ in encoded:
        if len(key.encode()) >= 32:
            raise ValueError(f"property name {key!r} does not fit the 32-byte field")
        if key in seen:
            raise ValueError(f"duplicate property {key!r} on node {name!r}")
        seen.add(key)
    result = struct.pack("<II", len(encoded), len(children))
    for key, data in encoded:
        result += struct.pack("<32sI", key.encode(), len(data))
        result += data + bytes(-len(data) % 4)
    return result + b"".join(children)


def parse_node(data: bytes, at: int = 0):
    """Decode one node of an Apple flattened device tree: (name, properties, children, end).
    A property's length carries iBoot's placeholder flag in bit 31."""
    nprops, nchildren = struct.unpack_from("<II", data, at)
    at += 8
    name, props = "", []
    for _ in range(nprops):
        key, length = struct.unpack_from("<32sI", data, at)
        at += 36
        size = length & 0x7FFFFFFF
        value = data[at:at + size]
        at += size + (-size % 4)
        key = key.split(b"\0", 1)[0].decode()
        if key == "name":
            name = value.split(b"\0", 1)[0].decode()
        else:
            props.append((key, value, bool(length >> 31)))
    children = []
    for _ in range(nchildren):
        child = parse_node(data, at)
        children.append(child)
        at = child[3]
    return name, props, children, at


def encode_parsed(parsed) -> bytes:
    """Re-encode a parsed node without iBoot's placeholders (syscfg/... values it would fill in)."""
    name, props, children, _ = parsed
    return node(name, [(key, value) for key, value, placeholder in props if not placeholder],
                [encode_parsed(child) for child in children])


def iphone_identity(path: Path):
    """Root identity and /product of the real device tree of the iPhone whose userland runs here.
    MobileGestalt takes product type, device class, the home-button-less UI and artwork from them;
    with the Lumia's own identity it treats the phone as a Home-button iPod."""
    tree = parse_node(path.read_bytes())
    root = {key: value for key, value, placeholder in tree[1] if not placeholder}
    product = next((child for child in tree[2] if child[0] == "product"), None)
    if product is None or "compatible" not in root or "model" not in root:
        raise SystemExit(f"{path}: no compatible/model or /product; is it an Apple device tree?")
    return root, encode_parsed(without_flash(product))


def without_flash(product):
    """/product with the iPhone's camera flash removed. MobileGestalt's camera-flash comes from
    /product/camera/flash; with it, SpringBoard's lock screen builds an AVFlashlight, whose sync XPC
    to mediaserverd's flashlight server never returns here (no camera stack), and the startup
    transition to the lock screen waits on it forever (boot 4, 2026-10-31)."""
    name, props, children, end = product
    children = [(child[0], [p for p in child[1] if child[0] != "camera" or
                            p[0] not in ("flash", "front-flash-capability")], child[2], child[3])
                for child in children]
    return name, props, children, end


def u32(value: int) -> bytes:
    return struct.pack("<I", value & 0xFFFFFFFF)


def u64(value: int) -> bytes:
    return struct.pack("<Q", value & 0xFFFFFFFFFFFFFFFF)


def string(*values: str) -> bytes:
    """Apple property holding one NUL-terminated string per value."""
    return b"".join(value.encode() + b"\0" for value in values)


def chrp_header(name: bytes, size: int) -> bytearray:
    """A CHRP NVRAM header whose length covers `size` bytes in 16-byte blocks."""
    if size % CHRP_BLOCK:
        raise ValueError("CHRP partitions are a whole number of 16-byte blocks")
    raw = bytearray(struct.pack("<BBH12s", CHRP_SIGNATURE, 0, size // CHRP_BLOCK, name))
    checksum = sum(raw)
    while checksum > 0xFF:
        checksum = (checksum & 0xFF) + (checksum >> 8)
    raw[1] = checksum
    return raw


def empty_nvram_proxy() -> list[tuple[str, bytes]]:
    """A volatile CHRP bank: no controller, nothing persisted.

    The layout is XNU's (iokit/Kernel/IONVRAMCHRPHandler.cpp): a 16-byte
    `nvram` header, the 16-byte Apple header (Adler-32 over everything from the
    generation field, then a generation and padding), a `common` partition with
    2 KiB of data, and a `free` partition filling the rest of the bank.
    """
    image = bytearray(NVRAM_BANK_SIZE)
    image[:16] = chrp_header(b"nvram", 32)
    image[32:48] = chrp_header(b"common", 16 + NVRAM_COMMON_DATA)
    free_at = 32 + 16 + NVRAM_COMMON_DATA
    image[free_at:free_at + 16] = chrp_header(b"free", len(image) - free_at)
    struct.pack_into("<I", image, 16, zlib.adler32(image[20:]) & 0xFFFFFFFF)
    return [
        ("nvram-bank-size", u32(len(image))),
        ("nvram-bank-count", u32(1)),
        ("nvram-current-bank", u32(0)),
        ("nvram-proxy-data", bytes(image)),
    ]


def parse_cpu_regs(value: str) -> list[int]:
    try:
        regs = [int(item, 0) for item in value.split(",") if item.strip()]
    except ValueError as error:
        raise SystemExit(f"--cpu-regs expects comma-separated integers: {error}") from None
    if not regs:
        raise SystemExit("--cpu-regs needs at least one affinity value")
    if len(regs) != len(set(regs)):
        raise SystemExit("--cpu-regs repeats an affinity value")
    if any(not 0 <= reg < 1 << 24 for reg in regs):
        raise SystemExit("--cpu-regs values must fit the 32-bit reg property")
    return regs


def cpu_node(index: int, reg: int) -> bytes:
    if reg not in CPU_COMPATIBLES:
        raise ValueError(f"unsupported MSM8992 CPU affinity {reg:#x}")
    return node(
        f"cpu{index}",
        [
            ("state", string("running" if index == 0 else "waiting")),
            ("reg", u32(reg)),
            ("device_type", string("cpu")),
            ("compatible", string(CPU_COMPATIBLES[reg])),
            ("timebase-frequency", u32(TIMEBASE_FREQUENCY)),
        ],
    )


def build_tree(
    *,
    dram_size: int,
    dram_base: int = DRAM_BASE,
    cpu_regs: list[int] | None = None,
    unique_chip_id: int | None = None,
    chip_id: int | None = None,
    owned_memory_size: int | None = None,
    root_match_volume_uuid: uuid.UUID | None = None,
    psci_method: str = "hvc",
    osenvironment: str = "main",
    crypto_hash_method: str = "sha2-384",
    data_ramdisk: bool = False,
    packed_data_ramdisk: bool = False,
    trust_cache: bool = False,
    persistent_data: bool = False,
    text_fixups: tuple[str, ...] = (),
    seed: bytes | None = None,
    identity: Path | None = None,
) -> bytes:
    """Encode the whole tree. Every input is explicit so a test can pin it."""
    if osenvironment not in OSENVIRONMENTS:
        raise SystemExit(f"--osenvironment must be one of {', '.join(OSENVIRONMENTS)}")
    if crypto_hash_method not in CRYPTO_HASH_METHODS:
        raise SystemExit(f"--crypto-hash-method must be one of {', '.join(CRYPTO_HASH_METHODS)}")
    if data_ramdisk and packed_data_ramdisk:
        raise SystemExit("--data-ramdisk and --packed-data-ramdisk are alternatives")
    if persistent_data and not (data_ramdisk or packed_data_ramdisk):
        raise SystemExit("--persistent-data describes a Data seed; pass --data-ramdisk or --packed-data-ramdisk")
    regs = cpu_regs if cpu_regs is not None else parse_cpu_regs(DEFAULT_CPU_REGS)

    chosen: list[tuple[str, bytes]] = [
        ("dram-base", u64(dram_base)),
        ("dram-size", u64(dram_size)),
        ("random-seed", seed if seed is not None else os.urandom(64)),
        ("osenvironment", string(osenvironment)),
        ("crypto-hash-method", string(crypto_hash_method)),
    ]
    if chip_id is not None:
        chosen.append(("chip-id", u32(chip_id)))
    if unique_chip_id is not None:
        chosen.append(("unique-chip-id", u64(unique_chip_id)))
    # iBoot's Image4 identity. The 16.x AppleImage4 reads each of these as a u32 from /chosen
    # and panics on a missing one ("failed to get mix-n-match policy: 2" on the 950 XL). The
    # first four are upstream's development-fused, undemoted values; mix-n-match prevention 0
    # means mix-n-match is allowed, since no Apple manifest chain exists on this board.
    chosen.extend([
        ("certificate-production-status", u32(0)),
        ("effective-production-status-ap", u32(0)),
        ("certificate-security-mode", u32(1)),
        ("effective-security-mode-ap", u32(1)),
        ("effective-security-mode-sep", u32(1)),
        ("mix-n-match-prevention-status", u32(0)),
        ("research-enabled", u32(0)),
        ("internal-use-only-unit", u32(0)),
        ("engineering-use-only-unit", u32(0)),
        ("factory-prerelease-global-trust", u32(0)),
        # The "previous stage manifest hash": iBoot's SHA-384 of the boot manifest. AppleImage4's
        # nonce manager panics with "failed to get previous stage manifest hash: 2" without it.
        ("boot-manifest-hash", hashlib.sha384(os.urandom(32)).digest()),
        # AMFI's initializer panics "no amfi-only-platform-code device tree node!" without it
        # (950 XL, 16.7.16); 1 keeps AMFI in its platform-code-only policy, as upstream sets it.
        ("amfi-only-platform-code", u32(1)),
        # Non-zero makes AMFI log "delaying developer mode latching" instead of waiting for
        # ApplePMGRNub to latch it into the PMGR always-on register; there is no Apple PMGR
        # here, so it panicked "AMFI: No PMGR?" (ConfigurationSettings.cpp:152, 950 XL).
        ("security-mode-change-enable", u32(1)),
    ])
    chosen.extend(empty_nvram_proxy())
    if owned_memory_size is not None:
        chosen.append(("darwinboot-memory-size", u64(owned_memory_size)))
    if root_match_volume_uuid is not None:
        xml = (
            "<dict><key>IOProviderClass</key><string>IOMedia</string>"
            "<key>IOPropertyMatch</key><dict><key>UUID</key><string>"
            + str(root_match_volume_uuid).upper()
            + "</string></dict></dict>\0"
        )
        chosen.append(("root-matching", xml.encode()))
    if (data_ramdisk or packed_data_ramdisk) and not persistent_data:
        chosen.append(("ephemeral-storage", u32(1)))
    if text_fixups:
        chosen.append(("text-fixups", b"".join(item.encode() + b"\0" for item in text_fixups)))
    if identity is not None:
        # MobileGestalt's screen class: display-scale comes from /chosen (default 0 makes
        # SpringBoard's FBSDisplayMode invalid), the pixel size from IOMobileFramebuffer.
        chosen.append(("display-scale", u32(CANVAS_SCALE)))

    placeholders: list[tuple[str, bytes]] = []
    if data_ramdisk:
        placeholders.append(("RAMDisk", bytes(16)))
    if packed_data_ramdisk:
        placeholders.append(("PackedRAMDisk", bytes(16)))
    if trust_cache:
        placeholders.append(("TrustCache", bytes(16)))
    chosen_children = [node("memory-map", placeholders)]
    # iBoot copies the boot manifest's properties here. AppleImage4's start panics with "failed
    # to get manifest properties" when the node is missing (950 XL), then reads BNCH, a 48-byte
    # SHA-384 boot-nonce hash, and ECID on its NVRAM nonce-sync path.
    manifest = [("BNCH", hashlib.sha384(os.urandom(32)).digest())]
    if unique_chip_id is not None:
        manifest.append(("ECID", u64(unique_chip_id)))
    chosen_children.append(node("manifest-properties", manifest))
    # Local boot policy (kern_csr.c csr_bootstrap): lp-sip0 relaxes SIP like `csrutil disable`
    # (untrusted kexts, unrestricted FS, task_for_pid, kernel debugger, dtrace, nvram) and lp-sip1
    # allows the unauthenticated root. The merged HFS+ root's dyld shared cache is not
    # SF_RESTRICTED, so vm_unix.c maps it only with CSR_ALLOW_UNRESTRICTED_FS.
    chosen_children.append(node("asmb", [("lp-sip0", u64(0x6f)), ("lp-sip1", u32(1))]))

    intc = node(
        "intc",
        [
            ("compatible", string("qcom,msm-qgic2", "arm,gic-2")),
            ("interrupt-controller", b""),
            ("AAPL,phandle", u32(1)),
            ("#address-cells", u32(0)),
            ("#interrupt-cells", u32(3)),
            ("reg", b"".join(u64(address) + u64(size) for address, size in (GIC_DISTRIBUTOR, GIC_CPU_INTERFACE))),
        ],
    )
    sdhci = node(
        "sdhci@f9824900",
        [
            ("compatible", string("qcom,msm8994-sdhci", "qcom,sdhci-msm-v4")),
            ("reg", b"".join(u64(address) + u64(size) for address, size in SDHCI_WINDOWS)),
            ("interrupts", b"".join(u32(0) + u32(irq) + u32(4) for irq in SDHCI_INTERRUPTS)),
            ("interrupt-parent", u32(1)),
            ("clock-frequency", u32(TIMEBASE_FREQUENCY)),
        ],
    )
    kgsl = node(
        "kgsl@fdb00000",
        [
            ("compatible", string("qcom,kgsl-3d0", "qcom,kgsl-3d")),
            ("reg", b"".join(u64(address) + u64(size) for address, size in KGSL_WINDOWS)),
            ("interrupts", u32(0) + u32(KGSL_INTERRUPT) + u32(4)),
            ("interrupt-parent", u32(1)),
            # The device tree's qcom,chipid: generation 4 (a4xx) is what the
            # driver's register database and command stream cover.
            ("chip-id", u32(GPU_CHIP_ID)),
        ],
    )
    mdss = node(
        "mdss@fd900000",
        [
            ("compatible", string("qcom,mdss_mdp", "qcom,mdp")),
            ("reg", b"".join(u64(address) + u64(size) for address, size in MDSS_WINDOWS)),
            ("interrupts", u32(0) + u32(MDSS_INTERRUPT) + u32(4)),
            ("interrupt-parent", u32(1)),
            # The panel the firmware's GOP mode describes, so a driver that has
            # the ADT but not the firmware still knows what it is driving.
            ("panel-width", u32(1440)),
            ("panel-height", u32(2560)),
        ],
    )
    arm_io = node(
        "arm-io",
        [
            ("device_type", string("arm-io")),
            ("#address-cells", u32(2)),
            ("#size-cells", u32(2)),
            # Identity window over the whole 4 GiB peripheral space. Children
            # use absolute SoC addresses, so an offset is the address.
            ("ranges", u64(0) + u64(0) + u64(1 << 32)),
        ],
        [intc, sdhci, kgsl, mdss] + ([node(
            "disp0",
            [
                # IOMobileFramebuffer.framework counts /arm-io children of this device_type, waits
                # for that many IOMobileFramebuffer services and reads the canvas from the node:
                # the presented size (NativeDisplay fits it into the 1440x2560 panel).
                ("device_type", string("display-subsystem")),
                ("compatible", string("lumia950,display")),
                # The display hardware it stands for (the MDP block); NativeDisplay maps nothing:
                # it scans out through the firmware's GOP framebuffer.
                ("reg", u64(MDSS_WINDOWS[0][0]) + u64(MDSS_WINDOWS[0][1])),
                ("canvas-width", u32(CANVAS[0])),
                ("canvas-height", u32(CANVAS[1])),
            ],
        )] if identity is not None else []),
    )

    children = [
        node("chosen", chosen, chosen_children),
        node("cpus", children=[cpu_node(index, reg) for index, reg in enumerate(regs)]),
    ]
    if len(regs) > 1 and psci_method != "none":
        children.append(node("psci", [("compatible", string("arm,psci-1.0")), ("method", string(psci_method))]))
    children += [
        # no-effaceable-storage: keybagd reads /private/var/keybags/systembag.kb as a plain plist
        # instead of unwrapping it with an effaceable-storage key this phone does not have.
        # kern.maxvnodes: the real D221AP value. Without it the BCM2837 config's <xsmall>
        # CONFIG_VNODES=1024 caps the vnode table and a full iOS boot spends its time in
        # "vnode: table is full" retries.
        # kern.vm_compressor 2 = in-core compressor, no swap: the kernel's default (8, the
        # deprecated freezer pager) leaves the compressor inactive, so memory pressure can only
        # evict file pages and re-read them from eMMC. D221AP uses 0x20, which adds freezer swap.
        node("defaults", [("sleep-disabled", u32(1)), ("no-effaceable-storage", u32(1)),
                          ("kern.maxvnodes", u32(10000)), ("kern.vm_compressor", u32(2))]),
        node("options"),
    ]
    if identity is None:
        children.append(node("product", [("product-name", string("Lumia 950")),
                                         ("model", string("microsoft,lumia950"))]))
        children.append(arm_io)
        return node(
            "device-tree",
            [
                ("model", string("Microsoft Lumia 950")),
                ("compatible", string("microsoft,lumia950", "qcom,msm8992")),
                ("#address-cells", u32(2)),
                ("#size-cells", u32(2)),
            ],
            children,
        )
    # The iPhone identity: compatible[0] becomes hw.model/hw.target and model hw.machine/hw.product
    # (D221AP / iPhone10,6), which MobileGestalt expects; the Lumia strings stay in compatible.
    root, product = iphone_identity(identity)
    apple = [value.decode() for value in root["compatible"].split(b"\0") if value]
    compatible = [value for value in apple if value != "AppleARM"] + ["microsoft,lumia950", "qcom,msm8992", "AppleARM"]
    # MobileGestalt HomeButtonType = IODeviceTree:/buttons home-button-type (2: no Home button).
    children.append(node("buttons", [("button-names", string("volup", "voldown", "hold")),
                                     ("home-button-type", u32(2)),
                                     ("opposed-power-vol-buttons", u32(1))]))
    children.append(product)
    children.append(arm_io)
    return node(
        "device-tree",
        [
            ("model", root["model"]),
            ("compatible", string(*compatible)),
            ("target-type", root.get("target-type", string("D221"))),
            ("target-sub-type", root.get("target-sub-type", string("D221AP"))),
            ("manufacturer", root.get("manufacturer", string("Apple Inc."))),
            ("#address-cells", u32(2)),
            ("#size-cells", u32(2)),
        ],
        children,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, default=ROOT / "build/lumia950.adt")
    parser.add_argument("--dram-size", type=lambda value: int(value, 0), required=True,
                        help="DRAM the firmware reports to the OS, not the 3 GiB marketing figure")
    parser.add_argument("--dram-base", type=lambda value: int(value, 0), default=DRAM_BASE)
    parser.add_argument("--cpu-regs", default=DEFAULT_CPU_REGS,
                        help="comma-separated MPIDR affinity values, boot CPU first")
    parser.add_argument("--unique-chip-id", type=lambda value: int(value, 16),
                        help="64-bit platform seed in hex; XNU blocks gethostuuid() without it")
    parser.add_argument("--chip-id", type=lambda value: int(value, 0),
                        help="32-bit platform identity seed")
    parser.add_argument("--owned-memory-size", type=lambda value: int(value, 0),
                        help="contiguous window request, 256 MiB to 8 GiB in 2 MiB steps")
    parser.add_argument("--root-match-volume-uuid", type=uuid.UUID)
    parser.add_argument("--psci-method", choices=["hvc", "smc", "none"], default="hvc")
    parser.add_argument("--osenvironment", choices=OSENVIRONMENTS, default="main")
    parser.add_argument("--crypto-hash-method", choices=CRYPTO_HASH_METHODS, default="sha2-384")
    parser.add_argument("--data-ramdisk", action="store_true")
    parser.add_argument("--packed-data-ramdisk", action="store_true")
    parser.add_argument("--trust-cache", action="store_true")
    parser.add_argument("--owner-trust-cache", action="store_true",
                        help="accepted from the payload contract; it still only reserves TrustCache")
    parser.add_argument("--gpu-addon", action="store_true",
                        help="accepted from the payload contract; no ADT property is defined for it")
    parser.add_argument("--ipad-identity", action="store_true",
                        help="accepted from the payload contract; this board is not an iPad")
    parser.add_argument("--persistent-data", action="store_true")
    parser.add_argument("--iphone-identity", type=Path,
                        help="the real device tree (DeviceTree.d221ap.im4p payload) whose root identity "
                             "and /product the iOS userland expects")
    parser.add_argument("--text-fixup", action="append", default=[])
    args = parser.parse_args()

    if args.data_ramdisk and args.packed_data_ramdisk:
        parser.error("--data-ramdisk and --packed-data-ramdisk are alternatives")
    if args.persistent_data and not (args.data_ramdisk or args.packed_data_ramdisk):
        parser.error("--persistent-data needs --data-ramdisk or --packed-data-ramdisk")
    if args.owned_memory_size is not None and not (
            0x10000000 <= args.owned_memory_size <= 0x200000000 and args.owned_memory_size % 0x200000 == 0):
        parser.error("--owned-memory-size is 256 MiB to 8 GiB in 2 MiB steps")
    if args.chip_id is not None and not 0 <= args.chip_id < 1 << 32:
        parser.error("--chip-id is a 32-bit value")
    if args.unique_chip_id is not None and not 0 <= args.unique_chip_id < 1 << 64:
        parser.error("--unique-chip-id is a 64-bit value")

    if args.gpu_addon:
        print("note: --gpu-addon has no ADT property; nothing is emitted for it", file=sys.stderr)
    if args.ipad_identity:
        print("note: --ipad-identity does not apply to MSM8992; the product node stays the Lumia", file=sys.stderr)

    tree = build_tree(
        dram_size=args.dram_size,
        dram_base=args.dram_base,
        cpu_regs=parse_cpu_regs(args.cpu_regs),
        unique_chip_id=args.unique_chip_id,
        chip_id=args.chip_id,
        owned_memory_size=args.owned_memory_size,
        root_match_volume_uuid=args.root_match_volume_uuid,
        psci_method=args.psci_method,
        osenvironment=args.osenvironment,
        crypto_hash_method=args.crypto_hash_method,
        data_ramdisk=args.data_ramdisk,
        packed_data_ramdisk=args.packed_data_ramdisk,
        trust_cache=args.trust_cache or args.owner_trust_cache,
        persistent_data=args.persistent_data,
        text_fixups=tuple(args.text_fixup),
        identity=args.iphone_identity,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(tree)

    cpus = len(parse_cpu_regs(args.cpu_regs))
    print(f"lumia950 ADT: {len(tree)} bytes, {cpus} CPUs, dram {args.dram_size:#x} at {args.dram_base:#x}")
    if args.unique_chip_id is None or args.chip_id is None:
        print("warning: no chip identity; XNU seeds IOPlatformUUID from it and gethostuuid() blocks without it",
              file=sys.stderr)
    print("note: ArmCpu GICv2 integration still needs an XNU build and handset IRQ validation", file=sys.stderr)
    if cpus > 1 and args.psci_method != "none":
        print("note: MSM8992 PSCI is a subset over HVC with no SYSTEM_OFF/SYSTEM_RESET (README blocker 4)",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
