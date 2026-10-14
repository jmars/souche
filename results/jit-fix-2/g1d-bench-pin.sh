#!/bin/bash
# G1d (JIT-FIX-2): does the BENCH's own pinned 200M leg (tests/perf_bench.sh, gated via note())
# see the jcc array swaps?  The bench asserts, per run, the instruction count at the limit AND
# the terminal guest state AND the serial sha -- and its row IS note()d in the gate script.
# Two runs per variant, so a genuine catch can be told from host-clock jitter.
set -u
cd "$(dirname "$0")/../.." || exit 2
F=results/jit-fix-2/jit_codegen.c.fixed
T=src/cpu/jit_codegen.c
O=results/jit-fix-2

: > "$O/G1d-bench200m.txt"
for v in fixed jcc8-swap-bbe jcc8-swap-aea jcc32-swap-bbe jcc32-swap-aea all-swap; do
    cp "results/jit-fix-2/jit_codegen.c.$v" "$T" || exit 2
    make > "$O/G1d-make-$v.txt" 2>&1 || { echo "$v: MAKE FAILED"; continue; }
    for run in 1 2; do
        ./build/v86c --bzimage tests/images/vmlinuz-i386 200000000 --cmdline "console=ttyS0" \
            --jit on > "$O/G1d-200m-$v-$run.serial" 2> "$O/G1d-200m-$v-$run.err"
        rc=$?
        instr=$(sed -n 's/^v86c boot: \([0-9]*\) instructions.*/\1/p' "$O/G1d-200m-$v-$run.err")
        state=$(sed -n 's/^v86c boot: [0-9]* instructions, //p' "$O/G1d-200m-$v-$run.err")
        serial=$(sha256sum < "$O/G1d-200m-$v-$run.serial" | cut -d' ' -f1)
        printf '%-16s run%s rc=%s instr=%s state=%s serial=%s\n' \
            "$v" "$run" "$rc" "$instr" "$state" "${serial:0:16}" | tee -a "$O/G1d-bench200m.txt"
    done
done

cp "$F" "$T"
cmp -s "$F" "$T" && echo "restored: ok" | tee -a "$O/G1d-bench200m.txt" \
                 || echo "restored: FAILED" | tee -a "$O/G1d-bench200m.txt"
echo "-- the bench's own pins:" | tee -a "$O/G1d-bench200m.txt"
rg -n '^EXPECT_INSTRS|^EXPECT_STATE|^EXPECT_SERIAL_SHA' tests/perf_bench.sh | tee -a "$O/G1d-bench200m.txt"
