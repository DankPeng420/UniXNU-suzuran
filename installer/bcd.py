r"""Edit a Windows BCD store the way bcdedit does, for the Lumia's boot route to iOS.

A BCD store is a registry hive (regf.py) with one key per boot object:

    Objects\{guid}\Description   value Type (REG_DWORD): the object type, e.g. 0x10100002 boot manager
    Objects\{guid}\Elements\XXXXXXXX\Element   one value per setting, named by its element id

The element id says how its value is stored (bits 24-27): 1 device and 5/6/7 integers, booleans and
integer lists are REG_BINARY, 2 strings and 3 object GUIDs are REG_SZ, 4 object lists are
REG_MULTI_SZ. Only the settings this port needs are named here.

The edits are the ones docs/lumia950xl/INSTALL.md did with bcdedit on a Windows PC:

    bcdedit /copy {default} /d "iOS (UEFI)"          -> add_boot_entry()
    bcdedit /set {new} path \...\BootShimAA64.efi    -> (same)
    bcdedit /displayorder {new} /addlast             -> (same)
    bcdedit /bootsequence {new}                      -> set_one_shot()
    bcdedit /set {bootmgr} persistbootsequence No    -> (same)

tests/test_bcd.py replays them on the phone's original store and compares the result, element by
element, with the store bcdedit produced from the same commands.
"""
from __future__ import annotations

import copy
import re
import uuid
from pathlib import PureWindowsPath

import regf

BOOTMGR = "{9dea862c-5cdd-4e70-acc1-f32b344d4795}"

DEVICE = 0x11000001  # ApplicationDevice
PATH = 0x12000002  # ApplicationPath
DESCRIPTION = 0x12000004
DEFAULT = 0x23000003  # {bootmgr} default object
DISPLAY_ORDER = 0x24000001
BOOT_SEQUENCE = 0x24000002
DISPLAY_BOOT_MENU = 0x26000020
PERSIST_BOOT_SEQUENCE = 0x26000031

FORMAT_DEVICE = 1
FORMAT_STRING = 2
FORMAT_OBJECT = 3
FORMAT_OBJECT_LIST = 4
FORMAT_INTEGER = 5
FORMAT_BOOLEAN = 6
FORMAT_INTEGER_LIST = 7

_GUID = re.compile(r"\{[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\}")


class BcdError(Exception):
    pass


def element_format(element: int) -> int:
    return (element >> 24) & 0xF


def _check_guid(guid: str) -> str:
    guid = guid.lower()
    if not _GUID.fullmatch(guid):
        raise BcdError(f"not a BCD object GUID: {guid!r}")
    return guid


def _utf16z(text: str) -> bytes:
    return (text + "\0").encode("utf-16-le")


