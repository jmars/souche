
/*
 * uop_exec_selftest.c -- M2's GATE (PERF-4): the EXECUTOR's own suite.
 *
 *     make selftest        # builds and runs this binary with the other suites
 *
 * WHY A SEPARATE BINARY: same reason as M1's decode checker (uop_cache_selftest.c's header):
 * cpu_selftest.c supplies its own recording definitions of call_interrupt_vector/switch_seg/
 * the timer layer, so the REAL cpu.c/exceptions.c cannot be linked there -- and this suite
 * RUNS the machine, so it needs the real run loop, the real exceptions and a real device-free
 * machine.  It supplies its own io_port_* bridge and does NOT link src/hw, exactly as M1's
 * checker does (the device layer STOPS the machine for an unclaimed port).
 *
 * WHAT IT PINS, and why each one is the thing that can actually fail (plan ss8 step 6):
 *
 *  1. THE SMC SELFTEST (plan ss4.2).  A tiny image writes into ITS OWN EXECUTING PAGE -- it
 *     patches the immediate of the instruction that follows it -- and asserts that the
 *     MODIFIED instruction runs.  Plan ss7 is explicit that this is the only PROVABLE
 *     invalidation gate: the nasm corpus contains no self-modifying test (VERIFIED in M1), so
 *     a dropped write site would ship undetected.  Both halves of ss4 are needed for it: the
 *     page hook (which unlinks the stale block) and the abort (which stops the CURRENT block
 *     from executing its already-decoded uops).  The rig results/perf-4-mut-rig.sh fails
 *     this test on purpose by removing either one.
 *
 *  2. THE STAIRCASE PIN (plan ss5.2 -- "the part a naive cache gets wrong").  cache-on and
 *     cache-off must produce the SAME instruction_counter at every point the GUEST can
 *     observe it.  The observation here is the guest's own: the image executes `rdtsc`
 *     (= cpu.c's read_tsc -> microtick() -> instruction_counter/1000, exec_boundary.c:85) at
 *     chosen points and stores the low word into RAM, so the trace IS the counter staircase
 *     as the guest sees it -- including MID-BATCH reads, which is the only place a cache
 *     that flushes at block ends instead of at reference-batch ends differs from the
 *     reference.
 *
 *     DECODING THE TRACE EXACTLY (no tolerance is used, and this is the argument): read_tsc
 *     returns counter*1000 + extra, where extra is the "imprecise browser workaround"
 *     (cpu.c:264-274) bounded by tsc_resolution-1, and tsc_resolution is the SMALLEST
 *     observed difference between two consecutive reads.  Every difference is a multiple of
 *     1000 (one instruction = 1000 counts), so tsc_resolution >= 1000 always; and a WARMUP
 *     image (below) drives it to exactly 1000 before any measurement, so extra <= 999 in
 *     every measured run and `stored / 1000` is the counter EXACTLY.  tsc_offset is a
 *     multiple of 1000 for the same reason, so it cancels: the two runs' decoded traces are
 *     directly comparable.
 *     The warmup itself needs two consecutive reads one counter unit apart, which is what a
 *     PAGE-CROSSING `rdtsc` (the last two bytes of a page) gives: the page-cross break ends
 *     the batch at that single instruction (cpu.c:132's second clause), so the counter moves
 *     by exactly 1 between it and the next read.
 *
 *  3. THE STATE-KEY PIN (plan ss3).  The lookup key is (phys, state_flags); the same physical
 *     instructions, executed in a 32-bit and then a 16-bit code segment, must give the two
 *     DIFFERENT correct results -- which only happens if the second execution MISSED and was
 *     decoded for the new state.  A key that ignored `is_32` would re-run the first block and
 *     produce the first result twice.
 *
 * THE MACHINE is identity-mapped (CS/DS/SS base 0, paging off, 2 MiB of RAM), so virtual ==
 * physical and the guest can write RAM.  That is the opposite of M1's checker sandbox (which
 * bases every data segment ABOVE RAM so no probe can write); here the writes are the point.
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
 * the machine
 * ------------------------------------------------------------------ */

#define GUEST_RAM  (2u * 1024 * 1024)
#define GDT_BASE   0x5000u
#define SEG_CODE32 0x08u
#define SEG_DATA   0x10u
#define SEG_CODE16 0x20u
#define TRACE_ADDR 0x3000u

/* the one boundary symbol this binary supplies itself (the uop_cache_selftest.c:139
 * precedent): a benign, counting io_port_* bridge, so a probe that touches a port cannot stop
 * the machine the way src/hw's unclaimed-port handler does */
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

/* the GDT lives IN GUEST RAM (0x5000), so it has to be rebuilt whenever the RAM is reset */
static void build_tables(void)
{
	wr64(GDT_BASE + 0 * 8, 0);
	wr64(GDT_BASE + 1 * 8, make_desc(0, 0xFFFFF, 0x9A, 0xC)); /* code, D=1 -> 0x08 */
	wr64(GDT_BASE + 2 * 8, make_desc(0, 0xFFFFF, 0x92, 0xC)); /* data       -> 0x10 */
	wr64(GDT_BASE + 3 * 8, make_desc(0, 0xFFFFF, 0x92, 0xC)); /* data       -> 0x18 */
	wr64(GDT_BASE + 4 * 8, make_desc(0, 0xFFFFF, 0x9A, 0x8)); /* code, D=0 -> 0x20 */
	wr64(GDT_BASE + 5 * 8, make_desc(0, 0xFFFFF, 0x92, 0xC)); /* data       -> 0x28 */
	cpu->gdtr_offset = GDT_BASE;
	cpu->gdtr_size = 6 * 8 - 1;
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

	cpu->cr[0] = 1; /* PE, paging OFF */
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

	cpu_pristine = *cpu;
}

/* Put the whole machine back to the post-init state (no cache content, no counters). */
static void machine_reset(void)
{
	*cpu = cpu_pristine;
	memset(mem8, 0, GUEST_RAM);
	build_tables();
	uop_cache_clear();
	uop_stats_reset();
	interpreted_unimplemented = false;
}

/* the bounded run loop the machine is driven by -- do_many_cycles_native's job, with the
 * counter as the budget so a trace is comparable run to run */
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
 * THE SMC SELFTEST (plan ss4.2)
 *
 *     mov dword [0x100B], 0x2A    ; C7 05 0B 10 00 00 2A 00 00 00
 *     mov eax, 0                  ; B8 00 00 00 00   <- its immediate is at 0x100B
 *     mov [0x3000], eax           ; A3 00 30 00 00
 *     hlt                         ; F4
 *
 * None of these four instructions ends a reference batch (MEASURED: the flush-point
 * descriptors are 242 of 1182 and 0xC7/0xB8/0xA3 are not among them) and 0xC7 is not a block
 * end, so the decoder puts all four in ONE block and the executor would run the stale
 * `mov eax, 0` -- unless the write invalidates the page AND aborts the executing block.
 * ------------------------------------------------------------------ */
