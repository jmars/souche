#!/bin/bash
# jit-1-gates.sh -- UNIT JIT-1 (M1)'s G2: EVERY EXISTING GATE, TWICE (--jit off and --jit on),
# IN BOTH uop-cache MODES, with the counts unchanged -- plus the two things this unit's own
# footprint could break:
#
#   * `--jit off` (and no flag) must reproduce TODAY'S BYTES: checked against the SPIKE-ERA
#     transcript (results/jit-spike/transcript-c-jitoff.txt, produced by the tree as it stood
#     immediately before this unit's first edit) and against the PRE-SPIKE BINARY
#     (/tmp/jitspike/v86c-baseline, sha256 fe6e021d..., copied before the substrate landed) --
#     not merely against another invocation of the new one;
#   * the aarch64 leg must still compile (V86C_JIT_X64 == 0 compiles the whole backend out).
#
# The existing gates are run EXACTLY as PERF-5/JIT-SPIKE ran them (results/perf-5-gates.sh,
# results/jit-spike-gates.sh) so the counts are comparable: make / make selftest /
# make nasm (both cache modes, plus --jit on) / make transcript-diff (four legs) /
# the 1.5 G boot (three modes, final state + serial sha) / make bench (--jit off and --jit on).
#
# Usage: bash results/jit-1-gates.sh          (~15 minutes; the 1.5 G boots dominate)
set -u
cd "$(dirname "$0")/.." || exit 2
OUT=results/jit-1
BASE=/tmp/jitspike/v86c-baseline
mkdir -p "$OUT" "$OUT/aarch64"

note() { printf '%-52s rc=%s\n' "$1" "$2" | tee -a "$OUT/gates-summary.txt"; }

: > "$OUT/gates-summary.txt"
echo "== JIT-1 (M1) G2: $(date -u +%Y-%m-%dT%H:%M:%SZ) load=$(cut -d' ' -f1-3 /proc/loadavg)" \
    | tee -a "$OUT/gates-summary.txt"

# the two wrapper launch scripts: build/v86c is an APE whose re-exec fallback lives in a shell,
# and perf_bench.sh needs a single executable path (module the same way $(BUILD)/v86c-nasm is).
for m in off on; do
    printf '#!/bin/sh\nexec ./build/v86c "$@" --jit %s\n' "$m" > "$OUT/v86c-jit$m"
    chmod +x "$OUT/v86c-jit$m"
done

# ---- make / selftest ------------------------------------------------------------------
make > "$OUT/gates-make.txt" 2>&1; note "make" $?
sha256sum build/v86c | tee -a "$OUT/gates-summary.txt"

make selftest > "$OUT/gates-selftest.txt" 2>&1; note "make selftest" $?
grep -E "_selftest: [0-9]+ checks|selftest: all suites" "$OUT/gates-selftest.txt" \
    | tail -8 | tee -a "$OUT/gates-summary.txt"

# ---- nasm 118/118, FOUR legs (cache on/off x jit off/on), counts unchanged -------------
make nasm > "$OUT/gates-nasm-cacheon-jitoff.txt" 2>&1; note "make nasm (cache on, jit off)" $?
grep -E 'passed|failed|skipped' "$OUT/gates-nasm-cacheon-jitoff.txt" | tail -2 \
    | tee -a "$OUT/gates-summary.txt"

