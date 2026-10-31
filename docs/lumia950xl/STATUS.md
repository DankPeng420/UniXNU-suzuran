# Status: iOS 16.7.16 on the Lumia 950 XL

Last updated 2026-11-01. Phone: Lumia 950 XL (RM-1085, MSM8994 v2.1, 3 GB RAM, Samsung 32 GB eMMC),
running LineageOS with lk2nd; iOS boots through the Windows boot manager's one-shot entry.

## Boot chain

```
Windows Boot Manager (EFIESP, BCD one-shot bootsequence)
  → BootShim (LumiaWOA) → Lumia950XLPkg UEFI.elf (2.x)
  → \EFI\BOOT\BOOTAA64.EFI = UniXNU menu (unixnu.toml)
  → DarwinBoot (kernel, ADT and trust cache from /cache via EfiFs ext2)
  → XNU xnu-8796 (DEVELOPMENT, 4K pages) + prelinked Apple kexts from the iOS 16.7.16 kernelcache
  → root: one merged, decompressed, case-sensitive journaled HFS+ volume on GPT partition "ios"
  → launchd → SpringBoard / backboardd (iPhone X identity, D221AP / iPhone10,6)
```

Boot arguments in use:
`-v cpus=8 sheng-smp=1 -no_corpses rd=disk0s55 unsealed_root=1 rootrw=1 sdhcirw=1 ignition_level=1 launchd_unsecure_cache=1 wdt=-1 procmon=20 procmon_tasks=0 lumiafb_stayon=1`

## Drivers added for the 950 XL

| Driver | Hardware | Notes |
| --- | --- | --- |
| `NativeGicV2/ArmCpu.patch` | QGIC2, PSCI | GICv2 SMP: per-core banked targets, serial CPU_ON over HVC (parallel starts left cores stuck), a self-IPI per core (XNU keeps `SIGPdisabled` until a core takes its first IPI), timer PPI 20 |
| `NativeSdhci` | SDCC1 eMMC | cold re-identification (the firmware's card does not answer after ExitBootServices), 8-bit HS 50 MHz, PIO writes inside GPT `ios` only, **ADMA2 reads** up to 512 KiB per command after a self-test against PIO, a console log ring at the end of the partition (NATLOG) |
| `NativeDisplay` | GOP framebuffer | IOMobileFramebuffer + IOSurface (ported from ios-sweet), canvas from the ADT, nearest-neighbour fit into 886×1920 with cleared side bars, 30 Hz vsync, stays lit on "display off" |
| `NativeTouch` | Synaptics S3708 (RMI4, I2C 0x4b on BLSP1 QUP1) + side keys | polled QUP v2 FIFO I2C at 400 kHz, F01/F12 objects, 12 ms poll; keys from PM8994 PON (power, volume down) and PM8994 GPIO 3/4 (volume up, camera) |
| `NativeHID` | — | IOHIDEventService for backboardd (IOHIDFamily-1915 sources + a port of ios-sweet's SweetHID): touch digitizer, consumer keys, tap-to-wake |
| `NativeCpuClock` | APCS PLLs, PM8994 rails | reads PLLs/muxes/rails/fuses, times each core; moves the A57 cluster to its idle second PLL at 1267.2 MHz ÷ 2 |
| `NativeBattery` | PMI8994 FG + SMBCHG | IOPMPowerSource for powerd (level, charging, external power) |
| `NativeAES16`, `NativeWatchdog`, NativeKeyStore stand-ins | — | AppleImage4 / watchdogd / keybagd stand-ins |

## Numbers

| | Value |
| --- | --- |
| CPU clocks left by the firmware | A53 768 MHz, A57 384 MHz (both cluster PLL0 at 1536 MHz, ÷2 / ÷4) |
| CPU clocks now | A53 768 MHz, A57 633.6 MHz (measured by timed dependent adds per core) |
| Rails | A53 (S8) 1000 mV, A57 (S9-S11) 900 mV, MX (S2) 1035 mV |
| eMMC reads | PIO ~10 MB/s → ADMA2 64 KiB ~27 MB/s → ADMA2 512 KiB ~29 MB/s |
| Time to launchd | ~350 s → ~180 s (the 4K code-signature pass reads the 2.7 GB shared cache) |

Boot timings (kernel console, `procmon` ticks every 20 s; "launchd" = first tick with userland tasks):

| Boot | Storage | Shared-cache pass: reading / hashing | launchd running by |
| --- | --- | --- | --- |
| 17 (2026-10-31) | PIO | 278 s / 23 s | ~369 s |
| 19 (2026-10-31) | ADMA2, 64 KiB per command | 100 s / 23 s | ~189 s |
| 20 (2026-11-01) | ADMA2, 512 KiB per command | 95 s / 23 s | ~189 s |

Bigger commands barely helped: the bus (8-bit, 50 MHz, single data rate) is now the limit. DDR52 or
HS200 would be next, and overlapping the hashing with the reads would hide another ~20 s.
| Rendering | CoreAnimation's software renderer (`CA::OGL::SW`); backboardd uses up to ~2.4 cores while animating |

## Known issues and limits

- **No GPU.** iOS 16 has no CPU path in CoreImage (only Metal and OpenGL ES contexts); with no Metal device
  it creates an OpenGL ES context whose function table is empty and calls address 0. Blur views are grey;
  anything that needs CoreImage crashes. Seen so far: new app icons (IconServices), and **Settings >
  Accessibility > Bold Text**, which makes CoreUI restyle glyph images through `CUIShapeEffectStack`'s
  CIContext and crashes SpringBoard (it respawns). A Mesa/Freedreno OpenGL ES (as HackiOS did on Adreno)
  is the route.
- **2× rendering** (fewer pixels) works for SpringBoard but icons are cached only at 3× and new ones need
  CoreImage, so Settings and apps crash; 375×812 points at 2× also has no status-bar layout. Back to 3×.
- **A57 at 960 MHz** (1025 mV) ran ~6 minutes, then the SoC reset itself (PS_HOLD): above the second CPR
  fuse corner Linux also changes the memory-assist (mem-acc) setting through the secure monitor, which this
  port does not. The clock stays inside the fuse corner the firmware already set up.
- No RTC (the clock starts at 1970), no Wi-Fi, cellular, Bluetooth, audio, camera, sensors, SEP.
- Crash-looping background services: Siri TTS, installcoordinationd (`ENOTSUP` on the data directory),
  some widget extensions. `-no_corpses` keeps ReportCrash from burning CPU on them.
- First boot after writing the root takes longer (LaunchServices builds its database).
