#!/bin/bash
# perf-5-gates.sh -- PERF-5's G3: every existing gate, in BOTH cache modes, with the exit codes
# and the raw evidence kept.  The brief's rule is that the counts must be UNCHANGED except for
# the exec suite's own new arms; a gate that moves is a regression, not a speed-up.
#
# Usage: results/perf-5-gates.sh          (writes results/perf-5/gates-<name>.txt)
set -u
cd "$(dirname "$0")/.." || exit 2
OUT=results/perf-5
mkdir -p "$OUT"

rc_summary=""
note() { printf '%-46s rc=%s\n' "$1" "$2" | tee -a "$OUT/gates-summary.txt"; }

: > "$OUT/gates-summary.txt"
echo "== PERF-5 G3: $(date -u +%Y-%m-%dT%H:%M:%SZ) load=$(cut -d' ' -f1-3 /proc/loadavg)" | tee -a "$OUT/gates-summary.txt"

# G3a -- the build
make > "$OUT/gates-make.txt" 2>&1; note "make" $?

# G3b -- the selftest suites (their own counts must be unchanged except the new arms)
make selftest > "$OUT/gates-selftest.txt" 2>&1; rc=$?; note "make selftest" $rc
grep -E "check|failures" "$OUT/gates-selftest.txt" | tail -8 | tee -a "$OUT/gates-summary.txt"

# G3c -- the nasm corpus, cache ON (the shipped default) and OFF
make nasm > "$OUT/gates-nasm-on.txt" 2>&1; note "make nasm (cache on)" $?
tail -3 "$OUT/gates-nasm-on.txt" | tee -a "$OUT/gates-summary.txt"
python3 tests/nasm_run.py --stage run \
    --actual "build/v86c-nasm {img} {limit} --fixture --uop-cache off" \
    --skip-list tests/nasm_skip_c.txt > "$OUT/gates-nasm-off.txt" 2>&1
note "nasm with --uop-cache off" $?
tail -3 "$OUT/gates-nasm-off.txt" | tee -a "$OUT/gates-summary.txt"

# G3d -- the M0 transcript, byte-identical, both modes
make transcript-diff > "$OUT/gates-transcript-on.txt" 2>&1; note "make transcript-diff (cache on)" $?
./build/v86c tests/test.img 1000000 --uop-cache off > "$OUT/transcript-c-off.txt" 2>/dev/null
node oracle/run.mjs tests/test.img 1000000 > "$OUT/transcript-js-off.txt" 2>/dev/null
if diff -q "$OUT/transcript-js-off.txt" "$OUT/transcript-c-off.txt" > "$OUT/gates-transcript-off.txt" 2>&1; then
    note "transcript-diff with --uop-cache off" 0
else
    note "transcript-diff with --uop-cache off" 1
fi
cmp "$OUT/../transcript.js.txt" "$OUT/transcript-js-off.txt" && echo "the reference transcript is the same file in both legs" | tee -a "$OUT/gates-summary.txt"

# G3e -- the 1.5 G boot, both modes: the FINAL MACHINE STATE must be identical (PERF-FIX-3's
# G4c: the serial transcripts are allowed to differ on the guest-clock-dependent lines).
for mode in on off; do
    if [ "$mode" = off ]; then FLAG="--uop-cache off"; else FLAG=""; fi
    ./build/v86c --bzimage tests/images/vmlinuz-i386 1500000000 --cmdline "console=ttyS0" $FLAG \
        > "$OUT/boot-$mode-serial.txt" 2> "$OUT/boot-$mode-stderr.txt"
    note "1.5 G boot ($mode)" $?
    sed -n 's/^v86c boot: [0-9]* instructions, //p' "$OUT/boot-$mode-stderr.txt" \
        | tee -a "$OUT/gates-summary.txt"
done
echo "-- final-state compare (the brief's gate):" | tee -a "$OUT/gates-summary.txt"
a=$(sed -n 's/^v86c boot: [0-9]* instructions, //p' "$OUT/boot-on-stderr.txt")
b=$(sed -n 's/^v86c boot: [0-9]* instructions, //p' "$OUT/boot-off-stderr.txt")
if [ -n "$a" ] && [ "$a" = "$b" ]; then
    echo "IDENTICAL: $a" | tee -a "$OUT/gates-summary.txt"
else
    echo "DIFFER: on=\"$a\" off=\"$b\"" | tee -a "$OUT/gates-summary.txt"
fi
echo "-- serial sha256 (the transcripts themselves):" | tee -a "$OUT/gates-summary.txt"
sha256sum "$OUT/boot-on-serial.txt" "$OUT/boot-off-serial.txt" | tee -a "$OUT/gates-summary.txt"
