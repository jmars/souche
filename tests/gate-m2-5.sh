#!/bin/sh
# tests/gate-m2-5.sh -- SLICE 1's gates: THE SHARED v86 GUEST KERNEL (HANDOFF=v86guest).
#
# THE GATE IS AN ASYMMETRY: one machine, one initramfs, one patterned disk, booted with
#   1b  qemu-system-x86_64 + KVM      (the fast pre-check: seconds, before the
#                                      interpreter runs that cost hours)
#   1a  build/v86c        (the C port) and
#       node oracle/run-boot.mjs (the pinned JS reference)   <- the two-leg oracle
# and then the SAME 1a machine booted with the PINNED LEAN CONTROL kernel
# (tests/images/vmlinuz-i386-lean-control = fx-demo's web/bzImage, whose sha256 is in
# fx-demo/SHA256SUMS) -- which must FAIL every device assertion, because it has no PCI,
# no ATA and no virtio.  That is the fail-before, and it needs no reconstruction: it is
# the kernel the browser lane ships today.
#
# usage: sh tests/gate-m2-5.sh [pin|qemu|souche|lean|all]
#   pin     the sha256 pins of every input (fast)
#   qemu    gate 1b
#   souche  gate 1a's two legs
#   lean    the failure arm (both legs)
#   all     qemu + souche + lean
#
# Raw evidence lands in results/m2-5-*.txt; the write-up is results/m2-5-baliveau-kernel.md.

set -u
cd "$(dirname "$0")/.."

KERNEL=tests/images/vmlinuz-i386-v86guest
LEAN=tests/images/vmlinuz-i386-lean-control
INITRD=tests/images/guest-initramfs-v86gate.img
DISK=tests/images/disk.img
VIN=tests/v86gate-virtio-in.txt
# THE CMDLINE, AND WHY THOSE TWO FLAGS ARE PART OF THE MACHINE (MEASURED, not taste):
# both harnesses stop the machine at the FIRST HLT (M0's stop condition, kept), and a
# UP 6.12 guest booted by v86 hits an idle HLT MID-BOOT without them -- the emulated TSC
# jumps (the guest's own line: "sched_clock: Marking stable (61459108999, 281323000)->
# (65858044000, -4117612001)"), the tick is then not programmed, and the run ends before
# userspace.  The C port AND the pinned JS reference both stopped with in_hlt=1 at
# eip=0xc11db37e, which the kernel's own System.map resolves to default_idle+0x12 -- the
# instruction after default_idle's idle hlt -- so the stop is the machine idling, not a
# port-specific code path.  (CORRECTED round 2: round 1 called that eip an "identical
# point"; it is the SHARED idle address, and the two legs' serials stop at DIFFERENT boot
# points there -- C 109 lines to the "sched_clock" line, reference 48 lines to
# "Mountpoint-cache".  Raw transcripts: results/m2-5-probe-*.)
#   tsc=unstable  the semantic fix: stop using the emulated TSC as the clocksource
#                 (this is the flag the plan's own slice-3 cmdline already carries)
#   idle=poll     what the HLT stop condition requires: the guest must not idle into a
#                 HLT that has no pending interrupt.  MEASURED to be necessary on BOTH
#                 legs -- tsc=unstable ALONE still stopped the JS leg at the idle HLT.
CMD="console=hvc0 console=ttyS0 tsc=unstable idle=poll"
LIMIT=3000000000
MEM=80
FW=build/minibios-80m.bin
R=results

# The pins.  MD5 is m2-3's own pin (results/m2-3-ide.md): the md5 of the patterned disk's
# first 64 sectors, a pure function of tests/mkdisk.py's pattern -- the same number the
# 6.6 Alpine kernel's libata produced on both machines there, so it is a cross-kernel pin.
MD5=87d5ec3151caf04d90236947e6f09874
SHA_KERNEL=f0278a8f2893eb93a8c857f3f2d5a589a5b9aa6a45033171bc5411fc10f658cd
SHA_LEAN=25be0d5358d9445712f6d04e59a7be9133acbd3f69ad0720a69e43d5ba844b3d
SHA_INITRD=d3a9e3eca9547577f9f62f2ed67919dfe060047114b0a781ecd37d1b2ea9c900
SHA_DISK=f83dba09f75d2c0b205aa7f24be1dec2fb9e32fe9d9d7725205408a8e3f0758f

