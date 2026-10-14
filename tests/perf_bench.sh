#!/bin/bash
# perf_bench.sh -- the PERF-1 benchmark (HANDOFF=v86c-perf, unit PERF-1).
#
# THE REFERENCE WORKLOAD (fixed by the unit's brief):
#
#     <bin> --bzimage tests/images/vmlinuz-i386 200000000 --cmdline "console=ttyS0"
#
# i.e. 200,095,214 guest instructions of the Alpine i386 kernel boot.  It is the one
# workload the whole port already exercises end to end (it is the M2-2/M2-3/M2-4 boot
# gate's own command line), so a change measured on it is a change measured on a boot
# that has to keep working.
#
# PROTOCOL (stated so a reader can reproduce or refute the numbers):
#   * RUNS runs, all timed, no warmup discarded (the first run is reported like any
#     other: the workload re-reads its kernel from page cache every time, and there is
#     no JIT or per-run state to warm).
#   * BOTH `real` (wall clock, i.e. what the process costs a caller) and `user`+`sys`
#     are sampled, and `best` AND `median` are reported for each -- a single number is
#     not defensible on a machine with other work on it.
#   * The host's load average is printed before and after, with the core count, so a
#     contended run is visible rather than silently averaged in.
#   * The instruction count the binary itself reports on stderr is REQUIRED to be
#     identical across runs; a run whose count differs did not run the same workload,
#     and mixing it in would be comparing two things.  The script fails loudly.
#
#     WHAT THE COUNT IS, AND WHAT IT IS NOT (PERF-FIX-2, replacing a bare pin that a
#     reader could mistake for a statement about guest-visible behaviour).  The count is
#     the instruction_counter at the moment the run crossed the 200,000,000-instruction
#     LIMIT -- i.e. a batch boundary, not a property of the guest's results.  The GUEST
#     READS THE HOST CLOCK (rtc.c's CLOCK_REALTIME; microtick() itself is the instruction
#     counter, exec_boundary.c:85), so a change in the guest's timing loops can move the
#     boundary.  MEASURED on the current tree: 10 of 10 runs, and every recorded run in
#     results/perf-4/, report 200,095,214 exactly; the ONE deviation in this project's
#     record (200,095,212, reported by PERF-FIX as "host-dependent") was a mid-flight
#     M2 tree's sti batch-count bug, which M2 fixed and which ALSO moved the guest's own
#     /proc/interrupts line -- i.e. the pin had real detection power, so it is KEPT.
#     The count is therefore asserted ALONGSIDE the thing that must not change, the
#     guest-visible terminal state (EXPECT_STATE) and the guest's serial output bytes
#     (EXPECT_SERIAL_SHA); a failure in any of the three names which one changed.
#
#     THE VARIANCE IS ON THE OTHER WORKLOAD, stated so the distinction is not lost: the
#     boot-to-HLT kernel boot (579M instructions, results/m2-5-baliveau-kernel.md:171-175)
#     has a count that moves by tens of thousands run to run because the guest's own
#     timing decides when it halts.  THIS workload stops at a fixed limit, which is why
#     its count is stable.
#   * ALL stdout goes to a file; the numbers come from the shell's own TIMEFORMAT, not
#     from `time`'s stderr formatting of a pipeline.
#
# Usage: tests/perf_bench.sh [--runs N] [--bin PATH] [--limit N] [--label TEXT]
#                            [--samples FILE] [--image-kernel PATH]
#                            [--expect-instrs N] [--expect-state TEXT]
# Output: one summary block on stdout, and the raw per-run samples appended to
#         $SAMPLES (default: none) so the numbers can be re-checked.

set -u

RUNS=7
BIN=build/v86c
LIMIT=200000000
LABEL=""
SAMPLES=""
KERNEL=tests/images/vmlinuz-i386
EXPECT_INSTRS=200095214
# The guest-visible identity of the workload: the terminal machine state the binary
# prints after the count, and the sha256 of the guest's own serial output (stdout).  Both
# were MEASURED identical on every run of this tree, with the uop cache ON and OFF.
EXPECT_STATE="in_hlt=0, exception=-1, unimplemented=0, eip=0x21972c1 cs=0x10 cr0=0x60000011 cr3=0x0"
EXPECT_SERIAL_SHA="fae4924b13b07d441b0ad8458047d2f1d8abf0d1ec59876f44b88ef67d2d0b2a"

while [ $# -gt 0 ]; do
    case "$1" in
        --runs)   RUNS="$2"; shift 2 ;;
        --bin)    BIN="$2"; shift 2 ;;
        --limit)  LIMIT="$2"; shift 2 ;;
        --label)  LABEL="$2"; shift 2 ;;
        --samples) SAMPLES="$2"; shift 2 ;;
        --kernel) KERNEL="$2"; shift 2 ;;
        --expect-instrs) EXPECT_INSTRS="$2"; shift 2 ;;
        --expect-state)  EXPECT_STATE="$2"; shift 2 ;;
        *) echo "perf_bench.sh: unknown argument: $1" >&2; exit 2 ;;
    esac
done

cd "$(dirname "$0")/.." || exit 2