for leg in "off:--uop-cache off" "onjit:--jit on" "offjit:--uop-cache off --jit on"; do
    name=${leg%%:*}; flags=${leg#*:}
    python3 tests/nasm_run.py --stage run \
        --actual "build/v86c-nasm {img} {limit} --fixture $flags" \
        --skip-list tests/nasm_skip_c.txt > "$OUT/gates-nasm-$name.txt" 2>&1
    note "nasm with $flags" $?
    grep -E 'passed|failed|skipped' "$OUT/gates-nasm-$name.txt" | tail -2 \
        | tee -a "$OUT/gates-summary.txt"
done

# ---- the transcript gate, four legs ---------------------------------------------------
make transcript-diff > "$OUT/gates-transcript-cacheon-jitoff.txt" 2>&1
note "make transcript-diff (cache on, jit off)" $?
for leg in "cacheoff:--uop-cache off" "jitoff:--jit off" "jiton:--jit on"; do
    name=${leg%%:*}; flags=${leg#*:}
    ./build/v86c tests/test.img 1000000 $flags > "$OUT/transcript-c-$name.txt" 2>/dev/null
    node oracle/run.mjs tests/test.img 1000000 > "$OUT/transcript-js-$name.txt" 2>/dev/null
    if diff -q "$OUT/transcript-js-$name.txt" "$OUT/transcript-c-$name.txt" \
        > "$OUT/gates-transcript-$name.txt" 2>&1; then
        note "transcript-diff with $flags" 0
    else
        note "transcript-diff with $flags" 1
    fi
done

# ---- THE BYTE-IDENTITY GATE: --jit off against the PRE-CHANGE artifacts -----------------
./build/v86c tests/test.img 1000000 > "$OUT/transcript-c-noflag.txt" 2>/dev/null
./build/v86c tests/test.img 1000000 --jit off > "$OUT/transcript-c-jitoff2.txt" 2>/dev/null
if cmp -s "$OUT/transcript-c-noflag.txt" "$OUT/transcript-c-jitoff2.txt"; then
    note "--jit off == no flag (stdout)" 0
else
    note "--jit off == no flag (stdout)" 1
fi
if cmp -s results/jit-spike/transcript-c-jitoff.txt "$OUT/transcript-c-noflag.txt"; then
    note "--jit off == SPIKE-ERA (pre-change) transcript" 0
else
    note "--jit off == SPIKE-ERA (pre-change) transcript" 1
fi
if [ -x "$BASE" ]; then
    "$BASE" tests/test.img 1000000 > "$OUT/transcript-c-baseline.txt" 2>/dev/null
    if cmp -s "$OUT/transcript-c-baseline.txt" "$OUT/transcript-c-noflag.txt"; then
        note "--jit off == PRE-SPIKE BINARY's bytes" 0
    else
        note "--jit off == PRE-SPIKE BINARY's bytes" 1
    fi
else
    note "PRE-SPIKE baseline binary not present ($BASE)" 127
fi

# ---- the 200 M boot: final state, serial sha, and the JIT's coverage -------------------
for mode in off on cacheoff; do
    case "$mode" in
        off)      FLAG="--jit off" ;;
        cacheoff) FLAG="--jit off --uop-cache off" ;;
        *)        FLAG="--jit on --jit-stats --uop-cache-stats" ;;
    esac
    ./build/v86c --bzimage tests/images/vmlinuz-i386 200000000 --cmdline "console=ttyS0" $FLAG \
        > "$OUT/boot200m-$mode-serial.txt" 2> "$OUT/boot200m-$mode-stderr.txt"
    note "200 M boot ($mode)" $?
    sed -n 's/^v86c boot: 200095214 instructions, //p' "$OUT/boot200m-$mode-stderr.txt" \
        | tee -a "$OUT/gates-summary.txt"
done
echo "-- 200 M final-state compare + serial sha:" | tee -a "$OUT/gates-summary.txt"
a=$(sed -n 's/^v86c boot: [0-9]* instructions, //p' "$OUT/boot200m-off-stderr.txt")
b=$(sed -n 's/^v86c boot: [0-9]* instructions, //p' "$OUT/boot200m-on-stderr.txt")
c=$(sed -n 's/^v86c boot: [0-9]* instructions, //p' "$OUT/boot200m-cacheoff-stderr.txt")
if [ -n "$a" ] && [ "$a" = "$b" ] && [ "$a" = "$c" ]; then
    echo "IDENTICAL: $a" | tee -a "$OUT/gates-summary.txt"
else
    echo "DIFFER: on=\"$a\" off=\"$b\" cacheoff=\"$c\"" | tee -a "$OUT/gates-summary.txt"
fi
sha256sum "$OUT"/boot200m-*-serial.txt | tee -a "$OUT/gates-summary.txt"
cmp -s "$OUT/boot200m-on-serial.txt" "$OUT/boot200m-off-serial.txt" \
    && echo "the --jit on serial output is BYTE-IDENTICAL to --jit off" \
    | tee -a "$OUT/gates-summary.txt"
grep -h '^jit:' "$OUT/boot200m-on-stderr.txt" | tee -a "$OUT/gates-summary.txt"

# ---- THE 1.5 G BOOT, three modes (the spike's own gate, extended) ----------------------
for mode in on off onjit; do
    case "$mode" in
        off)   FLAG="--uop-cache off" ;;
        onjit) FLAG="--jit on" ;;
        *)     FLAG="" ;;
    esac
    ./build/v86c --bzimage tests/images/vmlinuz-i386 1500000000 --cmdline "console=ttyS0" $FLAG \
        > "$OUT/boot-1.5g-$mode-serial.txt" 2> "$OUT/boot-1.5g-$mode-stderr.txt"
    note "1.5 G boot ($mode)" $?
    sed -n 's/^v86c boot: [0-9]* instructions, //p' "$OUT/boot-1.5g-$mode-stderr.txt" \
        | tee -a "$OUT/gates-summary.txt"
