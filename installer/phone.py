"""The phone, through adb: Android (LineageOS, adb root) and TWRP recovery.

Every command that changes the phone goes through this module, so the installer's --dry-run can
print them instead. Reads always run, because the plan depends on what they return.
"""
from __future__ import annotations

import re
import shlex
import subprocess
import sys
import time
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path

DISK = "/dev/block/mmcblk0"
ESP_MOUNT = "/data/local/tmp/esp"
CACHE_PAYLOADS = "/cache/EFI/UniXNU/Payloads"


class PhoneError(Exception):
    pass


@dataclass
class Partition:
    index: int
    first: int
    last: int
    name: str
    type_guid: str = ""
    unique_guid: str = ""

    @property
    def sectors(self) -> int:
        return self.last - self.first + 1


@dataclass
class Gpt:
    sector_size: int
    total_sectors: int
    last_usable: int
    partitions: list[Partition]

    def by_name(self, name: str) -> Partition | None:
        found = [p for p in self.partitions if p.name == name]
        if len(found) > 1:
            raise PhoneError(f"more than one partition is named {name!r}")
        return found[0] if found else None


_PART_LINE = re.compile(r"^\s*(\d+)\s+(\d+)\s+(\d+)\s+\S+\s+\S+\s+\S+\s+(\S.*?)\s*$")


def parse_sgdisk_print(text: str) -> Gpt:
    """`sgdisk --print` output, from Android's build (1.0.4) or a desktop one."""
    def field(pattern: str) -> int:
        match = re.search(pattern, text)
        if not match:
            raise PhoneError(f"sgdisk -p output has no {pattern!r}")
        return int(match.group(1))

    sector_size = field(r"Sector size \(logical(?:/physical)?\): (\d+)")
    total = field(r"Disk \S+: (\d+) sectors")
    last_usable = field(r"last usable sector is (\d+)")
    partitions = []
    in_table = False
    for line in text.splitlines():
        if line.lstrip().startswith("Number"):
            in_table = True
            continue
        if in_table:
            match = _PART_LINE.match(line)
            if match:
                index, first, last, name = match.groups()
                partitions.append(Partition(int(index), int(first), int(last), name))
    if not partitions:
        raise PhoneError("sgdisk -p listed no partitions")
    return Gpt(sector_size, total, last_usable, partitions)


def parse_sgdisk_info(text: str) -> tuple[str, str]:
    """(type GUID, unique GUID) from `sgdisk --info=N`."""
    type_match = re.search(r"Partition GUID code: ([0-9A-Fa-f-]{36})", text)
    unique_match = re.search(r"Partition unique GUID: ([0-9A-Fa-f-]{36})", text)
    if not type_match or not unique_match:
        raise PhoneError("sgdisk -i output has no GUIDs")
    return type_match.group(1).upper(), unique_match.group(1).upper()


