#!/usr/bin/env python3
"""JIT-6 attribution instrument: copy the tree to a scratch dir and add rdtsc brackets
   (scratch ONLY -- the shipped tree is never built with the instrument, PERF-5's rule).
   Measures, on the real 200 M boot:
     t_uoprun   = rdtsc total inside uop_run (bracketed in cycle_internal, ~257K pairs)
     t_dispatch = rdtsc total of the segment between consecutive jit_execute_block calls
                  (one bracket pair per block run -- its own cost is calibrated by
                  results/jit-6/rdtsc-pair-cost.c and subtracted before reporting)
   interior = t_uoprun - t_dispatch - N_pairs*pair_cost  (compiled code + memory model,
   never bracketed itself).

   WHAT t_dispatch ACTUALLY BRACKETS, stated precisely (JIT-6F, from the reviewer's finding 2):
   the segment is [return of the previous jit_execute_block] -> [call of the next one], i.e. the
   C code BETWEEN compiled blocks: the exit-kind handling, the loop top and the uop-cache hash
   probe.  It EXCLUDES:
     * the EPILOGUE (spill + stats fold + pops) and the PROLOGUE (pushes + register reloads +
       imm64 materialisations), both of which run inside the compiled block -- the epilogue in the
       tail of `call code`, the prologue inside jit_block_enter + the block's own top;
     * jit_block_enter's page identification (it runs inside jit_execute_block, after the bracket).
   Chaining removes the epilogue + C tail + probe + enter + prologue, so t_dispatch is a LOWER
   BOUND on the dispatch cost the chain deletes; "interior 96.5 %" must NOT be read as "dispatch
   is only 3.5 %".  (To bracket the prologue/epilogue too, the brackets would have to move inside
   the emitted block; that needs in-emitter rdtsc and is not done here -- no decision in JIT-6
   turns on the split, its number comes from the identity-asserted A/B.)  JIT-6F also drops the
   FIRST segment, which used to include process-start-to-first-block time (j6_t_seg starts at 0)."""
import shutil, subprocess, sys, pathlib

SRC = pathlib.Path("/home/jaye/futaie/souche")
DST = pathlib.Path("/tmp/j6-rdtsc")

shutil.rmtree(DST, ignore_errors=True)
# NOTE: v86/ is NOT ignored -- the build regenerates src/cpu/interpreter_gen.c FROM
# v86/gen/x86_table.js, and make refuses ("no rule to make target") when that tree is absent.
# JIT-6F finding: with v86/ ignored the scratch could not be built at all, so the script as
# published was not runnable as-is.
shutil.copytree(SRC, DST, symlinks=True,
                ignore=shutil.ignore_patterns("build", "results", ".git", "oracle", "build-det"))
(DST / "build").mkdir()

# ---- 1. cycle_internal: bracket the uop_run call ----
p = DST / "src/cpu/cpu.c"
s = p.read_text()
old = """	if (uop_cache_enabled) {
		uop_run(phys_addr);
	}"""
new = """#ifndef J6_TSC_LOHI
/* JIT-6F: cosmocc builds the APE for x86-64 AND aarch64, so the rdtsc asm must be guarded --
 * the instrument did not build at all before this (the aarch64 pass: "impossible constraint in
 * 'asm'").  On a non-x86-64 pass the read is a no-op, which is fine: the instrument is only
 * meaningful on x86-64 anyway. */
#if defined(__x86_64__) || defined(_M_X64)
#define J6_TSC_LOHI(lo, hi) __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi))
#else
#define J6_TSC_LOHI(lo, hi) do { (lo) = 0; (hi) = 0; } while (0)
#endif
#endif
	if (uop_cache_enabled) {
		{ extern unsigned long long j6_t_uoprun, j6_n_uoprun;
		  unsigned int lo, hi; unsigned long long t0;
		  J6_TSC_LOHI(lo, hi);
		  t0 = ((unsigned long long)hi << 32) | lo;
		  uop_run(phys_addr);
		  J6_TSC_LOHI(lo, hi);
		  j6_t_uoprun += (((unsigned long long)hi << 32) | lo) - t0;
		  j6_n_uoprun++; }
	}"""
assert old in s, "cpu.c anchor"
p.write_text(s.replace(old, new))

# ---- 2. uop_run's compiled arm: bracket the dispatch segment ----
p = DST / "src/cpu/uop_cache.c"
s = p.read_text()
old = """		if (blk->code != NULL) {
			/* THE COMPILED BLOCK"""
