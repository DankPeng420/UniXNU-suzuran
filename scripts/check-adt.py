#!/usr/bin/env python3
"""Check an Apple device tree (ADT) for the Lumia 950 before DarwinBoot gets it.

The checker does not trust the generator. It parses the bytes with the encoding
XNU reads, then checks the things a kernel would trip over:

  * the structural encoding: counts, 32-byte names, 4-byte value padding;
  * the nodes ArmDTPlatform requires, and the properties it reads;
  * the CHRP NVRAM proxy against the layout in
    iokit/Kernel/IONVRAMCHRPHandler.cpp, including the Adler-32 XNU panics on;
  * the memory-map placeholders DarwinBoot fills, and their 16-byte shape;
  * the identity properties, because `gethostuuid()` waits forever without
    `unique-chip-id` and `chip-id`;
  * the cell layout this repository's generator uses (2 address cells and 2
    size cells, u64 values), the one the QEMU fixture proved to the platform
    expert.

`--self-test` mutates a known-good tree into specific defects and requires the
checker to reject each one, because a checker that only ever sees good input is
not a checker.

usage:
  python3 scripts/check-adt.py build/lumia950.adt
  python3 scripts/check-adt.py build/lumia950.adt --dump
  python3 scripts/check-adt.py --self-test
"""
from __future__ import annotations

import argparse
import importlib.util
import struct
import sys
import zlib
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "scripts" / "make-lumia950-adt.py"

REQUIRED_TOP_LEVEL = ("chosen", "cpus", "arm-io", "defaults", "options")
OSENVIRONMENTS = ("main", "recoveryos", "ramdisk")
CHRP_SIGNATURE = 0x70
CHRP_BLOCK = 0x10
MAX_COUNT = 4096


class Error(Exception):
    pass


@dataclass
class Prop:
    name: str
    value: bytes
    value_offset: int
    padding: bytes


@dataclass
class Node:
    name: str
    props: list[Prop] = field(default_factory=list)
    children: list["Node"] = field(default_factory=list)
    offset: int = 0

    def prop(self, name: str) -> Prop | None:
        for item in self.props:
            if item.name == name:
                return item
        return None

    def value(self, name: str) -> bytes | None:
        item = self.prop(name)
        return item.value if item else None

    def child(self, name: str) -> "Node" | None:
        for item in self.children:
            if item.name == name:
                return item
        return None

    def find(self, path: str) -> "Node" | None:
        node = self
        for part in [part for part in path.split("/") if part]:
            node = node.child(part)
            if node is None:
                return None
        return node


def parse(data: bytes) -> Node:
    """Parse the whole blob, rejecting anything ambiguous."""
    node, cursor = _parse_node(data, 0, ())
    if cursor != len(data):
        raise Error(f"{len(data) - cursor} trailing bytes after the root node")
    return node