static void test_smc(void)
{
	uint64_t smc_aborts = 0, smc_invalidations = 0;
	static const uint8_t prog[] = {
		0xC7, 0x05, 0x0B, 0x10, 0x00, 0x00, 0x2A, 0x00, 0x00, 0x00, /* mov [0x100B], 0x2A */
		0xB8, 0x00, 0x00, 0x00, 0x00,                               /* mov eax, 0    */
		0xA3, 0x00, 0x30, 0x00, 0x00,                               /* mov [0x3000], eax */
		0xF4,                                                       /* hlt */
		0xEB, 0xFD,                                                 /* jmp hlt */
	};

	/* (a) WITH THE CACHE ON: the patched instruction must run */
	machine_reset();
	load(0x1000, prog, (int)sizeof prog);
	uop_cache_enabled = true;
	cpu->instruction_pointer = 0x1000;
	cpu->previous_ip = 0x1000;
	cpu->last_virt_eip = VIRT_EIP_NONE;
	run_machine(100);
	CHECK_EQ(ram32(0x3000), 0x2A);
	CHECK(uop_stats.aborts >= 1);      /* the executing block WAS aborted (ss4.2) */
	CHECK(uop_stats.invalidations >= 1); /* and the page's block WAS unlinked (ss4.1) */
	smc_aborts = uop_stats.aborts;
	smc_invalidations = uop_stats.invalidations;

	/* (b) WITH THE CACHE OFF: the interpreter always reads the bytes it executes, so the
	 * same answer is the reference's own.  Both halves matter: (a) alone could pass on an
	 * image whose block happened not to contain the patched instruction. */
	machine_reset();
	load(0x1000, prog, (int)sizeof prog);
	uop_cache_enabled = false;
	cpu->instruction_pointer = 0x1000;
	cpu->previous_ip = 0x1000;
	cpu->last_virt_eip = VIRT_EIP_NONE;
	run_machine(100);
	uop_cache_enabled = true;
	CHECK_EQ(ram32(0x3000), 0x2A);

	fprintf(stderr, "  smc: patched instruction executed, aborts %llu, invalidations %llu\n",
	        (unsigned long long)smc_aborts, (unsigned long long)smc_invalidations);
}

/* ------------------------------------------------------------------ *
 * THE RMW SMC SELFTEST (PERF-FIX-3/BLOCKER-1)
 *
 * THE SMM ABOVE CANNOT SEE THIS CLASS.  Its patch is `C7 05 ...` -- a plain mov, which the
 * port routes through the HOOKED write32.  The 165 read-modify-write instructions with a
 * memory operand go through SAFE_READ_WRITE8/16/32 instead (src/cpu/paging.h:143-203), and
 * those macros were the one writer family the M2 reconciliation could not see: the writer is
 * a MACRO, and results/perf-4-recon.sh's list B matched write*() CALLS, so a
 * `SAFE_READ_WRITE8(...)` invocation was invisible to it.  The macro's non-straddle arm
 * wrote guest RAM with no invalidation and no SMC abort for a whole milestone.
 *
 *     xor byte [0x100B], 0x2A   ; 80 35 0B 10 00 00 2A  -- SAFE_READ_WRITE8 (instr_80_6_mem)
 *     90 90 90                  ; padding: the immediate of the mov below must start at 0x100B
 *     mov eax, 0                ; B8 00 00 00 00   <- its immediate's LOW byte is at 0x100B
 *     mov [0x3000], eax         ; A3 00 30 00 00
 *     hlt                       ; F4
 *
 * All five instructions are in ONE block: 0x80 group's entries are neither block ends nor
 * flush points (src/cpu/uop_gen.c:274: `instr_80_6_mem` flags 0x45, iflags 0x00), nor are
 * 0xB8/0xA3 (MEASURED for the mov SMM above).  So the executing block holds the stale
 * `mov eax, 0` uop when the xor writes the page -- only the hook (unlink) AND the abort stop
 * it.  The patched program must store 0x2A.
 * ------------------------------------------------------------------ */
static void test_smc_rmw(void)
{
	uint64_t smc_aborts = 0, smc_invalidations = 0;
	static const uint8_t prog[] = {
		0x80, 0x35, 0x0B, 0x10, 0x00, 0x00, 0x2A, /* xor byte [0x100B], 0x2A */
		0x90, 0x90, 0x90,                         /* padding to 0x100A */
		0xB8, 0x00, 0x00, 0x00, 0x00,             /* 0x100A mov eax, 0 (imm low byte 0x100B) */
		0xA3, 0x00, 0x30, 0x00, 0x00,             /* 0x100F mov [0x3000], eax */
		0xF4,                                     /* 0x1014 hlt */
		0xEB, 0xFD,                               /* jmp hlt */
	};

	/* (a) CACHE ON: the patched instruction must run.  Without the hook the page keeps its
	 * block and the executing block keeps its uops, so this reads 0 -- MEASURED on the
	 * pre-fix build (results/perf-fix-3.md's G1). */
	machine_reset();
	load(0x1000, prog, (int)sizeof prog);
	uop_cache_enabled = true;
	cpu->instruction_counter = 0;
	cpu->instruction_pointer = 0x1000;
	cpu->previous_ip = 0x1000;
	cpu->last_virt_eip = VIRT_EIP_NONE;
	run_machine(100);
	CHECK_EQ(ram32(0x3000), 0x2A);
	CHECK(uop_stats.uops_executed > 0); /* the RMW ran as a UOP (not the interpreter) */
	CHECK(uop_stats.aborts >= 1);       /* the executing block WAS aborted (ss4.2) */
	CHECK(uop_stats.invalidations >= 1); /* and the page's block WAS unlinked (ss4.1) */
	smc_aborts = uop_stats.aborts;
	smc_invalidations = uop_stats.invalidations;

	/* (b) CACHE OFF: the interpreter always reads the bytes it executes, so the same answer
	 * IS the reference's. */
	machine_reset();
	load(0x1000, prog, (int)sizeof prog);
	uop_cache_enabled = false;
	cpu->instruction_pointer = 0x1000;
	cpu->previous_ip = 0x1000;
	cpu->last_virt_eip = VIRT_EIP_NONE;
	run_machine(100);
	uop_cache_enabled = true;
	CHECK_EQ(ram32(0x3000), 0x2A);

	fprintf(stderr, "  smc-rmw: xor byte [mem] patch executed, aborts %llu, invalidations %llu\n",
	        (unsigned long long)smc_aborts, (unsigned long long)smc_invalidations);
}

/* ------------------------------------------------------------------ *
 * THE STRADDLING-WRITE SELFTEST (PERF-FIX-3/SHOULD-FIX-1)
 *
 * The PLAIN ROUTERS (memory.c's write16/write32) hook only the FIRST page of a write, and a
 * 16-bit write at 0xFFF (32-bit at 0xFFD-0xFFF) reaches one byte into each of two physical
 * pages.  The "cannot straddle" comment that used to justify the single hook was true of
 * virt_boundary_write16, not of the router -- and the router's live callers are the virtio
 * used-ring writes (src/hw/virtio.c:1062/1068/1069/1074) at a GUEST-CHOSEN address.
 *
 *   page 1 (0x1000): 90 | A3 00 30 00 00 | F4 | EB FD     (nop; mov [0x3000], eax; hlt)
 *   the "device" write16(0xFFF, 0xB800) / write32(0xFFD, 0xB8000000) -> byte 0x1000 becomes
 *   0xB8, i.e. the cached block's first byte is now `mov eax, imm32` (its immediate is the
 *   old A3 00 30 00, then 00 F4 = add ah, dh; the other written bytes land in unused page 0).
 *
 * Run 1 stores eax (0x11) into 0x3000 and leaves the block cached.  Run 2 sets eax = 0x22 and
 * re-enters: with the fix the write dropped the page's block, so the bytes are re-decoded and
 * `mov [0x3000], eax` is GONE -- 0x3000 still holds 0x11.  Without it the stale block re-runs
 * and stores 0x22 (MEASURED on the pre-fix build -- results/perf-fix-3.md's G3).
 *
 * BOTH WIDTHS ARE PINNED, and each has its own hook in memory.c, so each needs its own arm
 * (the rig removes the write16 hook in one arm and the write32 hook in another).
 * ------------------------------------------------------------------ */
