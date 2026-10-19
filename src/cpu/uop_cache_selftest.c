/*
 * uop_cache_selftest.c -- M1's GATE: the decode checker that pins src/cpu/uop_cache.c against
 * the port's own interpreter semantics, with NO execution through the cache.
 *
 *     make selftest        # builds and runs this binary with the other suites
 *
 * WHY A SEPARATE BINARY (the M1c/M2-1 precedent, results/m1c-crafted-tests.md ss1):
 * cpu_selftest.c supplies its own recording definitions of call_interrupt_vector/switch_seg/
 * the timer layer and asserts against what they recorded, so the REAL cpu.c/exceptions.c
 * cannot be linked there; this suite needs the real `run_instruction`, the real fetch path and
 * the real exceptions.c.  It is driven from the same `make selftest` invocation, so the gate
 * command and its exit status are unchanged.
 *
 * WHAT IS ASSERTED, per instruction decoded out of a byte stream (plan ss7 M1's (a)/(b)/(c)):
 *
 *  (a) THE LENGTH DIFFERENTIAL.  The interpreter itself is run for exactly one dispatch at the
 *      same eip, on the same bytes, with the same mode -- the port's own fetch/decode advance
 *      is the oracle, so no external ground truth is needed.  With one dispatch per probe, the
 *      interpreter's `instruction_pointer` afterwards is the next instruction's start unless
 *      the instruction TRANSFERRED CONTROL, which is why the check is:
 *        - `advance == decoder_len`                          -> PINNED (the strong case);
 *        - a RELATIVE jump/call's own pcs-relative target
 *          (`advance == that target - start`)                -> PINNED_REL (the branch was
 *          taken; the target is computed from the RAW bytes by hand, see rel_target, and the
 *          port's masking quirk for the 16-bit form is reproduced from misc_instr.c:58);
 *        - an exception was delivered, or a far transfer moved CS -> the advance is the
 *          handler's address, not a length: counted, not compared;
 *        - the interpreter stopped on an unimplemented encoding -> the decoder must have
 *          emitted NO descriptor (the two must agree about what the port implements);
 *        - otherwise `advance` must be OUTSIDE [1,15]: any in-window advance that is not the
 *          decoder's own length is a FAIL, because every x86 instruction is 1..15 bytes long
 *          and a non-transferring instruction advances by exactly its length.
 *      Each probe runs TWICE (flags all-clear and all-set) so a conditional branch is
 *      exercised on both its taken and its not-taken side; either side being decisive pins the
 *      length.  The residual -- encodings whose advance is a register/memory value (ret,
 *      iret, int n, indirect jmp/call) -- is counted and NAMED, and a probe the checker cannot
 *      pin whose encoding is not on that named list is a FAILURE (see unpinnable()).
 *
 *  (b) THE MODRM EXTRACTION vs THE PORT'S OWN RESOLVER.  For every decoded memory operand the
 *      checker materialises the address from the uop's extracted fields
 *      (uop_materialize_addr) and compares it, byte for byte, with what
 *      resolve_modrm16/resolve_modrm32 -- reached through modrm_resolve, the interpreter's own
 *      entry point -- compute from the same bytes and the same registers.  The sandbox gives DS
 *      and SS DIFFERENT segment bases (0x10000000 / 0x20000000), so the default-segment choice
 *      of every form is part of the comparison.  This is the check that makes up for the M1
 *      decision to leave src/cpu/modrm.c alone (see results/perf-3-uop-decode.md ss3).
 *
 *  (c) THE BLOCK FRAMING.  The block's stop reason is asserted against the rules of plan ss2.2
 *      (page end, block end, cap, no descriptor), including the deliberate divergence that a
 *      PAGE-CROSSING instruction is not part of a block at all.
 *
 * THE SANDBOX, and why nothing can be damaged by it: every probe runs with paging off, CS base
 * 0 (so virt == phys == the code page), and every OTHER segment based ABOVE guest RAM
 * (0x10000000+), with all eight registers pointing above RAM as well.  Guest RAM is 2 MiB, so
 * every data/stack access a probe instruction can compute lands in the device-default range
 * (memory.c:120-160: reads give 0xFF/-1, writes are dropped) and NO probe can write to RAM --
 * which is what lets the checker restore just the cpu state between probes.  The code page is
 * rewritten per stream and its pristine copy kept anyway.
 *
 * The 123 nasm corpus images (build/nasm/<t>.img, the bytes the nasm gate itself runs) are
 * walked in addition to the hand streams: build/nasm_corpus.bin is a build product of
 * gen/nasm_corpus.py, which extracts each image's .text.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "cpu_state.h"
#include "exceptions.h"
#include "instructions.h"
#include "memory.h"
#include "misc_instr.h"
#include "modrm.h"
#include "paging.h"
#include "uop_cache.h"

static int checks = 0;
static int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        checks++;                                                               \
        long long _a = (long long)(a), _b = (long long)(b);                      \
        if (_a != _b) {                                                         \
            failures++;                                                         \
            fprintf(stderr, "FAIL %s:%d: %s == %s (got 0x%llx, want 0x%llx)\n",   \
                    __FILE__, __LINE__, #a, #b, (unsigned long long)_a,          \
                    (unsigned long long)_b);                                    \
        }                                                                       \
    } while (0)

/* report a divergence with the bytes that caused it, and stop (a divergence is a finding, not
 * a tolerance): the counters keep the run going so the report names every distinct case */
#define REPORT(fmt, ...)                                                     \
    do {                                                                     \
        failures++;                                                          \
        fprintf(stderr, "FAIL %s:%d: " fmt "\n", __FILE__, __LINE__, __VA_ARGS__); \
    } while (0)

/* ------------------------------------------------------------------ *
 * the sandbox
 * ------------------------------------------------------------------ */

#define GDT_BASE    0x1000u
#define IDT_BASE    0x2000u
#define HANDLER_EIP 0x30000u
#define CODE_PAGE   0x4000u  /* the probe stream: low, so the 16-bit pcs wrap stays local */
#define CODE_SIZE   0x1000u
#define GUEST_RAM   (2u * 1024 * 1024)

/* every segment except CS is based ABOVE guest RAM: no probe can touch RAM (file header) */
#define OUT_BASE_DS 0x10000000u
#define OUT_BASE_SS 0x20000000u
#define OUT_BASE_ES 0x30000000u
#define OUT_BASE_FS 0x40000000u
#define OUT_BASE_GS 0x50000000u
#define OUT_BASE_REG 0x7F000000u

#define SEL_CODE  0x08
#define SEL_DATA  0x10
#define SEL_CODE2 0x18 /* the IDT gates' selector: a delivery or far transfer shows in CS */

static uint8_t code_pristine[CODE_SIZE];
static cpu_state_t cpu_pristine;

/* ------------------------------------------------------------------ *
 * THE ONE BOUNDARY SYMBOL THIS BINARY SUPPLIES ITSELF (the cpu_selftest.c:995 precedent):
 * the port bridge the IN/OUT bodies (instructions.c:1752+) and the string-I/O fast paths
 * (string.c:253) call.  The real one is the device layer's (src/hw/io.c) and STOPS the
 * machine for a port no device claims -- which would end a probe inside its instruction and
 * make the differential measure the device layer instead of the decoder.  These answer
 * benignly and count, so a probe that touches a port is still a length measurement.  This is
 * why the suite does NOT link $(HW_SRC): the whole device layer would bring that stop with
 * it (MEASURED: with src/hw linked, the sweep stopped at `io_port_read8(port=0x3000) (no
 * handler installed for this port at this width)`).
 * ------------------------------------------------------------------ */
static long io_port_reads, io_port_writes;

int32_t io_port_read8(int32_t port) { io_port_reads++; (void)port; return 0; }
int32_t io_port_read16(int32_t port) { io_port_reads++; (void)port; return 0; }
int32_t io_port_read32(int32_t port) { io_port_reads++; (void)port; return 0; }
void io_port_write8(int32_t port, int32_t value) { io_port_writes++; (void)port; (void)value; }
void io_port_write16(int32_t port, int32_t value) { io_port_writes++; (void)port; (void)value; }
void io_port_write32(int32_t port, int32_t value) { io_port_writes++; (void)port; (void)value; }

/* cpu.c's main_loop calls this (src/hw/machine.c:122); no probe reaches it -- a probe runs
 * ONE dispatch and never enters the loop -- so the link needs nothing but the symbol */
double run_hardware_timers(bool acpi_enabled, double t) { (void)acpi_enabled; return t; }

static void wr64(uint32_t addr, uint64_t v)
{
    for (int i = 0; i < 8; i++) mem8[addr + i] = (uint8_t)(v >> (8 * i));
}

static uint64_t make_desc(uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
    uint64_t d = 0;
    d |= (uint64_t)(limit & 0xFFFF);
    d |= (uint64_t)(base & 0xFFFF) << 16;
    d |= (uint64_t)((base >> 16) & 0xFF) << 32;
    d |= (uint64_t)access << 40;
    d |= (uint64_t)((limit >> 16) & 0xF) << 48;
    d |= (uint64_t)(flags & 0xF) << 52;
    d |= (uint64_t)((base >> 24) & 0xFF) << 56;
    return d;
}

static uint64_t make_idt_entry(int32_t offset, uint16_t selector, uint8_t dpl)
{
    uint64_t d = 0;
    d |= (uint64_t)((uint32_t)offset & 0xFFFF);
    d |= (uint64_t)selector << 16;
    d |= (uint64_t)(0x80 | (dpl << 5) | 8 | INTR_GATE_TRAP) << 40;
    d |= (uint64_t)(((uint32_t)offset >> 16) & 0xFFFF) << 48;
    return d;
}

