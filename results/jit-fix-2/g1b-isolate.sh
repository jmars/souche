#!/bin/bash
# G1b isolation (JIT-FIX-2): WHICH of the four swaps does the gate's WIDER legs see?
# The selftest arms cannot see any of them (G1-allswap..., all 2778 checks green); this asks
# whether the `--jit on` transcript vs the JS reference can, one swap at a time.
set -u
cd "$(dirname "$0")/../.." || exit 2
F=results/jit-fix-2/jit_codegen.c.fixed
T=src/cpu/jit_codegen.c
O=results/jit-fix-2

node oracle/run.mjs tests/test.img 1000000 > "$O/G1b-tc-js.txt" 2>/dev/null

for v in fixed jcc8-swap-bbe jcc8-swap-aea jcc32-swap-bbe jcc32-swap-aea all-swap; do
    cp "results/jit-fix-2/jit_codegen.c.$v" "$T" || exit 2
    make > "$O/G1b-make-$v.txt" 2>&1 || { echo "$v: MAKE FAILED"; continue; }
    ./build/v86c tests/test.img 1000000 --jit on > "$O/G1b-tc-$v.txt" 2>/dev/null
    if cmp -s "$O/G1b-tc-js.txt" "$O/G1b-tc-$v.txt"; then
        echo "$v: transcript --jit on == JS reference  (the wider gate does NOT see it)"
    else
        echo "$v: transcript --jit on DIFFERS from JS  (the wider gate SEES it)"
    fi
done

cp "$F" "$T"
cmp -s "$F" "$T" && echo "restored: ok" || echo "restored: FAILED"