static void straddle_case(bool wide)
{
	static const uint8_t page1[] = {
		0x90,                                           /* 0x1000 nop */
		0xA3, 0x00, 0x30, 0x00, 0x00,                   /* 0x1001 mov [0x3000], eax */
		0xF4,                                           /* 0x1006 hlt */
		0xEB, 0xFD,                                     /* 0x1007 jmp hlt */
	};
	uint64_t invalidations_before;

	machine_reset();
	load(0x1000, page1, (int)sizeof page1);
	uop_cache_enabled = true;
	cpu->instruction_counter = 0;
	cpu->instruction_pointer = 0x1000;
	cpu->previous_ip = 0x1000;
	cpu->last_virt_eip = VIRT_EIP_NONE;
	write_reg32(EAX, 0x11);
	run_machine(50);
	CHECK_EQ(ram32(0x3000), 0x11); /* run 1: the block ran and is now cached on page 0x1000 */

	/* THE DEVICE-STYLE WRITE, on the ROUTER (not through safe_write16/virt_boundary).  With
	 * write16 two bytes cross 0xFFF; with write32 four cross 0xFFD. */
	invalidations_before = uop_stats.invalidations;
	if (wide) write32(0xFFD, 0xB8000000);
	else write16(0xFFF, 0xB800);
	CHECK_EQ(uop_stats.invalidations - invalidations_before, 1); /* the SECOND page's block */

	/* run 2: same machine, same cache, eax changed so the two outcomes differ */
	cpu->in_hlt = false;
	interpreted_unimplemented = false;
	cpu->instruction_pointer = 0x1000;
	cpu->previous_ip = 0x1000;
	cpu->last_virt_eip = VIRT_EIP_NONE;
	write_reg32(EAX, 0x22);
	run_machine(50);
	CHECK_EQ(ram32(0x3000), 0x11); /* NOT 0x22: the patched bytes were re-decoded */

	fprintf(stderr, "  straddle-%s: %s invalidated the next page (0x3000 stayed 0x%x)\n",
	        wide ? "w32" : "w16", wide ? "write32(0xFFD)" : "write16(0xFFF)",
	        (unsigned)ram32(0x3000));
}

static void test_straddle_write(void)
{
	straddle_case(false);
	straddle_case(true);
}

/* ------------------------------------------------------------------ *
 * THE EXCEPTION MACHINERY these two pins need: a real IDT of per-vector stubs
 *
 * Each vector's stub is `mov byte [ss:0x3000], <vector>; hlt` -- 9 bytes per vector, so the
 * observation IS the delivered vector, and a wrong vector cannot be mistaken for the right
 * one.  (m1c_selftest.c:136 has the descriptor encoder; it is not in a header, so it is
 * repeated here for the two vectors this suite needs to distinguish.)
 *
 * THE SS OVERRIDE IS LOAD-BEARING, not decoration: the guard-order pin runs with DS NULL
 * (that is what makes the address materialisation fault), and a stub that stored through the
 * DEFAULT segment would fault every time it ran -- an endless delivery loop whose stack
 * walked down into the IDT and corrupted it.  MEASURED (the first version of this pin): the
 * stub's own #GP re-entered vector 13, the frames descended from 0x7FFF4 past 0x6068, and
 * the delivery read a frame word as a gate descriptor ("invalid gate type").  SS is not null
 * in this machine (machine_init sets every segment_is_null to false), so the override is a
 * valid way for a handler to write while DS is broken.
 * ------------------------------------------------------------------ */
#define IDT_BASE      0x6000u
#define IDT_STUB_BASE 0x2400u
#define IDT_STUB_LEN  9u /* 36 C6 05 disp32 imm8 F4 */
#define VEC_RECORD    0x3000u
#define GP_HANDLER    0x1F00u /* SAME PAGE as the probe code: the fault must not end the
                               * reference's batch (cpu.c:139's page-cross clause) */

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

static void build_idt_stubs(void)
{
	for (uint32_t v = 0; v < 256; v++) {
		uint32_t a = IDT_STUB_BASE + IDT_STUB_LEN * v;
		mem8[a + 0] = 0x36; /* the SS override -- see the header */
		mem8[a + 1] = 0xC6; /* mov byte [VEC_RECORD], imm8 */
		mem8[a + 2] = 0x05;
		mem8[a + 3] = (uint8_t)(VEC_RECORD & 0xFF);
		mem8[a + 4] = (uint8_t)((VEC_RECORD >> 8) & 0xFF);
		mem8[a + 5] = (uint8_t)((VEC_RECORD >> 16) & 0xFF);
		mem8[a + 6] = (uint8_t)((VEC_RECORD >> 24) & 0xFF);
		mem8[a + 7] = (uint8_t)v;
		mem8[a + 8] = 0xF4; /* hlt */
		wr64(IDT_BASE + 8u * v,
		     mk_idt_entry(a, SEG_CODE32, INTR_GATE_INTERRUPT /* the port's 32-bit form */, 0,
		                  true));
	}
	cpu->idtr_offset = IDT_BASE;
	cpu->idtr_size = 256 * 8 - 1;
}

/* ------------------------------------------------------------------ *
 * THE GUARD-ORDER PIN (PERF-FIX-3's G4a -- the refutation of SHOULD-FIX-2a)
 *
 * THE REVIEW PRESCRIBED MATERIALISING THE ADDRESS BEFORE TESTING THE
 * task_switch_test[_mmx] GUARD, on the claim that the reference "resolves first and guards
 * inside the body".  IT IS THE OTHER WAY ROUND.  The guard is the generated body's
 * `instruction_prefix`, emitted BEFORE the mem call whose argument evaluates
 * modrm_resolve -- upstream v86/gen/generate_interpreter.js:233-251 (the prefix) and
 * :315-330 (the call), and the port's own generated interpreter in every case
 * (src/cpu/interpreter_gen.c:12010-12017 for `fxsave`, :4994-5011 for the FPU D8 group).
 * The plan says the same thing (perf-2-blockcache-plan.md ss4.4).  So with CR0.TS set AND a
 * segment that would fault, the reference delivers #NM (7) and never reaches the resolve;
 * the prescribed reorder would deliver #GP (13).
 *
 * This pin fixes both halves: DS is NULL (so a materialisation would raise #GP via
 * get_seg's trigger_gp) and CR0.TS is set (so the guard fails), and the instruction is
 * `D8 05 00 01 00 00` -- `fadd dword [0x100]`, an FPU mem form whose descriptor carries
 * UOP_DESC_TS_GUARD (src/cpu/uop_gen.c:541: flags 0x15).  The delivered vector is read out
 * of the IDT stub.  cache-ON and cache-OFF must both record 7: the executor and the port's
 * own interpreter (which IS the reference here) agree.
 * ------------------------------------------------------------------ */
