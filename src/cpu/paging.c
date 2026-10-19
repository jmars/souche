/*
 * paging.c -- address translation, the TLB, the PSE/PAE-capable page walk, the
 * page-fault path and the safe_* accessors.
 *
 * Port of v86/src/rust/cpu/cpu.rs (see paging.h for the file:line map).  PAE is
 * ported (PAE-1): the walk interprets 64-bit PDPTE/PDE/PTE entries exactly as
 * upstream requires (high half 0 except NX, cpu.rs:2058), and load_pdpte's two
 * guest-reachable asserts became #GP(0) delivered at the load site.
 */

#include <stdio.h>
#include <stdlib.h>

#include "paging.h"
#include "cpu.h"         /* S3: unimplemented_instruction -- the long-mode stop below */
#include "cpu_backend.h" /* S2: the long-mode refusal (plan ss3.5.2, C5) */
#include "uop_cache.h"   /* PERF-4/M2: the write-path invalidation hook (plan ss4.1) */

int32_t tlb_data[0x100000];
int32_t valid_tlb_entries[VALID_TLB_ENTRY_MAX];
int32_t valid_tlb_entries_count = 0;

/* ------------------------------------------------------------------ *
 * S5a (plan ss4's PARTIAL verdict for tlb_data): THE 64-BIT-KEYED TLB.
 *
 * `tlb_data[0x100000]` is indexed by `addr >> 12` and therefore cannot key a 48-bit virtual
 * page -- the index alone needs 36 bits.  The plan's verdict is a SECOND, small,
 * direct-mapped table used ONLY under EFER.LMA, with the big one left exactly as it is so
 * the 32-bit path's invalidation semantics do not move by one entry.
 *
 * SHAPE, and why it is not the 32-bit one: the 32-bit entry packs `physical_page ^
 * (page << 12)` and translate_address recovers the address by XORing the linear address back
 * in.  That trick needs the virtual page number to fit in 32 bits, which is exactly what long
 * mode ends.  Here the tag is the 64-bit VIRTUAL PAGE NUMBER and the entry is
 * `physical_page_base | info_bits` (the same TLB_* bits, bits 0-6), so the physical address
 * is `(entry & 0xFFFFF000) | (addr & 0xFFF)` -- no XOR, and the port's 32-bit physical bound
 * is enforced where the walk composes it (a PTE above 4 GiB stops by name, plan ss5.1).
 *
 * `entry != 0` is the validity test: a successful walk always sets TLB_VALID.
 * ------------------------------------------------------------------ */
#define TLB64_ENTRIES 4096 /* power of two; a 32 KiB table, no allocation */
static uint64_t tlb64_tag[TLB64_ENTRIES];
static int32_t tlb64_data[TLB64_ENTRIES];

static inline uint32_t tlb64_slot(uint64_t page) { return (uint32_t)page & (TLB64_ENTRIES - 1u); }

static void tlb64_clear_all(void)
{
    for (int i = 0; i < TLB64_ENTRIES; i++) {
        tlb64_tag[i] = 0;
        tlb64_data[i] = 0;
    }
}

/* ------------------------------------------------------------------ *
 * loud unimplemented paths
 * ------------------------------------------------------------------ */

static void fatal(const char *msg)
{
    fprintf(stderr, "v86c: FATAL: %s\n", msg);
    abort();
}

/* ------------------------------------------------------------------ *
 * the TLB -- cpu.rs:2338-2385, 4467-4476
 * ------------------------------------------------------------------ */

/* clear_tlb_code(page) (cpu.rs:4458-4465) frees JIT code; there is no JIT, so the
 * call sites below simply drop it.  TLB_HAS_CODE is likewise never set. */

void full_clear_tlb(void)
{
    /* clear tlb including global pages */
    tlb64_clear_all(); /* S5a: the long-mode table is flushed from the same four entries */
    cpu->last_virt_eip = VIRT_EIP_NONE;
    for (int32_t i = 0; i < valid_tlb_entries_count; i++) {
        int32_t page = valid_tlb_entries[i];
        tlb_data[page] = 0;
    }
    valid_tlb_entries_count = 0;
    /* CHECK_TLB_INVARIANTS (cpu.rs:260) is a constant false upstream; its debug loop
     * is dropped with it. */
}

void clear_tlb(void)
{
    /* clear tlb excluding global pages */
    /* S5a: the 64-bit table has no GLOBAL filtering yet (no long-mode page in this slice's
     * corpus carries PGE, and holding a stale non-global entry is the silent-wrong
     * direction), so a "clear the non-global entries" request clears it whole.  S11 owns
     * the page-flags split here; the sentence is the gap, not a claim. */
    tlb64_clear_all();
    cpu->last_virt_eip = VIRT_EIP_NONE;
    int32_t global_page_offset = 0;
    for (int32_t i = 0; i < valid_tlb_entries_count; i++) {
        int32_t page = valid_tlb_entries[i];
        int32_t entry = tlb_data[page];
        if ((entry & TLB_GLOBAL) != 0) {
            /* reinsert at the front */
            valid_tlb_entries[global_page_offset] = page;
            global_page_offset += 1;
        }
        else {
            tlb_data[page] = 0;
        }
    }
    valid_tlb_entries_count = global_page_offset;
}

void invlpg(uint64_t addr)
{
    /* the TLB is indexed by the model's 32-bit physical/virtual page (plan ss4's PARTIAL
     * verdict for tlb_data; the 64-bit-keyed table is S11's) */
    int32_t page = (int32_t)((uint32_t)addr >> 12);
    /* Note (cpu.rs:4469-4473): this does NOT remove the page from valid_tlb_entries.
     * It is not necessary, because a full list is emptied by clear_tlb, which drops
     * this entry too as it is not global.  It does mean valid_tlb_entries can hold
     * some already-invalid pages -- preserved here, because the bookkeeping is part of
     * what the plan ss3 asks to keep verbatim. */
    tlb_data[page] = 0;
    cpu->last_virt_eip = VIRT_EIP_NONE;

    /* S5a: and the long-mode entry for THIS page (the same four entry points the plan names). */
    {
        uint64_t vpage = addr >> 12;
        uint32_t slot = tlb64_slot(vpage);
        if (tlb64_data[slot] != 0 && tlb64_tag[slot] == vpage) {
            tlb64_data[slot] = 0;
        }
    }
}

/* ------------------------------------------------------------------ *
 * page faults -- cpu.rs:2461-2512
 * ------------------------------------------------------------------ */

void trigger_pagefault(uint64_t addr,
                       bool present,
                       bool write,
                       bool user,
                       bool is_instruction_fetch,
                       bool rsvd)
{
    /* config::LOG_PAGE_FAULTS + dbg_trace dropped (no log channel in M1a) */
    cpu->cr[2] = (int32_t)addr; /* CR2 is 32-bit in this model (ss5.1); long mode's 64-bit
                                 * CR2 is S11's, and the projection is named here so a
                                 * >4 GiB fault address is a reported gap, not a silent
                                 * one (paging.h) */
    /* invalidate tlb entry */
    int32_t page = (int32_t)((uint32_t)addr >> 12);
    tlb_data[page] = 0;
    bool instruction_fetch = is_instruction_fetch &&
                             ((cpu->cr[4] & CR4_SMEP) != 0 ||
                              ((cpu->cr[4] & CR4_PAE) != 0 && (cpu->efer & EFER_NXE) != 0));
    int32_t error_code = ((int32_t)instruction_fetch) << 4 | ((int32_t)rsvd) << 3 |
                         ((int32_t)user) << 2 | ((int32_t)write) << 1 | (int32_t)present;
    /* the jit branch (cpu.rs:2503-2509) sets jit_exit_reason instead of delivering --
     * DROPPED with the JIT; this port is always the delivering path. */
    cpu->instruction_pointer = cpu->previous_ip;
    /* DEBUG: js::cpu_exception_hook(CPU_EXCEPTION_PF) -- the debugger hook, dropped */
    call_interrupt_vector(CPU_EXCEPTION_PF, false, true /* error code always present */, error_code);
}