done
xa=$(sed -n 's/^v86c boot: [0-9]* instructions, //p' "$OUT/boot-1.5g-on-stderr.txt")
xb=$(sed -n 's/^v86c boot: [0-9]* instructions, //p' "$OUT/boot-1.5g-off-stderr.txt")
xc=$(sed -n 's/^v86c boot: [0-9]* instructions, //p' "$OUT/boot-1.5g-onjit-stderr.txt")
if [ -n "$xa" ] && [ "$xa" = "$xb" ] && [ "$xa" = "$xc" ]; then
    echo "1.5 G final state IDENTICAL: $xa" | tee -a "$OUT/gates-summary.txt"
else
    echo "1.5 G DIFFER: on=\"$xa\" off=\"$xb\" onjit=\"$xc\"" | tee -a "$OUT/gates-summary.txt"
fi
sha256sum "$OUT"/boot-1.5g-*-serial.txt | tee -a "$OUT/gates-summary.txt"

# ---- the flag's own smoke test ---------------------------------------------------------
{
    echo "\$ ./build/v86c tests/test.img 1000 --jit on --jit-stats --uop-cache-stats"
    ./build/v86c tests/test.img 1000 --jit on --jit-stats --uop-cache-stats 2>&1 >/dev/null
    echo "\$ ./build/v86c tests/test.img 1000 --uop-cache off --jit on"
    ./build/v86c tests/test.img 1000 --uop-cache off --jit on 2>&1 >/dev/null
} > "$OUT/gates-jit-flag-smoke.txt" 2>&1
grep -q 'Linux VERIFIED' "$OUT/gates-jit-flag-smoke.txt" \
    && note "--jit on initializes and reports its caps" 0 \
    || note "--jit on initializes and reports its caps" 1
grep -q 'needs the uop cache' "$OUT/gates-jit-flag-smoke.txt" \
    && note "--uop-cache off --jit on is refused with its reason" 0 \
    || note "--uop-cache off --jit on is refused with its reason" 1

# ---- THE BENCHMARK, both ways (M1 is NOT expected to be faster: one recipe) -------------
tests/perf_bench.sh --runs 7 --bin "$PWD/$OUT/v86c-jitoff" --label "JIT-1 M1 --jit off" \
    --samples "$OUT/bench-jitoff.samples" > "$OUT/gates-bench-jitoff.txt" 2>&1
note "make bench (--jit off)" $?
tail -14 "$OUT/gates-bench-jitoff.txt" | tee -a "$OUT/gates-summary.txt"

tests/perf_bench.sh --runs 7 --bin "$PWD/$OUT/v86c-jiton" --label "JIT-1 M1 --jit on" \
    --samples "$OUT/bench-jiton.samples" > "$OUT/gates-bench-jiton.txt" 2>&1
note "make bench (--jit on)" $?
tail -14 "$OUT/gates-bench-jiton.txt" | tee -a "$OUT/gates-summary.txt"
python3 - "$OUT/bench-jitoff.samples" "$OUT/bench-jiton.samples" > "$OUT/gates-bench-ratio.txt" 2>&1 <<'PY'
import statistics, sys
def best(p):
    xs = [float(l.split()[0]) for l in open(p) if not l.startswith('#')]
    return min(xs), statistics.median(xs)
off, on = best(sys.argv[1]), best(sys.argv[2])
print("jit off  best %.3f  median %.3f s" % off)
print("jit on   best %.3f  median %.3f s" % on)
print("ratio (off/on, >1 means the JIT is faster): best %.3f  median %.3f"
      % (off[0] / on[0], off[1] / on[1]))
PY
cat "$OUT/gates-bench-ratio.txt" | tee -a "$OUT/gates-summary.txt"

# ---- the aarch64 posture ---------------------------------------------------------------
AARCH64=/home/jaye/.local/cosmo/bin/aarch64-linux-cosmo-cc
: > "$OUT/gates-aarch64.txt"
for f in src/cpu/jit_mem.c src/cpu/uop_cache.c src/cpu/jit_selftest.c src/cpu/jit_x64.c \
         src/cpu/jit_codegen.c src/cpu/jit_codegen_selftest.c; do
    "$AARCH64" -std=gnu11 -O2 -Wall -Wextra -foptimize-sibling-calls \
        -finline-functions-called-once -fpatchable-function-entry=0 -Werror \
        -I . -I src -c -o "$OUT/aarch64/$(basename "$f").o" "$f" >> "$OUT/gates-aarch64.txt" 2>&1
    note "aarch64 compile $(basename "$f")" $?
done

echo "== done $(date -u +%Y-%m-%dT%H:%M:%SZ) load=$(cut -d' ' -f1-3 /proc/loadavg)" \
    | tee -a "$OUT/gates-summary.txt"
