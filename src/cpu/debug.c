/*
 * debug.c -- the toolkit's implementation.  See debug.h for the surface and the cost contract.
 */
#include "debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "cpu_state.h"
#include "memory.h"
#include "paging.h"

bool v86c_dbg_watch_on;
int v86c_dbg_delivery_bound = 4;
bool v86c_dbg_ring_on;
bool v86c_dbg_break_on;
static bool dump_on_stop;

#define V86C_DBG_MAX_WATCH 8
static struct { uint64_t addr; int len; } watch[V86C_DBG_MAX_WATCH];
static int watch_n;

/* the EXECUTION watch: WATCH above sees stores, this one sees the instruction pointer */
#define V86C_DBG_MAX_BREAK 8
static struct { uint64_t addr; uint64_t len; int hits; } brk[V86C_DBG_MAX_BREAK];
static int brk_n, brk_max = 4;
static bool brk_ring;

#define V86C_DBG_RING_MAX 4096
static struct { uint64_t eip; uint8_t bytes[16]; } ring[V86C_DBG_RING_MAX];
static int ring_n, ring_i;
static int test_counts;

/* ---- THE DELIVERY TRACE (results/x86-64-ints.md) --------------------------------
 * WHY IT EXISTS: the amd64 guest's stack descent (results/x86-64-stackoverflow.md ss5) had to be
 * INFERRED from stack arithmetic because the toolkit could not see a single interrupt/exception
 * delivery -- not the vector, not the depth, not the rip that was interrupted.  This seat answers
 * exactly that, from the one place a gate is accepted (call_interrupt_vector).
 * THE STORM IS BOUNDED IN BOTH DIRECTIONS, deliberately: the printed lines by `ints_limit` (capped
 * at V86C_DBG_INTS_MAX so a delivery storm cannot fill a disk), and the COUNTING by nothing -- the
 * histogram is what answers "which vector drives the loop", so it must survive a limit of 0, and
 * the stop summary must print even then (a summary gated on the print limit is the budget knob
 * this lane has already found once: declared, and unable to fire). */
#define V86C_DBG_INTS_MAX 100000
#define V86C_DBG_INTS_VECTORS 256
#define V86C_DBG_INTS_TOP 8
bool v86c_dbg_ints_on;
static int ints_limit;    /* lines to print; 0 = the histogram only */
static int ints_printed;
static int ints_total;
static int ints_max_depth;
static int ints_hist[V86C_DBG_INTS_VECTORS];
static bool ints_window_closed;

void v86c_dbg_test_ints_arm(int limit)
{
    int i;
    if (limit < 0) limit = 0;
    if (limit > V86C_DBG_INTS_MAX) limit = V86C_DBG_INTS_MAX;
    ints_limit = limit;
    ints_printed = 0;
    ints_total = 0;
    ints_max_depth = 0;
    ints_window_closed = false;
    for (i = 0; i < V86C_DBG_INTS_VECTORS; i++) ints_hist[i] = 0;
    v86c_dbg_ints_on = true;
}
int v86c_dbg_ints_total(void) { return ints_total; }
int v86c_dbg_ints_printed(void) { return ints_printed; }
int v86c_dbg_ints_max_depth(void) { return ints_max_depth; }
int v86c_dbg_ints_limit(void) { return ints_limit; }
int v86c_dbg_ints_count(int vector)
{
    if (vector < 0 || vector >= V86C_DBG_INTS_VECTORS) return 0;
    return ints_hist[vector];
}

void v86c_dbg_test_arm(void)
{
    watch_n = 1; watch[0].addr = 0x31558; watch[0].len = 1;
    v86c_dbg_watch_on = true;
    ring_n = 8; ring_i = 0; v86c_dbg_ring_on = true;
    brk_n = 1; brk[0].addr = 0xFFFFFFFF9C2775C2ull; brk[0].len = 1; brk[0].hits = 0;
    v86c_dbg_break_on = true;
    test_counts = 0;
}
void v86c_dbg_test_disarm(void)
{
    v86c_dbg_watch_on = false; v86c_dbg_ring_on = false; watch_n = 0; ring_n = 0;
    v86c_dbg_break_on = false; brk_n = 0;
    v86c_dbg_ints_on = false;
}
int v86c_dbg_test_counts(void) { return test_counts; }