def _parse_node(data: bytes, offset: int, path: tuple[str, ...]) -> tuple[Node, int]:
    if offset + 8 > len(data):
        raise Error(f"truncated node header at offset {offset}")
    count, child_count = struct.unpack_from("<II", data, offset)
    if count > MAX_COUNT or child_count > MAX_COUNT:
        raise Error(f"implausible counts {count}/{child_count} at offset {offset}")
    cursor = offset + 8
    props: list[Prop] = []
    names = set()
    for _ in range(count):
        if cursor + 36 > len(data):
            raise Error(f"truncated property header at offset {cursor}")
        raw = data[cursor:cursor + 32]
        terminator = raw.find(b"\0")
        if terminator <= 0:
            raise Error(f"property name at offset {cursor} is empty or unterminated")
        if any(raw[terminator + 1:]):
            raise Error(f"property name at offset {cursor} has bytes after its terminator")
        try:
            name = raw[:terminator].decode("ascii")
        except UnicodeDecodeError:
            raise Error(f"property name at offset {cursor} is not ASCII") from None
        if name in names:
            raise Error(f"duplicate property {name!r} in a node at offset {offset}")
        names.add(name)
        (length,) = struct.unpack_from("<I", data, cursor + 32)
        cursor += 36
        if length > len(data) - cursor:
            raise Error(f"property {name!r} claims {length} bytes past the end of the file")
        value_offset = cursor
        value = data[cursor:cursor + length]
        pad_length = -length % 4
        cursor += length
        if pad_length > len(data) - cursor:
            raise Error(f"property {name!r} has no room for its padding")
        padding = data[cursor:cursor + pad_length]
        if any(padding):
            raise Error(f"property {name!r} at offset {value_offset} has nonzero padding")
        cursor += pad_length
        props.append(Prop(name, value, value_offset, padding))

    name_prop = next((item for item in props if item.name == "name"), None)
    if name_prop is None:
        raise Error(f"node at offset {offset} has no name property")
    if not name_prop.value.endswith(b"\0") or b"\0" in name_prop.value[:-1]:
        raise Error(f"node name at offset {offset} is not one NUL-terminated string")
    name = name_prop.value[:-1].decode("ascii", "replace")
    if not name:
        raise Error(f"node at offset {offset} has an empty name")
    if name in path:
        raise Error(f"node {name!r} repeats an ancestor name")
    here = (*path, name)

    children: list[Node] = []
    child_names = set()
    for _ in range(child_count):
        child, cursor = _parse_node(data, cursor, here)
        if child.name in child_names:
            raise Error(f"duplicate child {child.name!r} under /{'/'.join(here)}")
        child_names.add(child.name)
        children.append(child)
    return Node(name, props, children, offset), cursor


def encode(node: Node) -> bytes:
    """Re-encode a parsed node. Used by the self-test to build mutants."""
    props = [("name", node.name.encode() + b"\0")]
    props += [(item.name, item.value) for item in node.props if item.name != "name"]
    body = struct.pack("<II", len(props), len(node.children))
    for name, value in props:
        body += struct.pack("<32sI", name.encode(), len(value))
        body += value + bytes(-len(value) % 4)
    return body + b"".join(encode(child) for child in node.children)


def describe(value: bytes) -> str:
    if value.endswith(b"\0"):
        parts = value[:-1].split(b"\0")
        if all(parts) and all(all(32 <= byte < 127 for byte in part) for part in parts):
            return " | ".join(repr(part.decode()) for part in parts)
    if len(value) == 4:
        return f"{struct.unpack('<I', value)[0]:#x}"
    if len(value) == 8:
        return f"{struct.unpack('<Q', value)[0]:#x}"
    return f"{len(value)} bytes"


def validate(root: Node, *, allow_missing_identity: bool = False) -> list[str]:
    """Raise Error on a defect; return the notes a human should still read."""
    errors: list[str] = []
    notes: list[str] = []

    if root.name != "device-tree":
        errors.append(f"root node is {root.name!r}, not 'device-tree'")
    for name in REQUIRED_TOP_LEVEL:
        if root.child(name) is None:
            errors.append(f"missing required node /{name}")
    if root.child("product") is None:
        notes.append("/product is absent: MobileGestalt and the platform code read it for identity")

    arm_io = root.child("arm-io")
    chosen = root.child("chosen")
    cpus = root.child("cpus")

    if chosen is not None:
        _check_chosen(chosen, errors, notes, allow_missing_identity)
    if cpus is not None:
        _check_cpus(cpus, errors)
        if len(cpus.children) > 1 and root.child("psci") is None:
            errors.append(f"/cpus has {len(cpus.children)} CPUs but there is no /psci node")
    psci = root.child("psci")
    if psci is not None:
        _check_psci(psci, errors)
    if arm_io is not None:
        _check_arm_io(arm_io, errors, notes)

    if errors:
        raise Error("\n".join(errors))
    return notes


