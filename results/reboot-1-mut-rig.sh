#!/bin/sh
# reboot-1-mut-rig.sh -- REBOOT-1's G4: the new gate must be shown ABLE TO FAIL.
#
# Each mutation removes ONE reset from machine_reboot (or the post-unwind cache
# clear) -- the class of omission the gate exists to catch -- rebuilds, and runs
# both trigger images.  A mutation is CAUGHT when the run no longer reports the
# GOLD verdict 0x600D600D (the fixture's EBX names the first failed witness, as a
# signed int32: -87228414 = 0xFACD0002 "the CPU was not reset", ...0003 the PS/2,
# ...0004 the PIT), or fails outright.  The tree is restored and rebuilt after
# every mutation; the rig leaves the build identical to the source tree.
#
# NOT in this rig, deliberately: pic_reset and rtc_fill_cmos omissions are NOT
# observable by the two-boot image (the image masks both PICs itself before its
# only probe, and reads no CMOS) -- results/reboot-1.md states that gap and where
# each is caught instead (the kernel arm's boot 2 for the PIC's stale remap/IMR).
set -e
cd "$(dirname "$0")/.."

V86C=build/v86c
WORK=$(mktemp -d)
trap 'cp "$WORK/machine.c" src/hw/machine.c; cp "$WORK/main.c" src/main.c; make >/dev/null 2>&1; rm -rf "$WORK"' EXIT
cp src/hw/machine.c "$WORK/machine.c"
cp src/main.c "$WORK/main.c"

GOLD=1611489293 # 0x600D600D, the fixture's decimal rendering of EBX

apply() { # apply <file> <old> <new>
	python3 -c "
import sys
p = '$1'
s = open(p).read()
old = sys.argv[1]; new = sys.argv[2]
assert old in s, 'mutation target not found in ' + p
open(p, 'w').write(s.replace(old, new, 1))
" "$2" "$3"
	make >/dev/null # a failed build must not be read as a mutation result
}

verdict_of() { # verdict_of <img> -> EBX decimal, or RUNFAILED
	local v
	v=$(./$V86C "$1" 2000000 --fixture 2>/dev/null | sed -n 6p | tr -d ' ,')
	echo "${v:-RUNFAILED}"
}

mut() { # mut <name> <file> <old> <new>
	local name="$1" file="$2" old="$3" new="$4" v seen=""
	local caught=yes
	apply "$file" "$old" "$new"
	for img in tests/reboot-pci.img tests/reboot-ps2.img; do
		v=$(verdict_of "$img")
		seen="$seen $v"
		[ "$v" != "$GOLD" ] || caught=no
	done
	if [ "$caught" = yes ]; then
		echo "MUT $name: CAUGHT (EBX:$seen)"
	else
		echo "MUT $name: NOT CAUGHT by the two-boot image (EBX:$seen)"
	fi
	cp "$WORK/machine.c" src/hw/machine.c
	cp "$WORK/main.c" src/main.c
}

# M1: the CPU reset -- the gate's ECX witness (boot 1 leaves 0xDEADBEEF)
mut "M1 omit reset_cpu" src/hw/machine.c \
	"    reset_cpu();" "    /* MUTATED-OUT: reset_cpu(); */"

# M2: the PS/2 reset (the reference's own list) -- the command-register witness
# (boot 1 leaves 0x65; the reset default is 0x05)
mut "M2 omit ps2_reset" src/hw/machine.c \
	"    ps2_reset();" "    /* MUTATED-OUT: ps2_reset(); */"

# M3: the PIT reset -- the latch witness (boot 1 leaves channel 0 counting;
# the reset leaves it disabled, latching 0x0000)
mut "M3 omit pit_init" src/hw/machine.c \
	"    pic_reset();

    pit_init(machine_bus);" "    pic_reset();"

# M5: the post-unwind code-cache clear (main.c's reboot_prepare_reentry) -- the
# reference's own reset-time clear, cpu.rs:4757
mut "M5 omit uop_cache_clear" src/main.c \
	"	uop_cache_clear();  /* drops the compiled blocks too (uop_cache.c) */" "	/* MUTATED-OUT: uop_cache_clear(); */"

make >/dev/null
echo "mut-rig: tree restored and rebuilt"