void v86c_dbg_init(void)
{
    const char *w = getenv("V86C_WATCH");
    const char *r = getenv("V86C_RING");
    const char *d = getenv("V86C_DUMP_ON_STOP");
    const char *b = getenv("V86C_FAULT_DELIVERY_BOUND");
    if (b != NULL) {
        int v = (int)strtol(b, NULL, 0);
        if (v > 0) v86c_dbg_delivery_bound = v;
    }
    if (w != NULL) {
        const char *p = w;
        while (*p != '\0' && watch_n < V86C_DBG_MAX_WATCH) {
            char *end;
            uint64_t a = strtoull(p, &end, 0);
            int len = 1;
            if (*end == ':') { len = (int)strtol(end + 1, &end, 0); }
            watch[watch_n].addr = a;
            watch[watch_n].len = len > 0 ? len : 1;
            watch_n++;
            p = (*end == ',') ? end + 1 : end;
            if (p == end) break;
        }
        v86c_dbg_watch_on = watch_n > 0;
    }
    if (r != NULL) {
        ring_n = (int)strtol(r, NULL, 0);
        if (ring_n > V86C_DBG_RING_MAX) ring_n = V86C_DBG_RING_MAX;
        if (ring_n < 0) ring_n = 0;
        v86c_dbg_ring_on = ring_n > 0;
    }
    { const char *k = getenv("V86C_BREAK");
      if (k != NULL) {
        const char *p = k;
        while (*p != '\0' && brk_n < V86C_DBG_MAX_BREAK) {
            char *end;
            uint64_t a = strtoull(p, &end, 0);
            uint64_t len = 1;
            if (*end == ':') { len = strtoull(end + 1, &end, 0); }
            brk[brk_n].addr = a;
            brk[brk_n].len = len > 0 ? len : 1;
            brk[brk_n].hits = 0;
            brk_n++;
            p = (*end == ',') ? end + 1 : end;
            if (p == end) break;
        }
        v86c_dbg_break_on = brk_n > 0;
      } }
    { const char *m = getenv("V86C_BREAK_MAX");
      if (m != NULL) { int v = (int)strtol(m, NULL, 0); if (v > 0) brk_max = v; } }
    { const char *br = getenv("V86C_BREAK_RING"); brk_ring = br != NULL && *br != '0'; }
    { const char *n = getenv("V86C_INTS");
      if (n != NULL) {
        long v = strtol(n, NULL, 0);
        if (v > 0) {
            if (v > V86C_DBG_INTS_MAX) v = V86C_DBG_INTS_MAX;
            ints_limit = (int)v;
            v86c_dbg_ints_on = true;
        }
      } }
    dump_on_stop = d != NULL && *d != '0';
    /* UNBUFFERED BY DEFAULT: the whole point is that a crash cannot eat the output */
    setvbuf(stderr, NULL, _IONBF, 0);
    if (v86c_dbg_watch_on || v86c_dbg_ring_on || dump_on_stop || v86c_dbg_break_on ||
        v86c_dbg_ints_on) {
        fprintf(stderr, "v86c dbg: tools on (watch=%d break=%d ring=%d dump=%d ints=%d)\n",
                watch_n, brk_n, ring_n, (int)dump_on_stop, ints_limit);
    }
}

static void watch_hit(int is_physical, uint64_t addr, int width, uint64_t value, uint64_t rip)
{
    for (int i = 0; i < watch_n; i++) {
        uint64_t a = watch[i].addr;
        if (addr < a || addr >= a + (uint64_t)watch[i].len) continue;
        test_counts++;
        fprintf(stderr, "v86c dbg: WATCH %c 0x%llx width=%d value=0x%llx rip=0x%llx\n",
                is_physical ? 'P' : 'V', (unsigned long long)addr, width,
                (unsigned long long)value, (unsigned long long)rip);
    }
}

