#!/bin/sh
# tests/gate-m2-5-determinism.sh -- §7.3's REPRODUCIBILITY gate, the NATIVE lane
# (HANDOFF=v86det).
#
# THE CLAIM: souche's guest boot is REPRODUCIBLE.  The SAME image and the SAME
# cmdline, booted TWICE on build/v86c, produce BYTE-IDENTICAL serial and virtio
# transcripts.  No normalization, no filter: `cmp` is the assertion.  A line that
# legitimately differs is a FINDING (the fix is in src/, not in this file) --
# weakening this comparison to make it pass is exactly what it exists to prevent.
#
# WHY IT IS AN ASYMMETRY: the pre-fix tree has TWO host-entropy sites in the
# port's own device/CPU code -- the RTC's time base (src/hw/rtc.c's host_now_ms,
# clock_gettime(CLOCK_REALTIME) = upstream's Date.now()) and RDRAND
# (src/cpu/instructions_0f.c's rdrand_value, /dev/urandom).  This gate FAILS on
# the pre-fix binary and PASSES after, on the same pair of runs; results/
# m2-5-det-* carries both.
#
# THE MACHINERY IS THE SLICE-3 GATE'S, not a new boot path: the pinned kernel /
# initrd / disk / minibios-80m and the same idle=poll cmdline, re-verified by
# `tests/gate-m2-5-guest.sh pin` before anything boots.
#
# THE INSTRUCTION LIMIT is below 2**32-1 for the same MEASURED reason the slice-3
# gate records: at or above it nothing stops on either machine (it looks exactly
# like a hang).  A run ends at its limit; PID1 never halts.
#
# usage: sh tests/gate-m2-5-determinism.sh [check|run1|run2|all]
#   check  re-run slice 3's pins (fast, no boot)
#   run1   boot the C leg into results/m2-5-det-run1-*
#   run2   boot it again into results/m2-5-det-run2-*
#   all    check + run1 + run2 + the byte compare  (default; ~2 x 80 s MEASURED)

set -u
cd "$(dirname "$0")/.."

KERNEL="${KERNEL:-tests/images/vmlinuz-i386-v86guest}"
INITRD="${INITRD:-tests/images/guest-initrd-fxdisk.xz}"
DISK_FMT="${DISK_FMT:-tests/images/disk-fmt-64m.img}"
# the 64 MiB disk is not tracked (its xz is): materialise the default path on
# first use -- see tests/disk-fmt.sh
. tests/disk-fmt.sh
FW="${FW:-build/minibios-80m.bin}"
V86C="${V86C:-build/v86c}"
WORK="${WORK:-/tmp/gate-m2-5-det}"
R=results
MEM=80
LIMIT="${LIMIT:-4000000000}"
GATE3="tests/gate-m2-5-guest.sh"

fails=0
pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; fails=$((fails + 1)); }
die()  { echo "gate-m2-5-determinism: FATAL: $*" >&2; exit 2; }

# the store dir + the cmdline are RESOLVED, never remembered: the store dir is
# parsed out of the archive by the slice-3 gate itself (its `pin` mode walks the
# cpio headers), and the cmdline is the byte-for-byte string that gate boots with.
# STORE_DIR is read from the slice-3 gate's own pin output so the two cannot drift.
cmdline() {
    printf '%s' "console=hvc0 console=ttyS0 tsc=unstable idle=poll rdinit=$STORE_DIR/fx-init fx.store=/fx/store"
}

check() {
    echo "### THE SLICE-3 GATE'S OWN PIN CHECK (the same inputs, the same cmdline)"
    _out=$(sh "$GATE3" pin 2>&1)
    printf '%s\n' "$_out" | sed 's/^/  /'
    printf '%s\n' "$_out" | grep -q '^### m2-5 slice-3 gate: 0 FAILING' \
        || die "slice 3's pin check did not pass -- the inputs are not the pinned ones"
    STORE_DIR=$(printf '%s\n' "$_out" | sed -n 's|^  PASS  the store dir is the pinned one (\(.*\))$|\1|p')
    [ -n "$STORE_DIR" ] || die "cannot read the store dir out of $GATE3 pin"
    echo "  the cmdline this gate boots twice:"
    echo "    $(cmdline)"
}

