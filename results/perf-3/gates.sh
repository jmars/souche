#!/bin/bash
# results/perf-3/gates.sh -- the raw evidence for PERF-3's G1/G2/G4/G5.
#
# run: results/perf-3/gates.sh   (from the repo root; ~6 minutes)
# out: results/perf-3/gates.txt  (every gate's command, exit code and the tail of its output)

set -u
cd "$(dirname "$0")/../.." || exit 2
OUT=results/perf-3/gates.txt
: > "$OUT"

say() { echo "$@" | tee -a "$OUT"; }

run() { # run <label> <cmd...>
    local label="$1"; shift
    say "=== $label"
    say "\$ $*"
    "$@" > /tmp/perf-3-gate.log 2>&1
    local rc=$?
    tail -6 /tmp/perf-3-gate.log | sed 's/^/    /' | tee -a "$OUT"
    say "    rc=$rc"
    return $rc
}

say "PERF-3 M1 gates, $(date -Is), host $(uname -sr), load $(cut -d' ' -f1-3 /proc/loadavg)"
say ""

# --- G5: the plan's ss10 hashes, re-checked ---------------------------------------------
say "=== G5: the plan's ss10 file hashes, first-12 sha256 NOW"
for f in src/cpu/cpu.c src/cpu/cpu.h src/cpu/cpu_state.h src/cpu/memory.c src/cpu/paging.c \
         src/cpu/modrm.c src/cpu/modrm.h src/cpu/instructions.c src/cpu/instructions_0f.c \
         src/cpu/string.c src/cpu/cpu_state.c src/cpu/interpreter_gen.c gen/interpreter_c.js \
         v86/gen/x86_table.js results/cpu-port-plan.md Makefile; do
    printf '    %-32s %s\n' "$f" "$(sha256sum "$f" | cut -c1-12)" | tee -a "$OUT"
done
say ""

# --- G1: a scratch build, and the generator's byte-reproducibility ----------------------
say "=== G1: make clean && make -j16 (cosmocc, -Wall -Wextra -Werror)"
make clean >/dev/null 2>&1
start=$(date +%s)
if make -j16 > /tmp/perf-3-build.log 2>&1; then rc=0; else rc=$?; fi
say "    rc=$rc  ($(($(date +%s) - start))s)"
grep -E 'warning|error' /tmp/perf-3-build.log | grep -v 'cannot find entry symbol _start' | head -5 | tee -a "$OUT"
say "    (the ONLY diagnostic is the pre-existing minibios ld warning:) "
grep -c 'cannot find entry symbol _start' /tmp/perf-3-build.log | sed 's/^/    /' | tee -a "$OUT"

say "=== G1b: the emitted uop_gen.c is byte-reproducible"
cp src/cpu/uop_gen.c /tmp/uop_gen.c.1
node gen/interpreter_uop.js >> "$OUT" 2>&1
node gen/interpreter_uop.js | tee -a "$OUT"
diff -q /tmp/uop_gen.c.1 src/cpu/uop_gen.c && say "    rc=0 (identical after re-running the generator)" || say "    DIFFERS"
say "    (and after DELETING it:)"
rm -f src/cpu/uop_gen.c
node gen/interpreter_uop.js | tee -a "$OUT"
sha256sum src/cpu/uop_gen.c /tmp/uop_gen.c.1 | tee -a "$OUT"
say ""

# --- G1c: the unity FIT of the new files
run "G1c: make uop-unity (the decoder + its tables inside the shipped unity TU)" make uop-unity
say "    the shipped TU's own source list (18 = the step-1 list, the two new files NOT in it):"
make unity-src 2>/dev/null | sed 's/^/    /' | tee -a "$OUT"
say ""

# --- G2 + G4a: the suites --------------------------------------------------------------
say "=== G2/G4a: make selftest (all four suites; the uop decode checker is the fourth)"
say "\$ make selftest"
make selftest > results/perf-3/selftest.txt 2>&1
selftest_rc=$?
tail -4 results/perf-3/selftest.txt | sed 's/^/    /' | tee -a "$OUT"
say "    rc=$selftest_rc   (full output: results/perf-3/selftest.txt)"
grep -E '^[a-z_]*selftest: [0-9]+ checks' results/perf-3/selftest.txt | sed 's/^/    /' | tee -a "$OUT"
grep -cE 'FAIL' results/perf-3/selftest.txt | sed 's/^/    FAIL lines: /' | tee -a "$OUT"
say ""
say "=== G2 evidence: the decode checker's own output (from the same run)"
sed -n '/^uop_cache_selftest coverage:/,$p' results/perf-3/selftest.txt | tee -a "$OUT"
say ""
run "G4b: make nasm" make nasm
run "G4c: make transcript-diff" make transcript-diff
say ""
say "=== G4d: make bench (7 runs on the shipped build/v86c)"
make bench 2>&1 | tail -8 | sed 's/^/    /' | tee -a "$OUT"
say ""
say "=== G4e/G4f: results/perf-3/attribution.sh -- PART 1 the identity proof, PART 2 the"
say "===         files-IN-the-unity-TU arm.  FULL output saved to results/perf-3/attribution.txt:"
results/perf-3/attribution.sh 6 > results/perf-3/attribution.txt 2>&1
grep -E 'IDENTICAL|DIFFER|^   [0-9a-f]{64}|median difference|n=[0-9]+  best' results/perf-3/attribution.txt | sed 's/^/    /' | tee -a "$OUT"
say ""
say "=== done $(date -Is)"