if [ ! -x "$BIN" ]; then
    echo "perf_bench.sh: $BIN is not executable" >&2
    exit 2
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "== perf_bench: ${LABEL:-unlabelled}"
echo "bin=$BIN limit=$LIMIT kernel=$KERNEL runs=$RUNS"
echo "cores=$(nproc) load-before=$(cut -d' ' -f1-3 /proc/loadavg)"
echo "kernel=$(uname -r) cflags-host=$(cc --version | head -1)"

: > "$TMP/times"
echo "asserting: instruction count $EXPECT_INSTRS"
echo "asserting: terminal guest state \"$EXPECT_STATE\""
echo "asserting: serial output sha256 $EXPECT_SERIAL_SHA"
for i in $(seq 1 "$RUNS"); do
    # TIMEFORMAT: real, user, sys in seconds; the command's own output goes to a file.
    TIMEFORMAT='%R %U %S'
    { time "$BIN" --bzimage "$KERNEL" "$LIMIT" --cmdline "console=ttyS0" \
        > "$TMP/out.$i" 2> "$TMP/err.$i" ; } 2> "$TMP/t.$i"
    rc=$?
    read -r real user sys < "$TMP/t.$i"
    instr=$(sed -n 's/^v86c boot: \([0-9]*\) instructions.*/\1/p' "$TMP/err.$i")
    # the guest-visible state, verbatim from the boot line (everything after "instructions")
    state=$(sed -n 's/^v86c boot: [0-9]* instructions, //p' "$TMP/err.$i")
    serial=$(sha256sum < "$TMP/out.$i" | cut -d' ' -f1)
    if [ "$rc" != 0 ]; then
        echo "perf_bench: run $i exited $rc" >&2
        cat "$TMP/err.$i" >&2
        exit 1
    fi
    if [ "$instr" != "$EXPECT_INSTRS" ]; then
        echo "perf_bench: run $i executed $instr instructions, expected $EXPECT_INSTRS" >&2
        echo "perf_bench: what CHANGED is the instruction count: it is the batch boundary" >&2
        echo "perf_bench: nearest the $LIMIT-instruction limit, and the guest reads the" >&2
        echo "perf_bench: host-clock RTC in its timing loops, so a change here means the" >&2
        echo "perf_bench: guest's execution path (not its results) moved.  The same pin" >&2
        echo "perf_bench: caught M2's sti batch-count bug (200,095,212)." >&2
        echo "perf_bench: the workload was not the same -- refusing to report a number" >&2
        exit 1
    fi
    if [ "$state" != "$EXPECT_STATE" ]; then
        echo "perf_bench: run $i terminal guest state differs" >&2
        echo "perf_bench:   got      \"$state\"" >&2
        echo "perf_bench:   expected \"$EXPECT_STATE\"" >&2
        echo "perf_bench: THE GUEST-VISIBLE RESULT CHANGED -- this is the assertion that" >&2
        echo "perf_bench: must never pass silently (eip/cs/cr0/cr3/in_hlt/exception)." >&2
        exit 1
    fi
    if [ "$serial" != "$EXPECT_SERIAL_SHA" ]; then
        echo "perf_bench: run $i serial output sha256 differs" >&2
        echo "perf_bench:   got      $serial" >&2
        echo "perf_bench:   expected $EXPECT_SERIAL_SHA" >&2
        echo "perf_bench: the guest's OWN OUTPUT CHANGED -- see $TMP/out.$i" >&2
        exit 1
    fi
    printf '%s %s %s\n' "$real" "$user" "$sys" >> "$TMP/times"
    printf 'run %2d: real %ss user %ss sys %ss  instr=%s  state=ok serial=ok\n' \
        "$i" "$real" "$user" "$sys" "$instr"
done

echo "load-after=$(cut -d' ' -f1-3 /proc/loadavg)"

python3 - "$TMP/times" "$EXPECT_INSTRS" <<'PY'
import sys, statistics
path, instrs = sys.argv[1], int(sys.argv[2])
real, user, sys_ = [], [], []
for line in open(path):
    r, u, s = (float(x) for x in line.split())
    real.append(r); user.append(u); sys_.append(s)
cpu = [u + s for u, s in zip(user, sys_)]
def med(xs): return statistics.median(xs)
print("n=%d" % len(real))
print("real: best %.3f  median %.3f  worst %.3f" % (min(real), med(real), max(real)))
print("user: best %.3f  median %.3f  worst %.3f" % (min(user), med(user), max(user)))
print("cpu (user+sys): best %.3f  median %.3f" % (min(cpu), med(cpu)))
print("MIPS at best real: %.2f   at median real: %.2f" % (instrs/min(real)/1e6, instrs/med(real)/1e6))
PY
echo "verified (every run): $EXPECT_INSTRS instructions, the terminal guest state clause"
echo "                      above, and the guest's serial output sha256 -- every number in"
echo "                      this output times THAT workload; none of them asserts it."

if [ -n "$SAMPLES" ]; then
    {
        echo "# perf_bench ${LABEL:-unlabelled} bin=$BIN limit=$LIMIT runs=$RUNS $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "# real user sys"
        cat "$TMP/times"
    } >> "$SAMPLES"
    echo "raw samples appended to $SAMPLES"
fi
