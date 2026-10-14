#!/bin/bash
# spike-build.sh -- builds the spike's standalone measurement programs.  They are NOT part of
# `make` and NOT part of the shipped binary: the ceiling measurement links the real core per-TU
# (exactly as src/cpu/uop_exec_selftest.c does) and the prototypes link only the encoder.
#
#   ceiling   Q1: the emulator's current path vs a hand-written native block, interleaved
#   dynasm    Q2(a): the same block emitted by LuaJIT's DynASM (needs minilua + dynasm.lua)
set -u
cd "$(dirname "$0")/../.." || exit 2
CC=${COSMOCC_DIR:-/home/jaye/.local/cosmo/bin}/cosmocc
[ -x "$CC" ] || CC=cosmocc
CPU_SRC="src/cpu/cpu_state.c src/cpu/flags.c src/cpu/memory.c src/cpu/paging.c src/cpu/arith.c \
         src/cpu/modrm.c src/cpu/misc_instr.c src/cpu/string.c src/cpu/instructions.c \
         src/cpu/instructions_0f.c src/cpu/sse_instr.c src/cpu/softfloat.c src/cpu/fpu.c \
         src/cpu/exceptions.c src/cpu/pic.c src/cpu/cpu.c src/cpu/exec_boundary.c \
         src/cpu/uop_cache.c src/cpu/jit_mem.c"
SP=results/jit-spike
gen=0
[ -f build/cpu/uop_gen.o ] || gen=1
[ -f build/cpu/interpreter_gen.o ] || gen=1
if [ $gen = 1 ]; then node gen/interpreter_c.js; node gen/interpreter_uop.js; fi

set -x
$CC -std=gnu11 -O2 -Wall -Wextra -foptimize-sibling-calls -finline-functions-called-once \
    -fpatchable-function-entry=0 -Werror -I . -I src -o build/jit_ceiling \
    $SP/ceiling.c $SP/x64enc.c $CPU_SRC build/cpu/interpreter_gen.o build/cpu/uop_gen.o \
    build/sf/softfloat.o -Wl,--gc-sections
set +x
echo "built build/jit_ceiling"