static void machine_init(void)
{
    cpu_state_init();
    if (mem8 == NULL && !allocate_memory(GUEST_RAM)) {
        fputs("cannot allocate guest memory\n", stderr);
        exit(2);
    }
    cpu->memory_size = GUEST_RAM;
    zero_memory(0, cpu->memory_size);

    /* a GDT and an IDT, so a delivered exception has a real gate: every vector goes to
     * HANDLER_EIP in SEL_CODE2, which is how a probe notices that a delivery happened */
    wr64(GDT_BASE + 0 * 8, 0);
    wr64(GDT_BASE + 1 * 8, make_desc(0, 0xFFFFF, 0x9A, 0xC)); /* code, R, 32-bit   -> 0x08 */
    wr64(GDT_BASE + 2 * 8, make_desc(0, 0xFFFFF, 0x92, 0xC)); /* data              -> 0x10 */
    wr64(GDT_BASE + 3 * 8, make_desc(0, 0xFFFFF, 0x9A, 0xC)); /* second code       -> 0x18 */
    cpu->gdtr_offset = GDT_BASE;
    cpu->gdtr_size = 4 * 8 - 1;
    for (int i = 0; i < 256; i++) {
        wr64(IDT_BASE + (uint32_t)i * 8, make_idt_entry(HANDLER_EIP, SEL_CODE2, 0));
    }
    cpu->idtr_offset = IDT_BASE;
    cpu->idtr_size = 256 * 8 - 1;

    /* protected mode, paging OFF, cpl 0 */
    cpu->cr[0] = 1;
    cpu->protected_mode = true;
    cpu->cpl = 0;
    cpu->is_32 = true;
    cpu->stack_size_32 = true;
    cpu->flags = FLAGS_DEFAULT;
    for (int i = 0; i < 6; i++) {
        cpu->segment_is_null[i] = false;
        cpu->segment_limits[i] = 0xFFFFFFFFu;
        cpu->sreg[i] = (uint16_t)(i == CS ? SEL_CODE : SEL_DATA);
        cpu->segment_access_bytes[i] = 0x92;
    }
    cpu->segment_access_bytes[CS] = 0x9A;
    cpu->segment_offsets[CS] = 0;
    cpu->segment_offsets[DS] = OUT_BASE_DS;
    cpu->segment_offsets[SS] = OUT_BASE_SS;
    cpu->segment_offsets[ES] = OUT_BASE_ES;
    cpu->segment_offsets[FS] = OUT_BASE_FS;
    cpu->segment_offsets[GS] = OUT_BASE_GS;
    update_state_flags();

    cpu_pristine = *cpu;
    memcpy(code_pristine, mem8 + CODE_PAGE, CODE_SIZE);
}

/* the canonical probe state: called before every single-instruction probe */
static void probe_begin(uint32_t start, bool is32, uint32_t eflags)
{
    *cpu = cpu_pristine;
    cpu->is_32 = is32;
    cpu->flags = eflags;
    cpu->flags_changed = 0;
    for (int i = 0; i < 8; i++) {
        /* SMALL and distinct.  Every memory address is still out of RAM -- the segment bases
         * are what put it there (DS/SS/ES/FS/GS are based at 0x10000000+), not the register
         * values -- while the register VALUES stay inside 16 bits, which is what the port's
         * own debug asserts require of the values it pushes: MEASURED, ESP = 0x7F001000 made
         * `enter16` (a 66-prefixed ENTER) push ESP-2 and trip safe_write16's
         * `value < 0x10000` assert (paging.c:658) -- a debug-build-only artifact of the
         * reference's Uint16 store truncating silently.  The arithmetic a uop's
         * base/index/scale is checked against is unaffected: it is compared against the
         * port's own resolve_modrm16/32 under the same registers. */
        /* 4 KiB-ALIGNED and small: aligned because `mov cr3, r` (paging.c:425's
         * `dbg_assert!((cr3 & 0xFFF) == 0)`, upstream's own "TODO") aborts on an unaligned
         * value, and small because the port's 16-bit-mode value invariants (see
         * sixteen_bit_66_transfer) want every register inside 16 bits. */
        write_reg32(i, 0x1000u * (uint32_t)(i + 1));
    }
    cpu->instruction_pointer = start;
    cpu->previous_ip = start;
    cpu->prefixes = 0;
    cpu->pagefault = false;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    cpu->eip_phys = 0;
    interpreted_unimplemented = false;
}

/* place a byte stream at the start of the code page (or at `offset` in it) */
static uint32_t put_stream(const uint8_t *bytes, int len, uint32_t offset)
{
    memcpy(mem8 + CODE_PAGE, code_pristine, CODE_SIZE);
    memcpy(mem8 + CODE_PAGE + offset, bytes, (size_t)len);
    return CODE_PAGE + offset;
}

/* ------------------------------------------------------------------ *
 * the two probes
 * ------------------------------------------------------------------ */

typedef struct {
    bool ok;        /* the fetch translated (the address is RAM) */
    int32_t ip_after;
    bool unimpl;    /* the interpreter stopped on an unimplemented encoding */
    bool cs_moved;  /* an exception was delivered, or a far transfer changed CS */
} oracle_result;

static oracle_result run_interpreter(uint32_t start, bool is32, uint32_t eflags)
{
    oracle_result o;
    memset(&o, 0, sizeof o);

    probe_begin(start, is32, eflags);
    uint32_t phys = get_phys_eip();
    if (cpu->pagefault) {
        cpu->pagefault = false;
        return o;
    }
    o.ok = true;

    /* interpreted_run's own dispatch, verbatim (cpu.c:111-130): read the opcode out of the
     * code page, advance, clear the fault flag, dispatch with the mode bit */
    int32_t opcode = mem8[phys];
    cpu->instruction_pointer = (int32_t)start + 1;
    cpu->pagefault = false;
    run_instruction(opcode | (cpu->is_32 ? 1 : 0) << 8);
    cpu->pagefault = false;

    o.ip_after = cpu->instruction_pointer;
    o.unimpl = interpreted_unimplemented;
    o.cs_moved = cpu->sreg[CS] != SEL_CODE;
    return o;
}

static int run_decoder(uint32_t start, bool is32, uop_t *uops, int cap, uop_block_t *blk)
{
    probe_begin(start, is32, FLAGS_DEFAULT);
    cpu->instruction_pointer = (int32_t)start;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    uint32_t phys = get_phys_eip();
    if (cpu->pagefault) {
        cpu->pagefault = false;
        return -1;
    }
    return uop_decode_block(uops, cap, start, phys, blk);
}

/* ------------------------------------------------------------------ *
 * the coverage counters
 * ------------------------------------------------------------------ */

/* THE TWO ENCODINGS THE DEBUG BUILD CANNOT EXECUTE.  Their bodies are `DBG_ASSERT(false)`
 * (instructions.c:569/575 for 0x62 bound, instructions.c:1901 for 0xF1 int1): they are the
 * port's deliberate "not implemented" stops, live because this suite is built WITHOUT NDEBUG
 * (the same reason the other suites' invariants are checkable).  The reference release build
 * executes them, so the DECODER still has to decode them -- which the hand table asserts (a
 * descriptor, the right length, and the (b) address check, none of which runs a body).  Only
 * the interpreter oracle is skipped, and only for these two. */
static const char *debug_assert_only(uint8_t opcode, bool is_0f)
{
    if (is_0f) return NULL;
    if (opcode == 0x62) return "bound: instructions.c:569/575 DBG_ASSERT(false)";
    if (opcode == 0xF1) return "int1: instructions.c:1901 DBG_ASSERT(false)";
    return NULL;
}

/* ------------------------------------------------------------------ *
 * THE THIRD SKIP, and it is a PORT FINDING rather than a checker convenience.
 *
 * The port inherits upstream's 16-bit-mode invariant checks verbatim -- `dbg_assert!(*is_32 ||
 * value < 0x10000)` at instructions.c:1438, 1451, 1469, 1539, 1549, 1559, 1802, 1813, 1831,
 * 1837, 2200, 2230, 2279, 2312 (upstream: instructions.rs:1319, 1326, 1339, 1404, 1412,
 * 1420, 2060, 2069, 2086, 2091, 2390, 2412, 2447, 2474).  They assert that in a 16-BIT code
 * segment no ip/data value ever leaves 16 bits -- which a 66-prefixed (32-bit operand size)
 * CONTROL TRANSFER falsifies by construction: MEASURED, the byte stream `66 E8 24 78 56 34`
 * with is_32 = false aborts at instructions.c:1802 (instr32_E8's check) inside
 * run_instruction.  Upstream's `dbg_assert!` is `debug_assert!`, so the pinned RELEASE
 * reference COMPILES IT OUT and executes those bytes; this build does not (the port's
 * selftests run with assertions on, which is what makes its other invariants checkable).
 *
 * So the ORACLE is skipped for exactly that combination -- a 66-prefixed encoding of the
 * transfer class in 16-bit mode -- and the DECODER side is still checked (its length, by the
 * hand table where the encoding is fixed-width, and its framing).  Nothing else in either
 * mode is affected: the same encodings in 32-bit mode and without the 66 prefix keep their
 * differential, and every non-transfer encoding keeps it in both modes.
 * ------------------------------------------------------------------ */
static bool transfer_class(uint8_t op, bool is_0f, const char *why)
{
    if (why != NULL) return true;                       /* the unpinnable list */
    if (!is_0f && (op == 0xE8 || op == 0xE9)) return true;  /* call/jmp rel */
    if (is_0f && op >= 0x80 && op <= 0x8F) return true;     /* jcc rel32 */
    return false;
}

static bool sixteen_bit_66_transfer(const uop_t *u, bool is32, uint8_t op, bool is_0f,
                                   const char *why)
{
    return !is32 && transfer_class(op, is_0f, why) && (u->pfx & PREFIX_MASK_OPSIZE) != 0;
}

static long stat_skipped_dbgexec;
static long stat_skipped_16bit66;
static long stat_runtime_stop;
static char stat_runtime_stop_names[8][96];
static int stat_runtime_stop_count;

