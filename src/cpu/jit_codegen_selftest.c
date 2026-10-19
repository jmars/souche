/*
 * jit_codegen_selftest.c -- UNIT JIT-1's GATE (HANDOFF=v86c-jit, milestone M1).
 *
 *     make selftest        # builds and runs this binary with the other suites
 *     make jit-codegen-selftest   # this suite alone
 *
 * WHY A SEPARATE BINARY.  Same reason as the other two JIT-adjacent suites (jit_selftest.c's
 * and uop_exec_selftest.c's headers): this suite RUNS the machine -- the real run loop, the
 * real uop cache, the real compiled blocks -- so it needs the real cpu.c/exceptions.c and a
 * device-free machine, and it supplies its own io_port_* bridge and does NOT link src/hw (the
 * device layer STOPS the machine for an unclaimed port).  The machine fixture is COPIED from
 * uop_exec_selftest.c rather than shared, deliberately: a shared harness would have to live in
 * the shipped link, and the shipped link must not grow a test harness.
 *
 * WHAT IT PINS, AND WHY EACH THING IS THE THING THAT CAN FAIL.
 *
 *  1. THE ENCODER'S BYTES (jit_x64.c).  Every emitted form is compared byte for byte against
 *     the encoding the host's own assembler produces for the same instruction (the ground
 *     truth is `as`/objdump, transcribed into the pins).  A JIT whose encoder is wrong emits
 *     wrong code, and a wrong encoding of a single form is otherwise invisible until it
 *     executes something it should not -- the pins make it visible at `make selftest`.
 *     The label/relocation machinery is pinned with its own forward reference, and an UNBOUND
 *     label must make jx64_finish FAIL (a patcher that silently leaves a zero displacement is
 *     a JIT that jumps to its own beginning).
 *
 *  2. THE ONE RECIPE, DIFFERENTIALLY (jit-off vs jit-on).  A program of `89 /r` reg-form moves
 *     -- the recipe -- plus non-recipe uops runs twice, once through the uop executor and once
 *     through the compiled block, and the FULL cpu state, the pending count and the whole
 *     guest RAM are compared.  The assertion that gives it meaning: the compiled path was
 *     REALLY taken (jit_codegen_stats.blocks_compiled is 1 and inlined_uops is exactly the
 *     number of recipe uops the program executed) -- a probe that fell back to the uop path
 *     would pass every state comparison in this file and test nothing.
 *
 *  3. THE CONTRACT PINS the plan's M2 breadth relies on:
 *     (a) THE FAULT / EARLY-RETURN -- two programs whose instruction does NOT run: a
 *         task-switch-guarded FPU mem form with CR0.TS set (the guard fails -> #NM, and a
 *         materialisation-first implementation would deliver #GP instead -- the guard ORDER
 *         pin), and a plain mem form with a NULL DS segment (the address materialisation
 *         faults -> #GP).  Both must deliver the SAME vector and leave the SAME state through
 *         the compiled path, which is exactly the "the body did not run, do not flush" rule.
 *     (b) THE COUNTER/PENDING STAIRCASE -- a program that reads the guest-visible clock
 *         (rdtsc) at three points, through a FLUSH-flagged instruction (cmpxchg, a reference
 *         batch end in the middle of a block) and through `sti` (a block end that is NOT a
 *         batch end, the one whose count must stay PENDING).  The three readings, the final
 *         counter and uop_pending must be identical in all three arms: the interpreter, the
 *         uop cache, and the compiled block.
 *     (c) THE PREFIX / previous_ip STORES -- a segment-prefixed memory operand whose segment
 *         OVERRIDE lives in cpu->prefixes at execute time (uop_seg_base reads it), so a
 *         compiled block that skipped the prefixes store would read the wrong segment; and
 *         previous_ip, which is compared as part of the full state (and asserted directly).
 *     (d) THE SMC ABORT -- the port's self-modifying-code probe, with the compiled execution
 *         path: the block that is RUNNING must stop at its next uop boundary when the write
 *         lands on its own page, and the patched instruction must then execute.
 *
 *  4. THE EXIT KINDS ROUND-TRIP.  All four (RETURN, TAIL, ABORT, BLOCK_END) are produced by
 *     the suite's programs and counted in jit_codegen_stats.exits[] where uop_run reads the
 *     block's return value -- so each kind is asserted to have occurred, not assumed.
 *
 *  5. THE COVERAGE COUNTERS AGREE WITH WHAT WAS COMPILED (the brief's G4): the dynamic
 *     inlined/called split is asserted to equal the recipe/non-recipe split of the uops the
 *     program actually executed, and the compile-time counts to equal the descriptors in the
 *     block that was compiled.
 *
 * ABLE TO FAIL: results/jit-1-mut-rig.sh mutates one thing per arm (the recipe's direction, the
 * spill, the reload, the abort check, the materialisation-fault check, the prefixes store, the
 * flush, the previous_ip store, the REX bit in the encoder) and requires THIS BINARY to fail
 * for each -- asymmetry is the proof.  A control arm runs the unmutated tree.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "cpu_state.h"
#include "exceptions.h"
#include "flags.h"
#include "instructions.h"
#include "instructions_0f.h"
#include "jit_codegen.h"
#include "jit_mem.h"
#include "jit_x64.h"
#include "paging.h"
#include "memory.h"
#include "misc_instr.h"
#include "modrm.h"
#include "paging.h"
#include "uop_cache.h"

static int checks = 0;
static int failures = 0;

#define CHECK(cond)                                                          \
	do {                                                                     \
		checks++;                                                            \
		if (!(cond)) {                                                       \
			failures++;                                                      \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
		}                                                                    \
	} while (0)

#define CHECK_EQ(a, b)                                                            \
	do {                                                                          \
		checks++;                                                                 \
		long long _a = (long long)(a), _b = (long long)(b);                       \
		if (_a != _b) {                                                           \
			failures++;                                                           \
			fprintf(stderr, "FAIL %s:%d: %s == %s (got 0x%llx, want 0x%llx)\n",   \
			        __FILE__, __LINE__, #a, #b, (unsigned long long)_a,           \
			        (unsigned long long)_b);                                      \
		}                                                                         \
	} while (0)

/* ------------------------------------------------------------------ *
 * the machine (the uop_exec_selftest.c fixture, copied)
 * ------------------------------------------------------------------ */

#define GUEST_RAM  (2u * 1024 * 1024)
#define GDT_BASE   0x5000u
#define SEG_CODE32 0x08u
#define SEG_DATA   0x10u

#define CODE        0x1000u
#define TRACE       0x3100u
#define VEC_RECORD  0x3000u
#define IDT_BASE    0x6000u
#define IDT_STUB_BASE 0x2400u
#define IDT_STUB_LEN  9u

static long io_port_reads, io_port_writes;
int32_t io_port_read8(int32_t port) { io_port_reads++; (void)port; return 0; }
int32_t io_port_read16(int32_t port) { io_port_reads++; (void)port; return 0; }
int32_t io_port_read32(int32_t port) { io_port_reads++; (void)port; return 0; }
void io_port_write8(int32_t port, int32_t value) { io_port_writes++; (void)port; (void)value; }
void io_port_write16(int32_t port, int32_t value) { io_port_writes++; (void)port; (void)value; }
void io_port_write32(int32_t port, int32_t value) { io_port_writes++; (void)port; (void)value; }
double run_hardware_timers(bool acpi_enabled, double t) { (void)acpi_enabled; return t; }

static cpu_state_t cpu_pristine;

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

static uint64_t mk_idt_entry(uint32_t offset, uint16_t selector, uint8_t gate_type, uint8_t dpl,
                             bool is_32)
{
	uint64_t d = 0;
	d |= (uint64_t)(offset & 0xFFFF);
	d |= (uint64_t)selector << 16;
	d |= (uint64_t)(0x80 | (dpl << 5) | (is_32 ? 8 : 0) | gate_type) << 40;
	d |= (uint64_t)((offset >> 16) & 0xFFFF) << 48;
	return d;
}

static void build_tables(void)
{
	wr64(GDT_BASE + 0 * 8, 0);
	wr64(GDT_BASE + 1 * 8, make_desc(0, 0xFFFFF, 0x9A, 0xC));
	wr64(GDT_BASE + 2 * 8, make_desc(0, 0xFFFFF, 0x92, 0xC));
	cpu->gdtr_offset = GDT_BASE;
	cpu->gdtr_size = 2 * 8 - 1;
	cpu->idtr_offset = 0;
	cpu->idtr_size = 0;
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

	cpu->cr[0] = 1;
	cpu->protected_mode = true;
	cpu->cpl = 0;
	cpu->is_32 = true;
	cpu->stack_size_32 = true;
	cpu->flags = FLAGS_DEFAULT;
	for (int i = 0; i < 6; i++) {
		cpu->segment_is_null[i] = false;
		cpu->segment_limits[i] = 0xFFFFFFFFu;
		cpu->sreg[i] = (uint16_t)(i == CS ? SEG_CODE32 : SEG_DATA);
		cpu->segment_access_bytes[i] = 0x92;
		cpu->segment_offsets[i] = 0;
	}
	cpu->segment_access_bytes[CS] = 0x9A;
	for (int i = 0; i < 8; i++) write_reg32(i, 0);
	write_reg32(ESP, 0x80000);
	update_state_flags();
	build_tables();
	interpreted_unimplemented = false;
	cpu_pristine = *cpu;
}

/* Put the machine back to the post-init state.  The direct `mem8` writes this harness does are
 * NOT visible to the port's write hooks (that is the point of the uop_cache.c header's warning
 * about selftest harnesses), so the compiled blocks must be dropped explicitly -- which is
 * what uop_cache_clear does, through jit_code_clear_all (uop_cache.c). */
static void machine_reset(void)
{
	*cpu = cpu_pristine;
	memset(mem8, 0, GUEST_RAM);
	build_tables();
	uop_cache_clear();
	uop_stats_reset();
	interpreted_unimplemented = false;
	jit_block_boundary = false;
	uop_pending = 0; /* a fresh boot starts with no unflushed batch */
}

/* THE TSC'S WORKAROUND STATE IS GLOBAL (cpu.c's tsc_last_value / tsc_resolution /
 * tsc_number_of_same_readings are file statics, NOT cpu_state), so two arms of the same
 * program can see different `extra` values unless the harness equalises that history first.
 * This is not decoration: the port's rdtsc genuinely extrapolates when two reads return the
 * same value, and on a fresh machine tsc_resolution is u64::MAX, so the FIRST arm of a
 * program that reads the clock at counter 0 gets value+extra = 0 + (u64::MAX-1) and the low
 * word comes out 0xFFFFFFFE (MEASURED).  Warming drives tsc_resolution to one instruction's
 * worth of counts and leaves tsc_last_value ABOVE every value the arm will produce, which is
 * what makes each arm's readings exactly counter*1000. */
static void tsc_warm(void)
{
	uint32_t save = cpu->instruction_counter;
	cpu->instruction_counter = 0x100000u;
	for (int i = 0; i < 4; i++) {
		(void)read_tsc();
		cpu->instruction_counter += 1;
	}
	cpu->instruction_counter = save;
}

/* every vector's handler: record it with an SS override (DS may be broken by a probe) and hlt */
static void build_idt_stubs(void)
{
	for (uint32_t v = 0; v < 256; v++) {
		uint32_t a = IDT_STUB_BASE + IDT_STUB_LEN * v;
		mem8[a + 0] = 0x36;
		mem8[a + 1] = 0xC6;
		mem8[a + 2] = 0x05;
		mem8[a + 3] = (uint8_t)(VEC_RECORD & 0xFF);
		mem8[a + 4] = (uint8_t)((VEC_RECORD >> 8) & 0xFF);
		mem8[a + 5] = (uint8_t)((VEC_RECORD >> 16) & 0xFF);
		mem8[a + 6] = (uint8_t)((VEC_RECORD >> 24) & 0xFF);
		mem8[a + 7] = (uint8_t)v;
		mem8[a + 8] = 0xF4;
		wr64(IDT_BASE + 8u * v,
		     mk_idt_entry(a, SEG_CODE32, INTR_GATE_INTERRUPT, 0, true));
	}
	cpu->idtr_offset = IDT_BASE;
	cpu->idtr_size = 256 * 8 - 1;
}

static void run_machine(uint32_t budget)
{
	uint32_t start = cpu->instruction_counter;
	uint32_t guard = 0;
	while (!cpu->in_hlt && !interpreted_unimplemented
	       && (cpu->instruction_counter - start) < budget
	       && guard++ < 1000000u) {
		cycle_internal();
	}
}

static void load(uint32_t addr, const uint8_t *bytes, int len)
{
	memcpy(mem8 + addr, bytes, (size_t)len);
}

static uint32_t ram32(uint32_t addr)
{
	uint32_t v;
	memcpy(&v, mem8 + addr, 4);
	return v;
}

/* ------------------------------------------------------------------ *
 * 1. THE ENCODER'S BYTES
 * ------------------------------------------------------------------ */

static jx64_t pe;
static uint8_t pbuf[64];

static void pin_reset(void) { jx64_init(&pe, pbuf, sizeof pbuf); }

static void pin_want(const char *name, const uint8_t *want, size_t n)
{
	checks++;
	size_t got = jx64_len(&pe);
	if (got != n || memcmp(pbuf, want, n) != 0) {
		failures++;
		fprintf(stderr, "FAIL byte pin: %s (got %zu bytes, want %zu)\n", name, got, n);
		jx64_dump("  got", pbuf, got);
		jx64_dump(" want", want, n);
	}
}

/* The ground truth for every sequence below is the host assembler's own encoding of the same
 * instruction (`as --64` + `objdump -d -M intel`), transcribed; the two forms where the
 * assembler prefers a shorter encoding (the imm8 form of `add r32, imm8` and the 0x9x short
 * form of `xchg eax, r32`) are pinned to THIS emitter's chosen form, which is the same
 * instruction.  results/jit-1/g1-encoder-pins.txt is the transcript of that comparison. */
