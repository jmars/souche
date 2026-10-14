#!/bin/sh
# REVIEWFIX-1's PM-timer rig (AUDIT-1R SF2): builds results/reviewfix1-pmtimer-probe.c
# against a SOURCE ROOT and runs both modes.  The root is a parameter so the SAME probe
# runs against the live tree (src/) and against the pre-fix sources (414d194 materialized
# to a shadow) -- the before/after asymmetry is the gate, never a claim.
#
#   results/reviewfix1-pmtimer-probe.sh                 # the live tree:      src/
#   results/reviewfix1-pmtimer-probe.sh <shadow-root>   # e.g. /tmp/reviewfix1-prefix-src
#
# Expected: mode `pmtimer` rc 134 (the DBG_ASSERT abort) on the PRE-FIX root and rc 0
# (the graceful overshoot arm) on the POST-FIX root; mode `unclaimed` rc 0 on BOTH (the
# guest cannot reach the assert in any real boot -- 0xB008 is unregistered, D-F1).
set -u
ROOT=${1:-src}
COSMOCC=${COSMOCC:-/home/jaye/.local/cosmo/bin/cosmocc}
WORK=/tmp/reviewfix1-pmtimer-build
BIN=/tmp/reviewfix1-pmtimer.bin

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
"$COSMOCC" -std=gnu11 -O1 -g -I "$ROOT" -o "$BIN" results/reviewfix1-pmtimer-probe.c $OBJS \
	build/sf/softfloat.o -Wl,--gc-sections || exit 2

RC=0
for mode in pmtimer unclaimed; do
	printf '=== %s (%s) ===\n' "$mode" "$ROOT"
	"$BIN" "$mode"
	rc=$?
	printf '== mode %s rc=%d ==\n' "$mode" "$rc"
	case "$mode" in
	pmtimer)   # pre-fix: 134 (assert); post-fix: 0 (the arm) -- the caller judges
		;;
	unclaimed) [ "$rc" -eq 0 ] || RC=1 ;;
	esac
done
exit "$RC"
