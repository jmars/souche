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

void v86c_dbg_init(void);
void v86c_dbg_break_hit(uint64_t eip);
void v86c_dbg_watch_v(uint64_t addr, int width, uint64_t value, uint64_t rip);
void v86c_dbg_watch_p(uint32_t phys, int width, uint64_t value, uint64_t rip);
void v86c_dbg_watch_fault(uint64_t addr, bool write, uint64_t rip);
void v86c_dbg_ring_record(uint64_t eip, const uint8_t *bytes, int n); /* up to 16 bytes */
void v86c_dbg_on_stop(const char *why);

/* the selftest's own surface: turn the tools on without the environment, and read the count of
 * entries each of them produced (an instrument that cannot be observed is not an instrument) */
void v86c_dbg_test_arm(void);
void v86c_dbg_test_disarm(void);
int v86c_dbg_test_counts(void);

#endif /* V86C_DEBUG_H */
