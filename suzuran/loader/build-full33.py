#!/usr/bin/env python3
# full33 = full31 (kernel cpu2) + nativea53=0: A53 left at the firmware's 768 MHz / 1000 mV, A57 633.6 MHz; temperatures still logged.
# + suzuran-smp8-iphone8d.adt (= iphone8c + --addon) + addon.bin and root-addon.trustcache (build-addon.py).
# Usage: python build-full33.py [kernel zip or kernel.macho]
import hashlib, subprocess, sys, zipfile
src = sys.argv[1] if len(sys.argv) > 1 else '../ci/lumia950-kernel-cpu2.zip'
if src.endswith('.zip'):
    z = zipfile.ZipFile(src)
    names = [n for n in z.namelist() if n.endswith('kernel.macho')]
    assert names, z.namelist()
    k = z.read(names[0])
else:
    k = open(src, 'rb').read()
assert k[:4] == b'\xcf\xfa\xed\xfe', 'not a 64-bit Mach-O'
assert b'NativeAddon: archive of' in k, 'this kernel has no NativeAddon'
print('kernel', len(k), hashlib.sha256(k).hexdigest())
open('k-ci.macho', 'wb').write(k)
cmd = open('cmdline-full33.txt').read().strip()
subprocess.run([sys.executable, 'mkpayload2.py', 'xnu-payload-full33.bin', '--kernel', 'k-ci.macho', '--adt', 'suzuran-smp8-iphone8d.adt',
                '--cmdline', cmd, '--trustcache', 'root-addon.trustcache', '--addon', 'addon.bin', '--window-mb', '736'], check=True)
subprocess.run([sys.executable, 'mkboot.py', 'Image-loader-v19', '../loader-xnu-full33.img', 'xnu-payload-full33.bin', '0x50000000'], check=True)
print(hashlib.sha256(open('../loader-xnu-full33.img', 'rb').read()).hexdigest())
