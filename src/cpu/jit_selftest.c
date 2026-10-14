/*
 * jit_selftest.c -- THE SUBSTRATE'S GATE (unit JIT-SPIKE, HANDOFF=v86c-jit).  `make jit-selftest`.
 *
 * WHAT IT PINS, and why each one is the thing that can actually FAIL:
 *
 *  1. THE BLOCK RUNS.  A tiny hand-written x86-64 block is written into a page reserved from
 *     the pool (RW), published (mprotect RX) and CALLED; its result is checked against a C
 *     re-implementation.  Without this the rest is theatre: the whole layer exists to make
 *     bytes executable.
 *
 *  2. THE STALE-BLOCK PROBE -- the property a JIT lives or dies by (the brief's words).  A
 *     block is "compiled" from bytes on a guest code page and bound under the uop cache's key
 *     for that page; then the GUEST PATCHES ITS OWN CODE PAGE through the real instruction
 *     stream (the machine runs `mov dword [patch_addr], imm32`), and the probe asserts that
 *     (a) the cached block is GONE from the lookup and (b) re-running gives the NEW immediate.
 *     The write goes through the PERF-FIX-3 hook points and the uop cache's
 *     uop_invalidate_page -- i.e. through the real wiring, not by calling the invalidator
 *     directly, which is the only way this test can catch a wiring mistake.
 *     IT IS SHOWN ABLE TO FAIL by results/jit-spike-mut-rig.sh: arm 1 removes the forward call
 *     in uop_invalidate_page, arm 2 removes the drop in jit_code_invalidate_page, and the probe
 *     then executes the STALE block and fails.
 *
 *  3. THE POOL'S REFUSALS ARE LOUD (no silent stubs): publishing a pointer that is not in the
 *     pool is refused, publishing twice is refused, and exhausting the pool is reported (the
 *     pool is exhausted on purpose here by reserving every page).
 *
 *  4. THE EXECUTING PAGE'S RECYCLE IS DEFERRED.  Invalidating the page of the block that is
 *     "executing" must not flip that page to RW under it: the drop is deferred to
 *     jit_block_exit, and the abort flag is set for the executor to read.  The probe checks the
 *     page is NOT back in the free list while inside, and IS after.
 *
 * THE MACHINE.  Identity-mapped (CS/DS/SS base 0, paging off, 2 MiB), the same fixture as
 * src/cpu/uop_exec_selftest.c -- deliberately copied rather than shared, because a shared
 * harness would have to be in the shipped link and the shipped link must not grow a test
 * harness.  This binary supplies its own io_port_* bridge and does NOT link src/hw (the device
 * layer STOPS the machine for an unclaimed port).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "cpu_state.h"
#include "exceptions.h"
#include "instructions.h"
#include "jit_mem.h"
#include "memory.h"
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
 * the machine (the uop_exec_selftest.c fixture, trimmed)
 * ------------------------------------------------------------------ */

#define GUEST_RAM  (2u * 1024 * 1024)
#define GDT_BASE   0x5000u
#define SEG_CODE32 0x08u
#define SEG_DATA   0x10u

static long io_port_reads, io_port_writes;
int32_t io_port_read8(int32_t port) { io_port_reads++; (void)port; return 0; }
int32_t io_port_read16(int32_t port) { io_port_reads++; (void)port; return 0; }
int32_t io_port_read32(int32_t port) { io_port_reads++; (void)port; return 0; }
void io_port_write8(int32_t port, int32_t value) { io_port_writes++; (void)port; (void)value; }
void io_port_write16(int32_t port, int32_t value) { io_port_writes++; (void)port; (void)value; }
void io_port_write32(int32_t port, int32_t value) { io_port_writes++; (void)port; (void)value; }
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
	interpreted_unimplemented = false;
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

/* the bounded run loop (do_many_cycles_native's job, the counter as the budget) */
static void run_machine(uint32_t budget)
{
	uint32_t start = cpu->instruction_counter;
	uint32_t guard = 0;
	while (!cpu->in_hlt && !interpreted_unimplemented
	       && (cpu->instruction_counter - start) < budget
	       && guard++ < 100000u) {
		cycle_internal();
	}
}

