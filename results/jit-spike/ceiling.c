/*
 * ceiling.c -- JIT-SPIKE Q1: WHAT IS THE CEILING?
 *
 * It measures TWO versions of the same guest loop, in ONE process, INTERLEAVED:
 *
 *   (a) THE CURRENT PATH -- the real uop cache executing the loop (the shipped engine: this
 *       harness links the core per-TU, exactly as src/cpu/uop_exec_selftest.c does) with
 *       --uop-cache on, and the interpreted path (cache off) beside it for context;
 *   (b) A HAND-WRITTEN NATIVE x86-64 BLOCK doing the same work: the guest's registers in host
 *       registers, the loop's back edge INSIDE the block, and the TLB/page fast path INLINED
 *       (three arms for the memory shape, so the address handling is BOUNDED rather than
 *       asserted: an inline TLB read, and the folded-page upper bound).
 *
 * WHY THE NATIVE ARM IS A CEILING AND NOT A PROJECTION: it removes exactly the costs that a
 * full trace-specializing JIT removes, and it KEEPS everything else.  results/jit-spike.md ss2.3
 * lists both directions (what it removes, and how the ratio therefore OVERSTATES a real JIT's
 * win) -- a ceiling whose assumptions are not stated is a projection wearing a measurement's
 * clothes.
 *
 * NOTE ON THE INCLUDES: "cpu/xxx.h" and -I src, NOT -I src/cpu -- src/cpu/string.h shadows the
 * SYSTEM <string.h> for every -I dir searched before the system's (MEASURED: 78 error lines from
 * `#include <string.h>` resolving to the port's own string.h), which is why this file lives
 * outside src/ and names the directory in its includes.
 *
 * Usage: ./ceiling [rounds] [iterations]
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

#include "cpu/cpu.h"
#include "cpu/cpu_state.h"
#include "cpu/exceptions.h"
#include "cpu/instructions.h"
#include "cpu/memory.h"
#include "cpu/modrm.h"
#include "cpu/paging.h"
#include "cpu/uop_cache.h"
#include "x64enc.h"

/* ------------------------------------------------------------------ *
 * the machine (the uop_exec_selftest.c fixture, trimmed)
 * ------------------------------------------------------------------ */
#define GUEST_RAM  (2u * 1024 * 1024)
#define GDT_BASE   0x5000u
#define SEG_CODE32 0x08u
#define SEG_DATA   0x10u

#define CODE_ADDR  0x1000u
#define DATA_ADDR  0x2000u

int32_t io_port_read8(int32_t port) { (void)port; return 0; }
int32_t io_port_read16(int32_t port) { (void)port; return 0; }
int32_t io_port_read32(int32_t port) { (void)port; return 0; }
void io_port_write8(int32_t port, int32_t value) { (void)port; (void)value; }
void io_port_write16(int32_t port, int32_t value) { (void)port; (void)value; }
void io_port_write32(int32_t port, int32_t value) { (void)port; (void)value; }
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
		cpu->segment_offsets[i] = 0; /* IDENTITY: virtual == physical */
	}
	cpu->segment_access_bytes[CS] = 0x9A;
	for (int i = 0; i < 8; i++) write_reg32(i, 0);
	write_reg32(ESP, 0x80000);
	update_state_flags();
	build_tables();
	interpreted_unimplemented = false;
}

/* ------------------------------------------------------------------ *
 * THE TWO SHAPES, built out of the instructions the boot's profile ranks highest
 * (results/perf-5/gprof-flat-after.txt -- results/jit-spike.md ss1)
 * ------------------------------------------------------------------ */

/* SHAPE M -- 6 guest instructions/iteration, 2 read memory operands, one jcc:
 *
 *   8B 06          mov   eax, [esi]          instr32_8B_mem      25.3M calls in the boot
 *   01 C3          add   ebx, eax            instr32_01_reg       6.85M
 *                  (01 /r is ADD r/m32, r32: D8 would be `add eax, ebx` -- the direction was
 *                   this harness's SECOND own bug, caught by the emulator-vs-native check below)
 *   0F B6 4E 04    movzx ecx, byte [esi+4]   instr32_0FB6_mem     9.27M
 *   21 D9          and   ecx, ebx            instr32_21_reg       5.85M
 *   4A             dec   edx                 instr32_4A           2.69M
 *   75 F3          jnz   0x1000              instr32_74/75        5.40M
 *   F4             hlt
 */