static void test_guard_order(void)
{
	static const uint8_t prog[] = {
		0xD8, 0x05, 0x00, 0x01, 0x00, 0x00, /* fadd dword [0x100] -- a TS-guarded mem form */
		0xF4,                               /* hlt */
		0xEB, 0xFD,                         /* jmp hlt */
	};
	uint32_t on, off;

	for (int pass = 0; pass < 2; pass++) {
		bool cache = pass == 0;
		uint64_t uops_before;

		machine_reset();
		build_idt_stubs();
		load(0x1000, prog, (int)sizeof prog);
		mem8[VEC_RECORD] = 0;
		/* the two conditions the pin needs, set AFTER machine_reset (which restores the
		 * pristine cpu and wipes RAM) */
		cpu->cr[0] |= CR0_TS;              /* the guard fails -> #NM (misc_instr.c:658) */
		cpu->segment_is_null[DS] = true;   /* and a materialisation would -> #GP (modrm.c:91) */
		cpu->segment_limits[DS] = 0xFFFFFFFFu;
		uop_cache_enabled = cache;
		cpu->instruction_counter = 0;
		cpu->instruction_pointer = 0x1000;
		cpu->previous_ip = 0x1000;
		cpu->last_virt_eip = VIRT_EIP_NONE;
		uops_before = uop_stats.uops_executed;
		run_machine(50);
		if (cache) {
			/* THE PIN MUST HAVE TAKEN THE UOP PATH, or it would pass on the interpreter's
			 * own guard and say nothing about uop_exec */
			CHECK(uop_stats.uops_executed > uops_before);
			on = mem8[VEC_RECORD];
		}
		else {
			off = mem8[VEC_RECORD];
		}
	}
	uop_cache_enabled = true;

	CHECK_EQ(on, 7);  /* #NM: the guard failed FIRST, exactly as the reference */
	CHECK_EQ(off, 7);
	fprintf(stderr, "  guard-order: delivered vector cache-on %u / cache-off %u "
	                "(7 = #NM, 13 = #GP)\n", (unsigned)on, (unsigned)off);
}

/* ------------------------------------------------------------------ *
 * THE FAULTED FLUSH-POINT PIN (PERF-FIX-3's G4b -- SHOULD-FIX-2b, the half that stands)
 *
 * The generated body's resolve check is a `return` BEFORE its postfix
 * (gen/interpreter_c.js:261-273), so a FLUSH-flagged encoding that faults at materialisation
 * does NOT end a reference batch.  uop_exec used to return TRUE there, which the caller read
 * as "the body ran" and turned into uop_flush() -- a batch end the reference does not have.
 *
 * The pin counts them, because that is where the difference IS: uop_stats.flushes is the
 * executor's own tally of "reference batch ends reproduced" (uop_cache.h:350), and this
 * image has exactly FOUR of them, each derivable from the reference's own table:
 *
 *   1. the #GP handler's `mov ds, ax` -- a mov-to-sreg IS a block boundary (the plan's ss4.3
 *      list) and therefore a batch end (cpu.c:139);
 *   2. the handler's `iret` -- a block boundary;
 *   3. the cmpxchg itself, WHEN IT EXECUTES: its descriptor carries UOP_DESC_FLUSH
 *      (src/cpu/uop_gen.c:1061, iflags 0x01) -- it is the very instruction the staircase pin
 *      uses as a mid-block flush point (uop_exec_selftest.c's F1);
 *   4. the final hlt -- a block boundary.
 *
 * The FAULTED instance of the cmpxchg is NOT a fifth: the reference's resolve-fault `return`
 * skips its postfix.  MEASURED: the tree as built tallies 4; a build with `return true` on the
 * fault tallies 5 (results/perf-fix-3.md's G4 is the pair of pasted runs).
 *
 * THE INSTRUCTION IS `0F B0 05 00 01 00 00` = `cmpxchg byte [0x100], al`: a mem form whose
 * descriptor carries UOP_DESC_FLUSH and NOT UOP_DESC_BLOCK_END (src/cpu/uop_gen.c:1061), so
 * the pre-fix flush is a batch end in the MIDDLE of a block -- the reviewer's case exactly.
 * The handler is on the SAME PAGE (0x1F00) as the faulting instruction, so the reference's
 * page-cross clause (cpu.c:139) does not break the batch there either.  The handler repairs
 * DS, pops the #GP error code and irets, so the faulted instruction re-executes and the
 * program runs to its end.
 *
 * STATED LIMIT, measured rather than assumed: the guest-visible counter is NOT a
 * discriminator for this defect.  The spurious flush lands AFTER the handler has already run
 * (uop_exec returns only then), and the handler's iret is itself a batch end, so both builds
 * flush the same total at the same later point -- the guest's rdtsc read here is IDENTICAL
 * with and without the fix (this pin asserts that too, against cache-off).  What the fix
 * changes is the executor's batch bookkeeping (and, through uop_run's early return, the
 * budget window), which is what the tally reads.
 * ------------------------------------------------------------------ */
static void test_faulted_flush(void)
{
	/* 0x1000: cmpxchg byte [0x100], al   (FLUSH, faults at materialisation: DS is NULL)
	 * 0x1007: rdtsc                      (the guest's own observation of the counter)
	 * 0x1009: mov [0x3100], eax          (the trace; NOT 0x3000 -- that is VEC_RECORD)
	 * 0x100E: hlt                        (a reference batch end: the second one)
	 * 0x1F00: the #GP handler, SAME PAGE: mov ax, 0x10; mov ds, ax; pop eax (the error
	 *         code); iret              (the iret is a block boundary: the first one) */
	static const uint8_t prog[] = {
		0x0F, 0xB0, 0x05, 0x00, 0x01, 0x00, 0x00,
		0x0F, 0x31,
		0xA3, 0x00, 0x31, 0x00, 0x00,
		0xF4,
		0xEB, 0xFD,
	};
	static const uint8_t handler[] = {
		0x66, 0xB8, 0x10, 0x00, /* mov ax, 0x10 */
		0x8E, 0xD8,             /* mov ds, ax */
		0x58,                   /* pop eax -- the #GP error code */
		0xCF,                   /* iret */
	};
	uint64_t flushes_on, flushes_off;
	uint32_t read_on, read_off;

	for (int pass = 0; pass < 2; pass++) {
		bool cache = pass == 0;
		uint64_t flushes_before;

		machine_reset();
		build_idt_stubs();
		wr64(IDT_BASE + 8 * 13,
		     mk_idt_entry(GP_HANDLER, SEG_CODE32, INTR_GATE_INTERRUPT, 0, true));
		load(0x1000, prog, (int)sizeof prog);
		load(GP_HANDLER, handler, (int)sizeof handler);
		mem8[VEC_RECORD] = 0;
		cpu->segment_is_null[DS] = true; /* -> the materialisation's #GP (trigger_gp) */
		cpu->segment_limits[DS] = 0xFFFFFFFFu;
		uop_cache_enabled = cache;
		cpu->instruction_counter = 0;
		cpu->instruction_pointer = 0x1000;
		cpu->previous_ip = 0x1000;
		cpu->last_virt_eip = VIRT_EIP_NONE;
		write_reg32(EAX, 0);
		flushes_before = uop_stats.flushes;
		run_machine(200);
		if (cache) {
			flushes_on = uop_stats.flushes - flushes_before;
			read_on = ram32(0x3100);
		}
		else {
			flushes_off = uop_stats.flushes - flushes_before;
			read_off = ram32(0x3100);
		}
	}
	uop_cache_enabled = true;

	/* THE FAULT REALLY HAPPENED and the handler repaired the segment: the instruction
	 * after the faulted one ran (a `mov [0x3100], eax` that only executes if DS became
	 * valid), and the machine reached the hlt rather than stopping on the stub. */
	CHECK(cpu->in_hlt);
	/* the four reference batch ends listed in the header -- NOT a fifth for the fault */
	CHECK_EQ(flushes_on, 4);
	CHECK_EQ(flushes_off, 0); /* the cache is off: nothing tallies batch ends there */
	/* the guest's read (rdtsc, the low word) is the same either way -- the honest limit
	 * stated in the header: this defect is not guest-visible here */
	CHECK_EQ(read_on / 1000u, read_off / 1000u);

	fprintf(stderr, "  faulted-flush: cache-on batch ends %llu (must be 4), guest rdtsc "
	                "%u/%u\n", (unsigned long long)flushes_on, (unsigned)read_on, (unsigned)read_off);
}

