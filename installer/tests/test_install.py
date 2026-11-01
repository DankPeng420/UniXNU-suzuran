"""The installer's write path and helpers, without a phone.

A fake `adb` (a shell script) runs `adb shell -T CMD` locally, so the root-image write goes through
the same gzip stream, pipe and `gzip -dc | dd` command it uses on the phone, into a local file.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

import install  # noqa: E402
import phone  # noqa: E402

FAKE_ADB = """#!/bin/sh
# adb shell [-T] CMD -> sh -c CMD, locally
[ "$1" = shell ] || exit 1
shift
[ "$1" = -T ] && shift
exec sh -c "$1"
"""


class Helpers(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)

    def context(self, **extra) -> install.Context:
        args = argparse.Namespace(workdir=self.tmp / "work", adb="adb", dry_run=False, verbose=False, **extra)
        ctx = install.Context(args)
        self.addCleanup(ctx.close)
        return ctx

    def test_kernel_args_take_the_phones_partition_index(self):
        ctx = self.context()
        release = install.release_dir(ctx)
        release.mkdir(parents=True)
        (release / "args.txt").write_text("-v cpus=8 rd=disk0s55 rootrw=1\n")
        self.assertEqual(install.kernel_args(ctx, 57), "-v cpus=8 rd=disk0s57 rootrw=1")
        (release / "args.txt").write_text("-v cpus=8\n")
        with self.assertRaises(install.InstallError):
            install.kernel_args(ctx, 57)

    def test_release_zip_is_checked_against_its_sums(self):
        good = self.tmp / "rel.zip"
        with zipfile.ZipFile(good, "w") as zf:
            zf.writestr("kernel.macho", b"k")
            zf.writestr("VERSION", "t1\n")
            zf.writestr("SHA256SUMS", f"{hashlib.sha256(b'k').hexdigest()}  kernel.macho\n")
        ctx = self.context(release_zip=good)
        install.fetch_release(ctx)
        self.assertEqual(ctx.state["release_version"], "t1")

        bad = self.tmp / "bad.zip"
        with zipfile.ZipFile(bad, "w") as zf:
            zf.writestr("kernel.macho", b"tampered")
            zf.writestr("VERSION", "t2\n")
            zf.writestr("SHA256SUMS", f"{hashlib.sha256(b'k').hexdigest()}  kernel.macho\n")
        ctx.args.release_zip = bad
        with self.assertRaisesRegex(install.InstallError, "SHA256SUMS"):
            install.fetch_release(ctx)

    def test_run_hides_the_chip_seed_from_the_log(self):
        ctx = self.context()
        ctx.run(["true", "--unique-chip-id", "0123456789abcdef", "--x", "y"])
        log = (ctx.work / "install.log").read_text()
        self.assertNotIn("0123456789abcdef", log)
        self.assertIn("<seed>", log)

    def test_stream_write_through_a_fake_adb(self):
        adb = self.tmp / "adb"
        adb.write_text(FAKE_ADB)
        adb.chmod(0o755)
        source = self.tmp / "image.raw"
        data = os.urandom(3 << 20) + bytes(5 << 20) + os.urandom(12345)
        source.write_bytes(data)
        target = self.tmp / "target.bin"
        digest = hashlib.sha256()
        handset = phone.Phone(str(adb), log=lambda *_: None)
        handset.stream_to(f"gzip -dc | dd of={target} bs=4194304 2>/dev/null; sync",
                          install.gzip_stream(install._raw_reader(str(source), len(data)), digest), "write")
        self.assertEqual(target.read_bytes(), data)
        self.assertEqual(digest.hexdigest(), hashlib.sha256(data).hexdigest())

    def test_stream_failure_is_reported(self):
        adb = self.tmp / "adb"
        adb.write_text(FAKE_ADB)
        adb.chmod(0o755)
        handset = phone.Phone(str(adb), log=lambda *_: None)
        with self.assertRaises(phone.PhoneError):
            handset.stream_to("cat >/dev/null; exit 3", iter([b"x" * 1000]), "write")

    def test_raw_reader_stops_at_the_image_size(self):
        source = self.tmp / "img"
        source.write_bytes(b"a" * 100)
        self.assertEqual(b"".join(install._raw_reader(str(source), 60, chunk=16)), b"a" * 60)
        with self.assertRaises(install.InstallError):
            list(install._raw_reader(str(source), 200))

    @unittest.skipUnless(sys.platform == "darwin" and shutil.which("hdiutil"), "needs hdiutil")
    def test_attached_image_reads_like_the_converted_one(self):
        base = self.tmp / "t"
        subprocess.run(["hdiutil", "create", "-quiet", "-size", "8m", "-type", "SPARSE", "-layout", "NONE",
                        "-fs", "Case-sensitive Journaled HFS+", "-volname", "T", str(base)], check=True)
        image = base.with_suffix(".sparseimage")
        subprocess.run(["hdiutil", "convert", "-quiet", str(image), "-format", "UDTO", "-o", str(base)], check=True)
        converted = base.with_suffix(".cdr").read_bytes()
        device, size = install.attach_raw(image)
        try:
            raw = b"".join(install._raw_reader(device.replace("/dev/disk", "/dev/rdisk"), size))
        finally:
            subprocess.run(["hdiutil", "detach", "-quiet", device])
        self.assertEqual(size, len(converted))
        self.assertEqual(raw, converted)


if __name__ == "__main__":
    unittest.main()
