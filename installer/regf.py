r"""Read and write Windows registry hive files (regf), enough for a BCD store.

The Lumia's boot configuration lives in \EFI\Microsoft\BOOT\BCD on EFIESP, and a BCD store is an
ordinary registry hive. Editing it used to need a Windows machine running bcdedit; this module
lets the installer do the same edits on the Mac, with nothing but the standard library.

A hive is parsed into a tree of Key objects and written back as a *fresh* hive: every cell is laid
out again from the start of the file, with no free space left over from earlier edits. That is
what makes the writer short enough to trust. Editing cells in place (as Windows does) means
managing a free list, splitting and merging cells, and keeping the subkey lists sorted across
inserts; a fresh layout needs none of that. What is kept from the source file:

- the base block (the 4096-byte header) apart from the fields that describe the new layout,
  the sequence numbers and the checksum, so the hive keeps its GUIDs and offline-registry marks;
- every key's timestamp, flags, class name, access bits and security descriptor;
- every value's name, type and bytes.

Only what the hive format itself derives (offsets, counts, the max-length fields, subkey hashes,
security-cell reference counts) is recomputed. tests/test_regf.py checks that a round trip
reproduces the same tree and the same derived fields as bcdedit's own files.

The format notes come from the Windows registry file format specification by Maxim Suhanov
(github.com/msuhanov/regf), which matches what bcdedit writes for the Lumia's store: version
1.5, `lh` subkey lists, ASCII ("compressed") names.
"""
from __future__ import annotations

import struct
import time
from dataclasses import dataclass, field

BASE_BLOCK_SIZE = 4096
HBIN_SIZE = 4096
HBIN_HEADER_SIZE = 32
NO_CELL = 0xFFFFFFFF

KEY_HIVE_ENTRY = 0x0004
KEY_NO_DELETE = 0x0008
KEY_COMP_NAME = 0x0020
VALUE_COMP_NAME = 0x0001

# A value's data lives in the vk cell itself when it is 4 bytes or less; past this size
# hives of version 1.4 and later split it into a `db` record with segments.
RESIDENT_FLAG = 0x80000000
BIG_DATA_THRESHOLD = 16344

REG_NONE = 0
REG_SZ = 1
REG_EXPAND_SZ = 2
REG_BINARY = 3
REG_DWORD = 4
REG_MULTI_SZ = 7


class HiveError(Exception):
    pass


@dataclass
class Value:
    name: str
    type: int
    data: bytes
    flags: int = VALUE_COMP_NAME
    spare: int = 0


@dataclass
class Key:
    name: str
    flags: int = KEY_COMP_NAME
    timestamp: int = 0
    access: int = 0
    workvar: int = 0
    class_name: bytes | None = None
    security: bytes = b""
    values: list[Value] = field(default_factory=list)
    subkeys: list[Key] = field(default_factory=list)
    # Only the root key's parent field points outside the tree; it is kept as found.
    raw_parent: int = NO_CELL
    # The upper half of the max-subkey-name field carries flags (virtualization, user flags)
    # rather than a length.
    max_subkey_name_hi: int = 0

    def subkey(self, name: str) -> Key | None:
        folded = name.upper()
        for key in self.subkeys:
            if key.name.upper() == folded:
                return key
        return None

    def value(self, name: str) -> Value | None:
        folded = name.upper()
        for value in self.values:
            if value.name.upper() == folded:
                return value
        return None

    def add_subkey(self, name: str) -> Key:
        if self.subkey(name) is not None:
            raise HiveError(f"key {name!r} already exists")
        key = Key(name, timestamp=filetime_now(), security=self.security)
        self.subkeys.append(key)
        self.timestamp = filetime_now()
        return key

    def remove_subkey(self, name: str) -> None:
        key = self.subkey(name)
        if key is None:
            raise HiveError(f"no key {name!r}")
        self.subkeys.remove(key)
        self.timestamp = filetime_now()

    def set_value(self, name: str, type: int, data: bytes) -> None:
        existing = self.value(name)
        if existing is None:
            self.values.append(Value(name, type, data))
        else:
            existing.type, existing.data = type, data
        self.timestamp = filetime_now()

    def remove_value(self, name: str) -> None:
        existing = self.value(name)
        if existing is None:
            raise HiveError(f"no value {name!r}")
        self.values.remove(existing)
        self.timestamp = filetime_now()


@dataclass
class Hive:
    base_block: bytes
    root: Key
    # The derived fields of every nk cell as found in the file, keyed by path: the tests compare
    # these with what the writer computes.
    stored_derived: dict[str, tuple] = field(default_factory=dict, repr=False)

    @property
    def sequence(self) -> int:
        return struct.unpack_from("<I", self.base_block, 4)[0]


