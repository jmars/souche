#!/bin/sh
#
# results/m2-4-mut-rig.sh -- M2-4's G2: THE ASYMMETRY RIG.
#
# The brief: "at least three single-line mutations, each failing then restored-green".  Each
# mutation below is A SINGLE LINE of a file this unit wrote, and each is chosen so the thing
# it breaks is exactly what one of the unit's claims tests:
#
#   1. src/hw/virtio.c -- the negotiated-feature MASK removed.  The claim: a bit the device
#      does not offer is masked out of driver_feature (virtio.js:362-374)
#      and is_feature_negotiated then reports the truth.
#   2. src/hw/virtio.c -- the FEATURES_OK refusal removed.  The claim: a driver that asks
#      for a feature the device has not got is not allowed to reach FEATURES_OK
#      (virtio.js:429-437).
#   3. src/hw/virtio.c -- the chain's written-length accounting zeroed.  The claim: the used
#      ring entry carries the number of bytes the device wrote into the chain
#      (virtio.js:1515, and push_reply's use of it).
#   4. src/hw/virtio.c -- the WRITE descriptor flag ignored.  The claim: the device
#      classifies a chain's buffers into the readable and writable sides by
#      VIRTQ_DESC_F_WRITE (virtio.js:1396-1411), which is what makes the host -> guest
#      direction land in the guest's buffer.
#   5. src/hw/virtio.c -- the used-ring flush's ISR bit changed.  The claim: a batch of
#      replies raises VIRTIO_ISR_QUEUE (virtio.js:1243), which is the bit the ISR register
#      returns and the read-and-clear the driver depends on.
#   6. src/hw/virtio.c -- the capability's bar-size rounding disabled.  The claim: each
#      capability's bar is sized to its own field list, rounded up to a power of two
#      (virtio.js:731-739) -- which is what a driver's size probe reads back.
#   7. src/hw/virtio.c -- the notify register's INDEX ignored.  The claim: the notification
#      capability's handler is selected by the register's OFFSET (virtio.js:627-636), so the
#      port->host path is driven by the guest's `out` to 0xB900 + 2*notify_offset.
#   8. tests/minibios.asm -- THE FIRMWARE's PIRQ-routing write deleted.  The claim of this
#      unit's second finding: without the firmware's two config writes the guest's virtio
#      driver gets NO interrupt (v86's raise_irq reads the PIIX3's PIRQ router, and
#      upstream's own config space leaves that byte 0, i.e. IRQ 0 -- the PIT's).  This one
#      is measured by the BOOT, not the selftest, because the firmware is what writes it.
#
# THE LIVE TREE IS MUTATED AND RESTORED, one mutation at a time, and every touched file's
# sha256 is printed before and after: the rig's own restore is a claim, so it is checked.
# Nothing in this file is evidence -- results/m2-4-mut.txt is the raw capture.
#
# usage: sh results/m2-4-mut-rig.sh

set -u

cd "$(dirname "$0")/.."

KERNEL=tests/images/vmlinuz-i386
INITRD=tests/images/guest-initramfs-virtio.img
IN=/tmp/vc-in.txt
BOOT_LIMIT=1400000000
SELFTEST=build/hw_selftest
FILES="src/hw/virtio.c src/hw/virtio_console.c tests/minibios.asm"

echo "=== M2-4 asymmetry rig: each mutation alone, then restored ==="
echo
echo "--- the untouched tree, as the control ---"
sha256sum $FILES
make -s $SELFTEST build/v86c build/minibios-80m.bin > /dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }
./$SELFTEST > /tmp/m2_4_ctl_selftest.txt 2>&1
echo "control selftest rc=$? :: $(tail -1 /tmp/m2_4_ctl_selftest.txt)"
timeout 900 ./build/v86c --bzimage "$KERNEL" $BOOT_LIMIT --cmdline "console=hvc0 console=ttyS0" \
    --initrd "$INITRD" --bios build/minibios-80m.bin --memory 80 \
    --virtio-console /tmp/m2_4_ctl_virtio.txt --virtio-console-in "$IN" \
    --serial /tmp/m2_4_ctl_serial.txt > /dev/null 2>/tmp/m2_4_ctl.err
echo "control boot rc=$? :: $(grep 'virtio console:' /tmp/m2_4_ctl.err)"
echo "control boot: virtio0 interrupts = $(grep -m1 'XT-PIC *virtio0' /tmp/m2_4_ctl_serial.txt | tr -s ' ' | cut -d' ' -f3)"
echo "control boot: read-back line = $(grep -c 'read back from /dev/hvc0' /tmp/m2_4_ctl_serial.txt)"
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
{         # rebuilt measures the PREVIOUS binary and reports "no effect" (MEASURED in M2-2).
    make -s $SELFTEST build/v86c build/minibios-80m.bin > /dev/null 2>&1 || { echo "REBUILD FAILED"; exit 1; }
}

report_selftest() # report_selftest <label>
{
    ./$SELFTEST > /tmp/m2_4_mut_selftest.txt 2>&1
    echo "$1: selftest rc=$? :: $(tail -1 /tmp/m2_4_mut_selftest.txt)"
    grep -m3 '^FAIL' /tmp/m2_4_mut_selftest.txt | sed 's/^/    /'
}

