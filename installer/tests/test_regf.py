"""The hive reader and writer: round trips, and the structure of what the writer lays out."""
from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

import regf  # noqa: E402

FIXTURES = sorted((HERE / "fixtures").glob("*.bcd"))


def same_tree(test: unittest.TestCase, a: regf.Key, b: regf.Key, path: str = "") -> None:
    test.assertEqual((a.name, a.flags, a.timestamp, a.access, a.workvar, a.class_name, a.security),
                     (b.name, b.flags, b.timestamp, b.access, b.workvar, b.class_name, b.security), path)
    test.assertEqual([(v.name, v.type, v.data, v.flags) for v in a.values],
                     [(v.name, v.type, v.data, v.flags) for v in b.values], path)
    # The writer stores subkeys in sorted order, as the format requires.
    a_subs = sorted(a.subkeys, key=lambda k: k.name.upper())
    test.assertEqual([k.name for k in a_subs], [k.name for k in b.subkeys], path)
    for x, y in zip(a_subs, b.subkeys):
        same_tree(test, x, y, f"{path}\\{x.name}")


def walk_cells(data: bytes):
    """Every cell of every hive bin: (offset, size, allocated, payload)."""
    bins_size = struct.unpack_from("<I", data, 0x28)[0]
    offset = 0
    while offset < bins_size:
        sig, bin_offset, bin_size = struct.unpack_from("<4sII", data, regf.BASE_BLOCK_SIZE + offset)
        assert sig == b"hbin" and bin_offset == offset and bin_size % regf.HBIN_SIZE == 0
        cell = offset + regf.HBIN_HEADER_SIZE
        while cell < offset + bin_size:
            size = struct.unpack_from("<i", data, regf.BASE_BLOCK_SIZE + cell)[0]
            length = abs(size)
            assert length % 8 == 0 and length >= 8 and cell + length <= offset + bin_size
            start = regf.BASE_BLOCK_SIZE + cell
            yield cell, length, size < 0, data[start + 4 : start + length]
            cell += length
        offset += bin_size
    assert offset == bins_size


class RoundTrip(unittest.TestCase):
    def test_fixtures_exist(self):
        self.assertGreaterEqual(len(FIXTURES), 6)

    def test_round_trip_keeps_the_tree(self):
        for path in FIXTURES:
            with self.subTest(path.name):
                hive = regf.read(path.read_bytes())
                again = regf.read(regf.write(hive))
                same_tree(self, hive.root, again.root)
                self.assertEqual(again.sequence, hive.sequence + 1)

    def test_derived_fields_match_bcdedit(self):
        # Windows never shrinks the max-length fields and leaves stale ones on keys that lost
        # their subkeys, so bcdedit's values may be larger than the exact ones; never smaller.
        for path in FIXTURES:
            hive = regf.read(path.read_bytes())

            def check(key: regf.Key, where: str) -> None:
                ours = regf.derived_fields(key)
                stored = hive.stored_derived[where]
                self.assertEqual(ours[:2], stored[:2], where)
                for mine, theirs in zip(ours[2:], stored[2:]):
                    self.assertLessEqual(mine, theirs, where)
                for sub in key.subkeys:
                    check(sub, f"{where}\\{sub.name}")

            with self.subTest(path.name):
                check(hive.root, "")

    def test_written_layout(self):
        for path in FIXTURES:
            with self.subTest(path.name):
                data = regf.write(regf.read(path.read_bytes()))
                self.assertEqual(struct.unpack_from("<I", data, 0x1FC)[0], regf.base_block_checksum(data))
                seq1, seq2 = struct.unpack_from("<II", data, 4)
                self.assertEqual(seq1, seq2)
                cells = {off: (alloc, payload) for off, _, alloc, payload in walk_cells(data)}
                sk_refs: dict[int, int] = {}
                for alloc, payload in cells.values():
                    if not alloc:
                        continue
                    if payload[:2] == b"lh":
                        count = struct.unpack_from("<H", payload, 2)[0]
                        names = []
                        for i in range(count):
                            nk, hashed = struct.unpack_from("<II", payload, 4 + 8 * i)
                            nk_payload = cells[nk][1]
                            name_len = struct.unpack_from("<H", nk_payload, 72)[0]
                            name = nk_payload[76 : 76 + name_len].decode("latin-1")
                            self.assertEqual(hashed, regf.lh_hash(name))
                            names.append(name.upper())
                        self.assertEqual(names, sorted(names))
                    if payload[:2] == b"nk":
                        sk = struct.unpack_from("<I", payload, 44)[0]
                        sk_refs[sk] = sk_refs.get(sk, 0) + 1
                for sk, refs in sk_refs.items():
                    self.assertEqual(cells[sk][1][:2], b"sk")
                    self.assertEqual(struct.unpack_from("<I", cells[sk][1], 12)[0], refs)

    def test_rejects_dirty_and_corrupt_hives(self):
        data = bytearray(FIXTURES[0].read_bytes())
        dirty = bytearray(data)
        struct.pack_into("<I", dirty, 8, struct.unpack_from("<I", dirty, 4)[0] + 1)
        struct.pack_into("<I", dirty, 0x1FC, regf.base_block_checksum(bytes(dirty)))
        with self.assertRaisesRegex(regf.HiveError, "dirty"):
            regf.read(bytes(dirty))
        corrupt = bytearray(data)
        corrupt[0x40] ^= 1
        with self.assertRaisesRegex(regf.HiveError, "checksum"):
            regf.read(bytes(corrupt))
        with self.assertRaises(regf.HiveError):
            regf.read(b"not a hive" * 500)

    def test_edits_survive_a_round_trip(self):
        hive = regf.read(FIXTURES[0].read_bytes())
        key = hive.root.add_subkey("Test")
        key.set_value("Short", regf.REG_DWORD, b"\x01\x00\x00\x00")
        key.set_value("Long", regf.REG_BINARY, bytes(range(256)) * 20)
        key.set_value("Empty", regf.REG_SZ, b"")
        again = regf.read(regf.write(hive))
        test = again.root.subkey("TEST")
        self.assertEqual(test.value("short").data, b"\x01\x00\x00\x00")
        self.assertEqual(test.value("Long").data, bytes(range(256)) * 20)
        self.assertEqual(test.value("Empty").data, b"")
        self.assertEqual(test.security, hive.root.security)


if __name__ == "__main__":
    unittest.main()