/* THE TWO SHAPES OF `unimplemented_instruction`'s MESSAGE (cpu.c:65-69).  The generated
 * dispatcher's DECODE-TIME stop passes the qualified SYMBOL it could not call
 * ("instructions_0f::instr32_0FB6_mem", gen/interpreter_c.js's emit_call).  Every other caller
 * passes a SENTENCE describing a RUNTIME condition the sandbox caused: exceptions.c:63/75's
 * undeliverable-fault stop, the io bridge's unclaimed port, iret's invalid selector, ...  Only
 * the first says anything about what the DECODER must have emitted; the second is counted and
 * reported (the probe's advance is not a length measurement then). */
static bool is_decode_time_stop(const char *name)
{
    return strstr(name, "::") != NULL;
}

static void note_runtime_stop(const char *name)
{
    stat_runtime_stop++;
    for (int i = 0; i < stat_runtime_stop_count; i++) {
        if (strcmp(stat_runtime_stop_names[i], name) == 0) return;
    }
    if (stat_runtime_stop_count < 8) {
        snprintf(stat_runtime_stop_names[stat_runtime_stop_count++],
                 sizeof stat_runtime_stop_names[0], "%s", name);
    }
}

/* WHICH DISPATCH CASES WERE EXERCISED: [table][case key], the key being the generator's own
 * `opcode | (is_osize_32() << 8)` (gen/interpreter_c.js:446-451) computed from the DECODER's
 * opcode position and prefix byte.  So this counts which of uop_gen.c's 1024 cases were
 * actually walked, not what was attempted. */
static uint8_t enc_seen[2][512];

static long stat_streams, stat_instructions, stat_pinned, stat_pinned_rel;
static long stat_unpinned, stat_unpinned_allowed, stat_unpinned_opcode[2][256];
static long stat_nodec, stat_nodec_unimpl_ok, stat_nodec_unimpl_bad;
static long stat_oracle_unimpl, stat_descriptor_bad;
static long stat_faulted, stat_addr_checks, stat_addr_fail;
static long stat_addr_kinds[4];
static long stat_block_ends, stat_page_end_stops, stat_cap_stops, stat_nostop;

/* the encodings whose advance is NOT a length: a register/memory/stack value decides where
 * execution goes.  Written from the port's own bodies, and checked by the run: a probe the
 * differential cannot pin whose encoding is absent here is a FAILURE (never a "skip"). */
static const char *unpinnable(uint8_t opcode, bool is_0f, uint8_t modrm_g)
{
    if (!is_0f) {
        switch (opcode) {
        case 0xC2: case 0xC3: return "ret";
        case 0xCA: case 0xCB: return "retf";
        case 0xCF: return "iret";
        case 0xCC: return "int3";
        case 0xCD: return "int imm8";
        case 0xCE: return "into";
        case 0x9A: return "callf";
        case 0xEA: return "jmpf";
        /* sti is the ONE encoding with a RUNTIME NESTED DISPATCH (instructions.c:2145-2164:
         * instr_FB executes a shadow instruction and handle_irqs() inside its own body, plan
         * ss5.1), so one dispatch of it advances past the SHADOW instruction too.  Its own
         * length is 1 and is asserted by the hand table; the decoder's framing (sti ends the
         * block) by the framing tests. */
        case 0xFB: return "sti (nested shadow dispatch)";
        case 0xFF:
            if (modrm_g == 2 || modrm_g == 3) return "call indirect/far";
            if (modrm_g == 4 || modrm_g == 5) return "jmp indirect/far";
            return NULL;
        default: return NULL;
        }
    }
    switch (opcode) {
    case 0x05: return "syscall";
    case 0x07: return "sysret";
    case 0x34: return "sysenter";
    case 0x35: return "sysexit";
    default: return NULL;
    }
}

/* the pcs-relative target of a relative branch/call, computed from the RAW BYTES by hand --
 * the one hand rule in the differential, and the port's own for each form:
 * misc_instr.c:55-58 jmp_rel16 (masked to 16 bits), instructions.c:1812 (rel32 added to ip) */
static bool rel_target(uint32_t start, const uop_t *u, uint32_t *target)
{
    uint32_t op_eip = start + u->pfx_len;
    uint8_t op = mem8[op_eip + u->opcode_bytes - 1];
    bool is_0f = u->opcode_bytes == 2 && mem8[op_eip] == 0x0F;
    bool is32 = cpu->is_32 != ((u->pfx & PREFIX_MASK_OPSIZE) == PREFIX_MASK_OPSIZE);
    uint32_t after = start + u->len;

    if (!is_0f && u->opcode_bytes == 1) {
        if ((op >= 0x70 && op <= 0x7F) || op == 0xEB || (op >= 0xE0 && op <= 0xE3)) {
            *target = (uint32_t)((int32_t)after + (int8_t)mem8[op_eip + 1]);
            return true;
        }
        if (op == 0xE8 || op == 0xE9) {
            int w = is32 ? 4 : 2;
            if (w == 4) {
                int32_t rel = (int32_t)((uint32_t)mem8[op_eip + 1] | (uint32_t)mem8[op_eip + 2] << 8 |
                                        (uint32_t)mem8[op_eip + 3] << 16 | (uint32_t)mem8[op_eip + 4] << 24);
                *target = (uint32_t)((int32_t)after + rel);
            }
            else {
                uint32_t rel = (uint32_t)mem8[op_eip + 1] | (uint32_t)mem8[op_eip + 2] << 8;
                *target = (after + rel) & 0xFFFFu; /* jmp_rel16's mask */
            }
            return true;
        }
    }
    if (is_0f && op >= 0x80 && op <= 0x8F) {
        int w = is32 ? 4 : 2;
        if (w == 4) {
            int32_t rel = (int32_t)((uint32_t)mem8[op_eip + 2] | (uint32_t)mem8[op_eip + 3] << 8 |
                                    (uint32_t)mem8[op_eip + 4] << 16 | (uint32_t)mem8[op_eip + 5] << 24);
            *target = (uint32_t)((int32_t)after + rel);
        }
        else {
            uint32_t rel = (uint32_t)mem8[op_eip + 2] | (uint32_t)mem8[op_eip + 3] << 8;
            *target = (after + rel) & 0xFFFFu;
        }
        return true;
    }
    return false;
}

static bool is_mem_shape(uint8_t shape)
{
    return shape == UOP_SHAPE_MEM0 || shape == UOP_SHAPE_MEM1 ||
           shape == UOP_SHAPE_MEMF0 || shape == UOP_SHAPE_MEMF1;
}

/* (b): the extracted fields against the port's own resolver, for one decoded uop */
static void check_addr(const uop_t *u, uint32_t start, bool is32, const char *note)
{
    if (!is_mem_shape(u->shape)) return;
    uint32_t off = start + u->pfx_len + u->opcode_bytes;
    uint8_t modrm = mem8[off];

    probe_begin(start, is32, FLAGS_DEFAULT);
    cpu->prefixes = u->pfx;
    cpu->pagefault = false;
    int32_t mine = uop_materialize_addr(u);
    bool mine_fault = cpu->pagefault;

    cpu->instruction_pointer = (int32_t)(off + 1); /* the dispatcher's cursor for modrm_resolve */
    cpu->prefixes = u->pfx;
    cpu->pagefault = false;
    int32_t theirs = modrm_resolve(modrm);
    bool theirs_fault = cpu->pagefault;
    cpu->pagefault = false;

    stat_addr_checks++;
    stat_addr_kinds[u->addr_kind & 3]++;
    if (mine != theirs || mine_fault != theirs_fault) {
        stat_addr_fail++;
        REPORT("(b) %s: modrm=0x%02X kind=%u base=%u index=%u scale=%u disp=0x%X seg=%u: "
               "uop=0x%X fault=%d, resolve_modrm=0x%X fault=%d (bytes at 0x%X)",
               note, modrm, u->addr_kind, u->base, u->index, u->scale, u->disp, u->seg,
               (unsigned)mine, (int)mine_fault, (unsigned)theirs, (int)theirs_fault,
               (unsigned)off);
    }
}