void v86c_dbg_watch_v(uint64_t addr, int width, uint64_t value, uint64_t rip)
{
    watch_hit(0, addr, width, value, rip);
}

void v86c_dbg_watch_p(uint32_t phys, int width, uint64_t value, uint64_t rip)
{
    watch_hit(1, (uint64_t)phys, width, value, rip);
}

/* THE LONG-MODE STORE'S TWO ADDRESSES AT ONCE (results/x86-64-watchphys.md).  A long-mode store
 * is issued as a LINEAR address and the frame it lands in exists only AFTER the walk, so the two
 * hooks above could not both speak for one store: watch_v fires before translate_address_write()
 * and watch_p only ever sees the 32-bit path's already-physical address.  The consequence was
 * measured -- V86C_WATCH on a PHYSICAL range (the documented way to watch a frame) saw NOTHING
 * from any 64-bit store.  This one carries both, tests every entry against BOTH, and counts an
 * entry ONCE even when its range contains both: a hit is a hit, not two. */
void v86c_dbg_watch_vp(uint64_t va, uint32_t phys, int width, uint64_t value, uint64_t rip)
{
    for (int i = 0; i < watch_n; i++) {
        uint64_t a = watch[i].addr;
        uint64_t end = a + (uint64_t)watch[i].len;
        bool in_v = va >= a && va < end;
        bool in_p = (uint64_t)phys >= a && (uint64_t)phys < end;
        if (!in_v && !in_p) continue;
        test_counts++;
        fprintf(stderr, "v86c dbg: WATCH V 0x%llx -> P 0x%llx width=%d value=0x%llx rip=0x%llx\n",
                (unsigned long long)va, (unsigned long long)phys, width,
                (unsigned long long)value, (unsigned long long)rip);
    }
}

/* A BULK COPY IS ONE STORE THE PER-BYTE HOOK CANNOT SEE: string.c's fast path writes a whole
 * page run with ONE memcpy_no_mmap_or_dirty_check/memset_no_mmap_or_dirty_check call, and its
 * 64-bit slow path stores through safe_write*_addr, whose per-store hook sees one store, not the
 * run the fast path writes.
 * MEASURED as a real blind spot (results/x86-64-reach.md ss4): the decompressor's .data segment
 * copy and the kernel's -load_delta global appeared at their addresses with NO logged write, so
 * "the watch shows no writer" was not "nothing wrote it".  One call per copy with a range
 * overlap test, so a 65 MB memcpy costs what a 1-byte store does. */
void v86c_dbg_watch_range(uint64_t phys, uint64_t len, uint64_t rip)
{
    int i;
    for (i = 0; i < watch_n; i++) {
        uint64_t a = watch[i].addr;
        uint64_t b = a + (uint64_t)watch[i].len;
        uint64_t lo = phys > a ? phys : a;
        uint64_t hi = (phys + len) < b ? (phys + len) : b;
        if (lo >= hi) continue;
        test_counts++;
        fprintf(stderr, "v86c dbg: WATCH R phys=0x%llx len=0x%llx overlap@0x%llx+0x%llx rip=0x%llx\n",
                (unsigned long long)phys, (unsigned long long)len, (unsigned long long)lo,
                (unsigned long long)(hi - lo), (unsigned long long)rip);
    }
}

/* A WATCHED ADDRESS THAT FAULTS NEVER REACHES A STORE, so the write hook above cannot see it.
 * This one is the faulting access itself -- the shape that answers "who writes 0x31558". */
void v86c_dbg_watch_fault(uint64_t addr, bool write, uint64_t rip)
{
    for (int i = 0; i < watch_n; i++) {
        uint64_t a = watch[i].addr;
        if (addr < a || addr >= a + (uint64_t)watch[i].len) continue;
        test_counts++;
        fprintf(stderr, "v86c dbg: WATCH F 0x%llx %s rip=0x%llx (the access FAULTED, no store)\n",
                (unsigned long long)addr, write ? "write" : "read", (unsigned long long)rip);
    }
}

