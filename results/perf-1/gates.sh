#!/bin/bash
# PERF-1 gate run (HANDOFF=v86c-perf).  Every gate the unit's brief names, on the
# FROM-SCRATCH unity build, with the exit code the gate decides by (exit status, not a
# tail of stdout).
cd "$(dirname "$0")/../.." || exit 2
mine() { echo; echo "########## $* ##########"; }
rc() { echo "----- exit=$1"; }

mine "G3.1 make selftest"
make selftest; rc $?

mine "G3.2 make nasm"
make nasm; rc $?

mine "G3.3 make oracle"
make oracle; rc $?

mine "G3.4 make determinism"
make determinism; rc $?

mine "G3.5 make transcript-diff"
make transcript-diff; rc $?

mine "G3.6 the boot gate (M2-5 GATE 1a's command line, same recorded artifacts)"
make minibios-mem >/dev/null 2>&1
./build/v86c --bzimage tests/images/vmlinuz-i386-v86guest 3000000000 \
    --cmdline 'console=hvc0 console=ttyS0 tsc=unstable idle=poll' --hda tests/images/disk.img \
    --initrd tests/images/guest-initramfs-v86gate.img --bios build/minibios-80m.bin --memory 80 \
    --virtio-console results/perf-1/boot-virtio.txt --virtio-console-in tests/v86gate-virtio-in.txt \
    --serial results/perf-1/boot-serial.txt
rc $?
echo "--- diff serial vs the recorded pristine C leg (results/m2-5-c-serial.txt)"
diff results/m2-5-c-serial.txt results/perf-1/boot-serial.txt && echo "serial: BYTE-IDENTICAL"
echo "--- diff virtio vs the recorded pristine C leg (results/m2-5-c-virtio.txt)"
diff results/m2-5-c-virtio.txt results/perf-1/boot-virtio.txt && echo "virtio: BYTE-IDENTICAL"
