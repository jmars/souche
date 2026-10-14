#!/bin/sh
# results/m2-5-probe-rig.sh -- RE-RUN the §7 cmdline probe matrix and KEEP ITS RAW TRANSCRIPTS.
#
# WHY THIS EXISTS (REVIEW FINDING S1, ROUND 2): the §7 claim -- "the guest halts itself at the
# FIRST HLT, both machines at the IDENTICAL eip, unless the cmdline carries tsc=unstable /
# idle=poll" -- rested on a table in the write-up and a comment in tests/gate-m2-5.sh, with NO
# raw transcript of any FAILING run under results/.  This rig re-runs every configuration in §7's
# tables (plus two cells the reviewer asked for, S2) and writes the raw serial / virtio / stderr
# of EACH into results/m2-5-probe-*.txt, so the claim is checkable from the evidence.
#
# It changes NOTHING: the kernel, the initramfs, the disk, the recipe and tests/gate-m2-5.sh are
# read as pinned inputs; the same two machines (build/v86c, oracle/run-boot.mjs) are used, at the
# limits §7 states (C leg 2e9, reference leg 8e8).
#
# usage: sh results/m2-5-probe-rig.sh          (from the repo root)
#
# The runs are launched CONCURRENTLY (13 short runs: each stops at the harness's first-HLT
# condition, or at the guest's own halt) and a table lands in results/m2-5-probe-matrix.txt.

set -u
cd "$(dirname "$0")/.."

KERNEL=tests/images/vmlinuz-i386-v86guest
INITRD=tests/images/guest-initramfs-v86gate.img
DISK=tests/images/disk.img
VIN=tests/v86gate-virtio-in.txt
FW=build/minibios-80m.bin
MEM=80
LIM_C=2000000000
LIM_JS=800000000
R=results
SHA_KERNEL=f0278a8f2893eb93a8c857f3f2d5a589a5b9aa6a45033171bc5411fc10f658cd
MD5=87d5ec3151caf04d90236947e6f09874

# The cmdlines, named once.  The FIRST one ('hvc-ttyS0') is §7's first gate-1a run, the run
# whose two legs stopped at the IDENTICAL eip (the 'machine, not port' evidence); the rest are
# §7's two matrix tables.  'hvc-ttyS0-tscunst' is the S2 cell the reviewer asked for (the
# reference leg's FAILING cmdline, now also measured on the C leg) and 'ttyS0-tscunst' is the
# C leg's PASSING cmdline, now also measured on the reference leg -- so both legs are measured
# on the SAME cmdline in both directions.
CMD_hvc_ttyS0="console=hvc0 console=ttyS0"
CMD_ttyS0="console=ttyS0"
CMD_ttyS0_nohlt="console=ttyS0 nohlt"
CMD_ttyS0_tscunst="console=ttyS0 tsc=unstable"
CMD_hvc_ttyS0_nohlt="console=hvc0 console=ttyS0 nohlt"
CMD_hvc_ttyS0_poll="console=hvc0 console=ttyS0 idle=poll"
CMD_hvc_ttyS0_tscunst="console=hvc0 console=ttyS0 tsc=unstable"
CMD_hvc_ttyS0_tscunst_nohlt="console=hvc0 console=ttyS0 tsc=unstable nohlt"
CMD_hvc_ttyS0_tscunst_poll="console=hvc0 console=ttyS0 tsc=unstable idle=poll"

# The pins this rig is a claim about -- verified BEFORE any run, so a moved input fails here
# rather than silently invalidating every transcript below.
echo "### PINS (the inputs every transcript below is a claim about)"
got=$(sha256sum $KERNEL | cut -d' ' -f1)
[ "$got" = "$SHA_KERNEL" ] && echo "  PASS  $KERNEL sha256 $got" \
                           || echo "  FAIL  $KERNEL sha256 $got != $SHA_KERNEL"