static const uint8_t SHAPE_M[] = {
	0x8B, 0x06,
	0x01, 0xC3,
	0x0F, 0xB6, 0x4E, 0x04,
	0x21, 0xD9,
	0x4A,
	0x75, 0xF3,
	0xF4,
};
#define SHAPE_M_INSTR 6

/* SHAPE A -- the same loop control, PURE ALU (no memory operand at all):
 *
 *   01 D8          add eax, ebx
 *   29 D9          sub ecx, ebx             instr32_29_reg       7.98M
 *   31 F7          xor edi, esi
 *   4A             dec edx
 *   75 F7          jnz 0x1000
 *   F4             hlt
 */
static const uint8_t SHAPE_A[] = {
	0x01, 0xD8,
	0x29, 0xD9,
	0x31, 0xF7,
	0x4A,
	0x75, 0xF7,
	0xF4,
};
#define SHAPE_A_INSTR 5

/* SHAPE M2 -- THE SAME INSTRUCTION MIX, TWICE THE BLOCK.  Its only purpose is to measure what
 * the emulator's cost per GUEST INSTRUCTION does when the same loop is decoded into a 12-uop
 * block instead of a 6-uop one: that difference IS the per-block-entry overhead (the uop_run
 * entry, the hash lookup, the block setup), which is the part a JIT removes outright and the
 * part that makes a "dispatch-only JIT buys nothing" claim true or false. */
static const uint8_t SHAPE_M2[] = {
	0x8B, 0x06,
	0x01, 0xC3,
	0x0F, 0xB6, 0x4E, 0x04,
	0x21, 0xD9,
	0x8B, 0x46, 0x08,
	0x01, 0xC3,
	0x0F, 0xB6, 0x4E, 0x0C,
	0x21, 0xD9,
	0x8B, 0x46, 0x10,
	0x01, 0xC3,
	0x4A,
	0x75, 0xE3,
	0xF4,
};
#define SHAPE_M2_INSTR 12

/* the data the memory shape reads: a byte pattern, read as dwords and bytes */
static void fill_data(void)
{
	for (uint32_t a = DATA_ADDR; a < DATA_ADDR + 4096; a++) mem8[a] = (uint8_t)((a * 7u) & 0xFF);
}

/* the C reference for shape M, so the native block's result is checked and not merely timed */
/* shape M2's reference: EBX accumulates the THREE `mov` loads (the movzx/and pairs are dead for
 * EBX), so the expected value is iters * (d[0] + d[8] + d[16]) mod 2^32 */
static int64_t reference_M2(uint32_t iters)
{
	uint32_t d0, d8, d16;
	memcpy(&d0, mem8 + DATA_ADDR, 4);
	memcpy(&d8, mem8 + DATA_ADDR + 8, 4);
	memcpy(&d16, mem8 + DATA_ADDR + 16, 4);
	return (int64_t)(uint32_t)((d0 + d8 + d16) * (uint32_t)iters);
}

static int64_t reference_M(uint32_t iters)
{
	uint32_t ebx = 0, esi = DATA_ADDR;
	for (uint32_t i = 0; i < iters; i++) {
			uint32_t eax;
		memcpy(&eax, mem8 + esi, 4);
		uint32_t ecx = mem8[esi + 4];
		ebx += eax;
		ecx &= ebx;
		(void)ecx;
	}
	return ebx;
}

/* ------------------------------------------------------------------ *
 * (a) THE CURRENT PATH: the machine, warmed, over the shape
 * ------------------------------------------------------------------ */

typedef struct arm_result {
	uint64_t ns;
	uint64_t guest_instrs;
} arm_result_t;

/* Load the shape and put the machine in its starting state.  THE UOP CACHE IS CLEARED HERE, and
 * that is not housekeeping: this harness writes guest code with memcpy, i.e. NOT through the
 * hooked write paths, so a block decoded from a PREVIOUS shape at the same address would still
 * be cached and would run (MEASURED in the uop cache's own suite: --uop-cache on + direct RAM
 * writes segfaults the m1c suite for exactly this reason).  Clearing is the honest fix for a
 * harness; a real machine never needs it, because its writes go through the hooks. */
