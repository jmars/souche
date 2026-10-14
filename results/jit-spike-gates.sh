#!/bin/bash
# jit-spike-gates.sh -- JIT-SPIKE's G4: every EXISTING gate, in BOTH cache modes, plus the two
# things this unit's own footprint could break:
#
#   * `--jit off` (and no flag) must reproduce TODAY'S BYTES exactly -- checked as the M0
#     transcript against a PRE-CHANGE BINARY (/tmp/jitspike/v86c-baseline, sha256
#     fe6e021da4d182e059176bbb7fdab883d31835cb2436d7916d0eddbfee7eebeb, copied before the
#     first edit), not merely against another invocation of the new one;
#   * `--jit on` must not change the machine: the JIT does nothing until something binds a
#     block, so the 1.5 G boot must reach the same final state with it enabled.
#
# The existing gates are run EXACTLY as PERF-5 ran them (results/perf-5-gates.sh), so the
# counts are comparable: make / make selftest / make nasm (both cache modes) /
# make transcript-diff / the 1.5 G boot (both modes) / make bench.
#
# Usage: bash results/jit-spike-gates.sh
set -u
cd "$(dirname "$0")/.." || exit 2
OUT=results/jit-spike
BASE=/tmp/jitspike/v86c-baseline
mkdir -p "$OUT"

note() { printf '%-46s rc=%s\n' "$1" "$2" | tee -a "$OUT/gates-summary.txt"; }

: > "$OUT/gates-summary.txt"
echo "== JIT-SPIKE G4: $(date -u +%Y-%m-%dT%H:%M:%SZ) load=$(cut -d' ' -f1-3 /proc/loadavg)" | tee -a "$OUT/gates-summary.txt"

make > "$OUT/gates-make.txt" 2>&1; note "make" $?
sha256sum build/v86c | tee -a "$OUT/gates-summary.txt"

make selftest > "$OUT/gates-selftest.txt" 2>&1; note "make selftest" $?
grep -E "checks|failures" "$OUT/gates-selftest.txt" | tail -8 | tee -a "$OUT/gates-summary.txt"

make nasm > "$OUT/gates-nasm-on.txt" 2>&1; note "make nasm (cache on)" $?
grep -E 'passed|failed|skipped' "$OUT/gates-nasm-on.txt" | tail -2 | tee -a "$OUT/gates-summary.txt"

python3 tests/nasm_run.py --stage run \
    --actual "build/v86c-nasm {img} {limit} --fixture --uop-cache off" \
    --skip-list tests/nasm_skip_c.txt > "$OUT/gates-nasm-off.txt" 2>&1
note "nasm with --uop-cache off" $?
grep -E 'passed|failed|skipped' "$OUT/gates-nasm-off.txt" | tail -2 | tee -a "$OUT/gates-summary.txt"

make transcript-diff > "$OUT/gates-transcript-on.txt" 2>&1; note "make transcript-diff (cache on)" $?
./build/v86c tests/test.img 1000000 --uop-cache off > "$OUT/transcript-c-off.txt" 2>/dev/null
node oracle/run.mjs tests/test.img 1000000 > "$OUT/transcript-js-off.txt" 2>/dev/null
if diff -q "$OUT/transcript-js-off.txt" "$OUT/transcript-c-off.txt" > "$OUT/gates-transcript-off.txt" 2>&1; then
    note "transcript-diff with --uop-cache off" 0
else
    note "transcript-diff with --uop-cache off" 1
fi

# ---- THE BYTE-IDENTITY GATE: --jit off against the PRE-CHANGE binary -----------------
./build/v86c tests/test.img 1000000 > "$OUT/transcript-c-plain.txt" 2>/dev/null
./build/v86c tests/test.img 1000000 --jit off > "$OUT/transcript-c-jitoff.txt" 2>/dev/null
"$BASE" tests/test.img 1000000 > "$OUT/transcript-c-baseline.txt" 2>/dev/null
if cmp -s "$OUT/transcript-c-baseline.txt" "$OUT/transcript-c-plain.txt"; then
    note "--jit off (no flag) == PRE-CHANGE binary bytes" 0
else
    note "--jit off (no flag) == PRE-CHANGE binary bytes" 1
fi
if cmp -s "$OUT/transcript-c-baseline.txt" "$OUT/transcript-c-jitoff.txt"; then
    note "--jit off (explicit) == PRE-CHANGE binary bytes" 0
else
    note "--jit off (explicit) == PRE-CHANGE binary bytes" 1
fi
head -3 "$OUT/transcript-c-plain.txt" | tee -a "$OUT/gates-summary.txt"

# ---- THE 1.5 G BOOT, both cache modes, plus --jit on ---------------------------------
for mode in on off onjit; do
    case "$mode" in
        off)   FLAG="--uop-cache off" ;;
        onjit) FLAG="--jit on" ;;
        *)     FLAG="" ;;
    esac
    ./build/v86c --bzimage tests/images/vmlinuz-i386 1500000000 --cmdline "console=ttyS0" $FLAG \
        > "$OUT/boot-$mode-serial.txt" 2> "$OUT/boot-$mode-stderr.txt"
    note "1.5 G boot ($mode)" $?
    sed -n 's/^v86c boot: [0-9]* instructions, //p' "$OUT/boot-$mode-stderr.txt" \
        | tee -a "$OUT/gates-summary.txt"