def _check_chosen(chosen: Node, errors: list[str], notes: list[str], allow_missing_identity: bool) -> None:
    for name, length in (("dram-base", 8), ("dram-size", 8)):
        value = chosen.value(name)
        if value is None:
            errors.append(f"/chosen has no {name}")
        elif len(value) != length:
            errors.append(f"/chosen/{name} is {len(value)} bytes, expected {length}")
        elif name == "dram-size" and struct.unpack("<Q", value)[0] == 0:
            errors.append(f"/chosen/{name} is zero")

    seed = chosen.value("random-seed")
    if seed is None:
        errors.append("/chosen has no random-seed (the kernel wants 64 bytes of entropy)")
    elif len(seed) != 64:
        errors.append(f"/chosen/random-seed is {len(seed)} bytes, expected 64")

    environment = chosen.value("osenvironment")
    if environment is not None:
        text = environment[:-1].decode("ascii", "replace") if environment.endswith(b"\0") else None
        if text not in OSENVIRONMENTS:
            errors.append(f"/chosen/osenvironment is {environment!r}, expected one of {OSENVIRONMENTS}")

    method = chosen.value("crypto-hash-method")
    if method is None:
        notes.append("/chosen has no crypto-hash-method: Image4 policy work will have to name one")
    elif not method.endswith(b"\0"):
        errors.append("/chosen/crypto-hash-method is not NUL-terminated")

    chip_id = chosen.value("chip-id")
    unique = chosen.value("unique-chip-id")
    missing = []
    if chip_id is None or len(chip_id) != 4:
        missing.append("chip-id (4 bytes)")
    if unique is None or len(unique) != 8:
        missing.append("unique-chip-id (8 bytes)")
    if missing:
        message = "/chosen lacks " + " and ".join(missing) + ": gethostuuid() waits forever without them"
        if allow_missing_identity:
            notes.append(message)
        else:
            errors.append(message)

    window = chosen.value("darwinboot-memory-size")
    if window is not None:
        if len(window) != 8:
            errors.append("/chosen/darwinboot-memory-size is not 8 bytes")
        else:
            size = struct.unpack("<Q", window)[0]
            if not 0x10000000 <= size <= 0x200000000 or size % 0x200000:
                errors.append(f"/chosen/darwinboot-memory-size {size:#x} is not 256 MiB..8 GiB in 2 MiB steps")

    root_matching = chosen.value("root-matching")
    if root_matching is not None and (not root_matching.endswith(b"\0") or b"IOMedia" not in root_matching):
        errors.append("/chosen/root-matching is not a NUL-terminated IOMedia dictionary")

    bank_size = chosen.value("nvram-bank-size")
    bank_count = chosen.value("nvram-bank-count")
    current = chosen.value("nvram-current-bank")
    image = chosen.value("nvram-proxy-data")
    present = [name for name, value in (("nvram-bank-size", bank_size), ("nvram-bank-count", bank_count),
                                        ("nvram-current-bank", current), ("nvram-proxy-data", image)) if value is not None]
    if present and len(present) != 4:
        errors.append("/chosen NVRAM proxy is incomplete: " + ", ".join(sorted(present)))
    elif present:
        _check_nvram(bank_size, bank_count, current, image, errors)

    if chosen.child("memory-map") is None:
        errors.append("missing /chosen/memory-map: XNU VM initialization requires the node even without a trust cache")
    for child in chosen.children:
        if child.name == "memory-map":
            for placeholder in child.props:
                if placeholder.name == "name":
                    continue
                if len(placeholder.value) != 16:
                    errors.append(f"/chosen/memory-map/{placeholder.name} is {len(placeholder.value)} bytes, expected 16")