static void arm_setup(const uint8_t *shape, int len)
{
	machine_init();
	fill_data();
	memcpy(mem8 + CODE_ADDR, shape, (size_t)len);
	uop_cache_clear();
}

static void run_shape(arm_result_t *out, uint32_t iters,
                      int instr_per_iter, int shape_is_mem, bool use_cache)
{
	uint64_t budget = (uint64_t)instr_per_iter * iters + 64;

	cpu->instruction_pointer = (int32_t)CODE_ADDR;
	cpu->previous_ip = (int32_t)CODE_ADDR;
	cpu->last_virt_eip = -1;
	cpu->in_hlt = false;
	cpu->pagefault = false;
	cpu->instruction_counter = 0;
	for (int i = 0; i < 8; i++) write_reg32(i, 0);
	write_reg32(EBX, 0);
	write_reg32(ESI, shape_is_mem ? DATA_ADDR : 0x12345u);
	write_reg32(EDX, iters);
	write_reg32(ESP, 0x80000);
	uop_cache_enabled = use_cache;

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	uint32_t guard = 0;
	while (!cpu->in_hlt && !interpreted_unimplemented
	       && cpu->instruction_counter < budget && guard++ < 400000000u) {
		cycle_internal();
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);

	out->ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull
	          + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
	out->guest_instrs = cpu->instruction_counter;
}

/* ------------------------------------------------------------------ *
 * (b) THE HAND-WRITTEN NATIVE BLOCKS
 * ------------------------------------------------------------------ */

/* the spike's TLB: 1024 entries of host page base, indexed by (guest >> 12) & 1023.  The guest
 * is 2 MiB (512 pages), so the mask never aliases in this harness; a real design's mask is a
 * function of the guest's address space, which is a parameter of the design, not of this number. */
#define TLB_ENTRIES 1024u
static void *spike_tlb[TLB_ENTRIES];

static void fill_tlb(void)
{
	memset(spike_tlb, 0, sizeof spike_tlb);
	for (uint32_t page = 0; page < (GUEST_RAM >> 12); page++) {
		spike_tlb[(page) & (TLB_ENTRIES - 1)] = mem8 + (page << 12);
	}
}

typedef int64_t (*blk_M_t)(void **tlb, uint32_t n, uint32_t addr);
typedef int64_t (*blk_Mfold_t)(const uint8_t *data, uint32_t n);
typedef int64_t (*blk_A_t)(uint32_t n, uint32_t seed);

