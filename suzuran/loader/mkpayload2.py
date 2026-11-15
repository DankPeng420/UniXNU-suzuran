#!/usr/bin/env python3
"""Pack the suzuran loader payload (SZPK v1).
usage: mkpayload.py out.bin --kernel k.macho --adt tree.bin [--cmdline "..."] [--window-mb 512]"""
import argparse, struct
ap = argparse.ArgumentParser()
ap.add_argument('out'); ap.add_argument('--kernel', required=True); ap.add_argument('--adt', required=True)
ap.add_argument('--cmdline', default=''); ap.add_argument('--trustcache'); ap.add_argument('--addon'); ap.add_argument('--window-mb', type=int, default=512)
a = ap.parse_args()
items = [(1, open(a.kernel, 'rb').read()), (2, open(a.adt, 'rb').read()), (3, a.cmdline.encode() + b'\0')]
if a.trustcache: items.append((4, open(a.trustcache, 'rb').read()))
if a.addon: items.append((5, open(a.addon, 'rb').read()))   # loader v17+: NativeAddon archive
hdr_len = 16 + 24 * len(items)
off = (hdr_len + 0xfff) & ~0xfff
body = b''; entries = b''
for t, data in items:
    entries += struct.pack('<IIQQ', t, 0, off + len(body), len(data))
    body += data + b'\0' * (-len(data) % 0x1000)
blob = struct.pack('<4sIII', b'SZPK', 1, len(items), a.window_mb) + entries
blob += b'\0' * (off - len(blob)) + body
open(a.out, 'wb').write(blob)
print(f'{a.out}: {len(blob)} bytes, window {a.window_mb} MiB, ' + ', '.join(f'type {t}: {len(d)} B' for t, d in items))