fails=0
pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; fails=$((fails + 1)); }
# Every assertion reads the transcript with CR stripped: the guest's console is a TTY
# and its output is CRLF ("^M$" on every line, MEASURED), so an anchored pattern would
# otherwise fail on a line that is in fact exactly as expected.  This is m2-2's
# normalization (timestamps stripped) extended by the one other thing the TTY adds.
want()     { if tr -d '\r' < "$1" | grep -qE "$2"; then pass "$3"; else fail "$3 (no /$2/ in $1)"; fi; }
want_not() { if tr -d '\r' < "$1" | grep -qE "$2"; then fail "$3 (found /$2/ in $1)"; else pass "$3"; fi; }

pin_check() {
    echo "### PINS (the inputs this gate is a claim about)"
    for pair in "$KERNEL $SHA_KERNEL" "$LEAN $SHA_LEAN" "$INITRD $SHA_INITRD" "$DISK $SHA_DISK"; do
        set -- $pair
        got=$(sha256sum "$1" | cut -d' ' -f1)
        if [ "$got" = "$2" ]; then pass "$1 sha256 $got"; else fail "$1 sha256 $got != $2"; fi
    done
    got=$(dd if=$DISK bs=512 count=64 2>/dev/null | md5sum | cut -d' ' -f1)
    if [ "$got" = "$MD5" ]; then pass "patterned disk, md5 of first 64 sectors $got"; else fail "disk md5 $got != $MD5"; fi
    if [ -x build/v86c ]; then pass "build/v86c present"; else fail "build/v86c missing (make)"; fi
    if [ -f $FW ]; then pass "$FW present"; else fail "$FW missing (make minibios-mem)"; fi
}

# ------------------------------------------------------------------ gate 1b: qemu/KVM
gate_qemu() {
    echo "### GATE 1b: qemu-system-x86_64 + KVM -- the fast pre-check for the SAME lines"
    echo "\$ qemu-system-x86_64 -machine pc -m $MEM -enable-kvm -no-reboot -display none \\"
    echo "      -kernel $KERNEL -initrd $INITRD -append '$CMD' \\"
    echo "      -drive file=$DISK,format=raw,if=ide \\"
    echo "      -device virtio-serial-pci -device virtconsole,chardev=vc0 \\"
    echo "      -chardev file,id=vc0,path=$R/m2-5-qemu-virtio.txt -serial file:$R/m2-5-qemu-serial.txt"
    # The host end of the virtio console is a FILE here, so there is nothing to answer the
    # sentinel with: the guest's read is bounded (-t) and reports an empty payload.  The
    # read-back itself is gate 1a's (both souche legs), which is where the host can answer.
    timeout 120 qemu-system-x86_64 -machine pc -m $MEM -enable-kvm -no-reboot -display none \
        -kernel $KERNEL -initrd $INITRD -append "$CMD" \
        -drive file=$DISK,format=raw,if=ide \
        -device virtio-serial-pci -device virtconsole,chardev=vc0 \
        -chardev file,id=vc0,path=$R/m2-5-qemu-virtio.txt \
        -serial file:$R/m2-5-qemu-serial.txt
    echo "qemu rc=$? (124 = the timeout stopped an idle, halted machine, which is the"
    echo "          expected end: the guest halts itself at 'GUEST-INIT: end')"
    echo "--- the kernel's own lines (serial)"
    grep -E "ata[0-9]|libata|scsi [0-9]|sd [0-9]:|sda|virtio|hvc|console \[|GUEST-INIT" \
        $R/m2-5-qemu-serial.txt | sed 's/^/    /'
    echo "--- assertions (the serial transcript)"
    want $R/m2-5-qemu-serial.txt 'sd 0:0:0:0: \[sda\]' "the kernel's SCSI disk attach (sda)"
    want $R/m2-5-qemu-serial.txt "GUEST-INIT: /dev/sda: present" "/dev/sda is a block device"
    want $R/m2-5-qemu-serial.txt "GUEST-INIT: /dev/sda1: present" "/dev/sda1 (the MBR's partition)"
    want $R/m2-5-qemu-serial.txt "md5 of /dev/sda's first 64 sectors: $MD5" "the pinned disk md5"
    want $R/m2-5-qemu-serial.txt 'console \[hvc0\] enabled' "the kernel enables the hvc0 console"
    want $R/m2-5-qemu-serial.txt 'virtio0' "the virtio device is on an interrupt line"
    want $R/m2-5-qemu-serial.txt "GUEST-INIT: end" "the guest init completed"
    echo "--- the same kernel's log ARRIVING THROUGH the virtio console ($(wc -c < $R/m2-5-qemu-virtio.txt) bytes)"
    want $R/m2-5-qemu-virtio.txt 'console \[hvc0\] enabled' "hvc0 enabled, seen on the virtio stream"
    want $R/m2-5-qemu-virtio.txt 'sd 0:0:0:0: \[sda\]' "the sda line came out of the virtio console"
    want_not $R/m2-5-qemu-serial.txt 'Kernel panic' "no kernel panic"
}

