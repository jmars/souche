#!/bin/bash
# perf-5-ab.sh -- the INTERLEAVED A/B rig for PERF-5's per-change attribution (G2).
#
# WHY NOT TWO SEQUENTIAL BLOCKS: this host's load drifts (PERF-1's own record: 9.0 -> 10.3
# across one 4-round run), and a change worth 2% is smaller than that drift.  The rig
# therefore alternates the two binaries INSIDE each pair, with the order flipped between
# pairs, and reports best AND median per arm -- the same protocol PERF-4 used for the cache's
# own 1.45x and PERF-1 for its inlining arms.
#
# EVERY RUN IS ASSERTED against the workload's identity, exactly as tests/perf_bench.sh does:
# 200,095,214 instructions, the terminal guest state, and the sha256 of the guest's serial
# output.  A run that does not match is a FAILURE of the rig, not a number to average: a
# faster run that executes a different workload is not evidence about this one.
#
# Usage: results/perf-5-ab.sh <binA> <binB> [pairs] [labelA] [labelB]
# Output: per-run lines, a summary block per arm, and the A->B ratio (best, median).

set -u

A=$1; B=$2; PAIRS=${3:-3}; LA=${4:-A}; LB=${5:-B}

KERNEL=tests/images/vmlinuz-i386
LIMIT=200000000
EXPECT_INSTRS=200095214
EXPECT_STATE="in_hlt=0, exception=-1, unimplemented=0, eip=0x21972c1 cs=0x10 cr0=0x60000011 cr3=0x0"
EXPECT_SERIAL_SHA="fae4924b13b07d441b0ad8458047d2f1d8abf0d1ec59876f44b88ef67d2d0b2a"

cd "$(dirname "$0")/.." || exit 2
for f in "$A" "$B"; do
    [ -x "$f" ] || { echo "perf-5-ab: $f is not executable" >&2; exit 2; }
done

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
: > "$TMP/a"; : > "$TMP/b"

echo "== perf-5-ab: $LA vs $LB  pairs=$PAIRS"
echo "load-before=$(cut -d' ' -f1-3 /proc/loadavg)"

run_one() { # $1=bin $2=arm $3=tag
    local bin=$1 arm=$2 tag=$3 start end real
    start=$(date +%s.%N)
    "$bin" --bzimage "$KERNEL" "$LIMIT" --cmdline "console=ttyS0" \
        > "$TMP/out" 2> "$TMP/err"
    local rc=$?
    end=$(date +%s.%N)
    real=$(awk -v s="$start" -v e="$end" 'BEGIN{printf "%.3f", e-s}')
    local instr state serial
    instr=$(sed -n 's/^v86c boot: \([0-9]*\) instructions.*/\1/p' "$TMP/err")
    state=$(sed -n 's/^v86c boot: [0-9]* instructions, //p' "$TMP/err")
    serial=$(sha256sum < "$TMP/out" | cut -d' ' -f1)
    if [ "$rc" != 0 ] || [ "$instr" != "$EXPECT_INSTRS" ] || \
       [ "$state" != "$EXPECT_STATE" ] || [ "$serial" != "$EXPECT_SERIAL_SHA" ]; then
        echo "perf-5-ab: $arm run FAILED the workload identity (rc=$rc instr=$instr)" >&2
        echo "perf-5-ab:   state  got \"$state\"" >&2
        echo "perf-5-ab:   serial got $serial" >&2
        exit 1
    fi
    printf '%-4s real %ss  instr=ok state=ok serial=ok\n' "$arm" "$real"
    printf '%s %s\n' "$real" "$tag" >> "$TMP/$arm"
}

for i in $(seq 1 "$PAIRS"); do
    if [ $((i % 2)) -eq 1 ]; then
        run_one "$A" a "$i"; run_one "$B" b "$i"
    else
        run_one "$B" b "$i"; run_one "$A" a "$i"
    fi
done

echo "load-after=$(cut -d' ' -f1-3 /proc/loadavg)"
report() { # $1=file $2=label
    python3 - "$1" "$2" <<'PY'
import sys, statistics
vals = [float(l.split()[0]) for l in open(sys.argv[1])]
print("%-10s n=%d best %.3f median %.3f worst %.3f" %
      (sys.argv[2], len(vals), min(vals), statistics.median(vals), max(vals)))
PY
}
report "$TMP/a" "$LA"
report "$TMP/b" "$LB"
python3 - "$TMP/a" "$TMP/b" "$LA" "$LB" <<'PY'
import sys, statistics
a = [float(l.split()[0]) for l in open(sys.argv[1])]
b = [float(l.split()[0]) for l in open(sys.argv[2])]
print("ratio %s/%s: best %.4fx  median %.4fx  (>1 means %s is slower)" %
      (sys.argv[3], sys.argv[4], min(b)/min(a), statistics.median(b)/statistics.median(a), sys.argv[4]))
PY