static void test_encoder_pins(void)
{
	fprintf(stderr, "  == test_encoder_pins\n");
	pin_reset();
	jx64_mov_r32_r32(&pe, JX_RAX, JX_RCX);
	{ static const uint8_t w[] = { 0x89, 0xC8 }; pin_want("mov r32,r32 (eax,ecx)", w, sizeof w); }

	pin_reset();
	jx64_mov_r32_r32(&pe, JX_R12, JX_R11);
	{ static const uint8_t w[] = { 0x45, 0x89, 0xDC }; pin_want("mov r32,r32 (r12d,r11d)", w, sizeof w); }

	pin_reset();
	jx64_mov_r32_m32(&pe, JX_RCX, JX_RBP, 4);
	{ static const uint8_t w[] = { 0x8B, 0x4D, 0x04 }; pin_want("mov r32,[rbp+4]", w, sizeof w); }

	pin_reset();
	jx64_mov_m32_r32(&pe, JX_RBP, 28, JX_R15);
	{ static const uint8_t w[] = { 0x44, 0x89, 0x7D, 0x1C }; pin_want("mov [rbp+28],r15d", w, sizeof w); }

	pin_reset();
	jx64_mov_r32_imm32(&pe, JX_RDI, 0x1234);
	{ static const uint8_t w[] = { 0xBF, 0x34, 0x12, 0x00, 0x00 }; pin_want("mov edi,imm32", w, sizeof w); }

	pin_reset();
	jx64_mov_r64_imm64(&pe, JX_R11, 0x1122334455667788ull);
	{ static const uint8_t w[] = { 0x49, 0xBB, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };
	  pin_want("movabs r11,imm64", w, sizeof w); }

	pin_reset();
	jx64_mov_m8_imm8(&pe, JX_RBP, 672, 0);
	{ static const uint8_t w[] = { 0xC6, 0x85, 0xA0, 0x02, 0x00, 0x00, 0x00 };
	  pin_want("mov byte [rbp+672],0", w, sizeof w); }

	pin_reset();
	jx64_mov_m32_imm32(&pe, JX_RSP, 8, 3);
	{ static const uint8_t w[] = { 0xC7, 0x44, 0x24, 0x08, 0x03, 0x00, 0x00, 0x00 };
	  pin_want("mov dword [rsp+8],3 (the SIB framing case)", w, sizeof w); }

	pin_reset();
	jx64_add_r32_r32(&pe, JX_R11, JX_R9);
	{ static const uint8_t w[] = { 0x45, 0x01, 0xCB }; pin_want("add r11d,r9d", w, sizeof w); }

	pin_reset();
	jx64_add_r32_imm32(&pe, JX_RDI, 0x10);
	{ static const uint8_t w[] = { 0x81, 0xC7, 0x10, 0x00, 0x00, 0x00 };
	  pin_want("add edi,imm32", w, sizeof w); }

	pin_reset();
	jx64_add_m32_r32(&pe, JX_RDI, 0, JX_R11);
	{ static const uint8_t w[] = { 0x44, 0x01, 0x1F }; pin_want("add [rdi],r11d", w, sizeof w); }

	pin_reset();
	jx64_add_m64_r64(&pe, JX_RDI, 0, JX_R11);
	{ static const uint8_t w[] = { 0x4C, 0x01, 0x1F }; pin_want("add qword [rdi],r11", w, sizeof w); }

	pin_reset();
	jx64_add_m32_imm8(&pe, JX_RSP, 0, 1);
	{ static const uint8_t w[] = { 0x83, 0x04, 0x24, 0x01 }; pin_want("add dword [rsp],1", w, sizeof w); }

	pin_reset();
	jx64_sub_r32_r32(&pe, JX_R11, JX_R10);
	{ static const uint8_t w[] = { 0x45, 0x29, 0xD3 }; pin_want("sub r11d,r10d", w, sizeof w); }

	pin_reset();
	jx64_sub_r32_m32(&pe, JX_RDI, JX_R11, 0);
	{ static const uint8_t w[] = { 0x41, 0x2B, 0x3B }; pin_want("sub edi,[r11]", w, sizeof w); }

	pin_reset();
	jx64_and_r32_r32(&pe, JX_RAX, JX_RCX);
	{ static const uint8_t w[] = { 0x21, 0xC8 }; pin_want("and eax,ecx", w, sizeof w); }

	pin_reset();
	jx64_or_r32_r32(&pe, JX_RDX, JX_RSI);
	{ static const uint8_t w[] = { 0x09, 0xF2 }; pin_want("or edx,esi", w, sizeof w); }

	pin_reset();
	jx64_xor_r32_r32(&pe, JX_R11, JX_R10);
	{ static const uint8_t w[] = { 0x45, 0x31, 0xD3 }; pin_want("xor r11d,r10d", w, sizeof w); }

	pin_reset();
	jx64_cmp_r32_r32(&pe, JX_R9, JX_R10);
	{ static const uint8_t w[] = { 0x45, 0x39, 0xD1 }; pin_want("cmp r9d,r10d", w, sizeof w); }

	pin_reset();
	jx64_cmp_r32_imm32(&pe, JX_R9, 100001);
	{ static const uint8_t w[] = { 0x41, 0x81, 0xF9, 0xA1, 0x86, 0x01, 0x00 };
	  pin_want("cmp r9d,100001", w, sizeof w); }

	pin_reset();
	jx64_cmp_m32_r32(&pe, JX_RBP, 68, JX_RSI);
	{ static const uint8_t w[] = { 0x39, 0x75, 0x44 }; pin_want("cmp [rbp+68],esi", w, sizeof w); }

	pin_reset();
	jx64_cmp_m8_imm8(&pe, JX_R8, 0, 0);
	{ static const uint8_t w[] = { 0x41, 0x80, 0x38, 0x00 }; pin_want("cmp byte [r8],0", w, sizeof w); }

	pin_reset();
	jx64_cmp_m32_imm32(&pe, JX_RBP, 68, 0x12345678);
	{ static const uint8_t w[] = { 0x81, 0x7D, 0x44, 0x78, 0x56, 0x34, 0x12 };
	  pin_want("cmp dword [rbp+68],imm32", w, sizeof w); }

	pin_reset();
	jx64_test_r32_r32(&pe, JX_RAX, JX_RAX);
	{ static const uint8_t w[] = { 0x85, 0xC0 }; pin_want("test eax,eax", w, sizeof w); }

	pin_reset();
	jx64_test_r32_imm32(&pe, JX_R11, 0xFFFFF000u);
	{ static const uint8_t w[] = { 0x41, 0xF7, 0xC3, 0x00, 0xF0, 0xFF, 0xFF };
	  pin_want("test r11d,0xFFFFF000", w, sizeof w); }

	pin_reset();
	jx64_test_r8_imm8(&pe, JX_RDI, 4);
	{ static const uint8_t w[] = { 0x40, 0xF6, 0xC7, 0x04 };
	  pin_want("test dil,4 (a forced REX selects DIL over BH)", w, sizeof w); }

	pin_reset();
	jx64_xchg_r32_r32(&pe, JX_RAX, JX_RCX);
	{ static const uint8_t w[] = { 0x87, 0xC8 }; pin_want("xchg eax,ecx", w, sizeof w); }

	pin_reset();
	jx64_inc_r32(&pe, JX_R9);
	{ static const uint8_t w[] = { 0x41, 0xFF, 0xC1 }; pin_want("inc r9d", w, sizeof w); }

	pin_reset();
	jx64_dec_r32(&pe, JX_RDX);
	{ static const uint8_t w[] = { 0xFF, 0xCA }; pin_want("dec edx", w, sizeof w); }

	pin_reset();
	jx64_not_r32(&pe, JX_RCX);
	{ static const uint8_t w[] = { 0xF7, 0xD1 }; pin_want("not ecx", w, sizeof w); }

	pin_reset();
	jx64_neg_r32(&pe, JX_RCX);
	{ static const uint8_t w[] = { 0xF7, 0xD9 }; pin_want("neg ecx", w, sizeof w); }

	pin_reset();
	jx64_ror_r32_imm8(&pe, JX_RAX, 3);
	{ static const uint8_t w[] = { 0xC1, 0xC8, 0x03 }; pin_want("ror eax,3", w, sizeof w); }

	pin_reset();
	jx64_shl_r32_imm8(&pe, JX_RAX, 4);
	{ static const uint8_t w[] = { 0xC1, 0xE0, 0x04 }; pin_want("shl eax,4", w, sizeof w); }

	pin_reset();
	jx64_shr_r32_imm8(&pe, JX_RDX, 5);
	{ static const uint8_t w[] = { 0xC1, 0xEA, 0x05 }; pin_want("shr edx,5", w, sizeof w); }

	pin_reset();
	jx64_sar_r32_imm8(&pe, JX_RAX, 7);
	{ static const uint8_t w[] = { 0xC1, 0xF8, 0x07 }; pin_want("sar eax,7", w, sizeof w); }

	pin_reset();
	jx64_push_r64(&pe, JX_RBP);
	{ static const uint8_t w[] = { 0x55 }; pin_want("push rbp", w, sizeof w); }

	pin_reset();
	jx64_pop_r64(&pe, JX_R15);
	{ static const uint8_t w[] = { 0x41, 0x5F }; pin_want("pop r15", w, sizeof w); }

	pin_reset();
	jx64_sub_r64_imm8(&pe, JX_RSP, 24);
	{ static const uint8_t w[] = { 0x48, 0x83, 0xEC, 0x18 }; pin_want("sub rsp,24", w, sizeof w); }

	pin_reset();
	jx64_add_r64_imm8(&pe, JX_RSP, 8);
	{ static const uint8_t w[] = { 0x48, 0x83, 0xC4, 0x08 }; pin_want("add rsp,8", w, sizeof w); }

	pin_reset();
	jx64_call_r64(&pe, JX_R11);
	{ static const uint8_t w[] = { 0x41, 0xFF, 0xD3 }; pin_want("call r11", w, sizeof w); }

	pin_reset();
	jx64_ret(&pe);
	{ static const uint8_t w[] = { 0xC3 }; pin_want("ret", w, sizeof w); }

	/* JIT-FIX-1 should-fix 2: THE EIGHT EMITTERS THAT WERE NEITHER CALLED NOR PINNED.
	 * jit_x64.h:255 claims every emitter is either called from jit_codegen.c or pinned
	 * here; results/jit-2r-review.md's scan found eight that were neither.  They are
	 * PINNED rather than deleted: the header is the module's API, and a dead-but-covered
	 * emitter is a smaller liability than a missing one.  The ground truth is the host
	 * assembler again, with two FORCED-REX cases whose expectation comes from as's `sil`
	 * spelling -- an 8-bit REGISTER operand in this encoder always carries a REX prefix
	 * (jit_x64.c:514's `!dst->mem`), which is exactly what makes rm=6 mean SIL and not BH. */
	{
		jx_operand_t r16m = { true, JX_RBP, -1, 0, 4 };      /* [rbp+4] */
		jx_operand_t r16r8 = { true, JX_R8, -1, 0, 0 };      /* [r8] */
		jx_operand_t b672 = { true, JX_RBP, -1, 0, 672 };    /* [rbp+672] */
		jx_operand_t mrdi = { true, JX_RDI, -1, 0, 0 };      /* [rdi] */
		jx_operand_t rsi8 = { false, JX_RSI, -1, 0, 0 };     /* sil */
		jx_operand_t r11 = { false, JX_R11, -1, 0, 0 };
		jx_operand_t rax = { false, JX_RAX, -1, 0, 0 };
		jx_operand_t r10 = { false, JX_R10, -1, 0, 0 };

		pin_reset();
		jx64_mov_r16_rm16(&pe, JX_RDI, &r16m);
		{ static const uint8_t w[] = { 0x66, 0x8B, 0x7D, 0x04 };
		  pin_want("mov r16,r/m16 (di,[rbp+4])", w, sizeof w); }

		pin_reset();
		jx64_mov_r16_rm16(&pe, JX_R12, &r16r8);
		{ static const uint8_t w[] = { 0x66, 0x45, 0x8B, 0x20 };
		  pin_want("mov r16,r/m16 (r12w,[r8]) -- REX.R + REX.B", w, sizeof w); }

		pin_reset();
		jx64_inc_rm32(&pe, &r11);
		{ static const uint8_t w[] = { 0x41, 0xFF, 0xC3 }; pin_want("inc r/m32 (r11d)", w, sizeof w); }

		pin_reset();
		jx64_dec_rm32(&pe, &mrdi);
		{ static const uint8_t w[] = { 0xFF, 0x0F }; pin_want("dec r/m32 ([rdi])", w, sizeof w); }

		pin_reset();
		jx64_not_rm32(&pe, &rax);
		{ static const uint8_t w[] = { 0xF7, 0xD0 }; pin_want("not r/m32 (eax)", w, sizeof w); }

		pin_reset();
		jx64_alu_rm8_imm8(&pe, 4 /* and */, &b672, 0x0F);
		{ static const uint8_t w[] = { 0x80, 0xA5, 0xA0, 0x02, 0x00, 0x00, 0x0F };
		  pin_want("and r/m8,imm8 ([rbp+672],0x0F)", w, sizeof w); }

		pin_reset();
		jx64_alu_rm8_imm8(&pe, 4 /* and */, &rsi8, 0x0F);
		{ static const uint8_t w[] = { 0x40, 0x80, 0xE6, 0x0F };
		  pin_want("and r/m8,imm8 (sil,0x0F) -- the FORCED REX", w, sizeof w); }

		pin_reset();
		jx64_test_rm8_imm8(&pe, &b672, 0x0F);
		{ static const uint8_t w[] = { 0xF6, 0x85, 0xA0, 0x02, 0x00, 0x00, 0x0F };
		  pin_want("test r/m8,imm8 ([rbp+672],0x0F)", w, sizeof w); }

		pin_reset();
		jx64_test_rm8_imm8(&pe, &rsi8, 0x0F);
		{ static const uint8_t w[] = { 0x40, 0xF6, 0xC6, 0x0F };
		  pin_want("test r/m8,imm8 (sil,0x0F) -- the FORCED REX", w, sizeof w); }

		pin_reset();
		jx64_shift_rm32_cl(&pe, 4 /* shl */, &r10);
		{ static const uint8_t w[] = { 0x41, 0xD3, 0xE2 };
		  pin_want("shl r/m32,cl (r10d)", w, sizeof w); }

		pin_reset();
		jx64_test_rm32_r32(&pe, &mrdi, JX_R11);
		{ static const uint8_t w[] = { 0x44, 0x85, 0x1F };
		  pin_want("test r/m32,r32 ([rdi],r11d)", w, sizeof w); }
	}

	/* the labels: a FORWARD reference, patched by jx64_finish */
	pin_reset();
	int l = jx64_label(&pe);
	jx64_jmp_label(&pe, l);          /* E9 rel32: off = 1..4, target = 7 (after the mov) */
	jx64_mov_r32_r32(&pe, JX_RAX, JX_RCX);
	jx64_bind(&pe, l);
	jx64_ret(&pe);
	CHECK(jx64_finish(&pe));
	{ static const uint8_t w[] = { 0xE9, 0x02, 0x00, 0x00, 0x00, 0x89, 0xC8, 0xC3 };
	  pin_want("jmp label (rel32 = 2)", w, sizeof w); }

	pin_reset();
	l = jx64_label(&pe);
	jx64_jcc_label(&pe, JX_CC_NE, l); /* 0F 85 rel32: off = 2..5, target = 8 */
	jx64_ret(&pe);
	jx64_bind(&pe, l);
	jx64_ret(&pe);
	CHECK(jx64_finish(&pe));
	{ static const uint8_t w[] = { 0x0F, 0x85, 0x01, 0x00, 0x00, 0x00, 0xC3, 0xC3 };
	  pin_want("jne label (rel32 = 1)", w, sizeof w); }

	pin_reset();
	l = jx64_label(&pe);
	jx64_call_label(&pe, l);         /* E8 rel32: off = 1..4, target = 6 */
	jx64_ret(&pe);
	jx64_bind(&pe, l);
	jx64_ret(&pe);
	CHECK(jx64_finish(&pe));
	{ static const uint8_t w[] = { 0xE8, 0x01, 0x00, 0x00, 0x00, 0xC3, 0xC3 };
	  pin_want("call label (rel32 = 1)", w, sizeof w); }

	/* AN UNBOUND LABEL MUST FAIL: a patcher that leaves a zero displacement is a JIT that
	 * jumps to its own beginning, and it would be silent. */
	pin_reset();
	l = jx64_label(&pe);
	jx64_jmp_label(&pe, l);
	CHECK(!jx64_finish(&pe));

	/* and a BUFFER THAT CANNOT HOLD THE BLOCK must be reported, not written past */
	uint8_t tiny[4];
	jx64_t te;
	jx64_init(&te, tiny, sizeof tiny);
	jx64_mov_r64_imm64(&te, JX_RAX, 0xDEADBEEFDEADBEEFull);
	CHECK(jx64_overflow(&te));
	CHECK(!jx64_finish(&te));

	/* the emitter's error text names the cause (used by the diagnostics above) */
	pin_reset();
	CHECK(strcmp(jx64_last_error(&pe), "ok") == 0);
}

/* ------------------------------------------------------------------ *
 * 2-5. THE MACHINE PINS
 * ------------------------------------------------------------------ */

#define SNAP_RAM (2u * 1024 * 1024)

typedef struct snap {
	cpu_state_t cpu;
	uint32_t pending;
	uint32_t vec;
	uint8_t *ram;
} snap_t;

static snap_t snap[3];
static jit_codegen_stats_t arm_stats[3];
static uint64_t exits_seen[8]; /* accumulated over every compiled arm's run */
static uint64_t arm_invalidations[3];
/* S3 (round 2): which arm DECODED is the evidence for the last-page test -- the window fix
 * changed the engine a guest at the top page gets, so the counters have to be visible. */
static uint64_t arm_decodes[3];
static uint64_t arm_block_ends[3];
static uint64_t arm_flushes[3];
static bool jit_inited;

/* the per-test machine setup, applied to every arm alike (after machine_reset and the IDT) */
static void (*arm_setup)(void);

/* arm 0 = the interpreter (--uop-cache off), 1 = the uop cache, 2 = the uop cache + the JIT.
 * `code` is the virtual address the image is loaded at (CODE for every program but the
 * page-crossing one, which needs its first byte on the page's last two). */
static void arm_run(int arm, uint32_t code, const uint8_t *prog, int len, uint32_t budget)
{
	machine_reset();
	build_idt_stubs();
	if (arm_setup != NULL) arm_setup();
	if (len > 0) load(code, prog, len);

	uop_cache_enabled = arm != 0;
	jit_enabled = arm == 2;
	/* the LOOP_COUNTER window's end (cpu.h): do_many_cycles_native publishes it, and this
	 * harness does not, so it is set OUT OF THE WAY here -- otherwise the flush arm's budget
	 * test fires on the first batch end (counter >= 0) in BOTH engines, which is a legitimate
	 * but uninteresting shape to compare. */
	cpu_cycle_budget_end = 0x40000000u;
	if (arm == 2 && !jit_inited) {
		CHECK(jit_mem_init());
		jit_inited = true;
	}
	memset(&jit_codegen_stats, 0, sizeof jit_codegen_stats);
	uop_stats_reset();

	tsc_warm();
	cpu->instruction_counter = 0;
	/* S3: through the fetch-cursor store -- `(int32_t)code` was a copy of the old int32 field
	 * and would SIGN-EXTEND a code address above 0x7FFFFFFF into the 64-bit field, which is a
	 * high half the machine's own invariant stops on (the last-page test sits exactly there) */
	set_instruction_pointer(code);
	cpu->previous_ip = (uint64_t)code;
	cpu->last_virt_eip = VIRT_EIP_NONE;
	run_machine(budget);

	snap_t *s = &snap[arm];
	memcpy(&s->cpu, cpu, sizeof(cpu_state_t));
	s->pending = uop_pending;
	s->vec = mem8[VEC_RECORD];
	memcpy(s->ram, mem8, SNAP_RAM);
	arm_stats[arm] = jit_codegen_stats;
	if (arm == 2) {
		for (int k = 0; k < 8; k++) exits_seen[k] += jit_codegen_stats.exits[k];
	}
	arm_invalidations[arm] = jit_stats.invalidations;
	arm_decodes[arm] = uop_stats.decodes;
	arm_block_ends[arm] = uop_stats.block_ends;
	arm_flushes[arm] = uop_stats.flushes;

	uop_cache_enabled = true;
	jit_enabled = false;
}

/* the readable fields first, then the whole struct: a memcmp failure must NAME something */
static void cmp_state(int a, int b, const char *what)
{
	const cpu_state_t *x = &snap[a].cpu, *y = &snap[b].cpu;
	for (int i = 0; i < 8; i++) {
		if (x->reg64[i] != y->reg64[i]) {
			fprintf(stderr, "  %s: reg64[%d] 0x%llx vs 0x%llx\n", what, i,
			        (unsigned long long)x->reg64[i], (unsigned long long)y->reg64[i]);
		}
	}
	CHECK_EQ(x->instruction_pointer, y->instruction_pointer);
	CHECK_EQ(x->previous_ip, y->previous_ip);
	CHECK_EQ(x->instruction_counter, y->instruction_counter);
	CHECK_EQ(x->flags, y->flags);
	CHECK_EQ(x->last_op1, y->last_op1);
	CHECK_EQ(x->last_result, y->last_result);
	CHECK_EQ(x->flags_changed, y->flags_changed);
	CHECK_EQ(x->state_flags, y->state_flags);
	CHECK_EQ(snap[a].pending, snap[b].pending);
	CHECK_EQ(snap[a].vec, snap[b].vec);
	CHECK_EQ(memcmp(x, y, sizeof(cpu_state_t)), 0);
	CHECK_EQ(memcmp(snap[a].ram, snap[b].ram, SNAP_RAM), 0);
	if (memcmp(x, y, sizeof(cpu_state_t)) != 0) {
		const uint8_t *px = (const uint8_t *)x, *py = (const uint8_t *)y;
		for (size_t i = 0; i < sizeof(cpu_state_t); i++) {
			if (px[i] != py[i]) {
				fprintf(stderr, "  %s: cpu_state differs first at offset %zu: 0x%02x vs 0x%02x\n",
				        what, i, px[i], py[i]);
				break;
			}
		}
	}
}