def filetime_now() -> int:
    return int(time.time() * 10_000_000) + 116444736000000000


def lh_hash(name: str) -> int:
    value = 0
    for char in name.upper():
        value = (value * 37 + ord(char)) & 0xFFFFFFFF
    return value


def base_block_checksum(block: bytes) -> int:
    value = 0
    for (word,) in struct.iter_unpack("<I", block[:508]):
        value ^= word
    if value == 0xFFFFFFFF:
        return 0xFFFFFFFE
    if value == 0:
        return 1
    return value


def _decode_name(raw: bytes, compressed: bool) -> str:
    return raw.decode("latin-1") if compressed else raw.decode("utf-16-le")


def _encode_name(name: str) -> tuple[bytes, bool]:
    try:
        return name.encode("latin-1"), True
    except UnicodeEncodeError:
        return name.encode("utf-16-le"), False


def _utf16_length(name: str) -> int:
    return len(name.encode("utf-16-le"))


# ---------------------------------------------------------------------------------------------
# Reading


class _Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.bins_size = struct.unpack_from("<I", data, 0x28)[0]

    def cell(self, offset: int) -> bytes:
        if offset == NO_CELL or offset + 4 > self.bins_size:
            raise HiveError(f"cell offset {offset:#x} outside the hive bins")
        start = BASE_BLOCK_SIZE + offset
        size = struct.unpack_from("<i", self.data, start)[0]
        if size >= 0:
            raise HiveError(f"cell at {offset:#x} is free")
        size = -size
        if size < 8 or offset + size > self.bins_size:
            raise HiveError(f"cell at {offset:#x} has a bad size {size}")
        return self.data[start + 4 : start + size]

    def subkey_offsets(self, offset: int) -> list[int]:
        cell = self.cell(offset)
        sig = cell[:2]
        count = struct.unpack_from("<H", cell, 2)[0]
        if sig in (b"lf", b"lh"):
            return [struct.unpack_from("<I", cell, 4 + 8 * i)[0] for i in range(count)]
        if sig == b"li":
            return [struct.unpack_from("<I", cell, 4 + 4 * i)[0] for i in range(count)]
        if sig == b"ri":
            offsets: list[int] = []
            for i in range(count):
                offsets += self.subkey_offsets(struct.unpack_from("<I", cell, 4 + 4 * i)[0])
            return offsets
        raise HiveError(f"unknown subkey list {sig!r} at {offset:#x}")

    def value_data(self, size_field: int, offset: int) -> bytes:
        if size_field & RESIDENT_FLAG:
            size = size_field & ~RESIDENT_FLAG
            if size > 4:
                raise HiveError("resident value data longer than 4 bytes")
            return struct.pack("<I", offset)[:size]
        size = size_field
        if size == 0:
            return b""
        cell = self.cell(offset)
        if size > BIG_DATA_THRESHOLD and cell[:2] == b"db":
            count, segments = struct.unpack_from("<HI", cell, 2)
            seg_list = self.cell(segments)
            out = b""
            for i in range(count):
                seg = struct.unpack_from("<I", seg_list, 4 * i)[0]
                out += self.cell(seg)[:BIG_DATA_THRESHOLD]
            return out[:size]
        if size > len(cell):
            raise HiveError(f"value data at {offset:#x} is shorter than its size {size}")
        return cell[:size]

    def value(self, offset: int) -> Value:
        cell = self.cell(offset)
        if cell[:2] != b"vk":
            raise HiveError(f"expected a vk cell at {offset:#x}")
        name_len, size_field, data_offset, vtype, flags, spare = struct.unpack_from("<HIIIHH", cell, 2)
        name = _decode_name(cell[20 : 20 + name_len], bool(flags & VALUE_COMP_NAME))
        return Value(name, vtype, self.value_data(size_field, data_offset), flags, spare)

    def key(self, offset: int, path: str, derived: dict[str, tuple]) -> Key:
        cell = self.cell(offset)
        if cell[:2] != b"nk":
            raise HiveError(f"expected an nk cell at {offset:#x}")
        (flags, timestamp, access, parent, n_sub, _n_vol, sub_list, _vol_list, n_values, value_list,
         sk, class_off, max_sub_name, max_sub_class, max_value_name, max_value_data, workvar,
         name_len, class_len) = struct.unpack_from("<HQIIIIIIIIIIIIIIIHH", cell, 2)
        name = _decode_name(cell[76 : 76 + name_len], bool(flags & KEY_COMP_NAME))
        key = Key(name, flags, timestamp, access, workvar, raw_parent=parent,
                  max_subkey_name_hi=max_sub_name >> 16)
        if class_off != NO_CELL and class_len:
            key.class_name = self.cell(class_off)[:class_len]
        if sk != NO_CELL:
            sk_cell = self.cell(sk)
            if sk_cell[:2] != b"sk":
                raise HiveError(f"expected an sk cell at {sk:#x}")
            size = struct.unpack_from("<I", sk_cell, 16)[0]
            key.security = sk_cell[20 : 20 + size]
        if n_values:
            vl = self.cell(value_list)
            key.values = [self.value(struct.unpack_from("<I", vl, 4 * i)[0]) for i in range(n_values)]
        if n_sub:
            offsets = self.subkey_offsets(sub_list)
            if len(offsets) != n_sub:
                raise HiveError(f"{path}: subkey count {n_sub} but the list holds {len(offsets)}")
            key.subkeys = [self.key(o, f"{path}\\{n}", derived) for o, n in
                           ((o, self._name_at(o)) for o in offsets)]
        derived[path] = (n_sub, n_values, max_sub_name & 0xFFFF, max_sub_class, max_value_name,
                         max_value_data)
        return key

    def _name_at(self, offset: int) -> str:
        cell = self.cell(offset)
        flags = struct.unpack_from("<H", cell, 2)[0]
        name_len = struct.unpack_from("<H", cell, 72)[0]
        return _decode_name(cell[76 : 76 + name_len], bool(flags & KEY_COMP_NAME))