/* ------------------------------------------------------------------ *
 * THE WARMUP: pin read_tsc's tsc_resolution to exactly 1000
 *
 *     jmp 0x1FFB                 ; E9 F6 0F 00 00
 *  0x1FFB: cmpxchg al, al        ; 0F B0 C0   <- a flush point: the batch ends HERE
 *  0x1FFE: rdtsc                 ; 0F 31      <- the last two bytes of the page: the
 *                                               page-cross break ends its batch at this
 *                                               one instruction, so the counter moves by
 *                                               exactly 1 before the next read
 *  0x2000: mov [0x3000], eax     ; A3 00 30 00 00
 *  0x2005: rdtsc                 ; 0F 31      <- the difference from the read above is
 *                                               exactly 1000 counts
 *  0x2007: mov [0x3004], eax     ; A3 04 30 00 00
 *  0x200C: hlt                   ; F4
 * ------------------------------------------------------------------ */
static void warmup_tsc(void)
{
	static const uint8_t prog[] = {
		0xE9, 0xF6, 0x0F, 0x00, 0x00,                   /* jmp 0x1FFB */
	};
	static const uint8_t page_end[] = {
		0x0F, 0xB0, 0xC0,                               /* 0x1FFB: cmpxchg al, al */
		0x0F, 0x31,                                     /* 0x1FFE: rdtsc (crosses) */
		0xA3, 0x00, 0x30, 0x00, 0x00,                   /* 0x2000: mov [0x3000], eax */
		0x0F, 0x31,                                     /* 0x2005: rdtsc */
		0xA3, 0x04, 0x30, 0x00, 0x00,                   /* 0x2007: mov [0x3004], eax */
		0xF4,                                           /* 0x200C: hlt */
	};

	machine_reset();
	load(0x1000, prog, (int)sizeof prog);
	load(0x1FFB, page_end, (int)sizeof page_end);
	uop_cache_enabled = false; /* the warmup is about the clock, not the cache */
	cpu->instruction_counter = 0;
	cpu->instruction_pointer = 0x1000;
	cpu->previous_ip = 0x1000;
	cpu->last_virt_eip = VIRT_EIP_NONE;
	run_machine(100);
	uop_cache_enabled = true;

	uint32_t r1 = ram32(0x3000), r2 = ram32(0x3004);
	CHECK(r1 != r2);
	/* the point of the warmup: the delta IS one instruction (1000 counts) */
	CHECK_EQ(r2 - r1, 1000);
	fprintf(stderr, "  warmup: tsc reads %u / %u (delta %u)\n",
	        (unsigned)r1, (unsigned)r2, (unsigned)(r2 - r1));
}

/* ------------------------------------------------------------------ *
 * THE STAIRCASE PIN (plan ss5.2)
 *
 *  0x1000: mov edi, 0x3000        ; BF 00 30 00 00
 *  0x1005: mov esi, 6             ; BE 06 00 00 00
 *  0x100A: xor eax, eax           ; 31 C0
 *  0x100C: cmpxchg al, al         ; 0F B0 C0   F1: a MID-BLOCK flush point
 *  0x100F: inc eax                ; 40
 *  0x1010: inc eax                ; 40
 *  0x1011: rdtsc                  ; 0F 31      R1: a MID-BATCH read -- the reference's
 *                                                counter here counts the batch that F1 ended
 *  0x1013: mov [edi], eax         ; 89 07
 *  0x1015: add edi, 4             ; 83 C7 04
 *  0x1018: rdtsc                  ; 0F 31      R2: the SAME reference batch as R1, so the
 *                                                two samples must carry the SAME counter
 *                                                (the observation a block-end-flushing
 *                                                cache gets wrong)
 *  0x101A: mov [edi], eax         ; 89 07
 *  0x101C: add edi, 4             ; 83 C7 04
 *  0x101F: cmpxchg al, al         ; 0F B0 C0   F2
 *  0x1022: dec esi                ; 4E
 *  0x1023: jnz 0x100C             ; 75 E7      (jcc: no_bib, so NOT a flush point)
 *  0x1025: jmp 0x1FFB             ; E9 D1 0F 00 00
 *  0x1FFB: cmpxchg al, al         ; F3
 *  0x1FFE: rdtsc                  ; R: PAGE-CROSS end (the batch flushes here, count 1)
 *  0x2000: mov [edi], eax         ; 89 07
 *  0x2002: add edi, 4             ; 83 C7 04
 *  0x2005: hlt                    ; F4
 *  0x2006: jmp 0x2005             ; EB FD
 * ------------------------------------------------------------------ */
