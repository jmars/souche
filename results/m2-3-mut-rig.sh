#!/bin/sh
#
# results/m2-3-mut-rig.sh -- M2-3's G2/G4: THE ASYMMETRY RIG.
#
# The brief: "at least three single-line mutations, each failing then restored-green".  Each
# mutation below is A SINGLE LINE of a file this unit wrote, and each is chosen so the thing
# it breaks is exactly what one of the unit's gates claims to test:
#
#   1. src/hw/ide.c -- get_disk_geometry's MBR arm disabled (the geometry then comes from the
#      fallback).  The claim: the geometry the IDENTIFY packet reports is DERIVED FROM THE
#      DISK'S OWN MBR (ide.js:977-1057), and the selftest's second disk -- 8 heads, 32
#      sectors -- is built so the two routes disagree.
#   2. src/hw/ide.c -- the IDENTIFY packet's total-sector field zeroed.  The claim: the
#      packet carries the sector count a kernel reads (words 60-61), which is what the disk
#      gate's own dmesg prints.
#   3. src/hw/ide.c -- the 32-bit data-port read's byte order broken.  The claim: the data
#      port returns the disk's bytes in the order a driver expects, at every width.
#   4. src/hw/ide.c -- the bus-master PRD walk's destination shifted.  The claim: READ DMA
#      lands the bytes where the PRD table says (the path a real kernel's libata uses).
#   5. src/hw/ide.c -- the ATAPI INQUIRY response's device type changed to "direct access".
#      The claim: the ATAPI command set answers what a kernel's SCSI layer reads.
#   6. tests/minibios.asm -- the firmware's SECOND chipset write (the secondary channel's
#      IDE-enable bit) deleted.  The claim of the unit's central finding: without it the
#      kernel's ata_piix refuses that channel and NEVER PROBES the ATAPI device, and the
#      guest's own dmesg loses its "ata2.00: ATAPI:" line.  (This one is measured by the
#      BOOT, not the selftest, because the firmware is what writes those bits.)
#
# THE LIVE TREE IS MUTATED AND RESTORED, one mutation at a time, and every touched file's
# sha256 is printed before and after: the rig's own restore is a claim, so it is checked.
# Nothing in this file is evidence -- results/m2-3-mut.txt is the raw capture.
#
# usage: sh results/m2-3-mut-rig.sh

set -u

cd "$(dirname "$0")/.."

KERNEL=tests/images/vmlinuz-i386
DISK=tests/images/disk.img
INITRD=tests/images/guest-initramfs.img
BOOT_LIMIT=1400000000
SELFTEST=build/hw_selftest
FILES="src/hw/ide.c src/hw/machine.c tests/minibios.asm"

echo "=== M2-3 asymmetry rig: each mutation alone, then restored ==="
echo
echo "--- the untouched tree, as the control ---"
sha256sum $FILES
make -s build/v86c $SELFTEST build/minibios-80m.bin > /dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }
./$SELFTEST > /tmp/m2_3_ctl_selftest.txt 2>&1
echo "control selftest rc=$? :: $(tail -1 /tmp/m2_3_ctl_selftest.txt)"
echo

mutate() # mutate <file> <old> <new>
{
    python3 - "$1" "$2" "$3" <<'PYEOF'
import sys
path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(path).read()
assert s.count(old) == 1, "the mutation's anchor is not unique in %s (%d)" % (path, s.count(old))
open(path, 'w').write(s.replace(old, new))
PYEOF
    [ $? -eq 0 ] || { echo "MUTATION FAILED TO APPLY: $1"; exit 1; }
}

rebuild() # EVERY mutation must be REBUILT before its run: a mutated source that is not
{         # rebuilt measures the PREVIOUS binary and reports "no effect" (MEASURED in M2-2:
          # the first version of that rig did exactly that for four mutations).
    make -s build/v86c $SELFTEST build/minibios-80m.bin > /dev/null 2>&1 || { echo "REBUILD FAILED"; exit 1; }
}

report_selftest() # report_selftest <label>
{
    make -s $SELFTEST > /dev/null 2>&1
    ./$SELFTEST > /tmp/m2_3_mut_selftest.txt 2>&1
    echo "$1: selftest rc=$? :: $(tail -1 /tmp/m2_3_mut_selftest.txt)"
    grep -m3 '^FAIL' /tmp/m2_3_mut_selftest.txt | sed 's/^/    /'
}

