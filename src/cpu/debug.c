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
bool v86c_dbg_ring_on;
static bool dump_on_stop;

#define V86C_DBG_MAX_WATCH 8
static struct { uint64_t addr; int len; } watch[V86C_DBG_MAX_WATCH];
static int watch_n;

#define V86C_DBG_RING_MAX 4096
static struct { uint64_t eip; uint8_t bytes[16]; } ring[V86C_DBG_RING_MAX];
static int ring_n, ring_i;
static int test_counts;
void v86c_dbg_test_arm(void)
{
    watch_n = 1; watch[0].addr = 0x31558; watch[0].len = 1;
    v86c_dbg_watch_on = true;
    ring_n = 8; ring_i = 0; v86c_dbg_ring_on = true;
    test_counts = 0;
}
void v86c_dbg_test_disarm(void)
{
    v86c_dbg_watch_on = false; v86c_dbg_ring_on = false; watch_n = 0; ring_n = 0;
}
int v86c_dbg_test_counts(void) { return test_counts; }


void v86c_dbg_init(void)
{
    const char *w = getenv("V86C_WATCH");
    const char *r = getenv("V86C_RING");
    const char *d = getenv("V86C_DUMP_ON_STOP");
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
    dump_on_stop = d != NULL && *d != '0';
    /* UNBUFFERED BY DEFAULT: the whole point is that a crash cannot eat the output */
    setvbuf(stderr, NULL, _IONBF, 0);
    if (v86c_dbg_watch_on || v86c_dbg_ring_on || dump_on_stop) {
        fprintf(stderr, "v86c dbg: tools on (watch=%d ring=%d dump=%d)\n", watch_n, ring_n,
                (int)dump_on_stop);
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

void v86c_dbg_on_stop(const char *why)
{
    int i;
    if (!dump_on_stop && !v86c_dbg_ring_on) return;
    test_counts++;
    fflush(stderr);
    fprintf(stderr, "v86c dbg: STOP (%s)\n", why != NULL ? why : "?");
    if (v86c_dbg_ring_on) {
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
    }
    fflush(stderr);
}
