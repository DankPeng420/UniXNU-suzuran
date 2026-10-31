# UniXNU-lumia950xl

iOS 16.7.16 on a **Microsoft Lumia 950 XL** (RM-1085, MSM8994 / Snapdragon 810, codename *cityman*),
booted with [UniXNU](https://github.com/UniXNU/UniXNU): Apple's open-source XNU kernel built for
this phone, an unmodified iPhone X userland, and a handful of new drivers for the Lumia's hardware.

It boots to the **home screen on all 8 cores**, with **touch, the four side buttons and the battery
icon** working. It is a research port, not a usable phone OS: no GPU acceleration, no radios, no
audio, no camera.

This repository is a fork of [TwisstedToast/UniXNU-lumia950](https://github.com/TwisstedToast/UniXNU-lumia950),
the UniXNU board for the smaller Lumia 950 (MSM8992). The `lumia950xl` branch adds the 950 XL work on
top of commit `06d490c` of that repository. `main` still tracks upstream.

> **No Apple software is included or distributed here.** The kernel is built from Apple's open-source
> XNU, and the iOS userland comes from Apple's own iPhone X firmware image, which the tools download
> from Apple and convert on your machine. Use this only on hardware you own, at your own risk:
> it repartitions the phone's internal storage.

## What works

| Area | State | How |
| --- | --- | --- |
| Boot | ✅ Apple logo → lock screen → home screen | Lumia950XLPkg UEFI → UniXNU menu → DarwinBoot → XNU (xnu-8796) |
| CPUs | ✅ all 8 cores (4× A53 + 4× A57) | GICv2 SMP with PSCI over HVC, cores started one at a time, self-IPI |
| CPU clocks | ✅ A57 cluster raised 384 → 633.6 MHz | `NativeCpuClock`: second cluster PLL at 1267.2 MHz ÷ 2, no voltage change |
| Storage | ✅ eMMC read/write, ADMA2 reads | `NativeSdhci` (8-bit HS 50 MHz); writes confined to the `ios` partition |
| Display | ✅ 1125×2436 canvas scaled into the 1080×1920 framebuffer | `NativeDisplay` (IOMobileFramebuffer + IOSurface, CPU-rendered) |
| Touch | ✅ | `NativeTouch` (Synaptics S3708 RMI4 over I2C) → `NativeHID` |
| Buttons | ✅ power, volume up/down, camera (as Home) | PM8994 PON + PMIC GPIOs over SPMI |
| Battery | ✅ level and charging bolt in the status bar | `NativeBattery` (IOPMPowerSource from the PMI8994 fuel gauge/charger) |
| Charging | ✅ (by the charger's own firmware setup, 1.5 A) | — |
| Boot time | ~3 min to launchd (was ~6) | ADMA2 reads for the 4K code-signature pass |
| GPU / Metal / OpenGL ES | ❌ CoreAnimation renders on the CPU; blur and CoreImage do not work | see [docs](docs/lumia950xl/STATUS.md) |
| Wi-Fi, cellular, Bluetooth, audio, camera, sensors, RTC | ❌ | — |

Details, numbers and known issues: [docs/lumia950xl/STATUS.md](docs/lumia950xl/STATUS.md).
How we got there, boot by boot: [docs/lumia950xl/JOURNAL.md](docs/lumia950xl/JOURNAL.md).
Hardware facts measured on the phone (clocks, rails, fuses, PMIC, touch): [docs/lumia950xl/HARDWARE.md](docs/lumia950xl/HARDWARE.md).

## Trying it

[docs/lumia950xl/INSTALL.md](docs/lumia950xl/INSTALL.md) has the full procedure. In short, you need:

- a Lumia 950 XL that already runs Android (LineageOS) through the usual Lumia boot route, with
  root `adb`, TWRP, and the Windows boot manager still on EFIESP;
- a Mac with Xcode (the kernel build needs the iPhoneOS SDK) and ~60 GB free;
- a Windows PC (or VM) for `bcdedit`;
- patience: the first iOS boot takes several minutes, and every step is manual.

The phone keeps Android: iOS lives in its own GPT partition, and each iOS boot is a one-shot entry
in the Windows boot manager, so the next restart goes back to Android.

## Repository layout (lumia950xl branch)

| Path | What |
| --- | --- |
| `board.toml`, `configs/` | the UniXNU board: what the kernel build compiles, links and prelinks |
| `drivers/` | board drivers: `NativeGicV2` (ArmCpu GICv2 patch), `NativeSdhci`, `NativeDisplay`, `NativeTouch`, `NativeHID`, `NativeCpuClock`, `NativeBattery`, `NativeAES16`, `NativeWatchdog`, … |
| `scripts/` | device tree (`make-lumia950-adt.py`), staging (`stage-kernel.py`), iOS root image (`mkiosroot-root.sh`, `stage-ios-root.py`), log readers |
| `ci/` | patches the board applies to UniXNU and DarwinBoot |
| `unixnu-overlay/` | our extra XNU overlay patches for UniXNU (`patches/xnu-8796/`) |
| `tools/phone/` | `adb` helpers: push a kernel, arm a one-shot iOS boot, write the iOS root, pull logs |
| `docs/lumia950xl/` | status, journal, hardware notes, install guide |

## Credits

This port stands on other people's work. Thank you:

- **[UniXNU](https://github.com/UniXNU/UniXNU)**: the framework that builds XNU for non-Apple ARM64
  boards, the UEFI boot manager, DarwinBoot, the 4K-page code-signature layer and most of the build
  machinery.
- **[TwisstedToast/UniXNU-lumia950](https://github.com/TwisstedToast/UniXNU-lumia950)** (forked from
  [ManGuy-Legend/UniXNU-lumia950](https://github.com/ManGuy-Legend/UniXNU-lumia950)): the Lumia 950 board
  this is built on: the GICv2 ArmCpu port, the first `NativeSdhci`, the payload layout and the HackiOS
  iOS 16 kernel patches.
- **[dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet)** (MIT): the iOS 16.7.16-on-Android
  route we followed for the root filesystem, and the source of the ported display (`NativeDisplay`,
  `NativeSurface`) and HID (`NativeHID`) drivers. Its NOTES.md was invaluable.
- **[HackiOS](https://github.com/oosexgang/hackios-xnu-kernel)**: the xnu-8796 kernel patch set for iOS
  16 on non-Apple hardware that the Lumia boards use.
- **[14sea/ios-sunstone](https://github.com/14sea/ios-sunstone)**, **pawlickerr's negroni**,
  **[ShueiTsuki/ios-pandora](https://github.com/ShueiTsuki/ios-pandora)**: other iOS 16 ports whose notes
  confirmed the merged-HFS+ root route.
- **[eshard](https://eshard.com/blog/emulating-ios-14-with-qemu)**: write-ups on iOS without a GPU.
- **[Lumia950XLPkg / WOA-Project](https://github.com/WOA-Project)** and **LumiaWOA's Boot Shim**: the UEFI
  that boots the phone, and the shim that loads it from the Windows boot manager.
- **[EfiFs](https://efi.akeo.ie)**: the ext2/3/4 UEFI driver that lets DarwinBoot read the kernel from `/cache`.
- **LineageOS** and its `msft/cityman` kernel: register maps for the touch controller, clocks, PMICs,
  CPU PLLs, CPR regulators and charger.
- **[Mandiant macos-UnifiedLogs](https://github.com/mandiant/macos-UnifiedLogs)**: reading iOS's own
  unified log from the phone's storage.
- **Apple**: [XNU](https://github.com/apple-oss-distributions/xnu) and
  [IOHIDFamily](https://github.com/apple-oss-distributions/IOHIDFamily) are open source (APSL 2.0);
  `drivers/NativeHID/` contains IOHIDFamily sources under that licence.

Port work on the 950 XL: [@shrimpapplepro](https://github.com/shrimpapplepro), with Claude (Anthropic) as
a pair programmer.

## Licence

New code in this repository follows UniXNU's licence (BSD-2-Clause-Patent, see the SPDX headers).
Files ported from ios-sweet keep its MIT licence; Apple sources keep the APSL 2.0. The upstream board's
README is kept at [docs/upstream-lumia950-README.md](docs/upstream-lumia950-README.md).