/* ==================================================================== *
 * THE RECIPE, DIFFERENTIALLY
 *
 *     0x1000: 89 C8              mov eax, ecx      <- the RECIPE (inlined)
 *     0x1002: 89 D0              mov eax, edx      <- the RECIPE
 *     0x1004: 89 D9              mov ecx, ebx      <- the RECIPE
 *     0x1006: 89 C1              mov ecx, eax      <- the RECIPE
 *     0x1008: 90                 nop               <- CALLED
 *     0x1009: A3 00 30 00 00     mov [0x3000], eax <- CALLED
 *     0x100E: F4                 hlt               <- CALLED, and the block end
 *
 * The four moves make every register the recipe touches observable, and the expected values
 * are computed HERE (not only compared): eax = edx, ecx = eax = edx, ebx unchanged, and the
 * stored word = eax.
 * ==================================================================== */
/* the recipe's operands: a different value per register, so a swapped source/destination is
 * a DIFFERENT observable value and not a symmetric no-op */
static void setup_recipe(void)
{
	write_reg32(EAX, 0);
	write_reg32(ECX, 0x11111111);
	write_reg32(EDX, 0x22222222);
	write_reg32(EBX, 0x33333333);
}

static void test_recipe(void)
{
	fprintf(stderr, "  == test_recipe\n");
	static const uint8_t prog[] = {
		0x89, 0xC8,             /* mov eax, ecx */
		0x89, 0xD0,             /* mov eax, edx */
		0x89, 0xD9,             /* mov ecx, ebx */
		0x89, 0xC1,             /* mov ecx, eax */
		0x90,                   /* nop */
		0xA3, 0x00, 0x31, 0x00, 0x00, /* mov [0x3100], eax */
		0xF4,                   /* hlt */
		0xEB, 0xFD,             /* jmp hlt */
	};

	for (int arm = 0; arm < 3; arm++) {
		arm_setup = setup_recipe;
		arm_run(arm, CODE, prog, (int)sizeof prog, 100);
	}
	arm_setup = NULL;

	/* (a) THE PROGRAM'S OWN RESULT, in every arm */
	for (int arm = 0; arm < 3; arm++) {
		CHECK_EQ(snap[arm].cpu.reg64[EAX], 0x22222222);
		CHECK_EQ(snap[arm].cpu.reg64[ECX], 0x22222222);
		CHECK_EQ(snap[arm].cpu.reg64[EBX], 0x33333333);
		CHECK_EQ(ram32(TRACE), 0x22222222);
	}

	/* (b) THE ARMS AGREE -- the interpreter, the uop cache and the compiled block */
	cmp_state(0, 1, "interp vs uop-cache");
	cmp_state(1, 2, "uop-cache vs compiled");

	/* (c) THE COMPILED PATH WAS REALLY TAKEN.  Without this the whole file is theatre: a
	 * compiler that declined every block would pass every comparison above. */
	CHECK_EQ(arm_stats[2].blocks_compiled, 1);
	CHECK_EQ(arm_stats[2].compile_attempts, 1);
	CHECK_EQ(arm_stats[2].blocks_reused, 0);
	CHECK_EQ(arm_stats[2].refused_fit, 0);

	/* (d) THE COVERAGE COUNTERS AGREE WITH THE DESCRIPTORS COMPILED (brief G4).  M2 moved
	 * these numbers and the reason is the unit's own point: the four `89 /r` register moves
	 * AND the `90` nop now have recipes (the nop was called in M1), so five uops are inlined
	 * and two -- the `A3` moffs store and the hlt -- are called.  A register-only recipe
	 * cannot fail, so these two counts are EXACT and not TLB-dependent. */
	CHECK_EQ(arm_stats[2].uops_with_recipe, 5);
	CHECK_EQ(arm_stats[2].uops_without_recipe, 2);
	CHECK_EQ(arm_stats[2].inlined_uops, 5);
	CHECK_EQ(arm_stats[2].called_uops, 2);
	CHECK_EQ(arm_stats[1].blocks_compiled, 0); /* the jit-off arm compiled nothing */

	/* (e) AND THE KINDS: this program ends on the BLOCK_END path (hlt) */
	CHECK_EQ(arm_stats[2].exits[JIT_EXIT_BLOCK_END], 1);
	CHECK_EQ(arm_block_ends[2], arm_block_ends[1]);

	fprintf(stderr, "  recipe: 4 inlined + 3 called uops, block-end exit, state identical to "
	                "the uop cache and the interpreter\n");
}

/* ==================================================================== *
 * THE FAULT / EARLY-RETURN PIN (plan ss9 risk 2)
 *
 * TWO ways an instruction does NOT run, and both must leave the same state through the
 * compiled path as through the executor:
 *   (a) THE GUARD fails first.  `D8 05 00 01 00 00` is `fadd dword [0x100]`, an FPU mem form
 *       with UOP_DESC_TS_GUARD; with CR0.TS set task_switch_test raises #NM (7).  DS is ALSO
 *       null, so an implementation that materialised the address before testing the guard
 *       would deliver #GP (13) instead -- this is the guard-ORDER pin.
 *   (b) THE MATERIALISATION faults.  `8B 05 00 01 00 00` is `mov eax, [0x100]` (no guard) with
 *       DS null: get_seg raises #GP (13), the body does not run and its postfix does not run.
 * ==================================================================== */
static void setup_guard_fault(void)
{
	cpu->cr[0] |= 0x8; /* CR0.TS: the task-switch guard fails (misc_instr.c:658) */
	cpu->segment_is_null[DS] = true;
	cpu->segment_limits[DS] = 0xFFFFFFFFu;
}
static void setup_mem_fault(void)
{
	cpu->segment_is_null[DS] = true;
	cpu->segment_limits[DS] = 0xFFFFFFFFu;
}

static void test_fault_early_return(void)
{
	fprintf(stderr, "  == test_fault_early_return\n");
	static const uint8_t guard[] = {
		0xD8, 0x05, 0x00, 0x01, 0x00, 0x00, /* fadd dword [0x100] -- TS-guarded mem form */
		0xF4, 0xEB, 0xFD,
	};
	static const uint8_t memfault[] = {
		0x8B, 0x05, 0x00, 0x01, 0x00, 0x00, /* mov eax, [0x100] -- DS null */
		0xA3, 0x00, 0x30, 0x00, 0x00,
		0xF4, 0xEB, 0xFD,
	};

	arm_setup = setup_guard_fault;
	for (int arm = 0; arm < 3; arm++) arm_run(arm, CODE, guard, (int)sizeof guard, 50);
	CHECK_EQ(snap[0].vec, 7); /* #NM: the guard fails BEFORE the resolve */
	CHECK_EQ(snap[1].vec, 7);
	CHECK_EQ(snap[2].vec, 7);
	CHECK(arm_stats[2].inlined_uops + arm_stats[2].called_uops > 0); /* really compiled+run */
	cmp_state(0, 2, "guard-fault: interp vs compiled");
	cmp_state(1, 2, "guard-fault: uop-cache vs compiled");
	fprintf(stderr, "  fault (guard): vector %u, state identical (7 = #NM, 13 = #GP: the "
	                "guard-order pin)\n", (unsigned)snap[2].vec);

	static const uint8_t flushfault[] = {
		0x0F, 0xB0, 0x05, 0x00, 0x01, 0x00, 0x00, /* cmpxchg byte [0x100], al: FLUSH-flagged */
		0xF4, 0xEB, 0xFD,
	};

	arm_setup = setup_mem_fault;
	for (int arm = 0; arm < 3; arm++) arm_run(arm, CODE, memfault, (int)sizeof memfault, 50);
	CHECK_EQ(snap[0].vec, 13); /* #GP: the materialisation faulted */
	CHECK_EQ(snap[1].vec, 13);
	CHECK_EQ(snap[2].vec, 13);
	cmp_state(0, 2, "mem-fault: interp vs compiled");
	cmp_state(1, 2, "mem-fault: uop-cache vs compiled");
	fprintf(stderr, "  fault (materialise): vector %u, state identical\n", (unsigned)snap[2].vec);

	/* (c) THE `ran` PIN -- a FLUSH-flagged encoding whose materialisation FAULTED.
	 *
	 * The two ways a body does not run are indistinguishable everywhere in steps 6-11 EXCEPT
	 * step 7: a faulted instruction's postfix never ran, so the reference's batch does NOT end
	 * there and `ran &&` must suppress the flush (PERF-FIX-3/SHOULD-FIX-2b -- uop_exec used to
	 * return true for it).  `0F B0 05 00 01 00 00` is `cmpxchg byte [0x100], al`, whose
	 * descriptor carries UOP_DESC_FLUSH (uop_gen.c:1061, iflags 0x01), and with DS null its
	 * address materialisation raises #GP before the body -- so `uop_stats.flushes`, which both
	 * engines now maintain, counts exactly the difference: 1 with the rule, 2 without it.
	 * MEASURED by results/jit-1-mut-rig.py's "materialisation-fault check dropped" arm, which
	 * this pin exists to catch. */
	for (int arm = 1; arm < 3; arm++) arm_run(arm, CODE, flushfault, (int)sizeof flushfault, 50);
	CHECK_EQ(snap[1].vec, 13);
	CHECK_EQ(snap[2].vec, 13);
	CHECK(arm_flushes[1] >= 1);
	CHECK_EQ(arm_flushes[2], arm_flushes[1]);
	cmp_state(1, 2, "flush-fault (ran=false): uop-cache vs compiled");
	fprintf(stderr, "  fault (FLUSH-flagged + faulted): vector %u, flushes %llu in BOTH engines "
	                "(the batch did NOT end there)\n", (unsigned)snap[2].vec,
	        (unsigned long long)arm_flushes[2]);
	arm_setup = NULL;
}

/* ==================================================================== *
 * THE STAIRCASE PIN: the counter and pending, as the GUEST sees them
 *
 * The image reads the guest-visible clock (rdtsc) at three points spaced by the two things
 * that can end a reference batch in the middle of a block:
 *
 *   0x1000: 89 C8                 mov eax, ecx        (the recipe: NOT a batch end)
 *   0x1002: 0F 31                 rdtsc               (reading 1, mid-batch)
 *   0x1004: A3 00 31 00 00        mov [0x3100], eax
 *   0x1009: 0F B0 05 00 01 00 00  cmpxchg byte [0x100], al   (FLUSH: a batch end)
 *   0x1010: 0F 31                 rdtsc               (reading 2, after the flush)
 *   0x1012: A3 04 31 00 00        mov [0x3104], eax
 *   0x1017: FB                    sti                 (a BLOCK END that is NOT a batch end)
 *   0x1018: 0F 31                 rdtsc               (reading 3; `sti` dispatches the shadow
 *                                                      instruction inside its own body)
 *   0x101A: A3 08 31 00 00        mov [0x3108], eax
 *   0x101F: F4                    hlt
 *
 * The three readings, the final counter and uop_pending must be identical in all three arms --
 * that IS the staircase: the guest cannot tell which engine ran it.
 * ==================================================================== */
static void test_staircase(void)
{
	fprintf(stderr, "  == test_staircase\n");
	static const uint8_t prog[] = {
		0x89, 0xC8,
		0x0F, 0x31,
		0xA3, 0x00, 0x31, 0x00, 0x00,
		0x0F, 0xB0, 0x05, 0x00, 0x01, 0x00, 0x00,
		0x0F, 0x31,
		0xA3, 0x04, 0x31, 0x00, 0x00,
		0xFB,
		0x0F, 0x31,
		0xA3, 0x08, 0x31, 0x00, 0x00,
		0xF4,
		0xEB, 0xFD,
	};

	arm_setup = NULL;
	for (int arm = 0; arm < 3; arm++) arm_run(arm, CODE, prog, (int)sizeof prog, 200);

	uint32_t r[3][3];
	for (int arm = 0; arm < 3; arm++) {
		for (int i = 0; i < 3; i++) r[arm][i] = ram32(TRACE + 4u * (uint32_t)i);
	}
	for (int i = 0; i < 3; i++) {
		CHECK_EQ(r[0][i], r[1][i]);
		CHECK_EQ(r[1][i], r[2][i]);
		if (r[0][i] != r[2][i]) {
			fprintf(stderr, "  staircase: reading %d: interp %u cache %u compiled %u\n",
			        i, (unsigned)r[0][i], (unsigned)r[1][i], (unsigned)r[2][i]);
		}
	}
	/* the counter exactly, decoded from the guest's own reading (one instruction = 1000) */
	for (int i = 0; i < 3; i++) CHECK_EQ(r[2][i] / 1000u, r[0][i] / 1000u);

	cmp_state(0, 1, "staircase: interp vs uop-cache");
	cmp_state(1, 2, "staircase: uop-cache vs compiled");

	/* the flush counts are the executor's own tally of reproduced batch ends: the compiled
	 * path must reach exactly as many (the cmpxchg, the sti's shadow, the hlt, ...) */
	CHECK_EQ(arm_flushes[2], arm_flushes[1]);
	fprintf(stderr, "  staircase: rdtsc readings %u/%u/%u, counter %u, pending %u (identical "
	                "in all three arms; flushes %llu)\n",
	        (unsigned)r[2][0], (unsigned)r[2][1], (unsigned)r[2][2],
	        (unsigned)snap[2].cpu.instruction_counter, (unsigned)snap[2].pending,
	        (unsigned long long)arm_flushes[2]);
}

/* ==================================================================== *
 * THE PREFIX / previous_ip PIN
 *
 *   0x1000: 64 8B 05 00 14 00 00   mov eax, fs:[0x1400]   (the override lives in cpu->prefixes)
 *   0x1007: A3 00 31 00 00         mov [0x3100], eax
 *   0x100C: F4                     hlt
 *
 * FS's base is 0x40000 and DS's is 0, and the two addresses hold DIFFERENT values: a compiled
 * block that dropped the prefixes store reads the DS word instead (uop_seg_base reads
 * cpu->prefixes at execute time).  previous_ip is asserted directly as well as through the
 * full-state comparison.
 * ==================================================================== */
static void setup_prefix(void)
{
	cpu->segment_offsets[SS] = 0;
	cpu->segment_offsets[DS] = 0;
	cpu->segment_offsets[FS] = 0x40000;
	/* 0x1400 (DS) and 0x41400 (FS) hold different words; [0x3100] is the trace */
	uint32_t ds_word = 0x12345678u, fs_word = 0xA5A5A5A5u;
	memcpy(mem8 + 0x1400, &ds_word, 4);
	memcpy(mem8 + 0x41400, &fs_word, 4);
}

static void test_prefix_and_previous_ip(void)
{
	fprintf(stderr, "  == test_prefix_and_previous_ip\n");
	static const uint8_t prog[] = {
		0x64, 0x8B, 0x05, 0x00, 0x14, 0x00, 0x00, /* mov eax, fs:[0x1400] */
		0xA3, 0x00, 0x31, 0x00, 0x00,             /* mov [0x3100], eax */
		0xF4,                                     /* hlt */
		0xEB, 0xFD,
	};

	arm_setup = setup_prefix;
	for (int arm = 0; arm < 3; arm++) arm_run(arm, CODE, prog, (int)sizeof prog, 50);

	/* the FS word, NOT the DS one -- in every arm */
	for (int arm = 0; arm < 3; arm++) CHECK_EQ(ram32(TRACE), 0xA5A5A5A5u);
	cmp_state(0, 2, "prefix: interp vs compiled");
	cmp_state(1, 2, "prefix: uop-cache vs compiled");

	/* previous_ip, asserted directly: the LAST instruction to store it is the hlt uop at
	 * 0x100C (a BLOCK_END exit leaves it at that uop's eip in both engines) */
	CHECK_EQ(snap[1].cpu.previous_ip, 0x100C);
	CHECK_EQ(snap[2].cpu.previous_ip, 0x100C);
	fprintf(stderr, "  prefix: fs:[0x1400] override honoured (0x%08x), previous_ip 0x%x in "
	                "both engines\n", ram32(TRACE), (unsigned)snap[2].cpu.previous_ip);
	arm_setup = NULL;
}

/* ==================================================================== *
 * THE LAST VIRTUAL PAGE (S3 round 2, review S2)
 *
 * S3's decode-window fix made a block at 0xFFFFF000..0xFFFFFFFF DECODE where the old
 * `(virt_start | 0xFFF) + 1` in a uint32_t wrapped to 0 and refused every block there.  That
 * is a change in WHICH ENGINE runs a legal guest, so the claim to pin is not "the address
 * arithmetic is tidier" but "the guest still gets the same answer, in all three engines, and
 * the cached engines are the ones that now run it".
 *
 * The fixture: a 4 KiB page table mapping the LAST virtual page onto a physical page inside
 * guest RAM (a 32-bit guest reaches the top page the same way -- through paging; without it the
 * virtual address 0xFFFFFxxx is outside the model's 2 MiB of RAM and nothing can execute
 * there).  The program is `mov eax, 0x1234; hlt`, entirely inside that one page.
 *
 *   0x00008000  the page directory, entry 1023 -> the page table
 *   0x00009000  the page table, entry 0x3FF -> 0x00180000
 *   0x00180000  the code
 * ==================================================================== */
#define LASTPAGE_VIRT 0xFFFFF000u
#define LASTPAGE_PHYS 0x180000u
#define LASTPAGE_PD   0x8000u
#define LASTPAGE_PT   0x9000u

