#!/bin/sh
# R5F's G1/G2 asymmetry rig: build results/r5f-probe.c against a source root and run the
# three scenarios.  The source root is a parameter so the SAME probe can be built against a
# shadow copy of src/ carrying ONE mutation -- no mutation is ever applied to the live tree
# (another unit owns src/hw/ while this runs; see AGENTS.md, "one writer per lane").
#
#   results/r5f-probe.sh                 # the live tree:      src/
#   results/r5f-probe.sh <shadow-root>   # a shadow copy, e.g. /tmp/r5f-mut (contains cpu/)
#
# Exit status: 0 if all three scenarios exited 0, else the first nonzero rc seen.
set -u
ROOT=${1:-src}
COSMOCC=${COSMOCC:-/home/jaye/.local/cosmo/bin/cosmocc}
BIN=/tmp/r5f-probe.bin
CPU_SRC="cpu/cpu_state.c cpu/flags.c cpu/memory.c cpu/paging.c cpu/arith.c cpu/modrm.c \
cpu/misc_instr.c cpu/string.c cpu/instructions.c cpu/instructions_0f.c cpu/sse_instr.c \
cpu/softfloat.c cpu/fpu.c cpu/exceptions.c cpu/pic.c cpu/cpu.c cpu/exec_boundary.c"

SRCS=""
for f in $CPU_SRC; do SRCS="$SRCS $ROOT/$f"; done

echo "== building probe against ROOT=$ROOT =="
$COSMOCC -std=gnu11 -O2 -Wall -Wextra -Werror -I "$ROOT" -o "$BIN" \
    results/r5f-probe.c $SRCS build/cpu/interpreter_gen.o build/sf/softfloat.o \
    -Wl,--gc-sections || { echo "BUILD FAILED rc=$?"; exit 1; }

rc_total=0
for mode in same diff fetch; do
    echo "---- probe $mode ----"
    "$BIN" "$mode" 3
    rc=$?
    echo "rc=$rc"
    [ "$rc" -ne 0 ] && rc_total=$rc
done
exit $rc_total
