# UniXNU-lumia950

Device repository for the Microsoft Lumia 950 (MSM8992, Snapdragon 808), targeting the
UniXNU stack: the phone's own AArch64 UEFI, the UniXNU boot manager, DarwinBoot and a
patched XNU running an iOS userland.

This is a **P1 research skeleton**. `board.toml` keeps `example = true`, so the build driver
refuses to build or boot-image it — that is the intended guard, not an oversight. What is
here is the part that could be built and checked without the phone: the device repository
structure, the register map with its sources, the eMMC driver's host-testable core, and now
the chainload payload with its build and configuration checks.

## The boot route

The phone boots through its **own** UEFI, not through a firmware this repository builds.
Lumia950XLPkg already brings the MSM8992 up far enough to show a menu and to start EFI
applications from FAT on EFIESP — that is how Windows boots on these phones. The payload is
therefore plain files on that partition, launched from that menu:

| Path on EFIESP | File | Built by |
| --- | --- | --- |
| `\EFI\UniXNU\UniXNU.efi` | the menu | `package/build.sh` in the UniXNU checkout |
| `\EFI\UniXNU\unixnu.toml` | its configuration | [payload/EFI/UniXNU/unixnu.toml](payload/EFI/UniXNU/unixnu.toml) |
| `\EFI\UniXNU\Drivers\DarwinBoot.efi` | the loader, named in the config by the kernel stage | `DarwinBootPkg/DarwinBoot.dsc` |
| `\EFI\UniXNU\Payloads\kernel.macho` | the kernel, from the kernel job's artifact | `scripts/stage-kernel.py` |
| `\EFI\UniXNU\Payloads\kernel.macho.adt` | the device tree DarwinBoot hands the kernel | `scripts/make-lumia950-adt.py`, through the same script |

Mu-Silicium is not the base: it has no MSM8992 target, and its installation model is an
Android boot image that an Android bootloader loads with `fastboot`, which a Lumia does not
have. Chainloading also keeps the experiment reversible — nothing is flashed.

Two firmware services decide whether the menu can work. Both were measured on the 950 on
2026-10-23:

| Service | It decides | Observed |
| --- | --- | --- |
| `EFI_GRAPHICS_OUTPUT_PROTOCOL` | whether the menu draws | present: the logo and the three cards drew on the 1440x2560 panel |
| `EFI_SIMPLE_TEXT_IN_PROTOCOL` | whether keys reach the menu | present: the volume keys move the selection |

UniXNU's menu reads `ConIn`; touch arrives as an absolute pointer, which the menu does not
consume. That the firmware publishes the buttons as a text input device is measured now,
not assumed.

### Measure the firmware before staging a kernel

`tools/uefi-probe/` contains a standalone ARM64 EFI diagnostic for the installed firmware.
It reports the current exception level, GOP geometry and framebuffer address, memory-map
summary, and available firmware protocols without writing disks, changing firmware variables,
or calling `ExitBootServices`. See [its build and launch instructions](tools/uefi-probe/README.md).
This is a diagnostic, not a kernel boot or a working display driver.

The on-device v1 report photographed on 2026-10-26 confirms EL1, a Cortex-A53 at boot
affinity 0, the 19.2 MHz counter, and a **1080x1920 BGR GOP framebuffer at 0x00400000**.
The earlier 1440x2560 statements describe native panel geometry, not this installed
firmware's measured GOP mode. The report is recorded in
[docs/hardware/2026-10-26-probe-v1.json](docs/hardware/2026-10-26-probe-v1.json).
The corrected CPU affinities match the firmware's MSM8992 MADT: `0,1,2,3,0x100,0x101`,
with four A53s and two A57s. DRAM has a zero physical base in the firmware map, but its
carve-outs and gaps remain reserved; DarwinBoot must allocate a real conventional-memory
window rather than claiming the advertised 3 GiB as a contiguous usable range.

### Stage two: the kernel

The menu stage proves the firmware services; the kernel stage adds the loader and a kernel to
the same tree. It is a separate command so the menu keeps working when the kernel does not:

```sh
bash scripts/build-payload.sh                     # the menu, in out/payload
python3 scripts/get-ntfs-driver.py                # out/ntfs_aa64.efi, reads MainOS
python3 scripts/stage-kernel.py --kernel kernel.macho --unique-chip-id HEX \
    --menu-tree out/payload --ntfs-driver out/ntfs_aa64.efi \
    --out build/boot --payload-out build/mainos   # 280 KB for EFIESP, 56 MB for MainOS
```

`--kernel` takes the prelinked image from the `lumia950-kernel` artifact
(`out/lumia950/kernel/kernel.macho`), not the `mach.development.bcm2837` beside it: the merged
image is the one with the kernelcache's kexts in it.

`stage-kernel.py` does four things, and checks each before anything is copied to the phone: it
requires a 64-bit ARM `MH_EXECUTE` kernel (what DarwinBoot parses), runs
`scripts/make-lumia950-adt.py` with the values in `[payload]` and `[payload.adt]`, writes the
`.adt` sidecar DarwinBoot reads as `<kernel>.adt`, and inserts the `[[driver]]` and `xnu` entry
into the staged copy of `unixnu.toml` — the same file, so the panel layout and the action
entries cannot drift between the two stages. With `--ntfs-driver` it stages a fifth thing, the
driver that makes MainOS a readable volume. Then `check-adt.py` and `check-payload.py` re-read
what it wrote; the second also refuses a tree that names a kernel without its `.adt` sidecar,
a failure that would otherwise be `EFI_NOT_FOUND` on the panel, and it accepts the split tree
only when every name the configuration uses exists in one of the two halves.