class Store:
    def __init__(self, data: bytes):
        self.hive = regf.read(data)
        objects = self.hive.root.subkey("Objects")
        if objects is None or self.hive.root.subkey("Description") is None:
            raise BcdError("this hive is not a BCD store (no Objects or Description key)")
        self._objects = objects
        if self._object(BOOTMGR) is None:
            raise BcdError("the store has no Windows Boot Manager object")

    def save(self) -> bytes:
        return regf.write(self.hive)

    # -- objects --------------------------------------------------------------------------

    def objects(self) -> list[str]:
        return [key.name.lower() for key in self._objects.subkeys]

    def _object(self, guid: str) -> regf.Key | None:
        return self._objects.subkey(_check_guid(guid))

    def _need(self, guid: str) -> regf.Key:
        key = self._object(guid)
        if key is None:
            raise BcdError(f"no object {guid}")
        return key

    def object_type(self, guid: str) -> int:
        description = self._need(guid).subkey("Description")
        value = description.value("Type") if description else None
        if value is None or value.type != regf.REG_DWORD:
            raise BcdError(f"object {guid} has no type")
        return int.from_bytes(value.data, "little")

    def copy_object(self, source: str, new: str | None = None) -> str:
        new = _check_guid(new or "{" + str(uuid.uuid4()) + "}")
        if self._object(new) is not None:
            raise BcdError(f"object {new} already exists")
        key = copy.deepcopy(self._need(source))
        key.name = new

        def touch(k: regf.Key) -> None:
            k.timestamp = regf.filetime_now()
            for sub in k.subkeys:
                touch(sub)

        touch(key)
        self._objects.subkeys.append(key)
        return new

    # -- elements -------------------------------------------------------------------------

    def _elements(self, guid: str, create: bool = False) -> regf.Key | None:
        obj = self._need(guid)
        elements = obj.subkey("Elements")
        if elements is None and create:
            elements = obj.add_subkey("Elements")
        return elements

    def elements(self, guid: str) -> list[int]:
        elements = self._elements(guid)
        return sorted(int(key.name, 16) for key in elements.subkeys) if elements else []

    def raw(self, guid: str, element: int) -> bytes | None:
        elements = self._elements(guid)
        key = elements.subkey(f"{element:08x}") if elements else None
        value = key.value("Element") if key else None
        return value.data if value else None

    def _set(self, guid: str, element: int, vtype: int, data: bytes) -> None:
        elements = self._elements(guid, create=True)
        name = f"{element:08x}"
        key = elements.subkey(name) or elements.add_subkey(name)
        key.set_value("Element", vtype, data)

    def delete(self, guid: str, element: int) -> None:
        elements = self._elements(guid)
        if elements is not None and elements.subkey(f"{element:08x}") is not None:
            elements.remove_subkey(f"{element:08x}")

    def _expect(self, element: int, fmt: int) -> None:
        if element_format(element) != fmt:
            raise BcdError(f"element {element:#010x} is not of format {fmt}")

    def string(self, guid: str, element: int) -> str | None:
        data = self.raw(guid, element)
        return None if data is None else data.decode("utf-16-le").split("\0", 1)[0]

    def set_string(self, guid: str, element: int, text: str) -> None:
        if element_format(element) not in (FORMAT_STRING, FORMAT_OBJECT):
            raise BcdError(f"element {element:#010x} does not hold a string")
        self._set(guid, element, regf.REG_SZ, _utf16z(text))

    def object_list(self, guid: str, element: int) -> list[str] | None:
        self._expect(element, FORMAT_OBJECT_LIST)
        data = self.raw(guid, element)
        if data is None:
            return None
        return [item.lower() for item in data.decode("utf-16-le").split("\0") if item]

    def set_object_list(self, guid: str, element: int, guids: list[str]) -> None:
        self._expect(element, FORMAT_OBJECT_LIST)
        guids = [_check_guid(g) for g in guids]
        self._set(guid, element, regf.REG_MULTI_SZ, "".join(g + "\0" for g in guids).encode("utf-16-le") + b"\0\0")

    def boolean(self, guid: str, element: int) -> bool | None:
        self._expect(element, FORMAT_BOOLEAN)
        data = self.raw(guid, element)
        return None if data is None else any(data)

    def set_boolean(self, guid: str, element: int, value: bool) -> None:
        self._expect(element, FORMAT_BOOLEAN)
        self._set(guid, element, regf.REG_BINARY, b"\x01" if value else b"\x00")

    # -- the edits the installer makes --------------------------------------------------------

    def default_entry(self) -> str:
        default = self.string(BOOTMGR, DEFAULT)
        if default is None:
            raise BcdError("{bootmgr} has no default entry")
        return _check_guid(default)

    def find_entry(self, path: str) -> str | None:
        """The boot application whose path is `path` (compared case-insensitively), if any."""
        wanted = str(PureWindowsPath(path)).lower()
        for guid in self.objects():
            current = self.string(guid, PATH)
            if current is not None and str(PureWindowsPath(current)).lower() == wanted:
                return guid
        return None

    def add_boot_entry(self, path: str, description: str, template: str | None = None,
                       new: str | None = None) -> str:
        """Copy a boot application (default: {bootmgr}'s default, the Boot Shim that starts lk2nd),
        point the copy at `path` and append it to the display order. Copying keeps the template's
        device element (EFIESP) and its integrity-check flags, as `bcdedit /copy` does."""
        if not path.startswith("\\"):
            raise BcdError(f"path must be absolute on EFIESP: {path!r}")
        template = template or self.default_entry()
        if self.raw(template, PATH) is None or self.raw(template, DEVICE) is None:
            raise BcdError(f"template {template} is not a boot application with a device and path")
        guid = self.copy_object(template, new)
        self.set_string(guid, PATH, path)
        self.set_string(guid, DESCRIPTION, description)
        order = self.object_list(BOOTMGR, DISPLAY_ORDER) or []
        self.set_object_list(BOOTMGR, DISPLAY_ORDER, [g for g in order if g != guid] + [guid])
        return guid

    def set_one_shot(self, guid: str) -> None:
        """Boot `guid` once: Windows Boot Manager deletes a non-persistent boot sequence after
        using it, so the boot after that goes back to {bootmgr}'s default."""
        self._need(guid)
        self.set_object_list(BOOTMGR, BOOT_SEQUENCE, [guid])
        self.set_boolean(BOOTMGR, PERSIST_BOOT_SEQUENCE, False)

    def clear_boot_sequence(self) -> None:
        self.delete(BOOTMGR, BOOT_SEQUENCE)

    def describe(self) -> str:
        """A short listing like `bcdedit /enum`, for the installer's log and the user."""
        lines = []
        for guid in [BOOTMGR] + [g for g in self.objects() if g != BOOTMGR]:
            if self.raw(guid, PATH) is None:
                continue
            lines.append(guid)
            lines.append(f"  description  {self.string(guid, DESCRIPTION)}")
            lines.append(f"  path         {self.string(guid, PATH)}")
            if guid == BOOTMGR:
                lines.append(f"  default      {self.string(guid, DEFAULT)}")
                lines.append(f"  displayorder {' '.join(self.object_list(guid, DISPLAY_ORDER) or [])}")
                sequence = self.object_list(guid, BOOT_SEQUENCE)
                if sequence:
                    lines.append(f"  bootsequence {' '.join(sequence)}")
                persist = self.boolean(guid, PERSIST_BOOT_SEQUENCE)
                if persist is not None:
                    lines.append(f"  persistbootsequence {'Yes' if persist else 'No'}")
        return "\n".join(lines)
