"""Downloads for the installer: whole files, and single members of a remote zip.

Everything the installer downloads is pinned by SHA-256 in manifest.toml, and nothing is used
until its hash matches. Downloads resume: a partial file is kept as `<name>.part` and continued
with an HTTP Range request, which matters for the 4.7 GB OS image on a slow line.

The IPSW is a 6.2 GB zip of which the installer needs seven members (5.8 GB). Range requests read
the zip's central directory from the end of the file, then each member's bytes, so the rest (the
restore ramdisks, firmware for other boards) is never downloaded. Stored (uncompressed) members resume like plain files; deflated ones are small and
are fetched in one go.
"""
from __future__ import annotations

import ctypes
import hashlib
import io
import os
import shutil
import struct
import sys
import time
import urllib.error
import urllib.request
import zipfile
import zlib
from pathlib import Path

CHUNK = 1 << 20
USER_AGENT = "UniXNU-lumia950xl-installer"


class FetchError(Exception):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(CHUNK), b""):
            digest.update(block)
    return digest.hexdigest()


def _request(url: str, start: int | None = None, end: int | None = None):
    headers = {"User-Agent": USER_AGENT}
    if start is not None:
        headers["Range"] = f"bytes={start}-" + ("" if end is None else str(end))
    for attempt in range(5):
        try:
            return urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=60)
        except (urllib.error.URLError, TimeoutError) as error:
            if isinstance(error, urllib.error.HTTPError) and error.code < 500:
                raise FetchError(f"{url}: HTTP {error.code}") from error
            if attempt == 4:
                raise FetchError(f"{url}: {error}") from error
            time.sleep(2 ** attempt)
    raise AssertionError("unreachable")


class _Progress:
    def __init__(self, label: str, total: int, done: int = 0):
        self.label, self.total, self.done = label, total, done
        self.started, self.last = time.monotonic(), 0.0
        self.base = done

    def add(self, count: int) -> None:
        self.done += count
        now = time.monotonic()
        if now - self.last >= 1 or self.done >= self.total:
            self.last = now
            rate = (self.done - self.base) / max(now - self.started, 1e-3)
            pct = 100 * self.done / self.total if self.total else 100
            sys.stderr.write(f"\r  {self.label}: {self.done / 1e6:,.0f} / {self.total / 1e6:,.0f} MB "
                             f"({pct:.0f}%, {rate / 1e6:.1f} MB/s)   ")
            if self.done >= self.total:
                sys.stderr.write("\n")
            sys.stderr.flush()


def _stream_range(url: str, dest: Path, offset: int, size: int, label: str) -> None:
    """Copy bytes [offset, offset+size) of `url` to `dest`, continuing a `.part` file."""
    part = dest.with_name(dest.name + ".part")
    have = part.stat().st_size if part.exists() else 0
    if have > size:
        part.unlink()
        have = 0
    progress = _Progress(label, size, have)
    while have < size:
        try:
            with _request(url, offset + have, offset + size - 1) as response:
                if response.status != 206 and not (offset == 0 and have == 0 and response.status == 200):
                    raise FetchError(f"{url}: the server ignored the Range request (HTTP {response.status})")
                with part.open("ab") as out:
                    for block in iter(lambda: response.read(CHUNK), b""):
                        block = block[: size - have]
                        out.write(block)
                        have += len(block)
                        progress.add(len(block))
                        if have >= size:
                            break
        except (OSError, FetchError) as error:
            if isinstance(error, FetchError) and "HTTP 4" in str(error):
                raise
            sys.stderr.write(f"\n  {label}: {error}; resuming at {have / 1e6:,.0f} MB\n")
            time.sleep(3)
    part.replace(dest)


def verified(dest: Path, sha256: str) -> bool:
    return dest.is_file() and sha256_file(dest) == sha256


def _check(dest: Path, sha256: str) -> None:
    actual = sha256_file(dest)
    if actual != sha256:
        bad = dest.with_name(dest.name + ".bad")
        dest.replace(bad)
        raise FetchError(f"{dest.name}: SHA-256 {actual} does not match the pinned {sha256} "
                         f"(kept as {bad.name})")


def download(url: str, dest: Path, sha256: str, size: int | None = None) -> Path:
    if verified(dest, sha256):
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    if size is None:
        with _request(url, 0, 0) as response:
            content_range = response.headers.get("Content-Range", "")
            if response.status == 206 and "/" in content_range:
                size = int(content_range.rsplit("/", 1)[1])
            else:
                size = int(response.headers.get("Content-Length") or 0)
    if not size:
        raise FetchError(f"{url}: the server did not say how big the file is")
    _stream_range(url, dest, 0, size, dest.name)
    _check(dest, sha256)
    return dest