new = """		if (blk->code != NULL) {
			{ extern unsigned long long j6_t_dispatch, j6_n_dispatch, j6_t_seg;
			  extern int j6_seg_init;
			  unsigned int lo, hi;
			  J6_TSC_LOHI(lo, hi);
			  if (j6_seg_init) {
			      j6_t_dispatch += (((unsigned long long)hi << 32) | lo) - j6_t_seg;
			      j6_n_dispatch++;
			  } else j6_seg_init = 1;
			  j6_t_seg = ((unsigned long long)hi << 32) | lo; }
			/* THE COMPILED BLOCK"""
assert old in s, "uop_cache.c anchor 1"
s = s.replace(old, new)
old = """			int jit_why = jit_execute_block(blk->code);"""
new = """			int jit_why = jit_execute_block(blk->code);
			{ extern unsigned long long j6_t_seg;
			  unsigned int lo, hi;
			  J6_TSC_LOHI(lo, hi);
			  j6_t_seg = ((unsigned long long)hi << 32) | lo; }"""
assert old in s, "uop_cache.c anchor 2"
s = s.replace(old, new)

# the counters + the report, printed by the jit stats atexit
old = """void uop_stats_print(void)
{"""
new = """void j6_rdtsc_report(void);
void j6_rdtsc_report(void)
{
	extern unsigned long long j6_t_uoprun, j6_n_uoprun, j6_t_dispatch, j6_n_dispatch,
	    j6_t_seg, j6_pair_cost;
	/* JIT-6F: NO "+ j6_t_seg" here.  t_seg holds the start of the LAST (still in-flight)
	 * segment as an ABSOLUTE rdtsc value, so adding it grafted the host's TSC-since-boot
	 * onto the total -- MEASURED: it made dispatch_raw read 9.49e14 cycles (946 T, i.e.
	 * absurd) where the real between-block total is ~1e9.  The instrument now sums only
	 * COMPLETE segments and drops the first (boot) and last (in-flight) ones. */
	unsigned long long dispatch_raw = j6_t_dispatch;
	fprintf(stderr, "j6: t_uoprun %llu cyc / %llu calls; t_dispatch_raw %llu cyc / %llu brackets\\n",
	        j6_t_uoprun, j6_n_uoprun, dispatch_raw, j6_n_dispatch);
	fprintf(stderr, "j6: NOTE t_dispatch is the C BETWEEN blocks (exit handling + loop top + "
	        "hash probe); it EXCLUDES the prologue/epilogue, which run inside the block -- a "
	        "LOWER bound on what chaining removes.\\n");
	fprintf(stderr, "j6: pair_cost %llu cyc -> dispatch_est %llu cyc, interior_est %llu cyc\\n",
	        j6_pair_cost,
	        dispatch_raw - j6_n_dispatch * j6_pair_cost,
	        j6_t_uoprun - (dispatch_raw - j6_n_dispatch * j6_pair_cost));
}

void uop_stats_print(void)
{
	j6_rdtsc_report();"""
assert old in s, "uop_cache.c anchor 3"
s = s.replace(old, new)
old = """#include "cpu.h"         /* interpreted_run, jit_block_boundary, INTERPRETER_ITERATION_LIMIT */"""
new = old + """
#ifndef J6_TSC_LOHI
#if defined(__x86_64__) || defined(_M_X64)
#define J6_TSC_LOHI(lo, hi) __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi))
#else
#define J6_TSC_LOHI(lo, hi) do { (lo) = 0; (hi) = 0; } while (0)
#endif
#endif
unsigned long long j6_t_uoprun, j6_n_uoprun, j6_t_dispatch, j6_n_dispatch, j6_t_seg,
    j6_pair_cost;
int j6_seg_init;"""
assert old in s, "uop_cache.c anchor 4"
s = s.replace(old, new)
p.write_text(s)

# ---- 3. calibrate: pair cost from the environment, set by the runner ----
p = DST / "src/main.c"
s = p.read_text()
old = """	jit_init_if_enabled();"""
new = """	jit_init_if_enabled();
	{ const char *pc = getenv("J6_PAIR_COST");
	  if (pc) { extern unsigned long long j6_pair_cost; j6_pair_cost = strtoull(pc, 0, 10); } }"""
assert old in s, "main.c anchor"
p.write_text(s.replace(old, new))
print("scratch ready:", DST)
