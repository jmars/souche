#!/bin/bash
# G1e (JIT-FIX-2): IS THE HOLE CLASS WIDER THAN THE BRIEF'S TWO PAIRS?  A swap between a slot whose
# condition IS inlined (2 = jb) and one whose condition is NOT (0 = jo / 8 = js) makes the
# NON-inlined encoding look like it has a recipe -- it then gets compiled with jb's condition --
# while jb is de-inlined.  The matrix has no jo/js case at all, so ask the same legs the brief
# names: the suite, and the bench's pinned 200 M --jit on leg (which caught all four in-pair swaps).
set -u
cd "$(dirname "$0")/../.." || exit 2
F=results/jit-fix-2/jit_codegen.c.fixed
T=src/cpu/jit_codegen.c
O=results/jit-fix-2

: > "$O/G1e-unsupported-swap.txt"
for v in jcc8-swap-b0 jcc8-swap-s6; do
    cp "results/jit-fix-2/jit_codegen.c.$v" "$T" || exit 2
    make > "$O/G1e-make-$v.txt" 2>&1 || { echo "$v: MAKE FAILED" | tee -a "$O/G1e-unsupported-swap.txt"; continue; }

    make jit-codegen-selftest > "$O/G1e-selftest-$v.txt" 2>&1
    selrc=$?
    sel=$(grep -E '_selftest: [0-9]+ checks' "$O/G1e-selftest-$v.txt" | tail -1)

    ./build/v86c --bzimage tests/images/vmlinuz-i386 200000000 --cmdline "console=ttyS0" --jit on \
        > "$O/G1e-200m-$v.serial" 2> "$O/G1e-200m-$v.err"
    instr=$(sed -n 's/^v86c boot: \([0-9]*\) instructions.*/\1/p' "$O/G1e-200m-$v.err")
    state=$(sed -n 's/^v86c boot: [0-9]* instructions, //p' "$O/G1e-200m-$v.err")
    printf '%-14s selftest rc=%s [%s] | bench 200 M: instr=%s state=%s\n' \
        "$v" "$selrc" "$sel" "$instr" "$state" | tee -a "$O/G1e-unsupported-swap.txt"
done
echo "-- the pins the bench asserts (EXPECT_INSTRS / EXPECT_STATE):" \
    | tee -a "$O/G1e-unsupported-swap.txt"
rg -n '^EXPECT_INSTRS|^EXPECT_STATE' tests/perf_bench.sh | tee -a "$O/G1e-unsupported-swap.txt"

cp "$F" "$T"
cmp -s "$F" "$T" && echo "restored: ok" | tee -a "$O/G1e-unsupported-swap.txt" \
                 || echo "restored: FAILED" | tee -a "$O/G1e-unsupported-swap.txt"
