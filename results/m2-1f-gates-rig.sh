#!/bin/sh
# results/m2-1f-gates-rig.sh -- M2-1F's gate capture (G1-G4), exit codes included.
# Usage: sh results/m2-1f-gates-rig.sh > results/m2-1f-gates.txt
set -u
cd "$(dirname "$0")/.." || exit 2

gate() {
    label="$1"; shift
    echo "=== $label"
    echo "\$ $*"
    "$@" 2>&1
    echo "RC=$?"
    echo
}

gate "G3.1 make (clean compile + the APE)" make
gate "G3.2 make selftest (all three suites)" make selftest
gate "G3.3 make oracle" make oracle
gate "G3.4 make determinism" make determinism
gate "G3.5 make transcript-diff" make transcript-diff
gate "G3.6 make nasm" make nasm

echo "=== G1: the port-0x80 probe, before/after (its own rig)"
sh results/m2-1f-g1-rig.sh
echo "RC=$?"
echo

echo "=== G2: asymmetry -- 5 mutations of the two decisions, each alone (its own rig)"
sh results/m2-1f-mut-rig.sh
echo "RC=$?"
echo

echo "=== D-F1's DIAGNOSTIC must decide nothing: the same suite with V86C_LOG_PORTS set"
V86C_LOG_PORTS=1 ./build/hw_selftest >/tmp/m2-1f-gates-log-off.txt 2>/tmp/m2-1f-gates-log-on.txt
echo "selftest with V86C_LOG_PORTS=1: RC=$? ; unclaimed-access lines on stderr:"
grep -c 'unclaimed' /tmp/m2-1f-gates-log-on.txt
grep 'unclaimed' /tmp/m2-1f-gates-log-on.txt | head -4
echo "stdout (the suite's own verdict) with the diagnostic ON:"
tail -1 /tmp/m2-1f-gates-log-off.txt
echo
echo "=== the same suite with the diagnostic OFF: stderr must be EMPTY"
./build/hw_selftest >/tmp/m2-1f-gates-log-off.txt 2>/tmp/m2-1f-gates-log-off-stderr.txt
echo "RC=$?"
echo "stderr lines: $(wc -l < /tmp/m2-1f-gates-log-off-stderr.txt)"
tail -1 /tmp/m2-1f-gates-log-off.txt
