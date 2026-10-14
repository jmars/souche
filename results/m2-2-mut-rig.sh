#!/bin/sh
#
# results/m2-2-mut-rig.sh -- M2-2's G4: THE ASYMMETRY RIG.
#
# The brief: "at least three single-line mutations, each failing then restored-green".  Each
# mutation below is A SINGLE LINE of a file this unit wrote, and each is chosen so that the
# thing it breaks is exactly what one of the unit's gates claims to test:
#
#   1. src/cpu/instructions_0f.c -- rdrand's register body back to trigger_ud (the
#      transcription error this unit found and fixed).  The claim: an i386 kernel's
#      random_init_early executes `rdrand ecx`, and #UD there means no console ever comes up.
#   2. src/hw/pci.c -- the absent-device bound check neutered (the out-of-bounds index that
#      SIGSEGVed the port on the kernel's bus-255 config scan).
#   3. tests/minibios.asm -- the option ROM checksum loop back to ONE byte per iteration
#      (the bug that made the firmware report "no option ROM found").
#   4. tests/minibios.asm -- the E820 success path's CF patch removed (the IRET-frame
#      finding: a handler's stc/clc never reaches the caller, so the kernel falls back to the
#      int 15h/88h memory map and the boot's own output changes shape).
#   5. src/hw/uart.c -- the per-channel output message name collapsed to COM1's (the second
#      channel's sink assertion).
#   6. src/hw/kernel.c -- the heap_end_ptr the loader writes (0xDE00, the boot protocol's
#      end-of-setup-heap) moved by one page.
#
# THE LIVE TREE IS MUTATED AND RESTORED, one mutation at a time, and every touched file's
# sha256 is printed before and after: the rig's own restore is a claim, so it is checked.
# Nothing in this file is evidence -- results/m2-2-mut.txt is the raw capture.
#
# usage: sh results/m2-2-mut-rig.sh

set -u

cd "$(dirname "$0")/.."

KERNEL=tests/images/vmlinuz-i386
LIMIT=800000000
SELFTEST=build/hw_selftest
FILES="src/cpu/instructions_0f.c src/hw/pci.c src/hw/uart.c src/hw/kernel.c tests/minibios.asm"

echo "=== G4: the asymmetry rig (each mutation alone, then restored) ==="
echo
echo "--- the untouched tree, as the control ---"
sha256sum $FILES
make -s build/v86c $SELFTEST > /dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }
./$SELFTEST > /tmp/m2_2_ctl_selftest.txt 2>&1
echo "control selftest rc=$? :: $(tail -1 /tmp/m2_2_ctl_selftest.txt)"
./build/v86c --bzimage "$KERNEL" $LIMIT --cmdline "console=ttyS0" > /tmp/m2_2_ctl_boot.txt 2> /tmp/m2_2_ctl_boot.err
echo "control boot rc=$? :: $(wc -l < /tmp/m2_2_ctl_boot.txt) lines, e820=$(grep -c 'BIOS-e820' /tmp/m2_2_ctl_boot.txt), panic=$(grep -c 'Kernel panic' /tmp/m2_2_ctl_boot.txt)"
echo

mutate() # mutate <file> <old> <new>
{
    python3 - "$1" "$2" "$3" <<'PYEOF'
import sys
path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(path).read()
assert s.count(old) == 1, "the mutation's anchor is not unique in %s" % path
open(path, 'w').write(s.replace(old, new))
PYEOF
    [ $? -eq 0 ] || { echo "MUTATION FAILED TO APPLY: $1"; exit 1; }
}

report_boot() # report_boot <label>
{
    ./build/v86c --bzimage "$KERNEL" $LIMIT --cmdline "console=ttyS0" > /tmp/m2_2_mut_boot.txt 2> /tmp/m2_2_mut_boot.err
    echo "$1: boot rc=$?, $(wc -l < /tmp/m2_2_mut_boot.txt) lines, e820=$(grep -c 'BIOS-e820' /tmp/m2_2_mut_boot.txt), 88=$(grep -c 'BIOS-88' /tmp/m2_2_mut_boot.txt), panic=$(grep -c 'Kernel panic' /tmp/m2_2_mut_boot.txt), version=$(grep -c 'Linux version' /tmp/m2_2_mut_boot.txt), optrom=$(grep -c 'option ROM at' /tmp/m2_2_mut_boot.txt)"
}

report_selftest() # report_selftest <label>
{
    make -s $SELFTEST > /dev/null 2>&1
    ./$SELFTEST > /tmp/m2_2_mut_selftest.txt 2>&1
    echo "$1: selftest rc=$? :: $(tail -1 /tmp/m2_2_mut_selftest.txt)"
    grep -m2 '^FAIL' /tmp/m2_2_mut_selftest.txt | sed 's/^/    /'
}