def _check_nvram(bank_size: bytes, bank_count: bytes, current: bytes, image: bytes, errors: list[str]) -> None:
    for name, value in (("nvram-bank-size", bank_size), ("nvram-bank-count", bank_count),
                        ("nvram-current-bank", current)):
        if len(value) != 4:
            errors.append(f"/chosen/{name} is not 4 bytes")
            return
    size, count, index = struct.unpack("<I", bank_size)[0], struct.unpack("<I", bank_count)[0], struct.unpack("<I", current)[0]
    if size * count != len(image):
        errors.append(f"/chosen NVRAM is {len(image)} bytes, expected bank size {size} x {count}")
        return
    if count == 0 or index >= count:
        errors.append(f"/chosen NVRAM current bank {index} is outside {count} banks")
        return
    if len(image) < 32:
        errors.append("/chosen NVRAM image is too small for an Apple header")
        return
    adler = struct.unpack_from("<I", image, 16)[0]
    calculated = zlib.adler32(image[20:]) & 0xFFFFFFFF
    if adler != calculated:
        errors.append(f"/chosen NVRAM header adler {adler:#010x} != calculated {calculated:#010x} (XNU panics on this)")
    offset = 0
    names = []
    while offset < len(image):
        if offset + 16 > len(image):
            errors.append(f"/chosen NVRAM partition at {offset} is truncated")
            return
        header = image[offset:offset + 16]
        if header[0] != CHRP_SIGNATURE:
            errors.append(f"/chosen NVRAM partition at {offset} has signature {header[0]:#x}, expected {CHRP_SIGNATURE:#x}")
            return
        blocks = struct.unpack_from("<H", header, 2)[0]
        length = blocks * CHRP_BLOCK
        if length < 16 or offset + length > len(image):
            errors.append(f"/chosen NVRAM partition at {offset} has an impossible length {length}")
            return
        checksum = sum(header[:1] + header[2:16])
        while checksum > 0xFF:
            checksum = (checksum & 0xFF) + (checksum >> 8)
        if checksum != header[1]:
            errors.append(f"/chosen NVRAM partition at {offset} has checksum {header[1]:#x}, expected {checksum:#x}")
            return
        names.append(header[4:16].split(b"\0")[0].decode("ascii", "replace"))
        offset += length
    if offset != len(image):
        errors.append("/chosen NVRAM partitions do not fill the bank exactly")
    if "common" not in names:
        errors.append(f"/chosen NVRAM has no common partition: {names}")


def _check_cpus(cpus: Node, errors: list[str]) -> None:
    if not cpus.children:
        errors.append("/cpus has no CPU nodes")
        return
    running = 0
    affinities = set()
    for index, cpu in enumerate(cpus.children):
        expected = f"cpu{index}"
        if cpu.name != expected:
            errors.append(f"/cpus child {cpu.name!r} is not {expected!r}")
        reg = cpu.value("reg")
        if reg is None or len(reg) != 4:
            errors.append(f"/cpus/{cpu.name}/reg is not one 32-bit affinity value")
        else:
            affinity = struct.unpack("<I", reg)[0]
            if affinity in affinities:
                errors.append(f"/cpus/{cpu.name} repeats affinity {affinity:#x}")
            affinities.add(affinity)
            if affinity in (0, 1, 2, 3):
                expected_compatible = b"arm,cortex-a53\0"
            elif affinity in (0x100, 0x101, 0x102, 0x103):  # 0x102/0x103: MSM8994 (950 XL)
                expected_compatible = b"arm,cortex-a57\0"
            else:
                expected_compatible = None
                errors.append(f"/cpus/{cpu.name} has unsupported MSM8992 affinity {affinity:#x}")
            if expected_compatible is not None and cpu.value("compatible") != expected_compatible:
                errors.append(f"/cpus/{cpu.name}/compatible disagrees with MSM8992 affinity {affinity:#x}")
        state = cpu.value("state")
        if state is None:
            errors.append(f"/cpus/{cpu.name} has no state")
        else:
            text = state[:-1].decode("ascii", "replace") if state.endswith(b"\0") else None
            if text not in ("running", "waiting"):
                errors.append(f"/cpus/{cpu.name}/state is {state!r}, expected running or waiting")
            elif text == "running":
                running += 1
        if cpu.value("timebase-frequency") is None or len(cpu.value("timebase-frequency")) != 4:
            errors.append(f"/cpus/{cpu.name} has no 32-bit timebase-frequency")
        if cpu.value("compatible") is None:
            errors.append(f"/cpus/{cpu.name} has no compatible")
    if running != 1:
        errors.append(f"/cpus has {running} running CPUs, expected exactly one (the boot CPU)")
    first = cpus.children[0]
    if first.value("state") != b"running\0":
        errors.append(f"the boot CPU is /cpus/{first.name}, but its state is not running")


