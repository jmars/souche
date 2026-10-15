#!/bin/sh
# The other axis: with each jcc slot-swap variant installed, which SUITE catches it?
set -u
cd ~/futaie/souche || exit 2
SRC=src/cpu/jit_codegen.c
KEEP=/tmp/jitcg.fixed.c
OUT=/tmp/jitcg-repro
cp "$SRC" "$KEEP" || exit 2

for v in jcc8-swap-bbe jcc8-swap-aea jcc32-swap-bbe jcc32-swap-aea jcc8-swap-b0 jcc8-swap-s6 baseline; do
    if [ "$v" = baseline ]; then cp "$KEEP" "$SRC"; else cp "$OUT/$v.c" "$SRC"; fi
    make "$OUT/.never" >/dev/null 2>&1  # no-op guard, keeps shell history honest
    make build/cpu_jit_codegen_selftest >"$OUT/suite-build.$v.log" 2>&1
    rb=$?
    ./build/cpu_jit_codegen_selftest >"$OUT/suite-codegen.$v.txt" 2>&1
    rc=$?
    make build/cpu_jit_selftest >/dev/null 2>&1
    ./build/cpu_jit_selftest >"$OUT/suite-jit.$v.txt" 2>&1
    rj=$?
    printf '%-16s codegen_selftest rc=%-3s %-60s | jit_selftest rc=%s %s\n' "$v" "$rc" \
        "$(rg -o 'jit_codegen_selftest: [0-9]+ checks, [0-9]+ failures' "$OUT/suite-codegen.$v.txt" | head -1)" \
        "$rj" "$(rg -o 'jit-selftest: [0-9]+ checks, [0-9]+ failures' "$OUT/suite-jit.$v.txt" | head -1)"
done
cp "$KEEP" "$SRC"
cmp -s "$KEEP" "$SRC" && echo "restore check: cmp clean" || echo "RESTORE FAILED"
