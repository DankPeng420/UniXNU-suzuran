#!/bin/sh
# Builds the loader: build-loader.sh loader-v19 Image-loader-v19   (clang + ld.lld + llvm-objcopy, any host)
set -e
cd "$(dirname "$0")"
CF="--target=aarch64-none-elf -O2 -ffreestanding -fno-builtin -nostdlib -mgeneral-regs-only -mstrict-align -fno-stack-protector"
for f in m2/start m2/vectors2 enter; do clang $CF -c $f.S -o $(basename $f).o; done
clang $CF -c $1.c -o $1.o
ld.lld -T m2/link2.ld start.o vectors2.o $1.o enter.o -o $1.elf
llvm-objcopy -O binary $1.elf $2
rm -f start.o vectors2.o enter.o $1.o $1.elf
