#!/bin/sh
#
# results/m2-4-gates.sh -- M2-4's G3/G4 capture: every gate the brief names, with its exit
# code and its own summary line.  Nothing here decides anything -- results/m2-4-gates.txt is
# the raw capture.
#
# G3: make (clean), make selftest (all three suites), make nasm (118/118),
#     make transcript-diff (EMPTY), make oracle, make determinism
# G4: the M2-2 boot (no --hda, 64 MiB, the M2-2 firmware) and the M2-3 boot (--hda + initrd,
#     80 MiB), both re-run on the port, plus the M2-2 C-vs-reference transcript diff.
#
# usage: sh results/m2-4-gates.sh

set -u
cd "$(dirname "$0")/.."

KERNEL=tests/images/vmlinuz-i386

run() # run <label> <command...>
{
    label="$1"; shift
    echo "=== $label"
    "$@" > /tmp/m2_4_gate_out.txt 2>&1
    rc=$?
    echo "\$ $*"
    echo "rc=$rc"
    tail -6 /tmp/m2_4_gate_out.txt
    echo
}

echo "### G3"
run "make clean && make" sh -c "make clean && make"
run "make selftest (cpu_selftest + cpu_m1c_selftest + hw_selftest)" make selftest
run "make oracle" make oracle
run "make transcript-diff (the diff must be EMPTY)" make transcript-diff
run "make determinism" make determinism
run "make nasm (the differential corpus)" make nasm
grep -h "Passed\|passed" /tmp/m2_4_gate_out.txt | tail -1
echo

make -s minibios minibios-mem > /dev/null 2>&1 || { echo "FIRMWARE BUILD FAILED"; exit 1; }
ls -l build/minibios.bin build/minibios-80m.bin

echo "### G4a: THE M2-2 BOOT -- no --hda, 64 MiB, the M2-2 firmware, console=ttyS0"
timeout 1200 ./build/v86c --bzimage "$KERNEL" 1000000000 --cmdline "console=ttyS0" \
    --bios build/minibios.bin > results/m2-4-g4a-c.txt 2>/tmp/m2_4_g4a.err
echo "port rc=$?"
tail -2 /tmp/m2_4_g4a.err
echo "port: panic=$(grep -c 'Kernel panic' results/m2-4-g4a-c.txt) e820=$(grep -c 'BIOS-e820' results/m2-4-g4a-c.txt) lines=$(wc -l < results/m2-4-g4a-c.txt)"
echo
timeout 2400 node oracle/run-boot.mjs "$KERNEL" build/minibios.bin 1000000000 "console=ttyS0" \
    > results/m2-4-g4a-js.txt 2>/tmp/m2_4_g4a_js.err
echo "reference rc=$?"
tail -1 /tmp/m2_4_g4a_js.err
echo "reference: panic=$(grep -c 'Kernel panic' results/m2-4-g4a-js.txt) e820=$(grep -c 'BIOS-e820' results/m2-4-g4a-js.txt) lines=$(wc -l < results/m2-4-g4a-js.txt)"
echo
echo "--- the M2-2 C-versus-reference diff (timestamps stripped; the VGA and the clock are the"
echo "--- divergences M2-2/M2-3 already named)"
diff <(sed 's/^\[ *[0-9.]*\]//' results/m2-4-g4a-js.txt) \
     <(sed 's/^\[ *[0-9.]*\]//' results/m2-4-g4a-c.txt) | head -30
echo

echo "### G4b: THE M2-3 BOOT -- --hda + initrd, 80 MiB: the disk must still attach"
timeout 1800 ./build/v86c --bzimage "$KERNEL" 1400000000 --cmdline "console=ttyS0" \
    --hda tests/images/disk.img --initrd tests/images/guest-initramfs.img \
    --bios build/minibios-80m.bin --memory 80 > results/m2-4-g4b-c.txt 2>/tmp/m2_4_g4b.err
echo "port rc=$?"
tail -1 /tmp/m2_4_g4b.err
grep -hE "ata[12][:.]0|sda|md5|GUEST-INIT" results/m2-4-g4b-c.txt | sed 's/^/    /'
echo
echo "### G4c: the same boot on the reference, for the same lines"
timeout 2400 node oracle/run-boot.mjs "$KERNEL" build/minibios-80m.bin 1400000000 "console=ttyS0" \
    --hda tests/images/disk.img --initrd tests/images/guest-initramfs.img --memory 80 \
    > results/m2-4-g4b-js.txt 2>/tmp/m2_4_g4b_js.err
echo "reference rc=$?"
tail -1 /tmp/m2_4_g4b_js.err
grep -hE "ata[12][:.]0|sda|md5|GUEST-INIT" results/m2-4-g4b-js.txt | sed 's/^/    /'
echo
echo "### G1: THE VIRTIO CONSOLE GATE itself (both legs, one command line each)"
echo "port:      ./build/v86c --bzimage $KERNEL 3000000000 --cmdline 'console=hvc0 console=ttyS0' \\"
echo "               --initrd tests/images/guest-initramfs-virtio.img --bios build/minibios-80m.bin \\"
echo "               --memory 80 --virtio-console results/m2-4-c-virtio.txt \\"
echo "               --virtio-console-in results/m2-4-virtio-in.txt --serial results/m2-4-c-serial.txt"
echo "reference: node oracle/run-boot.mjs $KERNEL build/minibios-80m.bin 3000000000 \\"
echo "               'console=hvc0 console=ttyS0' --initrd tests/images/guest-initramfs-virtio.img \\"
echo "               --memory 80 --virtio-console results/m2-4-js-virtio.txt \\"
echo "               --virtio-console-in results/m2-4-virtio-in.txt > results/m2-4-js-serial.txt"
echo "(captured by the unit; the transcripts are in results/m2-4-{c,js}-{serial,virtio}.txt)"
echo
echo "### G1b: THE SAME KERNEL WITH console=hvc0 ALONE -- the kernel's log where no other"
echo "###      console exists (ttyS0 receives 30 bytes, the virtio console 2154)"
echo "the transcripts: results/m2-4-hvconly-serial.txt (30 bytes) and"
echo "                 results/m2-4-hvconly-virtio.txt (2154 bytes)"
grep -hE "unable to open an initial console|console \[hvc0\] enabled|read back" results/m2-4-hvconly-virtio.txt | sed 's/^/    /' 