done
echo "-- final-state compare (the brief's gate):" | tee -a "$OUT/gates-summary.txt"
a=$(sed -n 's/^v86c boot: [0-9]* instructions, //p' "$OUT/boot-on-stderr.txt")
b=$(sed -n 's/^v86c boot: [0-9]* instructions, //p' "$OUT/boot-off-stderr.txt")
c=$(sed -n 's/^v86c boot: [0-9]* instructions, //p' "$OUT/boot-onjit-stderr.txt")
if [ -n "$a" ] && [ "$a" = "$b" ] && [ "$a" = "$c" ]; then
    echo "IDENTICAL: $a" | tee -a "$OUT/gates-summary.txt"
else
    echo "DIFFER: on=\"$a\" off=\"$b\" onjit=\"$c\"" | tee -a "$OUT/gates-summary.txt"
fi
echo "-- serial sha256 + the jit smoke lines:" | tee -a "$OUT/gates-summary.txt"
sha256sum "$OUT/boot-on-serial.txt" "$OUT/boot-off-serial.txt" "$OUT/boot-onjit-serial.txt" | tee -a "$OUT/gates-summary.txt"
grep -h '^v86c: jit' "$OUT/boot-onjit-stderr.txt" | tee -a "$OUT/gates-summary.txt"

# ---- THE FLAG'S OWN SMOKE TEST (the thing that caught this unit's own init bug) --------
{
    echo "\$ ./build/v86c tests/test.img 1000 --jit on --uop-cache-stats  (stderr)"
    ./build/v86c tests/test.img 1000 --jit on --uop-cache-stats 2>&1 >/dev/null | head -3
    echo "\$ ./build/v86c tests/test.img 1000                                     (stderr, default)"
    ./build/v86c tests/test.img 1000 2>&1 >/dev/null | head -3
    echo "\$ ./build/v86c tests/test.img 1000 --jit off                           (stderr)"
    ./build/v86c tests/test.img 1000 --jit off 2>&1 >/dev/null | head -3
    echo "\$ ./build/v86c tests/test.img 1000 --uop-cache off --jit on            (stderr)"
    ./build/v86c tests/test.img 1000 --uop-cache off --jit on 2>&1 >/dev/null | head -3
} > "$OUT/gates-jit-flag-smoke.txt" 2>&1
if grep -q 'Linux VERIFIED' "$OUT/gates-jit-flag-smoke.txt"; then
    note "--jit on initializes and reports its caps" 0
else
    note "--jit on initializes and reports its caps" 1
fi
if grep -q 'needs the uop cache' "$OUT/gates-jit-flag-smoke.txt"; then
    note "--uop-cache off --jit on is refused with the reason" 0
else
    note "--uop-cache off --jit on is refused with the reason" 1
fi
if [ -s /dev/null ]; then :; fi
./build/v86c tests/test.img 1000 2>&1 >/dev/null > "$OUT/gates-jit-off-stderr.txt"
./build/v86c tests/test.img 1000 --jit off 2>&1 >/dev/null > "$OUT/gates-jit-off-stderr2.txt"
if cmp -s "$OUT/gates-jit-off-stderr.txt" "$OUT/gates-jit-off-stderr2.txt"; then
    note "--jit off == no flag, on stderr too" 0
else
    note "--jit off == no flag, on stderr too" 1
fi
cat "$OUT/gates-jit-flag-smoke.txt" | tee -a "$OUT/gates-summary.txt"

# ---- the benchmark the whole project's numbers are quoted on -------------------------
make bench > "$OUT/gates-bench.txt" 2>&1; note "make bench" $?
tail -12 "$OUT/gates-bench.txt" | tee -a "$OUT/gates-summary.txt"

# ---- the aarch64 posture: the tree must still compile for the other APE ---------------
# The guard is V86C_JIT_X64 (src/cpu/jit_mem.h): on aarch64 the whole layer compiles out and
# the flag stays a no-op, so the cross build must succeed exactly as before.
AARCH64=/home/jaye/.local/cosmo/bin/aarch64-linux-cosmo-cc
if [ -x "$AARCH64" ] || command -v "$AARCH64" >/dev/null 2>&1; then
    rm -rf "$OUT/aarch64"; mkdir -p "$OUT/aarch64"
    for f in src/cpu/jit_mem.c src/cpu/uop_cache.c src/cpu/jit_selftest.c; do
        "$AARCH64" -std=gnu11 -O2 -Wall -Wextra -foptimize-sibling-calls \
            -finline-functions-called-once -fpatchable-function-entry=0 -Werror \
            -I . -I src -c -o "$OUT/aarch64/$(basename "$f").o" "$f" \
            >> "$OUT/gates-aarch64.txt" 2>&1
        note "aarch64 compile $(basename "$f")" $?
    done
else
    note "aarch64 compiler not found at $AARCH64" 127
fi

echo "== done: $(date -u +%Y-%m-%dT%H:%M:%SZ) load=$(cut -d' ' -f1-3 /proc/loadavg)" | tee -a "$OUT/gates-summary.txt"
