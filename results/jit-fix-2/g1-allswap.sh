#!/bin/bash
# G1 measurement (JIT-FIX-2): run the WHOLE GATE's legs against the all-swap tree, to establish
# the reviewer's claim ("the swap pairs are invisible to the gate") before any fix.
# Run as: bash results/jit-fix-2/g1-allswap.sh
set -u
cd "$(dirname "$0")/../.." || exit 2
F=results/jit-fix-2/jit_codegen.c.fixed
T=src/cpu/jit_codegen.c
V=results/jit-fix-2/jit_codegen.c.all-swap
O=results/jit-fix-2

cp "$V" "$T" || exit 2
echo "installed all-swap (cmp vs fixed: $(cmp -s "$V" "$F" && echo IDENTICAL || echo differs))"

# 1. the two selftest suites
make jit-codegen-selftest > "$O/G1-allswap-jit-codegen-selftest.txt" 2>&1
echo "jit-codegen-selftest rc=$? $(grep -E '_selftest: [0-9]+ checks' "$O/G1-allswap-jit-codegen-selftest.txt" | tail -1)"
make jit-selftest > "$O/G1-allswap-jit-selftest.txt" 2>&1
echo "jit-selftest      rc=$? $(grep -E '_selftest: [0-9]+ checks' "$O/G1-allswap-jit-selftest.txt" | tail -1)"

# 2. the nasm corpus, all three legs
make nasm > "$O/G1-allswap-nasm-default.txt" 2>&1
echo "nasm default      rc=$? $(grep -E '^\[.\] [0-9]+ of' "$O/G1-allswap-nasm-default.txt" | tail -1)"
for leg in "cacheoff:--uop-cache off" "jiton:--jit on"; do
    name=${leg%%:*}; flags=${leg#*:}
    python3 tests/nasm_run.py --stage run \
        --actual "build/v86c-nasm {img} {limit} --fixture $flags" \
        --skip-list tests/nasm_skip_c.txt > "$O/G1-allswap-nasm-$name.txt" 2>&1
    echo "nasm $flags rc=$? $(grep -E '^\[.\] [0-9]+ of' "$O/G1-allswap-nasm-$name.txt" | tail -1)"
done

# 3. the transcripts, both legs, against the JS reference
make transcript-diff > "$O/G1-allswap-transcript-default.txt" 2>&1
echo "transcript-diff default rc=$?"
for leg in "jitoff:--jit off" "jiton:--jit on"; do
    name=${leg%%:*}; flags=${leg#*:}
    ./build/v86c tests/test.img 1000000 $flags > "$O/G1-allswap-tc-$name.txt" 2>/dev/null
    node oracle/run.mjs tests/test.img 1000000 > "$O/G1-allswap-tc-js.txt" 2>/dev/null
    cmp -s "$O/G1-allswap-tc-js.txt" "$O/G1-allswap-tc-$name.txt" \
        && echo "transcript $flags vs JS: IDENTICAL" || echo "transcript $flags vs JS: DIFFERS"
done

# 4. the 1.5 G boot
for mode in off on; do
    [ "$mode" = off ] && FLAG="" || FLAG="--jit on"
    ./build/v86c --bzimage tests/images/vmlinuz-i386 1500000000 --cmdline "console=ttyS0" $FLAG \
        > "$O/G1-allswap-boot-$mode-serial.txt" 2> "$O/G1-allswap-boot-$mode-stderr.txt"
    echo "boot 1.5G ($mode) rc=$? $(sed -n 's/^v86c boot: [0-9]* instructions, //p' "$O/G1-allswap-boot-$mode-stderr.txt")"
done
cmp -s "$O/G1-allswap-boot-off-serial.txt" "$O/G1-allswap-boot-on-serial.txt" \
    && echo "1.5 G serial: IDENTICAL off vs on" || echo "1.5 G serial: DIFFERS"

cp "$F" "$T"
cmp -s "$F" "$T" && echo "restored: ok" || echo "restored: FAILED"
