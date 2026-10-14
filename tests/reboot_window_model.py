#!/usr/bin/env python3
"""reboot_window_model.py -- the independent model of tests/reboot2x.asm's hashed
window, for tests/gate-reboot-1.sh.

Reconstructs the ENTIRE 4 KiB window (0x101000..0x102000) a two-boot run must leave
-- every word the image writes is documented in the image's header -- and requires
its sha256 to equal the transcript's memory_window.sha256.  Anything ELSE that
touched the window during the two boots (an interrupt frame under the stack top at
0x102000, a device writing through a hook, an unmodelled guest store) breaks the
match: the model is the canary for unaccounted writes, not a checksum of itself.

usage: reboot_window_model.py <trigger-id 1|2> <transcript-json>
"""
import hashlib
import json
import sys

WINDOW = 0x101000
SIZE = 0x1000


def main() -> int:
    trigid = int(sys.argv[1])
    transcript = json.load(open(sys.argv[2]))
    win = bytearray(SIZE)

    def put(addr: int, data: bytes) -> None:
        off = addr - WINDOW
        win[off : off + len(data)] = data

    marker = (0x31415926).to_bytes(4, "little")
    put(0x101000, marker)                    # boot 1's marker
    put(0x101004, marker)                    # read back by boot 2
    put(0x101008, (0x600D600D).to_bytes(4, "little"))  # the verdict
    put(0x10100C, (0).to_bytes(4, "little"))           # ECX at boot-2 entry
    put(0x101010, (0x05).to_bytes(4, "little"))        # the PS/2 command register
    put(0x101014, (0).to_bytes(2, "little"))           # the PIT latched count
    put(0x101018, (0x08).to_bytes(4, "little"))        # CS at boot-2 entry
    put(0x10101C, (1).to_bytes(4, "little"))           # CR0 at boot-2 entry
    put(0x101020, trigid.to_bytes(4, "little"))        # the trigger site id
    put(0x101024, trigid.to_bytes(4, "little"))        # ... read back by boot 2
    put(0x101800, (2).to_bytes(4, "little"))           # the boot count

    want = hashlib.sha256(bytes(win)).hexdigest()
    got = transcript["memory_window"]["sha256"]
    if got != want:
        print(f"window mismatch: model {want} transcript {got}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