report_boot() # report_boot <label>: the guest's /proc/interrupts and the read-back, which
{             # is what the virtio part of the boot gate reads
    timeout 900 ./build/v86c --bzimage "$KERNEL" $BOOT_LIMIT --cmdline "console=hvc0 console=ttyS0" \
        --initrd "$INITRD" --bios build/minibios-80m.bin --memory 80 \
        --virtio-console /tmp/m2_4_mut_virtio.txt --virtio-console-in "$IN" \
        --serial /tmp/m2_4_mut_serial.txt > /dev/null 2>/tmp/m2_4_mut.err
    echo "$1: boot rc=$? :: $(grep 'virtio console:' /tmp/m2_4_mut.err)"
    echo "    virtio0 interrupts: $(grep -m1 'XT-PIC *virtio0' /tmp/m2_4_mut_serial.txt | tr -s ' ' | cut -d' ' -f3)"
    echo "    hvc0 console line: $(grep -c 'console \[hvc0\] enabled' /tmp/m2_4_mut_serial.txt)"
    echo "    read-back lines:   $(grep -c 'read back from /dev/hvc0' /tmp/m2_4_mut_serial.txt)"
}

# ---------------------------------------------------------------- mutation 1
echo "=== M1: virtio.c -- the negotiated-feature mask removed ==="
cp src/hw/virtio.c /tmp/m2_4_m1.c
mutate src/hw/virtio.c '        virtio->driver_feature[select] = (uint32_t)data & supported_feature;' \
                      '        virtio->driver_feature[select] = (uint32_t)data; /* MUTATION */'
rebuild
report_selftest "M1 mutated"
cp /tmp/m2_4_m1.c src/hw/virtio.c
rebuild
report_selftest "M1 restored from the saved copy"

# ---------------------------------------------------------------- mutation 2
echo "=== M2: virtio.c -- the FEATURES_OK refusal removed ==="
cp src/hw/virtio.c /tmp/m2_4_m2.c
mutate src/hw/virtio.c '        data &= ~VIRTIO_STATUS_FEATURES_OK;' \
                      '        data |= VIRTIO_STATUS_FEATURES_OK; /* MUTATION */'
rebuild
report_selftest "M2 mutated"
cp /tmp/m2_4_m2.c src/hw/virtio.c
rebuild
report_selftest "M2 restored from the saved copy"

# ---------------------------------------------------------------- mutation 3
echo "=== M3: virtio.c -- the chain's written-length accounting zeroed ==="
cp src/hw/virtio.c /tmp/m2_4_m3.c
mutate src/hw/virtio.c '    chain->length_written += src_offset;' \
                      '    chain->length_written += 0; /* MUTATION */'
rebuild
report_selftest "M3 mutated"
cp /tmp/m2_4_m3.c src/hw/virtio.c
rebuild
report_selftest "M3 restored from the saved copy"

# ---------------------------------------------------------------- mutation 4
echo "=== M4: virtio.c -- the WRITE descriptor flag ignored ==="
cp src/hw/virtio.c /tmp/m2_4_m4.c
mutate src/hw/virtio.c '        if (desc.flags & VIRTQ_DESC_F_WRITE) {' \
                      '        if (0 && (desc.flags & VIRTQ_DESC_F_WRITE)) { /* MUTATION */'
rebuild
report_selftest "M4 mutated"
cp /tmp/m2_4_m4.c src/hw/virtio.c
rebuild
report_selftest "M4 restored from the saved copy"

# ---------------------------------------------------------------- mutation 5
echo "=== M5: virtio.c -- the flush's ISR bit changed ==="
cp src/hw/virtio.c /tmp/m2_4_m5.c
mutate src/hw/virtio.c '            virtio_raise_irq(queue->virtio, VIRTIO_ISR_QUEUE);' \
                      '            virtio_raise_irq(queue->virtio, VIRTIO_ISR_DEVICE_CFG); /* MUTATION */'
rebuild
report_selftest "M5 mutated"
cp /tmp/m2_4_m5.c src/hw/virtio.c
rebuild
report_selftest "M5 restored from the saved copy"

# ---------------------------------------------------------------- mutation 6
echo "=== M6: virtio.c -- the bar-size rounding disabled ==="
cp src/hw/virtio.c /tmp/m2_4_m6.c
mutate src/hw/virtio.c '        bar_size = bar_size < 16 ? 16 : 1 << (virtio_int_log2((uint32_t)(bar_size - 1)) + 1);' \
                      '        bar_size = 16; /* MUTATION */'
rebuild
report_selftest "M6 mutated"
cp /tmp/m2_4_m6.c src/hw/virtio.c
rebuild
report_selftest "M6 restored from the saved copy"

# ---------------------------------------------------------------- mutation 7
echo "=== M7: virtio.c -- the notify register's index ignored ==="
cp src/hw/virtio.c /tmp/m2_4_m7.c
mutate src/hw/virtio.c '    int32_t index = slot->index;' \
                      '    int32_t index = 0; /* MUTATION */'
rebuild
report_selftest "M7 mutated"
cp /tmp/m2_4_m7.c src/hw/virtio.c
rebuild
report_selftest "M7 restored from the saved copy"

# ---------------------------------------------------------------- mutation 8
echo "=== M8: tests/minibios.asm -- the firmware's PIRQ-routing write deleted ==="
cp tests/minibios.asm /tmp/m2_4_m8.asm
mutate tests/minibios.asm '	mov	al, VIRTIO_IRQ
	out	dx, al
1:
	ret' \
                         '	mov	al, VIRTIO_IRQ
	/* MUTATION: the PIRQ router write is gone */
1:
	ret'
rebuild
report_boot "M8 mutated"
cp /tmp/m2_4_m8.asm tests/minibios.asm
rebuild
report_boot "M8 restored from the saved copy"

echo
echo "--- the tree after the rig (must equal the control's sha256 lines above) ---"
sha256sum $FILES