echo "  initrd sha256 $(sha256sum $INITRD | cut -d' ' -f1)"
echo "  disk   sha256 $(sha256sum $DISK | cut -d' ' -f1)"
echo "  disk   md5 of first 64 sectors $(dd if=$DISK bs=512 count=64 2>/dev/null | md5sum | cut -d' ' -f1) (m2-3's pin $MD5)"

# leg_c <slug> <cmdline> -- the C port, exactly tests/gate-m2-5.sh's leg_c flags.
leg_c() {
    n=$1; c=$2
    timeout 3000 ./build/v86c --bzimage $KERNEL $LIM_C --cmdline "$c" --hda $DISK \
        --initrd $INITRD --bios $FW --memory $MEM \
        --virtio-console $R/m2-5-probe-c-$n-virtio.txt --virtio-console-in $VIN \
        --serial $R/m2-5-probe-c-$n-serial.txt 2> $R/m2-5-probe-c-$n-stderr.txt
}
# leg_js <slug> <cmdline> -- the pinned reference, exactly tests/gate-m2-5.sh's leg_js flags.
leg_js() {
    n=$1; c=$2
    timeout 7200 node oracle/run-boot.mjs $KERNEL $FW $LIM_JS "$c" --hda $DISK \
        --initrd $INITRD --memory $MEM \
        --virtio-console $R/m2-5-probe-js-$n-virtio.txt --virtio-console-in $VIN \
        > $R/m2-5-probe-js-$n-serial.txt 2> $R/m2-5-probe-js-$n-stderr.txt
}

echo "### RUNS (concurrent; each stops at the harness first-HLT condition or the guest's own halt)"
# C leg: §7's six matrix cells + the S2 cell + the first-run state.
leg_c hvc-ttyS0                    "$CMD_hvc_ttyS0" &
leg_c ttyS0                        "$CMD_ttyS0" &
leg_c ttyS0-nohlt                  "$CMD_ttyS0_nohlt" &
leg_c ttyS0-tscunst                "$CMD_ttyS0_tscunst" &
leg_c hvc-ttyS0-nohlt              "$CMD_hvc_ttyS0_nohlt" &
leg_c hvc-ttyS0-poll               "$CMD_hvc_ttyS0_poll" &
leg_c hvc-ttyS0-tscunst            "$CMD_hvc_ttyS0_tscunst" &
leg_c hvc-ttyS0-tscunst-nohlt      "$CMD_hvc_ttyS0_tscunst_nohlt" &
# reference leg: §7's matrix + the S2 cell + the first-run state + the final gate cmdline.
leg_js hvc-ttyS0                   "$CMD_hvc_ttyS0" &
leg_js ttyS0-tscunst               "$CMD_ttyS0_tscunst" &
leg_js hvc-ttyS0-tscunst           "$CMD_hvc_ttyS0_tscunst" &
leg_js hvc-ttyS0-poll              "$CMD_hvc_ttyS0_poll" &
leg_js hvc-ttyS0-tscunst-poll      "$CMD_hvc_ttyS0_tscunst_poll" &
wait

echo "### DONE -- transcripts: $R/m2-5-probe-{c,js}-<cmdline>-[serial|virtio|stderr].txt"

