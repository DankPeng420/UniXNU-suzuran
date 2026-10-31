#!/usr/bin/env python3
"""Build NativeSdhci and bind its IOBlockStorageDevice imports to the pinned 16.7.16 IOStorageFamily.

build-arm-driver.py compiles drivers/NativeSdhci into build/native-sdhci/libNativeSdhci.a; this adds
the recorded original bindings (drivers/NativeSdhci/original-bindings-16.7.16.S) and refuses to
finish if the compiled driver imports a base-class symbol they do not cover, or the abstract
complete/deleting destructors (which trap in the original).
"""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/native-sdhci"

subprocess.run([sys.executable, str(ROOT / "scripts/build-arm-driver.py"), "NativeSdhci"], check=True)
bindings = ROOT / "drivers/NativeSdhci/original-bindings-16.7.16.S"
bound = set(re.findall(r"^\.globl (\S+)$", bindings.read_text(), re.M))
undefined = subprocess.check_output(["xcrun", "nm", "-u", str(OUT / "NativeSdhci.o")], text=True).split()
base = {name for name in undefined if "IOBlockStorageDevice" in name}
if any(re.search(r"IOBlockStorageDeviceD[01]Ev$", name) for name in base):
    raise SystemExit("NativeSdhci must not call the abstract complete/deleting destructors")
missing = sorted(base - bound)
if missing:
    raise SystemExit("unbound IOBlockStorageDevice imports: " + ", ".join(missing))
subprocess.run(["xcrun", "clang", "-target", "arm64-apple-ios16.0", "-Werror", "-Wno-incompatible-sysroot",
                "-c", str(bindings), "-o", str(OUT / "original-bindings.o")], check=True)
subprocess.run(["xcrun", "ar", "rs", str(OUT / "libNativeSdhci.a"), str(OUT / "original-bindings.o")], check=True)
print(f"NativeSdhci: {len(base)} IOBlockStorageDevice imports bound")