void v86c_dbg_ring_record(uint64_t eip, const uint8_t *bytes, int n)
{
    int i;
    test_counts++;
    ring[ring_i].eip = eip;
    for (i = 0; i < 16; i++) ring[ring_i].bytes[i] = i < n ? bytes[i] : 0;
    ring_i = (ring_i + 1) % ring_n;
}

static void dump_ring(void)
{
    int i;
    fprintf(stderr, "v86c dbg: ring (%d recorded, oldest first):\n", ring_n);
    for (i = 0; i < ring_n; i++) {
        int k = (ring_i + i) % ring_n;
        fprintf(stderr, "v86c dbg:   eip=0x%llx bytes %02x %02x %02x %02x %02x %02x %02x %02x"
                        " %02x %02x %02x %02x %02x %02x %02x %02x\n",
                (unsigned long long)ring[k].eip,
                ring[k].bytes[0], ring[k].bytes[1], ring[k].bytes[2], ring[k].bytes[3],
                ring[k].bytes[4], ring[k].bytes[5], ring[k].bytes[6], ring[k].bytes[7],
                ring[k].bytes[8], ring[k].bytes[9], ring[k].bytes[10], ring[k].bytes[11],
                ring[k].bytes[12], ring[k].bytes[13], ring[k].bytes[14], ring[k].bytes[15]);
    }
}

/* THE EXECUTION WATCH (results/x86-64-reach.md).  Called BEFORE the instruction at `eip` runs, so * the registers printed are the ones the instruction sees -- that is the whole difference from
 * V86C_WATCH, which is called from the store path and can never answer "what was in %rax here".
 * It does NOT stop: a stop would end the run at the first hit, and the question is what the guest
 * does with the value, not that it got there. */
static void dump_chain(uint64_t va);
static void dump_peek(void);
static void dump_peek_sp(void);

static void dump_peek(void)
{
    const char *k = getenv("V86C_PEEK");
    const char *p = k;
    while (p != NULL && *p != '\0') {
        char *end;
        uint64_t a = strtoull(p, &end, 0);
        uint64_t len = 8;
        int i;
        if (*end == ':') len = strtoull(end + 1, &end, 0);
        if (len > 4096) len = 4096;
        fprintf(stderr, "v86c dbg: PEEK phys 0x%llx (%llu bytes):", (unsigned long long)a,
                (unsigned long long)len);
        for (i = 0; i < (int)len; i++) {
            fprintf(stderr, " %02x", (unsigned)read8((uint32_t)(a + (uint64_t)i)));
        }
        fprintf(stderr, "\n");
        p = (*end == ',') ? end + 1 : end;
        if (p == end) break;
    }
}

/* THE FRAME IS AT RSP, and the kernel's stack is not identity-mapped, so this dump is a VIRTUAL
 * read through the port's own walk (results/x86-64-nest.md).
 * THE WALK MUST BE THE NO-SIDE-EFFECTS ONE.  MEASURED (results/x86-64-peekperturb.md): the
 * side-effecting read DELIVERS the #PF (paging.c:201-215 -> trigger_pagefault ->
 * call_interrupt_vector), so a peek that ran off an unmapped page injected a real guest exception
 * mid-dump and changed the boot's path -- a 2048-byte V86C_PEEK_SP turned a stack-overflow stop at
 * 4,590,902,204 instructions into a 6,000,054,617-instruction run that stopped somewhere else
 * entirely.  A diagnostic that changes what it measures is worse than no diagnostic.
 * THE CAP IS THE STACK, NOT A LINE: a nesting guest's ancestor chain can be a full page deep
 * (results/x86-64-stackoverflow.md ss5 read none of it at 128 bytes), so the cap is 4 KiB. */