class Phone:
    def __init__(self, adb: str = "adb", dry_run: bool = False, log=print):
        self.adb = shlex.split(adb)
        self.dry_run = dry_run
        self.log = log

    # -- plumbing -------------------------------------------------------------------------

    def _run(self, args: list[str], input: bytes | None = None, check: bool = True,
             timeout: float | None = 600) -> subprocess.CompletedProcess:
        result = subprocess.run(self.adb + args, input=input, capture_output=True, timeout=timeout)
        if check and result.returncode != 0:
            raise PhoneError(f"adb {' '.join(args)} failed ({result.returncode}): "
                             f"{(result.stderr or result.stdout).decode(errors='replace').strip()}")
        return result

    def read(self, script: str, check: bool = True) -> str:
        """Run a shell snippet that only reads; returns stdout."""
        result = self._run(["shell", script], check=check)
        return result.stdout.decode(errors="replace").replace("\r\n", "\n")

    def change(self, script: str, what: str, timeout: float | None = 600) -> str:
        """Run a shell snippet that changes the phone (printed only, under --dry-run)."""
        self.log(f"  phone: {what}")
        if self.dry_run:
            self.log(f"    (dry run) adb shell {script!r}")
            return ""
        result = self._run(["shell", script], timeout=timeout)
        return result.stdout.decode(errors="replace").replace("\r\n", "\n")

    def push(self, local: Path, remote: str) -> None:
        self.log(f"  push {local.name} -> {remote}")
        if not self.dry_run:
            self._run(["push", str(local), remote], timeout=None)

    def pull(self, remote: str, local: Path) -> None:
        self._run(["pull", remote, str(local)], timeout=None)

    def read_to_file(self, script: str, local: Path) -> None:
        """Stream a command's stdout (exec-out: no pty, binary-safe) into a local file."""
        with local.open("wb") as out:
            result = subprocess.run(self.adb + ["exec-out", script], stdout=out, stderr=subprocess.PIPE)
        if result.returncode != 0:
            raise PhoneError(f"adb exec-out {script!r} failed: {result.stderr.decode(errors='replace')}")

    def stream_to(self, script: str, producer, what: str) -> None:
        """Feed `producer` (an iterable of bytes) to a phone command's stdin. `adb shell -T`
        (no pty) is what keeps binary data intact: with a pty, toybox saw a TTY on stdin."""
        self.log(f"  phone: {what}")
        if self.dry_run:
            self.log(f"    (dry run) adb shell -T {script!r} < data")
            return
        with subprocess.Popen(self.adb + ["shell", "-T", script], stdin=subprocess.PIPE,
                              stderr=subprocess.PIPE) as process:
            try:
                for block in producer:
                    process.stdin.write(block)
                process.stdin.close()
            except BrokenPipeError:
                pass
            error = process.stderr.read().decode(errors="replace")
            status = process.wait()
        if status != 0:
            raise PhoneError(f"{what} failed: {error.strip()}")

    # -- state ----------------------------------------------------------------------------

    def state(self) -> str | None:
        """'device' (Android), 'recovery' (TWRP), or None when no phone answers."""
        result = self._run(["get-state"], check=False, timeout=20)
        state = result.stdout.decode().strip()
        return state or None

    def wait_for(self, state: str, timeout: float = 300) -> None:
        self.log(f"  waiting for the phone in {state} mode (up to {timeout / 60:.0f} min)")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.state() == state and self._run(["shell", "true"], check=False, timeout=20).returncode == 0:
                if state == "device":
                    # Android answers adb before it has mounted /data and /cache.
                    if self.read("getprop sys.boot_completed", check=False).strip() != "1":
                        time.sleep(3)
                        continue
                return
            time.sleep(3)
        raise PhoneError(f"the phone did not come back in {state} mode within {timeout / 60:.0f} minutes")

    def root(self) -> None:
        if self.read("id -u", check=False).strip() == "0":
            return
        self._run(["root"], check=False)
        time.sleep(5)
        self._run(["wait-for-device"], timeout=120)
        if self.read("id -u").strip() != "0":
            raise PhoneError("adb root did not give a root shell; enable 'Rooted debugging' in "
                             "Settings > System > Developer options")

    def reboot(self, target: str = "") -> None:
        self.log(f"  reboot{' to ' + target if target else ''}")
        if self.dry_run:
            return
        self._run(["reboot"] + ([target] if target else []), check=False, timeout=60)
        time.sleep(10)

    def getprop(self, name: str) -> str:
        return self.read(f"getprop {shlex.quote(name)}").strip()

    def has(self, tool: str) -> bool:
        return self.read(f"command -v {shlex.quote(tool)} >/dev/null && echo yes", check=False).strip() == "yes"

    # -- storage --------------------------------------------------------------------------
    # Android's sgdisk is AOSP's build, whose option parser (android_popt.cc) takes only long
    # options with `=`: `-p` and `-i 54` are "invalid option", `--print` and `--info=54` work.

    def gpt(self) -> Gpt:
        gpt = parse_sgdisk_print(self.read(f"sgdisk --print {DISK}"))
        for part in gpt.partitions:
            if part.name in ("userdata", "ios", "EFIESP"):
                part.type_guid, part.unique_guid = parse_sgdisk_info(self.read(f"sgdisk --info={part.index} {DISK}"))
        return gpt

    def block(self, index: int) -> str:
        return f"{DISK}p{index}"

    def size_bytes(self, device: str) -> int:
        return int(self.read(f"blockdev --getsize64 {device}").strip())

    def sha256_head(self, device: str, size: int) -> str:
        return self.read(f"head -c {size} {device} | sha256sum", check=True).split()[0]

    @contextmanager
    def esp(self, esp_dev: str, writable: bool):
        """EFIESP mounted at ESP_MOUNT (vfat) for the duration of the block."""
        mode = "rw" if writable and not self.dry_run else "ro"
        self.read(f"mkdir -p {ESP_MOUNT}; umount {ESP_MOUNT} 2>/dev/null; "
                  f"mount -t vfat -o {mode} {esp_dev} {ESP_MOUNT}")
        try:
            yield ESP_MOUNT
        finally:
            self.read(f"sync; umount {ESP_MOUNT}", check=False)


def progress_reader(path: Path, label: str, chunk: int = 4 << 20):
    """Yield a file in chunks while printing how far along it is."""
    total = path.stat().st_size
    done = 0
    started = time.monotonic()
    last = 0.0
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(chunk), b""):
            done += len(block)
            now = time.monotonic()
            if now - last >= 2 or done == total:
                last = now
                rate = done / max(now - started, 1e-3)
                eta = (total - done) / rate if rate else 0
                sys.stderr.write(f"\r  {label}: {done / 1e9:.2f} / {total / 1e9:.2f} GB "
                                 f"({rate / 1e6:.1f} MB/s, {eta / 60:.0f} min left)   ")
                sys.stderr.flush()
            yield block
    sys.stderr.write("\n")