/* ------------------------------------------------------------------ *
 * 1. THE BLOCK RUNS
 *
 *     int64_t sum(const int64_t *mem /rdi/, int32_t n /esi/)
 *       xor  eax, eax        48 31 C0
 *     .L:
 *       add  rax, [rdi]      48 03 07
 *       add  rdi, 8          48 83 C7 08
 *       dec  esi             FF CE
 *       jnz  .L              -> 75 F5   (rel8 = 3 - 14 = -11 = 0xF5)
 *       ret                  C3
 *
 * Hand-written BYTES on purpose: this suite tests the SUBSTRATE (is a page reservable, is what
 * was written in it what executes), not an encoder -- the encoder question is Q2 of the spike.
 * ------------------------------------------------------------------ */
typedef int64_t (*sum_fn)(const int64_t *mem, int32_t n);

static void test_block_runs(void)
{
	static const uint8_t code[] = {
		0x48, 0x31, 0xC0,             /* xor eax, eax */
		0x48, 0x03, 0x07,             /* add rax, [rdi] */
		0x48, 0x83, 0xC7, 0x08,       /* add rdi, 8 */
		0xFF, 0xCE,                   /* dec esi */
		0x75, 0xF5,                   /* jnz -11 */
		0xC3,                         /* ret */
	};
	int64_t mem[16];
	for (int i = 0; i < 16; i++) mem[i] = (int64_t)(i * 3 + 1);

	uint8_t *buf = jit_code_reserve();
	CHECK(buf != NULL);
	if (buf == NULL) return;
	memcpy(buf, code, sizeof code);
	sum_fn fn = (sum_fn)(void *)jit_code_publish(buf, (uint32_t)sizeof code);
	CHECK(fn != NULL);
	if (fn == NULL) return;

	int64_t want = 0;
	for (int i = 0; i < 8; i++) want += mem[i];
	CHECK_EQ(fn(mem, 8), want);
	/* twice, so a first-call-only artefact cannot pass this */
	CHECK_EQ(fn(mem, 8), want);
	CHECK_EQ(fn(mem, 1), mem[0]);
	/* NOT n == 0: the block is a do-while (dec sets ZF only for a NON-zero result), so zero
	 * would run 2^32 iterations.  That is a property of the hand-written bytes, not a defect
	 * -- and it was the first version of this probe's own bug (MEASURED: SIGSEGV at 4e9
	 * iterations of `add rax,[rdi]`).  n = 16 is the widest case the array supports. */
	int64_t want16 = 0;
	for (int i = 0; i < 16; i++) want16 += mem[i];
	CHECK_EQ(fn(mem, 16), want16);

	fprintf(stderr, "  block: 15 bytes published and executed, sum(8) = %lld\n",
	        (long long)fn(mem, 8));
}

/* ------------------------------------------------------------------ *
 * 2. THE STALE-BLOCK PROBE
 *
 * THE FIXTURE.  A guest code page at 0x1000 holds the bytes a "compiler" reads:
 *
 *     0x1000: C7 05 00 30 00 00 <imm32>   mov dword [0x3000], imm32
 *
 * and 0x2000 holds the PATCHER, which is what the machine actually runs:
 *
 *     0x2000: C7 05 06 10 00 00 <imm32>   mov dword [0x1006], imm32   <- 0x1006 IS the
 *     0x200A: F4                           hlt                           compiled instr's imm
 *
 * THE "COMPILER" reads imm32 out of the guest bytes at 0x1006 and emits the equivalent native
 * block for the host's own guest RAM:
 *
 *     48 B8 <host addr of mem8+0x3000>    mov rax, imm64
 *     C7 00 <imm32>                       mov dword [rax], imm32
 *     C3                                  ret
 *
 * i.e. a 1:1 translation of the guest instruction with the immediate resolved at compile time
 * -- the smallest possible thing that still has the property under test.
 * ------------------------------------------------------------------ */

#define CODE_PAGE   0x1000u
#define PATCHER     0x2000u
#define DATA_ADDR   0x3000u