static void setup_last_page(void)
{
	memset(mem8 + LASTPAGE_PD, 0, 0x2000); /* the PD and the PT, one 8 KiB block */
	for (uint32_t i = 0; i < 0x2000; i += 4096) {
		/* zeroed above; the entries below are the only mappings */
	}

	static const uint8_t prog[] = { 0xB8, 0x34, 0x12, 0x00, 0x00, 0xF4 }; /* mov eax,0x1234; hlt */
	memcpy(mem8 + LASTPAGE_PHYS, prog, sizeof prog);

	/* PD[1023] (the 4 MiB region 0xFFC00000..0xFFFFFFFF) -> the page table, present|rw */
	uint32_t pde = LASTPAGE_PT | 3u;
	memcpy(mem8 + LASTPAGE_PD + 1023u * 4u, &pde, 4);
	/* PT[0x3FF] (the page 0xFFFFF000) -> the code page, present|rw */
	uint32_t pte = LASTPAGE_PHYS | 3u;
	memcpy(mem8 + LASTPAGE_PT + 0x3FFu * 4u, &pte, 4);

	cpu->cr[4] = 0;                          /* the 2-level walk: PAE is a different one */
	cpu->cr[3] = (int32_t)LASTPAGE_PD;
	cpu->cr[0] = (int32_t)((uint32_t)cpu->cr[0] | 0x80000000u); /* CR0.PG */
	full_clear_tlb();
}

static void test_last_page(void)
{
	fprintf(stderr, "  == test_last_page (S3: a block at the LAST virtual page)\n");

	arm_setup = setup_last_page;
	for (int arm = 0; arm < 3; arm++) arm_run(arm, LASTPAGE_VIRT, NULL, 0, 20);
	arm_setup = NULL;

	/* the program ran and the three engines agree -- on the WHOLE state and on RAM */
	for (int arm = 0; arm < 3; arm++) CHECK_EQ(snap[arm].cpu.reg64[EAX], 0x1234ull);
	CHECK_EQ(snap[0].cpu.instruction_pointer, LASTPAGE_VIRT + 6u);
	cmp_state(0, 1, "last page: interp vs uop-cache");
	cmp_state(1, 2, "last page: uop-cache vs compiled");
	cmp_state(0, 2, "last page: interp vs compiled");

	/* WHICH ENGINE RAN is the thing this slice changed: the interpreter arm has the cache
	 * off (nothing decoded), and both cached arms must have DECODED a block at the top page
	 * instead of falling back -- which is exactly what the old window refused.  The compiled
	 * arm must also have COMPILED it, or `--jit on` at the last page would silently be the
	 * uop arm. */
	CHECK_EQ(arm_decodes[0], 0);
	CHECK(arm_decodes[1] >= 1);
	CHECK(arm_decodes[2] >= 1);
	CHECK(arm_stats[2].blocks_compiled >= 1);
	CHECK_EQ(uop_last_stop, UOP_STOP_BLOCK_END); /* the hlt ended the block, at the top page */

	fprintf(stderr, "  last page: decodes interp=%llu uop=%llu compiled=%llu, blocks compiled %llu;"
	                " eip 0x%llx in all three arms, whole-state compare clean\n",
	        (unsigned long long)arm_decodes[0], (unsigned long long)arm_decodes[1],
	        (unsigned long long)arm_decodes[2],
	        (unsigned long long)arm_stats[2].blocks_compiled,
	        (unsigned long long)snap[2].cpu.instruction_pointer);
}

/* ==================================================================== *
 * THE SMC ABORT PIN, WITH THE COMPILED BLOCK RUNNING (uop_exec_selftest.c's image)
 *
 *   0x1000: C7 05 0B 10 00 00 2A 00 00 00  mov [0x100B], 0x2A   (patches the NEXT instruction's
 *   0x100A: B8 00 00 00 00                 mov eax, 0            immediate, in ITS OWN PAGE)
 *   0x100F: A3 00 31 00 00                 mov [0x3100], eax
 *   0x1014: F4                             hlt
 *
 * All four are in ONE block.  The write must (a) unlink the page's blocks and (b) stop the
 * RUNNING compiled block at its next uop boundary, or the stale `mov eax, 0` executes and the
 * trace reads 0 instead of 0x2A.
 * ==================================================================== */
static void test_smc_abort(void)
{
	fprintf(stderr, "  == test_smc_abort\n");
	static const uint8_t prog[] = {
		0xC7, 0x05, 0x0B, 0x10, 0x00, 0x00, 0x2A, 0x00, 0x00, 0x00,
		0xB8, 0x00, 0x00, 0x00, 0x00,
		0xA3, 0x00, 0x31, 0x00, 0x00,
		0xF4,
		0xEB, 0xFD,
	};

	arm_setup = NULL;
	for (int arm = 1; arm < 3; arm++) arm_run(arm, CODE, prog, (int)sizeof prog, 100);

	CHECK_EQ(ram32(TRACE), 0x2A);            /* the compiled path ran the PATCHED instruction */
	CHECK(arm_invalidations[2] >= 1);        /* the page's blocks were unlinked */
	CHECK(arm_stats[2].exits[JIT_EXIT_ABORT] >= 1); /* and the running block was ABORTED */
	cmp_state(1, 2, "smc: uop-cache vs compiled");
	fprintf(stderr, "  smc: patched instruction executed under the compiled block "
	                "(value 0x%02x, aborts %llu, invalidations %llu)\n", ram32(TRACE),
	        (unsigned long long)arm_stats[2].exits[JIT_EXIT_ABORT],
	        (unsigned long long)arm_invalidations[2]);
}

/* ==================================================================== *
 * THE TAIL EXIT (control flow leaving the block: the reference's loop tail)
 *
 *   0x1000: 75 01        jnz +1     (taken: ZF is clear at reset)
 *   0x1002: F4           hlt        (skipped by the jump)
 *   0x1003: B8 05 00 00 00      mov eax, 5
 *   0x1008: A3 00 31 00 00      mov [0x3100], eax
 *   0x100D: F4           hlt
 *
 * The block is {jnz, hlt}; the taken jump makes the instruction pointer leave it, which is the
 * executor's divergence test -> TAIL (not a batch end: the reference's batch continues).
 * ==================================================================== */
static void test_tail_exit(void)
{
	fprintf(stderr, "  == test_tail_exit\n");
	static const uint8_t prog[] = {
		0x75, 0x01,
		0xF4,
		0xB8, 0x05, 0x00, 0x00, 0x00,
		0xA3, 0x00, 0x31, 0x00, 0x00,
		0xF4,
		0xEB, 0xFD,
	};

	arm_setup = NULL;
	for (int arm = 1; arm < 3; arm++) arm_run(arm, CODE, prog, (int)sizeof prog, 100);

	CHECK_EQ(ram32(TRACE), 5);
	CHECK_EQ(snap[2].cpu.reg64[EAX], 5);
	CHECK(arm_stats[2].exits[JIT_EXIT_TAIL] >= 1);
	cmp_state(1, 2, "tail: uop-cache vs compiled");
	fprintf(stderr, "  tail: control left the block, TAIL exits %llu\n",
	        (unsigned long long)arm_stats[2].exits[JIT_EXIT_TAIL]);
}

/* ==================================================================== *
 * THE RETURN EXIT BY A PAGE CROSSING (the reference's second batch-end clause)
 *
 *   0x1FFE: EB 00              jmp +0: the target 0x2000 is on the NEXT page
 *   0x2000: B8 05 00 00 00     mov eax, 5
 *   0x2005: A3 00 31 00 00     mov [0x3100], eax
 *   0x200A: F4                 hlt
 *
 * The jump is a block end, so the block is {jmp} alone; the instruction MOVED to another page,
 * which is the reference's page-cross batch end -- flush + RETURN, not TAIL.
 * ==================================================================== */
static void test_cross_return(void)
{
	fprintf(stderr, "  == test_cross_return\n");
	/* loaded at 0x1FFE: the jmp occupies the page's last two bytes and its target (0x2000) is
	 * the first byte of the NEXT page */
	static const uint8_t prog[] = {
		0xEB, 0x00,
		0xB8, 0x05, 0x00, 0x00, 0x00,
		0xA3, 0x00, 0x31, 0x00, 0x00,
		0xF4,
		0xEB, 0xFD,
	};
	arm_setup = NULL;
	for (int arm = 1; arm < 3; arm++) arm_run(arm, 0x1FFEu, prog, (int)sizeof prog, 100);
	CHECK_EQ(ram32(TRACE), 5);
	CHECK(arm_stats[2].exits[JIT_EXIT_RETURN] >= 1);
	cmp_state(1, 2, "cross-return: uop-cache vs compiled");
	fprintf(stderr, "  page-cross return: RETURN exits %llu\n",
	        (unsigned long long)arm_stats[2].exits[JIT_EXIT_RETURN]);
}

/* ==================================================================== *
 * ALL FOUR EXIT KINDS, IN ONE PLACE (the brief's G1: "the exit kinds round-trip")
 * ==================================================================== */
static void test_exit_kinds(void)
{
	fprintf(stderr, "  == test_exit_kinds\n");
	/* every kind is produced by one of the programs above, and counted where uop_run reads
	 * the block's return value -- so this is a measurement of the emits, not a claim:
	 *   RETURN     -- test_cross_return  (the page-cross batch end)
	 *   TAIL       -- test_tail_exit     (control left the block)
	 *   ABORT      -- test_smc_abort     (the write landed on the running block's page)
	 *   BLOCK_END  -- test_recipe        (hlt) */
	CHECK(exits_seen[JIT_EXIT_RETURN] >= 1);
	CHECK(exits_seen[JIT_EXIT_TAIL] >= 1);
	CHECK(exits_seen[JIT_EXIT_ABORT] >= 1);
	CHECK(exits_seen[JIT_EXIT_BLOCK_END] >= 1);
	fprintf(stderr, "  exit kinds: return %llu, tail %llu, abort %llu, block-end %llu\n",
	        (unsigned long long)exits_seen[JIT_EXIT_RETURN],
	        (unsigned long long)exits_seen[JIT_EXIT_TAIL],
	        (unsigned long long)exits_seen[JIT_EXIT_ABORT],
	        (unsigned long long)exits_seen[JIT_EXIT_BLOCK_END]);
}



/* ==================================================================== *
 * THE SHAPE MATRIX: the CALLED path's argument mapping, one uop per SHAPE family
 *
 * THE CALLED PATH IS THE FALLBACK EVERY UOP EXCEPT THE RECIPE TAKES, and the shape decides HOW
 * the body is called -- `fn(modrm & 7, g)`, `fn(addr, imm)`, `fn(imm, imm2)`, ... A shape whose
 * argument mapping is wrong is SILENTLY wrong, and nothing else in this file would see it.  So
 * these two programs put one instruction of each shape family (READ from src/cpu/uop_gen.c's
 * descriptor flags: 0x01 modrm, 0x05 modrm+fixed_g, 0x40 imm, 0x45 modrm+fixed_g+imm,
 * 0x09 modrm+custom_modrm_resolve) into a compiled block, assert the VALUES the program must
 * produce, and compare the full state with the uop path and the interpreter.
 *
 * PROGRAM A -- the register and plain forms:
 *   0x1000: 98              cwde            PLAIN0   (eax = sign_extend16(ax))
 *   0x1001: 87 C8           xchg eax, ecx   REG0     (modrm, no fixed_g, no imm)
 *   0x1003: 83 C0 05        add eax, 5      REGF1    (fixed_g, imm8; lazy flags)
 *   0x1006: FF C2           inc edx         REGF0    (fixed_g, no imm)
 *   0x1008: 0F B6 C1        movzx eax, cl   REG0     (a byte-register read)
 *   0x100B: A1 00 31 00 00  mov eax,[0x3100] PLAIN1  (the moffs form: a memory read, no modrm)
 *   0x1010: A3 04 31 00 00  mov [0x3104],eax PLAIN1
 *   0x1015: F4              hlt
 *
 * PROGRAM B -- the memory forms, including the two that are NOT a plain address:
 *   0x1000: 8B 05 00 31 00 00              mov eax,[0x3100]        MEM0
 *   0x1005: C7 05 04 31 00 00 78 56 34 12  mov [0x3104],imm32      MEMF1
 *   0x100F: 8D 0D 00 31 00 00              lea ecx,[0x3100]        MEM0 + custom_modrm_resolve
 *   0x1015: FF 05 08 31 00 00              inc dword [0x3108]      MEMF0 (an RMW STORE)
 *   0x101B: F4                             hlt
 *
 * The `lea` is the one that pins the RAW-MODRM substitution (uop_cache.c's uop_exec: a
 * custom_modrm_resolve body is handed the modrm byte and reads the sib/disp itself) and the
 * `inc dword [mem]` is a store that goes through SAFE_READ_WRITE32 -- the same family
 * PERF-FIX-3 re-hooked, executed here through a CALLED body under a compiled block.
 * ==================================================================== */
static void setup_shapes_a(void)
{
	uint32_t v = 0xDEADBEEFu; /* the moffs load's target, on page 3 (NOT the code page) */
	memcpy(mem8 + 0x3100, &v, 4);
	write_reg32(EAX, 0x00008000); /* so cwde has something to sign-extend */
	write_reg32(ECX, 0x11111111);
	write_reg32(EDX, 0x22222222);
}
static void setup_shapes_b(void)
{
	uint32_t v = 0xDEADBEEFu, n = 41;
	memcpy(mem8 + 0x3100, &v, 4);
	memcpy(mem8 + 0x3108, &n, 4);
}

static void test_shape_matrix(void)
{
	static const uint8_t prog_a[] = {
		0x98,
		0x87, 0xC8,
		0x83, 0xC0, 0x05,
		0xFF, 0xC2,
		0x0F, 0xB6, 0xC1,
		0xA1, 0x00, 0x31, 0x00, 0x00,
		0xA3, 0x04, 0x31, 0x00, 0x00,
		0xF4, 0xEB, 0xFD,
	};
	static const uint8_t prog_b[] = {
		0x8B, 0x05, 0x00, 0x31, 0x00, 0x00,
		0xC7, 0x05, 0x04, 0x31, 0x00, 0x00, 0x78, 0x56, 0x34, 0x12,
		0x8D, 0x0D, 0x00, 0x31, 0x00, 0x00,
		0xFF, 0x05, 0x08, 0x31, 0x00, 0x00,
		0xF4, 0xEB, 0xFD,
	};

	arm_setup = setup_shapes_a;
	for (int arm = 0; arm < 3; arm++) arm_run(arm, CODE, prog_a, (int)sizeof prog_a, 100);
	for (int arm = 0; arm < 3; arm++) {
		/* the values, not only the agreement between arms */
		/* compared as 32-BIT patterns: CHECK_EQ's long long cast would sign-extend them */
		CHECK_EQ((uint32_t)snap[arm].cpu.reg64[ECX], 0xFFFF8000u); /* cwde then xchg */
		CHECK_EQ((uint32_t)snap[arm].cpu.reg64[EAX], 0xDEADBEEFu); /* the moffs load */
		CHECK_EQ((uint32_t)snap[arm].cpu.reg64[EDX], 0x22222223u); /* inc edx */
		CHECK_EQ(ram32(TRACE + 4), 0xDEADBEEFu);                   /* the moffs store */
	}
	/* M2: the program is no longer the CALLED path end to end -- `83 C0 05` (add eax, 5) and
	 * `0F B6 C1` (movzx eax, cl) are recipes now.  Both are register-only, so both are
	 * inlined exactly once; the other six uops (cwde, xchg, the two moffs forms, the `FF /0`
	 * inc, hlt) are CALLED, which is the half of this pin that still matters: the compiled
	 * block must keep taking the C path for everything it has no recipe for. */
	CHECK_EQ(arm_stats[2].blocks_compiled, 1);
	CHECK_EQ(arm_stats[2].uops_with_recipe, 2);
	CHECK_EQ(arm_stats[2].uops_without_recipe, 6);
	CHECK_EQ(arm_stats[2].inlined_uops, 2);
	CHECK_EQ(arm_stats[2].called_uops, 6);
	cmp_state(0, 2, "shapes A: interp vs compiled");
	cmp_state(1, 2, "shapes A: uop-cache vs compiled");
	fprintf(stderr, "  shapes A (PLAIN0/1, REG0 x2, REGF0/1): eax 0x%08x ecx 0x%08x edx 0x%08x, "
	                "state identical to both other arms\n", (unsigned)snap[2].cpu.reg64[EAX],
	        (unsigned)snap[2].cpu.reg64[ECX], (unsigned)snap[2].cpu.reg64[EDX]);

	arm_setup = setup_shapes_b;
	for (int arm = 0; arm < 3; arm++) arm_run(arm, CODE, prog_b, (int)sizeof prog_b, 100);
	for (int arm = 0; arm < 3; arm++) {
		CHECK_EQ((uint32_t)snap[arm].cpu.reg64[EAX], 0xDEADBEEFu); /* MEM0 */
		CHECK_EQ(ram32(TRACE + 4), 0x12345678u);                   /* MEMF1 */
		CHECK_EQ((uint32_t)snap[arm].cpu.reg64[ECX], 0x3100u);     /* lea: the raw-modrm path */
		CHECK_EQ(ram32(TRACE + 8), 42);                            /* MEMF0: the RMW store ran */
	}
	/* M2: `8B /r` (the load) and `8D /r` (the lea) are recipes now, so the compile-time split
	 * is 2 inlined-capable / 3 called.  The RUNTIME split is deliberately NOT pinned to an
	 * exact number here: a memory recipe's fast path gates on the TLB, and this program's
	 * first access may or may not find the entry an earlier test warmed -- so the exact count
	 * is a function of the file's test order, and pinning it would be pinning the order.  What
	 * IS pinned: the lea (a register-only recipe) always runs inline, and the two totals add
	 * up. */
	CHECK_EQ(arm_stats[2].blocks_compiled, 1);
	CHECK_EQ(arm_stats[2].uops_with_recipe, 2);
	CHECK_EQ(arm_stats[2].uops_without_recipe, 3);
	CHECK_EQ(arm_stats[2].inlined_uops + arm_stats[2].called_uops, 5);
	CHECK(arm_stats[2].inlined_uops >= 1);
	cmp_state(0, 2, "shapes B: interp vs compiled");
	cmp_state(1, 2, "shapes B: uop-cache vs compiled");
	fprintf(stderr, "  shapes B (MEM0, MEMF1, lea/custom-modrm, MEMF0/RMW store): eax 0x%08x "
	                "ecx 0x%x, [0x3104] 0x%08x [0x3108] %u, state identical\n",
	        (unsigned)snap[2].cpu.reg64[EAX], (unsigned)snap[2].cpu.reg64[ECX],
	        ram32(TRACE + 4), ram32(TRACE + 8));
	arm_setup = NULL;
}