static void dump_peek_sp(void)
{
    const char *k = getenv("V86C_PEEK_SP");
    uint64_t len;
    int i;
    bool saved;
    if (k == NULL) return;
    len = strtoull(k, NULL, 0);
    if (len > 4096) len = 4096;
    saved = cpu->pagefault;
    fprintf(stderr, "v86c dbg: PEEK SP 0x%llx (%llu bytes):", (unsigned long long)cpu->reg64[4],
            (unsigned long long)len);
    for (i = 0; i < (int)len; i++) {
        uint32_t p = translate_address_read_no_side_effects((uint64_t)(cpu->reg64[4] + (uint64_t)i));
        if (cpu->pagefault) { fprintf(stderr, " <fault at +%d>", i); break; }
        fprintf(stderr, " %02x", (unsigned)read8(p));
    }
    fprintf(stderr, "\n");
    cpu->pagefault = saved;
}

void v86c_dbg_break_hit(uint64_t eip)
{
    int i;
    for (i = 0; i < brk_n; i++) {
        if (eip < brk[i].addr || eip >= brk[i].addr + brk[i].len) continue;
        brk[i].hits++;
        test_counts++;
        if (brk[i].hits <= brk_max) {
            fprintf(stderr, "v86c dbg: BREAK %d 0x%llx (hit %d) rax=0x%llx rbx=0x%llx rcx=0x%llx"
                            " rdx=0x%llx rsi=0x%llx rdi=0x%llx rbp=0x%llx rsp=0x%llx\n",
                    i, (unsigned long long)eip, brk[i].hits,
                    (unsigned long long)cpu->reg64[0], (unsigned long long)cpu->reg64[3],
                    (unsigned long long)cpu->reg64[1], (unsigned long long)cpu->reg64[2],
                    (unsigned long long)cpu->reg64[6], (unsigned long long)cpu->reg64[7],
                    (unsigned long long)cpu->reg64[5], (unsigned long long)cpu->reg64[4]);
            fprintf(stderr, "v86c dbg:   r8=0x%llx r9=0x%llx r10=0x%llx r11=0x%llx r12=0x%llx"
                            " r13=0x%llx r14=0x%llx r15=0x%llx\n",
                    (unsigned long long)cpu->reg64[8], (unsigned long long)cpu->reg64[9],
                    (unsigned long long)cpu->reg64[10], (unsigned long long)cpu->reg64[11],
                    (unsigned long long)cpu->reg64[12], (unsigned long long)cpu->reg64[13],
                    (unsigned long long)cpu->reg64[14], (unsigned long long)cpu->reg64[15]);
            fprintf(stderr, "v86c dbg:   cr0=0x%x cr3=0x%x cr4=0x%x efer=0x%llx cpl=%d"
                            " cs=0x%llx ss=0x%llx\n",
                    (unsigned)cpu->cr[0], (unsigned)cpu->cr[3], (unsigned)cpu->cr[4],
                    (unsigned long long)cpu->efer, (int)cpu->cpl,
                    (unsigned long long)cpu->segment_offsets[CS],
                    (unsigned long long)cpu->segment_offsets[SS]);
            fprintf(stderr, "v86c dbg:   gs_base=0x%llx kernel_gs=0x%llx fs_base=0x%llx\n",
                    (unsigned long long)cpu->segment_offsets[GS],
                    (unsigned long long)cpu->kernel_gs_base,
                    (unsigned long long)cpu->segment_offsets[FS]);
            fprintf(stderr, "v86c dbg:   idtr=0x%llx/%d gdtr=0x%llx/%d cr2=0x%x cr2_64=0x%llx"
                            " prefixes=0x%x osize32=%d\n",
                    (unsigned long long)cpu->idtr_offset, (int)cpu->idtr_size,
                    (unsigned long long)cpu->gdtr_offset, (int)cpu->gdtr_size,
                    (unsigned)cpu->cr[2], (unsigned long long)cpu->cr2_64,
                    (unsigned)cpu->prefixes, (int)is_osize_32());
            { const char *c = getenv("V86C_BREAK_CHAIN");
              if (c != NULL) dump_chain((uint64_t)strtoull(c, NULL, 0)); }
            dump_peek();
            dump_peek_sp();
            if (brk_ring && v86c_dbg_ring_on) dump_ring();
        }
    }
}