boot() { # boot NAME
    _name=$1
    [ -n "${STORE_DIR:-}" ] || die "check must run first (STORE_DIR unresolved)"
    _disk="$WORK/$_name-disk.img"
    cp "$DISK_FMT" "$_disk" || die "cannot copy $DISK_FMT"
    echo "\$ $V86C --bzimage $KERNEL $LIMIT --cmdline '<above>' --hda $_disk \\"
    echo "      --initrd $INITRD --bios $FW --memory $MEM --serial $R/m2-5-det-$_name-serial.txt"
    timeout 5400 "$V86C" --bzimage "$KERNEL" "$LIMIT" --cmdline "$(cmdline)" --hda "$_disk" \
        --initrd "$INITRD" --bios "$FW" --memory "$MEM" \
        --virtio-console "$R/m2-5-det-$_name-virtio.txt" \
        --serial "$R/m2-5-det-$_name-serial.txt" 2> "$R/m2-5-det-$_name-stderr.txt"
    echo "  port rc=$? (a run ends at its instruction LIMIT: PID1 never halts)"
    grep -E "instructions|hda .* sha256|virtio console" "$R/m2-5-det-$_name-stderr.txt" | sed 's/^/    /'
}

compare() {
    echo "### THE COMPARISON: byte-identical, no normalization"
    for stream in serial virtio; do
        _a=$R/m2-5-det-run1-$stream.txt
        _b=$R/m2-5-det-run2-$stream.txt
        [ -s "$_a" ] || die "no $_a (run1 did not boot)"
        [ -s "$_b" ] || die "no $_b (run2 did not boot)"
        echo "  $stream: $(wc -c < "$_a") vs $(wc -c < "$_b") bytes"
        echo "    $(sha256sum "$_a" | cut -d' ' -f1)  run1"
        echo "    $(sha256sum "$_b" | cut -d' ' -f1)  run2"
        if cmp -s "$_a" "$_b"; then
            pass "the two runs' $stream transcripts are BYTE-IDENTICAL"
        else
            fail "the two runs' $stream transcripts DIFFER"
            cmp "$_a" "$_b" 2>&1 | sed 's/^/    /' | head -3
            # the differing lines are the finding: print them, whole
            diff "$_a" "$_b" > "$R/m2-5-det-$stream-diff.txt"
            echo "    the diff ($(wc -l < "$R/m2-5-det-$stream-diff.txt") lines) is kept in $R/m2-5-det-$stream-diff.txt:"
            sed 's/^/      /' "$R/m2-5-det-$stream-diff.txt" | head -40
        fi
    done

    # THE SECOND INSTRUMENT, and it is NOT redundant: the port prints the total number of
    # guest instructions it executed when the run hits its limit (stderr).  That number is
    # the guest's own executable path -- it changes if the guest takes a different branch,
    # or if a device injects an interrupt at a different point -- and it is INVISIBLE to the
    # serial transcript whenever the divergence is in code that prints nothing.
    # MEASURED, and it is why this check exists: on the PRE-FIX tree the transcripts of two
    # runs were byte-identical while their instruction counts were NOT (4000057327 vs
    # 4000057221 at this same limit; the slice-3 evidence's own run stopped at 4000032959),
    # because the RTC's interrupt scheduling read the HOST clock and IRQ8 was therefore
    # delivered at a wall-clock-dependent instruction.  A transcript-only gate would have
    # declared that tree reproducible.
    _c1=$(sed -n 's/^v86c boot: \([0-9]*\) instructions.*/\1/p' "$R/m2-5-det-run1-stderr.txt")
    _c2=$(sed -n 's/^v86c boot: \([0-9]*\) instructions.*/\1/p' "$R/m2-5-det-run2-stderr.txt")
    echo "  instructions executed: ${_c1:-<none>} vs ${_c2:-<none>}"
    if [ -n "$_c1" ] && [ "$_c1" = "$_c2" ]; then
        pass "the two runs executed the SAME instruction count ($_c1)"
    else
        fail "the two runs' instruction counts differ: '${_c1:-<none>}' vs '${_c2:-<none>}'"
        diff "$R/m2-5-det-run1-stderr.txt" "$R/m2-5-det-run2-stderr.txt" \
            > "$R/m2-5-det-stderr-diff.txt"
        sed 's/^/    /' "$R/m2-5-det-stderr-diff.txt" | head -10
    fi
}

mkdir -p "$WORK" "$R"
case "${1:-all}" in
    check) check ;;
    run1)  check; boot run1 ;;
    run2)  check; boot run2 ;;
    all)   check; boot run1; boot run2; compare ;;
    *)     echo "usage: sh tests/gate-m2-5-determinism.sh [check|run1|run2|all]"; exit 2 ;;
esac

echo
echo "### m2-5 determinism gate: $fails FAILING assertion(s)"
[ "$fails" = 0 ]
