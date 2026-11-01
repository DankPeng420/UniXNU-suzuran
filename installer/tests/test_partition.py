"""The partition plan against our phone's real GPT (fixtures/sgdisk-*.txt, unique GUIDs replaced).

On 2026-10-30 the phone was split by hand into userdata 8323072-25100287 and ios 25100288-61069311
(Android 8 GiB, iOS 17.15 GiB); the planner has to arrive at the same table from the same request.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

import partition  # noqa: E402
import phone  # noqa: E402

FIXTURES = HERE / "fixtures"


def gpt(name: str, info: dict[int, str]) -> phone.Gpt:
    table = phone.parse_sgdisk_print((FIXTURES / name).read_text())
    for index, file in info.items():
        part = next(p for p in table.partitions if p.index == index)
        part.type_guid, part.unique_guid = phone.parse_sgdisk_info((FIXTURES / file).read_text())
    return table


BEFORE = lambda: gpt("sgdisk-p-before.txt", {54: "sgdisk-i54-before.txt"})  # noqa: E731


class Parse(unittest.TestCase):
    def test_table(self):
        table = BEFORE()
        self.assertEqual((table.sector_size, table.total_sectors, table.last_usable), (512, 61071360, 61071326))
        self.assertEqual(len(table.partitions), 54)
        self.assertEqual(table.by_name("EFIESP").index, 35)
        userdata = table.by_name("userdata")
        self.assertEqual((userdata.index, userdata.first, userdata.last), (54, 8323072, 61069311))
        self.assertEqual(userdata.type_guid, "0FC63DAF-8483-4772-8E79-3D69D8477DE4")
        self.assertEqual(table.by_name("IS_UNLOCKED").first, 64)

    def test_android_sgdisk(self):
        # The phone's own sgdisk (AOSP 1.0.4) after the split: "logical/physical" header.
        table = gpt("android-print.txt", {54: "android-info54.txt", 55: "android-info55.txt"})
        self.assertEqual((table.sector_size, table.last_usable, len(table.partitions)), (512, 61071326, 55))
        ios = table.by_name("ios")
        self.assertEqual((ios.index, ios.first, ios.last), (55, 25100288, 61069311))
        self.assertEqual(ios.type_guid, "48465300-0000-11AA-AA11-00306543ECAC")


class PlanOurPhone(unittest.TestCase):
    def test_same_split_as_by_hand(self):
        table = BEFORE()
        ios_bytes = (61069311 - 25100288 + 1) * 512
        plan = partition.plan(table, data_used_bytes=int(2.2e9), ios_bytes=ios_bytes)
        self.assertEqual((plan.userdata, plan.ios), (54, 55))
        self.assertEqual((plan.first, plan.new_last, plan.ios_first, plan.ios_last),
                         (8323072, 25100287, 25100288, 61069311))
        self.assertEqual(plan.fs_blocks, 2097148)  # what we passed to resize2fs by hand
        self.assertEqual(plan.android_bytes, 8 * partition.GIB)
        self.assertEqual(plan.sgdisk_command(),
                         "sgdisk --delete=54 --new=54:8323072:25100287 "
                         "--typecode=54:0FC63DAF-8483-4772-8E79-3D69D8477DE4 "
                         "--partition-guid=54:1C0A2B3D-0000-4000-8000-00000000A002 --change-name=54:userdata "
                         "--new=55:25100288:61069311 --typecode=55:AF00 --change-name=55:ios /dev/block/mmcblk0")

        after = gpt("sgdisk-p-after.txt", {54: "sgdisk-i54-before.txt", 55: "sgdisk-i55-after.txt"})
        partition.check_result(after, plan)

    def test_ios_start_is_aligned_and_fits(self):
        plan = partition.plan(BEFORE(), int(2e9), 18 * partition.GIB)
        self.assertEqual(plan.ios_first % partition.ALIGN_SECTORS, 0)
        self.assertGreaterEqual(plan.ios_bytes, 18 * partition.GIB)
        self.assertLess(plan.fs_blocks * partition.FS_BLOCK, plan.android_bytes)

    def test_refusals(self):
        with self.assertRaisesRegex(phone.PhoneError, "at least"):
            partition.plan(BEFORE(), int(2e9), 16 * partition.GIB)
        with self.assertRaisesRegex(phone.PhoneError, "Android"):
            partition.plan(BEFORE(), int(9e9), 18 * partition.GIB)
        with self.assertRaisesRegex(phone.PhoneError, "already has"):
            partition.plan(gpt("sgdisk-p-after.txt", {54: "sgdisk-i54-before.txt"}), int(2e9),
                           18 * partition.GIB)
        table = BEFORE()
        table.partitions.append(phone.Partition(56, 61069312, 61071000, "extra"))
        with self.assertRaisesRegex(phone.PhoneError, "last partition"):
            partition.plan(table, int(2e9), 18 * partition.GIB)

    def test_check_result_catches_a_wrong_table(self):
        plan = partition.plan(BEFORE(), int(2e9), 18 * partition.GIB)
        after = gpt("sgdisk-p-after.txt", {54: "sgdisk-i54-before.txt", 55: "sgdisk-i55-after.txt"})
        with self.assertRaises(phone.PhoneError):
            partition.check_result(after, plan)  # the by-hand table was 17.15 GiB, not 18


if __name__ == "__main__":
    unittest.main()
