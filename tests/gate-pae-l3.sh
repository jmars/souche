#!/bin/sh
# gate-pae-l3.sh -- PAE-L3's gate (results/pae-l3.md): boot the pinned Debian
# CONFIG_X86_PAE=y kernel and assert the guest's OWN evidence that 3-level paging
# is live, on BOTH legs (the C port and the pinned JS reference).
#
# THE THREE ASSERTIONS, AND WHY THEY ARE DECISIVE:
#   1. "NX (Execute Disable) protection: active" -- this string exists ONLY in the
#      CONFIG_X86_PAE=y branch of arch/x86/kernel/setup.c; the non-PAE branch prints
#      its inverse ("cannot be enabled: non-PAE kernel!").  NX lives in the high 32
#      bits of the 64-bit PDE/PTE, which exist only in the 3-level walk, so the line
#      is the kernel's own proof that PAE paging is engaged.  The tree's Alpine kernel
#      prints the inverse on this machine (results/m2-2-boot.md), so the asymmetry is
#      real, not a happy-path coincidence.
#   2. the boot does NOT print the inverse ("non-PAE kernel!") -- the negative half.
#   3. the final machine state has PG set (cr0 bit 31) and a non-zero cr3 -- the
#      3-level walk is in use at the end of the run, not just advertised.
#
# WHAT THIS GATE DOES NOT ASSERT (results/pae-l3.md, the honest seat): reaching
# userspace.  The kernel stalls ~64s of guest time in async_synchronize_full() after
# "evm: HMAC attrs" (both legs, MEASURED), so a run to /init needs either a KASLR
# relocation that lands the kernel elsewhere (the reference reached userspace once,
# "PAE-INIT: end", at 2.15G) or a limit beyond this port's 4 GiB ceiling.  The PAE
# evidence above is printed BEFORE that stall and is what this gate pins.
#
# THE LIMIT.  3,000,000,000 instructions: the kernel is XZ-compressed and its
# decompression alone is ~2.34G instructions (MEASURED, results/pae-l3.md), so the
# 2G limit of an earlier measurement stopped the run mid-decompression and read as a
# "hang" -- it was the limit, not the machine.  3G leaves ~0.6G of headroom past the
# first kernel lines, which is where "NX protection: active" is printed.
#
# usage: sh tests/gate-pae-l3.sh [pin|c|js|all]
#   pin   the sha256 pins of every input (fast, no boot)
#   c     the C leg only (build/v86c, ~35 s)
#   js    the reference leg only (node oracle/run-boot.mjs, ~90 s)
#   all   pin + both legs (default)
set -u
cd "$(dirname "$0")/.."

KERNEL=tests/images/vmlinuz-6.1.0-50-686-pae
INITRD=tests/images/guest-initramfs-pae.img
V86C=build/v86c
LIMIT=3000000000
MEM=80
FW=build/minibios-80m.bin
CMD="console=ttyS0"
R=results

# The pins (results/pae-l3.md records the provenance: the .deb URL and its sha256, the
# vmlinuz extracted from it, and this initramfs's own builder).
SHA_KERNEL=c2ac6a2a88224f41e3006ff98a5e4f8a7af796806dc21f4afbaf83b99606925f
SHA_INITRD=2a4a29dc8dd900258135ee1543226f7d629dacaf560b95128951ae10e21e35a6

fails=0
pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; fails=$((fails + 1)); }
# The serial is a TTY (CRLF); the kernel's own lines are matched with CR stripped.
want()     { if tr -d '\r' < "$1" | grep -qE "$2"; then pass "$3"; else fail "$3 (no /$2/ in $1)"; fi; }
want_not() { if tr -d '\r' < "$1" | grep -qE "$2"; then fail "$3 (found /$2/ in $1)"; else pass "$3"; fi; }

pin_check() {
    echo "### PINS (the inputs this gate is a claim about)"
    for pair in "$KERNEL $SHA_KERNEL" "$INITRD $SHA_INITRD"; do
        set -- $pair
        got=$(sha256sum "$1" | cut -d' ' -f1)
        if [ "$got" = "$2" ]; then pass "$1 sha256 $got"; else fail "$1 sha256 $got != $2"; fi
    done
    if [ -x "$V86C" ]; then pass "$V86C present"; else fail "$V86C missing (make)"; fi
    if [ -f "$FW" ]; then pass "$FW present"; else fail "$FW missing (make minibios-mem)"; fi
}

