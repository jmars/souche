#!/bin/sh
# results/m1c-mut-rig.sh -- the M1c asymmetry rig (the brief's G4: at least three
# single-line mutations, each failing then restored-green).
#
# WHAT IT PROVES.  A gate is only a gate if something can make it fail.  Each mutation
# below is ONE line of exceptions.c/pic.c with ONE semantic change; the rig rebuilds the
# crafted M1c test binary (the real exceptions.c/pic.c -- the gate G3 exists for), runs
# it, restores the source and re-runs to show green again.  Two mutations are also run
# against the NASM corpus, because that measures the OTHER half of the claim the report
# makes: which of these semantics the corpus can see and which only the crafted binary
# can (the corpus boots a multiboot guest with idtr_size 0, so a delivered vector is not
# reachable in it at all).
#
# A MUTATION THAT DOES NOT COMPILE IS REPORTED AS SUCH, not as a pass: an invalid
# mutation proves nothing.  The source is restored on every exit path (trap), and the
# backup lives in /tmp with a fixed name that is REMOVED before each run -- an earlier
# version of this rig left a stale backup behind, and the next run's backup then saved a
# MUTATED file, which is exactly how a rig silently turns into a corruption.
#
# Usage: sh results/m1c-mut-rig.sh [name ...]   (default: all)
CC=/home/jaye/.local/cosmo/bin/cosmocc
CFLAGS="-std=gnu11 -O2 -Wall -Wextra -Werror"
cd "$(dirname "$0")/.."
BAK=/tmp/m1c-mut.bak
ACTIVE=
ERRS=/tmp/m1c-mut.err

restore_now() {
    if [ -n "$ACTIVE" ] && [ -f "$BAK" ]; then mv "$BAK" "$ACTIVE"; fi
    ACTIVE=
}
trap 'restore_now' EXIT INT TERM