/* ------------------------------------------------------------------ *
 * the page walk -- cpu.rs:2062-2336
 *
 * Returns the TLB entry; on fault it sets cpu->pagefault and returns 0 (the
 * caller checks the flag before using the value).  `jit` is gone (see paging.h);
 * the PAE arm (cpu.rs:2087-2134) is ported below.
 * ------------------------------------------------------------------ */

static int32_t do_page_walk(int32_t addr,
                            bool for_writing,
                            bool user,
                            bool side_effects,
                            bool is_instruction_fetch)
{
    bool global;
    bool allow_user = true;
    int32_t page = (int32_t)((uint32_t)addr >> 12);
    uint32_t high;

    int32_t cr0 = cpu->cr[0];
    int32_t cr4 = cpu->cr[4];
    bool nxe = (cpu->efer & EFER_NXE) != 0;
    bool no_exec = false;

    if ((cr0 & CR0_PG) == 0) {
        /* paging disabled */
        high = (uint32_t)addr & 0xFFFFF000u;
        global = false;
    }
    else {
        uint32_t page_dir_addr;
        int32_t page_dir_entry;
        /* cpu.rs:2084: the high half of a 64-bit entry must be 0, except the NX bit
         * when EFER.NXE is set */
        uint64_t pae_reserved_mask =
            0x7FFFFFFF00000000ull | (nxe ? 0ull : PAGE_TABLE_NX_MASK);

        if ((cr4 & CR4_PAE) != 0) {
            uint64_t pdpt_entry = cpu->reg_pdpte[(uint32_t)addr >> 30];
            if (((int32_t)pdpt_entry & PAGE_TABLE_PRESENT_MASK) == 0) {
                if (side_effects) {
                    trigger_pagefault(addr, false, for_writing, user, is_instruction_fetch, false);
                }
                cpu->pagefault = true;
                return 0;
            }

            page_dir_addr = ((uint32_t)pdpt_entry & 0xFFFFF000u) +
                            ((((uint32_t)addr >> 21) & 0x1FFu) << 3);
            /* read64s returns int64_t; cast to uint64_t BEFORE the mask work so the
             * reserved-mask check sees the true 64-bit value */
            uint64_t pde = (uint64_t)read64s(page_dir_addr);
            page_dir_entry = (int32_t)pde;
            if ((page_dir_entry & PAGE_TABLE_PRESENT_MASK) != 0) {
                /* bits 13 to 20 are reserved in a 2 MiB page directory entry */
                uint64_t reserved_mask =
                    pae_reserved_mask |
                    ((page_dir_entry & PAGE_TABLE_PSE_MASK) != 0 ? 0x1FE000u : 0u);
                if ((pde & reserved_mask) != 0) {
                    if (side_effects) {
                        trigger_pagefault(
                            addr, true, for_writing, user, is_instruction_fetch, true);
                    }
                    cpu->pagefault = true;
                    return 0;
                }
                no_exec = (pde & PAGE_TABLE_NX_MASK) != 0;
            }
        }
        else {
            page_dir_addr = (uint32_t)cpu->cr[3] + (((uint32_t)addr >> 22) << 2);
            page_dir_entry = read32s(page_dir_addr);
        }

        if ((page_dir_entry & PAGE_TABLE_PRESENT_MASK) == 0) {
            if (side_effects) {
                trigger_pagefault(addr, false, for_writing, user, is_instruction_fetch, false);
            }
            cpu->pagefault = true;
            return 0;
        }

        bool kernel_write_override = !user && (cr0 & CR0_WP) == 0;
        bool allow_write = (page_dir_entry & PAGE_TABLE_RW_MASK) != 0;
        allow_user = allow_user && (page_dir_entry & PAGE_TABLE_USER_MASK) != 0;

        if ((page_dir_entry & PAGE_TABLE_PSE_MASK) != 0 &&
            ((cr4 & CR4_PAE) != 0 || (cr4 & CR4_PSE) != 0)) {
            /* 2 MiB (PAE) or 4 MiB (PSE) page */
            if ((for_writing && !allow_write && !kernel_write_override) ||
                (user && !allow_user) || (is_instruction_fetch && no_exec)) {
                if (side_effects) {
                    trigger_pagefault(addr, true, for_writing, user, is_instruction_fetch, false);
                }
                cpu->pagefault = true;
                return 0;
            }

            /* set the accessed and dirty bits */
            int32_t new_page_dir_entry =
                page_dir_entry | PAGE_TABLE_ACCESSED_MASK |
                (for_writing ? PAGE_TABLE_DIRTY_MASK : 0);

            if (side_effects && page_dir_entry != new_page_dir_entry) {
                write8(page_dir_addr, new_page_dir_entry);
            }

            high = ((cr4 & CR4_PAE) != 0)
                       ? (((uint32_t)page_dir_entry & 0xFFE00000u) |
                          ((uint32_t)addr & 0x1FF000u))
                       : (((uint32_t)page_dir_entry & 0xFFC00000u) |
                          ((uint32_t)addr & 0x3FF000u));
            global = (page_dir_entry & PAGE_TABLE_GLOBAL_MASK) == PAGE_TABLE_GLOBAL_MASK;
        }
        else {
            uint32_t page_table_addr;
            int32_t page_table_entry;

            if ((cr4 & CR4_PAE) != 0) {
                page_table_addr = ((uint32_t)page_dir_entry & 0xFFFFF000u) +
                                  ((((uint32_t)addr >> 12) & 0x1FFu) << 3);
                uint64_t pte = (uint64_t)read64s(page_table_addr);
                page_table_entry = (int32_t)pte;
                if ((page_table_entry & PAGE_TABLE_PRESENT_MASK) != 0) {
                    if ((pte & pae_reserved_mask) != 0) {
                        if (side_effects) {
                            trigger_pagefault(
                                addr, true, for_writing, user, is_instruction_fetch, true);
                        }
                        cpu->pagefault = true;
                        return 0;
                    }
                    /* |=: the PDE's NX carries over on the 4 KiB path (cpu.rs:2220) */
                    no_exec |= (pte & PAGE_TABLE_NX_MASK) != 0;
                }
            }
            else {
                page_table_addr = ((uint32_t)page_dir_entry & 0xFFFFF000u) +
                                  ((((uint32_t)addr >> 12) & 0x3FFu) << 2);
                page_table_entry = read32s(page_table_addr);
            }

            bool present = (page_table_entry & PAGE_TABLE_PRESENT_MASK) != 0;
            allow_write = allow_write && (page_table_entry & PAGE_TABLE_RW_MASK) != 0;
            allow_user = allow_user && (page_table_entry & PAGE_TABLE_USER_MASK) != 0;

            if (!present || (for_writing && !allow_write && !kernel_write_override) ||
                (user && !allow_user) || (is_instruction_fetch && no_exec)) {
                if (side_effects) {
                    trigger_pagefault(
                        addr, present, for_writing, user, is_instruction_fetch, false);
                }
                cpu->pagefault = true;
                return 0;
            }

            /* Set the accessed and dirty bits.
             * Note: the dirty bit is only set on the page table entry. */
            int32_t new_page_dir_entry = page_dir_entry | PAGE_TABLE_ACCESSED_MASK;
            if (side_effects && new_page_dir_entry != page_dir_entry) {
                write8(page_dir_addr, new_page_dir_entry);
            }
            int32_t new_page_table_entry =
                page_table_entry | PAGE_TABLE_ACCESSED_MASK |
                (for_writing ? PAGE_TABLE_DIRTY_MASK : 0);
            if (side_effects && page_table_entry != new_page_table_entry) {
                write8(page_table_addr, new_page_table_entry);
            }

            high = (uint32_t)page_table_entry & 0xFFFFF000u;
            global = (page_table_entry & PAGE_TABLE_GLOBAL_MASK) == PAGE_TABLE_GLOBAL_MASK;
        }
    }

    if (side_effects && tlb_data[page] == 0) {
        /* profiler::stat_increment(stat::TLB_MISS/TLB_FULL/TLB_GLOBAL_FULL) dropped */
        if (valid_tlb_entries_count == VALID_TLB_ENTRY_MAX) {
            clear_tlb();
            /* also clear global entries if the tlb is almost full after clearing the
             * non-global pages */
            if (valid_tlb_entries_count > VALID_TLB_ENTRY_MAX * 3 / 4) {
                full_clear_tlb();
            }
        }
        DBG_ASSERT(valid_tlb_entries_count < VALID_TLB_ENTRY_MAX);
        valid_tlb_entries[valid_tlb_entries_count] = page;
        valid_tlb_entries_count += 1;
        /* TODO upstream: valid_tlb_entries can contain duplicates because invlpg drops
         * the entry from tlb_data but not from valid_tlb_entries (cpu.rs:2287-2289). */
    }

    bool is_in_mapped_range = in_mapped_range(high);
    /* upstream: has_code = !is_in_mapped_range && jit::jit_page_has_code(...).  This port
     * HAS a JIT now (V86C_JIT_X64), but its code protection does NOT ride this bit: a
     * guest write to a page holding decoded or compiled code is caught by the per-store
     * dirty/code bitmaps (uop_page_code, PERF-FIX-3's hooks) before any TLB entry matters,
     * so TLB_HAS_CODE is never set here -- keep it that way unless the uop-cache's own
     * invalidation chain moves onto this bit.  (The `else { true }` arm of upstream's
     * side_effects test exists only for translate_address_read_no_side_effects, which
     * discards this value anyway.) */
    bool has_code = false;
    int32_t info_bits = TLB_VALID | (for_writing ? 0 : TLB_READONLY) |
                        (allow_user ? 0 : TLB_NO_USER) |
                        (is_in_mapped_range ? TLB_IN_MAPPED_RANGE : 0) |
                        ((global && (cr4 & CR4_PGE) != 0) ? TLB_GLOBAL : 0) |
                        (has_code ? TLB_HAS_CODE : 0) | (no_exec ? TLB_NO_EXEC : 0);

    /* upstream adds mem8's host address here and subtracts it in translate_address;
     * this port stores the guest physical page instead (see paging.h) */
    int32_t tlb_entry = (int32_t)((high ^ (uint32_t)page << 12) | (uint32_t)info_bits);

    DBG_ASSERT(((high ^ ((uint32_t)page << 12)) & 0xFFF) == 0);
    if (side_effects) {
        tlb_data[page] = tlb_entry;
        /* jit::update_tlb_code(virt_page, phys_page) -- JIT-only */
    }

    return tlb_entry;
}

