# UniXNU overlay for the Lumia 950 XL

Changes this board needs in a [UniXNU](https://github.com/UniXNU/UniXNU) checkout, on top of commit
`1a01c2a` plus this repository's `ci/unixnu-16x.patch` (the iOS 16 / xnu-8796 route):

```sh
git apply /path/to/this/repo/ci/unixnu-16x.patch
cp patches/xnu-8796/*.patch /path/to/UniXNU/patches/xnu-8796/      # from this directory
cd /path/to/UniXNU && patch -p1 < /path/to/this/repo/unixnu-overlay/build-xnu-4k-baseline-native-sdhci.patch
```

UniXNU applies every `patches/xnu-8796/*.patch` to the XNU source in name order, so the `zz*` patches
come last. If you edit one after a build, reverse-apply the old version in
`out/lumia950/tree/.cache/xnu-work` first (`patch -R -p1 < old.patch`), or the next build fails.

| Patch | What it does |
| --- | --- |
| `verbose-text-console.patch` | a UEFI loader always reports a display; honour `-v` so the kernel log is drawn as text on the panel |
| `zzq-native-diagnostics.patch` | bring-up diagnostics: IOKit lookups, opens and user-client calls by daemons (the `IOK` lines), and what named daemons write to `/dev/null` (`nativenull=`, after ios-sweet 0048) |
| `zzr-console-mute.patch` | the display driver mutes the text console once the GUI owns the panel; every console character also goes to a ring that `NativeSdhci` writes to the eMMC (NATLOG) |
| `zzs-crash-log.patch` | a `CRASH` report on the console for every user-process crash (signal, reason, pc, lr, far, backtrace, shared-cache slide) |
| `zzt-percpu-single-cpu.patch` | an empty per-CPU secondary range with `cpus=1` (the pointer iterator wrapped and walked unmapped memory) |
| `zzu-unsealed-rw-root.patch` | an unsealed, writable HFS+ root: `unsealed_root=1` answers the root-authentication ioctl, `rootrw=1` mounts read-write, HFS rename-swap falls back to exchange |
| `zzv-aes-shim.patch` | publishes the board's stand-in services (IOAESAccelerator for AppleImage4, AppleKeyStore, IOWatchdog, IOSurface root, touch, HID, CPU clock, battery, display), the sandbox stand-in, the storage report |
| `zzw-procmon.patch` | `procmon=N`: every N seconds the busiest tasks by CPU and, for `nativesample=` processes, every thread's user backtrace |
| `build-xnu-4k-baseline-native-sdhci.patch` | links `libNativeSdhci.a` when `CONFIG_XNU_NATIVE_SDHCI=y` |
