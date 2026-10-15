#!/bin/sh
# results/apic-1-mut-rig.sh -- APIC-1's ASYMMETRY RIG (the brief's G5).
#
# The same protocol as m2-1-mut-rig.sh: for each single-line mutation below, apply it,
# rebuild build/hw_selftest, run it, and REQUIRE a non-zero exit (the mutation must be
# VISIBLE); then restore the source and re-require a zero exit.  A mutation that does not
# compile, or that the selftest cannot see, is printed as INVALID and fails the rig.
#
# The mutations cover the two surfaces the brief names -- the delivery path and the timer
# -- plus the IOAPIC bound (the audit's guest-controlled-index class: relaxing the range
# test by one register must become an out-of-bounds table access the selftest sees).
#
# Usage: sh results/apic-1-mut-rig.sh          (from the project root; ~2 min)

set -u
cd "$(dirname "$0")/.." || exit 2

rig_fail=0

run_case() {
    label="$1"; file="$2"; old="$3"; new="$4"

    cp "$file" /tmp/apic-1-mut-backup.c
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
        cp /tmp/apic-1-mut-backup.c "$file"
        rig_fail=1
        return
    fi

    if make build/hw_selftest >/tmp/apic-1-mut-build.log 2>&1; then
        if ./build/hw_selftest >/tmp/apic-1-mut-run.log 2>&1; then
            echo "CASE $label: INVALID (the mutation COMPILED and the selftest still PASSED)"
            rig_fail=1
        else
            echo "CASE $label: PASS (mutated -> hw_selftest FAILS, $(grep -c '^FAIL' /tmp/apic-1-mut-run.log) failing assertion(s))"
            grep '^FAIL' /tmp/apic-1-mut-run.log | head -2 | sed 's/^/    /'
        fi
    else
        echo "CASE $label: INVALID (the mutation did not compile)"
        rig_fail=1
    fi

    cp /tmp/apic-1-mut-backup.c "$file"
    if make build/hw_selftest >/tmp/apic-1-mut-build.log 2>&1 && ./build/hw_selftest >/tmp/apic-1-mut-run.log 2>&1; then
        echo "    restored -> hw_selftest PASSES again ($(grep -o '[0-9]* checks, [0-9]* failures' /tmp/apic-1-mut-run.log))"
    else
        echo "    restored -> hw_selftest STILL FAILS (the restore is broken!)"
        rig_fail=1
    fi
}

# M1 -- the delivery path: a delivered vector never latches the IRR
run_case "M1 deliver skips the irr set" src/cpu/apic.c \
    "    register_set_bit(apic->irr, vector);

    if (is_level) {" \
    "    (void)vector;

    if (is_level) {"

# M2 -- the timer: the fire comparison becomes strict (the boundary moves one tick)
run_case "M2 timer fires one tick late" src/cpu/apic.c \
    "if (diff_in_ticks >= (uint64_t)apic->timer_initial_count) {" \
    "if (diff_in_ticks > (uint64_t)apic->timer_initial_count) {"

# M3 -- the divider table: /16 decodes as /8
run_case "M3 divider shift table wrong" src/cpu/apic.c \
    "apic->timer_divider_shift = divide_shift == 0b111 ? 0 : divide_shift + 1;" \
    "apic->timer_divider_shift = divide_shift == 0b111 ? 0 : divide_shift;"

# M4 -- the EOI: the isr bit survives the guest's EOI write
run_case "M4 eoi leaves isr set" src/cpu/apic.c \
    "                register_clear_bit(apic->isr, (uint8_t)highest_isr);" \
    "                (void)highest_isr;"

# M5 -- the IOAPIC bound: one register too far is indexed (the audit's class).  The READ
# arm is the target because the u32 one past ioredtbl_config[24] is
# ioredtbl_destination[0], which the delivery case PROGRAMMED (0x01000000) -- so the
# out-of-bounds read returns a neighbour's value, and the "one past" check sees it.  (The
# write arm's first attempt at this mutation corrupted an entry the selftest never reads
# again -- INVALID, and itself a lesson in what the assertions can see.)
run_case "M5 ioregsel range relaxed by one" src/cpu/ioapic.c \
    "                    if (ioapic->ioregsel >= IOAPIC_FIRST_IRQ_REG &&
                        ioapic->ioregsel < IOAPIC_LAST_IRQ_REG) {
                        /* THE BOUND (ioapic.h): the branch test above makes" \
    "                    if (ioapic->ioregsel >= IOAPIC_FIRST_IRQ_REG &&
                        ioapic->ioregsel <= IOAPIC_LAST_IRQ_REG) {
                        /* THE BOUND (ioapic.h): relaxed by the rig"

# M6 -- the remote EOI: the level entry keeps its remote IRR forever
run_case "M6 remote_eoi keeps the remote irr" src/cpu/ioapic.c \
    "            ioapic_instance.ioredtbl_config[i] &= ~IOAPIC_CONFIG_REMOTE_IRR;" \
    "            (void)config;"

# M7 -- the ack: the irr bit survives the acknowledge
run_case "M7 ack leaves the irr bit set" src/cpu/apic.c \
    "    register_clear_bit(apic->irr, (uint8_t)highest_irr_value);
    register_set_bit(apic->isr, (uint8_t)highest_irr_value);" \
    "    register_set_bit(apic->isr, (uint8_t)highest_irr_value);"

if [ "$rig_fail" -ne 0 ]; then
    echo "apic-1-mut-rig: FAILED (an invalid or invisible mutation is above)"
    exit 1
fi
echo "apic-1-mut-rig: all mutations visible, every restore green"
