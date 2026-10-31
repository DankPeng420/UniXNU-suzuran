# Journal: bringing iOS 16 up on the Lumia 950 XL

The order things happened in, and what each step taught. Times are local (UTC+8). The phone is
called *Yuzu* below.

## 2026-10-22: assessment, dropped

- The only plain-arm64 (ARMv8.0, no PAC) iOS userland that fits is the iPhone X's iOS 16.7.16 (xnu-8796).
- Blockers on MSM8994: UniXNU's ArmCpu was GICv3-only, there was no eMMC driver and the phone has no UART.
  Set aside.

## 2026-10-28/29: revived on the Lumia 950 board

- [TwisstedToast/UniXNU-lumia950](https://github.com/TwisstedToast/UniXNU-lumia950) had a GICv2 ArmCpu, an
  untested `NativeSdhci` and the HackiOS iOS 16 kernel patches. Copied as the 950 XL board (8 cores:
  MPIDRs 0x0-0x3 A53, 0x100-0x103 A57).
- Boot route: the Windows boot manager on EFIESP, a one-shot BCD `bootsequence` to LumiaWOA's BootShim →
  Lumia950XLPkg `UEFI.elf` 2.x (1.x/the 2018 shim only shows "Preloader needs upgrade"). The kernel
  (56 MB) does not fit on EFIESP (32 MB), so DarwinBoot reads it from `/cache` (ext4) through EfiFs's ext2
  driver.
- **First kernel boots**, one panic at a time:
  - FIPS POST failed: a diagnostic patch to `__text` broke corecrypto's HMAC. Re-seal after patching.
  - `!prng_ready`: source corecrypto and the prelinked kext both registered the PRNG.
  - AppleImage4 wanted `/chosen` properties (`mix-n-match-prevention-status`, `manifest-properties` with
    BNCH/ECID, `boot-manifest-hash`).
  - A silent stall: DarwinBoot hard-codes graphics console mode; a text-console overlay fixed it.
  - `PIDR2 0x0 does not report GICv2`: the QGIC2 leaves GICD_PIDR2 unimplemented. Accept PIDR2 = 0 when
    TYPER/IIDR are non-zero.
  - The virtual timer is PPI 20 on MSM8994, not 27.
  - `pthread kernel extension not loaded`: prelink Apple's `com.apple.kec.pthread` (the source one is
    Darwin 23 only).
  - **22:17: XNU completes kernel startup**, then waits for a root device (no storage driver linked yet).
- Ramdisk detour: the restore ramdisk mounted as root, `launchd` exec panicked on missing AMFI. Prelinked
  Apple's AMFI (and its dependencies) with `early_security_init`, and accepted version-1 trust caches in
  DarwinBoot.

## 2026-10-30: storage

- Repartitioned in TWRP: Android `userdata` shrunk to 8 GiB, a new 17 GiB GPT partition `ios` (p55),
  Android data kept.
- `NativeSdhci` bound against the 16.7.16 IOStorageFamily vtable (`original-bindings-16.7.16.S`).
- The firmware's eMMC card does not answer after ExitBootServices, so the driver re-identifies it from
  CMD0: 400 kHz → 25 MHz → 8-bit → HS 50 MHz, every step proved by re-reading EXT_CSD. CMD3 hung until a
  40 µs settle after every interrupt-status clear (sdhci-msm's SLOW_INT_CLR).
- **22:51: eMMC works in iOS**, 55 partitions published.
- Root filesystem: the ios-sweet route, not UniXNU's sealed APFS (that needs iPadOS 17's cryptex ignition):
  one merged case-sensitive journaled HFS+ volume with OS + SystemOS + AppOS, files decompressed (the
  kernel only has zlib decmpfs; the IPSW uses LZBITMAP), cryptex files hard-linked into place.
  `scripts/mkiosroot-root.sh` + `stage-ios-root.py`. Kernel overlay `zzu-unsealed-rw-root.patch` answers
  the root-authentication ioctl and mounts it read-write.

## 2026-10-31: pixels, touch, SMP, home screen

- **First iOS pixels**: the Apple logo through `NativeDisplay` (ios-sweet's IOMobileFramebuffer/IOSurface
  port) on the GOP framebuffer.
- Reading iOS's own logs: the unified log on the `ios` partition, parsed with Mandiant's
  macos-UnifiedLogs (`scripts/read-ioslog.sh`); NativeSdhci also writes the kernel console to a ring at
  the end of the partition every second (`scripts/read-natlog.py`).
- **Stuck at the lock screen** transition: SpringBoard made a synchronous XPC call to mediaserverd for the
  flashlight because the iPhone X's device tree says it has a camera flash. The ADT drops `flash`.
- Display: the GOP framebuffer is 1080×1920 (the panel scales it to 1440×2560); the 1125×2436 canvas is
  fitted into 886×1920 with side bars.
- **Touch**: Synaptics S3708 over polled I2C (QUP v2 tags, 400 kHz), RMI4 F01/F12.
- **SMP**: all 8 cores after two fixes. Starting cores in parallel left some stuck; start them one at a
  time. A cross-call panic (`await_while_equals32` timeout) came from XNU's `SIGPdisabled`, cleared only
  by a core's first IPI; each core now sends itself one.
- Touch "dead" with 8 cores was iOS auto-lock: the display is "off" (kept lit), so touches are dropped.
  Tap-to-wake sends a power-button press.
- **~16:30: home screen on 8 cores, with touch.**

## 2026-10-31 evening: speed, keys, battery, faster boot

- **Side keys**: PM8994 PON real-time status (power, volume down) and PM8994 GPIOs 3/4 (volume up, camera
  as Home), read over SPMI.
- **Why it was slow**: backboardd's samples were all CoreAnimation's software renderer, and the CPU
  readout showed the firmware's clocks: A53 at 768 MHz, **A57 at 384 MHz** (a fifth of its 1958 MHz).
- Rails and fuses: A53 1000 mV, A57 900 mV, MX 1035 mV; fused open-loop voltages per CPR fuse corner.
  The voltage controller (SAW2) is idle, so rails take direct SPMI writes.
- **A57 at 960 MHz** on 1025 mV measured 960 MHz and ran ~6 minutes, then the SoC reset itself (PMIC
  power-off reason PS_HOLD, no panic). Linux also switches the A57 memory-assist setting above that fuse
  corner, through the secure monitor. Back inside the firmware's fuse corner: **633.6 MHz at the same
  900 mV**, 1.65× the original, stable.
- **Charging works under iOS** with no driver (the charger keeps the firmware's 1.5 A setup).
  **`NativeBattery`** publishes an IOPMPowerSource: the status bar shows the level and the bolt. (Its first
  version panicked: the catalogue's `init(OSDictionary *)` skips `IOPMPowerSource::init()`.)
- **2× rendering** experiment: smoother, but a red status-bar banner (no layout for 375×812 points at 2×)
  and Settings/app crashes: new icons go through CoreImage, which has no CPU renderer on iOS 16. Reverted.
- **Boot time**: the 5-minute wait before launchd was the 4K code-signature pass reading the 2.7 GB shared
  cache: 278 s reading eMMC by PIO, 23 s hashing. **ADMA2 reads** (self-tested against PIO, falling back
  on any error): 100 s, launchd at ~180 s instead of ~350 s. Then 512 KiB per command.