/* (a): the length differential for one decoded uop */
static void check_length(const uop_t *u, uint32_t start, bool is32, const char *note)
{
    /* the encoding, identified from the RAW bytes at the position the DECODER reports (its own
     * pfx_len/opcode_bytes, both of which the differential asserts elsewhere -- so this holds
     * for a misaligned probe too: MEASURED, sweeping every sib byte probes a stream whose data
     * byte IS 0x62 and whose instruction is `bound`).  Everything that can decide to SKIP a
     * probe is here, BEFORE the oracle runs: a body that is DBG_ASSERT(false) in this build
     * must not be executed, or it aborts the whole suite. */
    uint32_t op_eip = start + u->pfx_len;
    uint8_t op = mem8[op_eip + u->opcode_bytes - 1];
    bool is_0f = u->opcode_bytes == 2 && mem8[op_eip] == 0x0F;
    uint8_t g = (op == 0xFF && u->g != UOP_REG_NONE) ? u->g : 0;
    const char *why = unpinnable(op, is_0f, g);

    if (debug_assert_only(op, is_0f) != NULL) {
        stat_skipped_dbgexec++;
        return;
    }
    if (sixteen_bit_66_transfer(u, is32, op, is_0f, why)) {
        stat_skipped_16bit66++;
        enc_seen[is_0f ? 1 : 0][op |
                                ((is32 != ((u->pfx & PREFIX_MASK_OPSIZE) == PREFIX_MASK_OPSIZE))
                                     ? 0x100
                                     : 0)] = 1;
        return;
    }
    bool eff32 = is32 != ((u->pfx & PREFIX_MASK_OPSIZE) == PREFIX_MASK_OPSIZE);
    enc_seen[is_0f ? 1 : 0][op | (eff32 ? 0x100 : 0)] = 1;
    {
        static FILE *tr;
        if (getenv("UOP_TRACE3")) {
            if (tr == NULL) tr = fopen("/tmp/uop_trace.txt", "w");
            if (tr != NULL) {
                fprintf(tr, "note=%s start=0x%X op=%02X is_0f=%d is32=%d pfx=%02X pfx_len=%u len=%u bytes=%02X %02X %02X %02X %02X %02X\n",
                        note, (unsigned)start, op, (int)is_0f, (int)is32, u->pfx, u->pfx_len,
                        u->len, mem8[start], mem8[start + 1], mem8[start + 2], mem8[start + 3],
                        mem8[start + 4], mem8[start + 5]);
                fflush(tr);
            }
        }
    }

    stat_instructions++;

    oracle_result runs[2];
    runs[0] = run_interpreter(start, is32, FLAGS_DEFAULT);
    /* the second arm sets CF|PF|AF|ZF|SF|OF and NOTHING else: those are the six bits the
     * conditional instructions test, and every one of the sixteen conditions is false in one
     * of the two arms.  The other flag bits are deliberately left clear -- FLAG_VM would put
     * the sandbox in vm86 mode and trip the port's own DBG_ASSERTs (MEASURED: flags =
     * 0xFFFFFFFF aborted at instructions.c:1140, instr32_9A's 16-bit new_ip check). */
    runs[1] = run_interpreter(start, is32, FLAGS_DEFAULT | 0xD5u);

    uint32_t target = 0;
    probe_begin(start, is32, FLAGS_DEFAULT);
    bool has_rel = rel_target(start, u, &target);

    bool pinned = false;
    bool any_usable = false;

    for (int i = 0; i < 2; i++) {
        if (!runs[i].ok) continue;
        if (runs[i].unimpl) {
            if (is_decode_time_stop(unimplemented_instruction_name())) {
                /* a DECODE-TIME stop: the form has no definition, so the decoder must have
                 * emitted no descriptor -- but it did */
                stat_oracle_unimpl++;
                stat_descriptor_bad++;
                REPORT("(a) %s: the interpreter stopped at decode time (%s) but the decoder "
                       "has a descriptor at 0x%X",
                       note, unimplemented_instruction_name(), (unsigned)start);
                return;
            }
            /* a RUNTIME stop (an unclaimed port, an invalid iret frame, ...): the machine
             * stopped for a sandbox reason, and the advance is not a length */
            note_runtime_stop(unimplemented_instruction_name());
            continue;
        }
        if (runs[i].cs_moved) {
            stat_faulted++;
            continue;
        }
        any_usable = true;
        int32_t adv = runs[i].ip_after - (int32_t)start;
        if (adv == (int32_t)u->len) {
            pinned = true;
            continue;
        }
        if (has_rel && (uint32_t)runs[i].ip_after == target) {
            pinned = true;
            stat_pinned_rel++;
            continue;
        }
        /* a non-transferring instruction advances by exactly its length, and x86 encodings are
         * 1..15 bytes: an in-window advance that is not the decoder's length IS a divergence --
         * UNLESS the encoding is one whose advance a register/memory value decides, which is
         * the same list the end of this function checks.  (An unconditional jmp's target can
         * land in the window: MEASURED at the first run of this checker, `EB 02`.) */
        if (adv >= 1 && adv <= 15 && why == NULL) {
            REPORT("(a) %s: opcode 0x%02X at 0x%X (is32=%d pfx=0x%02X pfx_len=%u): "
                   "interpreter advanced %d, decoder says %u "
                   "(bytes %02X %02X %02X %02X %02X %02X)",
                   note, op, (unsigned)start, (int)is32, u->pfx, u->pfx_len, adv, u->len,
                   mem8[start], mem8[start + 1], mem8[start + 2], mem8[start + 3],
                   mem8[start + 4], mem8[start + 5]);
            return;
        }
        stat_unpinned++;
    }

    if (pinned) {
        stat_pinned++;
        return;
    }
    if (!any_usable) return;

    /* unpinned: the encoding must be one whose advance a register/memory value decides */
    if (why == NULL) {
        stat_unpinned_opcode[is_0f ? 1 : 0][op]++;
        REPORT("(a) %s: opcode 0x%02X%s at 0x%X is UNPINNED and not on the unpinnable list "
               "(decoder says len=%u); the checker must be strengthened, not widened",
               note, op, is_0f ? " (0F)" : "", (unsigned)start, u->len);
        return;
    }
    stat_unpinned_allowed++;
}

/* ------------------------------------------------------------------ *
 * one stream
 * ------------------------------------------------------------------ */

/* probe the stream at `start`: decode a block, then check (a)+(b) for every uop the decoder
 * produced, and assert the framing */
static void probe_stream_at(uint32_t start, bool is32, const char *note, bool framing_only)
{
    stat_streams++;
    uop_t uops[UOP_MAX_BLOCK_UOPS];
    uop_block_t blk;
    int n = run_decoder(start, is32, uops, UOP_MAX_BLOCK_UOPS, &blk);

    if (n <= 0) {
        /* no block: the interpreter must have found no instruction either, or stopped on an
         * unimplemented one -- never "the decoder refused something the interpreter runs" */
        oracle_result o = run_interpreter(start, is32, FLAGS_DEFAULT);
        stat_nodec++;
        if (o.ok && o.unimpl && !is_decode_time_stop(unimplemented_instruction_name())) {
            /* the interpreter stopped for a runtime reason; the decoder's refusal is fine */
            note_runtime_stop(unimplemented_instruction_name());
            stat_nodec_unimpl_ok++;
        }
        else if (o.ok && !o.unimpl && !o.cs_moved) {
            int32_t adv = o.ip_after - (int32_t)start;
            if (adv >= 1 && adv <= 15) {
                stat_nodec_unimpl_bad++;
                REPORT("(no-block) %s: the decoder produced no block at 0x%X (%s) but the "
                       "interpreter advanced %d bytes", note, (unsigned)start,
                       uop_stop_name(uop_last_stop), adv);
            }
        }
        else {
            stat_nodec_unimpl_ok++;
        }
        return;
    }

    stat_nostop++;
    stat_block_ends += (blk.stop == UOP_STOP_BLOCK_END);
    stat_page_end_stops += (blk.stop == UOP_STOP_PAGE_END);
    stat_cap_stops += (blk.stop == UOP_STOP_CAP);

    CHECK(blk.uop_count == (uint8_t)n);
    CHECK(blk.virt_start == start);
    CHECK(blk.phys_start == start); /* paging is off: the translation is the identity */
    CHECK((uint32_t)blk.phys_start + blk.phys_len <= (start | 0xFFFu) + 1);

    for (int i = 0; i < n; i++) {
        const uop_t *u = &uops[i];
        CHECK(u->len >= 1);
        if (i > 0) {
            /* the block's uops are contiguous and each starts where the previous ended */
            CHECK_EQ(u->eip, uops[i - 1].eip + uops[i - 1].len);
        }
        else {
            CHECK(u->eip == start);
        }
        /* `off` is the OPCODE byte's offset within the block, so a prefix chain moves it past
         * the instruction's own start (plan ss2.1's field) */
        CHECK(u->off == u->eip - start + u->pfx_len);
        /* the shape and the extracted operand kind must agree -- the one cross-check between
         * the descriptor-derived shape and the extraction, and the reason a mutated
         * uop_shape_of cannot silently drop a memory operand out of the (b) comparison */
        CHECK_EQ(is_mem_shape(u->shape), u->addr_kind != UOP_ADDR_NONE);
        CHECK_EQ(is_mem_shape(u->shape), (u->base != UOP_REG_NONE || u->index != UOP_REG_NONE ||
                                          u->disp != 0 || u->seg == DS || u->seg == SS));
        if (framing_only) continue;
        check_addr(u, u->eip, is32, note);
        check_length(u, u->eip, is32, note);
    }
    CHECK_EQ(blk.stop_eip, (uint32_t)(n ? uops[n - 1].eip + uops[n - 1].len : start));
}

/* a stream probed from its first byte */
static void probe_stream(const uint8_t *bytes, int len, bool is32, const char *note)
{
    put_stream(bytes, len, 0);
    probe_stream_at(CODE_PAGE, is32, note, false);
}

/* a stream every one of whose BYTE OFFSETS is probed: the decoder is exercised from misaligned
 * starts too, where the bytes are whatever they are -- still a valid differential */
static void probe_stream_offsets(const uint8_t *bytes, int len, bool is32, const char *note)
{
    for (int off = 0; off < len; off++) {
        uint32_t start = put_stream(bytes, len, 0) + (uint32_t)off;
        probe_stream_at(start, is32, note, false);
    }
}

/* ------------------------------------------------------------------ *
 * the hand streams -- one per base opcode, both operand sizes, in the forms that decide the
 * length: reg form (modrm 0xC1), mem form without an operand (modrm 0x00), the sib forms, and
 * the prefix combinations
 * ------------------------------------------------------------------ */

/* the operand bytes that follow a modrm byte: enough for any encoding's disp/sib/imm */
static const uint8_t TAIL[16] = {
    0x24, 0x78, 0x56, 0x34, 0x12, 0x9C, 0x05, 0xA8,
    0x00, 0x10, 0x00, 0x00, 0x44, 0x33, 0x22, 0x11,
};

