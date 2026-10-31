#!/usr/bin/env python3
"""Build NativeAES16 and bind its IOAESAccelerator imports to the pinned 16.7.16 family.

build-arm-driver.py compiles drivers/NativeAES16 into build/native-aes16/libNativeAES16.a; this adds
the recorded original bindings (drivers/NativeAES16/original-bindings-16.7.16.S) and refuses to
finish if the compiled class imports a base-class symbol the bindings do not cover.
"""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/native-aes16"

subprocess.run([sys.executable, str(ROOT / "scripts/build-arm-driver.py"), "NativeAES16"], check=True)
bindings = ROOT / "drivers/NativeAES16/original-bindings-16.7.16.S"
bound = set(re.findall(r"^\.globl (\S+)$", bindings.read_text(), re.M))
undefined = subprocess.check_output(["xcrun", "nm", "-u", str(OUT / "NativeAES.o")], text=True).split()
base = {name for name in undefined if "IOAESAccelerator" in name or "IOCryptoAccelerator" in name}
missing = sorted(base - bound)
if missing:
    raise SystemExit("unbound IOAESAccelerator imports: " + ", ".join(missing))
subprocess.run(["xcrun", "clang", "-target", "arm64-apple-ios16.0", "-Werror", "-Wno-incompatible-sysroot",
                "-c", str(bindings), "-o", str(OUT / "original-bindings.o")], check=True)
subprocess.run(["xcrun", "ar", "rs", str(OUT / "libNativeAES16.a"), str(OUT / "original-bindings.o")], check=True)
print(f"NativeAES16: {len(base)} base-class imports bound, {len(bound - base)} bindings unused")
