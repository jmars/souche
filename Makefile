# v86-to-C port -- M0: scaffold + the reference oracle; M1a: the CPU foundation.
#
#   make            build src/main.c with cosmocc into build/v86c (an APE), and
#                   compile the CPU-foundation translation units (they are not linked
#                   into the APE yet: main.c is still the M0 placeholder, and the core
#                   joins the run loop in M1c)
#   make selftest   build and run src/cpu/cpu_selftest.c -- the M1a unit tests
#   make oracle     build the test image, run it through the reference oracle,
#                   check the transcript against tests/model.py, exit 0
#   make determinism  two consecutive oracle runs must be byte-identical
#   make run        build and run the APE
#   make nasm-translate  translate+assemble+link all 123 v86/tests/nasm/*.asm
#   make nasm-fixtures   the native (ptrace) fixture for each of them
#   make nasm            the differential run -- native fixtures vs the actual
#                        implementation (default: the pinned JS reference); the gate
#   make nasm64-gen      regenerate the x86-64 corpus (gen/nasm64_gen.py -> tests/nasm64)
#   make nasm64          S4's gate: the x86-64 corpus's instrument must be able to FAIL
#                        (perturbed fixtures), its own controls must hold, and EVERY case
#                        must be RED with a named `unimplemented:` stop until S5
#   make clean

COSMOCC_DIR ?= /home/jaye/.local/cosmo/bin

# Only override make's built-in default (CC = cc); an explicit CC=... on the
# command line or in the environment wins.
ifeq ($(origin CC),default)
CC := $(if $(wildcard $(COSMOCC_DIR)/cosmocc),$(COSMOCC_DIR)/cosmocc,cosmocc)
endif

CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra

# --- PERF-1 (HANDOFF=v86c-perf): the flags cosmocc's wrapper sets FOR us, undone -----
#
# cosmocc invokes its gcc with a fixed prologue of options (`cosmocc -v`, MEASURED):
#
#     -fno-semantic-interposition -fno-optimize-sibling-calls -mno-omit-leaf-frame-pointer
#     -fno-schedule-insns2 -mno-tls-direct-seg-refs -fpatchable-function-entry=18,16
#     -fno-inline-functions-called-once -fportcosmo -DFTRACE -fno-pie ...
#
# User flags are appended AFTER that prologue, and gcc's same-option rule is last-wins,
# so the three below are re-enabled from this side.  They are not tuning knobs: this
# interpreter is CALL BOUND -- MEASURED by gprof call counts over 200,095,214 guest
# instructions, read_imm8 alone is 337.3M calls, read_reg32 212.6M, get_phys_eip 200.1M
# -- so a call/return pair per operand is most of the cost, and:
#
#   -foptimize-sibling-calls        a tail call becomes a jump.  MEASURED ALONE on the
#                                   non-unity build: 6.367 s -> 5.15 s, i.e. the ENTIRE
#                                   cosmocc-vs-glibc gap (glibc -O2 has it on by default).
#   -finline-functions-called-once  inlines the instruction bodies the generated
#                                   dispatcher calls exactly once.  MEASURED in an
#                                   interleaved block on the unity build: 4.306 -> 4.231 s
#                                   best, 4.337 -> 4.293 s median, i.e. ~1-1.7% -- a real
#                                   but SMALL effect (a first, cross-block comparison read
#                                   4.562 -> 4.421 s and overstated it), and ~0 before
#                                   the unity build, where nothing is called once.
#   -fpatchable-function-entry=0    drops cosmocc's 18-byte ftrace patch area from every
#                                   function.  MEASURED in the same interleaved block:
#                                   4.293 -> 4.012 s median (4.231 -> 3.985 s best) = ~6%:
#                                   on a call-bound interpreter the two NOPs the entry
#                                   executes are paid 1.1G times, and the padding also
#                                   feeds the inliner's size model.  Only affects
#                                   cosmopolitan's own `--ftrace`; this port never uses it.
#
# `?=`-style override is deliberately NOT used: PERF_CFLAGS is appended after $(CFLAGS)
# everywhere, so a command-line `CFLAGS=...` cannot silently drop the measured win.
PERF_CFLAGS := -foptimize-sibling-calls -finline-functions-called-once \
               -fpatchable-function-entry=0
AS := as
ASFLAGS := --32
LD := ld
LDFLAGS := -m elf_i386 -T tests/test.ld
OBJCOPY := objcopy
NODE := node
PYTHON := python3

BUILD := build
IMAGE := tests/test.img
TRANSCRIPT := results/transcript.txt
EXPECTED := results/expected.json
INSTRUCTION_LIMIT ?= 1000000

# The CPU core (M1a).  Compiled with -Werror: the brief's gate is "compile clean
# under -Wall -Wextra; warn on nothing".
#
# interpreter_gen.c is GENERATED (gen/interpreter_c.js) and is NOT in CPU_SRC: it is a
# build product of x86_table.js, so listing it as a source would hide that.  It is in
# UNITY_SRC below (and in the selftest links), and the generator's rule names its real
# inputs.
#
# PERF-1: these files are no longer compiled into one object each for the shipped
# binary -- they are concatenated into $(UNITY_C) and compiled as ONE translation unit.
# CPU_SRC is still the per-file list the selftests, the emitter and the rigs use, and
# `make selftest` still compiles every one of them individually with $(CPU_CFLAGS)
# -Werror, so per-file warning coverage is unchanged.
CPU_SRC := src/cpu/cpu_state.c src/cpu/flags.c src/cpu/memory.c src/cpu/paging.c \
           src/cpu/debug.c \
           src/cpu/arith.c src/cpu/modrm.c src/cpu/misc_instr.c \
           src/cpu/string.c src/cpu/instructions.c src/cpu/instructions_0f.c \
           src/cpu/instructions_64.c \
           src/cpu/sse_instr.c \
           src/cpu/softfloat.c src/cpu/fpu.c \
           src/cpu/exceptions.c src/cpu/pic.c \
           src/cpu/apic.c src/cpu/ioapic.c \
           src/cpu/cpu.c src/cpu/cpu_backend.c src/cpu/exec_boundary.c \
           src/cpu/uop_cache.c \
           src/cpu/jit_mem.c \
           src/cpu/jit_x64.c \
           src/cpu/jit_codegen.c
CPU_HDR := src/cpu/cpu_state.h src/cpu/flags.h src/cpu/memory.h src/cpu/paging.h \
           src/cpu/arith.h src/cpu/modrm.h src/cpu/misc_instr.h \
           src/cpu/string.h src/cpu/instructions.h src/cpu/instructions_0f.h \
           src/cpu/instructions_64.h \
           src/cpu/sse_instr.h \
           src/cpu/softfloat.h src/cpu/fpu.h \
           src/cpu/exceptions.h src/cpu/pic.h \
           src/cpu/apic.h src/cpu/ioapic.h \
           src/cpu/cpu.h src/cpu/cpu_backend.h src/cpu/exec_boundary.h \
           src/cpu/jit_mem.h src/cpu/jit_x64.h src/cpu/jit_codegen.h
CPU_CFLAGS := $(CFLAGS) $(PERF_CFLAGS) -Werror

