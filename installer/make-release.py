#!/usr/bin/env python3
r"""Package a kernel and the boot menu as the release zip the installer downloads.

    python3 installer/make-release.py --version 2026.11.01 \
        --kernel ../UniXNU/out/lumia950/kernel/kernel.macho --menu-tree /path/to/menu \
        --args "-v cpus=8 sheng-smp=1 ... rd=disk0s55 ..." --out unixnu-lumia950xl-2026.11.01.zip

The zip holds:

    kernel.macho   the prelinked kernel (`unixnu build kernel`)
    args.txt       its boot arguments; the installer rewrites rd=disk0sN for the phone's ios partition
    menu/EFI/...   the UniXNU menu + DarwinBoot (scripts/build-payload.sh), unstaged: the installer
                   runs scripts/stage-kernel.py on the phone owner's Mac, because the ADT carries a
                   per-phone seed and the trust cache comes from the IPSW they download
    VERSION, SHA256SUMS

Entries are written in a fixed order with a fixed timestamp, so the same inputs give the same zip
and the same SHA-256. The [release] block for installer/manifest.toml is printed at the end.

--scrub-user NAME replaces NAME, wherever it appears in the binaries, with a placeholder of the same
length. The kernel's version string records the build user ("NAME:ios-obj") and both the kernel and
the EFI binaries carry the build machine's paths (__FILE__ strings, the PDB path); keeping the length
keeps every offset in the files, so only those bytes of text change. The zip is refused if NAME is
still anywhere in it afterwards.
"""
from __future__ import annotations

import argparse
import hashlib
import re
import sys
import zipfile
from pathlib import Path

MH_MAGIC_64 = 0xFEEDFACF
CPU_TYPE_ARM64 = 0x0100000C
MARKER = "# stage-kernel.py: kernel entry"
MENU_FILES = ["EFI/BOOT/BOOTAA64.EFI", "EFI/UniXNU/UniXNU.efi", "EFI/UniXNU/unixnu.toml",
              "EFI/UniXNU/Drivers/DarwinBoot.efi"]
EPOCH = (2026, 1, 1, 0, 0, 0)
ARGUMENT = re.compile(r"[A-Za-z0-9_.\-]+(=[A-Za-z0-9_.,\-]+)?")


def fail(message: str):
    sys.exit(f"error: {message}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--version", required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--menu-tree", type=Path, required=True)
    parser.add_argument("--args", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--repo", default="shrimpapplepro/UniXNU-lumia950xl")
    parser.add_argument("--scrub-user", help="the build user name to replace in the binaries")
    args = parser.parse_args()

    head = args.kernel.read_bytes()[:8]
    if int.from_bytes(head[:4], "little") != MH_MAGIC_64 or int.from_bytes(head[4:8], "little") != CPU_TYPE_ARM64:
        fail(f"{args.kernel} is not an arm64 Mach-O")
    for rel in MENU_FILES:
        if not (args.menu_tree / rel).is_file():
            fail(f"{args.menu_tree} has no {rel}; pass the scripts/build-payload.sh output")
    toml = (args.menu_tree / "EFI/UniXNU/unixnu.toml").read_text()
    if toml.count(MARKER) != 1 or toml.count('default = "exit"') != 1:
        fail("the menu's unixnu.toml is not the unstaged one (needs the stage-kernel.py marker and "
             'default = "exit")')
    words = args.args.split()
    if not all(ARGUMENT.fullmatch(w) for w in words) or not any(re.fullmatch(r"rd=disk0s\d+", w) for w in words):
        fail("--args must be plain kernel arguments including rd=disk0sN")
    if not re.fullmatch(r"[0-9A-Za-z.\-]+", args.version):
        fail("--version must be a plain tag")

    entries = [("kernel.macho", args.kernel.read_bytes()),
               ("args.txt", (" ".join(words) + "\n").encode()),
               ("VERSION", (args.version + "\n").encode())]
    entries += [(f"menu/{rel}", (args.menu_tree / rel).read_bytes()) for rel in MENU_FILES]
    if args.scrub_user:
        name = args.scrub_user.encode()
        placeholder = (b"builder" + b"x" * len(name))[: len(name)]
        scrubbed = []
        for entry, data in entries:
            count = data.count(name)
            data = data.replace(name, placeholder)
            if name.lower() in data.lower():
                fail(f"{entry} still contains {args.scrub_user!r} in another case")
            if count:
                print(f"scrubbed {count} x {args.scrub_user!r} in {entry}")
            scrubbed.append((entry, data))
        entries = scrubbed
    sums = "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in entries)
    entries.append(("SHA256SUMS", sums.encode()))

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.out, "w") as zf:
        for name, data in entries:
            info = zipfile.ZipInfo(name, EPOCH)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            zf.writestr(info, data, compresslevel=9)
    digest = hashlib.sha256(args.out.read_bytes()).hexdigest()
    print(sums, end="")
    print(f"\n{args.out}: {args.out.stat().st_size:,} B\n")
    print("[release]")
    print(f'tag = "{args.version}"')
    print(f'url = "https://github.com/{args.repo}/releases/download/{args.version}/{args.out.name}"')
    print(f'sha256 = "{digest}"')
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
