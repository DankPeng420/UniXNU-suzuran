#!/usr/bin/env python3
"""READ-ONLY on the eMMC. Reports state of the 4 GPT sectors vs saved originals / intended new data,
and tests how adb exec-in + dd behave (writes only to /tmp in TWRP's RAM disk)."""
import sys, os, subprocess, hashlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gpt42
adb, out = sys.argv[1], sys.argv[2]
def run(mode, cmd, inp=None):
    r = subprocess.run([adb, mode, cmd], input=inp, capture_output=True)
    return r.returncode, r.stdout, r.stderr
def read(lba, cnt):
    rc, d, e = run('exec-out', 'dd if=/dev/block/mmcblk0 bs=512 skip=%d count=%d 2>/dev/null' % (lba, cnt))
    assert len(d) == cnt * 512, (lba, len(d), e); return d
total = int(run('exec-out', 'cat /sys/class/block/mmcblk0/size')[1].strip())
orig = {}
for f in os.listdir(out):
    if f.startswith('gpt-orig-lba'): orig[int(f[12:-4])] = open(os.path.join(out, f), 'rb').read()
print('saved originals for LBAs', sorted(orig))
def read_orig(lba, cnt):   # live disk with saved originals overlaid = disk as it was before
    d = bytearray(read(lba, cnt))
    for l, b in orig.items():
        if lba <= l < lba + cnt: d[(l - lba) * 512:(l - lba + 1) * 512] = b
    return bytes(d)
w, info = gpt42.patch(read_orig, total); print('reconstructed plan:', info)
for l, o, n in w:
    cur = read(l, 1)
    st = 'ORIGINAL' if cur == o else 'NEW' if cur == n else 'OTHER sha=' + hashlib.sha256(cur).hexdigest()[:16]
    print('LBA %d: %s' % (l, st))
print('--- exec-in tests (to /tmp only)')
blob = os.urandom(512)
print('cat  :', run('exec-in', 'cat > /tmp/t1', blob)[0], run('exec-out', 'wc -c < /tmp/t1')[1])
rc, o, e = run('exec-in', 'dd of=/tmp/t2 bs=512 count=1 conv=notrunc', blob); print('dd notrunc rc', rc, o, e)
print('  t2 size:', run('exec-out', 'wc -c < /tmp/t2 2>&1')[1])
rc, o, e = run('exec-in', 'dd of=/tmp/t3 bs=512 count=1', blob); print('dd plain rc', rc, o, e)
print('  t3 size:', run('exec-out', 'wc -c < /tmp/t3 2>&1')[1])
print('  t3 equal:', run('exec-out', 'cat /tmp/t3')[1] == blob)
print('dd is:', run('exec-out', 'ls -l $(command -v dd); dd --version 2>&1 | head -2; ls /sbin | head -50 | tr "\\n" " "')[1].decode(errors='replace'))
