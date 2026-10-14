#!/bin/sh
# results/perf-fix-2-probe.sh -- ITEM A's asymmetry rig (unit PERF-FIX-2).
#
# The tree-wide sweep found THREE more sites of the class PERF-FIX closed 14 of: the
# SECOND FORM of upstream's 16-bit ip guard, a bare `dbg_assert!(false)` under a
# guest-input check (instructions.rs:1001-1005 instr32_9A, 2462-2469 instr32_FF_3_mem,
# 2490-2494 instr32_FF_5_mem).  `dbg_assert!` is `debug_assert!` (dbg.rs:63), COMPILED
# OUT of the pinned release reference, which therefore far_jumps with the full 32-bit
# new_ip; the port shipped a live `DBG_ASSERT(((uint32_t)new_ip & 0xFFFF0000u) == 0)` at
# each and aborted on ordinary guest bytes (`66 FF 1E <fword>` in a 16-bit code segment
# with a >= 64 KiB target).
#
# This rig measures the asymmetry on the SAME tree, twice:
#
#   $WORK/v86c-before   the unity TU built with THE PRE-FIX instructions.c --
#                       results/perf-fix-2-instructions-prefix.c.gz, sha256 checked
#                       against the file as it stood before this unit's edit (the tree's
#                       other sources are the current ones, identical in both legs).
#   $WORK/v86c-after    the unity TU as the tree ships it now.
#   node oracle/run.mjs the pinned JS reference -- the oracle the port is compared to.
#
# Per mode it prints the exit status, the stop, and whether the AFTER leg's transcript is
# BYTE-IDENTICAL to the reference's -- which is the actual claim: at these sites the port
# must now do what the release reference does.
#
# Usage:  sh results/perf-fix-2-probe.sh
# Reads: results/perf-fix-2-probe.asm, results/perf-fix-2-instructions-prefix.c.gz,
#        tests/test.ld, the tree's sources (r/o), Makefile (for the source list + flags)
# Writes: build/pf2/*

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

COSMOCC=${COSMOCC:-$HOME/.local/cosmo/bin/cosmocc}
WORK=build/pf2
PREFIX_GZ=results/perf-fix-2-instructions-prefix.c.gz
PREFIX_SHA=38256a3bc7b5379015382a6e80b1ceede625ea94ab53cd7fc6ac185e1eb8b197
LIMIT=100000

say() { printf '%s\n' "$*"; }
hr() { say "-------------------------------------------------------------------"; }

mkdir -p "$WORK" || exit 1

# --- 1. the pre-fix ("before") instructions.c ----------------------------------------
python3 -c "import gzip,sys; sys.stdout.buffer.write(gzip.open('$PREFIX_GZ','rb').read())" \
	> "$WORK/instructions.prefix.c" || exit 1
got=$(sha256sum "$WORK/instructions.prefix.c" | cut -d' ' -f1)
if [ "$got" != "$PREFIX_SHA" ]; then
	say "FATAL: the stored pre-fix instructions.c is not the reviewed artifact"
	say "  want $PREFIX_SHA"
	say "  got  $got"
	exit 1
fi
say "pre-fix instructions.c sha256 == the stored artifact: $got"

# --- 2. the tree's own source list and flags (asked of make, not duplicated) -----------
db=$(make -s -p -n 2>/dev/null)
val() { printf '%s\n' "$db" | sed -n "s/^$1 :\?= *//p" | head -1; }
CPU_CFLAGS=$(val CPU_CFLAGS)
CFLAGS=$(val CFLAGS)
PERF_CFLAGS=$(val PERF_CFLAGS)
UNITY_SRC=$(val UNITY_SRC)
HW_OBJ=$(val HW_OBJ)
SF_OBJ=$(val SF_OBJ)
MINIBIOS_EMBED=$(val MINIBIOS_EMBED)
for v in CPU_CFLAGS UNITY_SRC HW_OBJ SF_OBJ MINIBIOS_EMBED; do
	eval "x=\$$v"
	if [ -z "$x" ]; then say "FATAL: make did not report $v"; exit 1; fi