# ----------------------------------------------------------------- gate 1a: two legs
leg_c() {
    echo "### GATE 1a, C LEG: build/v86c"
    echo "\$ ./build/v86c --bzimage $KERNEL $LIMIT --cmdline '$CMD' --hda $DISK \\"
    echo "      --initrd $INITRD --bios $FW --memory $MEM \\"
    echo "      --virtio-console $R/m2-5-c-virtio.txt --virtio-console-in $VIN \\"
    echo "      --serial $R/m2-5-c-serial.txt"
    timeout 5400 ./build/v86c --bzimage $KERNEL $LIMIT --cmdline "$CMD" --hda $DISK \
        --initrd $INITRD --bios $FW --memory $MEM \
        --virtio-console $R/m2-5-c-virtio.txt --virtio-console-in $VIN \
        --serial $R/m2-5-c-serial.txt 2> $R/m2-5-c-stderr.txt
    echo "port rc=$?"
    tail -3 $R/m2-5-c-stderr.txt | sed 's/^/    /'
}

leg_js() {
    echo "### GATE 1a, REFERENCE LEG: node oracle/run-boot.mjs (the pinned v86)"
    echo "\$ node oracle/run-boot.mjs $KERNEL $FW $LIMIT '$CMD' --hda $DISK \\"
    echo "      --initrd $INITRD --memory $MEM --virtio-console $R/m2-5-js-virtio.txt \\"
    echo "      --virtio-console-in $VIN > $R/m2-5-js-serial.txt"
    timeout 7200 node oracle/run-boot.mjs $KERNEL $FW $LIMIT "$CMD" --hda $DISK \
        --initrd $INITRD --memory $MEM --virtio-console $R/m2-5-js-virtio.txt \
        --virtio-console-in $VIN > $R/m2-5-js-serial.txt 2> $R/m2-5-js-stderr.txt
    echo "reference rc=$?"
    tail -3 $R/m2-5-js-stderr.txt | sed 's/^/    /'
}

