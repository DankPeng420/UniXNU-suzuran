#!/usr/bin/env python3
"""Finish the merged iOS 16.7.16 (20H392) HFS+ root after OS + both cryptexes were copied into it.

  sudo stage-ios-root.py MOUNTED_ROOT

The root follows the layout ios-sweet and ios-fuxi boot to SpringBoard on iOS 16 (their mkiosroot.sh,
fuxi-stage-root.py and sweet-root-tweaks.py were the reference; this is a separate implementation for
this device repository). Run as root against the mounted image, never /. Idempotent.

  cryptex links  every file of /private/preboot/Cryptexes/{OS,App} with no counterpart in the OS root
                 is hard-linked (symlinks recreated) at its root path. With ignition_level=1 dyld looks
                 for the shared cache only in /System/Library/Caches/com.apple.dyld and does not search
                 the cryptexes, so this also puts the cache where dyld finds it, without extra space.
  log stores     /private/var/db/{diagnostics,uuidtext,diagnostics/timesync} owned by _logd (272) with
                 .migrated markers (made at restore on a real device; logd crashes without them).
  keybag         /private/var/keybags/systembag.kb as a plain plist (read with /defaults
                 no-effaceable-storage instead of an effaceable-key-wrapped bag).
  launchd.plist  _PanicOnCrash -> _PanicOnCrasX (a crashing daemon must not panic the system);
                 CommCenter jobs removed (no baseband; Setup blocks on its XPC).
  containers     /private/var/containers/{Bundle,Data/System,Shared/SystemGroup,Temp} (on a device they
                 live on the Data volume, which this single-volume root does not have).
  activation     mobileactivationd data ark marked Activated.
  audio          VirtualAudio HAL plug-in moved aside, MediaToolbox StartRecorderServer off (mediaserverd
                 crash loops without a GPU/audio device).
  preferences    migration done for 20H392, SpringBoard watchdogs off, Setup marked done.
"""
import glob
import os
import plistlib
import shutil
import sys
from pathlib import Path

BUILD, VERSION = '20H392', '16.7.16'
MOBILE = 501
LOGD = 272


def inside(root, relative):
    """root/relative, or None when a parent of it inside the root is a symlink (never follow iOS
    absolute symlinks through the host's own file system)."""
    path = root / relative
    parent = path.parent
    while parent != root:
        if parent.is_symlink():
            return None
        parent = parent.parent
    return path


def load(path):
    with open(path, 'rb') as handle:
        return plistlib.load(handle)


def save(path, data, uid, gid, mode, fmt=plistlib.FMT_BINARY):
    with open(path, 'wb') as handle:
        plistlib.dump(data, handle, fmt=fmt)
    os.chown(path, uid, gid)
    os.chmod(path, mode)


def cryptex_links(root):
    linked = skipped = 0
    for cryptex in ('OS', 'App'):
        source = root / 'private/preboot/Cryptexes' / cryptex
        if not (source / 'System').is_dir():
            sys.exit(f'missing the {cryptex} cryptex under {source}')
        for current, directories, files in os.walk(source, followlinks=False):
            names = files + [name for name in directories if (Path(current) / name).is_symlink()]
            for name in names:
                src = Path(current) / name
                dst = inside(root, src.relative_to(source))
                if dst is None:
                    skipped += 1
                    continue
                if os.path.lexists(dst):
                    continue
                dst.parent.mkdir(parents=True, exist_ok=True)
                if src.is_symlink():
                    os.symlink(os.readlink(src), dst)
                    os.lchown(dst, src.lstat().st_uid, src.lstat().st_gid)
                else:
                    os.link(src, dst)
                linked += 1
    cache = root / 'System/Library/Caches/com.apple.dyld'
    caches = sorted(p.name for p in cache.glob('dyld_shared_cache_arm64*')) if cache.is_dir() else []
    if not caches:
        sys.exit('no dyld shared cache at /System/Library/Caches/com.apple.dyld after linking')
    print(f'cryptex links: {linked} linked, {skipped} under existing aliases; dyld cache files {len(caches)}')


def log_stores(root):
    for name, mode in (('diagnostics', 0o751), ('uuidtext', 0o755), ('diagnostics/timesync', 0o755)):
        directory = root / 'private/var/db' / name
        directory.mkdir(parents=True, exist_ok=True)
        os.chown(directory, LOGD, LOGD)
        os.chmod(directory, mode)
        marker = directory / '.migrated'
        marker.touch()
        os.chown(marker, LOGD, LOGD)
        os.chmod(marker, 0o644)


def keybag(root):
    keybags = root / 'private/var/keybags'
    keybags.mkdir(parents=True, exist_ok=True)
    os.chown(keybags, 0, 0)
    os.chmod(keybags, 0o700)
    bag = keybags / 'systembag.kb'
    if not os.path.lexists(bag):
        save(bag, {'KeyBagKeys': b'LUMIA'}, 0, 0, 0o600, plistlib.FMT_XML)