static void test_staircase(void)
{
		static const uint8_t body[] = {
		0xBF, 0x00, 0x30, 0x00, 0x00,                   /* mov edi, 0x3000 */
		0xBE, 0x06, 0x00, 0x00, 0x00,                   /* mov esi, 6 */
		0x31, 0xC0,                                     /* xor eax, eax */
		0x0F, 0xB0, 0xC0,                               /* 0x100C F1 */
		0x40,                                           /* inc eax */
		0x40,                                           /* inc eax */
		0x0F, 0x31,                                     /* 0x1011 R1 */
		0x89, 0x07,                                     /* mov [edi], eax */
		0x83, 0xC7, 0x04,                               /* add edi, 4 */
		0x0F, 0x31,                                     /* 0x1018 R2 -- the SAME batch as R1 */
		0x89, 0x07,                                     /* mov [edi], eax */
		0x83, 0xC7, 0x04,                               /* add edi, 4 */
		0x0F, 0xB0, 0xC0,                               /* 0x101F F2 */
		0x4E,                                           /* dec esi */
		0x75, 0xE7,                                     /* 0x1023 jnz 0x100C */
		0xE9, 0xD1, 0x0F, 0x00, 0x00,                   /* 0x1025 jmp 0x1FFB */
	};
	static const uint8_t tail[] = {
		0x0F, 0xB0, 0xC0,                               /* 0x1FFB F3 (a flush point) */
		0x0F, 0x31,                                     /* 0x1FFE R13 (crosses the page) */
		0x89, 0x07,                                     /* 0x2000 mov [edi], eax */
		0x83, 0xC7, 0x04,                               /* 0x2002 add edi, 4 */
		0xFB,                                           /* 0x2005 STI: a BLOCK end that is NOT
		                                                 * a reference batch end -- the one
		                                                 * shape whose count MUST survive the
		                                                 * return to cycle_internal */
		0x90,                                           /* 0x2006 the shadow instruction, run
		                                                 * INSIDE instr_FB's body (and counted
		                                                 * by its own instruction_counter += 1,
		                                                 * instructions.c:2179) */
		0x40,                                           /* 0x2007 inc eax */
		0x0F, 0x31,                                     /* 0x2008 R14 (mid-batch: R13's count is
		                                                 * not flushed yet, only the sti's own +1) */
		0x89, 0x07,                                     /* 0x200A mov [edi], eax */
		0x83, 0xC7, 0x04,                               /* 0x200C add edi, 4 */
		0x0F, 0xB0, 0xC0,                               /* 0x200F F4 (the flush that ends the
		                                                 * batch the sti is part of) */
		0x0F, 0x31,                                     /* 0x2012 R15 */
		0x89, 0x07,                                     /* 0x2014 mov [edi], eax */
		0x83, 0xC7, 0x04,                               /* 0x2016 add edi, 4 */
		0xF4,                                           /* 0x2019 hlt */
		0xEB, 0xFD,                                     /* jmp 0x2019 */
	};
	enum { SAMPLES = 15 };
	uint32_t on[SAMPLES], off[SAMPLES], nop[SAMPLES];
	uint32_t eip_on, eip_off, count_on, count_off;
	static uint8_t ram_on[GUEST_RAM], ram_off[GUEST_RAM];

	/* THREE PASSES.  0 and 1 are the pin (cache on vs off).  Pass 2 is the STI-EQUIVALENCE
	 * ARM: the same image with the sti's two bytes (FB 90: the sti and the shadow instruction
	 * its body runs) replaced by two NOPs.  That substitution cannot change the counter
	 * staircase -- the sti's body counts the shadow itself (instructions.c:2179), so the batch
	 * totals are equal by construction -- and it is EXACT, unlike a hand-derived absolute
	 * number (my first attempt at one was wrong by 2, which is what the arm is for).  A cache
	 * that dropped the count at sti's block-end return reads a LOWER counter in pass 0 than in
	 * pass 2, and both traces are compared sample for sample. */
	for (int pass = 0; pass < 3; pass++) {
		bool cache = pass != 1;
		machine_reset();
		load(0x1000, body, (int)sizeof body);
		load(0x1FFB, tail, (int)sizeof tail);
		if (pass == 2) {
			mem8[0x2005] = 0x90; /* the sti -> nop */
			mem8[0x2006] = 0x90; /* the shadow nop stays a nop */
		}
		uop_cache_enabled = cache;
		cpu->instruction_counter = 0; /* so the two runs' decoded traces are comparable */
		cpu->instruction_pointer = 0x1000;
		cpu->previous_ip = 0x1000;
		cpu->last_virt_eip = VIRT_EIP_NONE;
		run_machine(1000);

		uint32_t *trace = pass == 0 ? on : (pass == 1 ? off : nop);
		for (int i = 0; i < SAMPLES; i++) trace[i] = ram32(TRACE_ADDR + 4u * (uint32_t)i);
		if (pass == 0) {
			eip_on = (uint32_t)cpu->instruction_pointer;
			count_on = cpu->instruction_counter;
			memcpy(ram_on, mem8, GUEST_RAM);
		}
		else if (pass == 1) {
			eip_off = (uint32_t)cpu->instruction_pointer;
			count_off = cpu->instruction_counter;
			memcpy(ram_off, mem8, GUEST_RAM);
		}
	}
	uop_cache_enabled = true;

	/* the trace is the guest's OWN view of the counter: read_tsc returns counter*1000 +
	 * extra with extra <= 999 (see the file header), so /1000 is exact */
	for (int i = 0; i < SAMPLES; i++) {
		uint32_t con = on[i] / 1000u, coff = off[i] / 1000u;
		if (con != coff) {
			fprintf(stderr, "FAIL staircase sample %d: cache-on counter %u, cache-off %u"
			                " (raw 0x%x / 0x%x)\n",
			        i, (unsigned)con, (unsigned)coff, (unsigned)on[i], (unsigned)off[i]);
		}
		CHECK_EQ(con, coff);
	}
	/* AND THE PIN MUST HAVE TAKEN THE MID-BATCH CASE: two consecutive reads inside ONE
	 * reference batch see the SAME counter, which is exactly the observation a cache that
	 * flushes at block ends gets wrong.  Without this assertion the test could pass on a
	 * trace whose reads all landed on batch boundaries. */
	{
		int same = 0;
		for (int i = 1; i < SAMPLES; i++) if (off[i] / 1000u == off[i - 1] / 1000u) same++;
		CHECK(same >= 1);
		/* THE STI EQUIVALENCE ARM (pass 2, see above).  Two claims, both derived from the
		 * image rather than from a run:
		 *  - every sample EXCEPT the one immediately after the sti is UNCHANGED by
		 *    substituting two nops for `FB 90`.  In particular the sample after the batch
		 *    ends (R15, index 14) must be identical: the batch's own instruction count plus
		 *    the shadow's is 8 either way, and a cache that dropped the count at the sti's
		 *    block-end return reads 5 there instead of 8 -- MEASURED, that was the defect
		 *    this arm exists for.
		 *  - the sample immediately after the sti (R14, index 13) is exactly ONE
		 *    instruction higher with the sti, because instr_FB counts the shadow
		 *    instruction itself (instructions.c:2179) while a nop contributes nothing until
		 *    the batch ends.  Exactly 1000 TSC counts -- the read's value differs from the
		 *    previous read's, so no imprecise-workaround extra is involved. */
		for (int i = 0; i < SAMPLES; i++) {
			if (i == 13) {
				CHECK_EQ(on[i] - nop[i], 1000);
				continue;
			}
			if (on[i] != nop[i]) {
				fprintf(stderr, "FAIL staircase sample %d: sti image %u, nop image %u\n",
				        i, (unsigned)(on[i] / 1000u), (unsigned)(nop[i] / 1000u));
			}
			CHECK_EQ(on[i] / 1000u, nop[i] / 1000u);
		}
		/* the first read is the 5th instruction of the program's first reference batch --
		 * 0x1000 mov edi, 0x1005 mov esi, 0x100A xor, 0x100C the flush point -- so the
		 * guest must see exactly 4.  An independently derivable value, not a snapshot:
		 * it is the count of instructions in the only batch that has completed. */
		CHECK_EQ(off[0] / 1000u, 4);
	}
	CHECK_EQ(eip_on, eip_off);
	CHECK_EQ(count_on, count_off);
	CHECK(memcmp(ram_on, ram_off, GUEST_RAM) == 0);

	fprintf(stderr, "  staircase: %d samples, counters off/on:", (int)SAMPLES);
	for (int i = 0; i < SAMPLES; i++) fprintf(stderr, " %u", (unsigned)(off[i] / 1000u));
	fprintf(stderr, " (final counter %u, eip 0x%x)\n", (unsigned)count_off, (unsigned)eip_off);
}

