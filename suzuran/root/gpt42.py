#!/usr/bin/env python3
"""Rename/retype GPT entry 42 (userdata -> 'ios', Apple HFS+) on the phone via adb, touching only 4 sectors:
primary header, the primary entry sector holding #42, backup header, backup entry sector holding #42.
Verifies all CRCs before, keeps originals in root/out/gpt-orig-*.bin, reads back after.
usage: gpt42.py ADB OUTDIR [--dry]     (also usable offline: gpt42.py --selftest)"""
import sys, struct, zlib, subprocess, os
SS = 512; IDX = 41; DEV = '/dev/block/mmcblk0'
HFS = bytes.fromhex('005346480000aa11aa1100306543ecac')   # 48465300-0000-11AA-AA11-00306543ECAC on disk

def crc(b): return zlib.crc32(b) & 0xffffffff

def check_hdr(h, where):
    assert h[:8] == b'EFI PART', where + ': no GPT signature'
    hs = struct.unpack_from('<I', h, 12)[0]; assert 92 <= hs <= SS
    z = bytearray(h[:hs]); z[16:20] = b'\0' * 4
    assert crc(z) == struct.unpack_from('<I', h, 16)[0], where + ': header CRC bad'
    my, alt, _, _ = struct.unpack_from('<QQQQ', h, 24)
    elba, n, esz, ecrc = struct.unpack_from('<QIII', h, 72)
    return dict(hs=hs, my=my, alt=alt, elba=elba, n=n, esz=esz, ecrc=ecrc)

def name_of(e): return e[56:128].decode('utf-16le').split('\0')[0]

def patch(read, total):
    """read(lba, count)->bytes. returns list of (lba, old, new) single sectors to write, and info."""
    ph = read(1, 1); p = check_hdr(ph, 'primary')
    assert p['my'] == 1 and p['alt'] == total - 1, 'primary header LBAs odd: %r total %d' % (p, total)
    bh = read(total - 1, 1); b = check_hdr(bh, 'backup')
    assert b['my'] == total - 1 and b['alt'] == 1
    assert (p['n'], p['esz'], p['ecrc']) == (b['n'], b['esz'], b['ecrc']), 'primary/backup entry arrays differ'
    assert p['esz'] == 128 and p['n'] > IDX
    nsec = (p['n'] * p['esz'] + SS - 1) // SS
    pe = read(p['elba'], nsec); be = read(b['elba'], nsec)
    assert crc(pe[:p['n'] * 128]) == p['ecrc'], 'primary entries CRC bad'
    assert pe == be, 'primary/backup entries differ'
    e = pe[IDX * 128:(IDX + 1) * 128]
    first, last = struct.unpack_from('<QQ', e, 32)
    info = 'entry 42: name=%r first=%d last=%d sectors=%d' % (name_of(e), first, last, last - first + 1)
    if name_of(e) == 'ios' and e[:16] == HFS: return [], info + ' (already done)'
    assert name_of(e) == 'userdata', 'entry 42 is not userdata: ' + info
    ne = bytearray(e); ne[:16] = HFS; ne[56:128] = 'ios'.encode('utf-16le').ljust(72, b'\0')
    arr = bytearray(pe); arr[IDX * 128:(IDX + 1) * 128] = ne
    ecrc = crc(bytes(arr[:p['n'] * 128]))
    def newhdr(h, x):
        h = bytearray(h); struct.pack_into('<I', h, 88, ecrc); h[16:20] = b'\0' * 4
        struct.pack_into('<I', h, 16, crc(bytes(h[:x['hs']]))); return bytes(h)
    s = (IDX * 128) // SS
    assert (IDX * 128) % SS + 128 <= SS
    old_s = pe[s * SS:(s + 1) * SS]; new_s = bytes(arr[s * SS:(s + 1) * SS])
    w = [(p['elba'] + s, old_s, new_s), (1, ph, newhdr(ph, p)),     # entries before header
         (b['elba'] + s, old_s, new_s), (total - 1, bh, newhdr(bh, b))]
    lbas = [x[0] for x in w]
    assert all(l in (1, total - 1) or l == p['elba'] + s or l == b['elba'] + s for l in lbas)
    assert p['elba'] + s < first, 'primary entry sector would be inside a partition?!'
    return w, info + ' -> ios / AF00'