def _check_psci(psci: Node, errors: list[str]) -> None:
    compatible = psci.value("compatible")
    if compatible is None or b"arm,psci" not in compatible:
        errors.append("/psci has no arm,psci compatible string")
    method = psci.value("method")
    if method is None or method.split(b"\0")[0] not in (b"hvc", b"smc"):
        errors.append(f"/psci/method is {method!r}, expected hvc or smc")


def _check_arm_io(arm_io: Node, errors: list[str], notes: list[str]) -> None:
    for name, expected in (("#address-cells", 2), ("#size-cells", 2)):
        value = arm_io.value(name)
        if value is None or len(value) != 4 or struct.unpack("<I", value)[0] != expected:
            errors.append(f"/arm-io/{name} must be {expected} (one 64-bit value per address or size)")
    ranges = arm_io.value("ranges")
    if ranges is None:
        errors.append("/arm-io has no ranges")
    elif len(ranges) % 24 or len(ranges) < 24:
        errors.append(f"/arm-io/ranges is {len(ranges)} bytes; each entry is a 24-byte child/parent/size triple")

    if not arm_io.children:
        errors.append("/arm-io has no device children")

    for child in arm_io.children:
        reg = child.value("reg")
        if reg is None:
            errors.append(f"/arm-io/{child.name} has no reg")
        elif len(reg) % 16 or len(reg) < 16:
            errors.append(f"/arm-io/{child.name}/reg is {len(reg)} bytes; each window is a 16-byte address/size pair")

    # The GPU driver maps both of its windows by index, so the node has to carry
    # exactly the two the driver expects, and the identity the driver checks has
    # to be the one the device tree states.
    kgsl = next((child for child in arm_io.children
                 if (child.value("compatible") or b"").find(b"qcom,kgsl-3d0") >= 0), None)
    if kgsl is None:
        errors.append("/arm-io has no node compatible with qcom,kgsl-3d0")
    else:
        reg = kgsl.value("reg")
        if reg is None or len(reg) != 32:
            errors.append(f"/arm-io/{kgsl.name}/reg is {0 if reg is None else len(reg)} bytes, "
                          "expected two 16-byte windows (KGSL and MMSS)")
        interrupts = kgsl.value("interrupts")
        if interrupts is None or len(interrupts) != 12:
            errors.append(f"/arm-io/{kgsl.name}/interrupts must be three 32-bit cells")
        chip = kgsl.value("chip-id")
        if chip is None or len(chip) != 4:
            errors.append(f"/arm-io/{kgsl.name}/chip-id must be one 32-bit value")
        elif struct.unpack("<I", chip)[0] >> 24 != 4:
            errors.append(f"/arm-io/{kgsl.name}/chip-id is not an a4xx generation")

    # The display driver maps five windows by index — mdp, the MMSS clock
    # controller, DSI0's two windows and mmss_misc — and reads gdsc_mdss at
    # 0x2304 in the second, so a window shorter than that is a read past the
    # end: the register has to be inside the mapping, not merely near it.
    mdss = next((child for child in arm_io.children
                 if (child.value("compatible") or b"").find(b"qcom,mdss_mdp") >= 0), None)
    if mdss is None:
        errors.append("/arm-io has no node compatible with qcom,mdss_mdp")
    else:
        reg = mdss.value("reg")
        if reg is None or len(reg) != 5 * 16:
            errors.append(f"/arm-io/{mdss.name}/reg is {0 if reg is None else len(reg)} bytes, "
                          "expected five 16-byte windows (mdp, mmss, dsi ctrl, dsi phy, misc)")
        else:
            windows = [struct.unpack("<QQ", reg[at:at + 16]) for at in range(0, len(reg), 16)]
            if windows[0][0] != 0xFD900000:
                errors.append(f"/arm-io/{mdss.name}: the first window is the MDP at 0xfd900000")
            if windows[1][0] != 0xFD8C0000 or windows[1][1] < 0x2308:
                errors.append(f"/arm-io/{mdss.name}: the second window must be the MMSS clock "
                              "controller, big enough for gdsc_mdss at 0x2304")
            if windows[2][0] != 0xFD994000:
                errors.append(f"/arm-io/{mdss.name}: the third window is DSI0's ctrl at 0xfd994000")
        interrupts = mdss.value("interrupts")
        if interrupts is None or len(interrupts) != 12:
            errors.append(f"/arm-io/{mdss.name}/interrupts must be three 32-bit cells")
        for name, want in (("panel-width", 1440), ("panel-height", 2560)):
            value = mdss.value(name)
            if value is None or len(value) != 4 or struct.unpack("<I", value)[0] != want:
                errors.append(f"/arm-io/{mdss.name}/{name} must be {want}")

    intc = arm_io.child("intc")
    if intc is None:
        errors.append("/arm-io has no intc")
    else:
        compatible = intc.value("compatible") or b""
        if b"gic" not in compatible.lower():
            errors.append(f"/arm-io/intc/compatible is {compatible!r}, expected a GIC")
        cells = intc.value("#interrupt-cells")
        if cells is None or len(cells) != 4 or struct.unpack("<I", cells)[0] != 3:
            errors.append("/arm-io/intc/#interrupt-cells must be 3 for this GIC description")
        phandle = intc.value("AAPL,phandle")
        if phandle is None or len(phandle) != 4 or struct.unpack("<I", phandle)[0] == 0:
            errors.append("/arm-io/intc needs a nonzero 32-bit AAPL,phandle for IODT interrupt-parent lookup")
        if intc.value("#address-cells") != struct.pack("<I", 0):
            errors.append("/arm-io/intc/#address-cells must be zero for interrupt specifications")
        for child in arm_io.children:
            if child.value("interrupts") is not None and child.value("interrupt-parent") != phandle:
                errors.append(f"/arm-io/{child.name}/interrupt-parent does not reference the GIC AAPL,phandle")
        reg = intc.value("reg")
        if reg is None or len(reg) % 16 or len(reg) < 16:
            errors.append("/arm-io/intc/reg does not describe whole address/size windows")
        if b"gic-2" in compatible or b"qgic2" in compatible:
            notes.append("/arm-io/intc is a GICv2; native ArmCpu integration needs an SDK build and handset IRQ validation")

    sdhci = next((child for child in arm_io.children
                  if (child.value("compatible") or b"").find(b"qcom,sdhci-msm-v4") >= 0), None)
    if sdhci is None:
        errors.append("/arm-io has no node compatible with qcom,sdhci-msm-v4")
        return
    compatible = sdhci.value("compatible") or b""
    if b"qcom,msm8994-sdhci" not in compatible:
        errors.append("/arm-io/sdhci compatible list does not name qcom,msm8994-sdhci")
    reg = sdhci.value("reg")
    if reg is None or len(reg) != 32:
        errors.append(f"/arm-io/{sdhci.name}/reg is {0 if reg is None else len(reg)} bytes, expected two 16-byte windows")
    interrupts = sdhci.value("interrupts")
    if interrupts is None or not interrupts:
        errors.append(f"/arm-io/{sdhci.name} has no interrupts")
    elif len(interrupts) % 12:
        errors.append(f"/arm-io/{sdhci.name}/interrupts is {len(interrupts)} bytes; each interrupt is three 32-bit cells")
    clock = sdhci.value("clock-frequency")
    if clock is None or len(clock) != 4:
        errors.append(f"/arm-io/{sdhci.name} has no 32-bit clock-frequency")
    if sdhci.name.startswith("sdhci@") and sdhci.value("reg") is not None:
        address = struct.unpack("<Q", reg[:8])[0]
        if f"@{address:x}" != sdhci.name[len("sdhci"):]:
            errors.append(f"/arm-io/{sdhci.name} disagrees with its own reg address {address:#x}")


