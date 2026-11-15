#!/usr/bin/env python3
"""Pack IPSW trust caches into the bundle DarwinBoot hands to the kernel (stage-kernel.py --trustcache).

  make-trustcache-bundle.py OUT MODULE [MODULE ...]

Each MODULE is one IPSW trust cache (an IM4P "trst"/"trcs"/"trca"/"rtsc" or its raw payload), or several
joined with '+', which are merged into one version-2 module: the kernel loads at most two static caches,
so the root's OS cache is module 0 and the SystemOS + AppOS cryptex caches share module 1, as iBoot
would load them for a device that boots this root.

Bundle (bsd/sys/trust_caches.h): u32 count, u32 offset[count] from the start of the bundle, then the
modules back to back. DarwinBoot's ValidTrustCaches requires each module to end exactly where the next
begins (or at the end of the bundle), so there is no padding; modules are multiples of 4 bytes.
"""
import hashlib
import struct
import sys


def der(data, at):
    tag, length = data[at], data[at + 1]
    at += 2
    if length & 0x80:
        count = length & 0x7f
        length = int.from_bytes(data[at:at + count], 'big')
        at += count
    return tag, at, length


def payload(path):
    data = open(path, 'rb').read()
    if data[:1] != b'\x30':
        return data
    tag, start, length = der(data, 0)
    items, at = [], start
    while at < start + length:
        _, body, size = der(data, at)
        items.append(data[body:body + size])
        at = body + size
    if len(items) < 4 or items[0] != b'IM4P' or items[1] not in (b'trst', b'trcs', b'trca', b'rtsc'):
        sys.exit(f'{path}: not an IM4P trust cache')
    return items[3]


def entries(module, path):
    version, = struct.unpack_from('<I', module, 0)
    count, = struct.unpack_from('<I', module, 20)
    if version != 2 or len(module) != 24 + 24 * count:
        sys.exit(f'{path}: version {version} with {count} entries in {len(module)} bytes; expected version 2')
    rows = [module[24 + 24 * i:48 + 24 * i] for i in range(count)]
    if rows != sorted(rows):
        sys.exit(f'{path}: entries are not sorted')
    return module[4:20], rows


def build(spec):
    parts = [entries(payload(path), path) for path in spec.split('+')]
    if len(parts) == 1:
        uuid, rows = parts[0]
    else:
        rows = sorted({row for _, part in parts for row in part})
        hashes = [row[:20] for row in rows]
        if len(hashes) != len(set(hashes)):
            sys.exit(f'{spec}: one cdhash appears with different flags')
        uuid = hashlib.sha256(b''.join(u for u, _ in parts)).digest()[:16]
    return struct.pack('<I16sI', 2, uuid, len(rows)) + b''.join(rows), len(rows)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out, specs = sys.argv[1], sys.argv[2:]
    modules = [build(spec) for spec in specs]
    head = 4 + 4 * len(modules)
    offsets, body = [], b''
    for module, _ in modules:
        offsets.append(head + len(body))
        body += module
    blob = struct.pack(f'<I{len(modules)}I', len(modules), *offsets) + body
    with open(out, 'wb') as handle:
        handle.write(blob)
    for spec, (_, count) in zip(specs, modules):
        print(f'{count:6} entries  {spec}')
    print(f'{out}: {len(modules)} modules, {len(blob)} bytes')


if __name__ == '__main__':
    main()