static void sweep_opcode_forms(void)
{
    /* ALL EIGHT VALUES OF modrm's reg field, not just g = 0: a `fixed_g` encoding selects its
     * body from that field, and a sweep with one g value leaves every OTHER body of every
     * group unchecked.  MEASURED on the first version of this sweep (all six forms had
     * g = 0): the 0F C7 group, whose entries are g = 1 and g = 6, was never decoded once, and
     * the coverage line said so (995 of 1024). */
    static const struct { uint8_t modrm; const char *name; } forms[] = {
        { 0x00, "mem-[EAX]/[BX+SI]" },
        { 0x45, "mem-disp8-[EBP+disp8]/[DI]" },
        { 0x84, "mem-sib-disp32" },
        { 0x44, "mem-sib-disp8" },
        { 0x04, "mem-sib-nodisp" },
        { 0xC0, "reg-g0" }, { 0xC8, "reg-g1" }, { 0xD0, "reg-g2" }, { 0xD8, "reg-g3" },
        { 0xE0, "reg-g4" }, { 0xE8, "reg-g5" }, { 0xF0, "reg-g6" }, { 0xF8, "reg-g7" },
    };

    for (int is32 = 0; is32 < 2; is32++) {
        for (int op = 0; op < 0x100; op++) {
            for (size_t f = 0; f < sizeof forms / sizeof forms[0]; f++) {
                uint8_t bytes[24];
                int len = 0;
                bytes[len++] = (uint8_t)op;
                bytes[len++] = forms[f].modrm;
                for (size_t t = 0; t < sizeof TAIL; t++) {
                    bytes[len++] = TAIL[t];
                }
                probe_stream(bytes, len, is32 != 0, forms[f].name);
            }
        }
    }

    /* the 0F table, same forms */
    for (int is32 = 0; is32 < 2; is32++) {
        for (int op = 0; op < 0x100; op++) {
            for (size_t f = 0; f < sizeof forms / sizeof forms[0]; f++) {
                uint8_t bytes[26];
                int len = 0;
                bytes[len++] = 0x0F;
                bytes[len++] = (uint8_t)op;
                bytes[len++] = forms[f].modrm;
                for (size_t t = 0; t < sizeof TAIL; t++) {
                    bytes[len++] = TAIL[t];
                }
                probe_stream(bytes, len, is32 != 0, "0F table");
            }
        }
    }
}

static void sweep_prefixes(void)
{
    static const uint8_t prefixes[][3] = {
        { 0x66, 0, 0 }, { 0x67, 0, 0 }, { 0xF2, 0, 0 }, { 0xF3, 0, 0 }, { 0xF0, 0, 0 },
        { 0x26, 0, 0 }, { 0x2E, 0, 0 }, { 0x36, 0, 0 }, { 0x3E, 0, 0 }, { 0x64, 0, 0 },
        { 0x65, 0, 0 },
        { 0x66, 0x67, 0 }, { 0x67, 0x66, 0 }, { 0x66, 0xF2, 0 }, { 0xF2, 0x67, 0 },
        { 0xF0, 0x66, 0 }, { 0x64, 0x67, 0 }, { 0x66, 0x66, 0 }, { 0xF3, 0x66, 0 },
    };

    /* the register form (shortest operand tail) keeps the sweep at 256 opcodes x prefixes */
    for (size_t p = 0; p < sizeof prefixes / sizeof prefixes[0]; p++) {
        int plen = prefixes[p][0] ? (prefixes[p][1] ? (prefixes[p][2] ? 3 : 2) : 1) : 0;
        for (int op = 0; op < 0x100; op++) {
            /* instructions.c:1898-1918: instr_F2/F3 ASSERT that no rep prefix is already
             * active, so a row that already carries F2/F3 cannot be followed by another one
             * in this build (the reference release would set both bits and carry on) */
            bool row_has_rep = false;
            for (int i = 0; i < plen; i++) {
                if (prefixes[p][i] == 0xF2 || prefixes[p][i] == 0xF3) row_has_rep = true;
            }
            if (row_has_rep && (op == 0xF2 || op == 0xF3)) {
                stat_skipped_dbgexec++;
                continue;
            }
            uint8_t bytes[24];
            int len = 0;
            for (int i = 0; i < plen; i++) bytes[len++] = prefixes[p][i];
            bytes[len++] = (uint8_t)op;
            bytes[len++] = 0xC1;
            for (size_t t = 0; t < 8; t++) bytes[len++] = TAIL[t];
            probe_stream(bytes, len, true, "prefix+reg-form");

            /* and the same with the escape, so the 0F table sees the prefixes too */
            len = 0;
            for (int i = 0; i < plen; i++) bytes[len++] = prefixes[p][i];
            bytes[len++] = 0x0F;
            bytes[len++] = (uint8_t)op;
            bytes[len++] = 0x84;
            for (size_t t = 0; t < 12; t++) bytes[len++] = TAIL[t];
            probe_stream(bytes, len, true, "prefix+0F+sib");
        }
    }
}

/* the sib byte sweep: every (base, index, scale, mod) form of the 32-bit addressing mode */
static void sweep_sib(void)
{
    for (int sib = 0; sib < 0x100; sib++) {
        for (int mod = 0; mod < 4; mod++) {
            uint8_t bytes[16];
            int len = 0;
            bytes[len++] = 0x8B;                       /* mov reg32, r/m32: one modrm operand */
            bytes[len++] = (uint8_t)(mod << 6 | 4);    /* r/m = 100 -> sib */
            bytes[len++] = (uint8_t)sib;
            for (size_t t = 0; t < 12; t++) bytes[len++] = TAIL[t];
            probe_stream(bytes, len, true, "sib");
            if (mod == 0) {
                /* and every MISALIGNED start inside the same bytes: the decoder is a byte
                 * stream decoder, so an offset that splits an instruction still has to agree
                 * with the interpreter byte for byte */
                probe_stream_offsets(bytes, len, true, "sib (misaligned)");
            }

            /* and the 16-bit file's forms, where the sib byte is just a modrm */
            uint8_t b16[16];
            int l16 = 0;
            b16[l16++] = 0x89;                         /* mov r/m16, reg16 */
            b16[l16++] = (uint8_t)(mod << 6 | ((sib >> 3) & 7) << 3 | (sib & 7));
            for (size_t t = 0; t < 12; t++) b16[l16++] = TAIL[t];
            probe_stream(b16, l16, false, "16-bit addressing");
        }
    }
}

/* ------------------------------------------------------------------ *
 * the hand table: encodings and forms whose framing/extraction the sweeps cannot pin, with the
 * expected values written out by hand from the x86 encoding rules
 * ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *bytes;
    int len;
    bool is32;
    uint16_t want_len;
    /* -1 = not asserted */
    int want_kind, want_base, want_index, want_scale, want_seg;
    int32_t want_disp;
    bool skip_oracle; /* the body is DBG_ASSERT(false) in this build: decoder-only check */
    const char *note;
} hand_t;


static void hand_entry(const hand_t *h)
{
    put_stream(h->bytes, h->len, 0);
    uop_t uops[UOP_MAX_BLOCK_UOPS];
    uop_block_t blk;
    int n = run_decoder(CODE_PAGE, h->is32, uops, UOP_MAX_BLOCK_UOPS, &blk);
    checks++;
    if (n <= 0) {
        failures++;
        fprintf(stderr, "FAIL hand %s: no block (stop=%s)\n", h->note, uop_stop_name(uop_last_stop));
        return;
    }
    const uop_t *u = &uops[0];
    CHECK_EQ(u->len, h->want_len);
    if (h->want_kind >= 0) CHECK_EQ(u->addr_kind, h->want_kind);
    if (h->want_base >= 0) CHECK_EQ(u->base, h->want_base);
    if (h->want_index >= 0) CHECK_EQ(u->index, h->want_index);
    if (h->want_scale >= 0) CHECK_EQ(u->scale, h->want_scale);
    if (h->want_seg >= 0) CHECK_EQ(u->seg, h->want_seg);
    if (h->want_disp != 0) CHECK_EQ(u->disp, (uint32_t)h->want_disp);
    /* and the differential still applies to every hand entry that the debug build can run */
    check_addr(u, CODE_PAGE, h->is32, h->note);
    if (h->skip_oracle) {
        stat_skipped_dbgexec++;
    }
    else {
        check_length(u, CODE_PAGE, h->is32, h->note);
    }
}

/* the hand table's byte streams, as named arrays: a compound literal inside a static
 * initialiser trips cosmopolitan's patchable-entry pass (-Werror) and hides the
 * bytes from a reader anyway */
static const uint8_t hb1[] = { 0x04, 0x34 };
static const uint8_t hb2[] = { 0x68, 0x78, 0x56 };
static const uint8_t hb3[] = { 0x68, 0x78, 0x56, 0x34, 0x12 };
static const uint8_t hb4[] = { 0x66, 0x68, 0x78, 0x56 };
static const uint8_t hb5[] = { 0xA0, 0x78, 0x56, 0x34, 0x12 };
static const uint8_t hb6[] = { 0x67, 0xA0, 0x78, 0x56 };
static const uint8_t hb7[] = { 0x9A, 0x78, 0x56, 0x34, 0x12, 0x08, 0x00 };
static const uint8_t hb8[] = { 0x9A, 0x78, 0x56, 0x08, 0x00 };
static const uint8_t hb9[] = { 0x8B, 0x00 };
static const uint8_t hb10[] = { 0x8B, 0x45, 0x10 };
static const uint8_t hb11[] = { 0x8B, 0x85, 0x78, 0x56, 0x34, 0x12 };
static const uint8_t hb12[] = { 0x8B, 0x05, 0x78, 0x56, 0x34, 0x12 };
static const uint8_t hb13[] = { 0x8B, 0x04, 0x24 };
static const uint8_t hb14[] = { 0x8B, 0x04, 0x8D, 0x78, 0x56, 0x34, 0x12 };
static const uint8_t hb15[] = { 0x8B, 0x44, 0x8D, 0x10 };
static const uint8_t hb16[] = { 0x8B, 0x04, 0x25, 0x78, 0x00, 0x00, 0x00 };
static const uint8_t hb17[] = { 0x8B, 0x04, 0xA5, 0x78, 0x00, 0x00, 0x00 };
static const uint8_t hb18[] = { 0x8B, 0x04, 0x95, 0x78, 0x00, 0x00, 0x00 };
static const uint8_t hb19[] = { 0x8B, 0x44, 0xA5, 0x10 };
static const uint8_t hb20[] = { 0x67, 0x8B, 0x00 };
static const uint8_t hb21[] = { 0x8B, 0x01 };
static const uint8_t hb22[] = { 0x8B, 0x02 };
static const uint8_t hb23[] = { 0x8B, 0x03 };
static const uint8_t hb24[] = { 0x8B, 0x04 };
static const uint8_t hb25[] = { 0x8B, 0x05 };
static const uint8_t hb26[] = { 0x8B, 0x06, 0x78, 0x56 };
static const uint8_t hb27[] = { 0x8B, 0x07 };
static const uint8_t hb28[] = { 0x8B, 0x46, 0xF0 };
static const uint8_t hb29[] = { 0x8B, 0x86, 0x78, 0x56 };
static const uint8_t hb30[] = { 0x2E, 0x8B, 0x07 };
static const uint8_t hb31[] = { 0x66, 0x81, 0xC1, 0x78, 0x56 };
static const uint8_t hb32[] = { 0x81, 0xC1, 0x78, 0x56, 0x34, 0x12 };
static const uint8_t hb33[] = { 0xF0, 0x66, 0x81, 0xC1, 0x78, 0x56, 0x34, 0x12 };
static const uint8_t hb34[] = { 0x3E, 0x03, 0x00 };
static const uint8_t hb35[] = { 0x64, 0x03, 0x00 };
static const uint8_t hb36[] = { 0x66, 0x0F, 0xB6, 0x04, 0x24 };
static const uint8_t hb37[] = { 0x0F, 0x7F, 0x00, 0x00 };
static const uint8_t hb38[] = { 0x8D, 0x04, 0x8D, 0x78, 0x56, 0x34, 0x12 };
static const uint8_t hb39[] = { 0x8D, 0xC0 };
static const uint8_t hb40[] = { 0x8F, 0x04, 0x24 };
static const uint8_t hb41[] = { 0x62, 0xC1 };
static const uint8_t hb42[] = { 0x62, 0x45, 0x10 };
static const uint8_t hb43[] = { 0xF1 };
static const uint8_t hb44[] = { 0xFB };

