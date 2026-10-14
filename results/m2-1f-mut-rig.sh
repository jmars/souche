#!/bin/sh
# results/m2-1f-mut-rig.sh -- M2-1F.s ASYMMETRY RIG (G1: the D-F1 port contract; G2: the D-F2 clock).
#
# For each single-line mutation below: apply it, rebuild build/hw_selftest, run it, and
# REQUIRE a non-zero exit (the mutation must be visible); then restore the source and
# re-require a zero exit (the restored tree must be green again).  The asymmetry -- fails
# before, passes after -- is the proof that the selftest's assertions are load-bearing.
#
# A mutation that does not compile, or that the selftest cannot see, is NOT a pass: it is
# printed as such and must be replaced.  (m1c's rig records the same rule; two of M1a-2's
# first-attempt mutations were invalid for exactly these two reasons.)
#
# Usage: sh results/m2-1-mut-rig.sh          (from the project root)

set -u
cd "$(dirname "$0")/.." || exit 2

rig_fail=0

run_case() {
    label="$1"; file="$2"; old="$3"; new="$4"

    cp "$file" /tmp/m2-1f-mut-backup.c
    if ! python3 - "$file" "$old" "$new" <<'PY'
import sys
path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(path).read()
if s.count(old) != 1:
    sys.exit("MUTATION TARGET NOT UNIQUE (%d matches): %s" % (s.count(old), old))
open(path, 'w').write(s.replace(old, new))
PY
    then
        echo "CASE $label: INVALID (could not apply the mutation)"
        cp /tmp/m2-1f-mut-backup.c "$file"
        rig_fail=1
        return
    fi

    if make build/hw_selftest >/tmp/m2-1f-mut-build.log 2>&1; then
        if ./build/hw_selftest >/tmp/m2-1f-mut-run.log 2>&1; then
            echo "CASE $label: INVALID (the mutation COMPILED and the selftest still PASSED)"
            rig_fail=1
        else
            echo "CASE $label: PASS (mutated -> hw_selftest FAILS, $(grep -c '^FAIL' /tmp/m2-1f-mut-run.log) failing assertion(s))"
            grep '^FAIL' /tmp/m2-1f-mut-run.log | head -2 | sed 's/^/    /'
        fi
    else
        echo "CASE $label: INVALID (the mutation did not compile)"
        rig_fail=1
    fi

    cp /tmp/m2-1f-mut-backup.c "$file"
    if make build/hw_selftest >/tmp/m2-1f-mut-build.log 2>&1 && ./build/hw_selftest >/tmp/m2-1f-mut-run.log 2>&1; then
        echo "    restored -> hw_selftest PASSES again ($(grep -o '[0-9]* checks, [0-9]* failures' /tmp/m2-1f-mut-run.log))"
    else
        echo "    restored -> hw_selftest STILL FAILS (the restore is broken!)"
        rig_fail=1
    fi
}
# For each single-line mutation below: apply it, rebuild build/hw_selftest, run it, and
# REQUIRE a non-zero exit (the mutation must be visible in the new cases); then restore the
# source and re-require a zero exit.  A mutation the suite cannot see proves nothing, so it
# is printed as INVALID and must be replaced (the M2-1 rig records the same rule).

echo "=== M2-1F asymmetry (each mutation alone, then restored) ==="
date -u

# F1.1: an unclaimed READ must answer the reference's 0xFF, not a fabricated 0
run_case "F1.1 unclaimed read8 answers 0xFF" src/hw/io.c \
    "    int32_t value = entry->read8(entry->device, (int32_t)((uint32_t)port & 0xFFFF));" \
    "    int32_t value = entry->read8 == empty_port_read8 ? 0 : entry->read8(entry->device, (int32_t)((uint32_t)port & 0xFFFF));"

# F1.2: an unclaimed WRITE must be discarded -- it must not stop the machine
run_case "F1.2 unclaimed write8 is discarded" src/hw/io.c \
    "        io_log_unclaimed_write(8, port, value);" \
    "        io_stop_width(port, 8, \"io_port_write\");"

# F2.1: the clock's scale -- the instruction counter is MICROSECONDS and microtick() is its
# milliseconds (dropping the division is exactly M1c's 1000x-fast seat)
run_case "F2.1 microtick divides by 1000" src/cpu/exec_boundary.c \
    "	return (double)cpu->instruction_counter / INSTRUCTIONS_PER_MILLISECOND;" \
    "	return (double)cpu->instruction_counter;"

# F2.2: the same scale, through the named constant -- a 10x error must be visible too (a
# test that only catches a 1000x error is not testing the ratio)
run_case "F2.2 the clock's named constant" src/cpu/exec_boundary.c \
    "#define INSTRUCTIONS_PER_MILLISECOND 1000.0" \
    "#define INSTRUCTIONS_PER_MILLISECOND 100.0"

# F2.3: the ACPI PM timer's own rate must stay consistent with the clock's unit
run_case "F2.3 the PM timer's rate" src/hw/acpi.c \
    "    int32_t t = (int32_t)round(now * (PMTIMER_FREQ_SECONDS / 1000.0));" \
    "    int32_t t = (int32_t)round(now * PMTIMER_FREQ_SECONDS);"

echo
if [ "$rig_fail" -eq 0 ]; then
    echo "=== all 5 mutations were VISIBLE (and the restore is green) ==="
else
    echo "=== AT LEAST ONE MUTATION WAS INVALID (see INVALID above) ==="
fi
exit "$rig_fail"