gate_souche() {
    leg_c
    echo "--- assertions (the C leg)"
    want $R/m2-5-c-serial.txt 'ata1\.00: ATA-' "libata identified the disk (ata1.00)"
    want $R/m2-5-c-serial.txt 'sd 0:0:0:0: \[sda\]' "the kernel's SCSI disk attach (sda)"
    want $R/m2-5-c-serial.txt "GUEST-INIT: /dev/sda: present" "/dev/sda is a block device"
    want $R/m2-5-c-serial.txt "GUEST-INIT: /dev/sda1: present" "/dev/sda1 (the MBR's partition)"
    want $R/m2-5-c-serial.txt "md5 of /dev/sda's first 64 sectors: $MD5" "the pinned disk md5, read through ATA"
    want $R/m2-5-c-serial.txt 'GUEST-INIT: /proc/filesystems ext4 entries: 1' "ext4 is registered (the chain is =y)"
    want $R/m2-5-c-serial.txt 'console \[hvc0\] enabled' "the kernel enables the hvc0 console"
    want $R/m2-5-c-serial.txt 'XT-PIC +virtio0' "virtio0 has an XT-PIC interrupt line"
    want $R/m2-5-c-serial.txt 'read back from /dev/hvc0: \[VIRTIO-INPUT16\]' "the host->guest read-back through /dev/hvc0"
    want $R/m2-5-c-serial.txt "GUEST-INIT: end" "the guest init completed"
    want $R/m2-5-c-serial.txt 'reboot: System halted' "the guest halted ITSELF (the deterministic end)"
    want_not $R/m2-5-c-serial.txt 'Kernel panic' "no kernel panic"
    echo "--- assertions (the C leg's virtio console stream)"
    want $R/m2-5-c-virtio.txt 'console \[hvc0\] enabled' "hvc0 enabled, seen on the virtio stream"
    want $R/m2-5-c-virtio.txt 'sd 0:0:0:0: \[sda\]' "the sda line came out of the virtio console"

    leg_js
    echo "--- assertions (the reference leg)"
    want $R/m2-5-js-serial.txt 'ata1\.00: ATA-' "libata identified the disk (ata1.00)"
    want $R/m2-5-js-serial.txt 'sd 0:0:0:0: \[sda\]' "the kernel's SCSI disk attach (sda)"
    want $R/m2-5-js-serial.txt "GUEST-INIT: /dev/sda1: present" "/dev/sda1 (the MBR's partition)"
    want $R/m2-5-js-serial.txt "md5 of /dev/sda's first 64 sectors: $MD5" "the pinned disk md5, read through ATA"
    want $R/m2-5-js-serial.txt 'console \[hvc0\] enabled' "the kernel enables the hvc0 console"
    want $R/m2-5-js-serial.txt 'XT-PIC +virtio0' "virtio0 has an XT-PIC interrupt line"
    want $R/m2-5-js-serial.txt 'read back from /dev/hvc0: \[VIRTIO-INPUT16\]' "the host->guest read-back through /dev/hvc0"
    want $R/m2-5-js-serial.txt "GUEST-INIT: end" "the guest init completed"
    want $R/m2-5-js-serial.txt 'reboot: System halted' "the guest halted ITSELF (the deterministic end)"
    want_not $R/m2-5-js-serial.txt 'Kernel panic' "no kernel panic"

    echo "--- THE TWO LEGS' DEVICE LINES (timestamps stripped, m2-2's normalization)"
    sed 's/^\[ *[0-9.]*\]//' $R/m2-5-js-serial.txt > $R/m2-5-js-serial.norm.txt
    sed 's/^\[ *[0-9.]*\]//' $R/m2-5-c-serial.txt > $R/m2-5-c-serial.norm.txt
    diff $R/m2-5-js-serial.norm.txt $R/m2-5-c-serial.norm.txt > $R/m2-5-leg-diff.txt
    echo "the leg diff: $(wc -l < $R/m2-5-leg-diff.txt) lines (m2-3 named the v86 VGA model and"
    echo "the clock as this gate's only expected divergences)"
    sed 's/^/    /' $R/m2-5-leg-diff.txt | head -40
}

