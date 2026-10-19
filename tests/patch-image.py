#!/usr/bin/env python3
"""tests/patch-image.py -- one same-length byte patch in an image, with the uniqueness assertion.

    python3 tests/patch-image.py <image> <old-hex> <new-hex>

WHY THIS EXISTS (results/x86-64-s5.md ss4.6): the x86-64 corpus's page tables are BUILT AT
RUNTIME by the image's own stub, so a mutation probe for the 4-level walk cannot patch a table
in the file -- it patches the STUB'S IMMEDIATE (the `mov dword ptr [0x104000], 0x105003` that
installs PML4[0], for instance).  The pattern must therefore be an entire instruction, and it
must occur EXACTLY ONCE: a stub whose shape changed would otherwise mutate nothing and the
probe would pass while measuring nothing, which is the class this tree's control arms exist to
catch.  A missing or ambiguous pattern is an ERROR (exit 1), never a silent no-op.
"""
import sys


def main(argv):
    if len(argv) != 4:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    path, old_hex, new_hex = argv[1], argv[2], argv[3]
    old, new = bytes.fromhex(old_hex), bytes.fromhex(new_hex)
    if len(old) != len(new):
        print("patch-image: the replacement must be the same length", file=sys.stderr)
        return 2
    data = bytearray(open(path, "rb").read())
    occurrences = data.count(old)
    if occurrences != 1:
        print("patch-image: pattern %s occurs %d times in %s (must be exactly 1)"
              % (old.hex(), occurrences, path), file=sys.stderr)
        return 1
    at = data.index(old)
    data[at:at + len(old)] = new
    open(path, "wb").write(data)
    print("patched %s at 0x%X: %s -> %s" % (path, at, old.hex(), new.hex()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