/* THE BOOT_PARAMS ARTIFACT (results/x86-64-bootparams.md): the fields a kernel's early code reads,
 * printed from the struct the firmware handed over -- `V86C_BOOTPARAMS=<linear address>` with
 * V86C_DUMP_ON_STOP.  Offsets are the standard zero-page layout (arch/x86/include/uapi/asm/bootparam.h). */
static uint32_t bp32(uint64_t b, uint32_t off)
{
    return (uint32_t)read8((uint32_t)(b + off)) | ((uint32_t)read8((uint32_t)(b + off + 1)) << 8) |
           ((uint32_t)read8((uint32_t)(b + off + 2)) << 16) | ((uint32_t)read8((uint32_t)(b + off + 3)) << 24);
}
static uint64_t bp64(uint64_t b, uint32_t off)
{
    return (uint64_t)bp32(b, off) | ((uint64_t)bp32(b, off + 4) << 32);
}
static void dump_boot_params(uint64_t b)
{
    int i;
    fprintf(stderr, "v86c dbg: boot_params @0x%llx e820_entries=%u alt_mem_k=0x%x setup_sects=%u\n",
            (unsigned long long)b, (unsigned)read8((uint32_t)(b + 0x1e8)), (unsigned)bp32(b, 0x1e0),
            (unsigned)read8((uint32_t)(b + 0x1f1)));
    fprintf(stderr, "v86c dbg:   code32_start=0x%x ramdisk_image=0x%x ramdisk_size=0x%x"
                    " ext_ramdisk=0x%x/0x%x cmd_line_ptr=0x%x ext_cmd=0x%x cmdline_size=0x%x\n",
            (unsigned)bp32(b, 0x214), (unsigned)bp32(b, 0x218), (unsigned)bp32(b, 0x21c),
            (unsigned)bp32(b, 0x0c0), (unsigned)bp32(b, 0x0c4), (unsigned)bp32(b, 0x228),
            (unsigned)bp32(b, 0x0c8), (unsigned)bp32(b, 0x238));
    fprintf(stderr, "v86c dbg:   heap_end_ptr=0x%x initrd_addr_max=0x%x kernel_alignment=0x%x"
                    " relocatable=%u pref_address=0x%llx init_size=0x%x\n",
            (unsigned)bp32(b, 0x224), (unsigned)bp32(b, 0x22c), (unsigned)bp32(b, 0x230),
            (unsigned)read8((uint32_t)(b + 0x234)), (unsigned long long)bp64(b, 0x258),
            (unsigned)bp32(b, 0x260));
    for (i = 0; i < 6; i++) {
        uint64_t e = b + 0x2d0 + (uint64_t)(20 * i);
        fprintf(stderr, "v86c dbg:   e820[%d] base=0x%llx len=0x%llx type=%u\n", i,
                (unsigned long long)bp64(e, 0), (unsigned long long)bp64(e, 8),
                (unsigned)bp32(e, 16));
    }
}

