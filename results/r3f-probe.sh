#!/bin/sh
# results/r3f-probe.sh -- the N1 asymmetry rig (unit R3F, gate G1).
#
# It assembles results/r3f-probe.asm in five modes (see that file) and runs each image on
# three artifacts:
#
#   $WORK/v86c-prefix      THE SHIPPED BINARY BEFORE N1, rebuilt here from the pre-fix
#                          generated dispatcher kept as
#                          results/r3f-prefix-interpreter_gen.c.gz, whose sha256 is checked
#                          against bb53cf87... -- the sha results/r3-review.md recorded for
#                          the file it reviewed.  The "before" side is therefore the
#                          reviewed artifact, rebuilt, not a recollection of a run.
#   ./build/v86c           the binary AFTER N1/N2 (run `make` first)
#   node oracle/run.mjs    the pinned JS reference -- the oracle the port is compared to
#
# Usage:  sh results/r3f-probe.sh
# Reads: results/r3f-probe.asm, results/r3f-prefix-interpreter_gen.c.gz, tests/test.ld (r/o)
# Writes: build/r3f-probe/*

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

COSMOCC=${COSMOCC:-$HOME/.local/cosmo/bin/cosmocc}
WORK=build/r3f-probe
PREFIX_GZ=results/r3f-prefix-interpreter_gen.c.gz
PREFIX_SHA=bb53cf87e7969d27277e6d7c549bf7e1a6571b0b826386b32dfcb87e693c6f93
LIMIT=1000

say() { printf '%s\n' "$*"; }
hr() { say "-------------------------------------------------------------------"; }

mkdir -p "$WORK" || exit 1

# --- 1. the pre-fix ("before") binary ------------------------------------------------
python3 -c "import gzip,sys; sys.stdout.buffer.write(gzip.open('$PREFIX_GZ','rb').read())" > "$WORK/interpreter_gen.prefix.c" || exit 1
got=$(sha256sum "$WORK/interpreter_gen.prefix.c" | cut -d' ' -f1)
if [ "$got" != "$PREFIX_SHA" ]; then
	say "FATAL: the stored pre-fix dispatcher is not the reviewed artifact"
	say "  want $PREFIX_SHA"
	say "  got  $got"
	exit 1
fi
say "pre-fix dispatcher sha256 == the sha R3's review recorded: $got"
"$COSMOCC" -std=gnu11 -O2 -Wall -Wextra -Werror -I src -I src/cpu -c -o "$WORK/interpreter_gen.prefix.o" "$WORK/interpreter_gen.prefix.c" || exit 1
objs=$(ls build/cpu/*.o | grep -v 'interpreter_gen\.o$')
"$COSMOCC" -std=gnu11 -O2 -Wall -Wextra -I src -o "$WORK/v86c-prefix" src/main.c $objs "$WORK/interpreter_gen.prefix.o" || exit 1

# --- 2. the probe images -------------------------------------------------------------
for mode in 1 2 0 4 5; do
	as --32 --defsym MODE=$mode -o "$WORK/probe.$mode.o" results/r3f-probe.asm || exit 1
	ld -m elf_i386 -T tests/test.ld -o "$WORK/probe.$mode.elf" "$WORK/probe.$mode.o" || exit 1
	objcopy -O binary "$WORK/probe.$mode.elf" "$WORK/probe.$mode.img"
done
hr
say "PROBE IMAGES"
for mode in 1 2 0 4 5; do
	printf '  MODE=%s  %6s bytes  sha256=%s\n' "$mode" "$(wc -c < "$WORK/probe.$mode.img")" \
		"$(sha256sum "$WORK/probe.$mode.img" | cut -c1-16)"
done
say "  the sequence (MODE=1, disassembled):"
objdump -d "$WORK/probe.1.elf" 2>/dev/null | sed -n '/<_start>:/,/^$/p' | sed 's/^/    /'

# --- 3. run the three artifacts on every image ---------------------------------------
run_c() { # <label> <binary> <image>
	sh -c "$2 $3 $LIMIT" > "$WORK/out.txt" 2> "$WORK/err.txt"
	rc=$?
	printf '  %-22s exit=%-4s %s\n' "$1" "$rc" "$(head -1 "$WORK/err.txt" | cut -c1-96)"
	printf '%s' "$rc" > "$WORK/rc.$(basename "$3" .img).$(basename "$2")"
}

for mode in 1 2 0 4 5; do
	hr
	say "MODE=$mode ($WORK/probe.$mode.img)"
	run_c "BEFORE N1 (v86c-prefix)" "$WORK/v86c-prefix" "$WORK/probe.$mode.img"
	run_c "AFTER N1/N2 (v86c)" "./build/v86c" "$WORK/probe.$mode.img"
	node oracle/run.mjs "$WORK/probe.$mode.img" $LIMIT > "$WORK/js.$mode.txt" 2> "$WORK/js.$mode.err"
	printf '  %-22s exit=%-4s transcript=%s bytes\n' "JS REFERENCE" "$?" "$(wc -c < "$WORK/js.$mode.txt")"
done

# --- 4. what the reference did --------------------------------------------------------
hr
say "REFERENCE: is the stray prefix observable?  (MODE=1 vs MODE=2, equal length)"
if diff -u "$WORK/js.1.txt" "$WORK/js.2.txt" > "$WORK/js.1-2.diff"; then
	say "  transcripts IDENTICAL"
else
	say "  the only differing lines:"
	grep -E '^[+-][^+-]' "$WORK/js.1-2.diff" | sed 's/^/    /'
fi
say "REFERENCE: MODE=1 vs MODE=0 (the unprefixed encoding, one byte shorter)"
diff -u "$WORK/js.1.txt" "$WORK/js.0.txt" | grep -E '^[+-][^+-]' | sed 's/^/    /'

say "REFERENCE: does the fallthrough body EXECUTE, or is the prefix merely ignored?"
say "  MODE=4 puts eax into mm0 and then runs the STRAY-F2 store form (plain body: store"
say "  mm0 to 0x101000); MODE=5 writes the same 8 bytes with a GPR store."
for mode in 4 5; do
	printf '  MODE=%s memory_window sha256 = %s\n' "$mode" \
		"$(grep -A4 '"memory_window"' "$WORK/js.$mode.txt" | grep sha256 | cut -d'"' -f4)"
done
grep -A5 '"memory_window"' "$WORK/js.4.txt" | grep sha256 > "$WORK/win.4"
grep -A5 '"memory_window"' "$WORK/js.5.txt" | grep sha256 > "$WORK/win.5"
if cmp -s "$WORK/win.4" "$WORK/win.5"; then
	say "  EQUAL -> the reference EXECUTED the unprefixed body: the 8 bytes at 0x101000 are"
	say "  mm0 = 0x0000000011111111, byte for byte what the GPR store writes."
else
	say "  DIFFERENT -> the stray-prefix encoding's body did NOT merely store mm0; read"
	say "  results/r3f-fixes.md before drawing a conclusion from this probe."
fi
hr
say "exit codes recorded under $WORK/rc.*"
