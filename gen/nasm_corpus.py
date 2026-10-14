#!/usr/bin/env python3
"""Pack the nasm corpus images' .text into one file for the M1 decode checker.

    python3 gen/nasm_corpus.py --out build/nasm_corpus.bin build/nasm

The corpus (v86/tests/nasm/*.asm) is assembled and linked by `make nasm-translate` into
build/nasm/<t>.img -- bootable ELF images whose .text is the test's code (v86's own
link geometry, tests/nasm_run.py:LD_GEOMETRY).  The M1 decode checker
(src/cpu/uop_cache_selftest.c) walks THOSE bytes as a byte stream, so it needs them flat
and self-delimiting: this writes

    "NASM" <u32 count> then, per image, <u32 length> <length bytes>

in FILENAME order (the file is a pure function of the images, so it is reproducible).
Parsing the ELF here, rather than shelling out to objcopy, keeps the product a function of
its declared inputs alone.
"""

import argparse
import struct
import sys
from pathlib import Path


def text_section(path: Path):
    """The .text section's bytes of a 32-bit little-endian ELF, or None."""
    data = path.read_bytes()
    if len(data) < 0x34 or data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        return None
    e_shoff, = struct.unpack_from("<I", data, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    if e_shoff == 0 or e_shnum == 0 or e_shstrndx >= e_shnum:
        return None

    def header(i):
        off = e_shoff + i * e_shentsize
        name, _type, _flags, _addr, offset, size = struct.unpack_from("<IIIIII", data, off)
        return name, offset, size

    _, strtab, _ = header(e_shstrndx)
    for i in range(e_shnum):
        name, offset, size = header(i)
        end = data.find(b"\0", strtab + name)
        if data[strtab + name:end] == b".text":
            return data[offset:offset + size]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("workdir")
    args = ap.parse_args()

    images = sorted(Path(args.workdir).glob("*.img"))
    payload = []
    for img in images:
        text = text_section(img)
        if text is None or not text:
            print(f"[!] {img.name}: no .text, skipped", file=sys.stderr)
            continue
        payload.append(text)

    out = bytearray(b"NASM")
    out += struct.pack("<I", len(payload))
    for text in payload:
        out += struct.pack("<I", len(text))
        out += text
    Path(args.out).write_bytes(bytes(out))
    print(f"[+] {args.out}: {len(payload)} images, "
          f"{sum(len(t) for t in payload)} .text bytes")


if __name__ == "__main__":
    main()