One assumption is settled by the first flash, not by anything in this repository: DarwinBoot
starts the kernel only when the firmware runs at **EL1** (`DarwinCurrentEl () == 4`). If the
Lumia's UEFI hands control over at EL2, the loader prints `unsupported inputs or exception
level; firmware retained` on the panel and returns to the menu — a visible, harmless failure
that names its own cause. Nothing here is built around it until the phone says which it is.

**Where the kernel lives.** EFIESP is 32 MB and the prelinked image is 56 MB, so the kernel
cannot go on EFIESP: it goes on **MainOS**, the phone's NTFS OS volume. `--payload-out` splits
the staged tree for that — the menu, its configuration and the drivers stay in `--out` for
EFIESP (280 KB), and `Payloads/` with the kernel and its sidecars is written to
`--payload-out` for MainOS. Two things make the split work, and both are staged:

- The Lumia's UEFI image carries EDK II's FAT driver and no NTFS driver at all, so MainOS is
  not a volume until one is started. `--ntfs-driver` stages EfiFs's read-only NTFS driver
  (`scripts/get-ntfs-driver.py` fetches v1.13 and refuses anything but its pinned hash) as
  `\EFI\UniXNU\Drivers\ntfs.efi`, first in the `[[driver]]` list; UniXNU starts it, and
  `connect_all` binds it to the partition.
- `DarwinBoot` reads the kernel from the configuration's own volume first and then from every
  other volume with a file system, and takes `.adt`, `.trustcache` and `.gpuaddon` from
  whichever volume the kernel came from. `ci/darwinboot-payload-volume.patch` carries that
  change, which `scripts/build-payload.sh` applies before it builds the loader; the panel
  prints `DARWINBOOT: payload from another volume (n of m)` when the scan is what found it.

Without the split the whole tree has to fit EFIESP, which is only true of the native 14.8 MB
kernel with no kexts — not of the image an iOS 16 boot needs.

### Testing it on the phone

Nothing in the Lumia firmware runs an application from `\EFI\UniXNU` by name. It boots the
first `\EFI\BOOT\BOOTAA64.EFI` it can load from any FAT volume it has mounted
(`Application/BootSplashApp/SplashApp.c`, `DiscoverAndBootApp`), and falls back to its own
boot manager menu only when that fails. That removable-media name is the entry point, so the
payload is staged with the menu under both names:

| On EFIESP | From the artifact |
| --- | --- |
| `\EFI\BOOT\BOOTAA64.EFI` | `EFI/BOOT/BOOTAA64.EFI` — the same `UniXNU.efi` |
| `\EFI\UniXNU\unixnu.toml` | `EFI/UniXNU/unixnu.toml` |
| `\EFI\UniXNU\Drivers\DarwinBoot.efi` | the loader, present from the menu stage on |
| `\EFI\UniXNU\Drivers\ntfs.efi` | only with `--ntfs-driver`, and only needed for a kernel on MainOS |
| `\EFI\UniXNU\Payloads\kernel.macho` and its `.adt` | not here in a split tree: on MainOS, under `\EFI\UniXNU\Payloads` |

Unlocking with WPInternals and installing the Lumia950XLPkg UEFI (`UEFI.elf` in the EFIESP
root, the Boot Shim application, a BCD entry for it) come first and are not in this
repository. That is the one step in the whole plan that can brick the phone.

How that entry is selected matters on these stores: the phone's boot manager has
`displaybootmenu No`, `timeout 0` and a persistent `bootsequence`, so adding the entry to
`displayorder` does nothing. Pointing `{bootmgr} bootsequence` at the BootShim entry is
what actually boots it, and pointing it back at the mobile-startup GUID
(`{01de5a27-8705-40db-bad6-96fa5187d4a6}`) is the revert.

1. Mount EFIESP in Mass Storage Mode and back up `\EFI\BOOT`. Windows Phone's own loader is
   ARM32 (`BOOTARM.EFI`), so `BOOTAA64.EFI` is usually free — but if one is already there it
   is somebody else's fallback loader, so rename it rather than overwriting it.
2. Copy `out/payload/EFI` over `\EFI` (menu stage), or `build/boot/EFI` (kernel stage,
   which is the same tree plus the driver entries). Nothing else on the partition changes.
3. For a split tree, mount MainOS and copy `build/mainos/EFI/UniXNU/Payloads` to
   `\EFI\UniXNU\Payloads` there — 56 MB, so check the free space first. MainOS is the volume
   Windows Phone itself boots from; the copy adds a directory and changes nothing else, and
   `\EFI\UniXNU\Payloads` there can be deleted to undo it.
4. Reboot and leave the splash alone. Expect the UniXNU menu for 20 s; in the kernel stage
   the countdown selects `iOS 16.7.16`, in the menu stage "Exit to firmware" opens the
   firmware's own menu, the one with System Info.
5. To go back to Windows, mount EFIESP again and delete `\EFI\BOOT\BOOTAA64.EFI`. While it
   is there, that file is what the firmware starts.

What each outcome means:

| What happens | Likely cause |
| --- | --- |
| no menu, the firmware menu appears after a few seconds | the image did not load: check the copy is 95232 bytes and AArch64 |
| text console instead of cards | GOP is present but the graphics path is not; see `[ui] console` |
| `DARWINBOOT: unsupported inputs or exception level; firmware retained` | the firmware runs at EL2, not EL1, or the entry passed inputs the loader refuses (read the line: it names which) |
| `DARWINBOOT:` names a file or `EFI_COMPROMISED_DATA` | the `Payloads` tree is incomplete: the kernel, its `.adt` sidecar and the placeholders in the ADT have to match what the config hands over |
| `DARWINBOOT: payload not on any of N volume(s)` | the kernel is not on the config's volume and MainOS was not mounted: `ntfs.efi` failed to start, `connect_all` is off, or the copy went somewhere else. `N` is how many volumes the loader could read |
| `DARWINBOOT: payload from another volume (n of m)` | the split worked and the kernel was read from MainOS; the boot continues from there |
| screen stays black after the entry, phone resets a few seconds later | the kernel started and stopped; nothing can be drawn after `ExitBootServices` yet (blocker 3), so this is the expected blind rung, and the reset is the watchdog |
| menu draws, `Restart` / `Power off` do nothing | MSM8992 has no PSCI SYSTEM_RESET or SYSTEM_OFF; those entries call the firmware's `ResetSystem` |
| menu draws, `Restart` or `Power off` does nothing | the firmware's `ResetSystem` for that action is a stub |

The firmware registers a `UEFI Shell` boot option from a file GUID in its volume and maps it
to Volume Up (`Library/PlatformBootManagerLib/PlatformBm.c`). The release build has no Shell
in that volume, so the volume buttons are only a menu input, not a way into a shell.

## State

| Layer | State |
| --- | --- |
| Device repository | `board.toml`, defconfig, `NativeSdhci`, build and test scripts; accepted by `unixnu status` as `[EXAMPLE]` |
| UEFI firmware | not built here; the phone's own Lumia950XLPkg was observed starting `\EFI\BOOT\BOOTAA64.EFI` |
| Chainload payload | built by the `payload` job ([run 39869583766](https://github.com/ManGuy-Legend/UniXNU-lumia950/actions/runs/39869583766)), artifact `lumia950-payload`; loaded on the phone on 2026-10-23 through the BCD `bootsequence` entry |
| UniXNU menu | drawn on the 1440x2560 panel in the portrait card layout, three action entries, volume keys move the selection; touch is not consumed |
| DarwinBoot | `DarwinBoot.efi` builds standalone from its DSC and `scripts/stage-kernel.py` names it in the staged config; not yet loaded |
| ADT | generated and checked on the host (11976 B, six CPUs, NVRAM proxy); not yet handed to a kernel |
| XNU kernel | the updated native drivers compile and link on 16.x in [run 40394514004](https://github.com/TwisstedToast/UniXNU-lumia950/actions/runs/40394514004), source `36b5288`; merged with the 16.7.16 kexts, not loaded |
| Prelinked kernel | merged by the kernel job: 9 components, 12,882 rebindings, a sparse prelink index; the tool marks it unqualified and it has not run. The kernel stage takes that image without changing it |
| XNU entry | the on-device EFI probe confirms EL1; the handoff photograph confirms CPU framebuffer writes after ExitBootServices with firmware mappings retained, not XNU entry or GPU acceleration |
| Storage driver | shared command/PIO engine, 368 simulated transport checks, updated XNU wrapper ABI-compiled and linked in run 40379567698; not run on hardware |
| Native GICv2 | hash-qualified ArmCpu integration ABI-compiled and linked; asynchronous timer/SGI/SPI tests pass in standalone QEMU with security extensions off/on; no XNU boot or handset IRQ result |
| Display (`NativeMdss`) | rung 1 of 6: maps the MDP, MMSS, DSI0 and PHY windows, proves `gdsc_mdss` is on, then reads the MDSS/MDP revisions, the DSI controller's version and enable bits and the firmware's raw video-timing words. Publishes no framebuffer, never run on hardware |
| GPU (`NativeAdreno418`) | rung 1 of 6 runs: maps the KGSL and MMSS windows, refuses to read a collapsed domain, reads the revision. Rungs 2–4 and the fence half of 5 exist as host-tested cores (`gdscrEnableValue`/`gdscrPollUntilOn`, `IommuCore`, `CommandStream`) and are not wired; rung 6 has a host checker for the addon blob but no plugin built for iOS 16. Built by the archive step, publishes no GPU service, never run on hardware |
| Native RPM/SMD handoff | user reports `NATIVE RPM CHANNEL OK` from the independently installed v3 EFI image: post-firmware claim, native open/close and retirement passed; one polling run, no packet/ACK, resource vote or GPU power test. See [the handset record](docs/hardware/2026-10-26-rpm-handoff-v3.json) |
| Integrated native GPU command batch | host-tested native channel → leased power votes → A418 setup → both SMMU contexts → initialization and two fenced CP memory writes → verified teardown. Not activated on handset; board/baseline/security inputs remain unqualified. Not rendering or 60 FPS. See [the batch contract](docs/native-gpu-batch.md) |
| iOS visual effects | none, and blocked twice over: no display driver (blocker 3) and no Metal device (blocker 7). See "iOS visual effects" |
| Root volume and userland | not started |

## What is verified, and how

| Check | Command | Result |
| --- | --- | --- |
| SDHCI decoding and transport | `bash scripts/test-storage.sh` | 47 decoding checks and 368 simulated MMIO checks pass under ASan/UBSan |
| GICv2 core | `bash scripts/test-gicv2.sh` | 168 simulated MMIO checks pass under ASan/UBSan |
| GICv2 asynchronous exceptions | `bash scripts/build-gicv2-test.sh`, then `python3 scripts/test-gicv2-qemu.py build/gicv2-qemu/gicv2-test.elf` | two rounds each of timer PPI 27, SGI 0 and SPI 40; security extensions off/on; not XNU |
| Adreno 418 core logic (184 checks) | same runner: the second block compiles and runs `check-adreno-core` | all pass |
| Display core logic (49 checks) | same runner: the third block compiles and runs `check-mdss-core` | all pass |
| GPU addon blob (17 self-test cases) | `python3 scripts/check-gpu-addon.py --self-test` | a coherent blob accepted; a bad signature, a mis-sized header, missing padding, a climbing path, a non-executable probe, an executable plist, a bundle without `Info.plist`, an iOS 17 minimum, a macOS build, a non-Mach-O binary, an entitlements plist under `daemons/`, a bad publish file, a malformed client name and a relative primelist entry all refused |
| Board and defconfig parse | `unixnu defconfig lumia950_defconfig -D lumia950` | accepted |
| Module resolution | `unixnu status -D lumia950` | `driver NativeSdhci device`, `driver NativeAdreno418 device` |
| Incomplete-board guard | `unixnu build kernel --dry-run -D lumia950` | refused, as designed |
| CI workflow file | YAML parse, `bash -n` on all 17 run blocks | clean |
| Chainload payload configuration | `python3 scripts/check-payload.py` | accepted; the keys and enums are the ones `UniXNULib/Config.c` reads |
| Payload staging | `stage-esp.py` and `check-payload.py --payload-dir` on fixture EFI files | tree staged at `EFI/UniXNU` and accepted |
| Chainload payload build | `payload` job, [run 39869583766](https://github.com/ManGuy-Legend/UniXNU-lumia950/actions/runs/39869583766) | `UniXNU.efi` and `BOOTAA64.EFI` 95232 B, `DarwinBoot.efi` 26112 B, config 2119 B; staged tree checked, artifact uploaded |
| Kernel stage | `python3 scripts/stage-kernel.py --kernel <mach-o> --unique-chip-id … --menu-tree out/payload` | ADT generated (12,212 B, six CPUs) and accepted by `check-adt.py`; config re-read with 4 entries, the kernel first and `default = "xnu"`; a non-`MH_EXECUTE` kernel and a tree missing the `.adt` sidecar are both refused |
| On-phone chainload | mass-storage copy of `out/payload/EFI`, BCD `bootsequence` pointed at the `\EFI\boot\BootShim.efi` entry | Lumia splash then the UniXNU menu with three cards; volume keys move the selection (2026-10-23) |
| ADT generator and checker | `python3 scripts/make-lumia950-adt.py --dram-size 0xC0000000 --unique-chip-id ... --chip-id 0x8015` then `python3 scripts/check-adt.py build/lumia950.adt` | 12576-byte tree with 6 CPUs, the GPU node and the display node accepted; the checker's `--self-test` rejects 9 deliberate defects, including a broken NVRAM Adler-32 and an MMSS window too short for `gdsc_mdss` |
| Kernel-ABI compile of `NativeSdhci.cpp` | `kernel` job, [run 39858813829](https://github.com/ManGuy-Legend/UniXNU-lumia950/actions/runs/39858813829) | compiled clean under `-Werror`; archive holds two ARM64 `MH_OBJECT` members |
| XNU kernel link | same run | links with the board archives |
| Kernelcache merge | `kernel` job, [run 39973639606](https://github.com/ManGuy-Legend/UniXNU-lumia950/actions/runs/39973639606) | 9 components, 12,882 rebindings, 56,127,488-byte arm64 `MH_EXECUTE` (sha256 `5ccf511d…`), sparse index of 9 headers; the tool marks it unqualified |

The historical driver's *kernel* half (`NativeSdhci.cpp`) compiled against the recorded kernel ABI,
and the archive holds `NativeSdhci.o` and `SdhciCore.o`, both ARM64 `MH_OBJECT`; the
archive is not byte-reproducible, because `ar` records member timestamps, so it is not
quoted as an identity.

The updated storage wrapper, ArmCpu GICv2 integration and GPU/RPM cores now compile with
the recorded XNU/iPhoneOS SDK recipe and link in
[run 40379567698](https://github.com/TwisstedToast/UniXNU-lumia950/actions/runs/40379567698),
source `c76c869`. This is separate from the host and standalone IRQ tests. The resulting
prelinked candidate remains marked unqualified; no XNU boot or handset IRQ result is claimed.

The kernel links too. Upstream calls `native_legacy_display_probe()` from `bsd_init.c`
without the `XNU_NATIVE_LEGACY_DISPLAY` guard every other call site carries, so the symbol
is referenced even with the display provider unselected. `drivers/NativeLegacyDisplay`
defines both probe entry points and does nothing else — no framebuffer, no provider — and
the defconfig force-loads it through the kernel script's `XNU_EXTRA_LD_ARCHIVES` hook.

The merge is part of the build. `[kernel.prelink]` names the unstripped 16.x image, pins
the arm64 kernelcache of 16.7.16 (50,937,856 B out of `kernelcache.release.iphone10b`,
sha256 `2ec297ba…`) and lists the components this port takes from the cache instead of from
source: APFS, its decmpfs provider, the storage stack and corecrypto. The slide is measured,
not guessed: the native `__PRELINK_TEXT` is at `0xfffffff004004000` and the cache's at
`0xfffffff004dc8000`, so `-0xdc4000` maps the four coalesced ranges 4K aligned to the base of
the 48 MiB kext window, clear of every native segment and below `__TEXT` at
`0xfffffff007004000`.

The closure also carries what those declare as libraries — AppleEffaceableStorage,
CoreAnalyticsFamily, IOCryptoAcceleratorFamily, AppleImage4, CoreTrust — which are
Apple-SoC-adjacent and have no node in the ADT; they stay unstarted. `com.apple.kec.Libm` is
deliberately out: nothing selected depends on it, and its 16.7.16 `__text` embeds a constant
table the prelink's instruction inventory does not recognise and refuses to copy.
`early_security_init` is out too until an AMFI initializer address for this cache is
qualified; an unqualified address is worse than none.

The kernel is on 16.x now. `feeds.toml` pins `xnu-8796.141.3` — a source of the same name
replaces the base repository's 17.x entry — and `patches/xnu-8796/` is that line's overlay.
Of the 54 base patches, 42 apply unchanged and five are replaced by their 8796 form (the
non-Apple tunables guards, the external archive hook, the machine layer, the native AES
personalities and the 64-bit physical address shift); nine overlay-only patches are added. Six answer this line's
sources, the current clang or the runner's `iig`: `clang-trivial-auto-var-init`, because 16.x
passes a clang flag the runner's clang no longer knows; `clang-uio-stackbuf`, which declares
the uio stack buffer the way later XNU lines do, because current clang rejects an array of a
one-byte type carrying an eight-byte alignment, which is what every `bsd/` call site used to
say; `driverkit-rpc-kernel-content`, which keeps the kernel `IORPCMessage` in the rpc because
the runner's `iig` generates DriverKit dispatch code that reads it instead of deriving the
message from the mach header; `restore-arm64-kpc`, which restores `osfmk/arm64/kpc.c` from
`xnu-10063.141.1` because this line's file list builds it and its source drop does not publish
it — on a non-Apple SoC the file takes its own not-implemented branch; `compile-records`,
which makes the compile rules write the per-object JSON records the archive builders read to
reuse this kernel's exact flags, which this line does not write but the 17.x line's tooling
expects; and `pmap-pending-preemption`, which defines the debug pmap's preemption question in
the arm pmap this tree has, because its arm64 home is not published. Three answer the merge:
`native-prelink-sparse-index` and `native-prelink-security-init` are NativePrelink's two ABI
patches, which the kit publishes only for 10063 and which add the sparse-index ABI the tool
checks for and the early-initializer marker it uses when one is configured; and
`vfs-getvfs-with-vfsops` adds the one symbol the 16.7.16 APFS imports that the published
source lacks — the cache reports `xnu-8796.142.1.703.8`, which Apple has not published, and
the function is 10063's, unchanged. `skip.txt` names the seven this line has no mechanism
for, each with its reason: no page-guard zones, no SPTM pmap, no exclave owner chain, no
boot-progress console. The build prints every skip and refuses a skip list that names a
patch the base tree no longer has, so an omission cannot become a silent hole.

The 16.x kernel builds, links and merges. Run
[39973639606](https://github.com/ManGuy-Legend/UniXNU-lumia950/actions/runs/39973639606)
compiled all five components, wrote the compile records, built all six archives, linked
`mach.development.bcm2837` (14,840,992 B, sha256 `8b8c633d…`) with `NativeSdhci` and the
display stub force-loaded, then merged it with the pinned kernelcache into a
56,127,488-byte ARM64 `MH_EXECUTE` with 25 load commands and a 9-entry sparse index
(sha256 `5ccf511d93986ddff5f7d74880b096f980081b7d5ba3fb10d2dc15325cb3cb58`): 9
components, 7 kernel interfaces, 12,882 rebindings. Both images and the report ship in the
`lumia950-kernel` artifact; the job parses the candidate and its report rather than trusting
the exit status.

Seven stops stood in front of that, one per addition above: the annotated tag pinned as a
commit, the clang flag this line passes and the runner's clang rejects, the aligned-char uio
stack buffer, the `iig`-generated DriverKit dispatch, the omitted `osfmk/arm64/kpc.c`, the
missing compile records and the unpublished debug pmap hook. A failed run hands over
`out/<device>/logs/`: the step uploads it and greps for `error:` because the driver echoes
only the last thirty lines of a command.

## The reference image

`[ipsw]` pins iPhone X, iOS 16.7.16 (build 20H392), device class d221ap.

| Question | Answer |
| --- | --- |
| Why this release | iOS 16 is the requirement, and 16.7.16 is the last 16.x release and the one the community ports reached userland with |
| Why iPhone10,6 | the phone-shaped identity in a restore that serves iPhone10,3 and iPhone10,6; both use `kernelcache.release.iphone10b` |
| How it was checked | product, version, build, device class, chip `0x8015` and board `0x0e` were read from the image's own `BuildManifest.plist` over HTTP range requests |
| What is pinned | the arm64 slice of the `KernelCache` IM4P after LZFSE decoding: 50,937,856 bytes, sha256 `2ec297bac95bcba01046fd0148e97e7e5885c05a6c18827f39f534ba375c1221`; `[kernel.prelink]` refuses any other cache |

## Layout

```
lumia950/
  board.toml                        board description, incomplete by design
  configs/lumia950_defconfig        kernel configuration
  drivers/NativeLegacyDisplay/      probe-symbol archive; not a driver, and not a display
  drivers/NativeSdhci/              the eMMC driver
    BoardConstants.h                Lumia 950 addresses, each with its source
    SdhciRegs.h                     SDHCI register map (spec layout, not guessed)
    SdhciCore.{h,cpp}               clock, CSD, EXT_CSD and ADMA2 arithmetic
    NativeSdhci.{h,cpp}             IOKit driver: warm handoff, PIO reads
    Info.plist                      personality for `qcom,msm8994-sdhci`
    Test/check-sdhci-core.cpp       host test for the core
  drivers/NativeAdreno418/          the GPU, bring-up rung 1
    BoardConstants.h                KGSL, MMSS, SMMU and a4xx registers, each with its source
    AdrenoCore.{h,cpp}              chipid and GDSCR decoding, the GDSC sequence, CP ring
                                    arithmetic, the ladder
    IommuCore.{h,cpp}               ARMv7 short-descriptor page tables for the GPU's SMMU
    CommandStream.{h,cpp}           PM4 packets, ring pacing, the fence and its wait,
                                    the CP microcode loader plan
  drivers/NativeMdss/               the display, bring-up rung 1
    BoardConstants.h                MDP, DSI0, PHY, MMSS and gdsc_mdss, each with its source
    MdssCore.{h,cpp}                power-domain decode, DSI link arithmetic, DSI mode bits
    NativeMdss.{h,cpp}              IOKit driver: firmware handover, no framebuffer published
    Info.plist                      personality for `qcom,mdss_mdp`
    Test/check-mdss-core.cpp        host test for the core
    NativeAdreno418.{h,cpp}         IOKit driver: power gate, revision read, no service published
    Info.plist                      personality for `qcom,kgsl-3d0`
    Test/check-adreno-core.cpp      host test for the core
  payload/EFI/UniXNU/unixnu.toml    chainload config, in the layout it deploys as
  scripts/build-arm-driver.py       compile a driver against the recorded XNU ABI
  scripts/build-payload.sh          EDK II build and staging for the two EFI binaries
  scripts/check-payload.py          config and staged-tree check
  scripts/check-gpu-addon.py        the rung-6 addon blob: layout, bundle, iOS target
  scripts/check-screenshot.py       a screenshot is this panel or it is not evidence
  scripts/panel.py                  the panel size, shared by the two above
  scripts/test-core.bat             host test runner (Windows)
