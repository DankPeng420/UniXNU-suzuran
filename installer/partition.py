"""Plan the `ios` partition: shrink Android's userdata and put iOS in the space after it.

This is the step that can lose data, so the plan is a pure function of what the phone reports
(tests/test_partition.py runs it on our phone's real GPT) and refuses anything that is not the
layout it was written for: userdata is the last partition, ext4, unencrypted, and its files fit
in the smaller size with room to spare. The commands it produces are the ones that worked on our
phone (docs/lumia950xl/INSTALL.md section 2):

  TWRP:     e2fsck -f -y /dev/block/mmcblk0pN; resize2fs /dev/block/mmcblk0pN <blocks>
  Android:  sgdisk --delete=N --new=N:<first>:<new last> --typecode=N:<type>
                   --partition-guid=N:<unique> --change-name=N:userdata
                   --new=M:<ios first>:<old last> --typecode=M:AF00 --change-name=M:ios /dev/block/mmcblk0

(long options only: Android's sgdisk rejects the short ones, see phone.Phone.gpt)

The filesystem is shrunk first and to 16 KiB less than the new partition, so at no point does a
partition end before the filesystem inside it. userdata keeps its first sector, type, unique GUID
and name, which is everything Android and lk2nd look it up by.
"""
from __future__ import annotations

from dataclasses import dataclass

from phone import DISK, Gpt, PhoneError

MIB = 1 << 20
GIB = 1 << 30
FS_BLOCK = 4096
FS_SPARE = 16 * 1024
ALIGN_SECTORS = 2048  # 1 MiB

# The root image is 17,500 MiB (scripts/mkiosroot-root.sh), and NativeSdhci keeps its console log
# ring in the last 65,537 sectors of the partition (tools/phone/pull-natlog.sh).
ROOT_IMAGE_BYTES = 17_500 * MIB
NATLOG_SECTORS = 65_537
MIN_ANDROID_BYTES = 4 * GIB
ANDROID_HEADROOM = 1 * GIB
HFS_TYPE = "AF00"


@dataclass
class Plan:
    userdata: int  # partition index
    ios: int
    first: int  # userdata's first sector (unchanged)
    old_last: int
    new_last: int  # userdata's new last sector
    ios_first: int
    ios_last: int
    fs_blocks: int  # resize2fs target, in 4 KiB blocks
    type_guid: str
    unique_guid: str
    sector_size: int

    @property
    def android_bytes(self) -> int:
        return (self.new_last - self.first + 1) * self.sector_size

    @property
    def ios_bytes(self) -> int:
        return (self.ios_last - self.ios_first + 1) * self.sector_size

    def describe(self) -> str:
        return (f"  userdata (p{self.userdata}): sectors {self.first}-{self.old_last} "
                f"({(self.old_last - self.first + 1) * self.sector_size / GIB:.2f} GiB) "
                f"-> {self.first}-{self.new_last} ({self.android_bytes / GIB:.2f} GiB)\n"
                f"  ios      (p{self.ios}): sectors {self.ios_first}-{self.ios_last} "
                f"({self.ios_bytes / GIB:.2f} GiB, new, Apple HFS+)\n"
                f"  Android's filesystem is shrunk to {self.fs_blocks} blocks "
                f"({self.fs_blocks * FS_BLOCK / GIB:.2f} GiB) first.")

    def resize_commands(self) -> str:
        device = f"{DISK}p{self.userdata}"
        return f"e2fsck -f -y {device}; resize2fs {device} {self.fs_blocks}"

    def sgdisk_command(self) -> str:
        n, m = self.userdata, self.ios
        return (f"sgdisk --delete={n} --new={n}:{self.first}:{self.new_last} --typecode={n}:{self.type_guid} "
                f"--partition-guid={n}:{self.unique_guid} --change-name={n}:userdata "
                f"--new={m}:{self.ios_first}:{self.ios_last} --typecode={m}:{HFS_TYPE} --change-name={m}:ios {DISK}")


def min_ios_bytes(sector_size: int = 512) -> int:
    return ROOT_IMAGE_BYTES + NATLOG_SECTORS * sector_size


def plan(gpt: Gpt, data_used_bytes: int, ios_bytes: int) -> Plan:
    if gpt.by_name("ios") is not None:
        raise PhoneError("the phone already has an 'ios' partition")
    userdata = gpt.by_name("userdata")
    if userdata is None:
        raise PhoneError("no partition named 'userdata'")
    if max(p.last for p in gpt.partitions) != userdata.last:
        raise PhoneError("userdata is not the last partition on the disk; this layout is not "
                         "one the installer knows how to split")
    if not userdata.type_guid or not userdata.unique_guid:
        raise PhoneError("userdata's GUIDs were not read")
    sector = gpt.sector_size
    if ios_bytes < min_ios_bytes(sector):
        raise PhoneError(f"iOS needs at least {min_ios_bytes(sector) / GIB:.2f} GiB "
                         f"(the {ROOT_IMAGE_BYTES // MIB:,} MiB root image plus the log ring)")
    ios_sectors = -(-ios_bytes // sector)
    ios_first = (userdata.last + 1 - ios_sectors) // ALIGN_SECTORS * ALIGN_SECTORS
    new_last = ios_first - 1
    android_bytes = (new_last - userdata.first + 1) * sector
    need = max(MIN_ANDROID_BYTES, data_used_bytes + ANDROID_HEADROOM)
    if android_bytes < need:
        raise PhoneError(f"that leaves Android {android_bytes / GIB:.2f} GiB, but it needs "
                         f"{need / GIB:.2f} GiB ({data_used_bytes / GIB:.2f} GiB of files are on "
                         f"/data); free space on Android or choose a smaller iOS size")
    used = {p.index for p in gpt.partitions}
    ios_index = next((i for i in range(userdata.index + 1, 129) if i not in used), None)
    if ios_index is None:
        raise PhoneError("the partition table has no free entry after userdata")
    fs_blocks = (android_bytes - FS_SPARE) // FS_BLOCK
    return Plan(userdata.index, ios_index, userdata.first, userdata.last, new_last, ios_first,
                userdata.last, fs_blocks, userdata.type_guid, userdata.unique_guid, sector)


def check_result(gpt: Gpt, expected: Plan) -> None:
    """After sgdisk: the table must say exactly what the plan said."""
    userdata, ios = gpt.by_name("userdata"), gpt.by_name("ios")
    problems = []
    if userdata is None or (userdata.index, userdata.first, userdata.last) != \
            (expected.userdata, expected.first, expected.new_last):
        problems.append(f"userdata is {userdata}")
    elif (userdata.type_guid, userdata.unique_guid) != (expected.type_guid, expected.unique_guid):
        problems.append("userdata's GUIDs changed")
    if ios is None or (ios.index, ios.first, ios.last) != (expected.ios, expected.ios_first, expected.ios_last):
        problems.append(f"ios is {ios}")
    if problems:
        raise PhoneError("the new partition table is not the planned one: " + "; ".join(problems))
