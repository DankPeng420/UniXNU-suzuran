#!/usr/bin/env python3
"""Build the addon (files NativeAddon writes into the iOS root at boot) and the trust cache that lets them run.

  build-addon.py USERLAND_DIR

USERLAND_DIR: the CI's userland output (gputest, cdhashes.txt). Reads ../root/out8/launchd.plist (73-wsl-launchd-
plist.sh) and ../root/out8/root.trustcache. Writes addon.bin (SZADDON1 archive, see drivers/NativeAddon) and
root-addon.trustcache (the bundle with the programs' cdhashes added to module 1).

Files:
  /usr/libexec/suzuran-gputest            the GPU test (0755 root:wheel)
  /System/Library/xpc/launchd.plist       launchd's job list + job com.suzuran.gputest (runs it once per boot)
  /usr/lib/libOSMesa.8.dylib              Mesa freedreno + OSMesa (if the userland build has it)
  /usr/libexec/suzuran-gltri              the OpenGL ES triangle test (started by gputest)
  /usr/lib/libsuzuran.inject.dylib        loaded into backboardd (kernel boot-args dyldinsert=backboardd dyldallow=1)
  /usr/lib/libsuzuran.gl.dylib            OpenGL ES behind EAGLContext (loaded by the inject library with suzurangl=1)
"""
import hashlib, os, plistlib, struct, sys

ud = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
out8 = os.path.join(here, '..', 'root', 'out8')

jobs = plistlib.load(open(os.path.join(out8, 'launchd.plist'), 'rb'))
assert len(jobs['LaunchDaemons']) == 530, 'unexpected launchd.plist'
jobs['LaunchDaemons']['/System/Library/LaunchDaemons/com.suzuran.gputest.plist'] = {
    'Label': 'com.suzuran.gputest',
    'ProgramArguments': ['/usr/libexec/suzuran-gputest'],
    'RunAtLoad': True,
    'KeepAlive': False,
    'UserName': 'root',
    'POSIXSpawnType': 'Interactive',
    'EnablePressuredExit': False,
    'EnableTransactions': False,
}
# the OpenGL ES route: our library inside the window server (when the userland build has it)
inject = os.path.exists(os.path.join(ud, 'libsuzuran.inject.dylib'))
if inject:
    bbd = jobs['LaunchDaemons']['/System/Library/LaunchDaemons/com.apple.backboardd.plist']
    bbd.setdefault('EnvironmentVariables', {})['DYLD_INSERT_LIBRARIES'] = '/usr/lib/libsuzuran.inject.dylib'
launchd = plistlib.dumps(jobs, fmt=plistlib.FMT_BINARY)

files = [
    ('/usr/libexec/suzuran-gputest', 0o755, 0, 0, open(os.path.join(ud, 'gputest'), 'rb').read()),
    ('/System/Library/xpc/launchd.plist', 0o644, 0, 80, launchd),
]
# Mesa (when the userland build has it): gputest runs gltri after its own tests
for name, path, mode in (('libOSMesa.8.dylib', '/usr/lib/libOSMesa.8.dylib', 0o755),
                         ('gltri', '/usr/libexec/suzuran-gltri', 0o755),
                         ('libsuzuran.inject.dylib', '/usr/lib/libsuzuran.inject.dylib', 0o755),
                         ('libsuzuran.gl.dylib', '/usr/lib/libsuzuran.gl.dylib', 0o755)):
    if os.path.exists(os.path.join(ud, name)):
        files.append((path, mode, 0, 0, open(os.path.join(ud, name), 'rb').read()))
pad = lambda b: b + b'\0' * (-len(b) % 8)
blob = b'SZADDON1' + struct.pack('<II', len(files), 1)
for path, mode, uid, gid, data in files:
    p = path.encode() + b'\0'
    blob += struct.pack('<IIIIQ', len(p), mode, uid, gid, len(data)) + pad(p) + pad(data)
open(os.path.join(here, 'addon.bin'), 'wb').write(blob)
for path, mode, uid, gid, data in files:
    print(f'{len(data):10} {mode:o} {uid}:{gid} {hashlib.sha256(data).hexdigest()[:16]} {path}')
print(f'addon.bin: {len(blob)} bytes')

# trust cache: our cdhashes into module 1 (version 2 rows: cdhash, hash type 2, flags 0, category 0, 0), sorted
tc = open(os.path.join(out8, 'root.trustcache'), 'rb').read()
n, = struct.unpack_from('<I', tc, 0)
offs = list(struct.unpack_from('<%dI' % n, tc, 4)) + [len(tc)]
mods = [tc[offs[i]:offs[i + 1]] for i in range(n)]
m = mods[1]
version, uuid, count = struct.unpack_from('<I16sI', m, 0)
rows = {m[24 + 24 * k:48 + 24 * k] for k in range(count)}
for line in open(os.path.join(ud, 'cdhashes.txt')):
    h, name = line.split()
    rows.add(bytes.fromhex(h)[:20] + bytes([2, 0, 0, 0]))
    print('trust cache +', h[:40], name)
rows = sorted(rows)
mods[1] = struct.pack('<I16sI', 2, uuid, len(rows)) + b''.join(rows)
head = 4 + 4 * n
offsets, body = [], b''
for mod in mods:
    offsets.append(head + len(body)); body += mod
open(os.path.join(here, 'root-addon.trustcache'), 'wb').write(struct.pack('<I%dI' % n, n, *offsets) + body)
print(f'root-addon.trustcache: module 1 {count} -> {len(rows)} entries')
