#!/usr/bin/env python3
"""tests/mkdisk.py -- the ATA hard disk image the IDE unit's gates use.

    python3 tests/mkdisk.py <output> [sectors]

Writes a deterministic raw disk image: an MBR with one partition, and every sector
filled with a pattern that is a pure function of its own sector number, so a READ
SECTORS result can be checked byte for byte from either side (this file, the C selftest,
or a guest that reads /dev/sda and hashes it).

WHY THIS IMAGE AND NOT A REAL FILESYSTEM: the gates are about the CONTROLLER.  What a
kernel must be able to do is (a) read the IDENTIFY data, (b) read sector 0 and parse the
partition table, (c) read data sectors and get the bytes back.  The MBR makes (b)
observable in the kernel's own log ("sda: sda1") and the pattern makes (c) observable
anywhere.  Formatting a filesystem would add a second thing that can fail without
adding a fact about the controller.

THE PATTERN (also implemented in src/hw/hw_selftest.c, which asserts the bytes a READ
SECTORS leaves in guest memory):

    byte j of sector n  ==  (n * 31 + j * 17 + 0x5A) & 0xFF

and sector 0 is the MBR instead of the pattern (its first 446 bytes are zero, then the
partition table, then 0x55AA).

THE GEOMETRY the MBR declares is the one v86's get_disk_geometry derives from it
(cpu.js/ide.js:977-1057): end_head = 15 (16 heads) and end_sector = 63, so
`heads <= 16` selects the MBR's own geometry (TRANSLATION_NONE) instead of the
"standard" 16/63 fallback -- both give 16 heads and 63 sectors here, but the MBR route
is the one where the numbers came from the disk.
"""

import sys

SECTOR = 512
HEADS = 16
SECTORS_PER_TRACK = 63
START_LBA = 2048       # conventional first partition offset
DEFAULT_SECTORS = 8192  # 4 MiB


def pattern_byte(sector, offset):
    return (sector * 31 + offset * 17 + 0x5A) & 0xFF


def sector_bytes(n):
    return bytes(pattern_byte(n, j) for j in range(SECTOR))


def mbr(total_sectors):
    image = bytearray(SECTOR)
    cylinders = total_sectors // (HEADS * SECTORS_PER_TRACK)
    end_cyl = max(cylinders - 1, 0)
    entry = 0x1BE
    image[entry + 0] = 0x80                      # bootable
    image[entry + 1] = 0x00                      # CHS start: head 0
    image[entry + 2] = 0x01                      # CHS start: sector 1, cyl 0
    image[entry + 3] = 0x00
    image[entry + 4] = 0x83                      # Linux
    image[entry + 5] = HEADS - 1                 # CHS end: head 15 (v86 reads this)
    image[entry + 6] = SECTORS_PER_TRACK | ((end_cyl >> 8) << 6)
    image[entry + 7] = end_cyl & 0xFF
    length = total_sectors - START_LBA
    image[entry + 12:entry + 16] = START_LBA.to_bytes(4, "little")
    image[entry + 16:entry + 20] = length.to_bytes(4, "little")
    image[0x1FE] = 0x55
    image[0x1FF] = 0xAA
    return bytes(image)


def build(total_sectors):
    out = bytearray()
    for n in range(total_sectors):
        out += mbr(total_sectors) if n == 0 else sector_bytes(n)
    return bytes(out)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    total = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_SECTORS
    data = build(total)
    with open(path, "wb") as f:
        f.write(data)
    print("%s: %d bytes, %d sectors, geometry %d/%d/%d, partition at LBA %d (%d sectors)"
          % (path, len(data), total, total // (HEADS * SECTORS_PER_TRACK), HEADS,
             SECTORS_PER_TRACK, START_LBA, total - START_LBA))
    return 0


if __name__ == "__main__":
    sys.exit(main())