/* ==================================================================== *
 * THE FOLDED TLB MASK BITS (JIT-FIX-1 should-fix 4)
 *
 * jitcg_emit_mem_prep computes ONE mask at compile time:
 *
 *     TLB_VALID | (state & CACHED_MASK_CPL3 ? TLB_NO_USER : 0) | (write ? TLB_READONLY : 0)
 *
 * and the C reference (paging.c:285-286) tests the same three bits against the entry the TLB
 * walk stored (paging.c:255-256: a READ's entry carries TLB_READONLY, a WRITE's does not).
 *
 * THE READONLY BIT IS PINNED HERE, AND THE PIN IS OBSERVABLE WITHOUT PAGING: `tlb_data` IS
 * NOT PART OF cpu_state_t (paging.h:33), so the full-state differential cannot see it -- but
 * it is EXPORTED state, and the walk's rewrite of it is the fingerprint of the decision.  A
 * read of the page stores TLB_READONLY; if the compiled write's mask carries TLB_READONLY too
 * (as it must) the write's fast path is DECLINED, the C body walks, and the bit is CLEARED
 * again.  A mask missing the bit takes the fast path and leaves it SET, with the guest-visible
 * result IDENTICAL -- so this is the one place the difference exists to be seen.
 *
 * THE cpl3 BIT IS NOT PINNED, AND THAT IS A MEASUREMENT, NOT AN OMISSION: with paging off --
 * the only mode this harness has -- `allow_user` is unconditionally true (paging.c:136; only
 * the PG-on arm can clear it), so TLB_NO_USER is NEVER SET IN ANY ENTRY, and both the C test
 * and the compiled mask are then insensitive to the bit: a nominal cpl==3 case here would pass
 * with the bit deleted.  Pinning it needs a PG-on fixture (a page directory + tables, one
 * supervisor page, CR3, CR0.PG) plus a cpl3 guest -- new fixture work in a suite that has no
 * paging anywhere, deferred and named in results/jit-fix-1.md rather than faked with a test
 * that cannot fail.
 * ==================================================================== */
static void setup_tlb_fold(void)
{
	uint32_t v = 0xDEADBEEFu;
	memcpy(mem8 + TRACE, &v, 4);
	write_reg32(EAX, 0x11111111);
	write_reg32(ECX, 0xA5A5A5A5);
	write_reg32(EDX, 0x22222222);
	/* INVLPG, by hand: the entry must MISS so that the first access really walks (the walk
	 * is what stores the bits this test then reads back). */
	tlb_data[TRACE >> 12] = 0;
}

static void test_tlb_fold_bits(void)
{
	static const uint8_t prog[] = {
		0x8B, 0x05, 0x00, 0x31, 0x00, 0x00, /* mov eax, [0x3100]  READ: the entry MISSES,
		                                     * so this one declines and the C walk stores
		                                     * ...|TLB_READONLY */
		0x89, 0x0D, 0x00, 0x31, 0x00, 0x00, /* mov [0x3100], ecx  WRITE: the mask must now
		                                     * REJECT that entry, so this declines too and
		                                     * its walk re-stores the bit clear */
		0x8B, 0x15, 0x00, 0x31, 0x00, 0x00, /* mov edx, [0x3100]  READ: the entry is clean
		                                     * now, so THIS one must be inlined -- the arm
		                                     * that proves the compiled path ran at all */
		0xF4, 0xEB, 0xFD,
	};

	fprintf(stderr, "  == test_tlb_fold_bits (the TLB_READONLY fold)\n");
	arm_setup = setup_tlb_fold;
	for (int arm = 0; arm < 3; arm++) arm_run(arm, CODE, prog, (int)sizeof prog, 100);
	arm_setup = NULL;

	cmp_state(0, 2, "tlb fold: interp vs compiled");
	cmp_state(1, 2, "tlb fold: uop-cache vs compiled");
	for (int arm = 0; arm < 3; arm++) {
		CHECK_EQ((uint32_t)snap[arm].cpu.reg64[EAX], 0xDEADBEEFu); /* the first read */
		CHECK_EQ((uint32_t)snap[arm].cpu.reg64[EDX], 0xA5A5A5A5u); /* the third: after the write */
	}
	CHECK_EQ(ram32(TRACE), 0xA5A5A5A5u); /* the write landed, in every arm */
	/* THE PIN: after a read AND a write of the same page the entry must be VALID and NOT
	 * READONLY -- i.e. the write re-walked.  Read once, before the assert, to say what it
	 * is: an assertion failure that prints both bits is worth more than a boolean. */
	uint32_t entry = (uint32_t)tlb_data[TRACE >> 12];
	fprintf(stderr, "  tlb fold: entry after read+write = 0x%08x (want VALID set, READONLY "
	                "clear)\n", entry);
	CHECK_EQ(entry & (TLB_VALID | TLB_READONLY), TLB_VALID);
	/* AND THE DECISIONS THEMSELVES: the two cold/read-only accesses MUST have declined the
	 * fast path (that declining IS the correct behaviour), and the third MUST have inlined
	 * -- without which the case would pass with the compiled path never taken. */
	CHECK(arm_stats[2].blocks_compiled >= 1);
	CHECK(arm_stats[2].called_uops >= 2);
	CHECK(arm_stats[2].inlined_uops >= 1);
	fprintf(stderr, "  tlb fold: inlined %llu, called %llu (the read-only write DECLINED), "
	                "entry 0x%08x\n", (unsigned long long)arm_stats[2].inlined_uops,
	        (unsigned long long)arm_stats[2].called_uops, entry);
}

/* ==================================================================== *
 * THE PER-RECIPE DIFFERENTIAL MATRIX (the brief's G2, plan ss6 gate 1)
 *
 * The M1 suite compares a handful of hand-written programs across the three arms.  M2 adds a
 * RECIPE PER ENCODING FAMILY, and the thing that has to be true is per-recipe: "this recipe
 * computes what its C body computes".  So each case below is a program whose interesting part
 * is ONE instruction (or one instantiation of a family), run through all three arms, with the
 * FULL cpu_state compared against the INTERPRETER (arm 0) -- which is the reference, not
 * another copy of the JIT.
 *
 * TWO RULES make the matrix able to fail:
 *
 *  (a) A case asserts that at least one uop was INLINED.  Without it a case passes vacuously
 *      when the recipe is missing or refused -- the "seat that is never called" failure.
 *  (b) Each case runs the instruction TWICE.  A memory recipe's fast path gates on the TLB,
 *      and the first access to a page in a fresh machine is a miss (the C path takes it, which
 *      is correct but would leave the case with inlined == 0); the second access is the one
 *      that must be inlined, so `repeat = 2` is what makes the assertion about the RECIPE
 *      rather than about the TLB.  Register-only recipes are unaffected by repeating.
 *
 * WHAT IT FOUND: this matrix is what localised the AH/CH/DH/BH bug -- a 16-bit-mode `mov ah,al`
 * wrote the fourth register slot's high byte (ESP's) instead of the first's, which corrupts
 * the guest's stack
 * pointer and made the boot firmware print one message forever.  The M0 transcript and the
 * 1 M-instruction boot gate BOTH passed while it was live; only a case that uses a high-byte
 * register sees it.
 * ==================================================================== */

/* a register/memory state every case starts from: every guest register distinguishable, the
 * flags materialised (flags_changed == 0, so the getters' non-lazy arm is what runs), and a
 * data word at each width in a page that is NOT a code page. */
static void setup_matrix(void)
{
	write_reg32(EAX, 0x0000C0DE);
	write_reg32(ECX, 0x0000C0DE); /* equal, so `cmp` sets ZF */
	write_reg32(EDX, 0x12345678);
	write_reg32(EBX, 0x00003000);
	write_reg32(ESP, 0x0000FF00);
	write_reg32(EBP, 0x0000FF80);
	write_reg32(ESI, 0x0000FFF0);
	write_reg32(EDI, 0x000021000);
	{
		uint32_t w32 = 0xDEADBEEF, w2 = 0x00001234;
		uint16_t w16 = 0x1234;
		memcpy(mem8 + 0x3100, &w32, 4);
		memcpy(mem8 + 0x3104, &w16, 2);
		memcpy(mem8 + 0x3106, &w2, 4);
	}
}

/* a lazy flag state: `cmp eax, edx` leaves last_* set and flags_changed != 0, so the getters'
 * LAZY arm runs -- the two getters' arms are separate code paths and both must be exercised. */
static void setup_matrix_lazy(void)
{
	setup_matrix();
	write_reg32(EAX, 0x00000010);
	write_reg32(EDX, 0x00000020);
	flags_sub(0x10, 0x20, OPSIZE_32);
}

/* THE HIGH-BYTE REGISTER STATE (JIT-FIX-1): every one of the four AH/CH/DH/BH registers is
 * distinguishable AND every one of their 32-bit hosts has a NON-ZERO high half, which is the
 * only thing that makes the high-byte helpers' mask observable at all.
 *
 * WHY NOT setup_matrix.  Its EAX/EBX/ECX high halves are ZERO, so `movzx eax, ah` returns
 * `eax >> 8` masked and unmasked ALIKE -- the dead mask was invisible to all 13 of its cases,
 * including the four that already use a high byte (`mov al,ah`, `mov ah,al`, `mov ah,cl`,
 * `mov ah,cl` at 0x88 /r).  MEASURED: this setup is the difference between a matrix that finds
 * the JIT-FIX-1 blocker and one that does not.
 *
 * The values are ALSO chosen so the corruption LANDS: 0x1111 in EAX and 0x2222 in EBX both map
 * to a single OR, and 0x11 | 0x22 == 0x33 != 0x11, so the leaked bits cannot cancel in the
 * comparison.  That is not a nicety: the first probe written for this unit used 0x9ABCDEF0 in
 * EAX and 0x12345678 in EBX and `mov ah, bh` came out CORRECT pre-fix, because 0x12's bits are a
 * SUBSET of 0x9A's and the leak left the destination unchanged (results/jit-fix-1.md ss2). */
static void setup_matrix_highbyte(void)
{
	setup_matrix();
	write_reg32(EAX, 0x1111C0DE); /* AH = 0xC0, AL = 0xDE */
	write_reg32(EBX, 0x22223456); /* BH = 0x34, BL = 0x56 */
	write_reg32(ECX, 0x4444A5B6); /* CH = 0xA5, CL = 0xB6 */
	write_reg32(EDX, 0x8888AB12); /* DH = 0xAB, DL = 0x12 */
}

/* THE SHIFT STATE (PERF-7 F2): destination and count both controlled, and the values chosen
 * so the flag results are NON-TRIVIAL -- a shift whose CF and OF both land 0 exercises the
 * or-ing below the same way a shift whose inputs were zero would (the arithmetic passes for
 * the trivial reason).  EAX = 0x1A345678: `shl eax,cl` at CL=5 takes CF from bit 27 (set)
 * and OF = 1 ^ sign(result) = 1; `shr eax,cl` takes CF from bit 4 (set) and OF = 0.
 * EDX = 0x88000010: both directions of `shl/shr edx,cl` produce CF = 1 AND OF = 1, from a
 * NEGATIVE original (the sign-extended >> arith.c's shr-OF reads). */
static void setup_matrix_shift(void)
{
	setup_matrix();
	write_reg32(EAX, 0x1A345678);
	write_reg32(ECX, 0x00000005); /* CL = 5 */
	write_reg32(EDX, 0x88000010);
}

static void recipe_case(const char *name, const uint8_t *body, int blen, void (*setup)(void),
                        int repeat)
{
	uint8_t prog[128];
	int n = 0;
	for (int r = 0; r < repeat; r++) {
		memcpy(prog + n, body, (size_t)blen);
		n += blen;
	}
	prog[n++] = 0xF4;       /* hlt */
	prog[n++] = 0xEB;       /* jmp $ */
	prog[n++] = 0xFD;

	for (int arm = 0; arm < 3; arm++) {
		arm_setup = setup;
		arm_run(arm, CODE, prog, n, 200);
	}
	arm_setup = NULL;

	cmp_state(0, 2, name);
	cmp_state(1, 2, name);

	if (arm_stats[2].blocks_compiled < 1) {
		fprintf(stderr, "FAIL %s: no block was compiled\n", name);
		checks++;
		failures++;
		return;
	}
	if (arm_stats[2].inlined_uops < 1) {
		fprintf(stderr, "FAIL %s: NOTHING WAS INLINED (blocks %llu compiled, %llu with a "
		                "recipe, %llu without, inlined %llu called %llu)\n", name,
		        (unsigned long long)arm_stats[2].blocks_compiled,
		        (unsigned long long)arm_stats[2].uops_with_recipe,
		        (unsigned long long)arm_stats[2].uops_without_recipe,
		        (unsigned long long)arm_stats[2].inlined_uops,
		        (unsigned long long)arm_stats[2].called_uops);
		checks++;
		failures++;
		return;
	}
	checks++;
	/* the case's own traffic: report it, so a reader can see the recipes working */
	fprintf(stderr, "  matrix: %-28s inlined %llu called %llu\n", name,
	        (unsigned long long)arm_stats[2].inlined_uops,
	        (unsigned long long)arm_stats[2].called_uops);
}