/* ------------------------------------------------------------------ *
 * S5a: THE 4-LEVEL WALK, 4 KiB PAGES ONLY (plan ss6 S5, ss2.2.1)
 *
 * The plan's payoff argument, made concrete: this is NOT a second pager.  It is
 * `do_page_walk` above with ONE more level in front (PML4), a 64-bit base arithmetic and a
 * canonicality-free linear address; every per-entry rule -- present, RW, US, the reserved
 * high half, NX, the accessed/dirty update, the fault's error-code composition -- is the same
 * code shape, and the shared bits (PAGE_TABLE_*, TLB_*) are literally the same macros.
 *
 * WHAT IT DELIBERATELY DOES NOT DO, each a NAMED STOP rather than a silent answer, because a
 * wrong answer here is a wrong PAGE (the silent-wrong class this tree refuses):
 *   - a 1 GiB page (PDPTE.PS) and a 2 MiB page (PDE.PS): S11's.  The PAE walk above already
 *     composes 2 MiB pages at 32 bits; composing them under a 4-level walk needs the 1 GiB
 *     case beside it and its own mutation probe, and this slice's corpus uses 4 KiB pages
 *     throughout.  Falling back to 4 KiB here would read the entry as a page-table pointer
 *     and translate to a page nobody mapped -- the exact "return a wrong page" outcome.
 *   - a page frame at or above 4 GiB: the port's physical model is 32 bits (plan ss5.1).  A
 *     PTE that names one STOPS BY NAME rather than truncating, because truncating would turn
 *     a guest's perfectly legal mapping into a device read.
 *   - the canonicality check (bits 63:48 != bit 47): S7/S11.  Stated so the gap is a gap; the
 *     corpus cannot express a non-canonical load until S7 owns control flow, and the check
 *     without a #GP instrument would be an untested rule (plan ss2.2's rule).
 *
 * Returns `physical_page_base | info_bits` (see the 64-bit TLB above); 0 with cpu->pagefault
 * set on every fault.
 * ------------------------------------------------------------------ */

#define PAGE_TABLE_PML4E_PRESENT 0x1ull  /* a PML4E uses the same present bit; named for the walk */