# the PAE-live assertions, applied to a leg's serial and stderr
assert_leg() { # assert_leg <name> <serial> <stderr>
    name="$1"; serial="$2"; stderr="$3"
    echo "--- assertions ($name)"
    want "$serial" 'NX \(Execute Disable\) protection: active' \
        "the kernel's own PAE evidence: NX protection is active"
    want_not "$serial" 'non-PAE kernel!' \
        "the inverse line is absent (a non-PAE kernel would print it)"
    want "$serial" 'BIOS-e820: \[mem 0x0000000000100000-0x0000000004ffffff\] usable' \
        "the 80 MiB machine map the kernel saw"
    # PG (cr0 bit 31) set and cr3 non-zero, from the harness's final-state line
    # (the LAST line of stderr).  The C port prints "cr0=0x... cr3=0x..."; the
    # reference prints "cr0":"0x...","cr3":"0x...".  Extract the hex and check the
    # bits with shell arithmetic (the value is matched without its "0x" prefix).
    final=$(tail -1 "$stderr")
    cr0=$(printf '%s\n' "$final" | grep -oE 'cr0[=": ]+0x[0-9a-fA-F]+' | grep -oE '[0-9a-fA-F]{1,8}$')
    cr3=$(printf '%s\n' "$final" | grep -oE 'cr3[=": ]+0x[0-9a-fA-F]+' | grep -oE '[0-9a-fA-F]{1,8}$')
    if [ -n "$cr0" ] && [ $((0x$cr0 & 0x80000000)) -ne 0 ]; then pass "PG is set (cr0=0x$cr0)"; else fail "PG is NOT set (cr0=${cr0:-missing})"; fi
    if [ -n "$cr3" ] && [ "$((0x$cr3))" -ne 0 ]; then pass "cr3 is non-zero (0x$cr3) -- the 3-level walk is in use"; else fail "cr3 is zero (${cr3:-missing})"; fi
}

gate_c() {
    echo "### C LEG: build/v86c"
    echo "\$ ./$V86C --bzimage $KERNEL $LIMIT --cmdline '$CMD' --initrd $INITRD \\"
    echo "      --bios $FW --memory $MEM --serial $R/pae-l3-c-serial.txt"
    timeout 900 ./$V86C --bzimage "$KERNEL" "$LIMIT" --cmdline "$CMD" --initrd "$INITRD" \
        --bios "$FW" --memory "$MEM" --serial "$R/pae-l3-c-serial.txt" \
        2> "$R/pae-l3-c-stderr.txt"
    echo "port rc=$?"
    tail -1 "$R/pae-l3-c-stderr.txt" | sed 's/^/    /'
    assert_leg "C leg" "$R/pae-l3-c-serial.txt" "$R/pae-l3-c-stderr.txt"
}

gate_js() {
    echo "### REFERENCE LEG: node oracle/run-boot.mjs (the pinned v86)"
    echo "\$ node oracle/run-boot.mjs $KERNEL $FW $LIMIT '$CMD' --initrd $INITRD \\"
    echo "      --memory $MEM > $R/pae-l3-js-serial.txt"
    timeout 1800 node oracle/run-boot.mjs "$KERNEL" "$FW" "$LIMIT" "$CMD" --initrd "$INITRD" \
        --memory "$MEM" > "$R/pae-l3-js-serial.txt" 2> "$R/pae-l3-js-stderr.txt"
    echo "reference rc=$?"
    tail -1 "$R/pae-l3-js-stderr.txt" | sed 's/^/    /'
    assert_leg "reference leg" "$R/pae-l3-js-serial.txt" "$R/pae-l3-js-stderr.txt"
}

case "${1:-all}" in
    pin) pin_check ;;
    c)   pin_check; gate_c ;;
    js)  pin_check; gate_js ;;
    all) pin_check; gate_c; gate_js ;;
    *)   echo "usage: sh tests/gate-pae-l3.sh [pin|c|js|all]"; exit 2 ;;
esac

echo
echo "### pae-l3 gate: $fails FAILING assertion(s)"
[ "$fails" = 0 ]