static uint8_t *compile_block(uint32_t guest_page)
{
	uint32_t imm;
	memcpy(&imm, mem8 + guest_page + 6, 4);

	uint8_t *buf = jit_code_reserve();
	if (buf == NULL) return NULL;
	uint8_t *p = buf;
	*p++ = 0x48; *p++ = 0xB8;                       /* mov rax, imm64 */
	uint64_t host = (uint64_t)(uintptr_t)(mem8 + DATA_ADDR);
	memcpy(p, &host, 8);
	p += 8;
	*p++ = 0xC7; *p++ = 0x00;                       /* mov dword [rax], imm32 */
	memcpy(p, &imm, 4);
	p += 4;
	*p++ = 0xC3;                                    /* ret */
	uint32_t len = (uint32_t)(p - buf);
	if (jit_code_publish(buf, len) == NULL) return NULL;
	if (!jit_cache_bind(guest_page, guest_page, (uint8_t)(cpu->state_flags & CACHED_MASK_ALL),
	                    buf, len)) {
		return NULL;
	}
	return buf;
}

static void test_stale_block(void)
{
	static const uint8_t first[] = {
		0xC7, 0x05, 0x00, 0x30, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, /* mov [0x3000], 0x11 */
	};
	/* the patcher: writes imm32 into the compiled instruction's immediate field at 0x1006 */
	static const uint8_t patcher[] = {
		0xC7, 0x05, 0x06, 0x10, 0x00, 0x00, 0x22, 0x00, 0x00, 0x00, /* mov [0x1006], 0x22 */
		0xF4,                                                       /* hlt */
		0xEB, 0xFD,                                                 /* jmp hlt (post-halt padding) */
	};

	machine_init();
	uop_cache_enabled = true;
	CHECK(jit_mem_init());
	if (!jit_enabled) return;

	load(CODE_PAGE, first, (int)sizeof first);
	load(PATCHER, patcher, (int)sizeof patcher);
	mem8[DATA_ADDR] = 0;

	uint8_t *page = compile_block(CODE_PAGE);
	CHECK(page != NULL);
	if (page == NULL) return;

	uint8_t state = (uint8_t)(cpu->state_flags & CACHED_MASK_ALL);
	uint8_t *hit = jit_cache_lookup(CODE_PAGE, CODE_PAGE, state);
	CHECK(hit == page);
	/* (a) THE BLOCK EXECUTES AND WRITES THE OLD IMMEDIATE */
	jit_block_enter(page);
	((void (*)(void))(void *)page)();
	jit_block_exit();
	CHECK_EQ(ram32(DATA_ADDR), 0x11);

	/* (b) THE GUEST PATCHES ITS OWN CODE PAGE -- through the real instruction stream */
	uint64_t invalidations_before = jit_stats.invalidations;
	cpu->instruction_pointer = (int32_t)PATCHER;
	cpu->previous_ip = (int32_t)PATCHER;
	cpu->last_virt_eip = -1;
	run_machine(100);
	CHECK_EQ(ram32(0x1006), 0x22);              /* the patch landed (the premise) */
	CHECK(jit_stats.invalidations >= invalidations_before + 1); /* the block WAS dropped */

	/* (c) THE STALE BLOCK MUST NOT BE REACHABLE -- and the check is made in the EXECUTOR'S
	 * OWN FLOW (look up; compile ONLY on a miss), because that is the flow in which a missed
	 * invalidation becomes a wrong RESULT rather than a wrong counter.  A probe that only
	 * asserted the lookup missed, then recompiled unconditionally, would pass even if the
	 * stale block were still executable -- MEASURED: the first version of this test did
	 * exactly that, and mutation arm 2 (the block left live in the hash) was caught by the
	 * counter check alone.  With the lookup feeding the run, the mutation writes the STALE
	 * 0x11 and (d) fails on the VALUE. */
	uint8_t *page2 = jit_cache_lookup(CODE_PAGE, CODE_PAGE, state);
	CHECK(page2 == NULL);
	if (page2 == NULL) page2 = compile_block(CODE_PAGE);
	CHECK(page2 != NULL);
	if (page2 == NULL) return;

	/* (d) THE RE-RUN MUST WRITE THE NEW IMMEDIATE (0x11 here means STALE EXECUTION) */
	mem8[DATA_ADDR] = 0;
	jit_block_enter(page2);
	((void (*)(void))(void *)page2)();
	jit_block_exit();
	CHECK_EQ(ram32(DATA_ADDR), 0x22);

	fprintf(stderr, "  stale-block probe: block dropped on the guest write, the executor's "
	                "re-compile ran the new immediate (0x%02x)\n", ram32(DATA_ADDR));

	/* the whole fixture restated as the thing a reader should take away: had the block still
	 * been cached, (d)'s execution would have written 0x11 */
}