/* the chain walk this lane has hand-written three times */
static void dump_chain(uint64_t va)
{
    uint64_t cr3 = (uint64_t)(uint32_t)cpu->cr[3] & ~(uint64_t)0xFFF;
    uint64_t idx[4];
    uint64_t entry[4];
    int lvl;
    if ((cpu->efer & EFER_LMA) == 0) {
        fprintf(stderr, "v86c dbg: chain va=0x%llx -- not in long mode, no 4-level chain\n",
                (unsigned long long)va);
        return;
    }
    idx[0] = (va >> 39) & 511; idx[1] = (va >> 30) & 511;
    idx[2] = (va >> 21) & 511; idx[3] = (va >> 12) & 511;
    fprintf(stderr, "v86c dbg: chain va=0x%llx cr3=0x%llx idx=%llu/%llu/%llu/%llu\n",
            (unsigned long long)va, (unsigned long long)cr3,
            (unsigned long long)idx[0], (unsigned long long)idx[1],
            (unsigned long long)idx[2], (unsigned long long)idx[3]);
    entry[0] = (uint64_t)read64s((uint32_t)(cr3 + idx[0] * 8));
    fprintf(stderr, "v86c dbg:   PML4E  @0x%llx = 0x%llx\n",
            (unsigned long long)(cr3 + idx[0] * 8), (unsigned long long)entry[0]);
    for (lvl = 1; lvl < 4; lvl++) {
        uint64_t base = entry[lvl - 1] & 0x000FFFFFFFFFF000ull;
        if ((entry[lvl - 1] & 1) == 0) {
            fprintf(stderr, "v86c dbg:   level %d ABSENT (the previous entry is not present)\n", lvl);
            return;
        }
        if (lvl == 2 && (entry[lvl - 1] & 0x80) != 0) {
            fprintf(stderr, "v86c dbg:   1 GiB page at level 2\n");
            return;
        }
        entry[lvl] = (uint64_t)read64s((uint32_t)(base + idx[lvl] * 8));
        fprintf(stderr, "v86c dbg:   %s @0x%llx = 0x%llx%s\n",
                lvl == 1 ? "PDPTE" : lvl == 2 ? "PDE  " : "PTE  ",
                (unsigned long long)(base + idx[lvl] * 8), (unsigned long long)entry[lvl],
                (entry[lvl] & 1) == 0 ? "  (ABSENT)" : "");
        if ((entry[lvl] & 1) == 0) return;
    }
}

/* ONE LINE PER DELIVERY, bounded by the print limit, and the window's CLOSURE printed ONCE so a
 * later silence is never read as a quiet guest -- this lane has lost five units to an instrument's
 * silence, and "the trace stopped" is the most expensive of them. */
void v86c_dbg_ints_delivery(int32_t vector, bool is_int, bool has_error_code,
                            int32_t error_code, int depth, uint64_t interrupted_rip,
                            uint64_t rsp, uint64_t cs)
{
    if (!v86c_dbg_ints_on) return;   /* the entire cost when off: this test */
    ints_total++;
    if (vector >= 0 && vector < V86C_DBG_INTS_VECTORS) ints_hist[vector]++;
    if (depth > ints_max_depth) ints_max_depth = depth;
    if (ints_printed < ints_limit) {
        char err[24];
        if (has_error_code) snprintf(err, sizeof err, "0x%x", (unsigned)error_code);
        else strcpy(err, "-");
        ints_printed++;
        test_counts++;
        fprintf(stderr, "v86c dbg: INTS %d vec=%d int=%d err=%s cpl=%d depth=%d from_rip=0x%llx"
                        " rsp=0x%llx cs=0x%llx\n",
                ints_total, (int)vector, is_int ? 1 : 0, err, (int)cpu->cpl, depth,
                (unsigned long long)interrupted_rip, (unsigned long long)rsp,
                (unsigned long long)cs);
    }
    else if (!ints_window_closed) {
        ints_window_closed = true;
        if (ints_limit == 0) {
            fprintf(stderr, "v86c dbg: INTS per-delivery lines are OFF (limit 0): counting only,"
                            " the stop summary carries the histogram\n");
        }
        else {
            fprintf(stderr, "v86c dbg: INTS print window CLOSED at %d line(s) (delivery %d); the"
                            " histogram keeps counting and the stop summary carries the totals\n",
                    ints_limit, ints_total);
        }
    }
}

/* THE SUMMARY IS GATED ON THE TRACE BEING ARMED, NEVER ON THE PRINT LIMIT: with a limit of 0 the
 * per-delivery lines are silent by design, and the histogram is the ONLY thing that answers which
 * vector drives the loop. */
