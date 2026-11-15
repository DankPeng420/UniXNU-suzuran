# Booting iOS 16 on the Xperia Z5 Compact

This folder has everything outside the kernel that is needed to boot the port: the bare-metal loader, the
scripts that build the boot image, the scripts that build the iOS root from Apple's IPSW and write it to
the phone, and the Windows batch file that boots it. The kernel and the userland tools come from this
repository's CI.

**What you get:** the iPhone 8 userland of iOS 16.7.16 (20H392) on all 8 cores, rendered in software, with
touch, keys (camera key = Home) and battery. It takes about 4 minutes from power-on to the lock screen
(the very first boot takes longer, about 9 minutes, while the kernel caches code-signature hashes). There is
no Wi-Fi, cellular, audio, camera, or power-off.

**This image is `full33`**: kernel from commit `8840f7f`, loader v19, the
boot-args below. It is the last stable image. The OpenGL ES work done afterwards is described at the end;
it is not part of this image.

> **No Apple files are in this repository.** The boot image contains Apple's kernel extensions (prelinked
> into the kernel CI builds), Apple's iPhone 8 device tree and Apple's trust cache, and the phone's root is
> built from Apple's IPSW. So you build the image yourself from your own IPSW download; it is never
> published. Use it only on a phone you own.

> **This erases the phone's `userdata`.** The iOS root replaces partition 42 (`userdata`). Back up anything
> you want first. Nothing here ever writes the TA partition (`p1`, the phone's DRM keys); do not change
> that. Booting the image itself is a RAM boot (`fastboot boot`): it writes nothing to flash.

## What you need

- A Sony Xperia Z5 Compact **E5823** (*suzuran*) with an **unlocked bootloader** and **TWRP** in the
  recovery partition (`boot\restore-twrp.bat` flashes `TWRP_3.7.0_9-0-recovery_suzuran_2025-03-15.img`
  if it is placed in this folder).
- A Windows PC with Android **platform-tools** in `C:\Program Files\platform-tools\` and **WSL** (Ubuntu).
- Apple's IPSW for the 4.7-inch P3 devices (iPhone 8 / SE 2 / SE 3), iOS 16.7.16:
  `iPhone_4.7_P3_16.7.16_20H392_Restore.ipsw` (about 6 GB), placed in `ipsw\` inside this folder.
- A GitHub account, to build the kernel on a fork of this repository with GitHub Actions (no Mac needed).
- About 30 GB free on the PC.

Copy this folder to a working folder on the PC, for example `C:\Users\<you>\Desktop\suzuran-port\`.
The scripts below are run from WSL as `bash /mnt/c/Users/<you>/Desktop/suzuran-port/root/NN-....sh`;
each writes a log next to itself.

## 1. Tools in WSL (once)

1. `root/61-wsl-setup.sh`: apfs-fuse, hfsprogs, rsync. If your Ubuntu has no `apfs-fuse` package, run
   `root/61b-wsl-setup.sh` (builds it from source), then `61c` / `61d` if the build fails with a newer
   CMake / GCC.
2. `root/64-wsl-kernel.sh`: the stock WSL kernel has no HFS+. This builds Microsoft's WSL kernel with HFS+,
   FUSE and loop built in and points `.wslconfig` at it. Then run `wsl --shutdown` in PowerShell and reopen
   Ubuntu.

## 2. The iOS root (once)

1. `root/70-wsl-iphone8.sh`: extracts the OS, SystemOS and AppOS images and their trust caches from the IPSW
   (checked against the IPSW's own CRCs), builds a 20 GB HFS+ image `~/iosbuild8/ios-root.img`, stages it
   for this port (`stage-ios-root.py`), and writes `root/out8/` (`d201ap.adt`, `root.trustcache`,
   `ios-root.img.sha256`).
2. `root/73-wsl-launchd-plist.sh`: copies launchd's job list out of the image into
   `root/out8/launchd.plist` (the boot image's addon adds a job to it).
3. **Writes the phone.** Boot the phone into TWRP, turn MTP off (Mount → Disable MTP), connect USB, then run
   `root/71-wsl-phone-write-iphone8.sh`. It checks the model (`ios-prep.sh`), unmounts `/data`, renames GPT
   entry 42 to `ios` with the Apple HFS+ type (`gpt42.py` touches only the two GPT headers and the two
   sectors holding entry 42, and keeps the originals in `root/out/`), streams the image into
   `/dev/block/mmcblk0p42`, and compares the SHA-256 read back.
   `root/69-wsl-check.sh` checks the result read-only.

## 3. Kernel and userland tools (GitHub Actions)

1. Fork this repository. In the fork, push commit `8840f7f` to a branch named
   `suzuran` (`git push origin 8840f7f:refs/heads/suzuran --force`) (the workflow runs on pushes to `suzuran`), or push the current `suzuran` branch (its kernel
   should behave the same with these boot-args, but was not tested with them).
2. Wait for the workflow (about 25 minutes). Download the **`lumia950-kernel`** artifact of that run and save
   it as `ci\lumia950-kernel-cpu2.zip` in this folder.
3. The `userland` job publishes a branch `ci-userland-<run id>` of the fork; copy the files under its
   `run/userland/` into `ci\userland\` (`gputest`, `gltri`, `libOSMesa.8.dylib`, `cdhashes.txt`, …).
   They are GPU tests that run once at boot; iOS does not need them, but the addon expects them.

## 4. The boot image

From the `loader` folder (Python 3 on Windows or WSL; the loader build needs clang, ld.lld and
llvm-objcopy, e.g. in WSL):

```
# the loader (reproduces Image-loader-v19 bit for bit)
./build-loader.sh loader-v19 Image-loader-v19

# the device tree: the iPhone 8's (D201AP), with this phone's devices
python3 make-suzuran-adt.py --output suzuran-smp8-iphone8d.adt --canvas 750x1334 --canvas-scale 2 \
  --dram-size 0x80000000 --owned-memory-size 0x2E000000 --cpu-regs 0,1,2,3,0x100,0x101,0x102,0x103 \
  --psci-method smc --spin-table --addon --unique-chip-id 5A35434F4D504354 --chip-id 0x8015 \
  --trust-cache --iphone-identity ../root/out8/d201ap.adt

# the addon (files the kernel writes into the iOS root) and the trust cache with their hashes
python3 build-addon.py ../ci/userland

# the boot image: loader + kernel + device tree + boot-args + trust cache + addon
python3 build-full33.py
```

`build-full33.py` writes `loader-xnu-full33.img` into this folder.

## 5. Booting

1. Phone off. Hold **volume up** and plug in USB: the LED turns blue (fastboot mode).
2. Run `boot\boot-full33.bat`. It runs `fastboot boot loader-xnu-full33.img` and logs to
   `boot\boot-full33.log`.
3. The loader's bar appears, then the kernel's text console, the Apple logo, and the lock screen after about
   4 minutes. The camera key is Home.

To leave iOS: hold power + volume up until the phone vibrates (forced off). Nothing was written to flash,
so TWRP and Android's boot are as they were (Android itself no longer has its `userdata`).

### Boot-args of this image (`loader/cmdline-full33.txt`)

| Boot-arg | Meaning |
| --- | --- |
| `-v` | verbose boot (kernel console on screen until the first frame) |
| `cpus=8 sheng-smp=1` | all 8 cores |
| `-no_corpses` | no crash corpses (saves memory) |
| `rd=disk0s42` | the root is GPT partition 42 (`ios`) |
| `unsealed_root=1 rootrw=1 sdhcirw=1` | writable root; NativeSdhci may write, only inside `ios` |
| `ignition_level=1 launchd_unsecure_cache=1` | boot the unsealed root |
| `wdt=-1` | no watchdog |
| `keepsyms=1` | symbols in panics |
| `lumiafb_stayon=1` | the panel stays lit when iOS asks for display off |
| `lumiafb_hz=30` | panel copies per second |
| `procmon=10 procmon_tasks=0` | CPU use per process in the log every 10 s |
| `nativesample=launchd` | sampled launchd stacks in the log |
| `nativea53=0` | A53 cluster left at the firmware's 768 MHz (raising it gained little and ran hotter) |

## 6. The log

The kernel writes its console into a ring at the end of the `ios` partition. Boot the phone into TWRP,
turn MTP off, then run `root/68-wsl-read-log.sh`: it copies the ring to `root/out/natlog.bin` and the text
to `root/out/natlog.txt` (one `===== NATLOG boot N =====` per boot; each line is stamped with seconds since
power-on).

For crashes, `root/74-wsl-dyld-cache.sh` copies the iPhone 8 dyld shared cache out of the IPSW into
`root/dsc/`, and `python3 root/dscsym.py root/dsc SLIDE ADDR...` names the addresses of a crash line
(`CRASH name pc ... bt ...`, with its `slide`).

## Known problems

- installcoordinationd crashes twice per boot ("Failed to create data directory", ENOTSUP); app installation
  probably does not work.
- Widget and poster extensions time out at boot (killed and restarted); harmless.
- Power-off and reboot end in "Halt/Restart Timed Out": force the phone off with power + volume up.

## The OpenGL ES route (experimental, not in this image)

After full33, the `suzuran` branch added a route that renders iOS's UI on the Adreno 430: an OpenGL ES layer
(HakiOS's eagl.c on Mesa freedreno) injected into backboardd by the kernel (boot-args
`dyldallow=1 dyldinsert=backboardd suzurangl=1`), zero-copy screen surfaces in GPU memory, and a page flip
on the MDSS pipe the bootloader leaves running. It boots and draws the lock screen, home screen and
Settings correctly, but two problems were left open: frames are shown with some layers missing (flicker),
and the home screen's app icons are not drawn. See the commit history from `8840f7f` on, and
`docs/` for the details.