static uint8_t *emit_M_inline_tlb(uint32_t *len_out)
{
	uint8_t *buf = (uint8_t *)malloc(4096);
	enc_t e;
	e_init(&e, buf, 4096);

	/* THE PROLOGUE A NATIVE BLOCK NEEDS: the guest's EBX lives in the host's RBX, and RBX is
	 * CALLEE-SAVED under the host ABI, so the block must save it.  (MEASURED, and it was this
	 * harness's own first bug: without it the block silently corrupted the C caller -- the
	 * checks below compared a value the block had already destroyed.) */
	e_push_r64(&e, RBX);
	e_mov_r32_r32(&e, R8, RSI);      /* r8d = n (the loop counter) */
	e_mov_r32_r32(&e, R11, RDX);     /* r11d = the guest address (guest ESI's value) */
	e_xor_r32_r32(&e, RBX, RBX);     /* guest ebx = 0 */
	size_t loop = e.n;

	/* operand 1: mov eax, [guest_addr] */
	e_mov_r32_r32(&e, R9, R11);
	e_mov_r32_r32(&e, R10, R11);
	e_shr_r32_imm8(&e, R9, 12);
	e_and_r32_imm32(&e, R9, (int32_t)(TLB_ENTRIES - 1));
	e_mov_r64_mem_sib(&e, R9, RDI, R9, 8, 0);
	e_test_r64_r64(&e, R9, R9);
	size_t miss1 = e_jcc_rel8(&e, CC_E);
	e_and_r32_imm32(&e, R10, 0xFFF);
	e_mov_r32_mem_sib(&e, RAX, R9, R10, 0);
	e_add_r32_r32(&e, RBX, RAX);

	/* operand 2: movzx ecx, byte [guest_addr+4] */
	e_mov_r32_r32(&e, R9, R11);
	e_mov_r32_r32(&e, R10, R11);
	e_shr_r32_imm8(&e, R9, 12);
	e_and_r32_imm32(&e, R9, (int32_t)(TLB_ENTRIES - 1));
	e_mov_r64_mem_sib(&e, R9, RDI, R9, 8, 0);
	e_test_r64_r64(&e, R9, R9);
	size_t miss2 = e_jcc_rel8(&e, CC_E);
	e_and_r32_imm32(&e, R10, 0xFFF);
	e_movzx_r32_m8(&e, RCX, R9, R10, 4);
	e_and_r32_r32(&e, RCX, RBX);

	e_dec_r32(&e, R8);
	e_jcc_rel8(&e, CC_NE);
	e_patch_rel8(&e, e.n - 1, loop);

	e_mov_r32_r32(&e, RAX, RBX);     /* the result: the accumulator */
	e_pop_r64(&e, RBX);
	e_ret(&e);

	size_t bail = e.n;               /* the miss arm: exit the block with the partial result */
	e_mov_r32_r32(&e, RAX, RBX);
	e_pop_r64(&e, RBX);
	e_ret(&e);
	e_patch_rel8(&e, miss1, bail);
	e_patch_rel8(&e, miss2, bail);

	if (e.err) {
		fprintf(stderr, "encoder: block overflowed / patch out of range\n");
		exit(2);
	}
	*len_out = (uint32_t)e.n;
	return buf;
}

/* THE FOLDED UPPER BOUND: the page base is proven constant across the block, so there is no
 * per-operand translation at all -- 1 host instruction per memory operand.  This is not
 * something a block compiler can always assume; it is the other END of the range, reported so
 * the ceiling is a range and not a point. */
static uint8_t *emit_M_folded(uint32_t *len_out)
{
	uint8_t *buf = (uint8_t *)malloc(4096);
	enc_t e;
	e_init(&e, buf, 4096);

	e_push_r64(&e, RBX);
	e_mov_r32_r32(&e, R8, RSI);
	e_xor_r32_r32(&e, RBX, RBX);
	size_t loop = e.n;
	e_mov_r32_mem(&e, RAX, RDI, 0);
	e_add_r32_r32(&e, RBX, RAX);
	e_movzx_r32_m8(&e, RCX, RDI, -1, 4);
	e_and_r32_r32(&e, RCX, RBX);
	e_dec_r32(&e, R8);
	e_jcc_rel8(&e, CC_NE);
	e_patch_rel8(&e, e.n - 1, loop);
	e_mov_r32_r32(&e, RAX, RBX);
	e_pop_r64(&e, RBX);
	e_ret(&e);
	if (e.err) { fprintf(stderr, "encoder: folded block overflowed\n"); exit(2); }
	*len_out = (uint32_t)e.n;
	return buf;
}

static uint8_t *emit_A(uint32_t *len_out)
{
	uint8_t *buf = (uint8_t *)malloc(4096);
	enc_t e;
	e_init(&e, buf, 4096);

	e_push_r64(&e, RBX);
	e_mov_r32_r32(&e, R8, RDI);      /* r8d = n */
	e_xor_r32_r32(&e, RAX, RAX);
	e_xor_r32_r32(&e, RBX, RBX);
	e_mov_r32_r32(&e, RCX, RSI);     /* guest ecx = seed */
	e_xor_r32_r32(&e, RDI, RDI);     /* guest edi = 0 */
	size_t loop = e.n;
	e_add_r32_r32(&e, RAX, RBX);
	e_sub_r32_r32(&e, RCX, RBX);
	e_xor_r32_r32(&e, RDI, RSI);
	e_dec_r32(&e, R8);
	e_jcc_rel8(&e, CC_NE);
	e_patch_rel8(&e, e.n - 1, loop);
	e_mov_r32_r32(&e, RAX, RDI);     /* the result: the guest's EDI */
	e_pop_r64(&e, RBX);
	e_ret(&e);
	if (e.err) { fprintf(stderr, "encoder: A block overflowed\n"); exit(2); }
	*len_out = (uint32_t)e.n;
	return buf;
}