build_m1c() {
    $CC $CFLAGS -c -o build/cpu/exceptions.o src/cpu/exceptions.c 2>"$ERRS" &&
    $CC $CFLAGS -c -o build/cpu/pic.o src/cpu/pic.c 2>>"$ERRS" &&
    $CC $CFLAGS -Wl,--gc-sections -o build/cpu_m1c_selftest src/cpu/m1c_selftest.c \
        build/cpu/*.o build/sf/softfloat.o 2>>"$ERRS"
}

run_m1c() {
    if ./build/cpu_m1c_selftest >/tmp/m1c-mut.out 2>&1; then
        echo "PASS (not caught)"
    else
        echo "FAIL ($(grep -c '^FAIL' /tmp/m1c-mut.out) assertion(s))"
    fi
}

mutate() { # file
    rm -f "$BAK"
    cp "$1" "$BAK"
    ACTIVE="$1"
    python3 results/m1c-mutate.py "$1"
}

case_one() {
    name="$1"; file="$2"; old="$3"; new="$4"; with_corpus="$5"
    echo "=== $name"
    export MUT_OLD="$3" MUT_NEW="$4"
    if ! mutate "$file"; then
        echo "  MUTATION REJECTED (target not unique)"
        restore_now
        return
    fi
    if build_m1c; then
        printf '  crafted m1c binary: %s\n' "$(run_m1c)"
        if [ "$with_corpus" = yes ]; then
            make -s build/v86c >/dev/null
            python3 tests/nasm_run.py --stage run --quiet \
                  --actual "./build/v86c-nasm {img} {limit} --fixture" \
                  --skip-list tests/nasm_skip_c.txt >/tmp/m1c-mut.nasm 2>&1
            rc=$?
            printf '  nasm corpus:        rc=%d  %s\n' "$rc" \
                "$(grep -E '^\[\+\] Passed|^\[!\]' /tmp/m1c-mut.nasm | tr '\n' ' ')"
        fi
    else
        echo "  BUILD FAILED (an invalid mutation proves nothing):"
        sed -n '1,4p' "$ERRS" | sed 's/^/    /'
    fi
    restore_now
    if build_m1c; then
        printf '  restored:           %s\n' "$(run_m1c)"
    else
        echo "  RESTORE FAILED -- the source is not green"
        return 1
    fi
}

names="$*"
if [ -z "$names" ]; then names="rewind errorcode trapif rpl isr r3fault recordit accessed"; fi

for n in $names; do
case "$n" in
rewind)
    # trigger_de stops rewinding: the pushed eip becomes the post-decode one
    case_one "M1 trigger_de drops the eip rewind (cpu.rs:3429)" src/cpu/exceptions.c \
        'void trigger_de(void)
{
    cpu->instruction_pointer = cpu->previous_ip;
    call_interrupt_vector(CPU_EXCEPTION_DE, false, false, 0);' \
        'void trigger_de(void)
{
    call_interrupt_vector(CPU_EXCEPTION_DE, false, false, 0);' yes ;;
errorcode)
    # the error code is pushed even when upstream passes None
    case_one "M2 the error code is pushed unconditionally (has_error_code ignored)" src/cpu/exceptions.c \
        '            if (has_error_code) {
                push32(error_code);
                DBG_ASSERT(!cpu->pagefault);
            }' \
        '            if (true) {
                push32(error_code);
                DBG_ASSERT(!cpu->pagefault);
            }' no ;;
trapif)
    # a trap gate clears IF like an interrupt gate
    case_one "M3 a trap gate clears the interrupt flag too" src/cpu/exceptions.c \
        '        if (gate_type == INTR_GATE_INTERRUPT) {
            /* clear int flag for interrupt gates */
            cpu->flags &= ~FLAG_INTERRUPT;' \
        '        if (gate_type == INTR_GATE_INTERRUPT || gate_type == INTR_GATE_TRAP) {
            /* clear int flag for interrupt gates */
            cpu->flags &= ~FLAG_INTERRUPT;' no ;;
rpl)
    # the delivered CS loses the new cpl in its RPL
    case_one "M4 the delivered CS loses the cpl in its RPL" src/cpu/exceptions.c \
        '        cpu->sreg[CS] = (uint16_t)(((uint16_t)selector & ~3) | cpu->cpl);

        update_cs_size(seg_descriptor_is_32(cs_descriptor));' \
        '        update_cs_size(seg_descriptor_is_32(cs_descriptor));' no ;;
isr)
    # the PIC stops recording the in-service register
    case_one "M5 the master PIC does not set the in-service register" src/cpu/pic.c \
        '    if (!pic.master.auto_eoi) {
        pic.master.isr |= mask;
    }' \
        '    if (!pic.master.auto_eoi) {
        pic.master.isr &= (uint8_t)~mask;
    }' no ;;
r3fault)
    # R3: the in-flight delivery no longer aborts on the nested fault
    case_one "M6 R3: a fault while resolving the gate CS no longer aborts the delivery" src/cpu/exceptions.c \
        '            lookup_segment_selector(seg_selector_of_u16((uint16_t)selector), &cs_descriptor, &cs_descriptor_address);
        if (cpu->pagefault) {
            return;
        }' \
        '            lookup_segment_selector(seg_selector_of_u16((uint16_t)selector), &cs_descriptor, &cs_descriptor_address);
        if (cpu->pagefault) {
            cpu->pagefault = false; /* MUTATION: continue the interrupted delivery */
        }' no ;;
recordit)
    # the undeliverable stop stops recording the vector (the nasm class-A fixtures read it)
    case_one "M7 the undeliverable stop records the wrong vector" src/cpu/exceptions.c \
        '    if (guest_exception < 0) {
        guest_exception = vector;
    }' \
        '    if (guest_exception < 0) {
        guest_exception = vector + 1;
    }' yes ;;
accessed)
    # switch_seg stops writing the accessed bit back into the descriptor
    case_one "M8 switch_seg does not write the accessed bit back" src/cpu/exceptions.c \
        '        int32_t address = (int32_t)translate_address_system_write(descriptor_address + 5);
        DBG_ASSERT(!cpu->pagefault); /* upstream .unwrap() */
        write8((uint32_t)address, (int32_t)seg_descriptor_access_byte(descriptor));' \
        '        (void)descriptor_address;
        (void)descriptor;' no ;;
*) echo "unknown mutation: $n"; exit 2 ;;
esac
done
echo "=== all mutations restored"
