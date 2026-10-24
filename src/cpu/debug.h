/*
 * debug.h -- THE DEBUGGING TOOLKIT, all DEFAULT OFF.
 *
 * Why it exists: this lane hand-rolled the same probes four times and LOST evidence to its own
 * instrumentation twice (a 160-byte stop buffer cut a message mid-evidence; a SIGSEGV discarded
 * block-buffered walk output).  These tools are the reusable version, behind one env-var surface,
 * writing to stderr with a stable "v86c dbg:" prefix so a gate can grep them.
 *
 *   V86C_WATCH=<addr>:<len>[,<addr>:<len>...]  log every write to a VIRTUAL or PHYSICAL address
 *   V86C_BREAK=<addr>:<len>[,<addr>:<len>...]  log the first hits of an EXECUTION in a range, with
 *                                              registers -- WATCH only sees STORES, so "did the port
 *                                              execute this code, and with what in %rax" needs this
 *   V86C_BREAK_MAX=<N>                         hits printed per break slot (default 4)
 *   V86C_BREAK_RING=1                          dump the ring at each printed break hit (the path in)
 *   V86C_RING=<N>                              keep the last N instruction boundaries, dumped on a stop
 *   V86C_DUMP_ON_STOP=1                        on a stop: registers, cr0-4, efer, idtr/gdtr, cr3,
 *                                              and the page-table chain for V86C_CHAIN=<va>
 *   V86C_CHAIN=<va>                            the virtual address whose chain DUMP_ON_STOP prints
 *   V86C_INTS=<N>                              trace INTERRUPT/EXCEPTION DELIVERY: N printed lines
 *                                              (max 100000), plus a per-vector histogram, the max
 *                                              nesting depth and the total, dumped on a stop
 *   V86C_RSPLOW=<K>                            keep the last K instructions on which rsp reached a
 *                                              NEW MINIMUM (rip, rsp, the delta from the previous
 *                                              low), dumped on any stop -- the descent RECORDED
 *                                              instead of inferred from stack words
 *
 * THE COST WHEN OFF is one load and one branch per hooked write (the mask is a global bool read
 * once per hook), and the bench pin is the measurement that it is free (results/x86-64-debugtoolkit.md).
 * A stop FLUSHES stderr first, so nothing the tools wrote can die with the process.
 */
#ifndef V86C_DEBUG_H
#define V86C_DEBUG_H

#include <stdbool.h>
#include <stdint.h>

/* the masks the hot paths read; false unless v86c_dbg_init() turned them on */
extern bool v86c_dbg_watch_on;
/* the recursion bound, DEBUG ONLY: unset = the shipped 4; V86C_FAULT_DELIVERY_BOUND=N overrides it
 * so an investigation can see past the first delivery (results/x86-64-firstfault2.md) */
extern int v86c_dbg_delivery_bound;
extern bool v86c_dbg_ring_on;
extern bool v86c_dbg_break_on;
extern bool v86c_dbg_ints_on;
extern bool v86c_dbg_rsplow_on;

void v86c_dbg_init(void);
void v86c_dbg_break_hit(uint64_t eip);
void v86c_dbg_watch_v(uint64_t addr, int width, uint64_t value, uint64_t rip);
void v86c_dbg_watch_p(uint32_t phys, int width, uint64_t value, uint64_t rip);
/* the long-mode store's TWO addresses at once: the linear address it was issued as and the
 * physical frame it landed in.  A watch entry counts if EITHER is in its range, once. */
void v86c_dbg_watch_vp(uint64_t va, uint32_t phys, int width, uint64_t value, uint64_t rip);
void v86c_dbg_watch_fault(uint64_t addr, bool write, uint64_t rip);
void v86c_dbg_watch_range(uint64_t phys, uint64_t len, uint64_t rip);
void v86c_dbg_ring_record(uint64_t eip, const uint8_t *bytes, int n); /* up to 16 bytes */
/* THE DELIVERY TRACE (results/x86-64-ints.md): called from the ONE delivery seat,
 * call_interrupt_vector (exceptions.c), BEFORE the delivery nests, so `depth` is the depth this
 * delivery is NESTED IN (0 = not nested) and `interrupted_rip` is cpu->previous_ip -- the
 * faulting/interrupted instruction, which is why paging.c:145's rewind reads it.  The vector,
 * `is_int` and the error code are upstream's own arguments; rsp and cs are the machine's state
 * at the seat.  The CPL printed is cpu->cpl, read here like v86c_dbg_break_hit reads the regs. */
void v86c_dbg_ints_delivery(int32_t vector, bool is_int, bool has_error_code,
                            int32_t error_code, int depth, uint64_t interrupted_rip,
                            uint64_t rsp, uint64_t cs);
/* THE NEW-MINIMUM RING (results/x86-64-rsplow.md): called once per instruction from the
 * interpreter's per-instruction seat, AFTER the instruction ran, so `rsp` is the value it LEFT and
 * `rip` names the instruction that moved it.  Records only a value LOWER than every value seen so
 * far (the descent), never a recovery.  The first value seen seeds the minimum and records nothing,
 * so every recorded event is a real step below a previous low and `delta` is the size of that step. */
void v86c_dbg_rsplow_tick(uint64_t rip, uint64_t rsp);
void v86c_dbg_on_stop(const char *why);

/* the selftest's own surface: turn the tools on without the environment, and read the count of
 * entries each of them produced (an instrument that cannot be observed is not an instrument) */
void v86c_dbg_test_arm(void);
void v86c_dbg_test_disarm(void);
int v86c_dbg_test_counts(void);
/* the delivery trace's own surface: arm it with a PRINT LIMIT (0 = histogram only) and read back
 * what it counted.  The printed-line counter is separate from the total on purpose -- that
 * separation is what lets an arm assert the window is BOUNDED. */
void v86c_dbg_test_ints_arm(int limit);
int v86c_dbg_ints_total(void);
int v86c_dbg_ints_printed(void);
int v86c_dbg_ints_max_depth(void);
int v86c_dbg_ints_limit(void);   /* the PRINT limit after parsing/capping -- the storm bound */
/* the per-vector histogram's read-back: the summary prints the top eight, and the selftest asks
 * one vector directly -- a histogram nothing can assert is a seat nobody calls. */
int v86c_dbg_ints_count(int vector);
/* the new-minimum ring's own surface: arm it with a CAPACITY (capped at the ring bound), read back
 * how many events it saw and how many it still holds, and read one entry oldest-first -- a ring
 * nobody can inspect cannot be asserted to hold the LAST K rather than the first. */
void v86c_dbg_test_rsplow_arm(int k);
int v86c_dbg_rsplow_total(void);
int v86c_dbg_rsplow_kept(void);
int v86c_dbg_rsplow_limit(void);   /* the capacity after capping -- the bound, not the request */
bool v86c_dbg_rsplow_entry(int i, uint64_t *rip, uint64_t *rsp, uint64_t *delta);

#endif /* V86C_DEBUG_H */