/* ------------------------------------------------------------------ *
 * THE STATE-KEY PIN (plan ss3)
 *
 *  0x1000: mov eax, 0x00010000     ; B8 00 00 01 00
 *  0x1005: jmpf 0x0008:0x00001300  ; EA 00 13 00 00 08 00
 *  0x1020: mov eax, 0x0001FFFF     ; B8 FF FF 01 00     (the second entry point)
 *  0x1025: jmpf 0x0020:0x00001300  ; EA 00 13 00 00 20 00
 *  0x1300: 40 EA 10 14 08 00 08 00 ; inc + a far jump whose TWO readings differ:
 *                                  ;   32-bit: inc eax ; EA off32=0x00081410 sel=0x0008
 *                                  ;   16-bit: inc ax  ; EA off16=0x1410     sel=0x0008
 *  0x1410: mov [0x3000], eax       ; A3 00 30 00 00    (the 16-BIT excursion's result)
 *  0x1415: hlt                     ; F4
 *  0x81410: mov [0x3004], eax      ; A3 04 30 00 00    (the 32-BIT excursion's result)
 *  0x81415: hlt                     ; F4
 *  0x81416: jmp 0x81415            ; EB FD
 *
 * The 32-bit excursion runs FIRST (it is entered from 0x1000) with eax = 0x00010000; its
 * result is the 32-bit `inc eax` = 0x00010001.  The 16-bit one is entered from 0x1020 with
 * eax = 0x0001FFFF and CS = 0x20: `inc ax` wraps the LOW half only, so eax becomes
 * 0x00010000.  The three candidate answers are all different (0x10001 / 0x10000 / 0x20000 --
 * the last is what running the 16-bit excursion with the 32-bit block would give).  Both
 * excursions use the SAME physical bytes at 0x1300, so the second can only be right if the
 * lookup MISSED on the state byte.
 * ------------------------------------------------------------------ */
static void test_state_key(void)
{
	static const uint8_t prog[] = {
		0xB8, 0x00, 0x00, 0x01, 0x00,                   /* mov eax, 0x00010000 */
		0xEA, 0x00, 0x13, 0x00, 0x00, 0x08, 0x00,       /* jmpf 0x0008:0x1300 */
	};
	static const uint8_t entry2[] = {
		0xB8, 0xFF, 0xFF, 0x01, 0x00,                   /* mov eax, 0x0001FFFF */
		0xEA, 0x00, 0x13, 0x00, 0x00, 0x20, 0x00,       /* jmpf 0x0020:0x1300 */
	};
	static const uint8_t shared[] = {
		0x40,                                           /* inc eax / inc ax */
		0xEA, 0x10, 0x14, 0x08, 0x00, 0x08, 0x00,       /* the two readings above */
	};
	static const uint8_t res16[] = {
		0xA3, 0x00, 0x30, 0x00, 0x00,                   /* 0x1410 mov [0x3000], eax */
		0xFF, 0x0D, 0x10, 0x30, 0x00, 0x00,             /* dec dword [0x3010] */
		0x75, 0x05,                                     /* jnz 0x1422 */
		0xF4,                                           /* hlt */
		0x90, 0x90, 0x90, 0x90,                         /* padding */
		0xE9, 0xD9, 0xFB, 0xFF, 0xFF,                   /* 0x1422 jmp 0x1000 */
	};
	static const uint8_t res32[] = {
		0xA3, 0x04, 0x30, 0x00, 0x00,                   /* 0x81410 mov [0x3004], eax */
		0xE9, 0x06, 0xFC, 0xF7, 0xFF,                   /* jmp 0x1020 */
	};

	machine_reset();
	load(0x1000, prog, (int)sizeof prog);
	load(0x1020, entry2, (int)sizeof entry2);
	load(0x1300, shared, (int)sizeof shared);
	load(0x1410, res16, (int)sizeof res16);
	load(0x81410, res32, (int)sizeof res32);
	mem8[0x3010] = 2; /* the loop counter: two passes, so the second one must HIT the cache */
	uop_cache_enabled = true;
	cpu->instruction_counter = 0;
	cpu->instruction_pointer = 0x1000;
	cpu->previous_ip = 0x1000;
	cpu->last_virt_eip = VIRT_EIP_NONE;
	run_machine(1000);

	CHECK_EQ(ram32(0x3004), 0x00010001); /* the 32-BIT excursion at 0x1300 */
	CHECK_EQ(ram32(0x3000), 0x00010000); /* the 16-BIT excursion at the same phys */
	CHECK_EQ(mem8[0x3010], 0);           /* both passes ran to the end */
	CHECK((cpu->sreg[CS] & 0xFFF8u) == SEG_CODE32);
	/* the SECOND pass re-enters the same physical code in the same two states: those lookups
	 * must HIT (the key is stable) -- and both results above are from the second pass, so a
	 * hit still executed the right uops.  A key that ignored state_flags would have made the
	 * 16-bit excursion reuse the 32-bit block and store 0x00010001 twice. */
	CHECK(uop_stats.hits >= 2);
	CHECK_EQ(ram32(0x3000), 0x00010000);
	CHECK(uop_stats.decodes >= 5);

	fprintf(stderr, "  state-key: 32-bit result 0x%x, 16-bit result 0x%x, decodes %llu,"
	                " hits %llu\n",
	        (unsigned)ram32(0x3004), (unsigned)ram32(0x3000),
	        (unsigned long long)uop_stats.decodes,
	        (unsigned long long)uop_stats.hits);
}

/* ------------------------------------------------------------------ *
 * THE SEGMENT-BASE PIN (PERF-5/M4a) -- the differential that gates uop_cache.c's INLINE
 * COPY of get_seg_prefix/get_seg.
 *
 * WHAT IT GATES.  `uop_seg_base` (src/cpu/uop_cache.c) is a static inline transcription of
 * modrm.c:44-99 -- the plan's M4 first item, taken as an INLINE rather than as a segment-base
 * cache, because the call it removes is the benchmark's largest remaining one (78.2M calls per
 * 200M instructions, MEASURED by the profile in results/perf-5-tune.md).  A transcription is a
 * claim of equivalence, and the claim is only worth what its gate is worth, so the gate is the
 * same differential M1 used for the modrm extraction: run the SAME guest bytes through the
 * CACHE (the inline copy) and through interpreted_run (the port's own modrm_resolve ->
 * get_seg_prefix -> get_seg, i.e. the reference) and require identical results.
 *
 * THE MATRIX, and why each row can fail:
 *
 *   phase A -- FLAT (every offset 0): the zero-base answer, which the flat fast path a
 *              reviewer might have written instead would special-case;
 *   phase B -- NON-FLAT, and the four segment bases are FOUR DIFFERENT NON-ZERO VALUES
 *              (DS 0x8000, SS 0x4000, FS 0x2000, ES 0x10000).  The image reads the SAME guest
 *              address (0x1400) four times: once through the DS default, once through the SS
 *              default (an EBP-relative form), and once through an explicit FS and an explicit
 *              ES override.  Distinguishing all four proves the prefix branch AND the
 *              default-segment choice survived the transcription -- an implementation that
 *              dropped `u->seg`, or that ignored the prefix field, gives four equal values
 *              here;
 *   phase C -- a NULL default segment: the DS access must raise #GP (13) through the inline
 *              copy's fault path.  A transcription that dropped the `segment_is_null` test
 *              reads offset 0 and records NO vector -- and the vector is the assertion.
 *
 * THE STORES ARE DS-RELATIVE ON PURPOSE (they are ordinary `mov [disp32], eax` forms), so in
 * phase B the trace lands at 0x3100 + DS's base; `trace` below is computed from the phase's own
 * DS offset rather than hard-coded.  VEC_RECORD (0x3000) is the vector recorder the IDT stubs
 * write (build_idt_stubs), and it is never NULL-segment-relative (the stubs use an SS override).
 *
 * A FALLBACK WOULD MAKE THIS VACUOUS, so it is asserted away: uop_stats.fallback_steps must be
 * 0 for every phase, i.e. every instruction of the image ran THROUGH the cache, not through the
 * interpreter's own path (which is one of the two legs being compared).
 * ------------------------------------------------------------------ */