static int32_t do_page_walk64(uint64_t addr,
                              bool for_writing,
                              bool user,
                              bool side_effects,
                              bool is_instruction_fetch)
{
    int32_t cr0 = cpu->cr[0];
    int32_t cr4 = cpu->cr[4];
    bool nxe = (cpu->efer & EFER_NXE) != 0;
    bool no_exec = false;
    bool allow_user = true;
    bool allow_write = true;
    bool global = false;
    uint32_t high = 0;

    /* THE 4-LEVEL RESERVED MASK, and it is NOT the PAE one (paging.c:152 above uses
     * 0x7FFFFFFF00000000, which is right for a PAE entry whose frame is 32 bits).  In a
     * 4-level entry the frame is bits 12-51, so bits 52-62 are the reserved ones and NX is
     * writable only when EFER.NXE is set.  Getting this wrong in the PAE direction would
     * #PF on a perfectly legal frame above 4 GiB with a reserved-bit error code -- the wrong
     * FAULT, which is its own silent class.  A frame above 4 GiB is legal here and the port
     * cannot represent it, so it is the NAMED BOUND below that answers, not this mask. */
    uint64_t reserved_mask = 0x7FF0000000000000ull | (nxe ? 0ull : PAGE_TABLE_NX_MASK);

    /* A TABLE READ, BOUNDED BY GUEST RAM.  `read64s` indexes mem8 with a uint32_t and has no
     * bound of its own, so a table base that is under 4 GiB but PAST the guest's RAM is a HOST
     * OUT-OF-BOUNDS READ -- the port reading its own heap (or dying) where it should name the
     * problem.  MEASURED: a mutation that pointed a PDPTE at 0x83106000 (2.2 GB, in a 2 MiB
     * machine) read garbage and turned a table error into a bare #PF with the wrong reason.
     * Page tables must live in guest RAM, so anything else is a named stop.  The 32-bit/PAE
     * walk above has the same exposure through its own read64s calls and is NOT fixed here:
     * it is pre-existing and reachable only with paging tables outside RAM, and S11 owns the
     * whole walk's system layer (results/x86-64-s5.md ss7 names it). */
    #define WALK64_TABLE(addr_)                                                            \
        do {                                                                               \
            if ((uint64_t)(addr_) >= (uint64_t)cpu->memory_size) {                          \
                char stop_[160];                                                             \
                snprintf(stop_, sizeof(stop_),                                              \
                         "page walk: a page-table read at physical 0x%x is outside guest"   \
                         " RAM (0x%x)", (unsigned)(addr_), (unsigned)cpu->memory_size);      \
                unimplemented_instruction(stop_);                                            \
                cpu->pagefault = true;                                                       \
                return 0;                                                                    \
            }                                                                                \
        } while (0)

    /* THE PHYSICAL BOUND, at every level (plan ss5.1): a table base or a page frame at or
     * above 4 GiB is a legal 4-level entry and an unrepresentable address here, so the walk
     * STOPS BY NAME rather than truncating it into a different table or page. */
    #define WALK64_BOUND(entry, what)                                                       \
        do {                                                                                \
            if (((entry) & 0x000FFFFFFFFFF000ull) >= 0x100000000ull) {                       \
                char stop_[160];                                                            \
                snprintf(stop_, sizeof(stop_),                                              \
                         "page walk: %s 0x%llx is above the port's 32-bit physical model"   \
                         " (plan ss5.1)", (what),                                           \
                         (unsigned long long)((entry) & 0x000FFFFFFFFFF000ull));             \
                unimplemented_instruction(stop_);                                            \
                cpu->pagefault = true;                                                       \
                return 0;                                                                    \
            }                                                                                \
        } while (0)

    DBG_ASSERT((cr0 & CR0_PG) != 0); /* long mode implies paging */
    DBG_ASSERT((cr4 & CR4_PAE) != 0); /* ... and PAE: CR4.PAE is the IA-32e entry condition */

    /* LEVEL 4 -- the PML4, whose base is CR3 */
    uint32_t pml4_addr = ((uint32_t)cpu->cr[3] & 0xFFFFF000u) +
                         (uint32_t)(((addr >> 39) & 0x1FFu) * 8u);
    WALK64_TABLE(pml4_addr);
    uint64_t pml4e = (uint64_t)read64s(pml4_addr);
    if ((pml4e & PAGE_TABLE_PML4E_PRESENT) == 0) {
        if (side_effects) trigger_pagefault(addr, false, for_writing, user, is_instruction_fetch, false);
        cpu->pagefault = true;
        return 0;
    }
    if ((pml4e & reserved_mask) != 0) {
        if (side_effects) trigger_pagefault(addr, true, for_writing, user, is_instruction_fetch, true);
        cpu->pagefault = true;
        return 0;
    }

    WALK64_BOUND(pml4e, "the PML4 entry's PDPT base");

    /* LEVEL 3 -- the PDPT */
    uint32_t pdpt_addr = ((uint32_t)pml4e & 0xFFFFF000u) +
                         (uint32_t)(((addr >> 30) & 0x1FFu) * 8u);
    WALK64_TABLE(pdpt_addr);
    uint64_t pdpte = (uint64_t)read64s(pdpt_addr);
    if ((pdpte & PAGE_TABLE_PRESENT_MASK) == 0) {
        if (side_effects) trigger_pagefault(addr, false, for_writing, user, is_instruction_fetch, false);
        cpu->pagefault = true;
        return 0;
    }
    if ((pdpte & reserved_mask) != 0) {
        if (side_effects) trigger_pagefault(addr, true, for_writing, user, is_instruction_fetch, true);
        cpu->pagefault = true;
        return 0;
    }
    if ((pdpte & PAGE_TABLE_PSE_MASK) != 0) {
        unimplemented_instruction(
            "page walk: a 1 GiB page (PDPTE.PS) in long mode is S11's (plan ss6 S5 says"
            " 4 KiB pages only)");
        cpu->pagefault = true;
        return 0;
    }
    no_exec = (pdpte & PAGE_TABLE_NX_MASK) != 0;

    WALK64_BOUND(pdpte, "the PDPTE's PD base");

    /* LEVEL 2 -- the PD */
    uint32_t pd_addr = ((uint32_t)pdpte & 0xFFFFF000u) +
                       (uint32_t)(((addr >> 21) & 0x1FFu) * 8u);
    WALK64_TABLE(pd_addr);
    uint64_t pde = (uint64_t)read64s(pd_addr);
    if ((pde & PAGE_TABLE_PRESENT_MASK) == 0) {
        if (side_effects) trigger_pagefault(addr, false, for_writing, user, is_instruction_fetch, false);
        cpu->pagefault = true;
        return 0;
    }
    if ((pde & reserved_mask) != 0) {
        if (side_effects) trigger_pagefault(addr, true, for_writing, user, is_instruction_fetch, true);
        cpu->pagefault = true;
        return 0;
    }
    if ((pde & PAGE_TABLE_PSE_MASK) != 0) {
        unimplemented_instruction(
            "page walk: a 2 MiB page (PDE.PS) in long mode is S11's (plan ss6 S5 says"
            " 4 KiB pages only)");
        cpu->pagefault = true;
        return 0;
    }
    no_exec |= (pde & PAGE_TABLE_NX_MASK) != 0;
    global = (pde & PAGE_TABLE_GLOBAL_MASK) != 0;
    allow_write = (pde & PAGE_TABLE_RW_MASK) != 0;
    allow_user = allow_user && (pde & PAGE_TABLE_USER_MASK) != 0;

    WALK64_BOUND(pde, "the PDE's PT base");

    /* LEVEL 1 -- the PT */
    uint32_t pt_addr = ((uint32_t)pde & 0xFFFFF000u) +
                       (uint32_t)(((addr >> 12) & 0x1FFu) * 8u);
    WALK64_TABLE(pt_addr);
    uint64_t pte_64 = (uint64_t)read64s(pt_addr);
    int32_t pte = (int32_t)pte_64;
    bool present = (pte & PAGE_TABLE_PRESENT_MASK) != 0;
    no_exec |= (pte_64 & PAGE_TABLE_NX_MASK) != 0;
    global = (pte & PAGE_TABLE_GLOBAL_MASK) != 0;
    allow_write = allow_write && (pte & PAGE_TABLE_RW_MASK) != 0;
    allow_user = allow_user && (pte & PAGE_TABLE_USER_MASK) != 0;

    if ((pte_64 & reserved_mask) != 0) {
        if (side_effects) trigger_pagefault(addr, true, for_writing, user, is_instruction_fetch, true);
        cpu->pagefault = true;
        return 0;
    }
    /* the frame's own bound: a PTE at or above 4 GiB is a legal mapping on hardware and an
     * unrepresentable one here, so the machine stops BY NAME -- truncating it would translate
     * the guest's page to a different, mapped page, silently (plan ss5.1). */
    WALK64_BOUND(pte_64, "the page frame");

    bool kernel_write_override = !user && (cr0 & CR0_WP) == 0;
    if (!present || (for_writing && !allow_write && !kernel_write_override) ||
        (user && !allow_user) || (is_instruction_fetch && no_exec)) {
        if (side_effects) trigger_pagefault(addr, present, for_writing, user, is_instruction_fetch, false);
        cpu->pagefault = true;
        return 0;
    }

    /* THE ACCESSED/DIRTY UPDATE, on the same two entries the reference updates and in the
     * same shape (paging.c:274-285 above): the PDE gets A, the PTE gets A|D, and the store is
     * a BYTE store of the entry's low byte -- which is where A and D live (bits 5 and 6).
     * That is upstream's own idiom and it is kept verbatim rather than "cleaned up", because
     * the uop cache's invalidation hook and the dirty-page bitmap both ride these stores. */
    if (side_effects) {
        uint64_t new_pde = pde | PAGE_TABLE_ACCESSED_MASK;
        if (new_pde != pde) write8(pd_addr, (int32_t)(uint32_t)(new_pde & 0xFFu));
        uint64_t new_pte = pte_64 | PAGE_TABLE_ACCESSED_MASK |
                          (for_writing ? PAGE_TABLE_DIRTY_MASK : 0u);
        if (new_pte != pte_64) write8(pt_addr, (int32_t)(uint32_t)(new_pte & 0xFFu));
    }

    high = (uint32_t)pte & 0xFFFFF000u;

    int32_t info_bits = TLB_VALID | (for_writing ? 0 : TLB_READONLY) |
                        (allow_user ? 0 : TLB_NO_USER) |
                        (in_mapped_range(high) ? TLB_IN_MAPPED_RANGE : 0) |
                        ((global && (cr4 & CR4_PGE) != 0) ? TLB_GLOBAL : 0) |
                        (no_exec ? TLB_NO_EXEC : 0);

    return (int32_t)(high | (uint32_t)info_bits);
}

/* ------------------------------------------------------------------ *
 * translate_address -- cpu.rs:1955-2050
 * ------------------------------------------------------------------ */