def read(data: bytes) -> Hive:
    if len(data) < BASE_BLOCK_SIZE or data[:4] != b"regf":
        raise HiveError("not a registry hive (no regf signature)")
    seq1, seq2 = struct.unpack_from("<II", data, 4)
    if seq1 != seq2:
        raise HiveError(f"hive is dirty (sequence {seq1} != {seq2}); replay its logs on Windows first")
    if base_block_checksum(data) != struct.unpack_from("<I", data, 0x1FC)[0]:
        raise HiveError("base block checksum mismatch")
    major, minor = struct.unpack_from("<II", data, 0x14)
    if major != 1 or minor < 3:
        raise HiveError(f"unsupported hive version {major}.{minor}")
    reader = _Reader(data)
    if BASE_BLOCK_SIZE + reader.bins_size > len(data):
        raise HiveError("hive bins run past the end of the file")
    root_offset = struct.unpack_from("<I", data, 0x24)[0]
    derived: dict[str, tuple] = {}
    root = reader.key(root_offset, "", derived)
    return Hive(data[:BASE_BLOCK_SIZE], root, derived)


# ---------------------------------------------------------------------------------------------
# Writing


def derived_fields(key: Key) -> tuple:
    """The nk fields the format derives from a key's contents, as the reader reports them."""
    return (
        len(key.subkeys),
        len(key.values),
        max((_utf16_length(k.name) for k in key.subkeys), default=0),
        max((len(k.class_name or b"") for k in key.subkeys), default=0),
        max((_utf16_length(v.name) for v in key.values), default=0),
        max((len(v.data) for v in key.values), default=0),
    )


