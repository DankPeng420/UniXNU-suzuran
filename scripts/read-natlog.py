#!/usr/bin/env python3
"""Turn a NATLOG dump (the header block + ring pulled from the end of the "ios" partition) into text.

NativeSdhci writes the kernel console there once a second (see startConsoleLog in
drivers/NativeSdhci/NativeSdhci.cpp). Pull it from Android with tools/phone/pull-natlog.sh, then:

    read-natlog.py natlog.bin [--boot N | --all] > boot.txt
"""
import argparse
import struct
import sys

BLOCK = 512


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dump")
    parser.add_argument("--boot", type=int, help="only this boot (default: the latest)")
    parser.add_argument("--all", action="store_true", help="everything still in the ring")
    args = parser.parse_args()

    data = open(args.dump, "rb").read()
    magic, head, ring_blocks, boot, base, lost, flushes = struct.unpack_from("<8sQIIQQQ", data, 0)
    if magic != b"NATLOG01":
        sys.exit("no NATLOG header (magic %r)" % magic)
    ring = data[BLOCK:BLOCK + ring_blocks * BLOCK]
    size = len(ring)
    if size != ring_blocks * BLOCK:
        sys.exit("dump is short: %d of %d ring bytes" % (size, ring_blocks * BLOCK))
    print("# boot %d, stream head %d, boot base %d, lost %d, flushes %d" % (boot, head, base, lost, flushes),
          file=sys.stderr)

    start = 0 if head <= size else -(-(head - size) // BLOCK) * BLOCK
    stream = bytearray()
    for offset in range(start, head, BLOCK):
        slot = (offset // BLOCK) % ring_blocks
        stream += ring[slot * BLOCK:(slot + 1) * BLOCK]
    text = bytes(stream).replace(b"\0", b"").decode("utf-8", "replace")

    if not args.all:
        want = args.boot if args.boot is not None else boot
        marker = "\n===== NATLOG boot %d =====\n" % want
        at = text.find(marker)
        if at < 0:
            sys.exit("boot %d is no longer in the ring (try --all)" % want)
        following = text.find("\n===== NATLOG boot ", at + len(marker))
        text = text[at:following if following >= 0 else len(text)]
    sys.stdout.write(text)


if __name__ == "__main__":
    main()
