#!/bin/sh
# results/perf-fix-n3.sh -- ITEM 2's asymmetry rig (unit PERF-FIX).
#
# ITEM 2 drops the 14 `DBG_ASSERT(cpu->is_32 || value < 0x10000)` sites from
# src/cpu/instructions.c, because upstream guards them with `dbg_assert!` (instructions.rs
# 1319..2474) -- a DEBUG assert, COMPILED OUT of the pinned release reference, which
# therefore simply executes the instruction.  The shipped port had them live, so guest
# bytes that a 16-bit code segment can emit aborted the machine where the reference runs
# on (perf-3's finding 3, MEASURED at instructions.c:1802).
#
# This rig measures the asymmetry the fix claims, on the SAME tree, twice:
#
#   $WORK/v86c-before   the unity TU built with THE REVIEWED PRE-FIX instructions.c --
#                       results/perf-fix-instructions-prefix.c.gz, sha256 checked against
#                       bcab21bad3aeb943880bcbde247a694cf8a7b49f8e3b22bed9fa15601636324a,
#                       the file as it stood before this unit's edit (the tree's other
#                       sources are the current ones, identical in both legs).
#   $WORK/v86c-after    the unity TU as the tree ships it now.
#   node oracle/run.mjs the pinned JS reference -- the oracle the port is compared to.
#
# The only difference between the two port legs is the substitution of that one
# instructions.c into the generated unity TU, so a difference in verdict is attributable
# to it.  Per mode it prints the exit status, the stop (an assert message or a named
# stop), and whether the AFTER leg's transcript is BYTE-IDENTICAL to the reference's --
# which is the actual claim: at these sites the port must now do what the release
# reference does.
#
# Usage:  sh results/perf-fix-n3.sh
# Reads: results/perf-fix-probe.asm, results/perf-fix-instructions-prefix.c.gz,
#        tests/test.ld, the tree's sources (r/o), Makefile (for the source list + flags)
# Writes: build/pf-n3/*

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

COSMOCC=${COSMOCC:-$HOME/.local/cosmo/bin/cosmocc}
WORK=build/pf-n3
PREFIX_GZ=results/perf-fix-instructions-prefix.c.gz
PREFIX_SHA=bcab21bad3aeb943880bcbde247a694cf8a7b49f8e3b22bed9fa15601636324a
LIMIT=1000

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
say "pre-fix instructions.c sha256 == the reviewed artifact: $got"

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

# A concurrent unit edits this tree; the two legs must come from ONE snapshot.  The
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
nmodes=0
for mode in 0 1 2 3 4 5; do
	as --32 --defsym MODE=$mode -o "$WORK/probe.$mode.o" results/perf-fix-probe.asm || exit 1
	ld -m elf_i386 -T tests/test.ld -o "$WORK/probe.$mode.elf" "$WORK/probe.$mode.o" || exit 1
	objcopy -O binary "$WORK/probe.$mode.elf" "$WORK/probe.$mode.img"
	nmodes=$((nmodes + 1))
done
hr
say "PROBE IMAGES"
for mode in 0 1 2 3 4 5; do
	printf '  MODE=%s  %6s bytes  sha256=%s\n' "$mode" "$(wc -c < "$WORK/probe.$mode.img")" \
		"$(sha256sum "$WORK/probe.$mode.img" | cut -c1-16)"
done

# --- 6. the runs -----------------------------------------------------------------------
run_leg() { # <bin> <img> ; prints "rc=<n> stop=<assert message|named stop|none>"
	# NB the extra subshell + its 2>/dev/null: an aborting leg makes the SHELL print
	# "Aborted (core dumped)" on the script's stderr, which would look like rig noise;
	# the binary's own stderr is what this captures.
	bin=$1; img=$2
	out=$( ( "$bin" "$img" "$LIMIT" 2>&1 >/dev/null ) 2>/dev/null )
	rc=$?
	reason=$(printf '%s\n' "$out" | grep -m1 -E 'assert\(|v86c: ' | cut -c1-110)
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
say "THE RUNS  (MODE=1 is the brief's bytes; MODE=3/2/0 are the controls; MODE=4 is the"
say "           controlled in-image landing; MODE=5 is a 32-bit-mode huge-eip transfer)"
for mode in 0 1 2 3 4 5; do
	img=$WORK/probe.$mode.img
	say ""
	say "MODE=$mode"
	printf '  before : '; run_leg "$WORK/v86c-before" "$img"; say ""
	printf '  after  : '; run_leg "$WORK/v86c-after" "$img"; say ""
	( "$WORK/v86c-after" "$img" "$LIMIT" > "$WORK/c.$mode.txt" 2>"$WORK/c.$mode.err" ) 2>/dev/null; crc=$?
	node oracle/run.mjs "$img" "$LIMIT" > "$WORK/js.$mode.txt" 2>"$WORK/js.$mode.err"; jrc=$?
	if [ ! -s "$WORK/js.$mode.txt" ]; then
		# neither leg wrote a transcript: an empty-vs-empty diff would read as
		# IDENTICAL, which is exactly the vacuous comparison this line must not make
		printf '  reference: rc=%s  NEITHER leg produced a transcript (both stopped)\n' "$jrc"
		printf '    ref stop: %s\n' "$(head -3 "$WORK/js.$mode.err" | tr '\n' ' ' | cut -c1-150)"
	elif diff -q "$WORK/js.$mode.txt" "$WORK/c.$mode.txt" > /dev/null 2>&1; then
		printf '  reference: rc=%s  C transcript vs reference: IDENTICAL\n' "$jrc"
	else
		printf '  reference: rc=%s  C transcript vs reference: DIFFERS\n' "$jrc"
		head -3 "$WORK/js.$mode.err" | sed 's/^/    ref stderr: /'
		head -3 "$WORK/c.$mode.err" | sed 's/^/    C   stderr: /'
	fi
	printf '  eip/instruction count  : C   %s\n' "$(flight "$WORK/c.$mode.txt")"
	printf '                           ref %s\n' "$(flight "$WORK/js.$mode.txt")"
	printf '  C leg rc=%s\n' "$crc"
done
hr
say "raw transcripts: $WORK/{c,js}.<mode>.txt"
say "MODE=1 is the brief's bytes: the AFTER leg continues past the dropped assert and the"
say "NEXT fetch (linear 0x3557785A, outside guest RAM) diverges from the reference in a"
say "PRE-EXISTING, unrelated way -- see results/perf-fix.md ITEM 2 (the cpu.c:100 finding)."
say ""
say "note: any 'Aborted (core dumped)' line this script's SHELL prints on STDERR (kept in"
say "      results/perf-fix-n3.err) is the shell reporting an intentional rc=134 leg above;"
say "      a leg's stop message is the one printed in the 'stop=' field on STDOUT."