uint32_t translate_address(uint64_t address,
                           bool for_writing,
                           bool user,
                           bool side_effects,
                           bool is_instruction_fetch)
{
    /* THE ONE PROJECTION POINT (paging.h; plan ss4's "the fetch machinery is already
     * page-relative" generalises because of this and only this).  Outside long mode a
     * linear address IS 32 bits, so `(uint32_t)` is the model's own width and not a
     * truncation of a live address; it is also upstream's wraparound, exactly -- the low 32
     * bits of a 64-bit sum are the sum mod 2^32, which is what the i32 field computes.  A
     * 32-bit-mode address with a non-zero high half is a WIDENING LEAK, and cpu_state.h's
     * cpu_assert_address_widening names the field that produced it at the batch boundary.
     * The 4-level walk that makes the high half mean something is S11's. */
    /* THE LONG-MODE STOP, and the guard the projection below actually needs.  S3's other
     * instrument -- cpu_address_widening_leak() -- answers NULL when EFER.LMA is set, on
     * purpose (in IA-32e the high half IS the address), so the mode-boundary invariant does
     * NOT protect this cast in long mode: paging.h's signature note used to say it did, and
     * that was wrong.  Until S11 lands the 4-level walk, a long-mode translation stops BY
     * NAME instead of projecting, because returning the low 32 bits of a 64-bit linear
     * address would be a silent wrong page -- the one outcome this tree refuses.  The abort
     * flag is set as well, get_seg's precedent (modrm.c): the flag is this port's abort
     * channel even when the cause is not a page fault, so the instruction does not act on
     * the 0 returned here.
     *
     * UNREACHABLE TODAY, and shown so rather than assumed: S2 refuses the three writes that
     * could enter IA-32e, and nothing sets LMA (cpu_selftest's S2 arm puts the state there by
     * hand to exercise cpu_mode_of's arm). */
    /* S5a: THE LONG-MODE ARM -- the 4-level walk of plan ss6 S5, through the 64-bit-keyed
     * TLB.  Before this slice the same condition STOPPED BY NAME ("long mode needs the
     * 4-level walk"); the stop's own note said the projection below would be a silent wrong
     * page in long mode, which is why the stop and this arm are the same `if`. */
    if ((cpu->efer & EFER_LMA) != 0) {
        uint64_t vpage = address >> 12;
        uint32_t slot = tlb64_slot(vpage);
        int32_t entry64 = tlb64_data[slot];

        if (tlb64_tag[slot] != vpage ||
            (entry64 & (TLB_VALID | (user ? TLB_NO_USER : 0) |
                        (for_writing ? TLB_READONLY : 0) |
                        (is_instruction_fetch ? TLB_NO_EXEC : 0))) != TLB_VALID) {
            entry64 = do_page_walk64(address, for_writing, user, side_effects,
                                     is_instruction_fetch);
            if (cpu->pagefault) return 0;
            /* the physical frame is 32 bits by the walk's own bound check (ss5.1), so the
             * 64-bit linear address never reaches the mem8 index -- the address that does is
             * the projection of the MODEL's width, not of the guest's. */
            tlb64_tag[slot] = vpage;
            tlb64_data[slot] = entry64;
        }
        else if (side_effects && (entry64 & TLB_GLOBAL) == 0) {
            /* a hit: nothing to do, the A/D bits were updated by the walk */
        }

        return (uint32_t)(entry64 & 0xFFFFF000u) | (uint32_t)(address & 0xFFFu);
    }

    uint32_t va = (uint32_t)address;

    int32_t entry = tlb_data[va >> 12];
    if ((entry & (TLB_VALID | (user ? TLB_NO_USER : 0) | (for_writing ? TLB_READONLY : 0) |
                  (is_instruction_fetch ? TLB_NO_EXEC : 0))) != TLB_VALID) {
        entry = do_page_walk((int32_t)va, for_writing, user, side_effects, is_instruction_fetch);
        if (cpu->pagefault) {
            return 0;
        }
    }
    /* upstream: `(entry & !0xFFF ^ address) as u32 - memory::mem8 as u32`.  The XOR
     * restores high|offset; the mem8 subtraction is the wasm host-address trick and
     * is dropped (the result is the guest physical address). */
    return ((uint32_t)entry & ~0xFFFu) ^ va;
}

uint32_t translate_address_read_no_side_effects(uint64_t address)
{
    return translate_address(address, false, cpu->cpl == 3, false, false);
}

uint32_t translate_address_read(uint64_t address)
{
    return translate_address(address, false, cpu->cpl == 3, true, false);
}

uint32_t translate_address_fetch(uint64_t address)
{
    return translate_address(address, false, cpu->cpl == 3, true, true);
}

uint32_t translate_address_write(uint64_t address)
{
    return translate_address(address, true, cpu->cpl == 3, true, false);
}

uint32_t translate_address_system_read(uint64_t address)
{
    return translate_address(address, false, false, true, false);
}

uint32_t translate_address_system_write(uint64_t address)
{
    return translate_address(address, true, false, true, false);
}

uint32_t translate_address_write_and_can_skip_dirty(uint64_t address)
{
    /* S5a: the long-mode arm, TWINNED with translate_address's.  This is the half the write
     * paths use (paging.h), and a guard on only one of the two would leave every 64-bit store
     * projecting a 64-bit linear address into a 32-bit one -- which was the S3 note here
     * before the arm existed.  Delegating keeps ONE 4-level walk: the dirty-bit-skip variant
     * is an optimisation of the same translation, and S5a has no measurement that would
     * justify a second copy of it. */
    bool user = cpu->cpl == 3;
    if ((cpu->efer & EFER_LMA) != 0) {
        return translate_address(address, true, user, true, false);
    }

    uint32_t va = (uint32_t)address; /* the projection: as in translate_address */
    int32_t entry = tlb_data[va >> 12];
    if ((entry & (TLB_VALID | (user ? TLB_NO_USER : 0) | TLB_READONLY)) != TLB_VALID) {
        entry = do_page_walk((int32_t)va, true, user, true, false);
        if (cpu->pagefault) {
            return 0;
        }
    }
    return ((uint32_t)entry & ~0xFFFu) ^ va;
}

/* ------------------------------------------------------------------ *
 * readable/writable_or_pagefault -- cpu.rs:1921-1953
 * ------------------------------------------------------------------ */

void readable_or_pagefault(uint64_t addr, int32_t size)
{
    DBG_ASSERT(size < 0x1000);
    DBG_ASSERT(size > 0);

    bool user = cpu->cpl == 3;
    translate_address(addr, false, user, true, false);
    if (cpu->pagefault) {
        return;
    }

    /* S3: the page test is on the address's own 64-bit value; a crossing test is a
     * comparison, not an address, so nothing is projected here.  Outside long mode the two
     * agree with upstream's i32 test for every address the machine can form. */
    uint64_t end = (addr + (uint64_t)size - 1) & ~(uint64_t)0xFFF;
    if ((addr & ~(uint64_t)0xFFF) != end) {
        translate_address(end, false, user, true, false);
    }
}

void writable_or_pagefault(uint64_t addr, int32_t size)
{
    writable_or_pagefault_cpl(cpu->cpl, addr, size);
}

void writable_or_pagefault_cpl(uint8_t other_cpl, uint64_t addr, int32_t size)
{
    DBG_ASSERT(size < 0x1000);
    DBG_ASSERT(size > 0);

    bool user = other_cpl == 3;
    translate_address(addr, true, user, true, false);
    if (cpu->pagefault) {
        return;
    }

    uint64_t end = (addr + (uint64_t)size - 1) & ~(uint64_t)0xFFF;
    if ((addr & ~(uint64_t)0xFFF) != end) {
        translate_address(end, true, user, true, false);
    }
}

/* ------------------------------------------------------------------ *
 * control registers -- cpu.rs:2931-2997
 * ------------------------------------------------------------------ */

