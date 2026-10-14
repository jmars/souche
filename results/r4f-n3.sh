#!/bin/sh
# results/r4f-n3.sh -- the N3 asymmetry rig (unit R4F, gate G4's N3 half).
#
# It assembles results/r4f-probe.asm in four modes (see that file) and runs each image on
# three artifacts:
#
#   $WORK/v86c-before   THE REVIEWED PRE-FIX BINARY, built here from
#                       results/r4f-fpu-prefix.c.gz, whose sha256 is checked against
#                       5987fa5cf0c4323bc323d7b56d108145ee28d3d239bd813d9aba0b427042a096 --
#                       the sha results/r4-review.md recorded for the fpu.c it reviewed.
#                       The "before" side is therefore that artifact, rebuilt, not a
#                       recollection of a run.
#   ./build/v86c        the binary AFTER the fix (run `make` first)
#   node oracle/run.mjs the pinned JS reference -- the oracle the port is compared to
#
# What it measures, per mode: the exit status of both port binaries, whether the trailing
# store was reached (the transcript's hashed window / eip), and whether the AFTER port's
# transcript is BYTE-IDENTICAL to the reference's -- which is the actual claim: at a
# misaligned operand the port must now do what the release reference does.
#
# Usage:  sh results/r4f-n3.sh
# Reads: results/r4f-probe.asm, results/r4f-fpu-prefix.c.gz, tests/test.ld (r/o)
# Writes: build/r4f-probe/*

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

COSMOCC=${COSMOCC:-$HOME/.local/cosmo/bin/cosmocc}
WORK=build/r4f-probe
PREFIX_GZ=results/r4f-fpu-prefix.c.gz
PREFIX_SHA=5987fa5cf0c4323bc323d7b56d108145ee28d3d239bd813d9aba0b427042a096
LIMIT=1000

say() { printf '%s\n' "$*"; }
hr() { say "-------------------------------------------------------------------"; }

mkdir -p "$WORK" || exit 1

# --- 1. the pre-fix ("before") fpu.c, and the object built from it --------------------
python3 -c "import gzip,sys; sys.stdout.buffer.write(gzip.open('$PREFIX_GZ','rb').read())" > "$WORK/fpu.prefix.c" || exit 1
got=$(sha256sum "$WORK/fpu.prefix.c" | cut -d' ' -f1)
if [ "$got" != "$PREFIX_SHA" ]; then
	say "FATAL: the stored pre-fix fpu.c is not the reviewed artifact"
	say "  want $PREFIX_SHA"
	say "  got  $got"
	exit 1
fi
say "pre-fix fpu.c sha256 == the sha R4's review recorded: $got"

# --- 2. the probe images -------------------------------------------------------------
for mode in 0 1 2 3; do
	as --32 --defsym MODE=$mode -o "$WORK/probe.$mode.o" results/r4f-probe.asm || exit 1
	ld -m elf_i386 -T tests/test.ld -o "$WORK/probe.$mode.elf" "$WORK/probe.$mode.o" || exit 1
	objcopy -O binary "$WORK/probe.$mode.elf" "$WORK/probe.$mode.img"
done
hr
say "PROBE IMAGES"
for mode in 0 1 2 3; do
	printf '  MODE=%s  %6s bytes  sha256=%s\n' "$mode" "$(wc -c < "$WORK/probe.$mode.img")" \
		"$(sha256sum "$WORK/probe.$mode.img" | cut -c1-16)"
done

# --- 3. the two port binaries ---------------------------------------------------------
# The objects are the tree's, minus fpu.o: the before binary differs from the after one
# only in fpu.c, which is the file under test.
test -x ./build/v86c || { say "FATAL: ./build/v86c missing -- run make first"; exit 1; }
# NOTE the flags: -iquote (NOT -I) src/cpu.  fpu.c includes its neighbours by quoted
# name, which in the tree resolves against src/cpu/ because that is where fpu.c LIVES;
# the copy lives elsewhere, so the directory has to be named -- and an -I would be wrong
# here: an angle include like <string.h> searches -I dirs FIRST, and src/cpu/string.h is
# this port's string-instruction header, so -I src/cpu would silently replace <string.h>.
"$COSMOCC" -std=gnu11 -O2 -Wall -Wextra -Werror -iquote src/cpu -c -o "$WORK/fpu.prefix.o" "$WORK/fpu.prefix.c" || exit 1
objs=$(ls build/cpu/*.o | grep -v 'fpu\.o$')
# shellcheck disable=SC2086
"$COSMOCC" -std=gnu11 -O2 -Wall -Wextra -I src -Wl,--gc-sections -o "$WORK/v86c-before" \
	src/main.c $objs "$WORK/fpu.prefix.o" build/sf/softfloat.o || exit 1
hr
say "BINARIES"
printf '  before (pre-fix fpu.c): %s\n' "$(sha256sum "$WORK/v86c-before" | cut -c1-16)"
printf '  after  (the tree):      %s\n' "$(sha256sum ./build/v86c | cut -c1-16)"

# --- 4. the runs ---------------------------------------------------------------------
run_leg() { # <cmd...> ; prints "rc=<n> transcript=<lines> stop=<assert message|none>"
	img=$1; shift
	out=$("$@" "$img" "$LIMIT" 2>&1)
	rc=$?
	reason=$(printf '%s\n' "$out" | grep -m1 -E 'assert\(|v86c: ' | cut -c1-104)
	[ -z "$reason" ] && reason=none
	printf 'rc=%-4s stop=%s' "$rc" "$reason"
}

# the transcript's own account of how far the guest got (eip + instruction count), which is
# where "the machine executed PAST the fxsave and its trailing store" is visible
flight() { # <file>
	[ -s "$1" ] || { printf 'no transcript (the leg produced no state)'; return; }
	python3 - "$1" <<'PY'
import json,sys
d=json.load(open(sys.argv[1]))
print("eip=%s instructions=%s" % (d["cpu"]["eip"], d["instruction_count"]))
PY
}

hr
say "THE RUNS  (MODE=1/3 are the misaligned operands; MODE=2/0 are the controls)"
for mode in 0 1 2 3; do
	img=$WORK/probe.$mode.img
	say ""
	say "MODE=$mode"
	printf '  before : '; run_leg "$img" "$WORK/v86c-before"; say ""
	printf '  after  : '; run_leg "$img" ./build/v86c; say ""
	# the reference leg + the byte-for-byte comparison with the AFTER port
	./build/v86c "$img" "$LIMIT" > "$WORK/c.$mode.txt" 2>"$WORK/c.$mode.err"; crc=$?
	node oracle/run.mjs "$img" "$LIMIT" > "$WORK/js.$mode.txt" 2>"$WORK/js.$mode.err"; jrc=$?
	if diff -q "$WORK/js.$mode.txt" "$WORK/c.$mode.txt" > /dev/null; then
		printf '  reference: rc=%s  C transcript vs reference: IDENTICAL\n' "$jrc"
	else
		say "  reference: rc=$jrc  C transcript vs reference: DIFFERS"
		diff -u "$WORK/js.$mode.txt" "$WORK/c.$mode.txt" | head -30
	fi
	printf '  eip/instruction count  : C  %s\n' "$(flight "$WORK/c.$mode.txt")"
	printf '                           ref %s\n' "$(flight "$WORK/js.$mode.txt")"
	printf '  C leg rc=%s\n' "$crc"
done
hr
say "raw transcripts: $WORK/{c,js}.<mode>.txt"