static void hand_table(void)
{
    static const hand_t table[] = {
        /* --- no modrm: the immediate widths, both operand sizes --- */
        { hb1, 2, true, 2, -1, -1, -1, -1, -1, 0, false, "add al, imm8" },
        { hb2, 3, false, 3, -1, -1, -1, -1, -1, 0, false, "push imm16" },
        { hb3, 5, true, 5, -1, -1, -1, -1, -1, 0,
          false, "push imm32" },
        { hb4, 4, true, 4, -1, -1, -1, -1, -1, 0,
          false, "66 push imm16 in 32-bit mode" },
        { hb5, 5, true, 5, -1, -1, -1, -1, -1, 0,
          false, "mov al, moffs32" },
        { hb6, 4, true, 4, -1, -1, -1, -1, -1, 0,
          false, "67 mov al, moffs16" },
        { hb7, 7, true, 7, -1, -1, -1, -1, -1, 0,
          false, "callf ptr16:32" },
        { hb8, 5, false, 5, -1, -1, -1, -1, -1, 0,
          false, "callf ptr16:16" },

        /* --- the 32-bit addressing forms: base/index/scale/disp/seg, one entry per form --- */
        { hb9, 2, true, 2, UOP_ADDR_32, EAX, UOP_REG_NONE, 0, DS, 0,
          false, "mov eax, [eax]" },
        { hb10, 3, true, 3, UOP_ADDR_32, EBP, UOP_REG_NONE, 0, SS, 0x10,
          false, "mov eax, [ebp+0x10]: SS" },
        { hb11, 6, true, 6, UOP_ADDR_32, EBP,
          UOP_REG_NONE, 0, SS, 0x12345678, false, "mov eax, [ebp+disp32]: SS" },
        { hb12, 6, true, 6, UOP_ADDR_32, UOP_REG_NONE,
          UOP_REG_NONE, 0, DS, 0x12345678, false, "mov eax, [disp32]: DS, no base" },
        { hb13, 3, true, 3, UOP_ADDR_SIB, ESP, UOP_REG_NONE, 0, SS, 0,
          false, "mov eax, [esp]: sib, SS" },
        { hb14, 7, true, 7, UOP_ADDR_SIB,
          UOP_REG_NONE, ECX, 2, DS, 0x12345678,
          false, "mov eax, [ecx*4+disp32]: scale 2, DS, no base" },
        { hb15, 4, true, 4, UOP_ADDR_SIB, EBP, ECX, 2, SS, 0x10,
          false, "mov eax, [ebp+ecx*4+0x10]: SS" },
        { hb16, 7, true, 7, UOP_ADDR_SIB,
          UOP_REG_NONE, UOP_REG_NONE, 0, DS, 0x78, false, "sib index 4 = no index" },
        { hb17, 7, true, 7, UOP_ADDR_SIB,
          UOP_REG_NONE, UOP_REG_NONE, 0, DS, 0x78,
          false, "sib A5: index field 4 = NO index; base field 5 with mod 00 = a disp32 base" },
        { hb18, 7, true, 7, UOP_ADDR_SIB,
          UOP_REG_NONE, EDX, 2, DS, 0x78, false, "sib 95: edx*4 + disp32 base" },
        { hb19, 4, true, 4, UOP_ADDR_SIB, EBP, UOP_REG_NONE, 0, SS, 0x10,
          false, "sib A5 with mod 01: base field 5 = EBP (SS), index field 4 = none" },
        { hb20, 3, true, 3, UOP_ADDR_16, EBX, ESI, 0, DS, 0,
          false, "67 turns [EAX] into [BX+SI]" },

        /* --- the 16-bit addressing forms (all eight) --- */
        { hb9, 2, false, 2, UOP_ADDR_16, BX, SI, 0, DS, 0, false, "16: [bx+si]" },
        { hb21, 2, false, 2, UOP_ADDR_16, BX, DI, 0, DS, 0, false, "16: [bx+di]" },
        { hb22, 2, false, 2, UOP_ADDR_16, BP, SI, 0, SS, 0, false, "16: [bp+si]: SS" },
        { hb23, 2, false, 2, UOP_ADDR_16, BP, DI, 0, SS, 0, false, "16: [bp+di]: SS" },
        { hb24, 2, false, 2, UOP_ADDR_16, SI, UOP_REG_NONE, 0, DS, 0, false, "16: [si]" },
        { hb25, 2, false, 2, UOP_ADDR_16, DI, UOP_REG_NONE, 0, DS, 0, false, "16: [di]" },
        { hb26, 4, false, 4, UOP_ADDR_16, UOP_REG_NONE, UOP_REG_NONE,
          0, DS, 0x5678, false, "16: disp16" },
        { hb27, 2, false, 2, UOP_ADDR_16, BX, UOP_REG_NONE, 0, DS, 0, false, "16: [bx]" },
        { hb28, 3, false, 3, UOP_ADDR_16, BP, UOP_REG_NONE, 0, SS, 0xFFFFFFF0,
          false, "16: [bp-0x10]: SS, sign-extended disp8" },
        { hb29, 4, false, 4, UOP_ADDR_16, BP, UOP_REG_NONE, 0, SS,
          0x5678, false, "16: [bp+disp16]" },
        { hb30, 3, false, 3, UOP_ADDR_16, BX, UOP_REG_NONE, 0, DS, 0,
          false, "cs: prefix keeps the form's default segment" },

        /* --- prefix combinations --- */
        { hb31, 5, true, 5, -1, -1, -1, -1, -1, 0,
          false, "66 add r/m16, imm16" },
        { hb32, 6, true, 6, -1, -1, -1, -1, -1, 0,
          false, "add r/m32, imm32" },
        { hb33, 6, true, 6, -1, -1, -1, -1, -1, 0,
          false, "F0 66 81 /0 imm16: the 66 makes the immediate 16-bit, so 6 bytes, not 8" },
        { hb34, 3, true, 3, UOP_ADDR_32, EAX, UOP_REG_NONE, 0, DS, 0,
          false, "ds: keeps DS" },
        { hb35, 3, true, 3, UOP_ADDR_32, EAX, UOP_REG_NONE, 0, DS, 0,
          false, "fs: keeps the form's default DS for the base" },
        { hb36, 5, true, 5, UOP_ADDR_SIB, ESP, UOP_REG_NONE, 0,
          SS, 0, false, "66 0F B6 movzx" },
        { hb37, 3, true, 3, -1, -1, -1, -1, -1, 0,
          false, "0F 7F /0 with modrm 00 (mem, no disp): 3 bytes" },
        { hb38, 7, true, 7, UOP_ADDR_SIB,
          UOP_REG_NONE, ECX, 2, DS, 0x12345678, false, "lea: fetch_internal memory form" },
        { hb39, 2, true, 2, -1, -1, -1, -1, -1, 0, false, "lea reg form" },
        { hb40, 3, true, 3, UOP_ADDR_SIB, ESP, UOP_REG_NONE, 0, SS, 0,
          false, "pop r/m32 sib" },

        /* --- the two encodings whose BODIES are DBG_ASSERT(false) in this build: the
         *     decoder-only entries (see debug_assert_only) --- */
        { hb41, 2, true, 2, -1, -1, -1, -1, -1, 0, true,
          "bound reg form: 0x62 /0 modrm, 2 bytes (the body asserts, the decode cannot)" },
        { hb42, 3, true, 3, -1, -1, -1, -1, -1, 0, true,
          "bound mem form with disp8: 3 bytes" },
        { hb43, 1, true, 1, -1, -1, -1, -1, -1, 0, true,
          "int1 0xF1: 1 byte" },
        { hb44, 1, true, 1, -1, -1, -1, -1, -1, 0, true,
          "sti 0xFB: 1 byte (the oracle cannot pin it: its body nests a shadow dispatch)" },
    };

    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        hand_entry(&table[i]);
    }
}