/* ------------------------------------------------------------------ *
 * 3. THE POOL'S REFUSALS (no silent stubs)
 * ------------------------------------------------------------------ */
static void test_refusals(void)
{
	/* a pointer from outside the pool */
	uint8_t outside[16];
	CHECK(jit_code_publish(outside, 4) == NULL);

	/* a valid page that was never reserved is not in the pool's free list either: reserving
	 * every page must eventually be REFUSED and COUNTED, not silently succeed */
	uint64_t fails = jit_stats.lowmem_failures;
	uint8_t *bufs[JIT_CODE_PAGES + 4];
	uint32_t n = 0;
	for (; n < JIT_CODE_PAGES + 4; n++) {
		uint8_t *b = jit_code_reserve();
		if (b == NULL) break;
		bufs[n] = b;
		bufs[n][0] = 0xC3; /* ret, so the pages are legal code if anything ran them */
		if (jit_code_publish(bufs[n], 1) == NULL) break;
	}
	CHECK(n < JIT_CODE_PAGES + 4);                 /* the pool DID run out */
	CHECK_EQ(jit_stats.lowmem_failures, fails + 1); /* and it SAID so */

	/* publishing an already-published page again must be refused (it is RX now) */
	CHECK(jit_code_publish(bufs[0], 1) == NULL);
	fprintf(stderr, "  refusals: pool exhausted after %u pages, reported %llu time(s)\n",
	        n, (unsigned long long)jit_stats.lowmem_failures);
}

/* ------------------------------------------------------------------ *
 * 4. THE EXECUTING PAGE'S RECYCLE IS DEFERRED
 * ------------------------------------------------------------------ */
static void test_deferred_recycle(void)
{
	static const uint8_t ret_only[] = { 0xC3 };
	uint8_t *buf = jit_code_reserve();
	CHECK(buf != NULL);
	if (buf == NULL) return;
	memcpy(buf, ret_only, 1);
	CHECK(jit_code_publish(buf, 1) == buf);
	CHECK(jit_cache_bind(0x9000, 0x9000, 0, buf, 1));

	uint64_t deferred = jit_stats.deferred_frees;
	uint64_t free_before = jit_stats.pages_alloc;

	jit_block_enter(buf);
	/* the guest writes the page whose block is EXECUTING */
	jit_code_invalidate_page(0x9000);
	CHECK(jit_abort_pending());                 /* the executor is told to stop */
	CHECK_EQ(jit_stats.deferred_frees, deferred + 1); /* the recycle was DEFERRED */
	CHECK(jit_cache_lookup(0x9000, 0x9000, 0) == NULL); /* but the block is already gone */

	bool aborted = jit_block_exit();
	CHECK(aborted);                              /* the abort survives to the exit */
	CHECK(!jit_abort_pending());                 /* and is consumed there */

	/* the page is reusable again: a fresh reserve must hand the same page back, because it
	 * is the only one free (the rest were exhausted by test 3) */
	uint8_t *again = jit_code_reserve();
	CHECK(again == buf);
	CHECK(jit_stats.pages_alloc == free_before + 1);
	fprintf(stderr, "  deferred recycle: page not flipped under the executing block, "
	                "reused after exit\n");
}

/* ------------------------------------------------------------------ *
 * 3b. A REFUSED BIND MUST GIVE ITS SPAN BACK (JIT-FIX-1 should-fix 3)
 *
 * WHY THIS IS A TEST AND NOT A CODE READING.  jit_cache_bind is the only writer of a page's
 * owner, of the block hash and of the guest-page list, so a published page whose bind failed
 * is UNREACHABLE -- no lookup, no invalidation and no future reservation can ever see it
 * again.  The pool is finite, so the leak is bounded at JIT_CODE_PAGES pages and NOTHING ELSE
 * IN THE SUITE CAN SEE IT: every other test either uses a handful of spans or drains the pool
 * deliberately.  MEASURED on the 1.5 G kernel boot with --jit on: the block table reaches its
 * 2048 blocks (it is never evicted by an invalidate on this workload) and every later decode
 * that reached bind leaked its span -- "jit: bind: the block table is full (2048)" printed
 * ~1000 times while the pool drained.
 *
 * THE ASSERTION IS EXACT AND IT IS THE POOL'S OWN REUSE, not a count: the pool is ALL FREE
 * when this test starts (it is the first test in main), so the first free run of N pages is
 * page 0.  A refused bind whose span was recycled gives that SAME run back to the very next
 * reservation; one that leaked it gives the NEXT run.  So `again == span` is the whole test,
 * and it cannot pass vacuously: without the fix the pages are still RX and the comparison is
 * between two different addresses.
 *
 * The three cases are the three reachable refusals.  The block-table-full case ends with
 * jit_code_clear_all() because it fills the table with 2048 live blocks; the two others are
 * cleaned by binding their span properly and invalidating it, so the pool is all free again
 * for the tests after this one.
 * ------------------------------------------------------------------ */