def selftest():
    total = 100000; disk = bytearray(total * SS)
    ents = bytearray(128 * 128)
    for i, nm in enumerate(['TA', 'other'] + ['p%d' % k for k in range(3, 42)] + ['userdata']):
        e = bytearray(128); e[:16] = os.urandom(16); e[16:32] = os.urandom(16)
        struct.pack_into('<QQ', e, 32, 1000 + i * 1000, 1999 + i * 1000); e[56:56 + 2 * len(nm)] = nm.encode('utf-16le')
        ents[i * 128:(i + 1) * 128] = e
    def hdr(my, alt, elba):
        h = bytearray(SS); h[:8] = b'EFI PART'; struct.pack_into('<IIII', h, 8, 0x10000, 92, 0, 0)
        struct.pack_into('<QQQQ', h, 24, my, alt, 34, total - 34); h[56:72] = os.urandom(16)
        struct.pack_into('<QIII', h, 72, elba, 128, 128, crc(bytes(ents)))
        struct.pack_into('<I', h, 16, crc(bytes(h[:92]))); return h
    disk[SS:2 * SS] = hdr(1, total - 1, 2); disk[2 * SS:34 * SS] = ents
    disk[(total - 33) * SS:(total - 1) * SS] = ents; disk[(total - 1) * SS:] = hdr(total - 1, 1, total - 33)
    rd = lambda l, c: bytes(disk[l * SS:(l + c) * SS])
    w, info = patch(rd, total); print(info, [x[0] for x in w])
    for l, o, n in w: disk[l * SS:(l + 1) * SS] = n
    w2, info2 = patch(rd, total); print(info2); assert not w2
    print('selftest OK')

def main():
    if sys.argv[1] == '--selftest': return selftest()
    adb, out = sys.argv[1], sys.argv[2]; dry = '--dry' in sys.argv
    def sh(cmd, inp=None):
        r = subprocess.run([adb, 'exec-out' if inp is None else 'exec-in', cmd], input=inp, capture_output=True)
        return r.stdout
    def read(lba, cnt):
        d = sh('dd if=%s bs=512 skip=%d count=%d 2>/dev/null' % (DEV, lba, cnt))
        assert len(d) == cnt * SS, 'short read at LBA %d: %d bytes' % (lba, len(d)); return d
    total = int(sh('cat /sys/class/block/mmcblk0/size').strip())
    print('disk sectors', total)
    w, info = patch(read, total); print(info)
    if not w: print('GPT_OK (nothing to do)'); return
    for l, o, n in w: open(os.path.join(out, 'gpt-orig-lba%d.bin' % l), 'wb').write(o)
    print('will write LBAs', [x[0] for x in w])
    def push(data, remote):
        p = os.path.join(out, 'tmp-push.bin'); open(p, 'wb').write(data)
        wp = subprocess.run(['wslpath', '-w', p], capture_output=True, text=True).stdout.strip()
        r = subprocess.run([adb, 'push', wp, remote], capture_output=True, text=True)
        got = sh('cat %s' % remote); assert got == data, 'push to %s failed: %s %s' % (remote, r.stdout, r.stderr)
    def ddsh(cmd):
        r = subprocess.run([adb, 'shell', cmd + ' ; echo RC=$?'], capture_output=True, text=True)
        o = (r.stdout + r.stderr).replace('\r', ''); assert 'RC=0' in o, 'dd failed: ' + o; return o
    # self-test of TWRP dd seek/notrunc on a RAM file before touching the eMMC
    t = os.urandom(512); push(t, '/tmp/gpt-t.bin')
    ddsh('dd if=/dev/zero of=/tmp/dt bs=512 count=4')
    ddsh('dd if=/tmp/gpt-t.bin of=/tmp/dt bs=512 seek=2 count=1')   # TWRP dd has no conv=
    dt = sh('cat /tmp/dt'); assert dt[:1536] == b'\0' * 1024 + t, 'dd seek/notrunc self-test failed (%d bytes)' % len(dt)
    print('dd self-test OK')
    if dry: print('dry run, nothing written to eMMC'); return
    def put(l, data):
        push(data, '/tmp/gpt-lba%d.bin' % l)
        ddsh('dd if=/tmp/gpt-lba%d.bin of=%s bs=512 seek=%d count=1' % (l, DEV, l))
        subprocess.run([adb, 'shell', 'sync'])
        assert read(l, 1) == data, 'read-back mismatch at LBA %d' % l
    done = []
    try:
        for l, o, n in w:
            put(l, n); done.append((l, o)); print('LBA %d written + verified' % l)
    except Exception as ex:
        print('WRITE FAILED:', ex, '-> restoring originals')
        failing = w[len(done)]   # may be half-written too
        for l, o in reversed(done + [(failing[0], failing[1])]):
            try: put(l, o); print('LBA %d restored' % l)
            except Exception as e2: print('RESTORE FAILED LBA %d: %s' % (l, e2))
        raise
    check_hdr(read(1, 1), 'primary after'); check_hdr(read(total - 1, 1), 'backup after')
    w2, info2 = patch(read, total); print('after:', info2); assert not w2
    print('GPT_OK')

if __name__ == '__main__': main()