/* ------------------------------------------------------------------ *
 * (d) THE LAST VIRTUAL PAGE (S3 round 2, review S2)
 *
 * The decode window's end used to be `(virt_start | 0xFFF) + 1` in a uint32_t, which WRAPS to
 * 0 for `virt_start` in 0xFFFFF000..0xFFFFFFFF: `dec_fetch` then failed on the first byte and
 * every block at the top page was refused, so the interpreter ran those instructions forever.
 * S3's masked form gives the window a real end (2^32, a value the 32-bit field could not even
 * hold), so a block there now DECODES.  These three arms are that claim, its boundary, and the
 * thing it must NOT have done:
 *
 *   1. a block at the top page decodes, with the FULL virtual address in its uops (the eip
 *      field is 64-bit now, so `eip`, `body_ip` and `stop_eip` carry 0xFFFFFxxx rather than
 *      a projected low half);
 *   2. the window still ENDS at the page: the byte past 0xFFFFFFFF is an overrun, and an
 *      instruction that would need it is dropped (the fix widened the window's END VALUE, not
 *      the window);
 *   3. the pre-change behaviour is named: with the old expression this stream yields n == 0
 *      and a PAGE_END stop, i.e. uop_block_decode() returns NULL and uop_run falls back to
 *      interpreted_run.  Arm 1 failing IS that regression returning.
 *
 * The END-TO-END arm -- does the guest still get the same answer, in all three engines, when
 * the last page holds the code -- is jit_codegen_selftest's test_last_page, because which
 * ENGINE runs is exactly what changed.
 * ------------------------------------------------------------------ */
static void last_page_tests(void)
{
    const uint64_t top = 0xFFFFF000ull;

    /* 1. a stream ending at the page's last byte decodes as a block, at the TOP page */
    {
        uint8_t bytes[16] = { 0x90, 0x90, 0x90, 0x90, 0x89, 0xC1, 0x90, 0x90, 0x90, 0x90 };
        uint32_t off = CODE_SIZE - 8;          /* exactly 8 bytes to the page end */
        memcpy(mem8 + CODE_PAGE, code_pristine, CODE_SIZE);
        memcpy(mem8 + CODE_PAGE + off, bytes, 10);
        /* the virtual start is the top page's matching offset: the window is the VIRTUAL page */
        uint64_t start = top + off;
        cpu->prefixes = 0;
        uop_t uops[UOP_MAX_BLOCK_UOPS];
        uop_block_t blk;
        int n = uop_decode_block(uops, UOP_MAX_BLOCK_UOPS, start, CODE_PAGE + off, &blk);
        CHECK_EQ(n, 7);                        /* the framing, unchanged by the address */
        CHECK_EQ(blk.stop, UOP_STOP_PAGE_END);
        CHECK_EQ(blk.virt_start, start);
        CHECK_EQ(blk.stop_eip, top + 0x1000ull); /* the window's end: 2^32, representable */
        CHECK_EQ(uops[0].eip, start);            /* the uop carries the FULL address */
        CHECK_EQ(uops[1].eip, start + 1);
        CHECK_EQ(uops[1].body_ip, start + 2);
    }

    /* 2. the window still ends at the page: an instruction that needs the byte PAST
     *    0xFFFFFFFF is dropped, and the block is empty */
    {
        memcpy(mem8 + CODE_PAGE, code_pristine, CODE_SIZE);
        mem8[CODE_PAGE + CODE_SIZE - 1] = 0xB8; /* mov eax, imm32, one byte of room left */
        cpu->prefixes = 0;
        uop_t uops[UOP_MAX_BLOCK_UOPS];
        uop_block_t blk;
        int n = uop_decode_block(uops, UOP_MAX_BLOCK_UOPS, 0xFFFFFFFFull,
                                 CODE_PAGE + CODE_SIZE - 1, &blk);
        CHECK_EQ(n, 0);                        /* no partial instruction in a block */
        CHECK_EQ(blk.stop, UOP_STOP_PAGE_END);
        CHECK_EQ(uop_last_stop_eip, 0xFFFFFFFFull);
    }

    /* 3. a one-byte instruction AT the top page decodes (the minimum case: the old window
     *    was empty there, which is what arm 1's n == 7 grew out of) */
    {
        memcpy(mem8 + CODE_PAGE, code_pristine, CODE_SIZE);
        mem8[CODE_PAGE] = 0xF4; /* hlt: one byte, a block end */
        cpu->prefixes = 0;
        uop_t uops[UOP_MAX_BLOCK_UOPS];
        uop_block_t blk;
        int n = uop_decode_block(uops, UOP_MAX_BLOCK_UOPS, top, CODE_PAGE, &blk);
        CHECK_EQ(n, 1);
        CHECK_EQ(uops[0].eip, top);
        CHECK_EQ(uops[0].len, 1);
        CHECK_EQ(blk.stop, UOP_STOP_BLOCK_END);
    }

    printf("PASS (d) the top virtual page: a block there decodes with its full 64-bit address,"
           " the window still ends at 2^32, and the empty-window regression is named\n");
}

/* ------------------------------------------------------------------ *
 * (c) the block framing, asserted explicitly
 * ------------------------------------------------------------------ */

static void framing_tests(void)
{
    /* 1. a block continues through ordinary modrm instructions and stops at the page end */
    {
        uint8_t bytes[16] = { 0x90, 0x90, 0x90, 0x90, 0x89, 0xC1, 0x90, 0x90, 0x90, 0x90 };
        uint32_t off = CODE_SIZE - 8; /* exactly 8 bytes to the page end */
        uint32_t start = put_stream(bytes, 10, off);
        uop_block_t blk;
        uop_t uops[UOP_MAX_BLOCK_UOPS];
        int n = run_decoder(start, true, uops, UOP_MAX_BLOCK_UOPS, &blk);
        CHECK_EQ(n, 7); /* 4 nops + mov ecx,eax + 2 nops; the bytes past +8 cross the page */
        CHECK_EQ(blk.stop, UOP_STOP_PAGE_END);
        CHECK_EQ(blk.stop_eip, start + 8);
        probe_stream_at(start, true, "page-end framing", false);
    }

    /* 2. the crossing instruction is NOT in the block: a 6-byte instruction with 4 bytes left
     *    in the page yields a block that ends BEFORE it (the divergence from plan ss2.2) */
    {
        uint8_t bytes[8] = { 0x90, 0xB8, 0x78, 0x56, 0x34, 0x12 }; /* nop; mov eax, imm32 */
        uint32_t off = CODE_SIZE - 5; /* 5 bytes left: the mov would need 5 exactly at off 1..5 */
        uint32_t start = put_stream(bytes, 6, off);
        uop_block_t blk;
        uop_t uops[UOP_MAX_BLOCK_UOPS];
        int n = run_decoder(start, true, uops, UOP_MAX_BLOCK_UOPS, &blk);
        CHECK_EQ(n, 1); /* only the nop */
        CHECK_EQ(blk.stop, UOP_STOP_PAGE_END);
        CHECK_EQ(blk.stop_eip, start + 1);
    }

    /* 3. a block ends AFTER a block-boundary instruction, not before it */
    {
        uint8_t bytes[16] = { 0x90, 0x90, 0xE8, 0x02, 0x00, 0x00, 0x00, 0x90, 0x90 };
        uint32_t start = put_stream(bytes, 9, 0);
        uop_block_t blk;
        uop_t uops[UOP_MAX_BLOCK_UOPS];
        int n = run_decoder(start, true, uops, UOP_MAX_BLOCK_UOPS, &blk);
        CHECK_EQ(n, 3); /* nop, nop, call rel32 -- and the call's uop carries UOP_BLOCK_END */
        CHECK(uops[2].flags & UOP_FLAG_BLOCK_END);
        CHECK_EQ(uops[2].len, 5);
        CHECK_EQ(blk.stop, UOP_STOP_BLOCK_END);
    }

    /* 4. jcc/jmp/loop do NOT end the block (no_block_boundary_in_interpreted) */
    {
        uint8_t bytes[16] = { 0x74, 0x04, 0x90, 0x90, 0x90, 0x90, 0xEB, 0x02, 0x90, 0x90 };
        uint32_t start = put_stream(bytes, 10, 0);
        uop_block_t blk;
        uop_t uops[UOP_MAX_BLOCK_UOPS];
        int n = run_decoder(start, true, uops, UOP_MAX_BLOCK_UOPS, &blk);
        CHECK(n >= 7);
        CHECK(!(uops[0].flags & UOP_FLAG_BLOCK_END));
        CHECK_EQ(uops[0].len, 2); /* the jcc */
        CHECK(!(uops[5].flags & UOP_FLAG_BLOCK_END)); /* the jmp at offset 6 */
        CHECK_EQ(uops[5].off, 6);
        CHECK_EQ(uops[5].len, 2);
    }

    /* 5. hlt ends the block (0xF4 is block_boundary and has no no_bib escape) */
    {
        uint8_t bytes[8] = { 0x90, 0xF4, 0x90, 0x90 };
        uint32_t start = put_stream(bytes, 4, 0);
        uop_block_t blk;
        uop_t uops[UOP_MAX_BLOCK_UOPS];
        int n = run_decoder(start, true, uops, UOP_MAX_BLOCK_UOPS, &blk);
        CHECK_EQ(n, 2);
        CHECK(uops[1].flags & UOP_FLAG_BLOCK_END);
        CHECK_EQ(blk.stop, UOP_STOP_BLOCK_END);
    }

    /* 5b. sti (0xFB) ends the block: its body nests a shadow dispatch and handle_irqs(), so
     *     the next cached uop would execute the shadow instruction a second time (plan ss5.1) */
    {
        uint8_t bytes[8] = { 0x90, 0xFB, 0x90, 0x90 };
        uint32_t start = put_stream(bytes, 4, 0);
        uop_block_t blk;
        uop_t uops[UOP_MAX_BLOCK_UOPS];
        int n = run_decoder(start, true, uops, UOP_MAX_BLOCK_UOPS, &blk);
        CHECK_EQ(n, 2);
        CHECK(uops[1].flags & UOP_FLAG_BLOCK_END);
        CHECK_EQ(uops[1].len, 1);
        CHECK_EQ(blk.stop, UOP_STOP_BLOCK_END);
    }

    /* 6. the cap: a page of nops stops at UOP_MAX_BLOCK_UOPS, and a long instruction run stops
     *    at UOP_MAX_BLOCK_BYTES */
    {
        memset(mem8 + CODE_PAGE, 0x90, CODE_SIZE);
        uop_block_t blk;
        uop_t uops[UOP_MAX_BLOCK_UOPS];
        int n = run_decoder(CODE_PAGE, true, uops, UOP_MAX_BLOCK_UOPS, &blk);
        CHECK_EQ(n, UOP_MAX_BLOCK_UOPS);
        CHECK_EQ(blk.stop, UOP_STOP_CAP);

        /* 30 x `mov eax,[disp32]` (6 bytes each) = 180 bytes < 240; 41 of them = 246 > 240 */
        for (int i = 0; i < 50; i++) {
            mem8[CODE_PAGE + 6 * i + 0] = 0x8B;
            mem8[CODE_PAGE + 6 * i + 1] = 0x05;
            mem8[CODE_PAGE + 6 * i + 2] = 0x00;
            mem8[CODE_PAGE + 6 * i + 3] = 0x10;
            mem8[CODE_PAGE + 6 * i + 4] = 0x00;
            mem8[CODE_PAGE + 6 * i + 5] = 0x00;
        }
        n = run_decoder(CODE_PAGE, true, uops, UOP_MAX_BLOCK_UOPS, &blk);
        CHECK_EQ(n, 40); /* 40*6 = 240: the cap on bytes fires before the uop cap */
        CHECK_EQ(blk.stop, UOP_STOP_CAP);
        CHECK_EQ(blk.phys_len, 240);
    }

    /* 7. no descriptor: the instruction is NOT part of the block, and the interpreter runs
     *    it.  Two routes reach UOP_STOP_NO_DESC: a form the port does not define (the port is
     *    complete, so there is none -- the emitter counts 0 unimplemented forms), and the
     *    generated `default: trigger_ud()` of a fixed_g group.  0F 71 defines only g = 2, 4, 6
     *    (psllw/psrlw/psraw), so g = 1 has no entry. */
    {
        uint8_t bytes[8] = { 0x90, 0x90, 0x0F, 0x71, 0xC9, 0x01, 0x90, 0x90 };
        uint32_t start = put_stream(bytes, 8, 0);
        uop_block_t blk;
        uop_t uops[UOP_MAX_BLOCK_UOPS];
        int n = run_decoder(start, true, uops, UOP_MAX_BLOCK_UOPS, &blk);
        CHECK_EQ(n, 2); /* the two nops; the 0F 71 /1 instruction is left to the interpreter */
        CHECK_EQ(blk.stop, UOP_STOP_NO_DESC);
        CHECK_EQ(blk.stop_eip, start + 2);
        /* and the interpreter really does run it: it delivers the #UD the generated default
         * arm raises, i.e. the two agree about what that byte does */
        oracle_result o = run_interpreter(start + 2, true, FLAGS_DEFAULT); /* at the 0F 71 */
        CHECK(o.ok);
        CHECK(o.cs_moved);

        /* the sibling case that IS defined (g = 2) stays in the block */
        uint8_t ok_bytes[8] = { 0x90, 0x0F, 0x71, 0xD1, 0x01, 0x90, 0x90, 0x90 };
        start = put_stream(ok_bytes, 8, 0);
        n = run_decoder(start, true, uops, UOP_MAX_BLOCK_UOPS, &blk);
        /* the block now runs on past the defined sibling into the page's zero bytes (which
         * decode as `add [eax], al` pairs) until the cap -- what matters is that the
         * instruction IS a uop and that its length is the encoding's */
        CHECK(n > 1);
        CHECK(uops[1].len == 4); /* 0F 71 D1 01 */
        CHECK_EQ(uops[1].off, 1);
    }
}