static void test_seg_base(void)
{
	/* the four SAME-address reads, each stored at its own trace slot:
	 *   0x3100 = DS default, 0x3104 = SS default (EBP), 0x3108 = FS override,
	 *   0x310C = ES override */
	static const uint8_t prog[] = {
		0x26, 0x8B, 0x05, 0x00, 0x14, 0x00, 0x00, /* mov eax, es:[0x1400] */
		0x89, 0x05, 0x0C, 0x31, 0x00, 0x00,       /* mov [0x310C], eax */
		0x64, 0x8B, 0x05, 0x00, 0x14, 0x00, 0x00, /* mov eax, fs:[0x1400] */
		0x89, 0x05, 0x08, 0x31, 0x00, 0x00,       /* mov [0x3108], eax */
		0x8B, 0x05, 0x00, 0x14, 0x00, 0x00,       /* mov eax, [0x1400]    (DS default) */
		0x89, 0x05, 0x00, 0x31, 0x00, 0x00,       /* mov [0x3100], eax */
		0x8B, 0x45, 0x00,                         /* mov eax, [ebp+0]     (SS default) */
		0x89, 0x05, 0x04, 0x31, 0x00, 0x00,       /* mov [0x3104], eax */
		0xF4,                                     /* hlt */
		0xEB, 0xFD,                               /* jmp hlt */
	};
	/* phase C: ONE DS-relative load with DS null -- the fault path, and nothing else */
	static const uint8_t prog_null_ds[] = {
		0x8B, 0x05, 0x00, 0x14, 0x00, 0x00,
		0xF4,
		0xEB, 0xFD,
	};

	/* the phase table: the four segment bases, and what the image must read */
	struct {
		const char *name;
		const uint8_t *code;
		int len;
		int32_t base[6]; /* ES, CS, SS, DS, FS, GS -- the guest's own order */
		bool null_ds;
		uint32_t want[4]; /* DS, SS, FS, ES -- as stored into the trace */
		uint32_t want_vec;
	} phases[] = {
		{ "flat", prog, (int)sizeof prog,
		  { 0, 0, 0, 0, 0, 0 }, false,
		  { 0xA5A5A5A5u, 0xA5A5A5A5u, 0xA5A5A5A5u, 0xA5A5A5A5u }, 0 },
		{ "non-flat", prog, (int)sizeof prog,
		  { 0x10000, 0, 0x4000, 0x8000, 0x2000, 0 }, false,
		  { 0xA5A5A5A5u, 0x0FEEDFACu, 0x0C0FFEE1u, 0x0DEADBEEu }, 0 },
		{ "null-ds", prog_null_ds, (int)sizeof prog_null_ds,
		  { 0, 0, 0, 0, 0, 0 }, true,
		  { 0, 0, 0, 0 }, 13 /* #GP */ },
	};

	for (unsigned ph = 0; ph < sizeof phases / sizeof phases[0]; ph++) {
		uint32_t got[2][4];
		uint32_t vec[2];
		uint64_t uops[2];

		for (int pass = 0; pass < 2; pass++) {
			bool cache = pass == 0;
			uint32_t trace;

			machine_reset();
			build_idt_stubs();
			mem8[VEC_RECORD] = 0;

			for (int i = 0; i < 6; i++)
				cpu->segment_offsets[i] = phases[ph].base[i];
			cpu->segment_is_null[DS] = phases[ph].null_ds;
			cpu->segment_is_null[ES] = false;
			cpu->segment_is_null[FS] = false;
			update_state_flags(); /* the port's own choke point -- see perf-5-seg-audit.py */

			/* the four values the four segment bases must reach.  The writes are in
			 * REVERSE slot order (ES, FS, SS, DS) because in the FLAT phase all four
			 * targets coincide at 0x1400 and the DS write must be the last one: that
			 * makes the flat phase's expectation "0xA5A5A5A5 in all four slots", with
			 * the other three values still distinguishable in the non-flat phase. */
			uint32_t ds = 0x1400u + (uint32_t)phases[ph].base[DS];
			uint32_t ss = 0x1400u + (uint32_t)phases[ph].base[SS];
			uint32_t fs = 0x1400u + (uint32_t)phases[ph].base[FS];
			uint32_t es = 0x1400u + (uint32_t)phases[ph].base[ES];
			uint32_t v[4] = { 0xA5A5A5A5u, 0x0FEEDFACu, 0x0C0FFEE1u, 0x0DEADBEEu };
			memcpy(mem8 + es, &v[3], 4);
			memcpy(mem8 + fs, &v[2], 4);
			memcpy(mem8 + ss, &v[1], 4);
			memcpy(mem8 + ds, &v[0], 4);

			write_reg32(EBP, 0x1400);
			load(0x1000, phases[ph].code, phases[ph].len);
			trace = 0x3100u + (uint32_t)phases[ph].base[DS]; /* DS-relative stores */

			uop_cache_enabled = cache;
			cpu->instruction_counter = 0;
			cpu->instruction_pointer = 0x1000;
			cpu->previous_ip = 0x1000;
			cpu->last_virt_eip = VIRT_EIP_NONE;
			uops[pass] = uop_stats.uops_executed;
			run_machine(200);
			uops[pass] = uop_stats.uops_executed - uops[pass];

			got[pass][0] = ram32(trace + 0x00);
			got[pass][1] = ram32(trace + 0x04);
			got[pass][2] = ram32(trace + 0x08);
			got[pass][3] = ram32(trace + 0x0C);
			vec[pass] = mem8[VEC_RECORD];
		}

		/* THE ASSERTION THAT MAKES THE REST MEAN SOMETHING: the cache leg really ran
		 * through the cache (no mid-batch fallback to the very path it is compared with) */
		CHECK(uops[0] > 0);
		CHECK_EQ(uop_stats.fallback_steps, 0);

		for (int i = 0; i < 4; i++) {
			CHECK_EQ(got[0][i], got[1][i]);            /* cache == interpreter */
			CHECK_EQ(got[0][i], phases[ph].want[i]);   /* and both right */
		}
		CHECK_EQ(vec[0], vec[1]);
		CHECK_EQ(vec[0], phases[ph].want_vec);
		fprintf(stderr, "  seg-base %-8s: ds 0x%08x ss 0x%08x fs 0x%08x es 0x%08x vec %u\n",
		        phases[ph].name, got[0][0], got[0][1], got[0][2], got[0][3],
		        (unsigned)vec[0]);
	}
	uop_cache_enabled = true;
}

int main(void)
{
	machine_init();
	uop_stats_reset();

	test_smc();
	test_smc_rmw();      /* PERF-FIX-3: the SAFE_READ_WRITE* (RMW) patch */
	test_straddle_write(); /* PERF-FIX-3: the plain routers' second page */
	warmup_tsc();
	test_staircase();
	test_guard_order();  /* PERF-FIX-3: the guard runs BEFORE the resolve (#NM, not #GP) */
	test_faulted_flush(); /* PERF-FIX-3: a faulted FLUSH uop is not a reference batch end */
	test_state_key();
	test_seg_base();      /* PERF-5/M4a: the inline segment lookup vs the port's own resolver */

	fprintf(stderr, "uop_exec_selftest: %d checks, %d failures\n", checks, failures);
	if (checks == 0) {
		fprintf(stderr, "uop_exec_selftest: NO CHECKS RAN\n");
		return 1;
	}
	return failures != 0;
}