static void test_failed_bind_recycle(void)
{
	const uint32_t P2 = 2u * JIT_CODE_PAGE_BYTES;
	uint8_t *page0;

	/* --- (a) the span does not match the block: reserved 2 pages, bound as 1 --- */
	uint8_t *a = jit_code_reserve_span(P2);
	CHECK(a != NULL);
	if (a == NULL) return;
	page0 = a; /* the pool is all free here, so this IS page 0 */
	a[0] = 0xC3; /* ret, so the span is legal code if anything ever ran it */
	CHECK(jit_code_publish(a, P2) == a);
	CHECK(!jit_cache_bind(0x3000, 0x3000, 0, a, 1)); /* ONE page's worth of block */
	uint8_t *b = jit_code_reserve_span(P2);
	CHECK(b == a); /* recycled -- pre-fix these two pages are orphaned RX */
	if (b == a) {
		b[0] = 0xC3;
		CHECK(jit_code_publish(b, P2) == b);
		CHECK(jit_cache_bind(0x3000, 0x3000, 0, b, P2));
		jit_code_invalidate_page(0x3000); /* frees b's span again */
	}
	fprintf(stderr, "  failed bind (span mismatch): the span is back at the same page: %s\n",
	        b == a ? "yes" : "NO");

	/* --- (b) the physical page is outside the guest's memory --- */
	uint8_t *c = jit_code_reserve_span(P2);
	CHECK(c != NULL);
	if (c == NULL) return;
	c[0] = 0xC3;
	CHECK(jit_code_publish(c, P2) == c);
	CHECK(!jit_cache_bind(0x7FF00000u, 0x400000, 0, c, P2)); /* gpage 0x7FF00 >> jit_page_count */
	uint8_t *d = jit_code_reserve_span(P2);
	CHECK(d == c);
	if (d == c) {
		d[0] = 0xC3;
		CHECK(jit_code_publish(d, P2) == d);
		CHECK(jit_cache_bind(0x4000, 0x4000, 0, d, P2));
		jit_code_invalidate_page(0x4000);
	}
	fprintf(stderr, "  failed bind (page out of range): the span is back at the same page: %s\n",
	        d == c ? "yes" : "NO");

	/* --- (c) the block table is full: 2048 live blocks, the 2049th bind refused --- */
	uint32_t bound = 0;
	for (uint32_t i = 0; i < JIT_MAX_BLOCKS; i++) {
		uint8_t *s = jit_code_reserve_span(JIT_CODE_PAGE_BYTES);
		if (s == NULL) break;
		s[0] = 0xC3;
		if (jit_code_publish(s, JIT_CODE_PAGE_BYTES) != s) break;
		/* distinct (phys, virt): phys must stay inside the guest's memory, virt may not */
		if (!jit_cache_bind((i % 512u) << 12, 0x100000u + i * 0x1000u, 0, s,
		                    JIT_CODE_PAGE_BYTES))
			break;
		bound++;
	}
	CHECK_EQ(bound, JIT_MAX_BLOCKS); /* the table really filled, so the next bind is refused */
	uint8_t *e = jit_code_reserve_span(JIT_CODE_PAGE_BYTES);
	CHECK(e != NULL);
	uint8_t *f = NULL;
	if (e != NULL) {
		e[0] = 0xC3;
		CHECK(jit_code_publish(e, JIT_CODE_PAGE_BYTES) == e);
		CHECK(!jit_cache_bind(0x5000, 0x800000, 0, e, JIT_CODE_PAGE_BYTES)); /* full */
		f = jit_code_reserve_span(JIT_CODE_PAGE_BYTES);
		CHECK(f == e); /* the refused span came back, not the next one */
	}
	fprintf(stderr, "  failed bind (table full after %u blocks): the refused span came back: "
	                "%s\n", bound, f == e ? "yes" : "NO");

	jit_code_clear_all(); /* the 2048 blocks are dropped, so every span of them is recycled */
	if (f == e) {
		/* the reclaimed page is a RESERVATION like any other: take it or this test leaks
		 * the very page it just proved was given back (MEASURED: without this the pool
		 * showed 4091 free runs instead of 4092 -- one page out, exactly).  Its bind needs
		 * a FREE TABLE SLOT, so it has to come after the clear. */
		f[0] = 0xC3;
		CHECK(jit_code_publish(f, JIT_CODE_PAGE_BYTES) == f);
		CHECK(jit_cache_bind(0x7000, 0x7000, 0, f, JIT_CODE_PAGE_BYTES));
		jit_code_invalidate_page(0x7000);
	}
	uint8_t *g = jit_code_reserve_span(JIT_CODE_PAGE_BYTES);
	CHECK(g == page0); /* THE POOL IS BACK TO ITS INITIAL STATE, not merely usable */
	if (g != NULL) {
		g[0] = 0xC3;
		CHECK(jit_code_publish(g, JIT_CODE_PAGE_BYTES) == g);
		CHECK(jit_cache_bind(0x6000, 0x6000, 0, g, JIT_CODE_PAGE_BYTES));
		jit_code_invalidate_page(0x6000);
	}
	fprintf(stderr, "  after the clear: the pool is all free again (first reservation is "
	                "page 0: %s)\n", g == page0 ? "yes" : "NO");
}

