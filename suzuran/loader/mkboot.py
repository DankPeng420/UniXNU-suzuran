#!/usr/bin/env python3
"""Build an Android boot.img (v0, Sony kitakami layout) with a QCDT dt.img for the S1 bootloader.
usage: mkboot.py Image out.img [payload [ramdisk_addr]]
(payload is stored raw as the "ramdisk"; the loader finds it via /chosen linux,initrd-start)"""
import struct, sys, gzip, io

def fdt(nodes):
    """nodes: list of (path_depth_events) built via helper below -> FDT blob"""
    strings = b''; stroff = {}
    def soff(n):
        nonlocal strings
        if n not in stroff:
            stroff[n] = len(strings); strings += n.encode() + b'\0'
        return stroff[n]
    st = b''
    def pad(b): return b + b'\0' * (-len(b) % 4)
    for ev in nodes:
        if ev[0] == 'begin':
            st += struct.pack('>I', 1) + pad(ev[1].encode() + b'\0')
        elif ev[0] == 'end':
            st += struct.pack('>I', 2)
        else:
            _, name, val = ev
            st += struct.pack('>III', 3, len(val), soff(name)) + pad(val)
    st += struct.pack('>I', 9)
    rsv = struct.pack('>QQ', 0, 0)
    hdr_len = 40
    off_rsv = hdr_len
    off_st = off_rsv + len(rsv)
    off_str = off_st + len(st)
    total = off_str + len(strings) + 0x1000   # include free space for the bootloader's edits
    hdr = struct.pack('>10I', 0xd00dfeed, total, off_st, off_str, off_rsv, 17, 16, 0, len(strings), len(st))
    blob = hdr + rsv + st + strings
    return blob + b'\0' * (total - len(blob))

def u32s(*v): return b''.join(struct.pack('>I', x) for x in v)
def s(x): return x.encode() + b'\0'

MSM_IDS = [(207, 0x20000), (207, 0x20001), (207, 0x20002)]
BOARD_IDS = [(8, 0), (0, 0), (1, 0), (11, 0), (0x1f, 0), (0x22, 0), (0x24, 0)]

def make_dtb():
    msm = b''.join(u32s(p, r) for p, r in MSM_IDS)
    brd = b''.join(u32s(v, st) for v, st in BOARD_IDS)
    ev = [('begin', ''),
          ('prop', '#address-cells', u32s(2)), ('prop', '#size-cells', u32s(2)),
          ('prop', 'model', s('suzuran-ios-probe')),
          ('prop', 'compatible', s('somc,suzuran') + s('qcom,msm8994')),
          ('prop', 'qcom,msm-id', msm), ('prop', 'qcom,board-id', brd),
          ('begin', 'chosen'), ('prop', 'bootargs', s('')), ('end',),
          ('begin', 'memory'), ('prop', 'device_type', s('memory')),
          ('prop', 'reg', struct.pack('>QQ', 0, 0x80000000)), ('end',),
          ('end',)]
    return fdt(ev)

def make_qcdt(dtb, page=4096):
    # QCDT v2: magic, version, count; entries: platform, variant, subtype, soc_rev, offset, size
    entries = [(p, v, st, r) for p, r in MSM_IDS for v, st in BOARD_IDS]
    hdr_size = 12 + len(entries) * 24 + 4
    dtb_off = (hdr_size + page - 1) // page * page
    dtb_p = dtb + b'\0' * (-len(dtb) % page)
    h = struct.pack('<4sII', b'QCDT', 2, len(entries))
    for p, v, st, r in entries:
        h += struct.pack('<6I', p, v, st, r, dtb_off, len(dtb_p))
    h += b'\0' * (dtb_off - len(h))
    return h + dtb_p

def empty_ramdisk():
    trailer = b'070701' + b'00000000' * 11 + b'0000000B' + b'00000000' + b'TRAILER!!!\0'
    trailer += b'\0' * (-len(trailer) % 4)
    raw = trailer + b'\0' * (-len(trailer) % 512)
    bio = io.BytesIO()
    with gzip.GzipFile(fileobj=bio, mode='wb', mtime=0) as g: g.write(raw)
    return bio.getvalue()

def main(kernel_path, out, payload_path=None, rd_addr=0x50000000):
    page = 4096
    base = 0x00000000
    kernel = open(kernel_path, 'rb').read()
    if payload_path:
        ramdisk = open(payload_path, 'rb').read()
        rd_load = rd_addr
    else:
        ramdisk = empty_ramdisk()
        rd_load = base + 0x02000000
    dt = make_qcdt(make_dtb(), page)
    cmdline = b'suzuran-ios-probe'
    hdr = struct.pack('<8s10I', b'ANDROID!',
                      len(kernel), base + 0x00008000,
                      len(ramdisk), rd_load,
                      0, base + 0x00F00000,
                      base + 0x01E00000, page, len(dt), 0)
    hdr += b'\0' * 16                      # name
    hdr += cmdline + b'\0' * (512 - len(cmdline))
    hdr += b'\0' * 32                      # id
    hdr += b'\0' * 1024                    # extra cmdline
    def pg(b): return b + b'\0' * (-len(b) % page)
    img = pg(hdr) + pg(kernel) + pg(ramdisk) + pg(dt)
    open(out, 'wb').write(img)
    print(f'{out}: {len(img)} bytes (kernel {len(kernel)}, ramdisk {len(ramdisk)}, dt {len(dt)})')

if __name__ == '__main__':
    a = sys.argv[1:]
    main(a[0], a[1], a[2] if len(a) > 2 else None, int(a[3], 0) if len(a) > 3 else 0x50000000)