```

## The eMMC map

Everything below is from the mainline MSM8992 device tree (`msm8992.dtsi` is `msm8994.dtsi`
with two A57 cores deleted), node `sdhc1`, alias `mmc1`:

| Item | Value |
| --- | --- |
| compatible | `"qcom,msm8994-sdhci", "qcom,sdhci-msm-v4"` |
| SDHCI window (`hc`) | `0xf9824900`, length `0x1a0` |
| QSDCC window (`core`) | `0xf9824000`, length `0x800` |
| Interrupts | `GIC_SPI 123` (hc_irq), `GIC_SPI 138` (pwr_irq) |
| Clock source | XO `19.2 MHz`; `GCC_SDCC1_AHB_CLK`, `GCC_SDCC1_APPS_CLK` |
| Bus | 8-bit, non-removable |
| Interrupt controller | `qcom,msm-qgic2` at `0xf9000000` + `0xf9002000` — GICv2, no redistributors |
| Supporting windows | GCC `0xfc400000`, MMCC `0xfd8c0000`, TLMM `0xfd510000`, APCS `0xf900d000`, RPM msg RAM `0xfc428000` |
| microSD, for comparison | `0xf98a4900` / `0xf98a4000`, SPI 125 / 221, card detect GPIO 100 |

These are the SoC's addresses for the controller. They are not proof of what the Lumia
firmware programmed. Three ways to prove them, cheapest first:

1. **The open firmware is the oracle.** Lumia950XLPkg contains the SDCC DXE driver that
   actually brings this controller up on this board, in C, for this exact phone. Add
   register-level logging to it and photograph the framebuffer, or dump the same values
   from the driver here and compare.
2. **The ACPI tables.** Lumia950XLPkg copies its tables from the stock Windows Phone FFU,
   so the DSDT carries Qualcomm device entries with `_CRS` memory ranges and `_GSI`
   interrupt numbers.
3. **A UEFI shell.** Enumerate `EFI_BLOCK_IO_PROTOCOL`, print capacity and logical block
   size, read LBA 0 and LBA 1. That is the block map, with no new code.

### Vendor registers

The QSDCC `core` window holds the DLL and power-control registers required for HS200 and
HS400. Their offsets are **not** in this repository: they are absent rather than guessed,
because a wrong offset in a clock or DLL register is indistinguishable from dead hardware.
Collect them from `drivers/mmc/host/sdhci-msm.c` in mainline and confirm them against the
firmware's own writes before adding them.

## Driver design: a warm handoff

The firmware leaves the controller clocked, powered and the card selected. The driver
therefore does not run CMD0/CMD1/CMD2/CMD3, does not touch the clock or power registers, and
does not reset the controller — a full reset would discard the only state that makes the
handoff work. It resets the command and data state machines, then finds out which relative
card address the firmware used by trying CMD13 at RCA 1 and 2. RCA zero is not a card address.

The command/data path now uses the shared `PollingHost` core: explicit-width ordered
MMIO, bounded reset/command/buffer/completion waits, controller and R1 error checks,
correct SDHCI R2 normalization, and deselect/read-CSD/reselect sequencing. The warm path
requires a sector-addressed eMMC user partition with 512-byte sectors; it rejects a boot
partition, RPMB, command-queue mode, and unsupported sector sizes without switching them.
A failed transfer invalidates the medium and never reports partial FIFO data as a
successful block. See [storage bring-up](docs/storage-bringup.md) for evidence and limits.

Bring-up rungs, in order, each with a distinct failure signature:

| Rung | What it proves | State |
| --- | --- | --- |
| Register read at `0xf9824900` | the window is mapped and the controller is alive | written |
| CMD13 at the firmware's RCA | the command path works | written |
| CMD9 → CSD, CMD8 → EXT_CSD | capacity decoding | written, host-tested |
| CMD17 PIO read of LBA 1, `"EFI PART"` | end-to-end data path on real silicon | written |
| ADMA2 transfers | throughput; needs the descriptor registers wired | `buildAdma2` written and tested, unwired |
| Interrupt-driven completion | hc_irq; gated on ArmCpu's GICv2 path | not written |
| CMD24/CMD25 + cache flush | a writable medium | not written |
| HS200/HS400 | usable speed | not written |

Until the write path exists the driver reports the medium as write-protected and refuses
writes loudly, rather than accepting and dropping them.

### Linking it into the kernel

The kernel build needs three things this repository cannot provide on its own:

1. The archive, from `scripts/build-arm-driver.py NativeSdhci` (listed in
   `[kernel] archives`).
2. `XNU_NATIVE_SDHCI=1`, which `CONFIG_XNU_NATIVE_SDHCI=y` in the defconfig produces.
3. A personality in `iokit/KernelConfigTables.cpp`, applied as a patch:

```c
#if defined(XNU_NATIVE_SDHCI) && XNU_NATIVE_SDHCI
    ",{"
    " 'IOClass' = NativeSdhci;"
    " 'IOProviderClass' = IOPlatformDevice;"
    " 'IONameMatch' = 'qcom,msm8994-sdhci';"
    " 'IOProbeScore' = 1000:32;"
    "}"
