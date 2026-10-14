#!/bin/bash
# G1c (JIT-FIX-2): EVERY leg of the gate that can see a JIT recipe-table swap, one mutant at a
# time.  Only the --jit on legs can: the two arrays are referenced ONLY from jit_codegen.c
# (grep: the sole other mention is a comment in the selftest), so with the JIT off the swap is
# dead code -- MEASURED for the all-swap variant in G1-allswap-summary.txt, where every --jit
# off leg was identical and only the --jit on legs moved.
set -u
cd "$(dirname "$0")/../.." || exit 2
F=results/jit-fix-2/jit_codegen.c.fixed
T=src/cpu/jit_codegen.c
O=results/jit-fix-2

node oracle/run.mjs tests/test.img 1000000 > "$O/G1c-tc-js.txt" 2>/dev/null

for v in jcc8-swap-bbe jcc8-swap-aea jcc32-swap-bbe jcc32-swap-aea; do
    cp "results/jit-fix-2/jit_codegen.c.$v" "$T" || exit 2
    make > "$O/G1c-make-$v.txt" 2>&1 || { echo "$v: MAKE FAILED"; continue; }

    python3 tests/nasm_run.py --stage run \
        --actual "build/v86c-nasm {img} {limit} --fixture --jit on" \
        --skip-list tests/nasm_skip_c.txt > "$O/G1c-nasm-jiton-$v.txt" 2>&1
    nasmrc=$?

    ./build/v86c tests/test.img 1000000 --jit on > "$O/G1c-tc-$v.txt" 2>/dev/null
    cmp -s "$O/G1c-tc-js.txt" "$O/G1c-tc-$v.txt" && tcv=SAME || tcv=DIFFERS

    ./build/v86c --bzimage tests/images/vmlinuz-i386 1500000000 --cmdline "console=ttyS0" --jit on \
        > "$O/G1c-boot-$v-serial.txt" 2> "$O/G1c-boot-$v-stderr.txt"
    bootrc=$?
    state=$(sed -n 's/^v86c boot: [0-9]* instructions, //p' "$O/G1c-boot-$v-stderr.txt")

    echo "$v: nasm(jit on) rc=$nasmrc | transcript(jit on) vs JS: $tcv | boot rc=$bootrc: $state"
done

cp "$F" "$T"
cmp -s "$F" "$T" && echo "restored: ok" || echo "restored: FAILED"
