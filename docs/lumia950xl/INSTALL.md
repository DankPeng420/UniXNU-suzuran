# Installing iOS 16.7.16 on a Lumia 950 XL

Two ways: the **installer** (`installer/install.py`, below), which runs the whole procedure from a Mac
with checks at every step, or the **manual procedure** (sections 0 to 9), which is what the installer
automates and what to read when something goes wrong. Two steps (repartitioning and the boot
configuration) can leave the phone needing a re-flash if they go wrong. You are responsible for your
device.

## The installer

Needs: a Mac with ~35 GB free and Python 3.11+ (`brew install python android-platform-tools`), and the
phone in LineageOS with USB debugging and **Rooted debugging** on, TWRP as its recovery, on a USB cable
(not Wi-Fi: the root image is 17.5 GB). No Windows PC, no Xcode: the installer edits the BCD itself
(`installer/bcd.py`, checked against bcdedit's own output) and downloads a prebuilt kernel.

```sh
git clone -b lumia950xl https://github.com/shrimpapplepro/UniXNU-lumia950xl && cd UniXNU-lumia950xl
python3 installer/install.py install --dry-run    # see what it would do; changes nothing on the phone
python3 installer/install.py install              # ~1.5 h, mostly downloading and writing
python3 installer/install.py boot                 # start iOS once; the next boot is Android again
```

`install` runs these stages and records each one in `~/UniXNU-lumia950xl-install/state.json`, so an
interrupted run continues where it stopped (run one stage again with `install.py <stage>`):

| Stage | What it does |
| --- | --- |
| `preflight` | checks the Mac's tools and space, the phone's model (`ro.boot.hardware` = `cityman`), adb root, sgdisk, EFIESP, /cache and the BCD |
| `backup` | copies EFIESP, the BCD, the GPT and Android's `/data` to the Mac |
| `partition` | plans the split, asks you to type `SPLIT`, shrinks `userdata` in TWRP, adds `ios` (default 18 GiB, `--ios-size`) |
| `fetch` | downloads the 7 IPSW members it needs from Apple (5.8 GB of the 6.2 GB IPSW, resumable), UEFI 2.24b, BootShim, EfiFs and the kernel release, each pinned by SHA-256 in `installer/manifest.toml` |
| `root` | builds the HFS+ root image (`scripts/mkiosroot-root.sh`, asks for your Mac password) |
| `write` | asks you to type `WRITE`, streams the image to `ios`, verifies it by SHA-256 |
| `stage` | kernel + ADT (with a per-phone seed) + trust cache + menu (`scripts/stage-kernel.py`) |
| `esp` | UEFI, BootShim and the menu onto EFIESP; an "iOS (UEFI)" entry in the BCD |
| `kernel` | the kernel onto /cache, the menu configuration onto EFIESP |

Other commands: `status`, `update` (a newer kernel release: fetch, stage, push), `restore-bcd` (the BCD
from the first backup). If EFIESP already has a UniXNU install from elsewhere, `esp` stops and lists
the files it would replace; `--force` replaces them.

## The manual procedure

This is the procedure used on our phone, written so someone else can repeat it. Read it all first.

> **What gets changed on the phone:** a new GPT partition (`ios`) carved out of Android's `userdata`,
> files added to EFIESP (UEFI, the UniXNU menu), the Windows boot configuration (BCD) and files in
> `/cache`. Android keeps working: iOS is only started by a one-shot boot entry.
>
> **What you need to supply:** nothing from Apple by hand. `unixnu fetch` downloads the iPhone X
> (iPhone10,3/10,6) iOS 16.7.16 IPSW (build 20H392) from Apple's servers on your machine.

## 0. Requirements

**Phone:** Lumia 950 XL (MSM8994, "cityman") already running Android through the usual Lumia route:
Windows Boot Manager on EFIESP → Boot Shim → lk2nd → Android (we used LineageOS 20), with:
- root `adb` (`adb root` works),
- TWRP (for resizing `userdata`),
- `sgdisk` on the Android side (LineageOS ships it in `/system/bin`),
- at least ~18 GiB you can take from `userdata`.

**Build machine:** a Mac (Apple silicon or Intel) with:
- Xcode with the iPhoneOS SDK (the XNU build replays Apple's compile commands), Homebrew `llvm`, `lld`,
  `unifdef`, Python 3, `gh` (logged in; UniXNU's fetcher uses `GITHUB_TOKEN`);
- ~60 GB free (IPSW, sources, the 17.5 GB root image and its raw copy);
- an [EDK II / Mu_Basecore](https://github.com/microsoft/mu_basecore) checkout if you build the boot menu
  yourself (`scripts/build-payload.sh`).

**A Windows PC or VM** for `bcdedit` (editing the phone's BCD file offline).

**Files from other projects:**
- Lumia950XLPkg `UEFI.elf` **2.x** for the 950 XL ([WOA-Project](https://github.com/WOA-Project)),
- LumiaWOA's **Boot Shim** `BootShim.efi` that loads `\UEFI.elf` (the old 2018 shim only shows
  "Preloader needs upgrade" with UEFI 2.x),
- [EfiFs](https://efi.akeo.ie) `ext2_aa64.efi` (v1.13 worked), so DarwinBoot can read `/cache`.

## 1. Back up

From Android (`adb root`), copy off and keep somewhere safe:

```sh
adb shell "dd if=/dev/block/mmcblk0p35 2>/dev/null" > efiesp.img          # EFIESP (check: by-name/EFIESP)
adb shell "sgdisk --backup=/data/local/tmp/gpt.bak /dev/block/mmcblk0" && adb pull /data/local/tmp/gpt.bak
adb shell "tar -C /data -cf - ." > data.tar                                # your Android data
```

Also copy the BCD itself: `\EFI\Microsoft\BOOT\BCD` from EFIESP.

## 2. Make the `ios` partition

Partition numbers and sectors below are from our phone: **check yours with `sgdisk --print /dev/block/mmcblk0`
and recompute.** `userdata` was p54, the last partition. Android's `sgdisk` (AOSP's build) only takes
long options with `=`: `-p` and `-i 54` fail with "invalid option".

1. In **TWRP** (it mounts `userdata` at `/sdcard`, unmount it first):
   ```sh
   umount /sdcard; e2fsck -f /dev/block/mmcblk0p54
   resize2fs /dev/block/mmcblk0p54 2097148          # 8 GiB in 4 KiB blocks, leaves 16 KiB spare
   ```
2. Reboot to **Android**, then recreate p54 smaller with the **same** start sector, type, unique GUID and
   name, and add p55 for iOS (type AF00, Apple HFS+, name `ios`):
   ```sh
   sgdisk --info=54 /dev/block/mmcblk0              # note the type and unique GUIDs and first sector
   sgdisk --delete=54 --new=54:8323072:25100287 --typecode=54:<type GUID> \
          --partition-guid=54:<unique GUID> --change-name=54:userdata \
          --new=55:25100288:61069311 --typecode=55:AF00 --change-name=55:ios /dev/block/mmcblk0
   ```
3. Reboot, check that Android's data is intact and that `/dev/block/by-name/ios` exists.

The iOS root image is 17,500 MiB, so `ios` must be at least that big.

## 3. The UEFI boot route on EFIESP

1. Copy `UEFI.elf` to the root of EFIESP and `BootShim.efi` to `\Windows\System32\BOOT\BootShimAA64.efi`.
2. On Windows, add a boot application for it to a **copy** of the phone's BCD (just opening a store with
   `bcdedit` modifies it, so always work on copies). Copying the existing "Boot Shim" entry (the
   `{bootmgr}` default, which starts lk2nd) keeps its device setting and integrity flags:
   ```bat
   copy BCD.orig BCD.uefi
   bcdedit /store BCD.uefi /enum {bootmgr}
   rem note the "default" GUID: the Boot Shim entry, {BOOTSHIM-GUID}
   bcdedit /store BCD.uefi /copy {BOOTSHIM-GUID} /d "UEFI AArch64"
   rem prints {NEW-GUID}
   bcdedit /store BCD.uefi /set {NEW-GUID} path \Windows\System32\BOOT\BootShimAA64.efi
   bcdedit /store BCD.uefi /displayorder {NEW-GUID} /addlast
   ```
   Our resulting entry: `path \Windows\System32\BOOT\BootShimAA64.efi`, `description UEFI AArch64`,
   `nointegritychecks Yes`, `testsigning Yes`; the default stays the Boot Shim → lk2nd → Android.
3. Make the **one-shot** variant, which boots the UEFI once and then falls back to the default (Android):
   ```bat
   copy BCD.uefi BCD.oneshot
   bcdedit /store BCD.oneshot /bootsequence {NEW-GUID}
   bcdedit /store BCD.oneshot /set {bootmgr} persistbootsequence No
   ```
   Windows Boot Manager deletes the `bootsequence` after using it, so `BCD.oneshot` has to be copied back
   for every iOS boot: that is what `tools/phone/arm-ios-boot.sh` does.

## 4. Build the boot menu (UniXNU menu + DarwinBoot)

```sh
git clone https://github.com/UniXNU/UniXNU && cd UniXNU && git checkout 1a01c2a
git submodule update --init --depth 1 drivers/DarwinBoot
UNIXNU_SOURCE=$PWD UNIXNU_EDK2=/path/to/mu_basecore bash /path/to/this/repo/scripts/build-payload.sh /path/to/menu
```

`build-payload.sh` applies this board's DarwinBoot patches (`ci/darwinboot-*.patch`: payload on another
volume, accurate hand-off label, version-1 trust caches). The output tree (`EFI/BOOT/BOOTAA64.EFI`,
`EFI/UniXNU/…`) is the `--menu-tree` for step 7.

## 5. Build the kernel

```sh
cd UniXNU                                     # the 1a01c2a checkout from step 4
git apply /path/to/this/repo/ci/unixnu-16x.patch                  # the iOS 16 (xnu-8796) route
cp /path/to/this/repo/unixnu-overlay/patches/xnu-8796/*.patch patches/xnu-8796/
patch -p1 < /path/to/this/repo/unixnu-overlay/build-xnu-4k-baseline-native-sdhci.patch

export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
export PATH=/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/unifdef/bin:/opt/homebrew/opt/lld/bin:$PATH
export PYTHONPATH=/path/to/this/repo/ci-tools GITHUB_TOKEN=$(gh auth token)
python3 unixnu defconfig lumia950_defconfig -D /path/to/this/repo
python3 unixnu build kernel -D /path/to/this/repo
```

The first build fetches the sources and the 20H392 IPSW (into `out/ipsw/20H392`) and takes a while;
rebuilds take about 4 minutes. The result is `out/lumia950/kernel/kernel.macho` (~56 MB).

## 6. Build and write the iOS root

```sh
sudo IPSW=$PWD/out/ipsw/20H392 OUT=/path/to/iosroot sh /path/to/this/repo/scripts/mkiosroot-root.sh
python3 /path/to/this/repo/scripts/make-trustcache-bundle.py /path/to/iosroot/root.trustcache \
    out/ipsw/20H392/087-86698-044.dmg.trustcache \
    out/ipsw/20H392/087-86593-047.dmg.trustcache+out/ipsw/20H392/087-86715-047.dmg.trustcache
bash /path/to/this/repo/tools/phone/write-ios-root.sh /path/to/iosroot/ios-root.sparseimage
```

`mkiosroot-root.sh` merges the OS, SystemOS and AppOS images into one decompressed, case-sensitive
journaled HFS+ volume and applies `stage-ios-root.py` (cryptex hard links, logd stores, a software keybag,
a few userland tweaks). The write streams the raw image to `ios` and verifies it by hash (~45 minutes in
all over USB 2).

## 7. Stage and push the kernel

```sh
ARGS="-v cpus=8 sheng-smp=1 -no_corpses rd=disk0s55 unsealed_root=1 rootrw=1 sdhcirw=1 ignition_level=1 launchd_unsecure_cache=1 wdt=-1 lumiafb_stayon=1"
python3 /path/to/this/repo/scripts/stage-kernel.py --kernel out/lumia950/kernel/kernel.macho \
    --unique-chip-id $(openssl rand -hex 8) --menu-tree /path/to/menu \
    --ntfs-driver /path/to/ext2_aa64.efi --trustcache /path/to/iosroot/root.trustcache \
    --args "$ARGS" --out boot --payload-out cachevol
```

- `rd=disk0s55`: the `ios` partition's BSD name in iOS. It is the partition's GPT index on our phone
  (55); change it if yours differs.
- `--unique-chip-id` is a 64-bit seed for the device tree's identity; keep the same value across builds.
- `--ntfs-driver` takes any UEFI filesystem driver: here EfiFs's ext2 driver. It is staged as
  `Drivers/ntfs.efi` and the generated `unixnu.toml` loads it under that name.

Copy the `boot/EFI/BOOT` and `boot/EFI/UniXNU` trees onto EFIESP once (mount it from Android:
`mount -t vfat /dev/block/mmcblk0p35 /data/local/tmp/esp`). From then on, for every new kernel:

```sh
bash tools/phone/push-kernel.sh cachevol boot/EFI/UniXNU/unixnu.toml
```

## 8. Boot iOS

```sh
bash tools/phone/arm-ios-boot.sh BCD.oneshot
```

The phone reboots: Windows Boot Manager → Boot Shim → UEFI → UniXNU menu (it starts the iOS entry after
its timeout) → verbose kernel text → Apple logo. Expect about 3 minutes to launchd and a few more to the
lock screen; the first boot after writing the root takes longer.

To return to Android: hold **power + volume down** until the phone restarts. The next boot is Android.

## 9. Logs

```sh
bash tools/phone/pull-natlog.sh natlog.bin && python3 scripts/read-natlog.py natlog.bin > boot.txt
```

The kernel console of the last iOS boots, written to the end of the `ios` partition once a second. For
iOS's own unified log and crash reports: `tools/phone/pull-ios-partition.sh` + `scripts/read-ioslog.sh`.