#endif
```

and the archive itself through `-Wl,-force_load` so the linker keeps the class.

## The ADT node

[scripts/make-lumia950-adt.py](scripts/make-lumia950-adt.py) emits the tree and
[scripts/check-adt.py](scripts/check-adt.py) parses it back before DarwinBoot ever sees it.
The driver matches an `/arm-io` child:

```
/arm-io/sdhci@f9824900 {
    name = "sdhci@f9824900";
    compatible = "qcom,msm8994-sdhci", "qcom,sdhci-msm-v4";
    reg = <0xf9824900 0x1a0 0xf9824000 0x800>;   # two windows, 64-bit values
    interrupts = <0 123 4 0 138 4>;
    clock-frequency = <19200000>;
};
```

The cell layout is the one in [scripts/qemu-entry/make-dt.py](../unixnu/scripts/qemu-entry/make-dt.py):
two address cells and two size cells, every address and size a 64-bit little-endian value.
That fixture is the only ADT this project has watched a real `ArmDTPlatform::configure`
accept, so it is the layout to follow. The earlier four-cell `reg` sketch in this file assumed
one-cell addressing and is superseded. The three-cell interrupt encoding is still Apple's,
confirmed only structurally: no driver here has consumed one yet.

The generator emits `/chosen` (random seed, CHRP NVRAM proxy, DRAM base and size,
`osenvironment`, and the `unique-chip-id`/`chip-id` pair), `/cpus` (six cores, boot CPU
first), `/psci`, `/defaults`, `/options`, `/product`, and `/arm-io` with the GICv2, SDHCI and
KGSL windows. The NVRAM proxy follows the layout
`iokit/Kernel/IONVRAMCHRPHandler.cpp` parses, down to the Adler-32 it panics on, and the
checker recomputes that checksum rather than trusting the generator. `unique-chip-id` and
`chip-id` are required because anything calling `gethostuuid()` blocks forever without them;
the checker refuses a tree that omits them unless explicitly told to allow it.

What the tree does not solve: the compiled ArmCpu GICv2 integration still needs native-kernel
runtime and handset validation (blocker 2), PSCI is the MSM8992 subset without SYSTEM_OFF/SYSTEM_RESET
(blocker 4), and the MDSS node currently matches only a register-identity driver.

## The GPU (Adreno 418)

MSM8992's GPU is an **Adreno 418**, an a4xx part, and the generation decides everything here.
The new GPU backend, pinned firmware, tests, and remaining path to a real accelerated iOS UI
are documented in [GPU bring-up](docs/gpu-bringup.md). The default driver remains read-only:
it checks both power domains and all three clock branches before accessing GPU registers.

The active route is [firmware-native bring-up](docs/native-bringup.md): a post-EBS
Adreno hardware-activation entry and a separate source-only XNU early-boot entry.
Neither requires Windows. Neither is yet a GPU-rendered frame or a booted iOS UI.

Upstream's [Adreno 740 driver](../unixnu/docs/drivers/native-adreno.md) drives a6xx/a7xx
silicon: a GMU that boots firmware and owns power, a shared 4 KiB SMMU through `ArmSmmu4K`,
and a command stream from `AdrenoPM4` whose state packets are the ones A6xx and later take.
a4xx has no GMU, its own IOMMU and a different command processor, so this driver is its own
thing rather than a configuration of that one — which is why the class and the personality are
`NativeAdreno418` and not a variant of `NativeAdreno`.

| Item | Value | Source |
| --- | --- | --- |
| KGSL window | `0xfdb00000`, `0x40000` | Qualcomm `msm8992-gpu.dtsi`, `reg` of `qcom,kgsl-3d0@fdb00000` |
| Interrupt | GIC SPI 33 | same node, `interrupts = <0 33 0>` |
| Chip id | `0x04010800`, generation 4 | same node, `qcom,chipid` |
| Clocks | `clk_oxili_gfx3d_clk`, `clk_oxilicx_ahb_clk`, `clk_oxili_rbbmtimer_clk` | same node, `clocks` |
| Power levels | 600, 490, 450, 367, 300, 180, 27 MHz; boots at level 4 (300 MHz) | same node, `qcom,gpu-pwrlevels` and `qcom,initial-pwrlevel` |
| Power domains | `gdsc_oxili_gx` at `0xfd8c4024`, `gdsc_oxili_cx` at `0xfd8c4034` | `msm-gdsc.dtsi`, four bytes each in the MMSS controller |
| GPU IOMMU | `qcom,iommu@fdb10000`, `0x10000`, PMON GIC SPI 38 | `msm-iommu-v1.dtsi`, label `kgsl_iommu` |
| "domain is on" | GDSCR bit 31 | `drivers/clk/qcom/gdsc.c`, `PWR_ON_MASK` |
| Registers | dword indices `RBBM_HW_VERSION` `0x0000`, `RBBM_STATUS` `0x0191`, `CP_RB_BASE` `0x0200`, `CP_ME_CNTL` `0x022d` | freedreno `a4xx.xml`; Qualcomm's `a4xx_reg.h` uses the same indices |

Mainline Linux does not help on this one: `msm8994.dtsi` in mainline has no kgsl node at all,
so the values come from Qualcomm's msm-3.10 device tree for the same SoC as published for the
LG G4 (`LineageOS/android_kernel_lge_msm8992`, branch `lineage-15.1`). Each is repeated, with
the comment that names its source, in
[drivers/NativeAdreno418/BoardConstants.h](drivers/NativeAdreno418/BoardConstants.h).

### Why the driver refuses to read the block first

An AHB access to a powered-down Qualcomm block does not fail politely; it can hang the
interconnect, and on a phone with no serial console that looks exactly like a dead board. So
the first act is not a register read: the driver maps the MMSS window, reads both GDSCRs and
applies `PWR_ON`.

| GDSCR state | What the driver does |
| --- | --- |
| gx on, cx on, no collapse request | reads `RBBM_HW_VERSION`, `RBBM_HW_CONFIGURATION`, `RBBM_STATUS` and the CP's `RB_*`/`ME_CNTL`, logs them, stops at rung 1 |
| either domain collapsed | logs which one is down and reads nothing in the GPU window |
| a collapse request pending | treats the domain as mid-transition and reads nothing |
| `0xffffffff` back from the version register | reports that nothing backs the address the ADT gives, instead of reporting it as a revision |

That is the same shape as the eMMC driver's warm handoff: take what the firmware left, prove
it, and do not reset what cannot be rebuilt.

### The ladder

| Rung | What it proves | State |
| --- | --- | --- |
| 1 identity | the windows map, the domains are on, the revision answers | written, host-tested, and the only rung a driver runs |
| 2 power | RPM vote, GDSC state machine, OxiLi clocks and the bus at a level | standalone post-EBS RPM channel open/close is user-reported successful; packet/ACK, resource-vote ownership, the Lumia supply profile and GPU cold power remain unqualified |
| 3 smmu | 4 KiB page tables and the contexts a buffer is reached through | `LpaeCore` and register-backed `SmmuContext` are implemented and simulation-tested, but not activated by the native driver or tested on the handset |
| 4 ring | CP ring, write pointer, fences | `Submission` implements firmware download, ME initialization, ring wrap, synchronization, faults and bounded waits through hardware callbacks; no on-phone execution yet |
| 5 blit | PM4 that clears or copies, confirmed by a fence | a bounded CP memory-write fill and full data verification exist as a bring-up workload; this is not yet a blitter or shader renderer |
| 6 metal | compatible iOS graphics/shader integration | no plugin for A418/iOS 16; Turnip does not support this generation |

A written backend is not a working rung: the default native driver activates neither the
context nor submission backend until its hardware prerequisites are qualified. The distinction
is a constant (`kCoreRung`) and a test, not a sentence.

Rung 6 is where upstream's work is thickest, but not as it stands: upstream's
`build-adreno-metal-plugin.py` builds for ARM64 iOS 17 while this board's userland is
16.7.16. More fundamentally, Mesa explicitly excludes a4xx from Turnip: that Vulkan route
is unavailable, not merely untested. A compatible A4xx rendering/shader path and the iOS
Metal/IOSurface integration remain unwritten.

### The command stream (rungs 4 and 5)

The a4xx command processor reads **PM4 type-3** packets: a header whose top two bits are
`0b11`, a 14-bit count of the words that follow, and an 8-bit opcode. The numbers and the
packet shapes in [CommandStream.h](drivers/NativeAdreno418/CommandStream.h) are Linux's
`msm` driver for this exact GPU (v5.15, `a4xx_gpu.c`) over freedreno's generated
`adreno_pm4.xml.h`:

| Word | Meaning |
| --- | --- |
| `0xc0010026`, `0` | `CP_WAIT_FOR_IDLE` with its one payload word |
| `0xc0010046`, `7` | `CP_EVENT_WRITE` of `HLSQ_FLUSH` |
| `0xc0030046`, `0x80000004`, address, value | `CP_EVENT_WRITE` of `CACHE_FLUSH_TS` with `BIT(31)`, which raises the interrupt and writes `value` at `address` |

The third is the fence, and it is the only proof a submission finished. `a4xx_gpu.c` submits
the three in that order, and `writeSubmissionTail` is that order as one call: eight words,
flush then wait then fence, with the interrupt on the fence so the driver does not have to
poll. `fenceWaitPoll` is the poll for when it does: a signed comparison, because the counter
wraps — a fence asked for at `0xfffffff0` is reached by a counter that reads `0x00000004` —
and a budget of 1,000 reads, because on a phone with no console a wait that never ends is
indistinguishable from a dead board.

What is *not* here is the blit. On an a4xx a clear is not a copy engine command: freedreno
clears by drawing a rectangle with the binning and render stages programmed, so a clear means
a state block, a shader and a draw — a larger piece of work than the fence that confirms it,
and one that cannot be written honestly without the power and SMMU rungs running first. The
ladder's test asserts `kCoreRung == Ring` for that reason: rung 5's confirming half exists,
its drawing half does not.

### Rung 6: the addon, and the version that stops it

Upstream does not put the Metal driver in the kernel. It builds a plugin bundle outside it
and hands the phone one blob: `addons/NativeAdrenoMetal.bundle/*`, an optional `.publish`, a
`.clients` list, a `.primelist` of the system's `.metallib` files, `bin/adrenoprobed` and the
launchd jobs under `daemons/`, packed as
[`NGPUADD2`](scripts/check-gpu-addon.py) and mounted by the kernel from the boot payload.

This repository does not write that format — it is upstream's, and a second writer would be
a second guess at a parser the phone has. What it can do is check one, and
`scripts/check-gpu-addon.py` does: the container and its padding, the paths, the modes, the
bundle and its `Info.plist`, the sidecars, and then the two things a file listing cannot
show — that the bundle binary is thin ARM64, and that its `LC_BUILD_VERSION` (or its
`Info.plist`'s `MinimumOSVersion`) does not ask for more than iOS 16.7. The published plugin
asks for 17. That failure is silent on the phone: the loader says nothing, the render server
just never gets a device, and the screen stays as it was.

### Linking it into the kernel

Three things, the same three the eMMC driver needs:

1. The archive, from `scripts/build-arm-driver.py NativeAdreno418` (listed in
   `[kernel] archives`).
2. `CONFIG_XNU_NATIVE_ADRENO418=y` in the defconfig, which produces `XNU_NATIVE_ADRENO418`.
3. A personality in `iokit/KernelConfigTables.cpp`, applied as a patch:

```c
#if defined(XNU_NATIVE_ADRENO418) && XNU_NATIVE_ADRENO418
    ",{"
    " 'IOClass' = NativeAdreno418;"
    " 'IOProviderClass' = IOPlatformDevice;"
    " 'IONameMatch' = 'qcom,kgsl-3d0';"
    " 'IOProbeScore' = 1000:32;"
    "}"
#endif
```

and the archive through `-Wl,-force_load`. Until that personality is in, the archive is built
and never matched — which is the point of publishing nothing a client can bind.

## The display path

Blocker 3 has been the display, and for a reason: no driver means nothing scans out after
`ExitBootServices`, which means no surface for iOS to composite into. This is the first
attempt at that driver, and it stops where the sources stop.

### What is sourced, and what is not

| Value | Source |
| --- | --- |
| MDP `0xfd900000` + `0x90000`, VBIF `0xfd9b0000`, IRQ 83 | `msm8992-mdss.dtsi`, `mdss_mdp@fd900000` |
| MDP register block at `+0x1000` | same node, `qcom,mdss-mdp-reg-offset` |
| DSI0 ctrl `0xfd994000` + `0x260`, PHY `0xfd994500` + `0x280`, PHY regulator `0xfd994780`, misc `0xfd828000` | same node, `qcom,mdss_dsi@fd994000` |
| `gdsc_mdss` at `0xfd8c2304` | `msm-gdsc.dtsi` |
| PMIC rails L2, L12, L14, L28 | same DSI node, `vdda`/`vddio`/`vdd`/`vcca` |
| `DSI_VERSION` `0x1f0`, `DSI_CTRL` `0x0`, the six video-timing words, `DIS_INTF_SEL` | `dsi_host_v2.h`, `mdss_mdp_hwio.h` |

Not sourced, and therefore not written: the **DSI PHY's PLL table** (the 28 nm block's VCO,
post-dividers, timing, LDO and calibration values are a per-rate table in Qualcomm's
`mdss_dsi_phy` code) and the **panel's own init sequence** (the Lumia 950's panel is not the
LG G4's, so the G4's panel DTSI is not a source for it). A driver that invents either is a
driver that will drive a panel into an unknown state and report success.

### The ladder

| Rung | What it proves | State |
| --- | --- | --- |
| 1 identity | the windows map, `gdsc_mdss` is on, the MDSS/MDP/DSI revisions answer | written, host-tested, and the only rung a driver runs |
| 2 power | the pm8994 rails, the GDSC write, and the byte/pixel/esc clocks | the GDSC decode is written and host-tested; the rails and clocks are not written |
| 3 timings | the DSI PHY's PLL and the panel's init sequence actually program the link | the link arithmetic is written and host-tested (`pixelClockHz`, `byteClockFor`, `bitClockHz`, `clocksAgree`); the PHY table and the panel sequence are not |
| 4 scanout | MDP source → mixer → ping-pong → DSI0, and the panel keeps scanning | not written |
| 5 framebuffer | an `IOFramebuffer` over that scanout | not written |
| 6 effects | QuartzCore's filters over the scanout — blur is this rung | not written |

Rung 1 is a warm-handoff read, the same shape as the GPU's. The firmware drew the logo and
the UniXNU menu through this block, so the interesting question is what it left behind: the
MDSS and MDP revisions, `DSI_VERSION`, the three enable bits in `DSI_CTRL` (a link that is
enabled but in neither video nor command mode is power with no mode — a configuration this
driver would not program and would report as it read it), and the six video-timing words as
raw values. Those six are **logged, not decoded**: their field layout is not sourced here,
and a driver that decodes a timing it cannot cite will believe its own arithmetic over the
panel.

Rung 3's arithmetic is worth having anyway, because it is the arithmetic every later rung
needs: `htotal × vtotal × refresh` is the pixel clock, `pixel clock × bytes per pixel` is the
byte clock, `byte clock × 8 / lanes` is the lane clock, and `clocksAgree` compares a clock
the hardware reports with the one that arithmetic asks for, inside a tolerance the caller
states rather than one this repository has invented. A 1440x2560 timing with 60 lines of
blanking and 60 Hz is 232.92 MHz of pixels, 698.76 MB/s of bytes and 1.39752 Gb/s per lane
on four lanes — numbers the host test checks exactly.

### The window that was too small

Writing this driver found a real bug in the GPU driver it mirrors: the KGSL node's second
window, the MMSS clock controller, was declared as 4 KiB, while the GDSCs it exists to read
sit at `0x4024` and `0x4034`. Those reads were past the end of the mapping — and on a kernel
mapping that is not an access fault, it is whatever the next page happens to be. Both nodes
now declare 64 KiB, and both core test suites assert that every register offset is inside
the window that carries it.

### Nothing is published

`NativeMdss` does not create an `IOFramebuffer` and does not publish a service. A framebuffer
that half-drives a panel is a black screen that reads as a kernel panic to everyone who is
not looking at the log — and this board has no serial console to look at it on. The
`kWrittenRung` constant says rung 1; rung 5 is what changes that.

## iOS visual effects (blur and the rest)

Blur, vibrancy and the material behind a sheet are not something UIKit draws. They are a render
pass over the content behind the layer, done by the render server while it composites; on iOS
16 that pass is a QuartzCore/RenderBox filter, and the GPU-backed paths for it go through
Metal. "Proper effects", then, is a statement about the compositor, not about a driver that
draws a blurred rectangle.

That gives an order, and this board is at the start of it:

| What an effect needs | State here |
| --- | --- |
| a panel the kernel still scans out after `ExitBootServices` | no driver: blocker 3 |
| the render server compositing into that surface | upstream userland, not started on this board |
| a Metal device (Metal, metal2vulkan, Turnip, NativeAdreno) | rung 1 of 6, and that rung publishes nothing |
| a client that draws | SpringBoard, from the staged iOS image |

Two consequences worth stating plainly. First, no effect of any kind can appear while there is
no display driver: nothing owns the panel after the firmware hands over, so there is no surface
for anything to composite into — blur is not the missing piece, the scanout is. Second, a port
can put an iOS UI on a panel before the GPU works: upstream's tb128fu notes have QuartzCore
drawing on the CPU and still reach the home screen with touch. Which filters that software path
runs, and what a backdrop looks like when it does, is not established here. The GPU ladder is
what makes the effects the ones iOS intends; the display driver is what lets any of them be
seen.

## The interrupt controller (GICv2)

### Native UniXNU integration

`scripts/build-arm-driver.py ArmCpu` now prepares a hash-qualified source copy with
`drivers/NativeGicV2/ArmCpu.patch` and compiles `GicV2Core.cpp` into the same archive.
It recognizes the Lumia compatible list, maps the GICv2 CPU interface, routes the
virtual timer and self-IPI through MMIO, retains raw IAR tokens for EOI, and adds
GICv2 SPI configuration. CPU target bits come from hardware, not logical CPU numbers.
The existing GICv3 implementation is retained as the other branch.

Standalone QEMU tests take real asynchronous AArch64 exceptions for timer, SGI and
SPI twice, with security extensions disabled and enabled. This is not an XNU boot.
The new IOKit integration compiles against the recorded iPhoneOS/XNU ABI and links in
run 40379567698, but has not run on the Lumia. GICv2 secondary CPU startup remains
deliberately unqualified and refused.
See [native GICv2 bring-up](docs/native-gicv2.md).

### Earlier HackiOS route

The following notes describe the separate HackiOS patch and its historical results,
not validation of the native ArmCpu build.

The Lumia's controller is a GIC-400: GICv2, memory-mapped, with no redistributors. The
HackiOS kernel's driver (`osfmk/arm64/gic_v3_irq.c`) was GICv3-only — the CPU interface
through the `ICC_*_EL1` system registers and one redistributor frame per core — so a GICv2
machine had no timer, no inter-processor signal and no SPI. That is the change carried as
`ci/hackios-gic-v2.patch`:

| Part | What it does |
| --- | --- |
| Version | Reads the whole `compatible` list, not only its first name — `arm,gic-2`, `arm,gic-400` and `qcom,msm-qgic2` are a v2, `arm,gic-v3` a v3 — and checks it against `GICD_PIDR2`; the second `reg` window is the redistributor frames on a v3 and the CPU interface on a v2 |
| Per-CPU | GICv2: banked distributor registers for SGIs and the timer, `GICC_CTLR`/`PMR`/`BPR` for the interface. GICv3: redistributor frame and `ICC_*_EL1`, unchanged |
| Groups | A GICv2 with security extensions shows only Group 1 to a non-secure EL1, and only Group 0 to the secure side — which is what a machine with no EL3 is. A probe write to `GICD_IGROUPR` decides, and an interrupt marked with the other group is never delivered |
| SPIs | `GICD_IROUTER` on a v3, `GICD_ITARGETSR` on a v2; edge/level and priority the same on both |
| Firmware handover | The timers the firmware ran are stopped and their interrupts cleared before the CPU interface is enabled, because at that point of the start-up the stack an exception would land on is not the kernel's |
| Loader | A `gic_cpu` field per platform, `GIC2=1 xnuloader/build.sh` for a GICv2 machine, the Snapdragon's own list (`qcom,msm-qgic2`, `arm,gic-2`) and the GICC base in the device tree, the controller quieted just before the jump |
| QEMU | `QEMU_GIC_VERSION=2 qemu/run-ios.sh` selects `-M virt,gic-version=2` |

Verified on QEMU's `virt` machine with the same kernel and userland: with
`QEMU_GIC_VERSION=2` the kernel brings up the timer, takes the touch and display interrupts
and runs SpringBoard to the lock screen, and past it to the home screen; with
`QEMU_GIC_VERSION=3` the boot is the one this port already had. Both were run against the
images `userland/build.sh` makes and the kernel `xnu-build.sh` links.

The GICv2 machine's node is written the way the phone's ADT writes it, `qcom,msm-qgic2`
first and `arm,gic-2` after it, so that boot is also the test of the list path: the driver
took the v2 out of a list whose first name it does not look for (the earlier driver
compared only the first string, and would have called that node neither one). The kernel's
own line for it is `gic: compatible "qcom,msm-qgic2", PIDR2 revision 2, second region
10000/2000`; with the loader built for GICv3 the line says `arm,gic-v3` and the boot is
unchanged.

```sh
GIC2=1 xnuloader/build.sh           # the loader for a GICv2 machine (the default build is GICv3)
QEMU_GIC_VERSION=2 qemu/run-ios.sh  # -M virt,gic-version=2
```

A boot that takes its device tree from this repository's own ADT needs nothing more from
this driver. `scripts/make-lumia950-adt.py` writes the node as the phone's firmware does —
`qcom,msm-qgic2` then `arm,gic-2` in `compatible`, the distributor `0xf9000000` and the CPU
interface `0xf9002000` in `reg` — and `/arm-io`'s `ranges` is the identity window, so
`pe_arm_get_soc_base_phys()` is 0 and those two addresses are mapped as written. What is
left on that route is a display driver, blocker 3.

The HackiOS loader's own tree is the other case: it synthesizes a device tree and has no
MSM8992 platform entry, so a loader-built tree needs one (distributor `0xf9000000`, CPU
interface `0xf9002000`, six cores numbered in the second affinity field). The values are
already in the ADT generator above, so the entry is a table row, not a driver.

## Blockers

1. **The route is decided and the firmware environment is measured.** The phone boots its
   own Lumia950XLPkg UEFI and that firmware starts `UniXNU.efi` from EFIESP. Because this
   repository builds no firmware volume, `[firmware]` and `unixnu bootimg` stay unused. The
   chain ran on the phone on 2026-10-23 — BCD `bootsequence` → BootShim → `UEFI.elf` →
   `\EFI\BOOT\BOOTAA64.EFI` → the menu — with GOP drawing the cards and `ConIn` delivering
   volume-key input, and no `UniXNUDxe` entry: `SplashApp`'s `DiscoverAndBootApp` finds the
   removable-media name. `example = true` stays until the kernel route opens, not because
   the payload is unrun.
2. **The interrupt controller is GICv2** (`qcom,msm-qgic2`, no redistributors). The native
   ArmCpu integration and shared core are now written, and standalone QEMU delivers timer,
   software and peripheral IRQs with security extensions off/on. This does not qualify the
   XNU wrapper: a new SDK build, native-kernel timer test and handset IRQ result remain.
   First bring-up is boot-CPU-only; neither the second cluster nor GICv2 remote IPIs are
   enabled. The earlier HackiOS patch is a different implementation, not proof that this
   kernel boots. The on-device EFI probe established EL1, not XNU entry.
3. **Display: rung 1 of 6, and the panel is still unowned.** `NativeMdss` maps the windows,
   proves the mdss domain is on and reads what the firmware left (MDSS/MDP/DSI revisions, the
   DSI enable bits, the raw timing words), and it publishes no framebuffer. What is still
   missing is the part that cannot be guessed: the DSI PHY's PLL table (rung 3) and the
   panel's own init sequence. Lumia950XLPkg says its framebuffer depends on the stock
   Qualcomm UEFI and that MDP is not fully implemented. The EFI probe measures a linear
   1080x1920 BGR framebuffer at 0x00400000. The user's later handoff photograph confirms
   raw framebuffer writes after `ExitBootServices` with firmware MMU mappings retained;
   it does not prove animation, XNU's mappings, or the native driver. With no serial console,
   taking ownership of that display from XNU remains the
   single most expensive missing piece, and it gates every visual effect on this board (see
   "iOS visual effects").
4. **PSCI is HVC and incomplete** on 8992/8994: no `SYSTEM_OFF`, no `SYSTEM_RESET`. The ADT
   tool takes a `psci_method`, so this is expressible, but SMP bring-up is unproven.
5. **The XNU build needs Xcode and the iPhoneOS SDK.** This Mac currently has Command Line
   Tools but no verified iPhoneOS SDK; the standalone EFI builds do not require that SDK.
6. **Hardware feedback is available for the EFI path.** The on-device probe ran, but each
   remaining native-driver rung still needs a corresponding phone measurement.
7. **The GPU's hardware proof still stops before submission.** The default driver's
   read-only identity path checks rail and clock state. Mapping, context, firmware-download,
   submission and verification backends are now implemented, host-tested, and cross-compiled
   for the EFI target, but the hardware power/routing/ownership prerequisites and actual
   command completion still need on-device qualification. See [GPU bring-up](docs/gpu-bringup.md).
8. **Firmware is obtainable; Metal remains a separate port.** `scripts/adreno-firmware.py`
   fetches pinned PM4/PFP images selected by Qualcomm's A418 driver. Their valid zero
   container headers are supported. There is still no compatible iOS 16/A418 Metal plugin,
   and Turnip excludes A4xx entirely; a successful CP memory fill would not solve that gap.

## The CI jobs

> Before touching anything here, read
> [README_BEFORESTARTING!!!!!!!!!!!!!!.md](README_BEFORESTARTING!!!!!!!!!!!!!!.md). There is no
> verified iPhoneOS SDK on this machine: the `kernel` job compiles the IOKit driver, and a
> green `validate` does not mean a driver built.

There is no Mac here, and the kernel step needs one — Xcode plus the iphoneos SDK. Upstream
solves that with a macOS runner, and the payload needs an EDK II toolchain that Windows does
not have either, so this repository carries
[.github/workflows/build.yml](.github/workflows/build.yml) with three jobs:

| Job | Runner | Trigger | What it does |
| --- | --- | --- | --- |
| `validate` | `ubuntu-latest` | every push and pull request | parses `board.toml` and the defconfig, compiles and runs both core tests, generates and checks the ADT, checks that the build driver accepts the board, and that the `example` guard still refuses to build |
| `payload` | `ubuntu-latest` | manual, `target=payload` (the default) | installs clang, lld and the EDK II host tools, clones Mu_Basecore, builds `UniXNU.efi` and `DarwinBoot.efi`, stages `out/payload` and checks it |
| `kernel` | `macos-latest` | manual, `target=kernel` | installs the Homebrew toolchain, fetches the Apple references, runs the XNU build, compiles `NativeSdhci`, links, and merges the image with the pinned kernelcache |

UniXNU's 16.x support travels as `ci/unixnu-16x.patch`: the overlay and the per-line patch
selection belong in UniXNU, but this account cannot push to `UniXNU/UniXNU`. Both CI jobs
apply the shim to their checkout and treat an already-applied shim as a no-op, so the moment
the change is upstream the shim can be deleted without touching the jobs. The build driver
still comes from UniXNU `main`; only the patch selection and its overlay are carried.

The `kernel` job compiles and links the 16.x source line, then merges the image with the
pinned 16.7.16 kernelcache through `[kernel.prelink]`. Every stop fails the job: there is no
gate left to tolerate. The job proves the merge rather than trusting its exit status — the
output is parsed as a Mach-O, and the report must name components, rebindings and sparse
index entries. The image, the prelinked candidate, the report and every log travel in the
`lumia950-kernel` artifact.

That job is the first real test of `NativeSdhci.cpp`: it compiles with `-Wall -Wextra
-Werror` against the recorded kernel ABI and asserts the object is an ARM64 `MH_OBJECT`.

Trigger a job from **Actions → build → Run workflow**, or with the CLI. The dispatch input
picks the job, so a payload build does not also start the six-hour macOS one:

```sh
gh workflow run build.yml -f target=payload
gh run watch

gh workflow run build.yml -f target=kernel
```

## Commands

```sh
# from the UniXNU checkout, with this directory beside it
py -3 unixnu defconfig lumia950_defconfig -D ../lumia950
py -3 unixnu status -D ../lumia950
py -3 unixnu build kernel --dry-run -D ../lumia950     # refuses: example = true

# host test of the driver cores (39 eMMC + 184 GPU + 49 display checks)
cd lumia950 && scripts\test-core.bat

# the addon a rung-6 plugin would travel in: layout, bundle, iOS target
py -3 scripts/check-gpu-addon.py --self-test
py -3 scripts/check-gpu-addon.py build/gpu-addon.bin

# a screenshot is this panel's or it is not evidence
py -3 scripts/check-screenshot.py out/lockscreen.png
```

The hardware rung is the staged payload on EFIESP, not a boot image: `unixnu bootimg` packs a
firmware volume for an Android bootloader, and a Lumia has none. The kernel step stays
`unixnu build kernel` on macOS.