# ----------------------------------------------------------------- the matrix table
# Built FROM the transcripts above, so the table and the raw evidence cannot drift.
# The terminal eip is resolved against the kernel's own System.map (the build that
# produced this vmlinuz; the map is not shipped, so the resolution is recorded here):
#   FAIL eip 0xc11db37e = default_idle+0x12, the instruction AFTER the idle `hlt`
#                         (0xc11db37d hlt; 0xc11db37e cli) -- the kernel idling.
#   PASS eip 0xc101acb2 = stop_this_cpu+0x3e, the instruction AFTER the halt-loop `hlt`
#                         (0xc101acb1 hlt; 0xc101acb2 jmp back) -- the guest's own halt.
matrix() {
    echo "# m2-5 slice-1: the §7 cmdline probe matrix, RE-RUN and kept (REVIEW S1 + S2)."
    echo "# Generated by results/m2-5-probe-rig.sh from the raw transcripts beside this file."
    echo "# Machine: 80 MiB, tests/images/{vmlinuz-i386-v86guest,guest-initramfs-v86gate.img,disk.img}."
    echo "# C leg: build/v86c ... limit ${LIM_C}; reference leg: node oracle/run-boot.mjs ... limit ${LIM_JS}."
    echo "# Terminal eips resolved against the kernel's System.map: 0xc11db37e = default_idle+0x12"
    echo "# (the instruction AFTER default_idle's idle hlt), 0xc101acb2 = stop_this_cpu+0x3e (the"
    echo "# instruction AFTER the halt loop's hlt). Both harnesses stop on in_hlt, so the eip is"
    echo "# past the opcode either way."
    echo "#"
    printf '%-4s %-47s %12s %5s %-11s %-6s %5s %4s %4s %3s %s\n' \
        leg cmdline instr in_hlt eip cs lines end halt md5 verdict
    for n in hvc-ttyS0 ttyS0 ttyS0-nohlt ttyS0-tscunst hvc-ttyS0-nohlt hvc-ttyS0-poll \
             hvc-ttyS0-tscunst hvc-ttyS0-tscunst-nohlt; do
        emit c "$n"
    done
    for n in hvc-ttyS0 ttyS0-tscunst hvc-ttyS0-tscunst hvc-ttyS0-poll hvc-ttyS0-tscunst-poll; do
        emit js "$n"
    done
}
emit() {
    leg=$1; n=$2
    e=$R/m2-5-probe-$leg-$n-stderr.txt
    s=$R/m2-5-probe-$leg-$n-serial.txt
    cmd=$(sed -n "s/.*cmdline '\(.*\)'.*/\1/p" $e | head -1)
    if [ "$leg" = c ]; then
        line=$(grep -E '[0-9]+ instructions,' $e | head -1)
        ins=$(echo "$line" | grep -oE '[0-9]+ instructions' | cut -d' ' -f1)
        hlt=$(echo "$line" | grep -oE 'in_hlt=[0-9]' | cut -d= -f2)
        eip=$(echo "$line" | grep -oE 'eip=0x[0-9a-f]+' | cut -d= -f2)
        cs=$(echo "$line" | grep -oE 'cs=0x[0-9a-f]+' | cut -d= -f2)
    else
        ins=$(grep -oE '"instructions":[0-9]+' $e | head -1 | cut -d: -f2)
        hlt=$(grep -oE '"in_hlt":[0-9]' $e | head -1 | cut -d: -f2)
        eip=$(grep -oE '"eip":"0x[0-9a-f]+"' $e | head -1 | cut -d: -f2 | tr -d '"')
        cs=$(grep -oE '"cs":"0x[0-9a-f]+"' $e | head -1 | cut -d: -f2 | tr -d '"')
    fi
    lines=$(wc -l < $s)
    end=$(tr -d '\r' < $s | grep -c 'GUEST-INIT: end')
    halt=$(tr -d '\r' < $s | grep -c 'reboot: System halted')
    md5=$(tr -d '\r' < $s | grep -c "$MD5")
    if [ "$halt" -gt 0 ]; then v=PASS; else v="FAIL: idle"; fi
    printf '%-4s %-47s %12s %5s %-11s %-6s %5s %4s %4s %3s %s\n' \
        "$leg" "$cmd" "$ins" "$hlt" "$eip" "$cs" "$lines" "$end" "$halt" "$md5" "$v"
}
matrix > $R/m2-5-probe-matrix.txt
echo "### TABLE: $R/m2-5-probe-matrix.txt"
sed 's/^/    /' $R/m2-5-probe-matrix.txt