done
say "unity sources: $(printf '%s\n' $UNITY_SRC | wc -l) files, flags: $CPU_CFLAGS"

# A concurrent unit may edit this tree; the two legs must come from ONE snapshot.  The
# digest is taken of the exact sources the unity TU will include.
snap() { sha256sum $UNITY_SRC | sha256sum | cut -d' ' -f1; }
snap_before=$(snap)

# --- 3. the two unity TUs: identical except for instructions.c ------------------------
python3 gen/unity_c.py -o "$WORK/after.c" $UNITY_SRC || exit 1
sed "s|#include \"src/cpu/instructions.c\"|#include \"$(pwd)/$WORK/instructions.prefix.c\"|" \
	"$WORK/after.c" > "$WORK/before.c"
if [ "$(grep -c "$WORK/instructions.prefix.c" "$WORK/before.c")" -ne 1 ]; then
	say "FATAL: the pre-fix substitution did not hit exactly one include"
	exit 1
fi
say "before.c/after.c differ in exactly: $(diff "$WORK/before.c" "$WORK/after.c" | grep -c '^[<>]') line(s)"

# --- 4. the two binaries ---------------------------------------------------------------
# The hw/softfloat objects and the embedded firmware are the tree's own (identical in
# both legs); only the unity object differs.
# shellcheck disable=SC2086
make -s $HW_OBJ $SF_OBJ $MINIBIOS_EMBED || exit 1
# shellcheck disable=SC2086
# -iquote src/cpu (NOT -I): the pre-fix instructions.c lives in $WORK, so its quoted
# includes ("instructions.h") must resolve against src/cpu by name.  An -I would be
# wrong: an angle include like <string.h> searches -I dirs FIRST and src/cpu/string.h is
# this port's string-instruction header, which would silently replace <string.h>.
# shellcheck disable=SC2086
"$COSMOCC" $CPU_CFLAGS -iquote src/cpu -I . -I src -c -o "$WORK/before.o" "$WORK/before.c" || exit 1
# shellcheck disable=SC2086
"$COSMOCC" $CPU_CFLAGS -iquote src/cpu -I . -I src -c -o "$WORK/after.o" "$WORK/after.c" || exit 1
snap_after=$(snap)

# shellcheck disable=SC2086
"$COSMOCC" $CFLAGS $PERF_CFLAGS -I src -Wl,--gc-sections -o "$WORK/v86c-before" \
	src/main.c $MINIBIOS_EMBED "$WORK/before.o" $HW_OBJ $SF_OBJ || exit 1
# shellcheck disable=SC2086
"$COSMOCC" $CFLAGS $PERF_CFLAGS -I src -Wl,--gc-sections -o "$WORK/v86c-after" \
	src/main.c $MINIBIOS_EMBED "$WORK/after.o" $HW_OBJ $SF_OBJ || exit 1
hr
say "SOURCE SNAPSHOT (sha256 over the unity sources, before -> after the two compiles)"
say "  $snap_before"
say "  $snap_after"
if [ "$snap_before" != "$snap_after" ]; then
	say "  !! the tree changed between the legs (a concurrent writer) -- re-run"
fi
say "BINARIES"
printf '  before (pre-fix instructions.c): %s  %s bytes\n' \
	"$(sha256sum "$WORK/v86c-before" | cut -c1-16)" "$(wc -c < "$WORK/v86c-before")"
printf '  after  (the tree):               %s  %s bytes\n' \
	"$(sha256sum "$WORK/v86c-after" | cut -c1-16)" "$(wc -c < "$WORK/v86c-after")"

