#!/usr/bin/env python3
# Names for addresses in the iOS 16 split dyld shared cache: python3 dscsym.py DIR SLIDE ADDR...
# (addresses as the crash log prints them; SLIDE is subtracted first)
import os, struct, sys, bisect

D = sys.argv[1]
slide = int(sys.argv[2], 16)
addrs = [int(a, 16) for a in sys.argv[3:]]

files = sorted(f for f in os.listdir(D) if f.startswith('dyld_shared_cache_arm64') and not f.endswith('.symbols'))
maps = []          # (addr, size, file, fileoff)
fh = {}
for f in files:
    p = os.path.join(D, f)
    h = open(p, 'rb'); fh[f] = h
    hdr = h.read(0x200)
    if hdr[:4] != b'dyld':
        continue
    mo, mc = struct.unpack_from('<II', hdr, 0x10)
    h.seek(mo)
    for i in range(mc):
        a, s, o, _, _ = struct.unpack('<QQQII', h.read(32))
        maps.append((a, s, f, o))
maps.sort()

def read(addr, n):
    for a, s, f, o in maps:
        if a <= addr < a + s:
            fh[f].seek(o + addr - a)
            return fh[f].read(n)
    return None

main = fh['dyld_shared_cache_arm64']
main.seek(0); hdr = main.read(0x200)
io, ic = struct.unpack_from('<II', hdr, 0x1c0)
if ic == 0:
    io, ic = struct.unpack_from('<II', hdr, 0x18)
main.seek(io)
images = []
for i in range(ic):
    a, _, _, po, _ = struct.unpack('<QQQII', main.read(32))
    images.append((a, po))

def cstr(h, off):
    h.seek(off); b = h.read(512)
    return b.split(b'\0')[0].decode(errors='replace')

info = []   # (text_start, text_end, path, linkedit, symtab)
for a, po in images:
    path = cstr(main, po)
    mh = read(a, 32)
    if not mh:
        continue
    ncmds, sizecmds = struct.unpack_from('<II', mh, 16)
    cmds = read(a + 32, sizecmds)
    off = 0; text = None; le = None; st = None
    for _ in range(ncmds):
        cmd, cs = struct.unpack_from('<II', cmds, off)
        if cmd == 0x19:
            name = cmds[off + 8:off + 24].split(b'\0')[0]
            va, vs, fo, fs = struct.unpack_from('<QQQQ', cmds, off + 24)
            if name == b'__TEXT': text = (va, va + vs)
            if name == b'__LINKEDIT': le = (va, fo)
        elif cmd == 2:
            st = struct.unpack_from('<IIII', cmds, off + 8)
        off += cs
    if text:
        info.append((text[0], text[1], path, le, st, a))
info.sort()
starts = [i[0] for i in info]

# local symbols
loc = {}
sp = os.path.join(D, 'dyld_shared_cache_arm64.symbols')
if os.path.exists(sp):
    s = open(sp, 'rb'); sh = s.read(0x200)
    lo, ls = struct.unpack_from('<QQ', sh, 0x48)
    s.seek(lo); li = s.read(24)
    nlo, nlc, sto, sts, eo, ec = struct.unpack('<6I', li)
    s.seek(lo + eo)
    ents = s.read(ec * 16)
    # entry: u64 dylibOffset, u32 nlistStartIndex, u32 nlistCount
    loc_entries = [struct.unpack_from('<QII', ents, i * 16) for i in range(ec)]
    LOC = (s, lo, nlo, sto, loc_entries)
else:
    LOC = None

base = maps[0][0]

def syms_for(img):
    t0, t1, path, le, st, a = img
    out = []
    if st and le:
        symoff, nsyms, stroff, strsize = st
        nl = read(le[0] + symoff - le[1], nsyms * 16)
        strs = read(le[0] + stroff - le[1], strsize)
        if nl and strs:
            for i in range(nsyms):
                sx, ty, sect, desc, val = struct.unpack_from('<IBBHQ', nl, i * 16)
                if ty & 0xe0 == 0 and val:
                    out.append((val, strs[sx:strs.index(b'\0', sx)].decode(errors='replace')))
    if LOC:
        s, lo, nlo, sto, ents = LOC
        for do, start, cnt in ents:
            if do == a - base or do == a:
                s.seek(lo + nlo + start * 16); nl = s.read(cnt * 16)
                for i in range(cnt):
                    sx, ty, sect, desc, val = struct.unpack_from('<IBBHQ', nl, i * 16)
                    if ty & 0xe0 == 0 and val:
                        s.seek(lo + sto + sx); name = s.read(256).split(b'\0')[0].decode(errors='replace')
                        out.append((val, name))
    out.sort()
    return out

cache = {}
for x in addrs:
    u = x - slide
    k = bisect.bisect_right(starts, u) - 1
    if k < 0 or not (info[k][0] <= u < info[k][1]):
        print(f'{x:#x} ({u:#x}): not in an image')
        continue
    img = info[k]
    if img[2] not in cache:
        cache[img[2]] = syms_for(img)
    ss = cache[img[2]]
    j = bisect.bisect_right([v for v, _ in ss], u) - 1
    name = f'{ss[j][1]}+{u - ss[j][0]:#x}' if j >= 0 else '?'
    print(f'{x:#x} ({u:#x}): {os.path.basename(img[2])} {name}')