static void *publish(const uint8_t *code, uint32_t len)
{
	uint8_t *p = (uint8_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE,
	                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) { perror("mmap"); exit(2); }
	memcpy(p, code, len);
	if (mprotect(p, 4096, PROT_READ | PROT_EXEC) != 0) { perror("mprotect"); exit(2); }
	return p;
}

/* ------------------------------------------------------------------ *
 * timing
 * ------------------------------------------------------------------ */
static uint64_t now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : (x > y);
}

int main(int argc, char **argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 5;
	uint32_t iters = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 2000000u;
	if (rounds > 64) rounds = 64;

	machine_init();
	uop_cache_enabled = true;
	if (!uop_cache_init()) { fputs("uop_cache_init failed\n", stderr); return 2; }
	fill_data();
	fill_tlb();

	uint32_t len_inline = 0, len_fold = 0, len_a = 0;
	uint8_t *code_inline = emit_M_inline_tlb(&len_inline);
	uint8_t *code_fold = emit_M_folded(&len_fold);
	uint8_t *code_a = emit_A(&len_a);
	blk_M_t blk_inline = (blk_M_t)publish(code_inline, len_inline);
	blk_Mfold_t blk_fold = (blk_Mfold_t)publish(code_fold, len_fold);
	blk_A_t blk_a = (blk_A_t)publish(code_a, len_a);

	e_dump("shape M, inline TLB", code_inline, len_inline);
	e_dump("shape M, folded page", code_fold, len_fold);
	e_dump("shape A, pure ALU", code_a, len_a);
	printf("iterations per arm: %u, rounds: %d\n", iters, rounds);

	/* CORRECTNESS FIRST: a fast wrong block is not a ceiling, it is a bug */
	arm_setup(SHAPE_M, (int)sizeof SHAPE_M);
	int64_t want = reference_M(iters);
	int64_t got = blk_inline(spike_tlb, iters, DATA_ADDR);
	printf("shape M: reference sum 0x%llx, native inline-TLB 0x%llx -> %s\n",
	       (unsigned long long)want, (unsigned long long)got,
	       want == got ? "MATCH" : "MISMATCH");
	int64_t gotf = blk_fold(mem8 + DATA_ADDR, iters);
	printf("shape M: reference sum 0x%llx, native folded     0x%llx -> %s\n",
	       (unsigned long long)want, (unsigned long long)gotf,
	       want == gotf ? "MATCH" : "MISMATCH");
	int64_t wanta = (iters & 1u) ? 0x12345 : 0;   /* guest edi after `xor edi, esi` x iters */
	int64_t gota = blk_a(iters, 0x12345u);
	printf("shape A: reference edi 0x%llx, native 0x%llx -> %s\n",
	       (unsigned long long)wanta, (unsigned long long)gota,
	       wanta == gota ? "MATCH" : "MISMATCH");
	if (want != got || want != gotf || wanta != gota) {
		fputs("a native block disagrees with the C reference: NOT a ceiling measurement\n",
		      stderr);
		return 1;
	}

	/* the same check for the 12-uop variant */
	arm_result_t check2;
	arm_setup(SHAPE_M2, (int)sizeof SHAPE_M2);
	run_shape(&check2, iters, SHAPE_M2_INSTR, 1, true);
	int64_t want2 = reference_M2(iters);
	printf("shape M2 (12-uop block): reference 0x%llx, emulator ebx 0x%llx -> %s\n",
	       (unsigned long long)want2, (unsigned long long)(uint32_t)read_reg32(EBX),
	       (uint32_t)read_reg32(EBX) == (uint32_t)want2 ? "MATCH" : "MISMATCH");
	if ((uint32_t)read_reg32(EBX) != (uint32_t)want2) return 1;

	/* the emulator's own path check: with the cache ON, the same shape through the real
	 * machine must produce the reference sum in EBX */
	arm_setup(SHAPE_M, (int)sizeof SHAPE_M);
	arm_result_t check;
	run_shape(&check, iters, SHAPE_M_INSTR, 1, true);
	printf("  [check] instrs=%llu in_hlt=%d unimplemented=%d eip=0x%x cs=0x%x uop_pending=%u\n",
	       (unsigned long long)check.guest_instrs, (int)cpu->in_hlt, (int)interpreted_unimplemented,
	       (unsigned)cpu->instruction_pointer,
	       (unsigned)cpu->sreg[CS], (unsigned)uop_pending);
	printf("shape M through the emulator (cache on): ebx 0x%llx -> %s\n",
	       (unsigned long long)read_reg32(EBX),
	       (uint32_t)read_reg32(EBX) == (uint32_t)want ? "MATCH" : "MISMATCH");
	if ((uint32_t)read_reg32(EBX) != (uint32_t)want) return 1;

	/* warmups: the first emulator round pays the decode, and the native arms' branch
	 * predictors are cold.  A warmup round per arm, discarded. */
	arm_setup(SHAPE_M, (int)sizeof SHAPE_M);
	run_shape(&check, 200000, SHAPE_M_INSTR, 1, true);
	arm_setup(SHAPE_A, (int)sizeof SHAPE_A);
	run_shape(&check, 200000, SHAPE_A_INSTR, 0, true);
	blk_inline(spike_tlb, 200000, DATA_ADDR);
	blk_fold(mem8 + DATA_ADDR, 200000);
	blk_a(200000, 0x12345u);

	/* PER-ARM REPETITION, and why it is not 1 for every arm: the native arms are ~0.16 ns per
	 * guest instruction, so ONE call with EMU_ITERS iterations lasts ~2 ms -- a duration that
	 * a host at load 24 turns into noise (MEASURED: the first version of this harness read a
	 * native median of 0.329 ns/GI against a best of 0.162, i.e. the median was the host, not
	 * the block).  Each arm is therefore repeated until it lasts tens of ms, and the figure
	 * reported is ns PER GUEST INSTRUCTION, which is invariant to the repetition. */
	enum { ARM_M_EMU, ARM_M_EMU_NOCACHE, ARM_M2_EMU, ARM_M_INLINE, ARM_M_FOLD, ARM_A_EMU,
	       ARM_A_EMU_NOCACHE, ARM_A_NATIVE, ARM_COUNT };
	static const char *names[ARM_COUNT] = {
		"M emulator (uop cache on)", "M emulator (cache off)",
		"M2 emulator (12-uop block)", "M native inline-TLB",
		"M native folded-page", "A emulator (uop cache on)", "A emulator (cache off)",
		"A native",
	};
	uint64_t samples[ARM_COUNT][64];
	uint64_t instrs[ARM_COUNT];

	/* the native arms repeat the block; the emulator arms run it once (one run is already
	 * 10-20 ms) */
	const int native_reps = 512;

	for (int r = 0; r < rounds; r++) {
		for (int arm = 0; arm < ARM_COUNT; arm++) {
			uint64_t gi = 0;
			uint64_t t0 = now_ns();
			switch (arm) {
			case ARM_M_EMU:
				arm_setup(SHAPE_M, (int)sizeof SHAPE_M);
				run_shape(&check, iters, SHAPE_M_INSTR, 1, true);
				gi = check.guest_instrs;
				break;
			case ARM_M_EMU_NOCACHE:
				arm_setup(SHAPE_M, (int)sizeof SHAPE_M);
				run_shape(&check, iters, SHAPE_M_INSTR, 1, false);
				gi = check.guest_instrs;
				break;
			case ARM_M2_EMU:
				arm_setup(SHAPE_M2, (int)sizeof SHAPE_M2);
				run_shape(&check, iters, SHAPE_M2_INSTR, 1, true);
				gi = check.guest_instrs;
				break;
			case ARM_M_INLINE:
				for (int k = 0; k < native_reps; k++) blk_inline(spike_tlb, iters, DATA_ADDR);
				gi = (uint64_t)SHAPE_M_INSTR * iters * (uint64_t)native_reps;
				break;
			case ARM_M_FOLD:
				for (int k = 0; k < native_reps; k++) blk_fold(mem8 + DATA_ADDR, iters);
				gi = (uint64_t)SHAPE_M_INSTR * iters * (uint64_t)native_reps;
				break;
			case ARM_A_EMU:
				arm_setup(SHAPE_A, (int)sizeof SHAPE_A);
				run_shape(&check, iters, SHAPE_A_INSTR, 0, true);
				gi = check.guest_instrs;
				break;
			case ARM_A_EMU_NOCACHE:
				arm_setup(SHAPE_A, (int)sizeof SHAPE_A);
				run_shape(&check, iters, SHAPE_A_INSTR, 0, false);
				gi = check.guest_instrs;
				break;
			case ARM_A_NATIVE:
				for (int k = 0; k < native_reps; k++) blk_a(iters, 0x12345u);
				gi = (uint64_t)SHAPE_A_INSTR * iters * (uint64_t)native_reps;
				break;
			}
			samples[arm][r] = now_ns() - t0;
			instrs[arm] = gi;
		}
	}

	printf("\n%-28s %12s %10s %12s %12s %12s\n",
	       "arm", "guest instr", "best ms", "best ns/GI", "med ns/GI", "best MIPS");
	uint64_t med[ARM_COUNT], best[ARM_COUNT];
	for (int arm = 0; arm < ARM_COUNT; arm++) {
		uint64_t sorted[64];
		memcpy(sorted, samples[arm], sizeof(uint64_t) * (size_t)rounds);
		qsort(sorted, (size_t)rounds, sizeof(uint64_t), cmp_u64);
		best[arm] = sorted[0];
		med[arm] = sorted[rounds / 2];
		printf("%-28s %12llu %10.1f %12.3f %12.3f %12.2f\n", names[arm],
		       (unsigned long long)instrs[arm],
		       (double)best[arm] / 1e6,
		       (double)best[arm] / (double)instrs[arm],
		       (double)med[arm] / (double)instrs[arm],
		       (double)instrs[arm] / (double)best[arm] * 1000.0);
	}

	printf("\nRATIO (same process, same work, interleaved; %d rounds, host load in the record):\n",
	       rounds);
	/* THE RATIO IS PER GUEST INSTRUCTION, not per round: the native arms repeat the block 64
	 * times (above), so the raw times are not comparable -- only ns/GI is. */
#define RATIO(emulator, native, field) \
	(((double)field[emulator] / (double)instrs[emulator]) / \
	 ((double)field[native] / (double)instrs[native]))
	printf("  from BEST round   -- shape M: emulator/native-inline-TLB %.1fx,"
	       " emulator/native-folded %.1fx, shape A: %.1fx\n",
	       RATIO(ARM_M_EMU, ARM_M_INLINE, best),
	       RATIO(ARM_M_EMU, ARM_M_FOLD, best),
	       RATIO(ARM_A_EMU, ARM_A_NATIVE, best));
	printf("  from MEDIAN round -- shape M: emulator/native-inline-TLB %.1fx,"
	       " emulator/native-folded %.1fx, shape A: %.1fx\n",
	       RATIO(ARM_M_EMU, ARM_M_INLINE, med),
	       RATIO(ARM_M_EMU, ARM_M_FOLD, med),
	       RATIO(ARM_A_EMU, ARM_A_NATIVE, med));
	printf("  PER-BLOCK-ENTRY OVERHEAD, measured by block length alone: the SAME mix in a"
	       " 12-uop block costs %.2f ns/GI vs %.2f in the 6-uop block\n",
	       (double)best[ARM_M2_EMU] / (double)instrs[ARM_M2_EMU],
	       (double)best[ARM_M_EMU] / (double)instrs[ARM_M_EMU]);
	printf("  what the uop cache already buys on these shapes: M on/off = %.2fx,"
	       " A on/off = %.2fx\n",
	       (double)med[ARM_M_EMU_NOCACHE] / (double)med[ARM_M_EMU],
	       (double)med[ARM_A_EMU_NOCACHE] / (double)med[ARM_A_EMU]);

	FILE *l = fopen("/proc/loadavg", "r");
	if (l) {
		char b[128];
		if (fgets(b, sizeof b, l)) printf("host loadavg: %s", b);
		fclose(l);
	}
	uop_stats_print();
	fflush(stdout);
	return 0;
}