void set_cr0(int32_t cr0)
{
    int32_t old_cr0 = cpu->cr[0];

    if ((old_cr0 & CR0_AM) == 0 && (cr0 & CR0_AM) != 0) {
        /* upstream dbg_log!("Warning: Unimplemented: cr0 alignment mask") -- the C
         * port has no log channel; the mask is still accepted, as upstream does. */
    }
    if ((cr0 & (CR0_PE | CR0_PG)) == CR0_PG) {
        fatal("cannot load PG without PE");
    }

    /* S2 (plan ss3.5.2, C5): THE PG TRANSITION.  This is the SDM's trigger for entering
     * IA-32e mode -- CR0.PG set while EFER.LME and CR4.PAE are -- and before S2 a guest
     * that arranged those three bits got load_pdpte() below reading a PML4 as a PDPT and a
     * 3-level walk over 4-level tables, with nothing in the machine saying so.  Refused BY
     * NAME, and BEFORE the load_pdpte validation below (which would otherwise be the first
     * wrong-table read), with CR0 left exactly as it was: a refused write commits nothing. */
    if ((cr0 & CR0_PG) != 0 && (old_cr0 & CR0_PG) == 0 &&
        (cpu->efer & EFER_LME) != 0 && (cpu->cr[4] & CR4_PAE) != 0 &&
        cpu_backend_refuse_long_mode("CR0.PG")) {
        return;
    }

    /* S5a: THE ENTRY ITSELF, and the exit.  The refusal above is the arm for a backend that
     * cannot run IA-32e; when the selected backend CAN (S5a's `--long-mode`), the same edge
     * must SET EFER.LMA -- otherwise the guest has asked for IA-32e, been allowed, and the
     * machine silently stays 32-bit, which is C5's defect wearing the opposite sign.
     *
     * WHAT LMA DOES *NOT* DO ON THIS EDGE: it does not make the machine 64-bit.  Per the SDM
     * the machine enters IA-32e in COMPATIBILITY mode, because CS.L is still whatever the
     * current code segment's descriptor says (here: 0, the stub's 32-bit CS), and it is the
     * subsequent far jump to a 64-bit descriptor that sets CS.L and puts the CPU in 64-bit
     * mode.  cpu->is_64 is that bit (cpu_state.h), and cpu_mode_of reports LONG or COMPAT
     * from it -- which is plan correction GAP 2 in results/x86-64-s5.md.
     *
     * The 1->0 edge clears LMA (SDM: leaving IA-32e by clearing CR0.PG) and leaves LME
     * alone, so re-enabling paging re-enters with the same request.  Nothing here runs
     * unless the machine already has LME, so no 32-bit path's state moves. */
    bool lme_enter = (cr0 & CR0_PG) != 0 && (old_cr0 & CR0_PG) == 0 &&
                     (cpu->efer & EFER_LME) != 0 && (cpu->cr[4] & CR4_PAE) != 0;
    bool pg_leave = (cr0 & CR0_PG) == 0 && (old_cr0 & CR0_PG) != 0;

    /* validation BEFORE commit: a #GP from load_pdpte must leave CR0 (and the TLB)
     * untouched -- the guest's #GP handler must see the register state hardware would
     * (MOV CR0 never re-reads PDPTEs on hardware; this is the emulator's refresh of
     * the reg_pdpte cache, and #GP is the safest defined behaviour when it faults). */
    if (!lme_enter && (cpu->cr[4] & CR4_PAE) != 0 && (cr0 & CR0_PG) != 0 &&
        (old_cr0 & (CR0_CD | CR0_NW | CR0_PG)) != (cr0 & (CR0_CD | CR0_NW | CR0_PG))) {
        /* S5a: `!lme_enter` -- on the IA-32e ENTRY edge there is no PDPT to load (CR3 is a
         * PML4 from that instant), and reading one would #GP on tables that are perfectly
         * valid as 4-level tables. */
        if (!load_pdpte(cpu->cr[3])) {
            return;
        }
    }

    cpu->cr[0] = cr0;
    cpu->cr[0] |= CR0_ET;

    if ((old_cr0 & (CR0_PG | CR0_WP)) != (cr0 & (CR0_PG | CR0_WP))) {
        full_clear_tlb();
    }

    cpu->protected_mode = (cpu->cr[0] & CR0_PE) == CR0_PE;
    cpu->segment_access_bytes[CS] = 0x80 | 0x10 | 0x08 | 0x02; /* P dpl0 S E RW */

    /* S5a: the two edges, AFTER the commit (so a write that #GP'd above committed neither
     * CR0 nor LMA) and BEFORE the boundary check below (which is a no-op in long mode and
     * must not see a half-landed state). */
    if (lme_enter && !cpu_backend_refuse_long_mode("CR0.PG")) {
        cpu->efer |= EFER_LMA;
        /* The 4-level walk reads CR3 as a PML4, not as a PDPT: the reg_pdpte cache the PAE
         * path keeps is meaningless under EFER.LMA, so it is invalidated rather than left
         * holding a PDPT read out of what is now a PML4 (the exact confusion C5 was). */
        for (int i = 0; i < 4; i++) cpu->reg_pdpte[i] = 0;
        full_clear_tlb();
    }
    if (pg_leave && (cpu->efer & EFER_LMA) != 0) {
        cpu->efer &= ~EFER_LMA;
        cpu->is_64 = false;
        full_clear_tlb();
    }

    /* S3 MODE BOUNDARY: CR0's write is the first of the three transitions that can change
     * the mode (the SDM's IA-32e entry is CR0.PG set while EFER.LME && CR4.PAE, which is the
     * transition S2 refuses by name above; a machine that CAN enter it -- S5 -- must have a
     * clean 64-bit address state at the moment it does, or the widened fields carry a
     * 32-bit-mode high half into the long-mode walk).  Checked AFTER the commit, on the state
     * the write produced.  MEASURED absent until this round: the comment in cpu_state.h
     * claimed these three call sites and only reset/batch had them -- a seat described but
     * never called. */
    cpu_assert_address_widening("set_cr0");
}

void set_cr3(int32_t cr3)
{
    if ((cpu->cr[4] & CR4_PAE) != 0 && (cpu->efer & EFER_LMA) == 0) {
        cr3 &= ~0xF;
        if ((cpu->cr[0] & CR0_PG) != 0 && !load_pdpte(cr3)) {
            return; /* #GP delivered: CR3 stays unwritten and the TLB is not cleared */
        }
    }
    else if ((cpu->cr[4] & CR4_PAE) != 0) {
        /* S5a: UNDER EFER.LMA CR3 IS A PML4, NOT A PDPT.  read_pdpt would interpret the
         * PML4's first four entries as the PDPTE cache -- the very confusion C5 was, one
         * level down -- so it is skipped entirely: the 4-level walk
         * (do_page_walk64) reads the PML4 from cr[3] on every miss, and reg_pdpte holds
         * nothing it reads.  CR3's low bits are masked per the SDM (PWT/PCD kept). */
        cr3 &= ~0x1F;
        for (int i = 0; i < 4; i++) cpu->reg_pdpte[i] = 0;
    }
    else {
        cr3 &= ~0xFE7; /* upstream !0b111111100111: keep PWT/PCD (bits 3/4 -- real
                         * non-PAE CR3 bits) and clear bits 0-2 and 5-11.  The old
                         * mask ~0x7E7 also left bit 11, diverging from upstream
                         * (and hardware ignores it): aligned to ~0xFE7. */
        /* upstream dbg_assert!(cr3 & 0xFFF == 0, "TODO") -- DROPPED, not reworded:
         * it is guest-reachable here (a guest MOV CR3 with PWT/PCD set is legal
         * hardware input and the mask deliberately preserves those bits), and in
         * upstream it is debug_assert! (dbg.rs:63), compiled out of the shipped
         * wasm -- this port runs with asserts live (no -DNDEBUG, Makefile).  The
         * consequence, upstream-faithful NOT hardware-faithful: a PWT/PCD-set CR3
         * flows into the non-PAE walk (paging.c:181) and offsets the PD base by
         * 8/16, exactly the reference's release behaviour. */
    }
    cpu->cr[3] = cr3;
    clear_tlb();
}

