"""The BCD editor against bcdedit's own output.

fixtures/ holds the phone's real BCD store (orig) and the stores bcdedit made from it on Windows
(docs/lumia950xl/INSTALL.md section 3), with the device elements and the GUIDs that identify the
phone or the PC scrambled the same way in every file:

    uefi          bcdedit /copy {default} /d "UEFI AArch64"; /set {new} path ...; /displayorder {new} /addlast;
                  /bootsequence {new}; /set {bootmgr} persistbootsequence No
    keys          from uefi: /deletevalue {bootmgr} bootsequence; /set {bootmgr} custom:54000001 {new};
                  /set {new} description "iOS (UEFI)"
    keys-oneshot  from keys: /bootsequence {new}
    menu          from uefi: /deletevalue {bootmgr} bootsequence; displaybootmenu Yes; timeout 10;
                  descriptions "iOS (UEFI)" and "Android"
    menu-oneshot  from menu: /bootsequence {new}

    python3 -m unittest discover -s installer/tests
"""
from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

import bcd  # noqa: E402
import regf  # noqa: E402

FIXTURES = HERE / "fixtures"
NEW = "{ef2737f6-d2ea-11f1-8f6f-00005e0053a1}"  # the GUID bcdedit gave the copy (scrubbed)
SHIM = "{5533c825-78dc-11ee-8858-00005e0053b2}"  # the Boot Shim that starts lk2nd
PATH = "\\Windows\\System32\\BOOT\\BootShimAA64.efi"
CUSTOM_VOLUME_UP = 0x54000001
TIMEOUT = 0x25000004


def load(name: str) -> bcd.Store:
    return bcd.Store((FIXTURES / f"{name}.bcd").read_bytes())


def contents(store: bcd.Store) -> dict:
    """Everything a BCD store says, without the hive's layout or timestamps."""
    out = {}
    for guid in store.objects():
        out[guid, "type"] = store.object_type(guid)
        for element in store.elements(guid):
            out[guid, element] = store.raw(guid, element)
    return out


def reopen(store: bcd.Store) -> bcd.Store:
    return bcd.Store(store.save())


class EditsMatchBcdedit(unittest.TestCase):
    def assertSameStore(self, ours: bcd.Store, theirs: bcd.Store) -> None:
        a, b = contents(ours), contents(theirs)
        self.assertEqual(sorted(a, key=str), sorted(b, key=str))
        for key in a:
            self.assertEqual(a[key], b[key], key)

    def test_add_uefi_entry_one_shot(self):
        store = load("orig")
        self.assertEqual(store.default_entry(), SHIM)
        guid = store.add_boot_entry(PATH, "UEFI AArch64", new=NEW)
        store.set_one_shot(guid)
        self.assertSameStore(reopen(store), load("uefi"))

    def test_key_boot(self):
        store = load("uefi")
        store.clear_boot_sequence()
        store.set_object_list(bcd.BOOTMGR, CUSTOM_VOLUME_UP, [NEW])
        store.set_string(NEW, bcd.DESCRIPTION, "iOS (UEFI)")
        self.assertSameStore(reopen(store), load("keys"))

        store.set_one_shot(NEW)
        self.assertSameStore(reopen(store), load("keys-oneshot"))

    def test_menu(self):
        store = load("uefi")
        store.clear_boot_sequence()
        store.set_boolean(bcd.BOOTMGR, bcd.DISPLAY_BOOT_MENU, True)
        store._set(bcd.BOOTMGR, TIMEOUT, regf.REG_BINARY, struct.pack("<Q", 10))
        store.set_string(NEW, bcd.DESCRIPTION, "iOS (UEFI)")
        store.set_string(SHIM, bcd.DESCRIPTION, "Android")
        self.assertSameStore(reopen(store), load("menu"))

    def test_find_entry_is_case_insensitive(self):
        store = load("uefi")
        self.assertEqual(store.find_entry(PATH.lower()), NEW)
        self.assertIsNone(load("orig").find_entry(PATH))

    def test_reads_do_not_change_the_store(self):
        store = load("orig")
        before = contents(store)
        for guid in store.objects():
            store.raw(guid, bcd.PATH)
        self.assertEqual(contents(reopen(store)), before)

    def test_refuses_relative_path_and_duplicate_guid(self):
        store = load("orig")
        with self.assertRaises(bcd.BcdError):
            store.add_boot_entry("BootShimAA64.efi", "x")
        with self.assertRaises(bcd.BcdError):
            store.add_boot_entry(PATH, "x", new=SHIM)

    def test_describe_lists_the_new_entry(self):
        text = load("uefi").describe()
        self.assertIn(NEW, text)
        self.assertIn("bootsequence " + NEW, text)
        self.assertIn("persistbootsequence No", text)


if __name__ == "__main__":
    unittest.main()