/* JIT-6 (block chaining): THE CHAIN PROBE'S KEY IS THE FULL TRIPLE, and the translation gate
 * is real.  jit_chain_target is what a chain link calls instead of uop_run's dispatch, so a
 * wrong hit means entering a block compiled for a DIFFERENT guest state or a DIFFERENT page
 * -- silently.  This test addresses the probe DIRECTLY (no execution) with:
 *
 *   A  (P,      V,  S1) entry 0x10   -- the block the queries below must find
 *   B  (P,      V,  S2) entry 0x20   -- the other-state block at the same (phys, virt)
 *   D  (P,      Vd, S2) entry 0x30   -- a DECOY forced into A's probe slot: Vd is searched
 *                                       for hash(P,Vd,S2) == hash(P,V,S1), and D is bound
 *                                       BEFORE A, so A probes to the next slot and the walk
 *                                       for (P,V,S1) MEETS D FIRST
 *   C  (0x7F000,V', S1) entry 0x40   -- a block whose key is exactly the GARBAGE
 *                                       translation the probe would compute for ip=0x7F000
 *                                       if it ignored the fetch-translation cache
 *                                       (eip_phys ^ 0x7F000 == 0x7F000 here)
 *
 *   the assertions: S1 finds A (walking past D), S2 finds B, a third state misses, and an
 *   uncovered virtual misses.
 *
 * ABLE TO FAIL, per conjunct:
 *   virt+state deleted      -> the walk for S1 takes D (phys matches, the rest ignored) and
 *                              assertion 1 fires;
 *   translation gate deleted-> the garbage key hits C and the miss arm fires.
 *   state ALONE, phys ALONE -> NOT gated here, and state-alone is PROVABLY unobservable,
 *                              not merely unlayouted: the hash mixes the state into the
 *                              slot number, so a wrong-state entry is never in the queried
 *                              probe chain at all, and a decoy that survives the virt+phys
 *                              compares would have to have A's exact key.  The compare is
 *                              collision protection, gated as a PAIR above; the single-
 *                              conjunct classes are the system gates' (nasm, the boots).
 *                              Documented, like the cpl3 fold's deferral in JIT-FIX-1. */