rebuild() # EVERY mutation must be REBUILT before its run: a mutated source that is not
{         # rebuilt measures the PREVIOUS binary and reports "no effect" (MEASURED -- the
          # first version of this rig did exactly that for M1-M4, and the fix is this call).
    make -s build/v86c $SELFTEST > /dev/null 2>&1 || { echo "REBUILD FAILED"; exit 1; }
}

# ---------------------------------------------------------------- mutation 1
echo "=== M1: instructions_0f.c -- rdrand's register body back to #UD ==="
cp src/cpu/instructions_0f.c /tmp/m2_2_m1.c
mutate src/cpu/instructions_0f.c '	write_reg32(r, rdrand_value());' '	{ trigger_ud(); (void)r; } /* MUTATION: the pre-M2-2 body */'
rebuild
report_boot "M1 mutated"
cp /tmp/m2_2_m1.c src/cpu/instructions_0f.c
rebuild
report_boot "M1 restored from the saved copy"
echo

# ---------------------------------------------------------------- mutation 2
echo "=== M2: pci.c -- the absent-device bound check neutered (the out-of-bounds index) ==="
cp src/hw/pci.c /tmp/m2_2_m2.c
mutate src/hw/pci.c '    return bdf < 0 || bdf >= PCI_DEVICE_COUNT;' '    return (void)bdf, false; /* MUTATION */'
rebuild
report_boot "M2 mutated"
cp /tmp/m2_2_m2.c src/hw/pci.c
rebuild
report_boot "M2 restored from the saved copy"
echo

# ---------------------------------------------------------------- mutation 3
echo "=== M3: minibios.asm -- the option ROM checksum adds one byte per pump ==="
cp tests/minibios.asm /tmp/m2_2_m3.asm
mutate tests/minibios.asm '	add	al, es:[bx + si + 1]
' ''
rebuild
report_boot "M3 mutated"
cp /tmp/m2_2_m3.asm tests/minibios.asm
rebuild
report_boot "M3 restored from the saved copy"
echo

# ---------------------------------------------------------------- mutation 4
echo "=== M4: minibios.asm -- the E820 success path loses its CF patch ==="
cp tests/minibios.asm /tmp/m2_2_m4.asm
mutate tests/minibios.asm '1:	jmp	iret_cf0		/* CF=0: this entry is valid, the chain continues */' '1:	iret				/* MUTATION */'
rebuild
report_boot "M4 mutated"
cp /tmp/m2_2_m4.asm tests/minibios.asm
rebuild
report_boot "M4 restored from the saved copy"
echo

# ---------------------------------------------------------------- mutation 5
echo "=== M5: uart.c -- every channel's output message is COM1's ==="
cp src/hw/uart.c /tmp/m2_2_m5.c
mutate src/hw/uart.c '        BUS_SEND(uart->bus, uart_names[uart->com].output_byte, out_byte & 0xFF);' '        BUS_SEND(uart->bus, uart_names[0].output_byte, out_byte & 0xFF); /* MUTATION */'
rebuild
report_selftest "M5 mutated"
cp /tmp/m2_2_m5.c src/hw/uart.c
rebuild
report_selftest "M5 restored from the saved copy"
echo

# ---------------------------------------------------------------- mutation 6
echo "=== M6: kernel.c -- heap_end_ptr off by one page ==="
cp src/hw/kernel.c /tmp/m2_2_m6.c
mutate src/hw/kernel.c '#define HEAP_END_PTR (HEAP_END - 0x200)' '#define HEAP_END_PTR (HEAP_END - 0x1200) /* MUTATION */'
rebuild
report_selftest "M6 mutated"
cp /tmp/m2_2_m6.c src/hw/kernel.c
rebuild
report_selftest "M6 restored from the saved copy"
echo

# ---------------------------------------------------------------- the restore check
make -s build/v86c $SELFTEST > /dev/null 2>&1 || { echo "FINAL REBUILD FAILED"; exit 1; }
echo "--- the tree after every mutation has been restored ---"
sha256sum $FILES
echo
echo "--- the control again, from the restored tree ---"
./$SELFTEST > /tmp/m2_2_end_selftest.txt 2>&1
echo "restored selftest rc=$? :: $(tail -1 /tmp/m2_2_end_selftest.txt)"
./build/v86c --bzimage "$KERNEL" $LIMIT --cmdline "console=ttyS0" > /tmp/m2_2_end_boot.txt 2> /tmp/m2_2_end_boot.err
echo "restored boot rc=$? :: $(wc -l < /tmp/m2_2_end_boot.txt) lines, e820=$(grep -c 'BIOS-e820' /tmp/m2_2_end_boot.txt), panic=$(grep -c 'Kernel panic' /tmp/m2_2_end_boot.txt)"
echo
echo "=== end of the rig ==="
