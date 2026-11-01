#!/usr/bin/env python3
"""Install iOS 16.7.16 on a Lumia 950 XL that runs Android (LineageOS + lk2nd), from a Mac.

    python3 installer/install.py install          # every stage below, in order; resumes
    python3 installer/install.py boot             # start iOS once (the next boot is Android again)
    python3 installer/install.py status
    python3 installer/install.py <stage>          # run one stage again
    python3 installer/install.py update           # a newer kernel/menu release: fetch, stage, push
    python3 installer/install.py restore-bcd      # put the phone's original boot configuration back

Stages (docs/lumia950xl/INSTALL.md is the same procedure by hand):

    preflight  the Mac's tools and space; the phone's model, adb root, sgdisk, EFIESP, BCD
    backup     EFIESP image, BCD, GPT and Android's /data, copied to the Mac
    partition  shrink userdata in TWRP, add the `ios` partition (asks you to type a confirmation)
    fetch      the IPSW members from Apple, UEFI, BootShim, EfiFs, the kernel release (all pinned)
    root       build the 17.5 GB HFS+ root image from the IPSW (asks for your password: sudo)
    write      stream the root image to `ios` and verify it by SHA-256 (~45 min over USB 2)
    stage      kernel + device tree + trust cache + boot menu, as the files the phone reads
    esp        UEFI, BootShim and the menu onto EFIESP; an "iOS (UEFI)" boot entry in the BCD
    kernel     the kernel and its sidecars onto /cache, the menu configuration onto EFIESP

Everything lives in --workdir (default ~/UniXNU-lumia950xl-install, ~35 GB). state.json there
records what is done, so a failed or interrupted run continues where it stopped; a stage that is
run by name runs again. --dry-run prints what would change on the phone without changing it.

Android stays the default boot. iOS starts only through a one-shot BCD boot sequence (`boot`),
which Windows Boot Manager deletes after using it; hold power + volume down to restart into
Android from iOS.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import plistlib
import re
import secrets
import shlex
import shutil
import subprocess
import sys
import time
import zipfile
import zlib
from pathlib import Path

if sys.version_info < (3, 11):
    sys.exit("python3.11 or newer is required (tomllib); install it with `brew install python`")
import tomllib  # noqa: E402

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

import bcd  # noqa: E402
import fetch  # noqa: E402
import partition  # noqa: E402
from phone import CACHE_PAYLOADS, Phone, PhoneError  # noqa: E402

STAGES = ["preflight", "backup", "partition", "fetch", "root", "write", "stage", "esp", "kernel"]
# The Lumia 950 XL's hardware name, from lk2nd's device tree. ro.product.device is no use: LineageOS
# builds for this phone may spoof another model there (ours says "redfin", a Pixel 5).
DEVICES = {"cityman"}
SHIM_PATH = "\\Windows\\System32\\BOOT\\BootShimAA64.efi"
ENTRY_NAME = "iOS (UEFI)"
BCD_ON_ESP = "EFI/Microsoft/BOOT/BCD"
DEFAULT_IOS_GIB = 18
PHONE_TMP = "/data/local/tmp/unixnu-install"
ROOT_IMAGE = "ios-root.sparseimage"


class InstallError(Exception):
    pass


class Context:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.dry = args.dry_run
        self.work: Path = args.workdir.expanduser().resolve()
        self.work.mkdir(parents=True, exist_ok=True)
        self.downloads = self.work / "downloads"
        self.state_path = self.work / "state.json"
        self.state = json.loads(self.state_path.read_text()) if self.state_path.exists() else {}
        self.state.setdefault("done", {})
        self.manifest = tomllib.loads((HERE / "manifest.toml").read_text())
        self._log = (self.work / "install.log").open("a")
        self.phone = Phone(args.adb, dry_run=self.dry, log=self.log)

    def log(self, message: str = "") -> None:
        print(message)
        self._log.write(f"{dt.datetime.now():%H:%M:%S} {message}\n")
        self._log.flush()

    def close(self) -> None:
        self._log.close()

    def save(self) -> None:
        if not self.dry:
            self.state_path.write_text(json.dumps(self.state, indent=2, sort_keys=True))

    def mark(self, stage: str) -> None:
        self.state["done"][stage] = dt.datetime.now().isoformat(timespec="seconds")
        self.save()

    def done(self, stage: str) -> bool:
        return stage in self.state["done"]

    def confirm(self, prompt: str, word: str) -> None:
        if self.dry:
            self.log(f"  (dry run) would ask: {prompt} [type {word}]")
            return
        answer = input(f"\n{prompt}\nType {word} to continue, anything else stops: ").strip()
        if answer != word:
            raise InstallError("stopped at your request; nothing was changed by this stage")

    def run(self, command: list[str], **kwargs) -> None:
        # The ADT seed identifies the phone; keep it out of the log people paste into bug reports.
        shown = [("<seed>" if i and str(command[i - 1]) == "--unique-chip-id" else str(c))
                 for i, c in enumerate(command)]
        self.log("  $ " + " ".join(shlex.quote(c) for c in shown))
        if self.dry:
            return
        result = subprocess.run([str(c) for c in command], **kwargs)
        if result.returncode != 0:
            raise InstallError(f"{Path(str(command[0])).name} failed with exit status {result.returncode}")


def sha256(path: Path) -> str:
    return fetch.sha256_file(path)


def md5_local(path: Path) -> str:
    digest = hashlib.md5()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


# ---------------------------------------------------------------------------------------------
# Phone helpers shared by the stages


def android(ctx: Context) -> None:
    """The phone in Android with a root shell, and the same phone as before."""
    state = ctx.phone.state()
    if state != "device":
        raise InstallError("no phone in Android over adb" + (f" (it is in {state} mode)" if state else "")
                           + "; connect it by USB with USB debugging on, then run this again")
    ctx.phone.root()
    serial = ctx.phone.getprop("ro.serialno")
    known = ctx.state.get("serial")
    if known and serial != known:
        raise InstallError(f"this is not the phone the install started on (serial {serial}, "
                           f"expected {known}); use another --workdir for another phone")
    if not known:
        ctx.state["serial"] = serial
        ctx.save()


def esp_device(ctx: Context) -> str:
    gpt = ctx.phone.gpt()
    esp = gpt.by_name("EFIESP")
    if esp is None:
        raise InstallError("the phone has no EFIESP partition")
    return ctx.phone.block(esp.index)


def ios_partition(ctx: Context):
    return ctx.phone.gpt().by_name("ios")


def esp_exists(ctx: Context, mount: str, rel: str) -> str | None:
    """md5 of a file on the mounted EFIESP, or None."""
    out = ctx.phone.read(f"[ -f {mount}/{shlex.quote(rel)} ] && md5sum {mount}/{shlex.quote(rel)}", check=False)
    return out.split()[0] if out.strip() else None


def pull_bcd(ctx: Context, dest: Path) -> bcd.Store:
    with ctx.phone.esp(esp_device(ctx), writable=False) as mount:
        ctx.phone.read(f"mkdir -p {PHONE_TMP}; cp {mount}/{BCD_ON_ESP} {PHONE_TMP}/BCD.current")
    ctx.phone.pull(f"{PHONE_TMP}/BCD.current", dest)
    return bcd.Store(dest.read_bytes())


def push_bcd(ctx: Context, local: Path, what: str) -> None:
    """Copy a BCD store to EFIESP and read it back. The store is checked by parsing it first:
    Windows Boot Manager cannot start anything, Android included, from a BCD it cannot read."""
    bcd.Store(local.read_bytes())
    ctx.phone.push(local, f"{PHONE_TMP}/BCD.new")
    with ctx.phone.esp(esp_device(ctx), writable=True) as mount:
        ctx.phone.change(f"cp {PHONE_TMP}/BCD.new {mount}/{BCD_ON_ESP}", what)
        if ctx.dry:
            return
        back = ctx.phone.read(f"md5sum {mount}/{BCD_ON_ESP}").split()[0]
    if back != md5_local(local):
        raise InstallError(f"the BCD read back from EFIESP ({back}) is not the one written; restore it "
                           f"with `install.py restore-bcd` before rebooting")


# ---------------------------------------------------------------------------------------------
# Stages


def stage_preflight(ctx: Context) -> None:
    if sys.platform != "darwin":
        raise InstallError("the root image is built with hdiutil and ditto, so this runs on macOS")
    for tool in ("hdiutil", "ditto", "fsck_hfs", "diskutil"):
        if shutil.which(tool) is None:
            raise InstallError(f"{tool} not found")
    if shutil.which(ctx.phone.adb[0]) is None:
        raise InstallError("adb not found: `brew install android-platform-tools`, or pass --adb")
    need = 0
    if not ctx.done("fetch"):
        need += 8 * 10**9
    if not ctx.done("root"):
        need += 19 * 10**9
    free = shutil.disk_usage(ctx.work).free
    ctx.log(f"  work directory {ctx.work}: {free / 1e9:.0f} GB free, about {need / 1e9:.0f} GB still needed")
    if free < need:
        raise InstallError("not enough free space in the work directory (use --workdir)")

    android(ctx)
    device = ctx.phone.getprop("ro.boot.hardware")
    model = ctx.phone.read("cat /proc/device-tree/model", check=False).strip("\0\n ")
    ctx.log(f"  phone: {model or '?'} ({device}), Android "
            f"{ctx.phone.getprop('ro.build.version.release')}, serial {ctx.state['serial']}")
    if device not in DEVICES and not ctx.args.any_device:
        raise InstallError(f"this phone reports {device!r}; the installer is for the Lumia 950 XL "
                           f"({', '.join(sorted(DEVICES))})")
    missing = [t for t in ("sgdisk", "blockdev", "sha256sum", "md5sum", "gzip", "dd", "tar")
               if not ctx.phone.has(t)]
    if missing:
        raise InstallError(f"the phone's Android lacks {', '.join(missing)} (LineageOS ships them)")
    gpt = ctx.phone.gpt()
    for name in ("EFIESP", "cache", "userdata"):
        if gpt.by_name(name) is None:
            raise InstallError(f"the phone has no {name} partition")
    if ctx.phone.read("grep -q ' /cache ' /proc/mounts && echo yes", check=False).strip() != "yes":
        raise InstallError("/cache is not mounted on Android; DarwinBoot reads the kernel from it")
    store = pull_bcd(ctx, ctx.work / "BCD.preflight")
    shim = store.string(store.default_entry(), bcd.PATH)
    ctx.log(f"  BCD: default entry {store.default_entry()} -> {shim}")
    if not shim or "bootshim" not in shim.lower():
        ctx.log("  note: the BCD default is not a Boot Shim; Android is expected to boot through "
                "Boot Shim -> lk2nd. The installer copies the default entry for iOS anyway.")
    crypto = ctx.phone.getprop("ro.crypto.state")
    ios = gpt.by_name("ios")
    if ios is None and crypto not in ("", "unencrypted"):
        raise InstallError(f"/data is {crypto}; TWRP cannot shrink an encrypted userdata here")
    ctx.log("  ios partition: " + (f"p{ios.index}, {ios.sectors * gpt.sector_size / 2**30:.2f} GiB" if ios
                                   else "not yet (the partition stage makes it)"))


def stage_backup(ctx: Context) -> None:
    folder = ctx.work / "backup" / dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    folder.mkdir(parents=True)
    android(ctx)
    gpt = ctx.phone.gpt()
    esp = gpt.by_name("EFIESP")
    ctx.log(f"  into {folder}")
    (folder / "sgdisk-print.txt").write_text(ctx.phone.read("sgdisk --print /dev/block/mmcblk0"))
    ctx.phone.read(f"mkdir -p {PHONE_TMP}; sgdisk --backup={PHONE_TMP}/gpt.sgdisk /dev/block/mmcblk0")
    ctx.phone.pull(f"{PHONE_TMP}/gpt.sgdisk", folder / "gpt.sgdisk")
    ctx.phone.read_to_file(f"dd if={ctx.phone.block(esp.index)} bs=1048576 2>/dev/null", folder / "efiesp.img")
    pull_bcd(ctx, folder / "BCD")
    sums = {}
    for name in ("gpt.sgdisk", "efiesp.img", "BCD"):
        sums[name] = md5_local(folder / name)
    if (folder / "efiesp.img").stat().st_size != esp.sectors * gpt.sector_size:
        raise InstallError("the EFIESP copy is short")
    if gpt.by_name("ios") is None and not ctx.args.skip_data_backup:
        ctx.log("  /data (your Android apps and files; this takes a while)")
        ctx.phone.read_to_file("tar -C /data -cf - --exclude=./local/tmp . 2>/dev/null", folder / "data.tar")
        sums["data.tar"] = md5_local(folder / "data.tar")
    (folder / "MD5SUMS").write_text("".join(f"{v}  {k}\n" for k, v in sums.items()))
    for name, value in sums.items():
        ctx.log(f"  {name:<12} {(folder / name).stat().st_size:>14,} B  md5 {value}")
    # The first backup is the one restore-bcd goes back to.
    ctx.state.setdefault("first_backup", str(folder))
    ctx.state["last_backup"] = str(folder)
    ctx.save()


def stage_partition(ctx: Context) -> None:
    android(ctx)
    gpt = ctx.phone.gpt()
    existing = gpt.by_name("ios")
    if existing is not None:
        size = existing.sectors * gpt.sector_size
        if size < partition.min_ios_bytes(gpt.sector_size):
            raise InstallError(f"the existing ios partition (p{existing.index}) is {size / 2**30:.2f} GiB, "
                               f"too small for the root image")
        ctx.log(f"  ios partition already there: p{existing.index}, {size / 2**30:.2f} GiB")
        ctx.state["ios_index"] = existing.index
        return
    if not ctx.state.get("last_backup") or not (Path(ctx.state["last_backup"]) / "data.tar").exists():
        if not ctx.args.skip_data_backup:
            raise InstallError("run the backup stage first: it copies /data before the partition changes")
    used_kib = int(ctx.phone.read("df -k /data | tail -1").split()[2])
    plan = partition.plan(gpt, used_kib * 1024, int(ctx.args.ios_size * 2**30))
    ctx.log("  plan:\n" + plan.describe())
    ctx.log("  steps: reboot to TWRP, check and shrink Android's filesystem, reboot to Android,\n"
            "  rewrite the partition table, reboot to check. Keep the phone on USB and charged.")
    ctx.confirm("This changes the phone's partition table. Your backup is in "
                f"{ctx.state.get('last_backup')}.", "SPLIT")
    ctx.state["plan"] = plan.__dict__
    ctx.save()

    ctx.phone.reboot("recovery")
    if not ctx.dry:
        ctx.phone.wait_for("recovery")
        for tool in ("e2fsck", "resize2fs"):
            if not ctx.phone.has(tool):
                raise InstallError(f"recovery has no {tool}; the installer needs TWRP")
    device = ctx.phone.block(plan.userdata)
    ctx.phone.change(f"umount /sdcard 2>/dev/null; umount /data 2>/dev/null; "
                     f"! grep -q '^{device} ' /proc/mounts", "unmount userdata in TWRP")
    ctx.phone.change(f"e2fsck -f -y {device}; [ $? -le 1 ]", "check Android's filesystem", timeout=1800)
    ctx.phone.change(f"resize2fs {device} {plan.fs_blocks}", "shrink Android's filesystem", timeout=3600)
    if not ctx.dry:
        info = ctx.phone.read(f"(dumpe2fs -h {device} || tune2fs -l {device}) 2>/dev/null")
        match = re.search(r"Block count:\s+(\d+)", info)
        if not match or int(match.group(1)) != plan.fs_blocks:
            raise InstallError(f"the filesystem did not shrink to {plan.fs_blocks} blocks; the "
                               f"partition table is unchanged, so Android still boots")
    ctx.phone.reboot()
    if not ctx.dry:
        ctx.phone.wait_for("device", timeout=600)
        android(ctx)
        userdata = ctx.phone.gpt().by_name("userdata")
        if (userdata.first, userdata.last) != (plan.first, plan.old_last):
            raise InstallError("userdata changed while the phone was in recovery; stopping")
    ctx.phone.change(plan.sgdisk_command(), "rewrite the partition table (userdata + ios)")
    ctx.phone.reboot()
    if ctx.dry:
        return
    ctx.phone.wait_for("device", timeout=600)
    android(ctx)
    partition.check_result(ctx.phone.gpt(), plan)
    if ctx.phone.read("[ -e /dev/block/by-name/ios ] && ls /data/data | head -1", check=False).strip() == "":
        raise InstallError("after the reboot, /dev/block/by-name/ios or Android's data is missing")
    ctx.state["ios_index"] = plan.ios
    ctx.log(f"  done: ios is p{plan.ios}; Android's data is intact")


def release_dir(ctx: Context) -> Path:
    return ctx.downloads / "release"


def fetch_release(ctx: Context) -> None:
    out = release_dir(ctx)
    if ctx.args.release_zip:
        archive = ctx.args.release_zip.expanduser().resolve()
        digest = sha256(archive)
        ctx.log(f"  release: local {archive} (sha256 {digest[:16]})")
    else:
        spec = ctx.manifest["release"]
        if not spec.get("url"):
            raise InstallError("manifest.toml has no kernel release yet; pass --release-zip with a "
                               "zip from installer/make-release.py")
        archive = fetch.download(spec["url"], ctx.downloads / Path(spec["url"]).name, spec["sha256"])
        digest = spec["sha256"]
    if ctx.state.get("release_sha256") == digest and (out / "kernel.macho").exists():
        return
    if out.exists():
        shutil.rmtree(out)
    with zipfile.ZipFile(archive) as zf:
        zf.extractall(out)
    sums = (out / "SHA256SUMS").read_text().split("\n")
    for line in filter(None, sums):
        value, name = line.split(None, 1)
        if sha256(out / name.strip()) != value:
            raise InstallError(f"release file {name} does not match its SHA256SUMS line")
    ctx.state["release_sha256"] = digest
    ctx.state["release_version"] = (out / "VERSION").read_text().strip()
    ctx.save()
    ctx.log(f"  release {ctx.state['release_version']}")


def stage_fetch(ctx: Context) -> None:
    if ctx.dry:
        ctx.log("  (dry run) would download the pinned files")
        return
    ipsw = ctx.manifest["ipsw"]
    remote = None
    for member in ipsw["member"]:
        dest = ctx.downloads / "ipsw" / Path(member["member"]).name
        if fetch.verified(dest, member["sha256"]):
            ctx.log(f"  {member['role']:<22} ok")
            continue
        remote = remote or fetch.RemoteZip(ipsw["url"])
        ctx.log(f"  {member['role']:<22} downloading {member['member']} ({remote.info(member['member']).file_size / 1e6:,.0f} MB)")
        remote.extract(member["member"], dest, member["sha256"])
        if member.get("payload_sha256"):
            adt = ctx.downloads / "d221ap.adt"
            adt.write_bytes(fetch.im4p_payload(dest.read_bytes()))
            if sha256(adt) != member["payload_sha256"]:
                raise InstallError("the device tree payload does not match its pin")
    adt = ctx.downloads / "d221ap.adt"
    if not adt.exists():
        adt.write_bytes(fetch.im4p_payload((ctx.downloads / "ipsw" / "DeviceTree.d221ap.im4p").read_bytes()))

    uefi = ctx.manifest["uefi"]
    if not fetch.verified(ctx.downloads / "UEFI.elf", uefi["member_sha256"]):
        archive = fetch.download(uefi["url"], ctx.downloads / Path(uefi["url"]).name, uefi["sha256"])
        fetch.extract_local_zip(archive, uefi["member"], ctx.downloads / "UEFI.elf", uefi["member_sha256"])
    ctx.log("  UEFI.elf               ok")
    shim = ctx.manifest["bootshim"]
    if not fetch.verified(ctx.downloads / "BootShim.efi", shim["member_sha256"]):
        archive = ctx.downloads / "bootshim.zip"
        with fetch._request(shim["url"]) as response:
            archive.write_bytes(response.read())
        fetch.extract_local_zip(archive, shim["member"], ctx.downloads / "BootShim.efi", shim["member_sha256"])
    ctx.log("  BootShim.efi           ok")
    efifs = ctx.manifest["efifs"]
    fetch.download(efifs["url"], ctx.downloads / "ext2_aa64.efi", efifs["sha256"])
    ctx.log("  ext2_aa64.efi          ok")
    fetch_release(ctx)


def root_dir(ctx: Context) -> Path:
    return ctx.work / "iosroot"


def stage_root(ctx: Context) -> None:
    out = root_dir(ctx)
    image = out / ROOT_IMAGE
    ipsw = ctx.downloads / "ipsw"
    if image.exists():
        ctx.log(f"  removing the unfinished {image.name} from an earlier run")
        if not ctx.dry:
            image.unlink()
    out.mkdir(parents=True, exist_ok=True)
    ctx.log("  building the root image: sudo asks for your Mac password (hdiutil and ditto have to\n"
            "  run as root to keep iOS's file owners). About 20-30 minutes.")
    ctx.run(["sudo", f"IPSW={ipsw}", f"OUT={out}", "sh", ROOT / "scripts/mkiosroot-root.sh"])
    if not ctx.dry and not image.exists():
        raise InstallError("mkiosroot-root.sh finished without an image")


def trustcache(ctx: Context) -> Path:
    ipsw = ctx.downloads / "ipsw"
    out = ctx.work / "root.trustcache"
    ctx.run([sys.executable, ROOT / "scripts/make-trustcache-bundle.py", out,
             ipsw / "087-86698-044.dmg.trustcache",
             f"{ipsw / '087-86593-047.dmg.trustcache'}+{ipsw / '087-86715-047.dmg.trustcache'}"],
            stdout=subprocess.DEVNULL)
    return out


def attach_raw(image: Path) -> tuple[str, int]:
    out = subprocess.run(["hdiutil", "attach", "-nomount", "-readonly", "-noverify", "-plist", str(image)],
                         capture_output=True, check=True).stdout
    entities = plistlib.loads(out)["system-entities"]
    device = entities[0]["dev-entry"]
    info = plistlib.loads(subprocess.run(["diskutil", "info", "-plist", device], capture_output=True,
                                         check=True).stdout)
    return device, int(info["TotalSize"])


def gzip_stream(blocks, digest):
    """gzip -1 on the fly (the phone's toybox gzip unpacks it), hashing the raw bytes as they pass."""
    packer = zlib.compressobj(1, zlib.DEFLATED, 31)
    for block in blocks:
        digest.update(block)
        packed = packer.compress(block)
        if packed:
            yield packed
    yield packer.flush()


def stage_write(ctx: Context) -> None:
    image = root_dir(ctx) / ROOT_IMAGE
    if not image.exists() and not ctx.dry:
        raise InstallError("no root image; run the root stage first")
    android(ctx)
    ios = ios_partition(ctx)
    if ios is None:
        raise InstallError("the phone has no ios partition; run the partition stage first")
    target = ctx.phone.block(ios.index)
    part_size = ctx.phone.size_bytes(target)
    if ctx.dry:
        ctx.log(f"  (dry run) would write {image.name} to {target} ({part_size / 2**30:.2f} GiB) and verify it")
        return
    device, size = attach_raw(image)
    try:
        if size + partition.NATLOG_SECTORS * 512 > part_size:
            raise InstallError(f"the image ({size:,} B) and the log ring do not fit {target} ({part_size:,} B)")
        ctx.confirm(f"Write the iOS root ({size / 2**30:.2f} GiB) to {target} (the ios partition)? "
                    "Whatever is on it now is replaced.", "WRITE")
        raw = device.replace("/dev/disk", "/dev/rdisk")
        digest = hashlib.sha256()
        ctx.phone.stream_to(f"gzip -dc | dd of={target} bs=4194304 2>/dev/null; sync",
                            gzip_stream(_raw_reader(raw, size), digest), f"write the root image to {target}")
    finally:
        subprocess.run(["hdiutil", "detach", "-quiet", device])
    local = digest.hexdigest()
    ctx.log("  verifying: the phone re-reads what was written (~25 min)")
    remote = ctx.phone.sha256_head(target, size)
    ctx.log(f"  local {local}\n  phone {remote}")
    if local != remote:
        raise InstallError("the written root does not match the image; run the write stage again")
    ctx.state["root_sha256"] = local
    ctx.save()


def _raw_reader(raw: str, size: int, chunk: int = 4 << 20):
    """A raw disk device reports no size and may run past the image, so read exactly `size` bytes."""
    done, started, last = 0, time.monotonic(), 0.0
    with open(raw, "rb", buffering=0) as handle:
        while done < size:
            block = handle.read(min(chunk, size - done))
            if not block:
                raise InstallError(f"{raw} ended after {done:,} of {size:,} bytes")
            done += len(block)
            now = time.monotonic()
            if now - last >= 2 or done == size:
                last = now
                rate = done / max(now - started, 1e-3)
                sys.stderr.write(f"\r  writing: {done / 1e9:.2f} / {size / 1e9:.2f} GB "
                                 f"({rate / 1e6:.1f} MB/s, {(size - done) / rate / 60 if rate else 0:.0f} min left)   ")
                sys.stderr.flush()
            yield block
    sys.stderr.write("\n")


def kernel_args(ctx: Context, ios_index: int) -> str:
    line = (release_dir(ctx) / "args.txt").read_text().strip()
    if not re.search(r"\brd=disk0s\d+\b", line):
        raise InstallError("the release's args.txt has no rd=disk0sN")
    # iOS names the partition by its GPT index: disk0s<index>.
    return re.sub(r"\brd=disk0s\d+\b", f"rd=disk0s{ios_index}", line)


def stage_stage(ctx: Context) -> None:
    if ctx.dry and not (release_dir(ctx) / "kernel.macho").exists():
        ctx.log("  (dry run) would stage the kernel release")
        return
    if not ctx.dry:
        fetch_release(ctx)  # a no-op unless --release-zip or the manifest names another release
    ios_index = ctx.state.get("ios_index")
    if ios_index is None:
        android(ctx)
        ios = ios_partition(ctx)
        if ios is None:
            raise InstallError("the phone has no ios partition yet")
        ios_index = ctx.state["ios_index"] = ios.index
    chip = ctx.state.setdefault("unique_chip_id", secrets.token_hex(8))
    ctx.save()
    # board.toml's ADT generator reads the iPhone's device tree from build/ in the repository.
    identity = ROOT / "build/d221ap.adt"
    identity.parent.mkdir(exist_ok=True)
    shutil.copyfile(ctx.downloads / "d221ap.adt", identity)
    tc = trustcache(ctx)
    ctx.run([sys.executable, ROOT / "scripts/stage-kernel.py",
             "--kernel", release_dir(ctx) / "kernel.macho", "--unique-chip-id", chip,
             "--menu-tree", release_dir(ctx) / "menu", "--ntfs-driver", ctx.downloads / "ext2_aa64.efi",
             "--trustcache", tc, "--args", kernel_args(ctx, ios_index),
             "--out", ctx.work / "boot", "--payload-out", ctx.work / "cachevol"],
            stdout=subprocess.DEVNULL if not ctx.args.verbose else None)
    ctx.log(f"  staged release {ctx.state.get('release_version')} for rd=disk0s{ios_index}")


def esp_files(ctx: Context) -> dict[str, Path]:
    files = {
        "UEFI.elf": ctx.downloads / "UEFI.elf",
        SHIM_PATH.lstrip("\\").replace("\\", "/"): ctx.downloads / "BootShim.efi",
    }
    boot = ctx.work / "boot"
    for path in sorted(p for p in boot.rglob("*") if p.is_file()):
        files[path.relative_to(boot).as_posix()] = path
    return files


def remote_size(ctx: Context, path: str) -> int:
    out = ctx.phone.read(f"stat -c %s {path} 2>/dev/null", check=False).strip()
    return int(out) if out.isdigit() else 0


def free_bytes(ctx: Context, mount: str) -> int:
    return int(ctx.phone.read(f"df -k {mount} | tail -1").split()[3]) * 1024


def copy_to_esp(ctx: Context, files: dict[str, Path], what: str) -> None:
    """Copy files onto EFIESP by way of the phone's /data, then check each copy's md5. A file that is
    already there with other contents, and that an earlier run did not put there, stops the copy."""
    owned = set(ctx.state.get("esp_files", []))
    ctx.phone.read(f"rm -rf {PHONE_TMP}/esp; mkdir -p {PHONE_TMP}/esp")
    with ctx.phone.esp(esp_device(ctx), writable=True) as mount:
        conflicts, unchanged = [], set()
        for rel, local in files.items():
            current = esp_exists(ctx, mount, rel)
            if current == md5_local(local):
                unchanged.add(rel)
            elif current and rel not in owned:
                conflicts.append(rel)
        if conflicts and not ctx.args.force:
            raise InstallError("EFIESP already has different copies of " + ", ".join(conflicts) +
                               "; they are not from this installer (another UEFI install?). "
                               "Pass --force to replace them; the backup has the originals.")
        files = {rel: local for rel, local in files.items() if rel not in unchanged}
        if unchanged:
            ctx.log(f"  {what}: {len(unchanged)} file(s) already up to date")
        need = sum(p.stat().st_size for p in files.values())
        replaced = sum(remote_size(ctx, f"{mount}/{shlex.quote(rel)}") for rel in files)
        free = free_bytes(ctx, mount)
        if need > free + replaced:
            raise InstallError(f"EFIESP has {free / 1e6:.1f} MB free; these files need {need / 1e6:.1f} MB")
        for rel, local in files.items():
            ctx.phone.push(local, f"{PHONE_TMP}/esp/{rel}")
            target = f"{mount}/{rel}"
            ctx.phone.change(f"mkdir -p {shlex.quote(str(Path(target).parent))}; "
                             f"cp {PHONE_TMP}/esp/{shlex.quote(rel)} {shlex.quote(target)}",
                             f"{what}: {rel}")
            if not ctx.dry and esp_exists(ctx, mount, rel) != md5_local(local):
                raise InstallError(f"{rel} on EFIESP does not match what was copied")
    ctx.state["esp_files"] = sorted(owned | set(files) | unchanged)
    ctx.save()


def stage_esp(ctx: Context) -> None:
    android(ctx)
    if (ctx.work / "boot").is_dir():
        copy_to_esp(ctx, esp_files(ctx), "EFIESP")
    elif ctx.dry:
        ctx.log("  (dry run) nothing staged yet; would copy UEFI, BootShim and the menu to EFIESP")
    else:
        raise InstallError("nothing staged; run the stage stage first")

    folder = ctx.work / "bcd"
    folder.mkdir(exist_ok=True)
    store = pull_bcd(ctx, folder / "BCD.current")
    guid = store.find_entry(SHIM_PATH)
    if guid:
        ctx.log(f"  BCD already has {guid} -> {SHIM_PATH}")
    else:
        guid = store.add_boot_entry(SHIM_PATH, ENTRY_NAME)
        ctx.log(f"  BCD: new entry {guid} \"{ENTRY_NAME}\" (a copy of the default entry) -> {SHIM_PATH}")
        (folder / "BCD.base").write_bytes(store.save())
        push_bcd(ctx, folder / "BCD.base", "add the iOS boot entry to the BCD")
    ctx.state["bcd_entry"] = guid
    ctx.save()
    ctx.log("  " + store.describe().replace("\n", "\n  "))


def stage_kernel(ctx: Context) -> None:
    android(ctx)
    payloads = ctx.work / "cachevol/EFI/UniXNU/Payloads"
    names = ["kernel.macho", "kernel.macho.adt", "kernel.macho.trustcache"]
    if ctx.dry and not payloads.is_dir():
        ctx.log("  (dry run) would push the kernel to /cache")
        return
    need = sum((payloads / n).stat().st_size for n in names)
    free = free_bytes(ctx, "/cache")
    if need > free + sum(remote_size(ctx, f"{CACHE_PAYLOADS}/{n}") for n in names):
        raise InstallError(f"/cache has {free / 1e6:.0f} MB free; the kernel needs {need / 1e6:.0f} MB")
    ctx.phone.change(f"mkdir -p {CACHE_PAYLOADS}", "make the payload folder on /cache")
    for name in names:
        ctx.phone.push(payloads / name, f"{CACHE_PAYLOADS}/{name}")
    if not ctx.dry:
        sums = ctx.phone.read("md5sum " + " ".join(f"{CACHE_PAYLOADS}/{n}" for n in names))
        for name in names:
            if md5_local(payloads / name) not in sums:
                raise InstallError(f"{name} on /cache does not match")
        ctx.phone.read("sync")
    toml = ctx.work / "boot/EFI/UniXNU/unixnu.toml"
    copy_to_esp(ctx, {"EFI/UniXNU/unixnu.toml": toml}, "menu configuration")
    ctx.log(f"  kernel {ctx.state.get('release_version')} " + ("would be pushed" if ctx.dry else "is on the phone"))


STAGE_FUNCS = {name: globals()[f"stage_{name}"] for name in STAGES}


# ---------------------------------------------------------------------------------------------
# Commands


def cmd_install(ctx: Context) -> None:
    for name in STAGES:
        if name != "preflight" and ctx.done(name):
            ctx.log(f"== {name}: done {ctx.state['done'][name]}")
            continue
        ctx.log(f"== {name}")
        STAGE_FUNCS[name](ctx)
        if name != "preflight":
            ctx.mark(name)
    ctx.log("\nInstalled. Start iOS with: python3 installer/install.py boot")


def cmd_boot(ctx: Context) -> None:
    android(ctx)
    folder = ctx.work / "bcd"
    folder.mkdir(exist_ok=True)
    store = pull_bcd(ctx, folder / "BCD.current")
    guid = ctx.state.get("bcd_entry") or store.find_entry(SHIM_PATH)
    if not guid or guid not in store.objects():
        raise InstallError("the BCD has no iOS entry; run the esp stage")
    store.set_one_shot(guid)
    (folder / "BCD.oneshot").write_bytes(store.save())
    push_bcd(ctx, folder / "BCD.oneshot", "arm the one-shot iOS boot")
    ctx.phone.reboot()
    ctx.log("\nThe phone is restarting into iOS: Windows Boot Manager -> Boot Shim -> UEFI -> UniXNU menu\n"
            "(it starts iOS after its countdown) -> kernel text -> Apple logo. About 3 minutes to launchd,\n"
            "a few more to the lock screen; the first boot after writing the root takes longer.\n"
            "To go back to Android: hold power + volume down until the phone restarts.")


def cmd_status(ctx: Context) -> None:
    ctx.log(f"work directory {ctx.work}")
    for name in STAGES:
        ctx.log(f"  {name:<10} {ctx.state['done'].get(name, '-')}")
    for key in ("serial", "ios_index", "bcd_entry", "release_version", "first_backup"):
        if key in ctx.state:
            ctx.log(f"  {key:<16} {ctx.state[key]}")
    if ctx.phone.state() == "device":
        android(ctx)
        ios = ios_partition(ctx)
        ctx.log(f"  phone: ios partition {'p%d' % ios.index if ios else 'missing'}")
        store = pull_bcd(ctx, ctx.work / "BCD.status")
        ctx.log("  " + store.describe().replace("\n", "\n  "))
    else:
        ctx.log("  phone: not connected in Android")


def cmd_update(ctx: Context) -> None:
    ctx.state.pop("release_sha256", None)
    fetch_release(ctx)
    for name in ("stage", "esp", "kernel"):
        ctx.log(f"== {name}")
        STAGE_FUNCS[name](ctx)
        ctx.mark(name)


def cmd_restore_bcd(ctx: Context) -> None:
    backup = ctx.state.get("first_backup")
    if not backup or not (Path(backup) / "BCD").exists():
        raise InstallError("no backed-up BCD in this work directory")
    android(ctx)
    ctx.confirm(f"Put the BCD from {backup} back on EFIESP? The iOS entry goes away "
                "(the files stay; run the esp stage to add it again).", "RESTORE")
    push_bcd(ctx, Path(backup) / "BCD", "restore the original BCD")
    ctx.state.pop("bcd_entry", None)
    ctx.state["done"].pop("esp", None)
    ctx.save()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["install", "boot", "status", "update", "restore-bcd"] + STAGES)
    parser.add_argument("--workdir", type=Path, default=Path("~/UniXNU-lumia950xl-install"))
    parser.add_argument("--adb", default="adb", help="adb command, e.g. 'adb -s SERIAL' (default: adb)")
    parser.add_argument("--dry-run", action="store_true", help="print phone changes instead of making them")
    parser.add_argument("--ios-size", type=float, default=DEFAULT_IOS_GIB,
                        help=f"size of the ios partition in GiB (default {DEFAULT_IOS_GIB}, minimum "
                             f"{partition.min_ios_bytes() / 2**30:.2f})")
    parser.add_argument("--release-zip", type=Path, help="a local kernel release (installer/make-release.py)")
    parser.add_argument("--skip-data-backup", action="store_true",
                        help="do not copy /data before repartitioning (you made your own backup)")
    parser.add_argument("--force", action="store_true", help="replace EFIESP files this installer did not put there")
    parser.add_argument("--any-device", action="store_true", help="skip the phone model check")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)
    ctx = Context(args)
    ctx.log(f"\n# install.py {args.command} {dt.datetime.now():%Y-%m-%d %H:%M}" + (" (dry run)" if ctx.dry else ""))
    try:
        if args.command in STAGES:
            ctx.log(f"== {args.command}")
            STAGE_FUNCS[args.command](ctx)
            if args.command != "preflight":
                ctx.mark(args.command)
        else:
            {"install": cmd_install, "boot": cmd_boot, "status": cmd_status, "update": cmd_update,
             "restore-bcd": cmd_restore_bcd}[args.command](ctx)
    except (InstallError, PhoneError, bcd.BcdError, fetch.FetchError) as error:
        ctx.log(f"\nerror: {error}")
        return 1
    except KeyboardInterrupt:
        ctx.log("\ninterrupted; run the same command again to continue")
        return 130
    finally:
        ctx.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