# --------------------------------------------------------- the failure arm (lean)
gate_lean() {
    echo "### FAILURE ARM: THE SAME GATE WITH THE PINNED LEAN CONTROL KERNEL"
    echo "### (fx-demo's web/bzImage -- no PCI, no ATA, no virtio: it CANNOT print this gate's lines)"
    echo "\$ ./build/v86c --bzimage $LEAN $LIMIT --cmdline '$CMD' --hda $DISK \\"
    echo "      --initrd $INITRD --bios $FW --memory $MEM \\"
    echo "      --virtio-console $R/m2-5-leanc-virtio.txt --virtio-console-in $VIN \\"
    echo "      --serial $R/m2-5-leanc-serial.txt"
    timeout 5400 ./build/v86c --bzimage $LEAN $LIMIT --cmdline "$CMD" --hda $DISK \
        --initrd $INITRD --bios $FW --memory $MEM \
        --virtio-console $R/m2-5-leanc-virtio.txt --virtio-console-in $VIN \
        --serial $R/m2-5-leanc-serial.txt 2> $R/m2-5-leanc-stderr.txt
    echo "port rc=$?"
    echo "--- the lean control's own lines"
    grep -E "ata[0-9]|libata|sd [0-9]:|sda|virtio|hvc|console \[|GUEST-INIT" \
        $R/m2-5-leanc-serial.txt | sed 's/^/    /'
    echo "--- assertions: the SAME gate lines must be ABSENT (the fail-before)"
    want_not $R/m2-5-leanc-serial.txt 'ata1\.00: ATA-' "no libata line"
    want_not $R/m2-5-leanc-serial.txt 'sd 0:0:0:0: \[sda\]' "no SCSI disk"
    want_not $R/m2-5-leanc-serial.txt "GUEST-INIT: /dev/sda: present" "/dev/sda is NOT a block device"
    want_not $R/m2-5-leanc-serial.txt "$MD5" "the patterned disk is never read"
    want_not $R/m2-5-leanc-serial.txt 'console \[hvc0\] enabled' "no hvc0 console"
    want_not $R/m2-5-leanc-serial.txt 'XT-PIC +virtio0' "no virtio interrupt line"
    want_not $R/m2-5-leanc-serial.txt 'read back from /dev/hvc0: \[VIRTIO-INPUT16\]' "no read-back through hvc0"
    echo "--- and the SAME init ran (the negative is a measurement, not a silence)"
    want $R/m2-5-leanc-serial.txt "GUEST-INIT: /dev/sda: absent" "the gate's own init reached the disk probe"
    want $R/m2-5-leanc-serial.txt "GUEST-INIT: /dev/hvc\*: *$" "the gate's own init found no hvc device"
    want $R/m2-5-leanc-serial.txt "GUEST-INIT: end" "the gate's own init completed"

    echo "### FAILURE ARM, REFERENCE LEG"
    echo "\$ node oracle/run-boot.mjs $LEAN $FW $LIMIT '$CMD' --hda $DISK --initrd $INITRD \\"
    echo "      --memory $MEM --virtio-console $R/m2-5-leanjs-virtio.txt \\"
    echo "      --virtio-console-in $VIN > $R/m2-5-leanjs-serial.txt"
    timeout 7200 node oracle/run-boot.mjs $LEAN $FW $LIMIT "$CMD" --hda $DISK \
        --initrd $INITRD --memory $MEM --virtio-console $R/m2-5-leanjs-virtio.txt \
        --virtio-console-in $VIN > $R/m2-5-leanjs-serial.txt 2> $R/m2-5-leanjs-stderr.txt
    echo "reference rc=$?"
    want_not $R/m2-5-leanjs-serial.txt 'sd 0:0:0:0: \[sda\]' "no SCSI disk (reference leg)"
    want_not $R/m2-5-leanjs-serial.txt "$MD5" "the patterned disk is never read (reference leg)"
    want_not $R/m2-5-leanjs-serial.txt 'console \[hvc0\] enabled' "no hvc0 console (reference leg)"
    want $R/m2-5-leanjs-serial.txt "GUEST-INIT: /dev/sda: absent" "the same init ran (reference leg)"
}

case "${1:-all}" in
    pin)    pin_check ;;
    qemu)   pin_check; gate_qemu ;;
    souche) pin_check; gate_souche ;;
    lean)   pin_check; gate_lean ;;
    all)    pin_check; gate_qemu; gate_souche; gate_lean ;;
    *)      echo "usage: sh tests/gate-m2-5.sh [pin|qemu|souche|lean|all]"; exit 2 ;;
esac

echo
echo "### m2-5 slice-1 gate: $fails FAILING assertion(s)"
[ "$fails" = 0 ]
