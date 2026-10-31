#!/usr/bin/env python3
"""Prepare a hash-qualified ArmCpu GICv2 source without changing the upstream checkout."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE_REVISION = "9066b53bbc82e92a98ec44a11dd50fd13eaf1a1b"
SOURCE_SHA256 = "3c87b3bd617be5582a6a24ff797c359497ee9feaa702ee883ecee4270db21c9f"
PATCHED_SHA256 = "9c3dd3bf727e6bfa20e85a8ede64f4d0b72de46f78650193c0a61e3b902de728"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def prepare(source, destination, patch=None):
    source, destination = Path(source).resolve(), Path(destination).resolve()
    patch = Path(patch) if patch else ROOT / "drivers/NativeGicV2/ArmCpu.patch"
    if source == destination:
        raise ValueError("generated ArmCpu must not overwrite the upstream source")
    original = source.read_bytes()
    if digest(original) != SOURCE_SHA256:
        raise ValueError(
            f"unqualified ArmCpu source {source}; use UniXNU/ArmCpu at {SOURCE_REVISION}, "
            "or review and refresh the GICv2 patch and both source hashes"
        )
    patch_program = shutil.which("patch")
    if not patch_program:
        raise ValueError("patch is required to prepare ArmCpu")
    implementation = {
        name: digest((ROOT / "drivers/NativeGicV2" / name).read_bytes())
        for name in ("GicV2Core.h", "GicV2Core.cpp")
    }
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="armcpu-", dir=destination.parent) as temporary:
        staged = Path(temporary) / "ArmCpu.cpp"
        staged.write_bytes(original)
        result = subprocess.run([patch_program, "-t", "-N", "-p1", "-i", str(patch.resolve())],
                                cwd=temporary, capture_output=True, text=True, check=False)
        if result.returncode:
            raise ValueError(f"ArmCpu patch failed: {result.stdout}{result.stderr}")
        generated = staged.read_bytes()
        if digest(generated) != PATCHED_SHA256:
            raise ValueError("patched ArmCpu hash mismatch; no generated source published")
        os.replace(staged, destination)
    manifest = {
        "repository": "https://github.com/UniXNU/ArmCpu",
        "revision": SOURCE_REVISION,
        "source_sha256": SOURCE_SHA256,
        "patch_sha256": digest(patch.read_bytes()),
        "generated_sha256": PATCHED_SHA256,
        "implementation_sha256": implementation,
        "xnu_abi_compile_verified": False,
        "hardware_verified": False,
    }
    destination.with_suffix(".manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    try:
        manifest = prepare(args.source, args.destination)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print(f"ArmCpu source prepared: {manifest['generated_sha256']}")
    print("Source preparation only; compile with the recorded XNU ABI recipe.")


if __name__ == "__main__":
    main()
