# UniXNU-suzuran

iOS 16.7.16 on a **Sony Xperia Z5 Compact** (E5823, MSM8994 / Snapdragon 810, codename *suzuran*), running
on [UniXNU](https://github.com/UniXNU/UniXNU): Apple's open-source XNU kernel built for this phone, an
unmodified iPhone 8 userland, and drivers for the phone's hardware.

It boots to the **home screen on all 8 cores**, with **touch, the side keys (camera key as Home) and the
battery** working, in the iPhone 8's 750×1334 layout. Its **Adreno 430 GPU is up**: a user-space
program draws OpenGL ES through Mesa's freedreno driver, and the pixels come out right. iOS itself does
not use the GPU yet. It is a research port, not a usable phone OS: no radios, no audio, no camera, no
power-off.

This is a fork of [shrimpapplepro/UniXNU-lumia950xl](https://github.com/shrimpapplepro/UniXNU-lumia950xl)
(the Lumia 950 XL board, the same SoC). The work for this phone is on the **`suzuran` branch**; the
Lumia's own README is kept at [docs/lumia950xl/README-lumia950xl.md](docs/lumia950xl/README-lumia950xl.md).

> **What comes from Apple.** The source tree contains no Apple binaries. The kernel CI builds is made from
> Apple's open-source XNU, but it is prelinked with Apple's kernel extensions from the iOS 16.7.16
> kernelcache, so it is only kept as a short-lived Actions artifact and is not published as a release.
> The iOS userland is never distributed: it is built from Apple's IPSW on your own machine. Use this only
> on hardware you own, at your own risk: the iOS root replaces the phone's `userdata` partition.

## What works

| Area | State | How |
| --- | --- | --- |
| Boot | ✅ Apple logo → lock screen → home screen | Sony S1 bootloader → our bare-metal loader (`fastboot boot`) → XNU (xnu-8796) |
| Display | ✅ 750×1334 @2× (iPhone 8) fitted into the 720×1280 panel | loader brings up MDSS/DSI cold; `NativeDisplay` (CPU-rendered) |
| CPUs | ✅ all 8 (4× A53 at 768 MHz, 4× A57 at ~634 MHz) | no PSCI on this firmware: TZ `SCM_BOOT_ADDR_MC`, cores parked in a spin table, released by `ArmCpu` |
| Storage | ✅ eMMC read/write | `NativeSdhci`; writes needed the eMMC rail out of low-power mode (RPM vote in the loader) |
| Touch | ✅ | `NativeTouch` (Synaptics ClearPad on BLSP1 QUP6, address 0x2c, reset active high) |
| Buttons | ✅ power, volume, camera = Home | PM8994 PON + PMIC GPIOs (the loader adds the camera key's pull-up) |
| Battery | ✅ level and charging | `NativeBattery` (PMI8994) |
| GPU | 🟡 Adreno 430 runs Mesa freedreno (OpenGL ES 3.1) from user space; not yet used by iOS, slow fences | `NativeAdreno430` + `userland/mesa` |
| Power-off / reboot | ❌ ends in "Halt/Restart Timed Out" | — |
| Wi-Fi, cellular, Bluetooth, audio, camera, sensors, RTC | ❌ | — |

## How it differs from the Lumia board

The Z5 Compact has the same SoC as the Lumia 950 XL but none of its boot route: there is no UEFI, only
Sony's S1 bootloader, which boots an Android `boot.img` (or one sent with `fastboot boot`). So the phone
boots a small bare-metal loader of ours that does what UEFI and DarwinBoot do on the Lumia, and more:

- cold display bring-up (RPM clocks and rails, TrustZone `restore_sec_cfg`, DSI PHY/PLL, panel init,
  self-refreshing command-mode panel);
- the eMMC rail's RPM vote, touch power and reset, the camera key's pull-up;
- secondary cores: TrustZone boot address, power-up of both clusters (the A57 rail is off on this phone),
  each core parked in a spin-table slot that the kernel's `ArmCpu` releases (`/psci spin-table-mailbox`);
- the GPU's power domains, its RPM clock, and the TrustZone calls that release its SMMU;
- the Apple device tree (from the iPhone 8's own, with this phone's devices), the trust cache, boot args,
  and an "addon" archive of files that the kernel writes into the iOS root at boot (the phone has no shell).

The loader and the PC-side scripts that build the boot image are not in this repository yet.

## The GPU (work in progress)

- `drivers/NativeAdreno430`: powers the A430's SP/TP, runs the a4xx hardware init, loads the a420 PM4/PFP
  microcode (from linux-firmware, `LICENSE.qcom`), and serves Linux's **msm DRM interface as an IOKit user
  client** (the contract of HakiOS's hakios-drm shim): buffers, softpin submits as indirect buffers, fences.
  The SMMU is left untranslated by TrustZone, so buffers are physically contiguous and their GPU address is
  their physical address. Fences are still polled.
- `userland/mesa`: Mesa 25.0.7 (Gallium freedreno + OSMesa) cross-built for iOS on the macOS runner, with
  HakiOS's Darwin patches and shim (MIT, [LICENSE.HakiOS](userland/mesa/LICENSE.HakiOS)).
- `userland/gputest`, `userland/gltri`: tests started at boot; results go to the kernel log.
  First light (2026-11-12): `freedreno / FD430 / OpenGL ES 3.1 Mesa 25.0.7`, a red triangle on blue,
  pixel-checked.
- Next: fence interrupts, OCMEM as the GPU's tile memory (Mesa renders with `FD_MESA_DEBUG=sysmem` for now),
  then OpenGL ES for iOS itself (the EAGL hook HakiOS uses), so CoreAnimation and CoreImage run on the GPU.

## Building

Everything builds on GitHub Actions ([.github/workflows/build.yml](.github/workflows/build.yml)); no Mac
is needed. A push to `suzuran` runs:

- `validate`, `efi-preflight`, `gicv2-emulation` (Linux): checks inherited from the Lumia board;
- `kernel` (macOS): XNU + the board's drivers + the prelinked 16.7.16 kernelcache (`lumia950-kernel`
  artifact); logs and the kernel's SHA-256 go to a `ci-results-<run>` branch;
- `userland` (macOS): gputest, Mesa and gltri for iOS, signed ad hoc, with their cdhashes for the trust
  cache, published on a `ci-userland-<run>` branch. A commit with `[userland]` in its message skips the
  kernel job.

The repository variable `MACOS_RUNNER` picks the macOS runner (default `macos-latest`; `macos-15-intel`
also works). Board values for this phone are selected by `NATIVE_TARGET_SUZURAN` (1 in this fork;
`-DNATIVE_TARGET_SUZURAN=0` builds the Lumia's).

## Repository layout (additions on `suzuran`)

| Path | What |
| --- | --- |
| `drivers/NativeAdreno430/` | the GPU driver, its msm DRM user client (`MsmUapi.h`), the a420 microcode |
| `drivers/NativeAddon/` | writes the files of the boot payload's addon archive into the iOS root |
| `drivers/NativeTouch/`, `drivers/NativeCpuClock/` | Z5 Compact values under `NATIVE_TARGET_SUZURAN` |
| `drivers/NativeGicV2/ArmCpu.patch` | spin-table release for firmware without PSCI |
| `userland/` | gputest, gltri, Mesa build (`mesa/build-mesa.sh`), IOKit prototypes for the iPhoneOS SDK |

Everything else is the Lumia 950 XL board as forked; see its README.

## Credits

- **[UniXNU](https://github.com/UniXNU/UniXNU)**: XNU for non-Apple ARM64 boards and most of the build machinery.
- **[shrimpapplepro/UniXNU-lumia950xl](https://github.com/shrimpapplepro/UniXNU-lumia950xl)** and the
  boards before it ([TwisstedToast](https://github.com/TwisstedToast/UniXNU-lumia950),
  [ManGuy-Legend](https://github.com/ManGuy-Legend/UniXNU-lumia950)): the MSM8994 drivers this port runs
  on, and their credits (ios-sweet, HackiOS, LineageOS, …).
- **[HackiOS](https://github.com/oosexgang/hackios-xnu-kernel)**: the kernel patches, and the Mesa-on-Darwin
  route (hakios-drm, freedreno patches) used for the GPU.
- **[Mesa](https://mesa3d.org)** (freedreno, ir3, OSMesa) and **Linux**'s msm/kgsl drivers: how the
  Adreno 4xx works. **linux-firmware**: the a420 microcode.
- **Sony's** open-source kitakami kernel and device trees: this phone's register values.
- **Apple**: [XNU](https://github.com/apple-oss-distributions/xnu) (APSL 2.0).

Port work on the Z5 Compact by [@DankPeng420](https://github.com/DankPeng420), with Claude (Anthropic) as a
pair programmer.

## Licence

New code follows UniXNU's licence (BSD-2-Clause-Patent, see the SPDX headers). Files from HakiOS keep
their MIT licence ([userland/mesa/LICENSE.HakiOS](userland/mesa/LICENSE.HakiOS)), the a420 microcode its
Qualcomm licence ([drivers/NativeAdreno430/LICENSE.qcom](drivers/NativeAdreno430/LICENSE.qcom)), Apple
sources the APSL 2.0, and everything inherited from the Lumia board its own terms.