def launchd(root):
    path = root / 'System/Library/xpc/launchd.plist'
    st = path.stat()
    plist = load(path)
    jobs = plist['LaunchDaemons']
    renamed = 0
    for job in jobs.values():
        if '_PanicOnCrash' in job:
            job['_PanicOnCrasX'] = job.pop('_PanicOnCrash')
            renamed += 1
    removed = [key for key, job in jobs.items() if job.get('Label') == 'com.apple.CommCenter']
    for key in removed:
        del jobs[key]
    save(path, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777)
    print(f'launchd.plist: {renamed} _PanicOnCrash renamed, {len(removed)} CommCenter jobs removed')


def containers(root):
    base = root / 'private/var/containers'
    for sub, uid in (('Bundle', 33), ('Data/System', 0), ('Shared/SystemGroup', 0), ('Temp', 0)):
        directory = base / sub
        if not directory.is_dir():
            directory.mkdir(parents=True)
            os.chown(directory, uid, uid)
            os.chmod(directory, 0o755)


def activation(root):
    for meta in glob.glob(str(root / 'private/var/containers/Data/System/*/.com.apple.mobile_container_manager.metadata.plist')):
        if load(meta).get('MCMMetadataIdentifier') == 'com.apple.mobileactivationd':
            directory = Path(meta).parent / 'Library/internal'
            directory.mkdir(parents=True, exist_ok=True)
            os.chown(directory, MOBILE, MOBILE)
            os.chmod(directory, 0o755)
            save(directory / 'data_ark.plist', {
                '-ActivationState': 'Activated', '-BrickState': False,
                '-BuildVersion': BUILD, '-ProductVersion': VERSION,
                '-DataMigrationCompleted': True, '-PreSydroDataMigrationCompleted': True,
            }, MOBILE, MOBILE, 0o600)
            print('activation data ark written')
            return
    print('no mobileactivationd system container; activation comes from the ADT security properties')


def audio(root):
    plugin = root / 'Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin'
    if plugin.exists():
        disabled = root / 'Library/Audio/Plug-Ins/HAL.disabled'
        disabled.mkdir(parents=True, exist_ok=True)
        shutil.move(str(plugin), str(disabled))
    for conf in glob.glob(str(root / 'System/Library/Frameworks/MediaToolbox.framework/*/SubServerConfiguration.plist')):
        st = os.stat(conf)
        data = load(conf)
        data['StartRecorderServer'] = False
        save(conf, data, st.st_uid, st.st_gid, st.st_mode & 0o7777, plistlib.FMT_XML)


def preference(root, name, values):
    directory = root / 'private/var/mobile/Library/Preferences'
    path = directory / (name + '.plist')
    data = load(path) if path.exists() else {}
    data.update(values)
    save(path, data, MOBILE, MOBILE, 0o600)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = Path(sys.argv[1]).resolve()
    if os.geteuid() != 0 or root == Path('/'):
        sys.exit('run as root against the mounted image, never /')
    version = load(root / 'System/Library/CoreServices/SystemVersion.plist')
    if version.get('ProductBuildVersion') != BUILD:
        sys.exit(f'expected iOS {BUILD}, found {version.get("ProductBuildVersion")}')
    if (root / 'sbin/launchd').stat().st_uid != 0:
        sys.exit('source ownership was not preserved (mount the image with owners on)')
    cryptex_links(root)
    log_stores(root)
    keybag(root)
    launchd(root)
    containers(root)
    activation(root)
    audio(root)
    preference(root, 'com.apple.migration', {
        'LastSystemVersion': BUILD,
        'DMUserDataDisposition': {'SyntheticDidUpgrade': False, 'buildVersion': BUILD}})
    preference(root, 'com.apple.springboard', {
        'BKNoWatchdogs': True,
        'FBLaunchWatchdogExceptions': {'com.apple.purplebuddy': 100.0},
        'MCForceShowCloudConfigurationUI': False})
    preference(root, 'com.apple.purplebuddy', {
        'SetupDone': True, 'SetupFinishedAllSteps': True, 'SetupVersion': 11, 'daemonizeNeedsToRun': False})
    preference(root, 'com.apple.purplebuddy.notbackedup', {
        key: False for key in ('ShouldRunDiagnosticsMiniBuddy', 'ShouldRunHSA2MiniBuddy',
                               'ShouldRunKeychainSyncMiniBuddy', 'ShouldRunMesaMiniBuddy',
                               'ShouldRunPaymentMiniBuddy', 'ShouldRunStandardMiniBuddy', 'SilentICDPUpgrade')})
    preference(root, 'com.apple.managedconfiguration.notbackedup', {
        'LockdownActivationIndicatesCloudConfigurationAvailable': False})
    print('root staged')


if __name__ == '__main__':
    main()