class HttpFile(io.RawIOBase):
    """A read-only, seekable view of a remote file, for zipfile to read the directory through."""

    def __init__(self, url: str):
        self.url, self.pos = url, 0
        with _request(url, 0, 0) as response:
            content_range = response.headers.get("Content-Range", "")
            if response.status != 206 or "/" not in content_range:
                raise FetchError(f"{url}: the server does not support Range requests")
            self.size = int(content_range.rsplit("/", 1)[1])

    def readable(self) -> bool:
        return True

    def seekable(self) -> bool:
        return True

    def tell(self) -> int:
        return self.pos

    def seek(self, offset: int, whence: int = 0) -> int:
        self.pos = {0: offset, 1: self.pos + offset, 2: self.size + offset}[whence]
        return self.pos

    def readinto(self, buffer) -> int:
        if self.pos >= self.size:
            return 0
        end = min(self.pos + len(buffer), self.size) - 1
        with _request(self.url, self.pos, end) as response:
            data = response.read()
        buffer[: len(data)] = data
        self.pos += len(data)
        return len(data)


class RemoteZip:
    def __init__(self, url: str):
        self.url = url
        self.raw = HttpFile(url)
        # A large buffer keeps the central directory to a few requests.
        self.zip = zipfile.ZipFile(io.BufferedReader(self.raw, buffer_size=4 * CHUNK))

    def info(self, member: str) -> zipfile.ZipInfo:
        try:
            return self.zip.getinfo(member)
        except KeyError:
            raise FetchError(f"{self.url} has no member {member}") from None

    def _data_offset(self, info: zipfile.ZipInfo) -> int:
        self.raw.seek(info.header_offset)
        header = self.raw.read(30)
        if header[:4] != b"PK\x03\x04":
            raise FetchError(f"{info.filename}: bad local header")
        name_len, extra_len = struct.unpack_from("<HH", header, 26)
        return info.header_offset + 30 + name_len + extra_len

    def extract(self, member: str, dest: Path, sha256: str) -> Path:
        if verified(dest, sha256):
            return dest
        dest.parent.mkdir(parents=True, exist_ok=True)
        info = self.info(member)
        offset = self._data_offset(info)
        if info.compress_type == zipfile.ZIP_STORED:
            _stream_range(self.url, dest, offset, info.compress_size, Path(member).name)
        elif info.compress_type == zipfile.ZIP_DEFLATED:
            packed = dest.with_name(dest.name + ".deflate")
            _stream_range(self.url, packed, offset, info.compress_size, Path(member).name)
            inflate = zlib.decompressobj(-15)
            with packed.open("rb") as src, dest.open("wb") as out:
                for block in iter(lambda: src.read(CHUNK), b""):
                    out.write(inflate.decompress(block))
                out.write(inflate.flush())
            packed.unlink()
        else:
            raise FetchError(f"{member}: unsupported zip compression {info.compress_type}")
        _check(dest, sha256)
        return dest


def extract_local_zip(archive: Path, member: str, dest: Path, sha256: str) -> Path:
    if verified(dest, sha256):
        return dest
    with zipfile.ZipFile(archive) as zf, zf.open(member) as src, dest.open("wb") as out:
        shutil.copyfileobj(src, out)
    _check(dest, sha256)
    return dest


# ---------------------------------------------------------------------------------------------
# IMG4 payloads


def _der(data: bytes, pos: int) -> tuple[int, int, int]:
    """(tag, content start, content end) of the DER element at `pos`."""
    tag = data[pos]
    length = data[pos + 1]
    pos += 2
    if length & 0x80:
        count = length & 0x7F
        length = int.from_bytes(data[pos : pos + count], "big")
        pos += count
    return tag, pos, pos + length


def _lzfse_decode(data: bytes, size_hint: int) -> bytes:
    """LZFSE through macOS's libcompression (COMPRESSION_LZFSE = 0x801)."""
    lib = ctypes.CDLL("/usr/lib/libcompression.dylib")
    lib.compression_decode_buffer.restype = ctypes.c_size_t
    lib.compression_decode_buffer.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                                              ctypes.c_size_t, ctypes.c_void_p, ctypes.c_int]
    capacity = max(size_hint, len(data) * 8)
    while True:
        out = ctypes.create_string_buffer(capacity)
        n = lib.compression_decode_buffer(out, capacity, data, len(data), None, 0x801)
        if n == 0:
            raise FetchError("LZFSE payload did not decode")
        if n < capacity:
            return out.raw[:n]
        capacity *= 2


def im4p_payload(data: bytes) -> bytes:
    """The payload of an unencrypted IM4P, decompressed if it is LZFSE ("bvx2")."""
    tag, start, _ = _der(data, 0)
    if tag != 0x30:
        raise FetchError("not an IM4P (no DER sequence)")
    pos = start
    fields = []
    for _ in range(4):
        tag, cstart, cend = _der(data, pos)
        fields.append((tag, data[cstart:cend]))
        pos = cend
    if fields[0] != (0x16, b"IM4P") or fields[3][0] != 0x04:
        raise FetchError("not an IM4P")
    payload = fields[3][1]
    if payload[:4] in (b"bvx2", b"bvx1", b"bvxn", b"bvx-"):
        return _lzfse_decode(payload, 1 << 20)
    return payload


def main(argv: list[str]) -> int:
    """`python3 fetch.py list URL` prints the members of a remote zip (for updating manifest.toml)."""
    if len(argv) == 3 and argv[1] == "list":
        remote = RemoteZip(argv[2])
        for info in remote.zip.infolist():
            print(f"{info.compress_type:2} {info.file_size:>12} {info.filename}")
        return 0
    print(main.__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