# M2-1 (HANDOFF=v86c): THE MACHINE -- the device layer.  ONE C FILE PER UPSTREAM JS DEVICE
# FILE in v86/src/ (io/pit/rtc/dma/ps2/acpi/pci + the device bus), so the JS->C mapping
# stays auditable, plus machine.c for the three things that have no JS device file of their
# own: cpu.js:1176-1266's device block, cpu.js:1924-1941's run_hardware_timers (which
# replaces exec_boundary.c's M1c stub) and cpu.rs:4596-4610's device_raise_irq /
# device_lower_irq.  See src/hw/machine.h.
HW_SRC := src/hw/io.c src/hw/bus.c src/hw/pit.c src/hw/rtc.c src/hw/dma.c \
          src/hw/ps2.c src/hw/acpi.c src/hw/acpi_tables.c src/hw/pci.c src/hw/machine.c \
          src/hw/uart.c src/hw/kernel.c src/hw/ide.c \
          src/hw/virtio.c src/hw/virtio_console.c \
          src/hw/vga.c src/hw/vga_screen.c
HW_HDR := src/hw/io.h src/hw/bus.h src/hw/pit.h src/hw/rtc.h src/hw/dma.h \
          src/hw/ps2.h src/hw/acpi.h src/hw/acpi_tables.h src/hw/pci.h src/hw/machine.h \
          src/hw/uart.h src/hw/kernel.h src/hw/ide.h \
          src/hw/virtio.h src/hw/virtio_console.h src/hw/vga.h
HW_OBJ := $(patsubst src/hw/%.c,$(BUILD)/hw/%.o,$(HW_SRC))

# M2-2's firmware.  THESE MUST BE DEFINED BEFORE THE $(BUILD)/v86c RULE BELOW: a rule's
# PREREQUISITE list is expanded when make READS the rule, while a recipe is expanded when it
# RUNS, so a variable defined further down gives a build with the file on the command line
# and no rule to produce it (MEASURED: `make clean && make` failed with "cosmocc: fatal
# error: build/minibios_embed.c: no such file"; the recipe's expansion hid the mistake).
MINIBIOS_ASM := tests/minibios.asm
MINIBIOS_BIN := $(BUILD)/minibios.bin
MINIBIOS_EMBED := $(BUILD)/minibios_embed.c

# M2-3: the same firmware with an 80 MiB memory top (see tests/minibios.asm).  It is a
# SEPARATE FILE, never the embedded default: the M2-2 boot's firmware must stay the same
# bytes, and both legs of the disk gate are handed this one via `--bios`/the positional
# argument, so the two machines still run identical firmware.
MINIBIOS_80_BIN := $(BUILD)/minibios-80m.bin
MINIBIOS_80_MEM_TOP := 0x5000000

# M1b-3: the Berkeley SoftFloat engine (softfloat.c's arithmetic core).  It is the
# READ-ONLY upstream amalgamation, compiled with upstream's own defines (v86/Makefile:
# 242-248) so the reference's fast-int64 paths are the ones that run.  It is third-party
# C, so it gets its OWN rule and NOT -Werror: a new compiler's diagnostics in 32k lines of
# vendor code are not this port's gate (and its globals -- softfloat_roundingMode,
# extF80_roundingPrecision, softfloat_exceptionFlags -- are declared in softfloat.h).
SF_SRC := v86/lib/softfloat/softfloat.c
SF_OBJ := $(BUILD)/sf/softfloat.o
SF_DEFINES := -DSOFTFLOAT_FAST_INT64 -DINLINE_LEVEL=5 \
              -DSOFTFLOAT_FAST_DIV32TO16 -DSOFTFLOAT_FAST_DIV64TO32
# The amalgamation ships the WHOLE 8086-SSE library, including the float128 routines,
# whose unresolved helpers (softfloat_shiftRightJam128Extra / softfloat_shiftRightJam256M
# -- the amalgamation's `#ifndef` idiom leaves their definitions out) are never reachable
# from any extF80/f32/f64 path.  Upstream drops them at wasm link time (v86/Makefile:242-248
# compiles with -ffunction-sections, and the wasm link is gc'd); on ELF the same thing
# needs the two flags below -- a function-per-section vendor object plus --gc-sections on
# the links that take it.  MEASURED: without them the link is four undefined-reference
# errors from softfloat_roundPackToF128/addMagsF128/mulAddF128; with them the four F128
# functions are simply not in the binary.
SF_SECTIONS := -ffunction-sections -fdata-sections
GC_LDFLAGS := -Wl,--gc-sections
# (its RULE sits below `all:` -- an explicit rule before the first target would become
# make's default goal, which is how this bit itself the first time)

# --- the C emitter: x86_table.js -> src/cpu/interpreter_gen.c --------------------
# v86/Makefile:199-202's arrangement: `make` regenerates the dispatcher whenever the table
# or the emitter changes, so the checked-in generated file can never be stale.  It also
# depends on the hand-written instruction files, because the emitter's availability scan
# (which `instr*` symbols exist) is a function of THEM -- adding instructions regenerates
# the file, replacing "unimplemented" cases with real calls.
GENERATOR := gen/interpreter_c.js
X86_TABLE := v86/gen/x86_table.js
GEN_C := src/cpu/interpreter_gen.c
GEN_OBJ := $(BUILD)/cpu/interpreter_gen.o
LONG_MODE_GEN := src/cpu/long_mode_gen.c
LONG_MODE_GEN_OBJ := $(BUILD)/cpu/long_mode_gen.o
# S5a: the emitter's SECOND output, the default-64 predicate (dispatch_os_index's second half).
# It is its own translation unit because modrm.c needs it and the SELFTESTS do not link the
# generated dispatcher -- they supply their own run_instruction, so a definition inside
# interpreter_gen.c is invisible to them and the link fails (MEASURED: undefined reference to
# `long_mode_default64` in cpu_selftest).

# --- PERF-3 (HANDOFF=v86c-perf): the uop DECODE table (M1) -------------------------
# The SECOND emitter pass over the same x86_table.js: the descriptor per dispatch case that
# src/cpu/uop_cache.c's decoder walks (results/perf-2-blockcache-plan.md ss8 step 1).  It is
# a build product of the table AND of the hand-written instruction files, because its
# availability scan (which `instr*` symbols the port DEFINES) is a function of them -- the
# same construction as $(GEN_C) above, and the same reason imperatively.
#
# NOT IN $(CPU_SRC) / $(UNITY_SRC) FOR M1, and the reason is NOT a measured slowdown -- it is
# that nothing in the shipped path calls the decoder yet (M2 wires it into cycle_internal), so
# shipping 750 lines of decoder plus ~50 KB of descriptor tables and 1182 name strings would be
# pure bloat and a slower rebuild for no M1 benefit.  THE SPEED QUESTION WAS MEASURED AND IS
# NOT RESOLVABLE ON THIS HOST: results/perf-3/attribution.sh (interleaved, the two Makefiles
# differing only in where the files sit) read +1.9% for files-IN on a 4-round run whose load
# climbed 9.0 -> 10.3 with the files-IN binary ALWAYS measured second, and the 6-round repeat
# gave -0.2% (i.e. the noise band).  What IS measured is stronger: with the files out, the
# shipped binary is byte-identical to the same tree built with every PERF-3 Makefile addition
# removed (objcopy --strip-all of the two .com.dbg files:  b8f247fb054445758c7ea33a9c58f4dd8
# 2048aa734dee2c59fe0ecbc30aafcb4, identical) -- this unit changes nothing the shipped path
# compiles.  The files are compiled per-TU with the same $(CPU_CFLAGS) -Werror, linked into the
# selftest, and the UNITY FIT is proven by the `uop-unity` target, which builds the unity TU they
# will join in M2.
UOP_GENERATOR := gen/interpreter_uop.js
UOP_GEN := src/cpu/uop_gen.c
UOP_OBJ := $(BUILD)/cpu/uop_gen.o
UOP_SRC := src/cpu/uop_cache.c
UOP_C_OBJ := $(BUILD)/cpu/uop_cache.o
# JIT-SPIKE (HANDOFF=v86c-jit): the executable-memory layer's own object, for the ONE link that
# takes uop_cache.c as a separate object rather than through $(CPU_SRC) (cpu_selftest).  Every
# other link gets it as a SOURCE (it is in $(CPU_SRC)), which is also how it reaches the unity
# translation unit the shipped binary is built from.
JIT_SRC := src/cpu/jit_mem.c
JIT_HDR := src/cpu/jit_mem.h
JIT_OBJ := $(BUILD)/cpu/jit_mem.o