def dump(node: Node, depth: int = 0) -> None:
    tail = f"  [{len(node.props)} props, {len(node.children)} children]" if depth else ""
    print("  " * depth + node.name + tail)
    for prop in node.props:
        if prop.name == "name":
            continue
        print("  " * (depth + 1) + f"{prop.name} = {describe(prop.value)}")
    for child in node.children:
        dump(child, depth + 1)


def load_generator():
    spec = importlib.util.spec_from_file_location("lumia950_adt_generator", GENERATOR)
    if spec is None or spec.loader is None:
        raise SystemExit(f"cannot load {GENERATOR}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def self_test() -> int:
    module = load_generator()
    good = module.build_tree(dram_size=0xC0000000, unique_chip_id=0x1122334455667788,
                             chip_id=0x8015, seed=b"\x5a" * 64)
    notes = validate(parse(good), allow_missing_identity=False)
    good_tree = parse(good)
    assert struct.unpack("<Q", good_tree.child("chosen").value("dram-base"))[0] == 0
    assert [struct.unpack("<I", cpu.value("reg"))[0]
            for cpu in good_tree.child("cpus").children] == [0, 1, 2, 3, 0x100, 0x101]
    assert good_tree.child("cpus").children[0].value("compatible") == b"arm,cortex-a53\0"
    assert {prop.name for prop in good_tree.child("chosen").child("memory-map").props} == {"name"}
    rejected_count = 0

    def expect_failure(data: bytes, needle: str, label: str) -> None:
        nonlocal rejected_count
        try:
            validate(parse(data), allow_missing_identity=False)
        except Error as error:
            if needle not in str(error):
                raise SystemExit(f"{label}: rejected, but for the wrong reason: {error}") from None
        else:
            raise SystemExit(f"{label}: was accepted")
        rejected_count += 1

    expect_failure(good[:12], "truncated", "truncated header")

    tree = parse(good)
    tree.child("chosen").children = [child for child in tree.child("chosen").children if child.name != "memory-map"]
    expect_failure(encode(tree), "/chosen/memory-map", "empty mandatory memory-map node omitted")

    tree = parse(good)
    tree.find("arm-io/intc").props = [prop for prop in tree.find("arm-io/intc").props if prop.name != "AAPL,phandle"]
    expect_failure(encode(tree), "AAPL,phandle", "missing IODT interrupt controller reference")

    tree = parse(good)
    tree.find("arm-io/sdhci@f9824900").prop("interrupt-parent").value = struct.pack("<I", 2)
    expect_failure(encode(tree), "interrupt-parent", "unresolved IODT interrupt parent")

    tree = parse(good)
    tree.children = [child for child in tree.children if child.name != "cpus"]
    expect_failure(encode(tree), "/cpus", "missing /cpus")

    tree = parse(good)
    cpu = tree.child("cpus").children[0]
    cpu.prop("compatible").value = b"arm,cortex-a57\0"
    expect_failure(encode(tree), "compatible", "boot A53 mislabeled as A57")

    tree = parse(good)
    tree.child("cpus").children[-1].prop("reg").value = struct.pack("<I", 0x104)
    expect_failure(encode(tree), "MSM8992 affinity", "nonexistent CPU affinity")

    tree = parse(good)
    tree.child("cpus").children[1].prop("reg").value = struct.pack("<I", 0)
    expect_failure(encode(tree), "repeats affinity", "duplicate CPU affinity")

    tree = parse(good)
    tree.child("chosen").prop("dram-size").value = struct.pack("<Q", 0)
    expect_failure(encode(tree), "dram-size", "zero DRAM size with valid zero base")

    tree = parse(good)
    chosen = tree.child("chosen")
    chosen.props = [item for item in chosen.props if item.name != "random-seed"]
    expect_failure(encode(tree), "random-seed", "missing random-seed")

    tree = parse(good)
    chosen = tree.child("chosen")
    prop = chosen.prop("nvram-proxy-data")
    prop.value = prop.value[:16] + b"\0\0\0\0" + prop.value[20:]
    expect_failure(encode(tree), "adler", "bad NVRAM adler")

    tree = parse(good)
    arm_io = tree.child("arm-io")
    sdhci = arm_io.child("sdhci@f9824900")
    sdhci.props = [item if item.name != "reg" else Prop("reg", item.value[:16], item.value_offset, b"")
                   for item in sdhci.props]
    expect_failure(encode(tree), "expected two 16-byte windows", "short sdhci reg")

    no_psci = module.build_tree(dram_size=0xC0000000, cpu_regs=[0, 1], psci_method="none",
                                unique_chip_id=1, chip_id=2, seed=b"\x5a" * 64)
    expect_failure(no_psci, "psci", "two CPUs without PSCI")

    no_identity = module.build_tree(dram_size=0xC0000000, seed=b"\x5a" * 64)
    expect_failure(no_identity, "gethostuuid", "missing identity")
    validate(parse(no_identity), allow_missing_identity=True)

    # The display driver reads gdsc_mdss at 0x2304 through the second window, so
    # a window that stops before it is a read past the end of a mapping.
    tree = parse(good)
    arm_io = tree.child("arm-io")
    mdss = arm_io.child("mdss@fd900000")
    reg = mdss.prop("reg")
    windows = [struct.unpack("<QQ", reg.value[at:at + 16]) for at in range(0, len(reg.value), 16)]
    windows[1] = (windows[1][0], 0x1000)
    mdss.props = [item if item.name != "reg" else
                  Prop("reg", b"".join(struct.pack("<QQ", *window) for window in windows),
                       item.value_offset, b"")
                  for item in mdss.props]
    expect_failure(encode(tree), "gdsc_mdss", "MMSS window too short for gdsc_mdss")

    tree = parse(good)
    arm_io = tree.child("arm-io")
    mdss = arm_io.child("mdss@fd900000")
    mdss.props = [item for item in mdss.props if item.name != "panel-width"]
    expect_failure(encode(tree), "panel-width", "no panel width")

    # Nonzero padding in a real value: the generator never writes it, so patch a copy.
    tree = parse(good)
    chosen = tree.child("chosen")
    prop = chosen.prop("osenvironment")
    padded = bytearray(good)
    padded[prop.value_offset + len(prop.value)] = 0xFF
    expect_failure(bytes(padded), "padding", "nonzero padding")

    print(f"self-test ok: the good tree passes ({len(notes)} notes) and {rejected_count} mutants are rejected")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("adt", nargs="?", type=Path, help="ADT sidecar to check")
    parser.add_argument("--dump", action="store_true", help="print the decoded tree")
    parser.add_argument("--allow-missing-identity", action="store_true",
                        help="downgrade the chip-id/unique-chip-id error to a note")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if args.adt is None:
        parser.error("pass an ADT file, or --self-test")

    data = args.adt.read_bytes()
    root = parse(data)
    if args.dump:
        dump(root)
    notes = validate(root, allow_missing_identity=args.allow_missing_identity)
    print(f"ADT ok: {args.adt} ({len(data)} bytes, root {root.name})")
    for note in notes:
        print(f"note: {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