report_boot() # report_boot <label>: the guest's own dmesg, which is what the gate reads
{
    timeout 900 ./build/v86c --bzimage "$KERNEL" $BOOT_LIMIT --cmdline "console=ttyS0" \
        --hda "$DISK" --initrd "$INITRD" --bios build/minibios-80m.bin --memory 80 \
        > /tmp/m2_3_mut_boot.txt 2> /tmp/m2_3_mut_boot.err
    echo "$1: boot rc=$?, ata1=$(grep -c 'ata1: PATA' /tmp/m2_3_mut_boot.txt), ata2=$(grep -c 'ata2: PATA' /tmp/m2_3_mut_boot.txt), ata1.00=$(grep -c 'ata1.00:' /tmp/m2_3_mut_boot.txt), ata2.00=$(grep -c 'ata2.00:' /tmp/m2_3_mut_boot.txt)"
    grep -h 'ata[12][:.]0*' /tmp/m2_3_mut_boot.txt | sed 's/^/    /'
}

# ---------------------------------------------------------------- mutation 1
echo "=== M1: ide.c -- the MBR-derived geometry disabled ==="
cp src/hw/ide.c /tmp/m2_3_m1.c
mutate src/hw/ide.c '    if (have_mbr_geometry && mbr_heads <= 16) {' '    if (false && have_mbr_geometry && mbr_heads <= 16) { /* MUTATION */'
rebuild
report_selftest "M1 mutated"
cp /tmp/m2_3_m1.c src/hw/ide.c
rebuild
report_selftest "M1 restored from the saved copy"

# ---------------------------------------------------------------- mutation 2
echo "=== M2: ide.c -- the IDENTIFY packet's total-sector field zeroed ==="
cp src/hw/ide.c /tmp/m2_3_m2.c
mutate src/hw/ide.c '    W16(ide->sector_count);                  /* 60-61 total addressable sectors */' \
                    '    W16(0);                                 /* MUTATION: 60-61 */'
rebuild
report_selftest "M2 mutated"
cp /tmp/m2_3_m2.c src/hw/ide.c
rebuild
report_selftest "M2 restored from the saved copy"

# ---------------------------------------------------------------- mutation 3
echo "=== M3: ide.c -- the 32-bit data-port read's byte order broken ==="
cp src/hw/ide.c /tmp/m2_3_m3.c
mutate src/hw/ide.c '    return (int32_t)((uint32_t)ide->data[offset] | ((uint32_t)ide->data[offset + 1] << 8) |
                     ((uint32_t)ide->data[offset + 2] << 16) |
                     ((uint32_t)ide->data[offset + 3] << 24));' \
                    '    return (int32_t)((uint32_t)ide->data[offset + 1] | ((uint32_t)ide->data[offset] << 8) | /* MUTATION */
                     ((uint32_t)ide->data[offset + 2] << 16) |
                     ((uint32_t)ide->data[offset + 3] << 24));'
rebuild
report_selftest "M3 mutated"
cp /tmp/m2_3_m3.c src/hw/ide.c
rebuild
report_selftest "M3 restored from the saved copy"

# ---------------------------------------------------------------- mutation 4
echo "=== M4: ide.c -- the DMA PRD walk's destination shifted ==="
cp src/hw/ide.c /tmp/m2_3_m4.c
mutate src/hw/ide.c '            write_blob(guest_data + offset, prd_count, prd_addr);' \
                    '            write_blob(guest_data + offset, prd_count, prd_addr + 16); /* MUTATION */'
rebuild
report_selftest "M4 mutated"
cp /tmp/m2_3_m4.c src/hw/ide.c
rebuild
report_selftest "M4 restored from the saved copy"

# ---------------------------------------------------------------- mutation 5
echo "=== M5: ide.c -- the ATAPI INQUIRY's device type changed ==="
cp src/hw/ide.c /tmp/m2_3_m5.c
mutate src/hw/ide.c '            0x05, 0x80, 0x01, 0x31, 31, 0, 0, 0,' \
                    '            0x00, 0x80, 0x01, 0x31, 31, 0, 0, 0, /* MUTATION: device type 0 */'
rebuild
report_selftest "M5 mutated"
cp /tmp/m2_3_m5.c src/hw/ide.c
rebuild
report_selftest "M5 restored from the saved copy"

# ---------------------------------------------------------------- mutation 6
echo "=== M6: minibios.asm -- the SECOND chipset write deleted (the finding itself) ==="
cp tests/minibios.asm /tmp/m2_3_m6.asm
mutate tests/minibios.asm '	mov	dx, 0xCFE
	mov	ax, 0x8000		/* 0xCFC+2 -> config byte 0x43 |= 0x80 (IDE1 enable) */
	out	dx, ax' \
                         '	/* MUTATION: the secondary channel is never enabled */'
rebuild
report_boot "M6 mutated"
cp /tmp/m2_3_m6.asm tests/minibios.asm
rebuild
report_boot "M6 restored from the saved copy"

echo
echo "--- the restored tree ---"
sha256sum $FILES