/* Read the PDPT at cr3 and run the two guest-reachable checks, WITHOUT committing
 * reg_pdpte.  This is the shared body: load_pdpte (below) calls it and then installs
 * the cache; validate_pdpte calls it and discards the entries.  The mask is HERE,
 * not at each call site: set_cr0 (above) and the CR4 write (instructions_0f.c) pass
 * cpu->cr[3] unmasked, and a CR3 written in non-PAE mode legally carries PWT/PCD
 * (bits 3/4).  Masking makes upstream's dbg_assert premise hold by construction
 * instead of by caller discipline. */
static bool read_pdpt(int32_t cr3, uint64_t out[4])
{
    cr3 &= ~0xF;
    DBG_ASSERT((cr3 & 0xF) == 0); /* upstream dbg_assert!(cr3 & 0b1111 == 0) */
    for (int32_t i = 0; i < 4; i++) {
        uint64_t pdpt_entry = (uint64_t)read64s((uint32_t)cr3 + 8u * (uint32_t)i);
        pdpt_entry &= ~0xE00ull; /* bits 9-11 are ignored */
        pdpt_entry &= ~0x18ull;  /* bits 3 (PWT) and 4 (PCD) are valid; ignored (no
                                  * cache emulation) */
        /* upstream's two dbg_asserts (cpu.rs:2984, :2989) are GUEST-REACHABLE and
         * became #GP(0) (plan Q1) -- a guest that points CR3 at a bad PDPT gets a
         * real fault, not an abort. */
        if ((pdpt_entry & 0xFFFFFFFF00000000ull) != 0) {
            trigger_gp(0);
            return false;
        }
        if (((int32_t)pdpt_entry & PAGE_TABLE_PRESENT_MASK) != 0 &&
            (pdpt_entry & 0x1E6ull) != 0) {
            trigger_gp(0);
            return false;
        }
        out[i] = pdpt_entry;
    }
    return true;
}

bool load_pdpte(int32_t cr3)
{
    /* validate into a local first: a #GP at index i>=1 must not leave entries
     * 0..i-1 refreshed from the new PDPT while CR3/CR0/CR4 stay uncommitted (the
     * guest's #GP handler would translate through PDPTEs of neither committed
     * state; hardware leaves the cache untouched on a faulting MOV CR3). */
    uint64_t tmp[4];
    if (!read_pdpt(cr3, tmp)) {
        return false;
    }
    for (int32_t i = 0; i < 4; i++) {
        cpu->reg_pdpte[i] = tmp[i];
    }
    return true;
}

bool validate_pdpte(int32_t cr3)
{
    /* the task switch's validate-before-commit arm: it must #GP before any register
     * commit (a faulting task switch leaves the old task intact), but it must NOT
     * install the new PDPTE cache yet -- the new TSS is read under the OLD page
     * tables (hardware reads the TSS before loading CR3), and reg_pdpte is exactly
     * what switches translation.  load_pdpte (via set_cr3) re-runs this and commits
     * at the end of the switch. */
    uint64_t tmp[4];
    return read_pdpt(cr3, tmp);
}

/* ------------------------------------------------------------------ *
 * virt_boundary_* -- cpu.rs:3477-3530
 *
 * Only the write halves are M1a's: safe_write* call them.  The read halves belong to
 * read_imm16/read_imm32s, which are the fetch path of a later unit.
 * ------------------------------------------------------------------ */

int32_t virt_boundary_read16(uint32_t low, uint32_t high)
{
    DBG_ASSERT((low & 0xFFF) == 0xFFF);
    DBG_ASSERT((high & 0xFFF) == 0);
    return read8(low) | read8(high) << 8;
}

int32_t virt_boundary_read32s(uint32_t low, uint32_t high)
{
    DBG_ASSERT((low & 0xFFF) >= 0xFFD);
    DBG_ASSERT(((high - 3) & 0xFFF) == (low & 0xFFF));
    int32_t mid;
    if ((low & 1) != 0) {
        if ((low & 2) != 0) {
            /* 0xFFF */
            mid = read16(high - 2);
        }
        else {
            /* 0xFFD */
            mid = read16(low + 1);
        }
    }
    else {
        /* 0xFFE */
        mid = virt_boundary_read16(low + 1, high - 1);
    }
    return read8(low) | mid << 8 | read8(high) << 24;
}

void virt_boundary_write16(uint32_t low, uint32_t high, int32_t value)
{
    DBG_ASSERT((low & 0xFFF) == 0xFFF);
    DBG_ASSERT((high & 0xFFF) == 0);
    write8(low, value);
    write8(high, value >> 8);
}

void virt_boundary_write32(uint32_t low, uint32_t high, int32_t value)
{
    DBG_ASSERT((low & 0xFFF) >= 0xFFD);
    DBG_ASSERT(((high - 3) & 0xFFF) == (low & 0xFFF));
    write8(low, value);
    if ((low & 1) != 0) {
        if ((low & 2) != 0) {
            /* 0xFFF */
            write8(high - 2, value >> 8);
            write8(high - 1, value >> 16);
        }
        else {
            /* 0xFFD */
            write8(low + 1, value >> 8);
            write8(low + 2, value >> 16);
        }
    }
    else {
        /* 0xFFE */
        write8(low + 1, value >> 8);
        write8(high - 1, value >> 16);
    }
    write8(high, value >> 24);
}

/* ------------------------------------------------------------------ *
 * safe reads -- cpu.rs:3538-3585
 * ------------------------------------------------------------------ */

int32_t safe_read8(int32_t addr)
{
    uint32_t p = translate_address_read(addr);
    if (cpu->pagefault) {
        return 0;
    }
    return read8(p);
}

int32_t safe_read16(int32_t addr)
{
    if ((addr & 0xFFF) == 0xFFF) {
        int32_t low = safe_read8(addr);
        if (cpu->pagefault) {
            return 0;
        }
        int32_t high = safe_read8(addr + 1);
        if (cpu->pagefault) {
            return 0;
        }
        return low | high << 8;
    }
    else {
        uint32_t p = translate_address_read(addr);
        if (cpu->pagefault) {
            return 0;
        }
        return read16(p);
    }
}

int32_t safe_read32s(int32_t addr)
{
    if ((addr & 0xFFF) >= 0xFFD) {
        int32_t low = safe_read16(addr);
        if (cpu->pagefault) {
            return 0;
        }
        int32_t high = safe_read16(addr + 2);
        if (cpu->pagefault) {
            return 0;
        }
        return low | high << 16;
    }
    else {
        uint32_t p = translate_address_read(addr);
        if (cpu->pagefault) {
            return 0;
        }
        return read32s(p);
    }
}

float safe_read_f32(int32_t addr)
{
    /* upstream f32::from_bits(i32::cast_unsigned(safe_read32s(addr)?)) */
    int32_t v = safe_read32s(addr);
    if (cpu->pagefault) {
        return 0.0f;
    }
    union {
        uint32_t u;
        float f;
    } bits;
    bits.u = (uint32_t)v;
    return bits.f;
}

uint64_t safe_read64s(int32_t addr)
{
    if ((addr & 0xFFF) > 0x1000 - 8) {
        int32_t low = safe_read32s(addr);
        if (cpu->pagefault) {
            return 0;
        }
        int32_t high = safe_read32s(addr + 4);
        if (cpu->pagefault) {
            return 0;
        }
        /* upstream zero-extends both halves here (unlike memory::read64s) */
        return (uint64_t)(uint32_t)low | ((uint64_t)(uint32_t)high << 32);
    }
    else {
        uint32_t p = translate_address_read(addr);
        if (cpu->pagefault) {
            return 0;
        }
        return (uint64_t)read64s(p);
    }
}

reg128 safe_read128s(int32_t addr)
{
    reg128 z;
    if ((addr & 0xFFF) > 0x1000 - 16) {
        uint64_t lo = safe_read64s(addr);
        if (cpu->pagefault) {
            return z;
        }
        uint64_t hi = safe_read64s(addr + 8);
        if (cpu->pagefault) {
            return z;
        }
        z.u64[0] = lo;
        z.u64[1] = hi;
        return z;
    }
    else {
        uint32_t p = translate_address_read(addr);
        if (cpu->pagefault) {
            return z;
        }
        return read128(p);
    }
}