# JIT-PLAN (HANDOFF=v86c-jit, unit JIT-1): the encoder and the block compiler.  Both are in
# $(CPU_SRC) (so the shipped unity TU and every link that takes CPU_SRC get them), and the ONE
# link that takes uop_cache.c as a SEPARATE OBJECT -- cpu_selftest, whose rules predate the
# unity TU -- needs them as sources, because uop_cache.c now CALLS jit_compile_block /
# jit_execute_block.  MEASURED: without this the cpu_selftest link fails on exactly those two
# undefined references (the JIT-SPIKE unit hit the same shape with jit_code_invalidate_page
# when it added jit_mem.c to CPU_SRC).
JIT_CG_SRC := src/cpu/jit_x64.c src/cpu/jit_codegen.c
JIT_CG_HDR := src/cpu/jit_x64.h src/cpu/jit_codegen.h
UOP_HDR := src/cpu/uop_cache.h
UOP_UNITY_C := $(BUILD)/uop_unity.c
UOP_EXEC_SELFTEST := src/cpu/uop_exec_selftest.c

# The M1 decode checker's corpus: each nasm image's .text, flat (gen/nasm_corpus.py).
# It depends on the images when they exist, and on the translate stage when they do not --
# so a from-scratch `make` builds the corpus WITHOUT a glob prerequisite that make cannot
# satisfy.
NASM_CORPUS := $(BUILD)/nasm_corpus.bin
NASM_IMGS := $(wildcard $(NASM_WORKDIR)/*.img)
ifeq ($(NASM_IMGS),)
NASM_CORPUS_DEP := nasm-translate
else
NASM_CORPUS_DEP := $(NASM_IMGS)
endif

# --- PERF-1 (HANDOFF=v86c-perf): THE UNITY TRANSLATION UNIT -------------------------
#
# WHY.  cosmocc cannot inline across translation units and has no LTO (`-flto`: "LTO
# support has not been enabled in this configuration"), while this interpreter is
# per-operand-call bound (MEASURED call counts: read_imm8 337.3M, read_reg32 212.6M,
# get_phys_eip 200.1M, write_reg32 119.0M, resolve_modrm32 83.1M for 200,095,214 guest
# instructions -- results/perf-1/).  Compiling the whole core as ONE translation unit
# gives the compiler intra-TU inlining WITHOUT LTO, i.e. most of what -flto would have
# given under glibc.  MEASURED: 6.367 s (per-TU, shipped toolchain) -> 4.234 s
# (unity + $(PERF_CFLAGS)) on the boot benchmark -- results/perf-1-inlining.md.
#
# THE GENERATOR IS THE LAW: gen/unity_c.py emits one `#include` per source file, in this
# order, and REFUSES to emit a unit in which two inputs define the same file-scope
# `static` name (in one TU that would be a collision -- and if the signatures agreed, a
# silent misbind).  Do not hand-maintain the include list here: the list below is the
# same $(CPU_SRC) the rest of the Makefile uses.
#
# CPU ONLY, and deliberately.  The per-operand calls are in the core; the device layer is
# reached through port I/O.  MEASURED: folding src/hw in as well is worth 0.2% (4.962 vs
# 4.970 s on the same build), while it costs the hw rule its -Werror (the inlined io.c
# port-map accesses trip -Warray-bounds) and puts 14 files' file-local statics in one
# namespace for no measured gain.  Keep src/hw per-TU.
UNITY_SRC := $(CPU_SRC) $(GEN_C) $(LONG_MODE_GEN) $(UOP_GEN)
UNITY_C := $(BUILD)/unity.c
UNITY_OBJ := $(BUILD)/unity.o

# P0: the nasm differential oracle.  tests/mkfixture.c is a host binary (ptrace,
# process_vm_readv), so it is built with the host cc, never with cosmocc.
HOSTCC ?= cc
NASM_MKFIXTURE := $(BUILD)/mkfixture
NASM_WORKDIR := $(BUILD)/nasm

# S4: the x86-64 corpus (HANDOFF=v86c-x86-64).  tests/mkfixture64.c is a host binary for
# the same reason tests/mkfixture.c is: it mmaps the flat image at its link base, jumps to
# the body under ptrace, and reads x86-64 PTRACE_GETREGS/GETFPREGS -- none of which cosmocc
# has any business doing.  The corpus itself is GENERATED (gen/nasm64_gen.py), and the
# image/ELF/objcopy pipeline lives in tests/nasm64_run.py.
NASM64_MKFIXTURE := $(BUILD)/mkfixture64
NASM64_WORKDIR := $(BUILD)/nasm64

# The unit tests' source set: the M1a CPU files the selftest was written against.  It is
# deliberately NOT CPU_SRC -- cpu_state.c here has its own recording definitions of every
# boundary symbol (cpu.c/exec_boundary.c/instructions_0f.c DEFINE them now), so linking
# both would be a duplicate-symbol error.  The run loop is covered by the transcript gate
# instead (gates G1/G2), which is the stronger check for it.
SELFTEST_SRC := src/cpu/cpu_state.c src/cpu/flags.c src/cpu/memory.c src/cpu/paging.c \
                src/cpu/debug.c \
                src/cpu/arith.c src/cpu/modrm.c src/cpu/misc_instr.c \
                src/cpu/string.c src/cpu/instructions.c \
                src/cpu/instructions_0f.c \
                src/cpu/instructions_64.c \
                src/cpu/sse_instr.c \
                src/cpu/softfloat.c src/cpu/fpu.c

.PHONY: all oracle determinism expected image run selftest clean generator \
        transcript-diff nasm nasm-js nasm-translate nasm-fixtures minibios unity unity-src bench \
        unity-symbols uop-gen uop-unity jit-selftest jit-codegen-selftest engine-identity \
        poison-probe nasm64 nasm64-gen

all: $(BUILD)/v86c

$(GEN_C) $(LONG_MODE_GEN): $(GENERATOR) $(X86_TABLE) gen/x86_table64.js $(CPU_SRC) $(CPU_HDR)
	$(NODE) $(GENERATOR)

$(LONG_MODE_GEN_OBJ): $(LONG_MODE_GEN)
	@mkdir -p $(dir $@)
	$(CC) $(CPU_CFLAGS) -c -o $@ $<

$(UOP_GEN): $(UOP_GENERATOR) $(X86_TABLE) $(CPU_SRC) $(CPU_HDR)
	$(NODE) $(UOP_GENERATOR)

generator: $(GEN_C) $(UOP_GEN)

uop-gen: $(UOP_GEN)

$(UOP_C_OBJ) $(UOP_OBJ): $(UOP_HDR)
$(JIT_OBJ): $(JIT_HDR) $(UOP_HDR)
$(JIT_CG_SRC): $(JIT_CG_HDR) $(JIT_HDR) $(UOP_HDR) src/cpu/cpu_state.h

# THE UNITY FIT (brief: "your new files must fit that [one translation unit]").  This builds
# the SAME unity TU M2 will ship -- $(CPU_SRC) + $(GEN_C) + the decoder and its tables -- as a
# separate object that is never linked, so the fit (no duplicate file-scope statics, no
# warnings under $(CPU_CFLAGS) -Werror) is checked without paying for it in the shipped binary.
# PERF-4/M2: THE TARGET IS NOW THE SHIPPED UNITY TU ITSELF.  $(UOP_SRC) is in $(CPU_SRC) and
# $(UOP_GEN) is in $(UNITY_SRC) since M2 (nothing calls the decoder until then, which is why
# M1 could keep them out), so there is no second arrangement left to check: the former
# "M2 will ship this set" build IS $(UNITY_OBJ).  Keeping the target means the brief's gate
# ("your new files must fit the one shipped translation unit") still has a name to run, and
# it can no longer drift from what ships.
uop-unity: $(UNITY_OBJ)
	@echo "uop-unity: the decoder and its tables compile inside the shipped unity TU"

$(NASM_CORPUS): gen/nasm_corpus.py $(NASM_CORPUS_DEP)
	@mkdir -p $(BUILD)
	$(PYTHON) gen/nasm_corpus.py --out $@ $(NASM_WORKDIR)

# PERF-FIX (PERF-1R's SHOULD-FIX): the EXACT unity guard -- defined-global symbol parity
# PLUS the outside-reference preservation gen/unity_symbols.py documents.  It reads the
# COMPILED objects, so it sees the one silent class the generator's scan cannot (a unit
# `static` whose name is a global defined OUTSIDE the unit); see the script's header for
# the MEASURED asymmetry that made the parity check alone insufficient.
#
# The per-TU object list is derived from $(UNITY_SRC) -- the SAME list the generator is
# given -- so whatever joins the unit (M2's decoder files) is covered without a second
# list to keep in sync.  The watched files are the rest of the shipped link: src/hw
# (already per-TU) and softfloat.
UNITY_WATCH := $(HW_SRC) src/main.c $(SF_SRC) $(CPU_HDR) $(HW_HDR)
UNITY_PERTU_OBJ := $(patsubst src/cpu/%.c,$(BUILD)/cpu/%.o,$(UNITY_SRC))
UNITY_SYM_OK := $(BUILD)/.unity-symbols-ok

# PERF-1: the unity TU.  `-I .` is for the generator's `#include "src/cpu/....c"` paths
# (it is emitted into $(BUILD)); a quoted include inside an included file still resolves
# against THAT FILE's own directory, which is what src/hw's "../cpu/cpu.h" relies on.
# -Werror is kept: the core compiles as one TU with ZERO warnings (MEASURED).
# --watch is PERF-FIX: the generator also refuses a unit static whose name is declared or
# defined outside the unit (the silent class, here as a heuristic gate).
$(UNITY_C): gen/unity_c.py $(UNITY_SRC) $(CPU_HDR) Makefile $(UNITY_WATCH)
	@mkdir -p $(BUILD)
	$(PYTHON) gen/unity_c.py --watch $(UNITY_WATCH) -o $@ $(UNITY_SRC)

$(UNITY_OBJ): $(UNITY_C) $(CPU_HDR)
	$(CC) $(CPU_CFLAGS) -I . -I src -c -o $@ $<

$(UNITY_SYM_OK): $(UNITY_OBJ) $(UNITY_PERTU_OBJ) gen/unity_symbols.py
	$(PYTHON) gen/unity_symbols.py --unity $(UNITY_OBJ) $(UNITY_PERTU_OBJ) -- $(HW_OBJ) $(SF_OBJ)
	@touch $@

# The exact check as its own target, and a prerequisite of `selftest`: `make` no longer
# compiles the core per-TU (the shipped binary is the unity TU), so this is ALSO the
# answer to "tested != shipped" -- the unity object is now checked structurally on every
# `make selftest`.
unity-symbols: $(UNITY_SYM_OK)
	@echo "unity-symbols: per-TU and unity defined-global symbol sets identical, outside references preserved"

unity: $(UNITY_C)

# The unity source list, for a caller that needs to know exactly what is in the unit
# (results/perf-1/guard-asymmetry.sh feeds it back to the generator to assert the guard
# still emits for the shipped set).
unity-src:
	@echo $(UNITY_SRC)

# PERF-1: the benchmark the unit's numbers were measured with (a script, not a make
# rule: it must run against ANY binary path -- the reverted build for the attribution,
# the host-cc leg for the cosmocc-vs-glibc gap -- and it must refuse to report a number
# when a run did not execute the same workload).
bench: $(BUILD)/v86c
	tests/perf_bench.sh --runs 7 --bin $(BUILD)/v86c --label "make bench"

# --- S0 (results/x86-64-s0.md): THE ENGINE IDENTITY GATE --------------------------
# The seam's whole claim is that cycle_internal's engine half is the SELECTED backend's
# ->run_batch.  The C selftest (src/cpu/cpu_selftest.c's test_cpu_backend) pins the
# resolver, the name and the run_batch SYMBOL by pointer identity; what it CANNOT reach is
# the routing, because that suite has no run loop.  This target is the second instrument:
# the shipped binary is run once per engine and the ENGINES' OWN counters must separate the
# three, which they can only do if the selection really chose the engine:
#
#   interp   the decoded-block cache never ran          uop lookups 0
#   uop      it ran, compiled path held down for it     uop lookups > 0, jit compiled 0
#   uopjit   it ran, compiled path on                   uop lookups > 0, jit compiled > 0
#
# NOT three pinned numbers: those counts belong to the guest image, and a change to them is
# another unit's finding.  What is asserted is the SHAPE -- zero / positive-and-uncompiled /
# positive-and-compiled -- which is exactly what a dispatch hard-wired to one engine (or a
# selection that resolves everything to one instance) destroys.
engine-identity: $(BUILD)/v86c tests/test.img
	@set -e; \
	run() { ./$(BUILD)/v86c tests/test.img 200000 --cpu-engine "$$1" --uop-cache-stats --jit-stats 2>&1 >/dev/null; }; \
	lookups() { run "$$1" | sed -n 's/^uop-cache: lookups \([0-9][0-9]*\).*/\1/p'; }; \
	compiled() { run "$$1" | sed -n 's/^jit: pages alloc [0-9][0-9]*, blocks compiled \([0-9][0-9]*\).*/\1/p'; }; \
	i_l=$$(lookups interp); u_l=$$(lookups uop); j_l=$$(lookups uopjit); \
	i_c=$$(compiled interp); u_c=$$(compiled uop); j_c=$$(compiled uopjit); \
	echo "engine-identity: lookups interp=$${i_l:-?} uop=$${u_l:-?} uopjit=$${j_l:-?};" \
	     "compiled interp=$${i_c:-?} uop=$${u_c:-?} uopjit=$${j_c:-?}"; \
	[ "$$i_l" = 0 ] || { echo "engine-identity: 'interp' ran the decoded-block cache (lookups $$i_l)"; exit 1; }; \
	[ "$$i_c" = 0 ] || { echo "engine-identity: 'interp' compiled blocks ($$i_c)"; exit 1; }; \
	[ "$${u_l:-0}" -gt 0 ] || { echo "engine-identity: 'uop' did not run the decoded-block cache"; exit 1; }; \
	[ "$$u_c" = 0 ] || { echo "engine-identity: 'uop' compiled $$u_c blocks -- the compiled path is not held down"; exit 1; }; \
	[ "$${j_l:-0}" -gt 0 ] || { echo "engine-identity: 'uopjit' did not run the decoded-block cache"; exit 1; }; \
	[ "$${j_c:-0}" -gt 0 ] || { echo "engine-identity: 'uopjit' compiled no block -- the compiled path is not on"; exit 1; }; \
	echo "engine-identity: the three selections are observably different (gate passed)"

# --- S1 (results/x86-64-s1-s2.md): THE POISON PROBE ------------------------------
# S1 widens the register file and the lazy-flag operands to 64 bits IN PLACE, and its claim
# is that nothing 32-bit reads the high half.  The probe is the negative control for that
# claim: the shipped binary seeds every widened field's high half (and the debug registers
# and the two lazy operands) with 0xA5A5A5A5 and runs the M0 image, and the transcript must
# be BYTE-IDENTICAL to the unseeded run.  If it moves, a widened field leaked into a 32-bit
# path -- that is the finding, not a re-pin.
#
# The M0 pin is checked by transcript-diff; this target checks the SEED's invisibility, and
# both numbers are printed so a reader can see they are the same document.
poison-probe: $(BUILD)/v86c $(IMAGE)
	@set -e; 	./$(BUILD)/v86c $(IMAGE) $(INSTRUCTION_LIMIT) > $(BUILD)/poison-clean.txt; 	./$(BUILD)/v86c $(IMAGE) $(INSTRUCTION_LIMIT) --poison > $(BUILD)/poison-seeded.txt 2>/dev/null; 	cmp $(BUILD)/poison-clean.txt $(BUILD)/poison-seeded.txt; 	a=$$(sha256sum < $(BUILD)/poison-clean.txt | cut -c1-16); 	b=$$(sha256sum < $(BUILD)/poison-seeded.txt | cut -c1-16); 	echo "poison-probe: the widened fields' high halves do not reach the M0 transcript ($$a == $$b)"

# PERF-1: the core is linked from $(UNITY_OBJ); $(GEN_OBJ) $(LONG_MODE_GEN_OBJ) is still a prerequisite (it is
# the object the results/*-mut-rig.sh scripts link by path, and the selftest links take it
# too) but is INSIDE the unity object, so it is not on the command line.
$(BUILD)/v86c: src/main.c $(UNITY_OBJ) $(GEN_OBJ) $(LONG_MODE_GEN_OBJ) $(HW_OBJ) $(SF_OBJ) $(MINIBIOS_EMBED)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(PERF_CFLAGS) -I src $(GC_LDFLAGS) -o $@ src/main.c $(MINIBIOS_EMBED) $(UNITY_OBJ) $(HW_OBJ) $(SF_OBJ)

# M1a: the core's translation units, compiled standalone (see the header note).
$(BUILD)/cpu/%.o: src/cpu/%.c $(CPU_HDR)
	@mkdir -p $(BUILD)/cpu
	$(CC) $(CPU_CFLAGS) -c -o $@ $<

# M2-1: the device layer's own rule (same flags as the core: the brief's gate is "compile
# clean under -Wall -Wextra", and a warning here is as much a defect as one in the core).
$(BUILD)/hw/%.o: src/hw/%.c $(HW_HDR) $(CPU_HDR)
	@mkdir -p $(BUILD)/hw
	$(CC) $(CPU_CFLAGS) -c -o $@ $<

# M1b-3: the Berkeley SoftFloat engine's rule (see the variable block above for why it is
# its own, unwarned rule).
$(SF_OBJ): $(SF_SRC)
	@mkdir -p $(BUILD)/sf
	$(CC) -std=gnu11 -O2 $(PERF_CFLAGS) $(SF_DEFINES) $(SF_SECTIONS) -c -o $@ $<

# M2-2 (HANDOFF=v86c): THE FIRMWARE.  tests/minibios.asm is the 16-bit shim BOTH machines run
# for the bzImage boot gate (see its header, and results/m2-2-boot.md).  It is linked FLAT AT
# 0 (a real-mode segment offset IS the binary's own offset -- the image is executed with
# CS=0xF000 at 0xF0000), so the assembled file's last 16 bytes are the reset vector 0xFFFF0.
# It is a BUILD PRODUCT: `make` regenerates it from the assembly, and the embedded copy
# (gen/minibios_bin.py -> build/minibios_embed.c) is regenerated from it, so neither can be
# stale.  The 64 KiB size check is not decoration: the reset vector's position depends on it.
$(MINIBIOS_BIN): $(MINIBIOS_ASM)
	@mkdir -p $(BUILD)
	$(AS) $(ASFLAGS) -o $(BUILD)/minibios.o $<
	$(LD) -m elf_i386 -Ttext 0 -o $(BUILD)/minibios.elf $(BUILD)/minibios.o
	$(OBJCOPY) -O binary -j .text $(BUILD)/minibios.elf $@
	@test $$(stat -c %s $@) -eq 65536 || { echo "minibios: not 64 KiB"; exit 1; }

$(MINIBIOS_EMBED): $(MINIBIOS_BIN) gen/minibios_bin.py
	$(PYTHON) gen/minibios_bin.py $(MINIBIOS_BIN) $@

# M2-3: the 80 MiB variant.  Same source, same layout, one different constant -- the
# 64 KiB size check is repeated because the reset vector's position depends on it.
$(MINIBIOS_80_BIN): $(MINIBIOS_ASM)
	@mkdir -p $(BUILD)
	$(AS) $(ASFLAGS) --defsym MEM_TOP=$(MINIBIOS_80_MEM_TOP) -o $(BUILD)/minibios80.o $<
	$(LD) -m elf_i386 -Ttext 0 -o $(BUILD)/minibios80.elf $(BUILD)/minibios80.o
	$(OBJCOPY) -O binary -j .text $(BUILD)/minibios80.elf $@
	@test $$(stat -c %s $@) -eq 65536 || { echo "minibios-80m: not 64 KiB"; exit 1; }

minibios: $(MINIBIOS_BIN)

.PHONY: minibios minibios-mem

minibios-mem: $(MINIBIOS_80_BIN)

# M1a's gate G1/G2: the unit tests.  Assertions are ON (no -DNDEBUG), which is what
# makes the flags invariants checkable.
selftest: $(BUILD)/cpu_selftest $(BUILD)/cpu_m1c_selftest $(BUILD)/hw_selftest $(BUILD)/cpu_uop_selftest $(BUILD)/cpu_uop_exec_selftest $(BUILD)/cpu_jit_selftest $(BUILD)/cpu_jit_codegen_selftest uop-unity $(UNITY_SYM_OK) engine-identity poison-probe
	./$(BUILD)/cpu_selftest
	./$(BUILD)/cpu_m1c_selftest
	./$(BUILD)/hw_selftest
	./$(BUILD)/cpu_uop_selftest $(NASM_CORPUS)
	./$(BUILD)/cpu_uop_exec_selftest
	./$(BUILD)/cpu_jit_selftest
	./$(BUILD)/cpu_jit_codegen_selftest
	@echo "selftest: all suites green"

# Each binary is its own file target since M2-1: the asymmetry rig (results/m2-1-mut-rig.sh)
# rebuilds ONE of them after a single-line mutation, which needs a real rule rather than a
# recipe inside the phony target.
$(BUILD)/cpu_selftest: src/cpu/cpu_selftest.c src/cpu/cpu_backend.c $(SELFTEST_SRC) src/cpu/*.h $(SF_OBJ) $(UOP_C_OBJ) $(UOP_OBJ) $(JIT_OBJ) $(JIT_CG_SRC) $(LONG_MODE_GEN)
	@mkdir -p $(BUILD)
	$(CC) $(CPU_CFLAGS) $(GC_LDFLAGS) -o $@ src/cpu/cpu_selftest.c $(SELFTEST_SRC) src/cpu/cpu_backend.c $(UOP_C_OBJ) $(UOP_OBJ) $(JIT_OBJ) $(JIT_CG_SRC) $(SF_OBJ) $(LONG_MODE_GEN)

$(BUILD)/cpu_m1c_selftest: src/cpu/m1c_selftest.c $(CPU_SRC) $(HW_SRC) src/cpu/*.h $(HW_HDR) $(GEN_OBJ) $(LONG_MODE_GEN_OBJ) $(UOP_OBJ) $(SF_OBJ)
	@mkdir -p $(BUILD)
	$(CC) $(CPU_CFLAGS) $(GC_LDFLAGS) -o $@ src/cpu/m1c_selftest.c $(CPU_SRC) $(HW_SRC) $(GEN_OBJ) $(LONG_MODE_GEN_OBJ) $(UOP_OBJ) $(SF_OBJ)

# PERF-3 M1's gate: THE DECODE CHECKER.  It links the REAL core (CPU_SRC), the real device
# layer (for the io_port_* bridge the IN/OUT bodies call) and the generated uop tables, but
# NOT interpreter_gen.c's two switches as a separate object -- $(GEN_OBJ) $(LONG_MODE_GEN_OBJ) is the dispatcher
# its differential drives, exactly as cpu_m1c_selftest takes it.
# It does NOT link $(HW_SRC): the checker supplies its own io_port_* bridge (a benign,
# counting stub -- see the file), because the device layer's unclaimed-port handler STOPS the
# machine, which would end a probe inside its own instruction.
# NOTE (PERF-4/M2): $(UOP_SRC) is IN $(CPU_SRC) since M2, so this link must NOT also name
# $(UOP_C_OBJ) -- it would be both a source and an object (MEASURED: multiple definition of
# uop_run and friends).  $(UOP_OBJ) stays: the generated tables are not in $(CPU_SRC).
$(BUILD)/cpu_uop_selftest: src/cpu/uop_cache_selftest.c $(CPU_SRC) src/cpu/*.h $(CPU_HDR) $(UOP_HDR) $(GEN_OBJ) $(LONG_MODE_GEN_OBJ) $(UOP_OBJ) $(SF_OBJ) $(NASM_CORPUS)
	@mkdir -p $(BUILD)
	$(CC) $(CPU_CFLAGS) $(GC_LDFLAGS) -o $@ src/cpu/uop_cache_selftest.c $(CPU_SRC) $(GEN_OBJ) $(LONG_MODE_GEN_OBJ) $(UOP_OBJ) $(SF_OBJ)

# PERF-4 M2's gate (the plan's ss8 step 6): THE EXECUTOR'S OWN SUITE -- the SMC selftest, the
# STAIRCASE PIN (a cache-on vs cache-off counter trace over a stream with jcc loops, mid-block
# flushes, page-cross ends and a fetch fault) and the state-key pin.  It links the REAL core
# including the cache and the generated tables (uop_cache.c is in $(CPU_SRC) since M2), and
# like the M1 checker it supplies its own io_port_* bridge and does NOT link src/hw.
$(BUILD)/cpu_uop_exec_selftest: $(UOP_EXEC_SELFTEST) $(CPU_SRC) src/cpu/*.h $(CPU_HDR) $(UOP_HDR) $(GEN_OBJ) $(LONG_MODE_GEN_OBJ) $(UOP_OBJ) $(SF_OBJ)
	@mkdir -p $(BUILD)
	$(CC) $(CPU_CFLAGS) $(GC_LDFLAGS) -o $@ $(UOP_EXEC_SELFTEST) $(CPU_SRC) $(GEN_OBJ) $(LONG_MODE_GEN_OBJ) $(UOP_OBJ) $(SF_OBJ)

# JIT-SPIKE (HANDOFF=v86c-jit): THE EXECUTABLE-MEMORY SUBSTRATE'S GATE.  It links the REAL core
# (CPU_SRC, which now carries src/cpu/jit_mem.c), the generated dispatcher and the uop tables,
# because the property it pins -- a guest write to a code page must drop the block compiled from
# it -- is a property of the WIRING (the PERF-FIX-3 hook -> uop_invalidate_page ->
# jit_code_invalidate_page) and a unit-linked stub would not exercise it.  Like the two M1/M2
# suites it supplies its own io_port_* bridge and does not link src/hw.
#
# It is IN `make selftest` deliberately (a gate that is not run rots), and it adds a suite
# rather than changing one: the four existing suites' counts are untouched by it.
$(BUILD)/cpu_jit_selftest: src/cpu/jit_selftest.c $(CPU_SRC) src/cpu/*.h $(CPU_HDR) $(GEN_OBJ) $(LONG_MODE_GEN_OBJ) $(UOP_OBJ) $(SF_OBJ)
	@mkdir -p $(BUILD)
	$(CC) $(CPU_CFLAGS) $(GC_LDFLAGS) -o $@ src/cpu/jit_selftest.c $(CPU_SRC) $(GEN_OBJ) $(LONG_MODE_GEN_OBJ) $(UOP_OBJ) $(SF_OBJ)

# JIT-PLAN (HANDOFF=v86c-jit, unit JIT-1): M1'S GATE -- the encoder's byte pins, the one
# recipe run differentially (jit-off vs jit-on: full cpu state, pending, RAM), the fault/
# early-return, staircase, prefix/previous_ip, SMC-abort and exit-kind contract pins.  Same
# shape as the substrate's suite above and for the same reason: it RUNS the machine.
$(BUILD)/cpu_jit_codegen_selftest: src/cpu/jit_codegen_selftest.c $(CPU_SRC) src/cpu/*.h $(CPU_HDR) $(GEN_OBJ) $(LONG_MODE_GEN_OBJ) $(UOP_OBJ) $(SF_OBJ)
	@mkdir -p $(BUILD)
	$(CC) $(CPU_CFLAGS) $(GC_LDFLAGS) -o $@ src/cpu/jit_codegen_selftest.c $(CPU_SRC) $(GEN_OBJ) $(LONG_MODE_GEN_OBJ) $(UOP_OBJ) $(SF_OBJ)

jit-codegen-selftest: $(BUILD)/cpu_jit_codegen_selftest
	./$(BUILD)/cpu_jit_codegen_selftest

jit-selftest: $(BUILD)/cpu_jit_selftest
	./$(BUILD)/cpu_jit_selftest

# M2-1's device gate (the brief's G1): the machine's devices driven through their register
# interfaces, linked against the REAL device layer (src/hw) and the real core.
$(BUILD)/hw_selftest: src/hw/hw_selftest.c $(CPU_SRC) $(HW_SRC) $(CPU_HDR) $(HW_HDR) $(GEN_OBJ) $(LONG_MODE_GEN_OBJ) $(UOP_OBJ) $(SF_OBJ)
	@mkdir -p $(BUILD)
	$(CC) $(CPU_CFLAGS) $(GC_LDFLAGS) -o $@ src/hw/hw_selftest.c $(CPU_SRC) $(HW_SRC) $(GEN_OBJ) $(LONG_MODE_GEN_OBJ) $(UOP_OBJ) $(SF_OBJ)

run: $(BUILD)/v86c $(IMAGE)
	./$(BUILD)/v86c $(IMAGE) $(INSTRUCTION_LIMIT)

image: $(IMAGE)

$(IMAGE): tests/test.asm tests/test.ld
	@mkdir -p $(BUILD)
	$(AS) $(ASFLAGS) -o $(BUILD)/test.o tests/test.asm
	$(LD) $(LDFLAGS) -o $(BUILD)/test.elf $(BUILD)/test.o
	$(OBJCOPY) -O binary $(BUILD)/test.elf $@

# VGA-U1's differential image (tests/gate-vga-1.sh): self-programs a text mode, draws cells
# with bg != fg, sets a non-zero cursor, reads back and halts.  The same build shape as
# test.img (as --32 + test.ld + objcopy).
tests/vga-text.img: tests/vga-text.asm tests/test.ld
	@mkdir -p $(BUILD)
	$(AS) $(ASFLAGS) -o $(BUILD)/vga-text.o tests/vga-text.asm
	$(LD) $(LDFLAGS) -o $(BUILD)/vga-text.elf $(BUILD)/vga-text.o
	$(OBJCOPY) -O binary $(BUILD)/vga-text.elf $@

# VGA-U2's differential image (tests/gate-vga-2.sh): self-programs mode 13h, paints a
# 16-colour ramp + edge cells, reads back through read modes 0 and 1, halts.  Same shape.
tests/vga-mode13.img: tests/vga-mode13.asm tests/test.ld
	@mkdir -p $(BUILD)
	$(AS) $(ASFLAGS) -o $(BUILD)/vga-mode13.o tests/vga-mode13.asm
	$(LD) $(LDFLAGS) -o $(BUILD)/vga-mode13.elf $(BUILD)/vga-mode13.o
	$(OBJCOPY) -O binary $(BUILD)/vga-mode13.elf $@

# VGA-U3's differential images (tests/gate-vga-3.sh): the planar 16-colour path.  planar12 is
# mode 0x12 (640x480x16, chain-2, shift 0x00) mixing write modes 0-3 + rotate/logical/setreset/
# bitmask + colorset on one frame; planar4 is the packed shift 0x20 (CGA 4-colour); planar0d is
# the cheap mode-0x0D geometry (320x200x16); planarsub reaches the row-substitution arm.  Same
# build shape (as --32 + test.ld + objcopy).
tests/vga-planar12.img: tests/vga-planar12.asm tests/test.ld
	@mkdir -p $(BUILD)
	$(AS) $(ASFLAGS) -o $(BUILD)/vga-planar12.o tests/vga-planar12.asm
	$(LD) $(LDFLAGS) -o $(BUILD)/vga-planar12.elf $(BUILD)/vga-planar12.o
	$(OBJCOPY) -O binary $(BUILD)/vga-planar12.elf $@

tests/vga-planar4.img: tests/vga-planar4.asm tests/test.ld
	@mkdir -p $(BUILD)
	$(AS) $(ASFLAGS) -o $(BUILD)/vga-planar4.o tests/vga-planar4.asm
	$(LD) $(LDFLAGS) -o $(BUILD)/vga-planar4.elf $(BUILD)/vga-planar4.o
	$(OBJCOPY) -O binary $(BUILD)/vga-planar4.elf $@

tests/vga-planar0d.img: tests/vga-planar0d.asm tests/test.ld
	@mkdir -p $(BUILD)
	$(AS) $(ASFLAGS) -o $(BUILD)/vga-planar0d.o tests/vga-planar0d.asm
	$(LD) $(LDFLAGS) -o $(BUILD)/vga-planar0d.elf $(BUILD)/vga-planar0d.o
	$(OBJCOPY) -O binary $(BUILD)/vga-planar0d.elf $@

tests/vga-planarsub.img: tests/vga-planarsub.asm tests/test.ld
	@mkdir -p $(BUILD)
	$(AS) $(ASFLAGS) -o $(BUILD)/vga-planarsub.o tests/vga-planarsub.asm
	$(LD) $(LDFLAGS) -o $(BUILD)/vga-planarsub.elf $(BUILD)/vga-planarsub.o
	$(OBJCOPY) -O binary $(BUILD)/vga-planarsub.elf $@

# VGA-U4's differential image (tests/gate-vga-4.sh): self-programs a Bochs VBE 320x200x32
# mode through the dispi interface, paints the LFB through 0xE0000000 + a bank-switched
# 0xA0000 write, reads back in-guest and halts.  Same build shape (as --32 + test.ld +
# objcopy).
tests/vga-lfb.img: tests/vga-lfb.asm tests/test.ld
	@mkdir -p $(BUILD)
	$(AS) $(ASFLAGS) -o $(BUILD)/vga-lfb.o tests/vga-lfb.asm
	$(LD) $(LDFLAGS) -o $(BUILD)/vga-lfb.elf $(BUILD)/vga-lfb.o
	$(OBJCOPY) -O binary $(BUILD)/vga-lfb.elf $@

# HANDOFF=v86c-repo-weight: the 64 MiB guest-gate disk, materialised from the
# committed xz (the raw image is not tracked; the helper's contract -- default
# path only, never clobber, sha256-verified before it is put in place -- is in
# tests/disk-fmt.sh).  Deliberately NOT a prerequisite of `all`: a 64 MB
# decompress must not become part of the default build; the gates that need the
# disk run the helper themselves.
DISK_FMT_IMG := tests/images/disk-fmt-64m.img

.PHONY: disk-fmt
disk-fmt: $(DISK_FMT_IMG)

$(DISK_FMT_IMG): tests/images/disk-fmt-64m.img.xz tests/disk-fmt.sh
	. tests/disk-fmt.sh

# --- REBOOT-1: the TWO-BOOT reboot gate image (results/reboot-1.md, G1) ---------
# One source, two trigger variants: the PCI dance (0x02 then 0x06 to 0xCF9) and
# the PS/2 command (0xFE to 0x64).  The guest writes a memory marker in boot 1,
# fires the trigger, and boot 2 reads the witnesses back -- see the file header.
REBOOT_AFLAGS := $(ASFLAGS)
tests/reboot-pci.img: tests/reboot2x.asm tests/test.ld
	@mkdir -p $(BUILD)
	$(AS) $(REBOOT_AFLAGS) --defsym TRIGGER_PS2=0 -o $(BUILD)/reboot-pci.o tests/reboot2x.asm
	$(LD) $(LDFLAGS) -o $(BUILD)/reboot-pci.elf $(BUILD)/reboot-pci.o
	$(OBJCOPY) -O binary $(BUILD)/reboot-pci.elf $@

tests/reboot-ps2.img: tests/reboot2x.asm tests/test.ld
	@mkdir -p $(BUILD)
	$(AS) $(REBOOT_AFLAGS) --defsym TRIGGER_PS2=1 -o $(BUILD)/reboot-ps2.o tests/reboot2x.asm
	$(LD) $(LDFLAGS) -o $(BUILD)/reboot-ps2.elf $(BUILD)/reboot-ps2.o
	$(OBJCOPY) -O binary $(BUILD)/reboot-ps2.elf $@

# REBOOT-FIX-1: the two ASYMMETRY images (results/reboot-fix-1.md, G1 + G2).
# reboot-fwpage: the SF1 corner -- the trigger EXECUTED FROM PAGE 0xFF000 (the
#   reset vector's page); pre-fix the compiled TAIL arm runs boot-2 code in the
#   stale batch, visible as the 0x101028 marker + verdict 0xFACD0007.
# reboot-geom: the SF2 corner -- the triggering batch's count equals the
#   cumulative count at its cycle's entry (19 = 19; PADS = 19 - 2, see the
#   image's header); pre-fix cycle_internal's DBG_ASSERT aborts the process.
tests/reboot-fwpage.img: tests/reboot-fwpage.asm tests/test.ld
	@mkdir -p $(BUILD)
	$(AS) $(REBOOT_AFLAGS) -o $(BUILD)/reboot-fwpage.o tests/reboot-fwpage.asm
	$(LD) $(LDFLAGS) -o $(BUILD)/reboot-fwpage.elf $(BUILD)/reboot-fwpage.o
	$(OBJCOPY) -O binary $(BUILD)/reboot-fwpage.elf $@

tests/reboot-geom.img: tests/reboot-geom.asm tests/test.ld
	@mkdir -p $(BUILD)
	$(AS) $(REBOOT_AFLAGS) --defsym PADS=13 -o $(BUILD)/reboot-geom.o tests/reboot-geom.asm
	$(LD) $(LDFLAGS) -o $(BUILD)/reboot-geom.elf $(BUILD)/reboot-geom.o
	$(OBJCOPY) -O binary $(BUILD)/reboot-geom.elf $@

# PAE-1's Level-2 differential guest (results/pae-1.md): a synthetic PAE workload
# (3-level walk, 4 KiB + 2 MiB mappings, an NX #PF through a minimal IDT) run under
# both legs -- see tests/pae-guest.asm and tests/gate-pae-1.sh.
tests/pae-guest.img: tests/pae-guest.asm tests/test.ld
	@mkdir -p $(BUILD)
	$(AS) $(ASFLAGS) -o $(BUILD)/pae-guest.o tests/pae-guest.asm
	$(LD) $(LDFLAGS) -o $(BUILD)/pae-guest.elf $(BUILD)/pae-guest.o
	$(OBJCOPY) -O binary $(BUILD)/pae-guest.elf $@

.PHONY: reboot-images
reboot-images: tests/reboot-pci.img tests/reboot-ps2.img tests/reboot-fwpage.img tests/reboot-geom.img

expected: tests/model.py
	@mkdir -p results
	$(PYTHON) tests/model.py --out $(EXPECTED) > /dev/null

# The gate: build the image, run the reference implementation, and require the
# transcript to match the independent model.
oracle: $(IMAGE) expected
	@mkdir -p results
	$(NODE) oracle/run.mjs $(IMAGE) $(INSTRUCTION_LIMIT) > $(TRANSCRIPT)
	$(PYTHON) tests/check_transcript.py $(EXPECTED) $(TRANSCRIPT)

determinism: $(IMAGE)
	@mkdir -p results
	$(NODE) oracle/run.mjs $(IMAGE) $(INSTRUCTION_LIMIT) > results/transcript.run1.txt
	$(NODE) oracle/run.mjs $(IMAGE) $(INSTRUCTION_LIMIT) > results/transcript.run2.txt
	diff -u results/transcript.run1.txt results/transcript.run2.txt
	@echo "determinism: two consecutive oracle runs are byte-identical"

# --- M1a-4: THE MILESTONE -- the C binary's transcript, byte for byte -------------
# README.md's contract, from the other side: same image, same limit, and the two
# documents must be identical.  Both are nonempty or the diff is meaningless, so the
# C leg's exit status is checked first (a nonzero exit means NO transcript existed).
transcript-diff: $(BUILD)/v86c $(IMAGE)
	@mkdir -p results
	./$(BUILD)/v86c $(IMAGE) $(INSTRUCTION_LIMIT) > results/transcript.c.txt
	$(NODE) oracle/run.mjs $(IMAGE) $(INSTRUCTION_LIMIT) > results/transcript.js.txt
	diff -u results/transcript.js.txt results/transcript.c.txt
	@echo "milestone: the C transcript is byte-identical to the reference's"

# --- P0: the nasm differential oracle (v86/tests/nasm/*.asm, READ-ONLY) ---------
# Every stage is unconditional: a stale .s/.o/.img/.fixture must never be read as a
# result of the current sources.  See tests/nasm_run.py for the pipeline, the two
# measured link deviations, and the run.js comparison it transcribes.
$(NASM_MKFIXTURE): tests/mkfixture.c
	@mkdir -p $(BUILD)
	$(HOSTCC) $(CFLAGS) -o $@ $<

nasm-translate:
	@mkdir -p $(NASM_WORKDIR)
	$(PYTHON) tests/nasm_run.py --stage translate

nasm-fixtures: $(NASM_MKFIXTURE)
	@mkdir -p $(NASM_WORKDIR)
	$(PYTHON) tests/nasm_run.py --stage fixtures

# The C leg is the ACTUAL side from M1a-4 on (the port exists now).  Note the separate
# skip list: tests/nasm_skip.txt lists the REFERENCE leg's blind spots (P0), and the
# port's own gaps are a different set with different reasons (tests/nasm_skip_c.txt).
# THE LAUNCHER.  build/v86c is an APE -- a polyglot whose kernel exec gets ENOEXEC and
# whose re-exec fallback lives in a SHELL, so a process spawned directly (which is how
# tests/nasm_run.py starts the actual leg: subprocess.run, no shell) cannot start it,
# while `./build/v86c` in a terminal can.  This one-line launcher is that shell; it is a
# build product, not a source file.
$(BUILD)/v86c-nasm: $(BUILD)/v86c
	@printf '#!/bin/sh\nexec ./%s "$$@"\n' '$(BUILD)/v86c' > $@
	@chmod +x $@

# --- S4: the x86-64 corpus's harness, launcher and gate -------------------------
# The launcher exists for the same measured reason build/v86c-nasm does: the APE's kernel
# exec path gets ENOEXEC and its re-exec fallback lives in a shell, so a process spawned
# directly (which is how tests/nasm64_run.py starts the actual leg) cannot start it.
$(NASM64_MKFIXTURE): tests/mkfixture64.c
	@mkdir -p $(BUILD)
	$(HOSTCC) $(CFLAGS) -o $@ $<

$(BUILD)/v86c-nasm64: $(BUILD)/v86c
	@printf '#!/bin/sh\nexec ./%s "$$@"\n' '$(BUILD)/v86c' > $@
	@chmod +x $@

# The corpus is a REPO ARTIFACT (tests/nasm64/*.asm): it is generated, and checked in, so
# that a reader can see what each slice will be graded on without running a generator.
nasm64-gen:
	$(PYTHON) gen/nasm64_gen.py --out tests/nasm64

# S4'S GATE IS NEGATIVE (results/x86-64-plan.md ss6 S4): the comparison must be shown to
# bite on a perturbed fixture, the harness's own controls must hold, and every case must be
# RED with a named `unimplemented:` stop -- the machine cannot execute 64-bit code, so a
# green corpus today would have tested nothing.  tests/gate-nasm64.sh is the gate; its exit
# code is the verdict.
nasm64: $(NASM64_MKFIXTURE) $(BUILD)/v86c-nasm64 nasm64-gen
	@mkdir -p $(NASM64_WORKDIR)
	sh tests/gate-nasm64.sh

nasm: $(NASM_MKFIXTURE) $(BUILD)/v86c-nasm
	@mkdir -p $(NASM_WORKDIR)
	$(PYTHON) tests/nasm_run.py --stage run \
		--actual "$(BUILD)/v86c-nasm {img} {limit} --fixture" \
		--skip-list tests/nasm_skip_c.txt

# The reference leg, kept so the two skip lists can be compared at any time.
nasm-js: $(NASM_MKFIXTURE)
	@mkdir -p $(NASM_WORKDIR)
	$(PYTHON) tests/nasm_run.py --stage run

clean:
	rm -rf $(BUILD)

tests/reboot-loop.img: tests/reboot2x.asm tests/test.ld
	@mkdir -p $(BUILD)
	$(AS) $(REBOOT_AFLAGS) --defsym TRIGGER_PS2=1 --defsym REBOOT_LOOP=1 -o $(BUILD)/reboot-loop.o tests/reboot2x.asm
	$(LD) $(LDFLAGS) -o $(BUILD)/reboot-loop.elf $(BUILD)/reboot-loop.o
	$(OBJCOPY) -O binary $(BUILD)/reboot-loop.elf $@