static void test_recipe_matrix(void)
{
	fprintf(stderr, "  == test_recipe_matrix (per-recipe differential, G2)\n");

	/* --- data movement, register and memory, every width --- */
	{
		static const uint8_t c1[] = { 0x89, 0xC8 };             /* mov eax, ecx */
		static const uint8_t c2[] = { 0x8B, 0xC1 };             /* mov eax, ecx */
		static const uint8_t c3[] = { 0x0F, 0xB6, 0xC1 };       /* movzx eax, cl */
		static const uint8_t c4[] = { 0x0F, 0xB7, 0xC1 };       /* movzx eax, cx */
		static const uint8_t c5[] = { 0x8A, 0xC1 };             /* mov al, cl */
		static const uint8_t c6[] = { 0x88, 0xE0 };             /* mov al, ah */
		static const uint8_t c7[] = { 0x88, 0xC4 };             /* mov ah, al */
		static const uint8_t c8[] = { 0x8A, 0xE1 };             /* mov ah, cl */
		static const uint8_t c9[] = { 0x88, 0xCC };             /* mov ah, cl */
		static const uint8_t c10[] = { 0x8D, 0x04, 0x4B };      /* lea eax, [ebx+ecx*2] */
		static const uint8_t c11[] = { 0x66, 0x89, 0xC8 };      /* mov ax, cx */
		static const uint8_t c12[] = { 0x66, 0x8B, 0xC1 };      /* mov ax, cx */
		static const uint8_t c13[] = { 0x8B, 0x04, 0x0D, 0x00, 0x31, 0x00, 0x00 }; /* mov eax,[ecx+0x3100] */
		recipe_case("mov r/m32,r32 reg", c1, sizeof c1, setup_matrix, 2);
		recipe_case("mov r32,r/m32 reg", c2, sizeof c2, setup_matrix, 2);
		recipe_case("movzx r32,r/m8 reg", c3, sizeof c3, setup_matrix, 2);
		recipe_case("movzx r32,r/m16 reg", c4, sizeof c4, setup_matrix, 2);
		recipe_case("mov r8,r/m8 reg (AL<-CL)", c5, sizeof c5, setup_matrix, 2);
		recipe_case("mov r/m8,r8 reg (AL<-AH)", c6, sizeof c6, setup_matrix, 2);
		recipe_case("mov r/m8,r8 reg (AH<-AL)", c7, sizeof c7, setup_matrix, 2);
		recipe_case("mov r8,r/m8 reg (AH<-CL)", c8, sizeof c8, setup_matrix, 2);
		recipe_case("mov r/m8,r8 reg (AH<-CL)", c9, sizeof c9, setup_matrix, 2);
		recipe_case("lea r32,m (base)", c10, sizeof c10, setup_matrix, 2);
		recipe_case("mov r/m16,r16 reg", c11, sizeof c11, setup_matrix, 2);
		recipe_case("mov r16,r/m16 reg", c12, sizeof c12, setup_matrix, 2);
		recipe_case("mov r32,[base+disp32]", c13, sizeof c13, setup_matrix, 2);
	}

	/* --- THE HIGH-BYTE REGISTERS (AH/CH/DH/BH), READ AND WRITTEN (JIT-FIX-1) ------
	 *
	 * WHY THIS BLOCK EXISTS, in one line: 118/118 nasm, the byte-identical M0 transcript
	 * and the identical 1.5 G-instruction boot ALL PASSED while `movzx r32, ah` computed a
	 * 24-bit value (jit_codegen.c's high-byte read emitted `83 /4 ib`, whose imm8 0xFF
	 * sign-extends to 0xFFFFFFFF, so the mask that was supposed to drop the bits the shift
	 * left behind did nothing).  MEASURED: nothing in the corpus's EXECUTED path has both a
	 * high-byte source and a non-zero upper half, so every existing gate was blind to it.
	 * These cases are the coverage that was missing, and they are the reason this can never
	 * come back silently.
	 *
	 * repeat = 1, NOT 2.  The matrix's rule (b) exists because a MEMORY recipe's first
	 * access misses the TLB and only the second is inlined; a register-only recipe is
	 * inlined on the first.  These cases WRITE their destination, so a second execution
	 * COMPOSES: `movzx eax, ah` twice ends at EAX = 0 (the first pass sets AH = 0), which is
	 * a value a broken JIT could also produce.  One execution is the value the encoding
	 * means, and it is asserted EXPLICITLY in all three arms -- the interpreter's own value
	 * is the reference, so a case cannot pass by agreeing with itself.
	 *
	 * h1..h4 are the READ path, h5..h7 and h9 the WRITE path.  EVERY case below except h8
	 * FAILS before the fix (MEASURED: results/jit-fix-1.md G2).  h8 (`mov ah, bl`) is
	 * deliberately kept even though it cannot fail for this bug: its source is a LOW byte,
	 * and the reader's low-byte arm is the one arm that was never broken -- so h8 is exactly
	 * the case that let the write direction be reported "correct" while `mov ah, bh` was
	 * wrong.  It pins the encoding; h5/h6/h7/h9 pin the mask, and jitcg_emit_write_reg8's
	 * header states the measured four-state relationship between the two helpers. */
	{
		static const uint8_t h1[] = { 0x0F, 0xB6, 0xC4 }; /* movzx eax, ah */
		static const uint8_t h2[] = { 0x0F, 0xB6, 0xFF }; /* movzx edi, bh */
		static const uint8_t h3[] = { 0x0F, 0xB6, 0xC5 }; /* movzx eax, ch */
		static const uint8_t h4[] = { 0x0F, 0xB6, 0xC6 }; /* movzx eax, dh */
		static const uint8_t h5[] = { 0x88, 0xFC };       /* mov ah, bh */
		static const uint8_t h6[] = { 0x88, 0xEC };       /* mov ah, ch */
		static const uint8_t h7[] = { 0x88, 0xE6 };       /* mov dh, ah */
		static const uint8_t h8[] = { 0x88, 0xDC };       /* mov ah, bl (low source) */
		static const uint8_t h9[] = { 0x8A, 0xEC };       /* mov ch, ah (0x8A entry) */

		recipe_case("movzx r32,ah (high-byte read)", h1, sizeof h1, setup_matrix_highbyte, 1);
		for (int arm = 0; arm < 3; arm++)
			CHECK_EQ((uint32_t)snap[arm].cpu.reg64[EAX], 0x000000C0u);
		recipe_case("movzx r32,bh (high-byte read)", h2, sizeof h2, setup_matrix_highbyte, 1);
		for (int arm = 0; arm < 3; arm++)
			CHECK_EQ((uint32_t)snap[arm].cpu.reg64[EDI], 0x00000034u);
		recipe_case("movzx r32,ch (high-byte read)", h3, sizeof h3, setup_matrix_highbyte, 1);
		for (int arm = 0; arm < 3; arm++)
			CHECK_EQ((uint32_t)snap[arm].cpu.reg64[EAX], 0x000000A5u);
		recipe_case("movzx r32,dh (high-byte read)", h4, sizeof h4, setup_matrix_highbyte, 1);
		for (int arm = 0; arm < 3; arm++)
			CHECK_EQ((uint32_t)snap[arm].cpu.reg64[EAX], 0x000000ABu);
		recipe_case("mov ah,bh (high-byte write)", h5, sizeof h5, setup_matrix_highbyte, 1);
		for (int arm = 0; arm < 3; arm++)
			CHECK_EQ((uint32_t)snap[arm].cpu.reg64[EAX], 0x111134DEu);
		recipe_case("mov ah,ch (high-byte write)", h6, sizeof h6, setup_matrix_highbyte, 1);
		for (int arm = 0; arm < 3; arm++)
			CHECK_EQ((uint32_t)snap[arm].cpu.reg64[EAX], 0x1111A5DEu);
		recipe_case("mov dh,ah (high-byte write)", h7, sizeof h7, setup_matrix_highbyte, 1);
		for (int arm = 0; arm < 3; arm++)
			CHECK_EQ((uint32_t)snap[arm].cpu.reg64[EDX], 0x8888C012u);
		recipe_case("mov ah,bl (low src, high dst)", h8, sizeof h8, setup_matrix_highbyte, 1);
		for (int arm = 0; arm < 3; arm++)
			CHECK_EQ((uint32_t)snap[arm].cpu.reg64[EAX], 0x111156DEu);
		recipe_case("mov ch,ah (8A entry, high dst)", h9, sizeof h9, setup_matrix_highbyte, 1);
		for (int arm = 0; arm < 3; arm++)
			CHECK_EQ((uint32_t)snap[arm].cpu.reg64[ECX], 0x4444C0B6u);
	}

	/* --- the memory forms (repeat 2: the first access warms the TLB, the second must be
	 * inlined) --- */
	{
		static const uint8_t m1[] = { 0x8B, 0x05, 0x00, 0x31, 0x00, 0x00 }; /* mov eax,[0x3100] */
		static const uint8_t m2[] = { 0x89, 0x0D, 0x00, 0x31, 0x00, 0x00 }; /* mov [0x3100],ecx */
		static const uint8_t m3[] = { 0x0F, 0xB6, 0x05, 0x00, 0x31, 0x00, 0x00 };
		static const uint8_t m4[] = { 0x0F, 0xB7, 0x05, 0x04, 0x31, 0x00, 0x00 };
		static const uint8_t m5[] = { 0x8A, 0x05, 0x00, 0x31, 0x00, 0x00 }; /* mov al,[0x3100] */
		static const uint8_t m6[] = { 0x88, 0x0D, 0x00, 0x31, 0x00, 0x00 }; /* mov [0x3100],cl */
		static const uint8_t m7[] = { 0x66, 0x8B, 0x05, 0x04, 0x31, 0x00, 0x00 };
		static const uint8_t m8[] = { 0x66, 0x89, 0x0D, 0x04, 0x31, 0x00, 0x00 };
		static const uint8_t m9[] = { 0x8B, 0x04, 0x0D, 0x00, 0x31, 0x00, 0x00 };
		/* A PREFIXED recipe uop: the segment override is re-materialised into cpu->prefixes
		 * for the body and must be cleared again afterwards (the reference's block-entry
		 * invariant is prefixes == 0).  MEASURED: without the clear the 200 M boot aborted in
		 * uop_decode_block's DBG_ASSERT. */
		static const uint8_t m10a[] = { 0x26, 0x8B, 0x05, 0x00, 0x31, 0x00, 0x00 }; /* mov eax,es:[m] */
		static const uint8_t m10b[] = { 0x2E, 0x89, 0x0D, 0x00, 0x31, 0x00, 0x00 }; /* mov cs:[m],ecx */
		recipe_case("mov r32,[m]", m1, sizeof m1, setup_matrix, 2);
		recipe_case("mov [m],r32", m2, sizeof m2, setup_matrix, 2);
		recipe_case("movzx r32,[m8]", m3, sizeof m3, setup_matrix, 2);
		recipe_case("movzx r32,[m16]", m4, sizeof m4, setup_matrix, 2);
		recipe_case("mov r8,[m8]", m5, sizeof m5, setup_matrix, 2);
		recipe_case("mov [m8],r8", m6, sizeof m6, setup_matrix, 2);
		recipe_case("mov r16,[m16]", m7, sizeof m7, setup_matrix, 2);
		recipe_case("mov [m16],r16", m8, sizeof m8, setup_matrix, 2);
		recipe_case("mov r32,[base+index*1]", m9, sizeof m9, setup_matrix, 2);
		recipe_case("mov r32,es:[m] (prefix)", m10a, sizeof m10a, setup_matrix, 2);
		recipe_case("mov cs:[m],ecx (prefix)", m10b, sizeof m10b, setup_matrix, 2);
	}

	/* --- the ALU family, register source, BOTH flag states (lazy and materialised) --- */
	{
		static const uint8_t a1[] = { 0x01, 0xC8 }; /* add eax, ecx */
		static const uint8_t a2[] = { 0x09, 0xC8 }; /* or  eax, ecx */
		static const uint8_t a3[] = { 0x21, 0xC8 }; /* and eax, ecx */
		static const uint8_t a4[] = { 0x29, 0xC8 }; /* sub eax, ecx */
		static const uint8_t a5[] = { 0x31, 0xC8 }; /* xor eax, ecx */
		static const uint8_t a6[] = { 0x39, 0xC8 }; /* cmp eax, ecx */
		static const uint8_t a7[] = { 0x85, 0xC8 }; /* test eax, ecx */
		recipe_case("add reg (materialised)", a1, sizeof a1, setup_matrix, 2);
		recipe_case("add reg (lazy)", a1, sizeof a1, setup_matrix_lazy, 2);
		recipe_case("or reg (lazy)", a2, sizeof a2, setup_matrix_lazy, 2);
		recipe_case("and reg (lazy)", a3, sizeof a3, setup_matrix_lazy, 2);
		recipe_case("sub reg (lazy)", a4, sizeof a4, setup_matrix_lazy, 2);
		recipe_case("xor reg (lazy)", a5, sizeof a5, setup_matrix_lazy, 2);
		recipe_case("cmp reg (lazy)", a6, sizeof a6, setup_matrix_lazy, 2);
		recipe_case("test reg (lazy)", a7, sizeof a7, setup_matrix_lazy, 2);
	}

	/* --- the ALU immediate forms --- */
	{
		static const uint8_t i1[] = { 0x83, 0xC0, 0x05 };             /* add eax, 5 */
		static const uint8_t i2[] = { 0x83, 0xC8, 0x05 };             /* or  eax, 5 */
		static const uint8_t i3[] = { 0x83, 0xE0, 0x05 };             /* and eax, 5 */
		static const uint8_t i4[] = { 0x83, 0xE8, 0x05 };             /* sub eax, 5 */
		static const uint8_t i5[] = { 0x83, 0xF0, 0x05 };             /* xor eax, 5 */
		static const uint8_t i6[] = { 0x83, 0xF8, 0x05 };             /* cmp eax, 5 */
		static const uint8_t i7[] = { 0xF7, 0xC0, 0x78, 0x56, 0x34, 0x12 }; /* test eax, imm32 */
		static const uint8_t i8[] = { 0xF6, 0xC0, 0x0F };             /* test al, imm8 */
		static const uint8_t i9[] = { 0xF7, 0xD0 };                   /* not eax */
		recipe_case("add imm8", i1, sizeof i1, setup_matrix_lazy, 2);
		recipe_case("or imm8", i2, sizeof i2, setup_matrix_lazy, 2);
		recipe_case("and imm8", i3, sizeof i3, setup_matrix_lazy, 2);
		recipe_case("sub imm8", i4, sizeof i4, setup_matrix_lazy, 2);
		recipe_case("xor imm8", i5, sizeof i5, setup_matrix_lazy, 2);
		recipe_case("cmp imm8", i6, sizeof i6, setup_matrix_lazy, 2);
		recipe_case("test imm32", i7, sizeof i7, setup_matrix_lazy, 2);
		recipe_case("test r/m8 imm8", i8, sizeof i8, setup_matrix_lazy, 2);
		recipe_case("not r/m32", i9, sizeof i9, setup_matrix_lazy, 2);
	}

	/* --- the read-modify-write memory forms: the SAME translate for the read and the write,
	 * plus the code-page gate --- */
	{
		static const uint8_t r1[] = { 0x01, 0x0D, 0x00, 0x31, 0x00, 0x00 };
		static const uint8_t r2[] = { 0x09, 0x0D, 0x00, 0x31, 0x00, 0x00 };
		static const uint8_t r3[] = { 0x21, 0x0D, 0x00, 0x31, 0x00, 0x00 };
		static const uint8_t r4[] = { 0x29, 0x0D, 0x00, 0x31, 0x00, 0x00 };
		static const uint8_t r5[] = { 0x31, 0x0D, 0x00, 0x31, 0x00, 0x00 };
		static const uint8_t r6[] = { 0x39, 0x0D, 0x00, 0x31, 0x00, 0x00 }; /* cmp [m],ecx (read) */
		static const uint8_t r7[] = { 0x83, 0x05, 0x00, 0x31, 0x00, 0x00, 0x05 };
		static const uint8_t r8[] = { 0x83, 0x3D, 0x00, 0x31, 0x00, 0x00, 0x05 };
		static const uint8_t r9[] = { 0xF7, 0x15, 0x00, 0x31, 0x00, 0x00 }; /* not [0x3100] */
		static const uint8_t r10[] = { 0x85, 0x0D, 0x00, 0x31, 0x00, 0x00 }; /* test [m],ecx */
		static const uint8_t r11[] = { 0xF7, 0x05, 0x00, 0x31, 0x00, 0x00, 0x78, 0x56, 0x34, 0x12 };
		recipe_case("add [m],r32 (RMW)", r1, sizeof r1, setup_matrix_lazy, 2);
		recipe_case("or [m],r32 (RMW)", r2, sizeof r2, setup_matrix_lazy, 2);
		recipe_case("and [m],r32 (RMW)", r3, sizeof r3, setup_matrix_lazy, 2);
		recipe_case("sub [m],r32 (RMW)", r4, sizeof r4, setup_matrix_lazy, 2);
		recipe_case("xor [m],r32 (RMW)", r5, sizeof r5, setup_matrix_lazy, 2);
		recipe_case("cmp [m],r32 (read)", r6, sizeof r6, setup_matrix_lazy, 2);
		recipe_case("add [m],imm8 (RMW)", r7, sizeof r7, setup_matrix_lazy, 2);
		recipe_case("cmp [m],imm8 (read)", r8, sizeof r8, setup_matrix_lazy, 2);
		recipe_case("not [m]", r9, sizeof r9, setup_matrix_lazy, 2);
		recipe_case("test [m],r32 (read)", r10, sizeof r10, setup_matrix_lazy, 2);
		recipe_case("test [m],imm32 (read)", r11, sizeof r11, setup_matrix_lazy, 2);
	}

	/* --- the shifts (PERF-7 F2): shl/shr r32, cl ---------------------------------
	 * THE AUDIT'S EXPLICIT CAVEAT, closed here: the two by-cl recipes were first measured
	 * with NO differential-matrix arms (the 3006-check matrix did not grow) -- their only
	 * validation was the boot identity.  The semantics that make arms worth it are the
	 * class this project has shipped wrong before: the count mask (CL = 0x20 is count 0),
	 * the count-0 case (arith.c stores NOTHING -- no write-back, no flag state), and the
	 * flag results (CF = the last bit out, OF per direction).  cmp_state compares flags /
	 * flags_changed / last_result across all three arms, so a wrong CF -- or a flag store
	 * on a count-0 shift, which the body never does -- fails the case, not just a reg. */
	{
		static const uint8_t s1[] = { 0xD3, 0xE0 }; /* shl eax, cl */
		static const uint8_t s2[] = { 0xD3, 0xE8 }; /* shr eax, cl */
		static const uint8_t s3[] = { 0xD3, 0xE2 }; /* shl edx, cl */
		static const uint8_t s4[] = { 0xD3, 0xEA }; /* shr edx, cl */

		recipe_case("shl eax,cl (composed)", s1, sizeof s1, setup_matrix_shift, 2);
		recipe_case("shr eax,cl (composed)", s2, sizeof s2, setup_matrix_shift, 2);
		recipe_case("shl edx,cl (negative, CF=1 OF=1)", s3, sizeof s3, setup_matrix_shift, 2);
		recipe_case("shr edx,cl (negative, CF=1 OF=1)", s4, sizeof s4, setup_matrix_shift, 2);
	}

	/* THE COUNT SEMANTICS, one execution each with the answer pinned in every arm (the
	 * high-byte rule: a shift WRITES its destination, so a second execution composes and
	 * the pin would need the composition -- one pass is the value the encoding means). */
	{
		static const uint8_t c1[] = { 0xD3, 0xE0 }; /* shl eax, cl */
		static const uint8_t c2[] = { 0xD3, 0xE8 }; /* shr eax, cl */

		recipe_case("shl eax,cl (count 5: CF=1 OF=1)", c1, sizeof c1, setup_matrix_shift, 1);
		for (int arm = 0; arm < 3; arm++) {
			CHECK_EQ((uint32_t)snap[arm].cpu.reg64[EAX], 0x468ACF00u);
		}
		recipe_case("shr eax,cl (count 5: CF=1 OF=0)", c2, sizeof c2, setup_matrix_shift, 1);
		for (int arm = 0; arm < 3; arm++) {
			CHECK_EQ((uint32_t)snap[arm].cpu.reg64[EAX], 0x00D1A2B3u);
		}

		/* THE MASK + COUNT-0 CASES.  CL = 0x20 is count 0 through the body's `& 31` -- a
		 * recipe that tested CL directly (or shifted by it unmasked) stores flag state on
		 * a count-0 shift, which the body never does: flags_changed must STILL be the 0
		 * the materialised setup left, and the destination must be untouched. */
		{
			static const uint8_t z0[] = { 0xB9, 0x00, 0x00, 0x00, 0x00, /* mov ecx, 0 */
			                              0xD3, 0xE0 };                 /* shl eax, cl */
			static const uint8_t z1[] = { 0xB9, 0x20, 0x00, 0x00, 0x00, /* mov ecx, 0x20 */
			                              0xD3, 0xE8 };                 /* shr eax, cl */

			recipe_case("shl eax,cl (CL=0: NOTHING stored)", z0, sizeof z0,
			            setup_matrix_shift, 1);
			for (int arm = 0; arm < 3; arm++) {
				CHECK_EQ((uint32_t)snap[arm].cpu.reg64[EAX], 0x1A345678u);
				CHECK_EQ(snap[arm].cpu.flags_changed, 0);
			}
			recipe_case("shr eax,cl (CL=0x20 masked to 0)", z1, sizeof z1,
			            setup_matrix_shift, 1);
			for (int arm = 0; arm < 3; arm++) {
				CHECK_EQ((uint32_t)snap[arm].cpu.reg64[EAX], 0x1A345678u);
				CHECK_EQ(snap[arm].cpu.flags_changed, 0);
			}
		}

		/* THE 5-BIT MASK'S OTHER END: CL = 0xFF is count 31.  `mov eax, 1` (a RECIPE uop)
		 * and `mov cl, 0xff` (a CALLED uop) ride in the program -- a recipe, a call and a
		 * shift in one block is also a shape the elision must survive -- so neither the
		 * setup's EAX nor its ECX is the operand here. */
		{
			static const uint8_t m1[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, /* mov eax, 1 */
			                              0xB1, 0xFF,                   /* mov cl, 0xff */
			                              0xD3, 0xE0 };                 /* shl eax, cl */

			recipe_case("shl eax,cl (CL=0xff -> count 31)", m1, sizeof m1,
			            setup_matrix_shift, 1);
			for (int arm = 0; arm < 3; arm++) {
				CHECK_EQ((uint32_t)snap[arm].cpu.reg64[EAX], 0x80000000u);
			}
		}

		/* THE LAZY-FLAG STATE: the shift REPLACES the getter source (last_result becomes
		 * the shift's result, flags_changed the shift's mask), so a following reader must
		 * see the shift's state and not the stale cmp's -- the same composition the ALU
		 * cases exercise, on a recipe whose flags are half-materialised (CF/OF direct). */
		{
			static const uint8_t l1[] = { 0xD3, 0xE0, /* shl eax, cl */
			                              0x73, 0x01, 0xF4, 0x90, 0xF4 }; /* jae +1 */

			recipe_case("shl eax,cl; jae (lazy before)", l1, sizeof l1,
			            setup_matrix_lazy, 2);
		}
	}

	/* --- inc/dec (CF-preserving), mov imm, nop --- */
	{
		static const uint8_t d1[] = { 0x40 };                   /* inc eax */
		static const uint8_t d2[] = { 0x4F };                   /* dec edi */
		static const uint8_t d3[] = { 0x42 };                   /* inc edx */
		static const uint8_t d4[] = { 0xB8, 0x78, 0x56, 0x34, 0x12 };
		static const uint8_t d5[] = { 0x90 };
		recipe_case("inc eax (lazy CF)", d1, sizeof d1, setup_matrix_lazy, 2);
		recipe_case("dec edi (lazy CF)", d2, sizeof d2, setup_matrix_lazy, 2);
		recipe_case("inc edx (materialised CF)", d3, sizeof d3, setup_matrix, 2);
		recipe_case("mov r32,imm32", d4, sizeof d4, setup_matrix, 2);
		recipe_case("nop", d5, sizeof d5, setup_matrix, 2);
	}

	/* --- the jumps: the condition evaluated from the MATERIALISED flags (a fresh machine has
	 * flags_changed == 0) and from the LAZY state (a preceding cmp), taken and not taken --- */
	{
		static const uint8_t j1[] = { 0x74, 0x01, 0xF4, 0x90, 0xF4 };   /* je +1 (taken/not) */
		static const uint8_t j2[] = { 0x75, 0x01, 0xF4, 0x90, 0xF4 };   /* jne */
		static const uint8_t j3[] = { 0x73, 0x01, 0xF4, 0x90, 0xF4 };   /* jae */
		static const uint8_t j4[] = { 0x77, 0x01, 0xF4, 0x90, 0xF4 };   /* ja */
		static const uint8_t j5[] = { 0x0F, 0x84, 0x01, 0x00, 0x00, 0x00, 0xF4, 0x90, 0xF4 };
		static const uint8_t j6[] = { 0x0F, 0x86, 0x01, 0x00, 0x00, 0x00, 0xF4, 0x90, 0xF4 };
		static const uint8_t j7[] = { 0x72, 0x01, 0xF4, 0x90, 0xF4 };   /* jb */
		static const uint8_t j8[] = { 0x0F, 0x82, 0x01, 0x00, 0x00, 0x00, 0xF4, 0x90, 0xF4 };
		static const uint8_t j9[] = { 0x00, 0xC8, 0x74, 0x01, 0xF4, 0x90, 0xF4 }; /* add al,cl; je */
		static const uint8_t j10[] = { 0x00, 0xC8, 0x72, 0x01, 0xF4, 0x90, 0xF4 }; /* add al,cl; jb */
		static const uint8_t j11[] = { 0xEB, 0x01, 0xF4, 0x90, 0xF4 };  /* jmp rel8 */
		static const uint8_t j12[] = { 0xE9, 0x01, 0x00, 0x00, 0x00, 0xF4, 0x90, 0xF4 };
		/* JIT-FIX-1 should-fix 1: THE FOUR ENCODINGS THE MATRIX WAS MISSING.  The recipe
		 * table has all TWELVE (six conditions x two widths, jit_codegen.c:905-920), and
		 * the matrix executed 8 of them: `76` (jbe rel8), `0F 83` (jae rel32), `0F 85`
		 * (jne rel32) and `0F 87` (ja rel32) had neither a case here nor a pin below --
		 * and the two arrays are POSITIONAL, so a wrong pointer would have silently
		 * de-inlined a condition (safe, invisible, and exactly the "seat that is never
		 * called" failure this project treats as real). */
		static const uint8_t j13[] = { 0x76, 0x01, 0xF4, 0x90, 0xF4 };  /* jbe rel8 */
		static const uint8_t j14[] = { 0x0F, 0x83, 0x01, 0x00, 0x00, 0x00, 0xF4, 0x90, 0xF4 };
		static const uint8_t j15[] = { 0x0F, 0x85, 0x01, 0x00, 0x00, 0x00, 0xF4, 0x90, 0xF4 };
		static const uint8_t j16[] = { 0x0F, 0x87, 0x01, 0x00, 0x00, 0x00, 0xF4, 0x90, 0xF4 };
		/* c6/c1 set ZF (cmp eax,ecx with equal values) and c7/c2 leave it clear */
		recipe_case("je (materialised, ZF=0)", j1, sizeof j1, setup_matrix, 2);
		recipe_case("jne (materialised, ZF=0)", j2, sizeof j2, setup_matrix, 2);
		recipe_case("jae (materialised, CF=0)", j3, sizeof j3, setup_matrix, 2);
		recipe_case("ja (materialised, CF=0)", j4, sizeof j4, setup_matrix, 2);
		recipe_case("je rel32 (materialised)", j5, sizeof j5, setup_matrix, 2);
		recipe_case("jbe rel32 (materialised)", j6, sizeof j6, setup_matrix, 2);
		recipe_case("jb (materialised, CF=0)", j7, sizeof j7, setup_matrix, 2);
		recipe_case("jb rel32 (materialised)", j8, sizeof j8, setup_matrix, 2);
		/* THE COMPOUND CONDITIONS WITH A LAZY STATE: jbe/ja call BOTH getters, and the matrix
		 * above only ever exercised them with materialised flags -- which is exactly how the
		 * `or` with last_op_size survived (see jit_codegen.c's rec_jcc). */
		recipe_case("je rel32 (lazy)", j5, sizeof j5, setup_matrix_lazy, 2);
		recipe_case("jbe rel32 (lazy)", j6, sizeof j6, setup_matrix_lazy, 2);
		recipe_case("add al,cl; je (lazy ZF)", j9, sizeof j9, setup_matrix_lazy, 2);
		recipe_case("add al,cl; jb (lazy CF)", j10, sizeof j10, setup_matrix_lazy, 2);
		recipe_case("jmp rel8", j11, sizeof j11, setup_matrix, 2);
		recipe_case("jmp rel32", j12, sizeof j12, setup_matrix, 2);
		/* the four the matrix was missing (see j13..j16's note) -- and the two COMPOUND
		 * conditions among them run in BOTH flag states, because BE and A read CF and ZF
		 * from two getter calls and setup_matrix_lazy's `cmp 0x10, 0x20` sets CF=1, ZF=0,
		 * which is the combination that makes a wrong compound read visible. */
		recipe_case("jbe rel8 (materialised, CF=0 ZF=0)", j13, sizeof j13, setup_matrix, 2);
		recipe_case("jbe rel8 (lazy, CF=1 ZF=0)", j13, sizeof j13, setup_matrix_lazy, 2);
		recipe_case("jae rel32 (materialised, CF=0)", j14, sizeof j14, setup_matrix, 2);
		recipe_case("jne rel32 (materialised, ZF=0)", j15, sizeof j15, setup_matrix, 2);
		recipe_case("ja rel32 (materialised, CF=0 ZF=0)", j16, sizeof j16, setup_matrix, 2);
		recipe_case("ja rel32 (lazy, CF=1 ZF=0)", j16, sizeof j16, setup_matrix_lazy, 2);

		/* JIT-FIX-2: THE CF=0, ZF=1 STATE, the one flag state no case above reaches (they use
		 * CF=0 ZF=0 and CF=1 ZF=0).  It is exactly the state that separates the two COMPOUND
		 * conditions from their simple neighbours: B(CF) and BE(CF||ZF) differ only when
		 * CF=0 && ZF=1, and AE(!CF) and A(!CF && !ZF) differ only there too.  So a swap of
		 * the rel8 or rel32 array's nibble-2 and nibble-6 slots (jb<->jbe) or nibble-3 and
		 * nibble-7 (jae<->ja) substituted one condition's recipe for another with no other
		 * trace, and NO case below -- nor the twelve fn-pointer pins, which compare a lookup
		 * to itself -- could see it.  MEASURED before these cases (results/jit-fix-2.md ss1):
		 * all four swaps left this suite at 2778 checks, 0 failures and the nasm corpus at
		 * 118/118.  What DID see them was the bench's pinned 200 M `--jit on` leg (the guest
		 * halts or stops at a different eip) -- so this is a hole in the MATRIX, not in the
		 * gate's teeth, and these cases buy attribution (which pair, which width) in 0.2 s
		 * instead of a red light from a 200 M boot.
		 *
		 * `cmp eax,eax` IS the state: EAX is 0xC0DE in setup_matrix, so the compare leaves
		 * CF=0, ZF=1, OF=0, SF=0 -- with a LAZY flag state, which is the arm the compound
		 * conditions exercise when they call both getters. */
		static const uint8_t z1[] = { 0x39, 0xC0, 0x72, 0x01, 0xF4, 0x90, 0xF4 }; /* cmp; jb */
		static const uint8_t z2[] = { 0x39, 0xC0, 0x76, 0x01, 0xF4, 0x90, 0xF4 }; /* cmp; jbe */
		static const uint8_t z3[] = { 0x39, 0xC0, 0x73, 0x01, 0xF4, 0x90, 0xF4 }; /* cmp; jae */
		static const uint8_t z4[] = { 0x39, 0xC0, 0x77, 0x01, 0xF4, 0x90, 0xF4 }; /* cmp; ja */
		static const uint8_t z5[] = { 0x39, 0xC0, 0x0F, 0x82, 0x01, 0x00, 0x00, 0x00,
		                              0xF4, 0x90, 0xF4 };                          /* cmp; jb rel32 */
		static const uint8_t z6[] = { 0x39, 0xC0, 0x0F, 0x86, 0x01, 0x00, 0x00, 0x00,
		                              0xF4, 0x90, 0xF4 };                          /* cmp; jbe rel32 */
		static const uint8_t z7[] = { 0x39, 0xC0, 0x0F, 0x83, 0x01, 0x00, 0x00, 0x00,
		                              0xF4, 0x90, 0xF4 };                          /* cmp; jae rel32 */
		static const uint8_t z8[] = { 0x39, 0xC0, 0x0F, 0x87, 0x01, 0x00, 0x00, 0x00,
		                              0xF4, 0x90, 0xF4 };                          /* cmp; ja rel32 */
		/* z1 and z2 separate B<->BE, z3 and z4 separate AE<->A, and z5..z8 repeat BOTH pairs
		 * for the rel32 array (a second positional array with the same blindness).  The
		 * taken/not-taken word in each name is the CORRECT x86 answer for the state. */
		recipe_case("cmp; jb rel8 (ZF=1 CF=0, not taken)", z1, sizeof z1, setup_matrix, 2);
		recipe_case("cmp; jbe rel8 (ZF=1 CF=0, taken)", z2, sizeof z2, setup_matrix, 2);
		recipe_case("cmp; jae rel8 (ZF=1 CF=0, taken)", z3, sizeof z3, setup_matrix, 2);
		recipe_case("cmp; ja rel8 (ZF=1 CF=0, not taken)", z4, sizeof z4, setup_matrix, 2);
		recipe_case("cmp; jb rel32 (ZF=1 CF=0, not taken)", z5, sizeof z5, setup_matrix, 2);
		recipe_case("cmp; jbe rel32 (ZF=1 CF=0, taken)", z6, sizeof z6, setup_matrix, 2);
		recipe_case("cmp; jae rel32 (ZF=1 CF=0, taken)", z7, sizeof z7, setup_matrix, 2);
		recipe_case("cmp; ja rel32 (ZF=1 CF=0, not taken)", z8, sizeof z8, setup_matrix, 2);
	}
}