static void dump_ints_summary(void)
{
    int i, k, n = 0;
    int vec[V86C_DBG_INTS_VECTORS];
    test_counts++;   /* the summary is an entry this tool PRODUCED: a caller can assert it spoke */
    fprintf(stderr, "v86c dbg: INTS total=%d max_depth=%d printed=%d limit=%d\n",
            ints_total, ints_max_depth, ints_printed, ints_limit);
    for (i = 0; i < V86C_DBG_INTS_VECTORS; i++) if (ints_hist[i] > 0) vec[n++] = i;
    if (n == 0) { fprintf(stderr, "v86c dbg:   INTS no delivery was traced\n"); return; }
    fprintf(stderr, "v86c dbg:   INTS vectors:");
    for (k = 0; k < V86C_DBG_INTS_TOP && k < n; k++) {
        int j, best = k;
        for (j = k + 1; j < n; j++) if (ints_hist[vec[j]] > ints_hist[vec[best]]) best = j;
        { int t = vec[k]; vec[k] = vec[best]; vec[best] = t; }
        fprintf(stderr, " vec=%d x%d", vec[k], ints_hist[vec[k]]);
    }
    fprintf(stderr, "\n");
}

void v86c_dbg_on_stop(const char *why)
{
    int i;
    if (!dump_on_stop && !v86c_dbg_ring_on && !v86c_dbg_ints_on) return;
    test_counts++;
    fflush(stderr);
    fprintf(stderr, "v86c dbg: STOP (%s)\n", why != NULL ? why : "?");
    if (v86c_dbg_break_on) {
        for (i = 0; i < brk_n; i++) {
            fprintf(stderr, "v86c dbg: break slot %d 0x%llx+0x%llx hits=%d\n", i,
                    (unsigned long long)brk[i].addr, (unsigned long long)brk[i].len, brk[i].hits);
        }
    }
    if (v86c_dbg_ints_on) dump_ints_summary();
    if (v86c_dbg_ring_on) dump_ring();
    if (dump_on_stop) {
        const char *c = getenv("V86C_CHAIN");
        fprintf(stderr, "v86c dbg: bases cs=0x%llx ss=0x%llx ds=0x%llx fs=0x%llx gs=0x%llx\n",
                (unsigned long long)cpu->segment_offsets[CS], (unsigned long long)cpu->segment_offsets[SS],
                (unsigned long long)cpu->segment_offsets[DS], (unsigned long long)cpu->segment_offsets[FS],
                (unsigned long long)cpu->segment_offsets[GS]);
        fprintf(stderr, "v86c dbg: state rip=0x%llx cr0=0x%x cr2=0x%x cr3=0x%x cr4=0x%x efer=0x%llx"
                        " idtr=0x%llx/%d gdtr=0x%llx/%d cpl=%d\n",
                (unsigned long long)cpu->instruction_pointer, (unsigned)cpu->cr[0],
                (unsigned)cpu->cr[2], (unsigned)cpu->cr[3], (unsigned)cpu->cr[4],
                (unsigned long long)cpu->efer, (unsigned long long)cpu->idtr_offset,
                (int)cpu->idtr_size, (unsigned long long)cpu->gdtr_offset, (int)cpu->gdtr_size,
                (int)cpu->cpl);
        fprintf(stderr, "v86c dbg: regs rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx rsi=0x%llx"
                        " rdi=0x%llx rbp=0x%llx rsp=0x%llx\n",
                (unsigned long long)cpu->reg64[0], (unsigned long long)cpu->reg64[3],
                (unsigned long long)cpu->reg64[1], (unsigned long long)cpu->reg64[2],
                (unsigned long long)cpu->reg64[6], (unsigned long long)cpu->reg64[7],
                (unsigned long long)cpu->reg64[5], (unsigned long long)cpu->reg64[4]);
        if (c != NULL) dump_chain((uint64_t)strtoull(c, NULL, 0));
        { const char *bp = getenv("V86C_BOOTPARAMS");
          if (bp != NULL) dump_boot_params((uint64_t)strtoull(bp, NULL, 0)); }
    }
    fflush(stderr);
}