# --- 5. the probe images ---------------------------------------------------------------
for mode in 6 7 8 9; do
	as --32 --defsym MODE=$mode -o "$WORK/probe.$mode.o" results/perf-fix-2-probe.asm || exit 1
	ld -m elf_i386 -T tests/test.ld -o "$WORK/probe.$mode.elf" "$WORK/probe.$mode.o" || exit 1
	objcopy -O binary "$WORK/probe.$mode.elf" "$WORK/probe.$mode.img"
done
hr
say "PROBE IMAGES"
for mode in 6 7 8 9; do
	printf '  MODE=%s  %6s bytes  sha256=%s\n' "$mode" "$(wc -c < "$WORK/probe.$mode.img")" \
		"$(sha256sum "$WORK/probe.$mode.img" | cut -c1-16)"
done

# --- 6. the runs -----------------------------------------------------------------------
run_leg() { # <bin> <img> ; prints "rc=<n> stop=<assert message|named stop|none>"
	bin=$1; img=$2
	out=$( ( "$bin" "$img" "$LIMIT" 2>&1 >/dev/null ) 2>/dev/null )
	rc=$?
	reason=$(printf '%s\n' "$out" | grep -m1 -E 'assert\(|v86c: ' | cut -c1-118)
	[ -z "$reason" ] && reason=none
	printf 'rc=%-4s stop=%s' "$rc" "$reason"
}

flight() { # <file>
	[ -s "$1" ] || { printf 'no transcript (the leg produced no state)'; return; }
	python3 - "$1" <<'PY'
import json,sys
d=json.load(open(sys.argv[1]))
print("eip=%s instructions=%s" % (d["cpu"]["eip"], d["instruction_count"]))
PY
}

hr
say "THE RUNS (MODE=6 callf m16:32, MODE=7 callf ptr16:32, MODE=8 jmpf m16:32, MODE=9 the"
say "         control: the same instruction with a < 64 KiB new_ip)"
for mode in 6 7 8 9; do
	img=$WORK/probe.$mode.img
	say ""
	say "MODE=$mode"
	printf '  before : '; run_leg "$WORK/v86c-before" "$img"; say ""
	printf '  after  : '; run_leg "$WORK/v86c-after" "$img"; say ""
	( "$WORK/v86c-after" "$img" "$LIMIT" > "$WORK/c.$mode.txt" 2>"$WORK/c.$mode.err" ) 2>/dev/null; crc=$?
	node oracle/run.mjs "$img" "$LIMIT" > "$WORK/js.$mode.txt" 2>"$WORK/js.$mode.err"; jrc=$?
	if [ ! -s "$WORK/js.$mode.txt" ]; then
		printf '  reference: rc=%s  NEITHER leg produced a transcript (both stopped)\n' "$jrc"
		printf '    ref stop: %s\n' "$(head -3 "$WORK/js.$mode.err" | tr '\n' ' ' | cut -c1-150)"
	elif diff -q "$WORK/js.$mode.txt" "$WORK/c.$mode.txt" > /dev/null 2>&1; then
		printf '  reference: rc=%s  C transcript vs reference: IDENTICAL\n' "$jrc"
	else
		printf '  reference: rc=%s  C transcript vs reference: DIFFERS\n' "$jrc"
		diff "$WORK/js.$mode.txt" "$WORK/c.$mode.txt" | head -6 | sed 's/^/    /'
		head -3 "$WORK/c.$mode.err" | sed 's/^/    C   stderr: /'
	fi
	printf '  eip/instruction count  : C   %s\n' "$(flight "$WORK/c.$mode.txt")"
	printf '                           ref %s\n' "$(flight "$WORK/js.$mode.txt")"
	printf '  C leg rc=%s\n' "$crc"
done
hr
say "raw transcripts: $WORK/{c,js}.<mode>.txt"
say ""
say "note: any 'Aborted (core dumped)' line this script's SHELL prints on STDERR is the"
say "      shell reporting an intentional rc=134 leg above; a leg's own stop message is the"
say "      one printed in the 'stop=' field on STDOUT."