static void test_chain_key(void)
{
	const uint32_t P = 0x9000u, V = 0x9000u; /* phys == virt, identity-mapped like the rest */
	const uint8_t S1 = 0, S2 = (uint8_t)CACHED_MASK_CPL3;
	uint8_t saved_state = cpu->state_flags;

	/* the exhaustion test before this one leaves the pool intentionally empty: reset it the
	 * same way test_failed_bind_recycle does (the clear drops compiled blocks and recycles
	 * every page, which is what the comment in main() promises between tests) */
	/* D's virtual address: the page whose (P, Vd, S2) hash COLLIDES with (P, V, S1)'s slot.
	 * The hash is jit_mem.c's file-private mixing function; jit_test_hash is its exported
	 * identity (a copy here would go silent if the real one changed, and the gate would lose
	 * its tooth without anything noticing). */
	uint32_t vd = 0;
	for (uint32_t cand = 0x10000u; cand < 0xF0000000u; cand += 0x1000u) {
		if (jit_test_hash(P, cand, S2) == jit_test_hash(P, V, S1)) {
			vd = cand;
			break;
		}
	}
	CHECK(vd != 0); /* a 32-bit space against a 4096-slot table: collisions exist */
	if (vd == 0) return;

	uint8_t *d = jit_code_reserve();
	uint8_t *a = jit_code_reserve();
	uint8_t *b = jit_code_reserve();
	uint8_t *c = jit_code_reserve();
	CHECK(d != NULL); CHECK(a != NULL); CHECK(b != NULL); CHECK(c != NULL);
	if (d == NULL || a == NULL || b == NULL || c == NULL) return;
	d[0] = 0xC3; a[0] = 0xC3; b[0] = 0xC3; c[0] = 0xC3;
	CHECK(jit_code_publish(d, 1) == d);
	CHECK(jit_code_publish(a, 1) == a);
	CHECK(jit_code_publish(b, 1) == b);
	CHECK(jit_code_publish(c, 1) == c);
	CHECK(jit_cache_bind(P, vd, S2, d, 1));   /* D first: it takes A's slot */
	CHECK(jit_cache_bind(P, V, S1, a, 1));    /* A probes to the next slot */
	CHECK(jit_cache_bind(P, V, S2, b, 1));
	CHECK(jit_cache_bind(0x7F000u, 0x7F000u, S1, c, 1));
	jit_chain_bind(d, 0x30);
	jit_chain_bind(a, 0x10);
	jit_chain_bind(b, 0x20);
	jit_chain_bind(c, 0x40);

	/* the fetch-translation cache, as get_phys_eip would have left it for a same-page tail */
	cpu->instruction_pointer = (int32_t)V;
	cpu->last_virt_eip = (int32_t)(V & ~0xFFFu);
	cpu->eip_phys = (int32_t)(P ^ V);

	cpu->state_flags = S1;
	CHECK(jit_chain_target() == a + 0x10); /* walks PAST D (the decoy in A's slot) */
	cpu->state_flags = S2;
	CHECK(jit_chain_target() == b + 0x20);

	/* a state with NO block at this key must miss (the link then falls back, which is the
	 * C dispatch's own decode-and-compile path) */
	cpu->state_flags = (uint8_t)CACHED_MASK_SS32; /* distinct from S1 (0) and S2 (CPL3) */
	CHECK(jit_chain_target() == NULL);

	/* a tail whose page the fetch-translation cache does not cover must miss: the chain
	 * never walks (jit_chain_target's comment -- the walk's fault contract is the C
	 * tail's, and only the C tail may run it).  With the cache gate deleted, phys would be
	 * eip_phys ^ 0x7F000 == 0x7F000 -- which is exactly C's key, so the miss arm FIRES. */
	cpu->state_flags = S1;
	cpu->instruction_pointer = (int32_t)0x7F000u;
	CHECK(jit_chain_target() == NULL);

	/* drop them so the later tests see the table/pool as they found it */
	jit_code_invalidate_page(P);
	jit_code_invalidate_page(0x7F000u);
	cpu->state_flags = saved_state;
	fprintf(stderr, "  chain probe: key is (phys, virt, state) with a colliding decoy; "
	                "cache-gated; miss falls back\n");
}

int main(void)
{
	machine_init();
	uop_cache_enabled = true;
	if (!jit_mem_init()) {
		fprintf(stderr, "jit-selftest: the substrate could not be initialized\n");
		return 2;
	}
	jit_enabled = true;

	/* FIRST: it fills the block table and then clears it, and every test below assumes the
	 * pool and the table are in their initial state -- this one puts them back there (and
	 * asserts it, `g == page0`). */
	test_failed_bind_recycle();
	test_block_runs();
	test_stale_block();
	test_chain_key();
	test_deferred_recycle();
	test_refusals();

	fprintf(stderr, "jit-selftest: %d checks, %d failures\n", checks, failures);
	jit_stats_print();
	return failures == 0 ? 0 : 1;
}