/* ==================================================================== *
 * THE RECIPE TABLE ITSELF
 *
 * M1 pinned this with `jit_recipe_count() == 1`; M2's table is the point of the unit, so the
 * pin is the table's INVARIANTS instead of a number that every new recipe would move:
 *
 *   (a) the families the unit claims are PRESENT -- each pin below synthesises a uop with the
 *       body and the shape the decoder produces for that encoding and asks the table for it.
 *       Dropping a recipe, or moving it to a shape it does not describe, fails here.
 *   (b) the table is SORTED AND UNIQUE by the key.  The lookup is a binary search over the fn
 *       pointer, so a duplicate key would make one of the two unreachable -- the "seat that is
 *       never called" failure this project treats as real, and one that no runtime counter can
 *       see.
 *   (c) the STATIC REACH over the generated descriptor table: how many of uop_gen.c's
 *       (descriptor, fn) pairs the recipes cover.  This is a measurement, not a threshold.
 * ==================================================================== */
static const char *recipe_shape_name(uint8_t shape)
{
	switch (shape) {
	case UOP_SHAPE_PLAIN0: return "PLAIN0";
	case UOP_SHAPE_PLAIN1: return "PLAIN1";
	case UOP_SHAPE_MEM0: return "MEM0";
	case UOP_SHAPE_MEM1: return "MEM1";
	case UOP_SHAPE_MEMF0: return "MEMF0";
	case UOP_SHAPE_MEMF1: return "MEMF1";
	case UOP_SHAPE_REG0: return "REG0";
	case UOP_SHAPE_REG1: return "REG1";
	case UOP_SHAPE_REGF0: return "REGF0";
	case UOP_SHAPE_REGF1: return "REGF1";
	default: return "?";
	}
}

/* one encoding the unit claims a recipe for: the body, the shape the decoder gives it, and the
 * immediate the shape implies (MEMF1/REGF1/PLAIN1 carry one; the rest do not). */
typedef struct {
	void (*fn)(void);
	uint8_t shape;
	int imm;
	const char *what;
} recipe_pin_t;