class _Writer:
    """Lays cells out one after another in 4 KiB hive bins. A cell never crosses a bin boundary,
    so a cell that does not fit in what is left of the current bin starts a new one, and the
    leftover becomes a free cell."""

    def __init__(self) -> None:
        self.bins: list[bytearray] = []
        self.used = 0  # bytes used in the last bin

    def _offset(self) -> int:
        return sum(len(b) for b in self.bins[:-1]) + self.used

    def _new_bin(self, need: int) -> None:
        self._close_bin()
        size = max(HBIN_SIZE, -(-(need + HBIN_HEADER_SIZE) // HBIN_SIZE) * HBIN_SIZE)
        offset = sum(len(b) for b in self.bins)
        hbin = bytearray(size)
        struct.pack_into("<4sII", hbin, 0, b"hbin", offset, size)
        self.bins.append(hbin)
        self.used = HBIN_HEADER_SIZE

    def _close_bin(self) -> None:
        if self.bins and self.used < len(self.bins[-1]):
            struct.pack_into("<i", self.bins[-1], self.used, len(self.bins[-1]) - self.used)
            self.used = len(self.bins[-1])

    def alloc(self, payload_size: int) -> int:
        size = -(-(payload_size + 4) // 8) * 8
        if not self.bins or self.used + size > len(self.bins[-1]):
            self._new_bin(size)
        offset = self._offset()
        struct.pack_into("<i", self.bins[-1], self.used, -size)
        self.used += size
        return offset

    def put(self, offset: int, payload: bytes) -> None:
        # Offsets are relative to the first bin; find the bin holding this one.
        start = 0
        for hbin in self.bins:
            if offset < start + len(hbin):
                pos = offset - start + 4
                hbin[pos : pos + len(payload)] = payload
                return
            start += len(hbin)
        raise HiveError(f"offset {offset:#x} not allocated")

    def store(self, payload: bytes) -> int:
        offset = self.alloc(len(payload))
        self.put(offset, payload)
        return offset

    def finish(self) -> bytes:
        self._close_bin()
        return b"".join(bytes(b) for b in self.bins)


def _sorted_subkeys(key: Key) -> list[Key]:
    return sorted(key.subkeys, key=lambda k: k.name.upper())


def write(hive: Hive) -> bytes:
    w = _Writer()
    nk_offsets: dict[int, int] = {}
    sk_offsets: dict[bytes, int] = {}
    sk_refs: dict[bytes, int] = {}

    def walk(key: Key):
        yield key
        for sub in _sorted_subkeys(key):
            yield from walk(sub)

    keys = list(walk(hive.root))
    if not hive.root.security:
        raise HiveError("the root key has no security descriptor")
    for key in keys:
        if not key.security:
            key.security = hive.root.security
        sk_refs[key.security] = sk_refs.get(key.security, 0) + 1

    # nk cells first, in tree order, so the root sits at the start of the first bin as in
    # bcdedit's files. Their contents are filled in once every other cell has an offset.
    for key in keys:
        name, _ = _encode_name(key.name)
        nk_offsets[id(key)] = w.alloc(76 + len(name))

    descriptors = list(sk_refs)
    for desc in descriptors:
        sk_offsets[desc] = w.alloc(20 + len(desc))
    for i, desc in enumerate(descriptors):
        flink = sk_offsets[descriptors[(i + 1) % len(descriptors)]]
        blink = sk_offsets[descriptors[i - 1]]
        w.put(sk_offsets[desc], struct.pack("<2sHIIII", b"sk", 0, flink, blink, sk_refs[desc], len(desc)) + desc)

    parents: dict[int, int] = {}
    for key in keys:
        for sub in key.subkeys:
            parents[id(sub)] = nk_offsets[id(key)]

    for key in keys:
        subs = _sorted_subkeys(key)
        sub_list = NO_CELL
        if subs:
            body = b"".join(struct.pack("<II", nk_offsets[id(k)], lh_hash(k.name)) for k in subs)
            sub_list = w.store(struct.pack("<2sH", b"lh", len(subs)) + body)

        value_list = NO_CELL
        if key.values:
            vk_offsets = []
            for value in key.values:
                vname, compressed = _encode_name(value.name)
                vflags = (value.flags | VALUE_COMP_NAME) if compressed else (value.flags & ~VALUE_COMP_NAME)
                if len(value.data) <= 4:
                    size_field = len(value.data) | RESIDENT_FLAG
                    data_field = struct.unpack("<I", value.data.ljust(4, b"\0"))[0]
                    if not value.data:
                        size_field, data_field = 0, NO_CELL
                elif len(value.data) > BIG_DATA_THRESHOLD:
                    raise HiveError(f"value {value.name!r} is too big for this writer")
                else:
                    size_field = len(value.data)
                    data_field = w.store(value.data)
                vk_offsets.append(w.store(struct.pack("<2sHIIIHH", b"vk", len(vname), size_field, data_field,
                                                      value.type, vflags, value.spare) + vname))
            value_list = w.store(b"".join(struct.pack("<I", o) for o in vk_offsets))

        class_off = NO_CELL
        if key.class_name:
            class_off = w.store(key.class_name)

        name, compressed = _encode_name(key.name)
        flags = (key.flags | KEY_COMP_NAME) if compressed else (key.flags & ~KEY_COMP_NAME)
        n_sub, n_values, max_name, max_class, max_vname, max_vdata = derived_fields(key)
        parent = key.raw_parent if key is hive.root else parents[id(key)]
        nk = struct.pack(
            "<2sHQIIIIIIIIIIIIIIIHH", b"nk", flags, key.timestamp, key.access, parent, n_sub, 0, sub_list,
            NO_CELL, n_values, value_list, sk_offsets[key.security], class_off,
            max_name | (key.max_subkey_name_hi << 16), max_class, max_vname, max_vdata, key.workvar,
            len(name), len(key.class_name or b""),
        ) + name
        w.put(nk_offsets[id(key)], nk)

    bins = w.finish()
    base = bytearray(hive.base_block)
    sequence = hive.sequence + 1
    struct.pack_into("<IIQ", base, 4, sequence, sequence, filetime_now())
    struct.pack_into("<II", base, 0x24, nk_offsets[id(hive.root)], len(bins))
    struct.pack_into("<I", base, 0x1FC, base_block_checksum(bytes(base)))
    return bytes(base) + bins
