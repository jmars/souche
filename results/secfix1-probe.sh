#!/bin/sh
# SECFIX-1's probe rig: builds results/secfix1-probe.c against a SOURCE ROOT and runs all
# four modes.  The root is a parameter so the SAME probe runs against the live tree (src/)
# and against the audited pre-fix sources (a49a887 materialized to a shadow) -- the
# before/after asymmetry is the gate, never a claim.
#
#   results/secfix1-probe.sh                 # the live tree:      src/
#   results/secfix1-probe.sh <shadow-root>   # a shadow copy, e.g. /tmp/secfix1-prefix-src
#
# Exit status: 0 iff every mode exited 0.
set -u
ROOT=${1:-src}
COSMOCC=${COSMOCC:-/home/jaye/.local/cosmo/bin/cosmocc}
WORK=/tmp/secfix1-probe-build
BIN=/tmp/secfix1-probe.bin

CORE="cpu/cpu_state.c cpu/flags.c cpu/memory.c cpu/paging.c cpu/arith.c cpu/modrm.c \
cpu/misc_instr.c cpu/string.c cpu/instructions.c cpu/instructions_0f.c cpu/sse_instr.c \
cpu/softfloat.c cpu/fpu.c cpu/exceptions.c cpu/pic.c cpu/cpu.c cpu/exec_boundary.c \
cpu/uop_cache.c cpu/jit_mem.c cpu/jit_x64.c cpu/jit_codegen.c cpu/interpreter_gen.c cpu/uop_gen.c"
HW="hw/io.c hw/bus.c hw/pit.c hw/rtc.c hw/dma.c hw/ps2.c hw/acpi.c hw/pci.c hw/machine.c \
hw/uart.c hw/kernel.c hw/ide.c hw/virtio.c hw/virtio_console.c"

rm -rf "$WORK"; mkdir -p "$WORK"
SRCS=""
for f in $CORE $HW; do SRCS="$SRCS $ROOT/$f"; done
# one TU for the core (same composition as build/unity.c), the hw files beside it
LIST=""
for f in $CORE; do LIST="$LIST
#include \"$ROOT/$f\""; done
printf '%s\n' "$LIST" > "$WORK/core.c"
"$COSMOCC" -std=gnu11 -O1 -g -I "$ROOT" -c -o "$WORK/core.o" "$WORK/core.c" || exit 2
OBJS="$WORK/core.o"
for f in $HW; do
	O="$WORK/$(echo "$f" | tr / _).o"
	"$COSMOCC" -std=gnu11 -O1 -g -I "$ROOT" -c -o "$O" "$ROOT/$f" || exit 2
	OBJS="$OBJS $O"
done
"$COSMOCC" -std=gnu11 -O1 -g -I "$ROOT" -o "$BIN" results/secfix1-probe.c $OBJS \
	build/sf/softfloat.o -Wl,--gc-sections || exit 2

RC=0
for mode in write read atapi hash virtio3 virtio4 virtio5; do
	printf '=== %s (%s) ===\n' "$mode" "$ROOT"
	case "$mode" in
	virtio*) "$BIN" virtio "${mode#virtio}" ;;
	*)       "$BIN" "$mode" ;;
	esac
	rc=$?
	printf '== mode %s rc=%d ==\n' "$mode" "$rc"
	[ $rc -ne 0 ] && RC=$rc
done
exit $RC