static void test_recipe_table(void)
{
	static const recipe_pin_t pins[] = {
		{ (void (*)(void))instr32_89_reg, UOP_SHAPE_REG0, 0, "mov r/m32,r32 reg" },
		{ (void (*)(void))instr32_89_mem, UOP_SHAPE_MEM0, 0, "mov r/m32,r32 mem" },
		{ (void (*)(void))instr32_8B_reg, UOP_SHAPE_REG0, 0, "mov r32,r/m32 reg" },
		{ (void (*)(void))instr32_8B_mem, UOP_SHAPE_MEM0, 0, "mov r32,r/m32 mem" },
		{ (void (*)(void))instr32_0FB6_reg, UOP_SHAPE_REG0, 0, "movzx r32,r/m8 reg" },
		{ (void (*)(void))instr32_0FB6_mem, UOP_SHAPE_MEM0, 0, "movzx r32,r/m8 mem" },
		{ (void (*)(void))instr32_0FB7_reg, UOP_SHAPE_REG0, 0, "movzx r32,r/m16 reg" },
		{ (void (*)(void))instr32_0FB7_mem, UOP_SHAPE_MEM0, 0, "movzx r32,r/m16 mem" },
		{ (void (*)(void))instr_8A_reg, UOP_SHAPE_REG0, 0, "mov r8,r/m8 reg" },
		{ (void (*)(void))instr_8A_mem, UOP_SHAPE_MEM0, 0, "mov r8,r/m8 mem" },
		{ (void (*)(void))instr_88_reg, UOP_SHAPE_REG0, 0, "mov r/m8,r8 reg" },
		{ (void (*)(void))instr_88_mem, UOP_SHAPE_MEM0, 0, "mov r/m8,r8 mem" },
		{ (void (*)(void))instr16_89_reg, UOP_SHAPE_REG0, 0, "mov r/m16,r16 reg" },
		{ (void (*)(void))instr16_89_mem, UOP_SHAPE_MEM0, 0, "mov r/m16,r16 mem" },
		{ (void (*)(void))instr16_8B_reg, UOP_SHAPE_REG0, 0, "mov r16,r/m16 reg" },
		{ (void (*)(void))instr16_8B_mem, UOP_SHAPE_MEM0, 0, "mov r16,r/m16 mem" },
		{ (void (*)(void))instr32_8D_mem, UOP_SHAPE_MEM0, 0, "lea r32,m" },
		{ (void (*)(void))instr32_01_reg, UOP_SHAPE_REG0, 0, "add reg" },
		{ (void (*)(void))instr32_01_mem, UOP_SHAPE_MEM0, 0, "add mem (RMW)" },
		{ (void (*)(void))instr32_09_reg, UOP_SHAPE_REG0, 0, "or reg" },
		{ (void (*)(void))instr32_21_reg, UOP_SHAPE_REG0, 0, "and reg" },
		{ (void (*)(void))instr32_29_reg, UOP_SHAPE_REG0, 0, "sub reg" },
		{ (void (*)(void))instr32_31_reg, UOP_SHAPE_REG0, 0, "xor reg" },
		{ (void (*)(void))instr32_39_reg, UOP_SHAPE_REG0, 0, "cmp reg" },
		{ (void (*)(void))instr32_39_mem, UOP_SHAPE_MEM0, 0, "cmp mem (read)" },
		{ (void (*)(void))instr32_83_0_reg, UOP_SHAPE_REGF1, 5, "add imm8" },
		{ (void (*)(void))instr32_83_0_mem, UOP_SHAPE_MEMF1, 5, "add imm8 mem (RMW)" },
		{ (void (*)(void))instr32_83_7_reg, UOP_SHAPE_REGF1, 5, "cmp imm8" },
		{ (void (*)(void))instr32_85_reg, UOP_SHAPE_REG0, 0, "test reg" },
		{ (void (*)(void))instr32_F7_0_reg, UOP_SHAPE_REGF1, 0x1234, "test imm32" },
		{ (void (*)(void))instr_F6_0_reg, UOP_SHAPE_REGF1, 0x0F, "test r/m8 imm8" },
		{ (void (*)(void))instr32_F7_2_reg, UOP_SHAPE_REGF0, 0, "not reg" },
		{ (void (*)(void))instr32_F7_2_mem, UOP_SHAPE_MEMF0, 0, "not mem" },
		/* PERF-7 F2: the by-cl shift register forms.  The matrix's shift arms drive the
		 * semantics; these pins keep the ENTRIES findable (a dropped entry silently
		 * de-inlines 73.7% of the benchmark's called toll -- a slowdown, invisible to
		 * every state gate, which is why it is pinned here and not only measured). */
		{ (void (*)(void))instr32_D3_4_reg, UOP_SHAPE_REGF0, 0, "shl r32,cl" },
		{ (void (*)(void))instr32_D3_5_reg, UOP_SHAPE_REGF0, 0, "shr r32,cl" },
		{ (void (*)(void))instr32_40, UOP_SHAPE_PLAIN0, 0, "inc eax" },
		{ (void (*)(void))instr32_4F, UOP_SHAPE_PLAIN0, 0, "dec edi" },
		{ (void (*)(void))instr32_BE, UOP_SHAPE_PLAIN1, 42, "mov r32,imm32" },
		{ (void (*)(void))instr_90, UOP_SHAPE_PLAIN0, 0, "nop" },
		{ (void (*)(void))instr32_74, UOP_SHAPE_PLAIN1, 5, "je rel8" },
		{ (void (*)(void))instr32_75, UOP_SHAPE_PLAIN1, 5, "jne rel8" },
		{ (void (*)(void))instr32_73, UOP_SHAPE_PLAIN1, 5, "jae rel8" },
		{ (void (*)(void))instr32_0F82, UOP_SHAPE_PLAIN1, 5, "jb rel32" },
		{ (void (*)(void))instr32_0F86, UOP_SHAPE_PLAIN1, 5, "jbe rel32" },
		{ (void (*)(void))instr32_E9, UOP_SHAPE_PLAIN1, 5, "jmp rel32" },
		/* JIT-FIX-1 should-fix 1: ALL TWELVE jcc encodings are pinned here, not five.
		 * jitcg_jcc8_fns/jitcg_jcc32_fns are INDEXED BY THE CONDITION NIBBLE, so a pointer
		 * in the wrong slot silently gives some other condition's recipe -- which is a
		 * wrong-answer bug, and the array is typed to take any of them.  The five above
		 * happened to be the five the matrix covered; these seven are the rest. */
		{ (void (*)(void))instr32_72, UOP_SHAPE_PLAIN1, 5, "jb rel8" },
		{ (void (*)(void))instr32_76, UOP_SHAPE_PLAIN1, 5, "jbe rel8" },
		{ (void (*)(void))instr32_77, UOP_SHAPE_PLAIN1, 5, "ja rel8" },
		{ (void (*)(void))instr32_0F83, UOP_SHAPE_PLAIN1, 5, "jae rel32" },
		{ (void (*)(void))instr32_0F84, UOP_SHAPE_PLAIN1, 5, "je rel32" },
		{ (void (*)(void))instr32_0F85, UOP_SHAPE_PLAIN1, 5, "jne rel32" },
		{ (void (*)(void))instr32_0F87, UOP_SHAPE_PLAIN1, 5, "ja rel32" },
	};

	fprintf(stderr, "  == test_recipe_table\n");
	CHECK(jit_recipe_count() >= 88);

	for (size_t i = 0; i < sizeof pins / sizeof pins[0]; i++) {
		uop_t u;
		memset(&u, 0, sizeof u);
		u.fn = (void (*)(void))pins[i].fn;
		u.shape = pins[i].shape;
		u.modrm = 0xC9; /* mod = 3, reg = 1, rm = 1: a register form */
		u.g = 1;
		u.base = UOP_REG_NONE;
		u.index = UOP_REG_NONE;
		u.seg = UOP_SEG_NONE;
		u.addr_kind = UOP_ADDR_32;
		u.imm = pins[i].imm;
		const jit_recipe_t *r = jit_recipe_lookup(&u);
		if (r == NULL) {
			fprintf(stderr, "FAIL the recipe table has no %s (%s)\n", pins[i].what,
			        recipe_shape_name(pins[i].shape));
			checks++;
			failures++;
			continue;
		}
		CHECK_EQ(r->shape, pins[i].shape);
		CHECK(r->name != NULL && r->name[0] != 0);
		CHECK(jit_recipe_name(u.fn) == r->name);
		/* the SHAPE must be load-bearing: the same body under a shape the recipe does not
		 * describe must NOT match (that is what makes a mis-call impossible) */
		if (pins[i].shape != UOP_SHAPE_MEM0 && pins[i].shape != UOP_SHAPE_MEMF0) {
			u.shape = UOP_SHAPE_MEM0;
			u.addr_kind = UOP_ADDR_32;
			CHECK(jit_recipe_lookup(&u) == NULL);
		}
	}

	/* (c) the static reach over the generated descriptor table */
	{
		uint32_t pairs = 0, covered = 0;
		for (uint32_t i = 0; i < uop_desc_count; i++) {
			void (*fns[3])(void);
			fns[0] = uop_descs[i].fn_plain;
			fns[1] = uop_descs[i].fn_mem;
			fns[2] = uop_descs[i].fn_reg;
			for (int k = 0; k < 3; k++) {
				if (fns[k] == NULL) continue;
				pairs++;
				if (jit_recipe_name((void (*)(void))fns[k]) != NULL) covered++;
			}
		}
		fprintf(stderr, "  recipe table: %u recipes; %u of the descriptor table's %u "
		                "(descriptor, body) pairs are covered\n",
		        jit_recipe_count(), covered, pairs);
		CHECK(covered > 0);
		CHECK(pairs > covered); /* the table is a subset: the fallback is real */

		/* (d) PERF-7 F1: THE FLAG-CLAUSE INVARIANT.  The elision criterion excludes
		 * FLUSH/BLOCK_END uops by a flag test; its mechanical ground is that NO recipe
		 * encoding carries either descriptor flag (scanned over all descriptors at
		 * landing, results/perf-7.md).  A future table entry whose encoding carries
		 * UOP_DESC_FLUSH or UOP_DESC_BLOCK_END makes that clause LOAD-BEARING -- this
		 * pin names it at make-selftest time, instead of the clause being a no-op
		 * someone deletes. */
		for (uint32_t i = 0; i < uop_desc_count; i++) {
			void (*fns[3])(void);
			fns[0] = uop_descs[i].fn_plain;
			fns[1] = uop_descs[i].fn_mem;
			fns[2] = uop_descs[i].fn_reg;
			for (int k = 0; k < 3; k++) {
				if (fns[k] == NULL) continue;
				if (jit_recipe_name(fns[k]) == NULL) continue;
				CHECK((uop_descs[i].flags & UOP_DESC_BLOCK_END) == 0);
				CHECK((uop_descs[i].iflags & UOP_DESC_FLUSH) == 0);
			}
		}
	}
}

/* ==================================================================== *
 * JIT-6: BLOCK CHAINING -- THE DIFFERENTIAL AND THE ENGAGE PIN
 *
 *   0x2000: B8 05 00 00 00     mov eax, 5
 *   0x2005: B9 11 11 11 11     mov ecx, 0x11111111
 *   0x200A: BA 22 22 22 22     mov edx, 0x22222222
 *   loop:
 *   0x200F: 48                 dec eax
 *   0x2010: 75 FD              jnz loop            (rel8 back to 0x200F)
 *   0x2012: 89 14 25 00 310000 mov [0x3100], edx
 *   0x2019: 89 0C 25 04 310000 mov [0x3104], ecx
 *   0x2020: F4                 hlt
 *   0x2021: EB FD              jmp $
 *
 * The first block is {mov eax, mov ecx, mov edx, dec, jnz}; its first TAIL (jnz taken)
 * decodes a second block {dec, jnz} at 0x200F -- which is then both the chain SOURCE and the
 * chain TARGET of every later iteration: a self-loop through the chain stub.  ECX/EDX are
 * held NON-ZERO across every link on purpose: they are what makes a dropped or swapped
 * spill-slot reload VISIBLE (with both zero, a broken reload is invisible and the test would
 * pass vacuously).  The assertions:
 *
 *   1. the three arms agree on the full state, including ECX/EDX and the two stores (the
 *      chain is semantically inert);
 *   2. chains > 0 in the compiled arm -- the chain ENGAGED, which is what distinguishes
 *      "the chain path ran and was right" from "the fallback path ran and was right" (a
 *      chain stub that always fell back would pass assertion 1 forever);
 *   3. the guest result itself: eax reached 0 and the stores wrote the setup values -- the
 *      loop really iterated through the chain.
 *
 * ABLE TO FAIL: mutation "link drops the pending count" zeroes it at every link, the flush
 * at hlt then adds too few, and instruction_counter mismatches; mutation "the RCX/RDX
 * reloads are swapped" swaps the stored values; mutation "the stub always falls back" fails
 * 2.  The fallback side has its own pin: the FIRST taken jnz chains nowhere (the target
 * block does not exist yet), which is the decode-and-compile path, and its result is part
 * of the same state comparison.
 * ==================================================================== */
static void test_chain(void)
{
	fprintf(stderr, "  == test_chain (self-loop through the chain stub)\n");
	static const uint8_t prog[] = {
		0xB8, 0x05, 0x00, 0x00, 0x00, /* mov eax, 5          */
		0xB9, 0x11, 0x11, 0x11, 0x11, /* mov ecx, 0x11111111 */
		0xBA, 0x22, 0x22, 0x22, 0x22, /* mov edx, 0x22222222 */
		0x48,                         /* dec eax             */
		0x75, 0xFD,                   /* jnz loop            */
		/* mov [0x3100], edx ; mov [0x3104], ecx -- the link-surviving registers, stored */
		0x89, 0x14, 0x25, 0x00, 0x31, 0x00, 0x00,
		0x89, 0x0C, 0x25, 0x04, 0x31, 0x00, 0x00,
		0xF4,                         /* hlt                 */
		0xEB, 0xFD,                   /* jmp $               */
	};

	arm_setup = NULL;
	for (int arm = 0; arm < 3; arm++) arm_run(arm, CODE, prog, (int)sizeof prog, 2000);

	cmp_state(0, 2, "chain: interpreter vs compiled");
	cmp_state(1, 2, "chain: uop-cache vs compiled");
	CHECK_EQ(ram32(TRACE), 0x22222222u);
	CHECK_EQ(ram32(TRACE + 4), 0x11111111u);
	CHECK_EQ(snap[2].cpu.reg64[EAX], 0);
	if (arm_stats[2].chains < 1) {
		fprintf(stderr, "FAIL chain: the chain never engaged (links %llu, tails %llu)\n",
		        (unsigned long long)arm_stats[2].chains,
		        (unsigned long long)arm_stats[2].exits[JIT_EXIT_TAIL]);
		checks++;
		failures++;
		return;
	}
	/* the fallback is exercised too: the first tail could not have chained (its target was
	 * not compiled yet), so the run must contain a compile of the loop block */
	CHECK(arm_stats[2].blocks_compiled >= 2);
	fprintf(stderr, "  chain: links %llu, tails seen %llu, blocks %llu, state equal in all arms\n",
	        (unsigned long long)arm_stats[2].chains,
	        (unsigned long long)arm_stats[2].exits[JIT_EXIT_TAIL],
	        (unsigned long long)arm_stats[2].blocks_compiled);
}

/* ==================================================================== *
 * PERF-7 F1: THE ELISION BATTERY -- the checks-skip is a claim about which
 * checks a recipe CANNOT need, and that claim needs its own probes.
 *
 *  (a) THE ELIDED SHAPE: consecutive recipe uops in one block must elide the shared
 *      checks routine on EVERY one of them (the criterion's compile-time count is the
 *      instrument), and the full state -- instruction_counter and uop_pending included,
 *      which is where a wrongly-elided flush or a missed pending count would show --
 *      must be identical across all three engines.
 *
 *  (b) THE EXCLUDED SHAPE: the criterion's last clause (same page) is not vacuous.  An
 *      instruction ENDING at its page's last byte (eip+len on the next page -- the
 *      decoder only drops instructions whose BYTES leave the page) must keep its checks:
 *      the executor's step 8 (the page-cross break) fires there and ends a reference
 *      batch.  The probe pins the count relationship -- EVERY recipe uop elides EXCEPT
 *      the edge uop -- and the counter equality across arms.
 *
 *  THE OTHER EXCLUDED SHAPES are driven by the existing pins and would fail without
 *  them: JCC/JMP/LEA (the ip writers) by the matrix's jcc/jmp/lea cases and
 *  test_tail_exit (a taken branch must reach step 11's TAIL), FLUSH by test_staircase's
 *  cmpxchg arm, BLOCK_END/jit_block_boundary by its sti arm.  The mutation arms in
 *  results/perf-7/mut-*.txt invert each clause and require the matching pin to fail.
 * ==================================================================== */
static void test_continuation_elision(void)
{
	fprintf(stderr, "  == test_continuation_elision (PERF-7 F1: the checks-skip battery)\n");

	/* (a) six consecutive recipe uops -- mov imm, add, sub, mov imm, inc, shl -- then hlt.
	 * Every recipe is on one page and carries neither flag, so all six must elide; the hlt
	 * is a CALLED uop whose checks run (and which is the only reason the block exits). */
	{
		static const uint8_t prog[] = {
			0xB8, 0x01, 0x00, 0x00, 0x00, /* mov eax, 1     (recipe: mov imm32) */
			0x01, 0xC8,                   /* add eax, ecx   (recipe; ECX = 5 -> 6) */
			0x29, 0xC8,                   /* sub eax, ecx   (recipe; -> 1) */
			0xB9, 0x03, 0x00, 0x00, 0x00, /* mov ecx, 3     (recipe) */
			0x40,                         /* inc eax        (recipe; -> 2) */
			0xD3, 0xE0,                   /* shl eax, cl    (recipe; cl = 3 -> 16) */
			0xF4,                         /* hlt            (CALLED: its checks run) */
			0xEB, 0xFD,                   /* jmp $ */
		};

		for (int arm = 0; arm < 3; arm++) {
			arm_setup = setup_matrix_shift;
			arm_run(arm, CODE, prog, (int)sizeof prog, 200);
		}
		arm_setup = NULL;
		cmp_state(0, 1, "elision: interp vs uop-cache");
		cmp_state(1, 2, "elision: uop-cache vs compiled");
		/* the elision ENGAGED -- all six recipes, or the probes above are measuring an
		 * un-elided block and proving nothing (the vacuous-pass class) */
		CHECK(arm_stats[2].blocks_compiled >= 1);
		CHECK_EQ(arm_stats[2].uops_checks_elided, 6);
		CHECK_EQ(arm_stats[2].inlined_uops, 6);
		CHECK_EQ(arm_stats[2].called_uops, 1);
		/* and the answer is the composed arithmetic, in every arm */
		for (int arm = 0; arm < 3; arm++)
			CHECK_EQ((uint32_t)snap[arm].cpu.reg64[EAX], 16u);
	}

	/* (b) the page-edge uop: `add eax, ecx` placed so it ENDS at CODE+0x1000 (eip
	 * CODE+0xFFE, len 2 -> ip on the next page).  Its checks MUST run and step 8 must end
	 * the reference batch there; every other recipe uop in the page (the mov and the
	 * nop fill) elides.  The budget covers the fill: this program is ~0x1001 instructions,
	 * each executed once -- which is also what makes the static elided count comparable
	 * to the dynamic inlined count. */
	{
		static uint8_t prog[0x1010];
		int n = 0;

		memset(prog, 0x90, sizeof prog); /* nop fill: recipe uops, all elidable */
		prog[n++] = 0xB8;                /* mov eax, 1 */
		prog[n++] = 0x01;
		prog[n++] = 0x00;
		prog[n++] = 0x00;
		prog[n++] = 0x00;
		n = 0xFFE;                       /* (the fill is already there) */
		prog[n++] = 0x01;                /* add eax, ecx @ CODE+0xFFE ... */
		prog[n++] = 0xC8;                /*   ... its ip lands on the NEXT page */
		prog[n++] = 0xF4;                /* hlt @ CODE+0x1000 */
		prog[n++] = 0xEB;
		prog[n++] = 0xFD;                /* jmp $ */

		for (int arm = 0; arm < 3; arm++) {
			arm_setup = setup_matrix_shift;
			arm_run(arm, CODE, prog, n, 8192);
		}
		arm_setup = NULL;
		cmp_state(0, 1, "page edge: interp vs uop-cache");
		cmp_state(1, 2, "page edge: uop-cache vs compiled");
		/* exactly ONE recipe uop kept its checks: the page-edge add.  Every recipe ran
		 * once, so the dynamic inlined count IS the static recipe count. */
		CHECK_EQ(arm_stats[2].uops_checks_elided, arm_stats[2].inlined_uops - 1);
		CHECK(arm_stats[2].uops_checks_elided > 0xFF0);
		for (int arm = 0; arm < 3; arm++)
			CHECK_EQ((uint32_t)snap[arm].cpu.reg64[EAX], 6u); /* 1 + 5 */
	}
}

int main(void)
{
	for (int arm = 0; arm < 3; arm++) {
		snap[arm].ram = (uint8_t *)malloc(SNAP_RAM);
		if (snap[arm].ram == NULL) {
			fputs("cannot allocate the snapshot buffers\n", stderr);
			return 2;
		}
	}

	machine_init();
	uop_cache_enabled = true; /* the JIT rides the uop cache (jit_mem.h) */
	uop_cache_enabled = true; /* the JIT rides the uop cache (jit_mem.h) */
	CHECK(jit_codegen_available());

	test_encoder_pins();
	test_recipe_table();
	test_recipe();
	test_recipe_matrix();
	test_shape_matrix();
	test_tlb_fold_bits();
	test_fault_early_return();
	test_staircase();
	test_prefix_and_previous_ip();
	test_last_page();
	test_smc_abort();
	test_tail_exit();
	test_cross_return();
	test_exit_kinds();
	test_chain();
	test_continuation_elision();

	if (checks == 0) {
		fprintf(stderr, "jit_codegen_selftest: NO CHECKS RAN\n");
		return 1;
	}
	fprintf(stderr, "jit_codegen_selftest: %d checks, %d failures\n", checks, failures);
	jit_codegen_stats_print();
	return failures == 0 ? 0 : 1;
}