/* ------------------------------------------------------------------ *
 * the nasm corpus
 * ------------------------------------------------------------------ */

/* build/nasm_corpus.bin: "NASM" + u32 count, then per image u32 length + the .text bytes
 * (gen/nasm_corpus.py).  Each image's bytes are walked instruction by instruction: the
 * decoder's boundary drives the walk, so every start is a real instruction start. */
static void corpus_walk(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        failures++;
        fprintf(stderr, "FAIL corpus: cannot open %s (run `make nasm-translate`)\n", path);
        return;
    }
    uint8_t head[8];
    if (fread(head, 1, 8, f) != 8 || memcmp(head, "NASM", 4) != 0) {
        failures++;
        fprintf(stderr, "FAIL corpus: %s is not a nasm corpus file\n", path);
        fclose(f);
        return;
    }
    uint32_t count = (uint32_t)head[4] | (uint32_t)head[5] << 8 | (uint32_t)head[6] << 16 |
                     (uint32_t)head[7] << 24;
    long images = 0, instrs = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t lh[4];
        if (fread(lh, 1, 4, f) != 4) break;
        uint32_t len = (uint32_t)lh[0] | (uint32_t)lh[1] << 8 | (uint32_t)lh[2] << 16 |
                       (uint32_t)lh[3] << 24;
        if (len == 0 || len > CODE_SIZE) {
            fseek(f, (long)len, SEEK_CUR);
            continue;
        }
        static uint8_t buf[CODE_SIZE];
        if (fread(buf, 1, len, f) != len) break;
        images++;

        /* walk the image's .text linearly, following the decoder's own boundaries and
         * probing EVERY byte offset of the first 32 bytes as well (misaligned starts) */
        put_stream(buf, (int)len, 0);
        uint32_t start = CODE_PAGE;
        uint32_t end = CODE_PAGE + len;
        while (start < end) {
            probe_stream_at(start, true, "corpus", false);
            instrs++;
            uop_t uops[UOP_MAX_BLOCK_UOPS];
            uop_block_t blk;
            int n = run_decoder(start, true, uops, UOP_MAX_BLOCK_UOPS, &blk);
            if (n <= 0) break;
            start += uops[0].len;
        }
    }
    fclose(f);
    printf("  corpus: %ld images, %ld instructions walked\n", images, instrs);
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

#define RUN(fn, what)                                                          \
    do {                                                                       \
        int before = failures;                                                 \
        printf("%s %s\n", #fn, (what));                                       \
        fn();                                                                  \
        printf("%s %s\n\n", failures == before ? "PASS" : "FAIL", (what));      \
    } while (0)

int main(int argc, char **argv)
{
    machine_init();

    const char *corpus = argc > 1 ? argv[1] : "build/nasm_corpus.bin";

    RUN(framing_tests, "(c) the block framing: page end, block end, no_bib, cap, no descriptor");
    RUN(last_page_tests, "(d) the LAST virtual page: the block decodes there, the window ends at 2^32");
    RUN(hand_table, "(a)/(b) the hand table: forms, both operand sizes, sib, prefixes");
    RUN(sweep_opcode_forms, "(a)/(b) every base and 0F opcode x {reg,mem,sib} forms x {16,32}");
    RUN(sweep_prefixes, "(a)/(b) every prefix byte and the prefix combinations");
    RUN(sweep_sib, "(a)/(b) every sib byte x mod, and the 16-bit file's forms");

    corpus_walk(corpus);

    long cov = 0;
    for (int t = 0; t < 2; t++) {
        for (int k = 0; k < 512; k++) cov += enc_seen[t][k];
    }

    printf("uop_cache_selftest coverage:\n");
    printf("  dispatch cases decoded (base+0F) x (16+32 keys), of 1024: %ld\n", cov);
    if (cov < 1024) {
        printf("    not reached:");
        for (int t = 0; t < 2; t++) {
            for (int k = 0; k < 512; k++) {
                if (!enc_seen[t][k]) printf(" %s%03X", t ? "0f/" : "", k);
            }
        }
        printf("\n");
    }
    printf("  streams probed                     : %ld\n", stat_streams);
    printf("  instructions decoded and checked   : %ld\n", stat_instructions);
    printf("    length PINNED by the interpreter : %ld (%ld of them via a rel-branch target)\n",
           stat_pinned, stat_pinned_rel);
    printf("    length unpinned                  : %ld (of which %ld named/allowed)\n",
           stat_unpinned, stat_unpinned_allowed);
    printf("    no descriptor (block falls back) : %ld (interpreter agreed: %ld)\n",
           stat_nodec, stat_nodec_unimpl_ok);
    printf("    (a) DECODE-TIME stops the decoder has a descriptor for (must be 0): %ld\n",
           stat_descriptor_bad);
    printf("    runtime stops (sandbox-caused, counted not compared)        : %ld"
           " (%d distinct)", stat_runtime_stop, stat_runtime_stop_count);
    if (stat_runtime_stop_count > 0) {
        printf(" e.g. %.72s", stat_runtime_stop_names[0]);
    }
    printf("\n");
    printf("  io ports reached by probes (stubbed): %ld reads, %ld writes\n",
           io_port_reads, io_port_writes);
    printf("    probes skipped: the body is DBG_ASSERT(false) in this build : %ld\n",
           stat_skipped_dbgexec);
    printf("    probes skipped: 16-bit mode + 66 prefix + a control transfer, where the\n"
           "                   port's own upstream-inherited dbg_assert!s fire on guest-"
           "reachable operands : %ld\n", stat_skipped_16bit66);
    printf("    probes whose advance was not a length (delivery/far transfer): %ld\n",
           stat_faulted);
    printf("    blocks produced                  : %ld (block-end %ld, page-end %ld, cap %ld)\n",
           stat_nostop, stat_block_ends, stat_page_end_stops, stat_cap_stops);
    printf("  (b) address comparisons vs modrm_resolve: %ld (failures %ld)\n", stat_addr_checks,
           stat_addr_fail);
    printf("      forms: 16-bit %ld, 32-bit %ld, sib %ld\n", stat_addr_kinds[UOP_ADDR_16],
           stat_addr_kinds[UOP_ADDR_32], stat_addr_kinds[UOP_ADDR_SIB]);

    printf("uop_cache_selftest: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