/* ------------------------------------------------------------------ *
 * safe writes -- cpu.rs:3941-4040
 *
 * Upstream's `can_skip_dirty_page` bool only gates jit::jit_dirty_page, so the three
 * write paths collapse to the two below (see translate_address_write_and_can_skip_dirty).
 * ------------------------------------------------------------------ */

void safe_write8(int32_t addr, int32_t value)
{
    uint32_t phys_addr = translate_address_write_and_can_skip_dirty(addr);
    if (cpu->pagefault) {
        return;
    }
    if (in_mapped_range(phys_addr)) {
        mmap_write8(phys_addr, value);
    }
    else {
        /* upstream's `if !can_skip_dirty_page { jit::jit_dirty_page(..) }`
         * (cpu.rs:3941-3956): the port collapsed the can_skip_dirty split when it dropped the
         * JIT (this file's own comment above), so the hook is UNCONDITIONAL here -- and that
         * is the safe direction: can_skip_dirty_page is the JIT's optimisation for a page it
         * knows holds no code, which is exactly the test uop_invalidate_page_fast makes
         * anyway (PERF-4/M2, plan ss4.1 row 4). */
        uop_invalidate_page_fast(phys_addr);
        write8_no_mmap_or_dirty_check(phys_addr, value);
    }
}

void safe_write16(int32_t addr, int32_t value)
{
    uint32_t phys_addr = translate_address_write_and_can_skip_dirty(addr);
    if (cpu->pagefault) {
        return;
    }
    DBG_ASSERT(value >= 0 && value < 0x10000);
    if ((addr & 0xFFF) == 0xFFF) {
        uint32_t high = translate_address_write(addr + 1);
        if (cpu->pagefault) {
            return;
        }
        virt_boundary_write16(phys_addr, high, value);
    }
    else if (in_mapped_range(phys_addr)) {
        mmap_write16(phys_addr, value);
    }
    else {
        /* the straddle arm above went through virt_boundary_write16 -> write8 -> its own
         * hook, so this arm only has to cover this page (plan ss4.1 row 5) */
        uop_invalidate_page_fast(phys_addr);
        write16_no_mmap_or_dirty_check(phys_addr, value);
    }
}

void safe_write32(int32_t addr, int32_t value)
{
    uint32_t phys_addr = translate_address_write_and_can_skip_dirty(addr);
    if (cpu->pagefault) {
        return;
    }
    if ((addr & 0xFFF) > 0x1000 - 4) {
        uint32_t high = translate_address_write((addr + 3) & ~3);
        if (cpu->pagefault) {
            return;
        }
        virt_boundary_write32(phys_addr, high | ((uint32_t)(addr + 3) & 3), value);
    }
    else if (in_mapped_range(phys_addr)) {
        mmap_write32(phys_addr, value);
    }
    else {
        uop_invalidate_page_fast(phys_addr); /* plan ss4.1 row 5 */
        write32_no_mmap_or_dirty_check(phys_addr, value);
    }
}

void safe_write64(int32_t addr, uint64_t value)
{
    if ((addr & 0xFFF) > 0x1000 - 8) {
        writable_or_pagefault(addr, 8);
        if (cpu->pagefault) {
            return;
        }
        /* upstream .unwrap()s these: the straddle is already known writable, so an
         * Err here is a bug.  The C port keeps the check so a bug returns instead of
         * writing through a fault. */
        safe_write32(addr, (int32_t)value);
        if (cpu->pagefault) {
            return;
        }
        safe_write32(addr + 4, (int32_t)(value >> 32));
    }
    else {
        uint32_t phys_addr = translate_address_write_and_can_skip_dirty(addr);
        if (cpu->pagefault) {
            return;
        }
        if (in_mapped_range(phys_addr)) {
            mmap_write64(phys_addr, value);
        }
        else {
            uop_invalidate_page_fast(phys_addr); /* plan ss4.1 row 5's 64-bit arm */
            write64_no_mmap_or_dirty_check(phys_addr, value);
        }
    }
}

void safe_write128(int32_t addr, reg128 value)
{
    if ((addr & 0xFFF) > 0x1000 - 16) {
        writable_or_pagefault(addr, 16);
        if (cpu->pagefault) {
            return;
        }
        safe_write64(addr, value.u64[0]);
        if (cpu->pagefault) {
            return;
        }
        safe_write64(addr + 8, value.u64[1]);
    }
    else {
        uint32_t phys_addr = translate_address_write_and_can_skip_dirty(addr);
        if (cpu->pagefault) {
            return;
        }
        if (in_mapped_range(phys_addr)) {
            mmap_write128(phys_addr, value.u64[0], value.u64[1]);
        }
        else {
            uop_invalidate_page_fast(phys_addr); /* plan ss4.1 row 5's 128-bit arm; the
                                                  * straddle arm above goes through
                                                  * safe_write64 -> its own hook */
            write128_no_mmap_or_dirty_check(phys_addr, value);
        }
    }
}

/* ------------------------------------------------------------------ *
 * S5a: THE 64-BIT-ADDRESS ACCESSORS (paging.h)
 *
 * Deliberately thin: the translation is the SAME translate_address* the 32-bit forms call
 * (which dispatches on the mode itself), and the only new logic is the page-straddle split,
 * because a quadword can cross a page boundary and the two halves then translate separately.
 * Everything else -- the fault channel, the abort discipline, the mmio/device routing -- is
 * reached through the existing physical accessors, unchanged.
 * ------------------------------------------------------------------ */

int32_t safe_read8_addr(int64_t addr)
{
    uint32_t p = translate_address_read((uint64_t)addr);
    if (cpu->pagefault) return 0;
    return read8(p);
}

void safe_write8_addr(int64_t addr, int32_t value)
{
    uint32_t p = translate_address_write((uint64_t)addr);
    if (cpu->pagefault) return;
    write8(p, value);
}

static int32_t safe_read32_addr_local(int64_t addr)
{
    uint32_t p = translate_address_read((uint64_t)addr);
    if (cpu->pagefault) return 0;
    return read32s(p);
}

static void safe_write32_addr_local(int64_t addr, int32_t value)
{
    /* the write half translates through the write path, which is also where the uop cache's
     * dirty-page hook lives (write8/32 below reach it) */
    if (((uint64_t)addr & 0xFFF) > 0x1000 - 4) {
        uint32_t p = translate_address_write_and_can_skip_dirty((uint64_t)addr);
        if (cpu->pagefault) return;
        (void)p;
    }
    uint32_t p = translate_address_write((uint64_t)addr);
    if (cpu->pagefault) return;
    write32(p, value);
}

uint64_t safe_read64_addr(int64_t addr)
{
    /* a quadword whose page offset leaves fewer than eight bytes is split into two 32-bit
     * halves, each translated on its own page -- the reference's straddle rule, reused */
    if (((uint64_t)addr & 0xFFF) > 0x1000 - 8) {
        uint64_t low = (uint32_t)safe_read32_addr_local(addr);
        if (cpu->pagefault) return 0;
        uint64_t high = (uint32_t)safe_read32_addr_local(addr + 4);
        if (cpu->pagefault) return 0;
        return low | (high << 32);
    }
    uint32_t p = translate_address_read((uint64_t)addr);
    if (cpu->pagefault) return 0;
    /* inside one page: eight physical bytes, read as the machine reads them */
    uint64_t low = (uint32_t)read32s(p);
    uint64_t high = (uint32_t)read32s(p + 4);
    return low | (high << 32);
}

void safe_write64_addr(int64_t addr, uint64_t value)
{
    if (((uint64_t)addr & 0xFFF) > 0x1000 - 8) {
        safe_write32_addr_local(addr, (int32_t)value);
        if (cpu->pagefault) return;
        safe_write32_addr_local(addr + 4, (int32_t)(value >> 32));
        return;
    }
    uint32_t p = translate_address_write((uint64_t)addr);
    if (cpu->pagefault) return;
    write32(p, (int32_t)value);
    write32(p + 4, (int32_t)(value >> 32));
}
