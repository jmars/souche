/*
 * paging.c -- address translation, the TLB, the PSE-capable page walk, the
 * page-fault path and the safe_* accessors.
 *
 * Port of v86/src/rust/cpu/cpu.rs (see paging.h for the file:line map).  PAE is
 * DEFERRED (plan ss3): the state still carries reg_pdpte, and every path that would
 * have to interpret a 64-bit PAE entry traps loudly instead of misreading it.
 */

#include <stdio.h>
#include <stdlib.h>

#include "paging.h"
#include "uop_cache.h" /* PERF-4/M2: the write-path invalidation hook (plan ss4.1) */

int32_t tlb_data[0x100000];
int32_t valid_tlb_entries[VALID_TLB_ENTRY_MAX];
int32_t valid_tlb_entries_count = 0;

/* ------------------------------------------------------------------ *
 * loud unimplemented paths
 * ------------------------------------------------------------------ */

/* C stand-in for upstream's panic!/dbg_assert in these two spots.  An emulator that
 * silently misinterprets a 64-bit PAE entry would produce wrong guest results, which
 * is exactly the failure this port must not have; aborting names the gap instead. */
static void pae_unimplemented(void)
{
    fprintf(stderr,
            "v86c: FATAL: PAE paging (CR4.PAE / 64-bit page tables) is not implemented "
            "in M1 -- deferred to M2 (plan ss3). Refusing to misinterpret a 64-bit PTE.\n");
    abort();
}

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
    cpu->last_virt_eip = -1;
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
    cpu->last_virt_eip = -1;
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

void invlpg(int32_t addr)
{
    int32_t page = (int32_t)((uint32_t)addr >> 12);
    /* Note (cpu.rs:4469-4473): this does NOT remove the page from valid_tlb_entries.
     * It is not necessary, because a full list is emptied by clear_tlb, which drops
     * this entry too as it is not global.  It does mean valid_tlb_entries can hold
     * some already-invalid pages -- preserved here, because the bookkeeping is part of
     * what the plan ss3 asks to keep verbatim. */
    tlb_data[page] = 0;
    cpu->last_virt_eip = -1;
}

/* ------------------------------------------------------------------ *
 * page faults -- cpu.rs:2461-2512
 * ------------------------------------------------------------------ */

void trigger_pagefault(int32_t addr,
                       bool present,
                       bool write,
                       bool user,
                       bool is_instruction_fetch,
                       bool rsvd)
{
    /* config::LOG_PAGE_FAULTS + dbg_trace dropped (no log channel in M1a) */
    cpu->cr[2] = addr;
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
 * the PAE arm is replaced by an explicit trap (plan ss3).
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
    /* upstream's `nxe` is consumed only by the PAE arm (pae_reserved_mask), which is
     * deferred -- so it does not exist here */
    bool no_exec = false;

    if ((cr0 & CR0_PG) == 0) {
        /* paging disabled */
        high = (uint32_t)addr & 0xFFFFF000u;
        global = false;
    }
    else {
        if ((cr4 & CR4_PAE) != 0) {
            /* upstream walks the PDPTE here (cpu.rs:2087-2134); deferred, trapped */
            pae_unimplemented();
        }

        uint32_t page_dir_addr = (uint32_t)cpu->cr[3] + (((uint32_t)addr >> 22) << 2);
        int32_t page_dir_entry = read32s(page_dir_addr);

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

        if ((page_dir_entry & PAGE_TABLE_PSE_MASK) != 0 && (cr4 & CR4_PSE) != 0) {
            /* 4 MiB page (the `pae` word of upstream's condition is always false here) */
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

            high = ((uint32_t)page_dir_entry & 0xFFC00000u) | ((uint32_t)addr & 0x3FF000u);
            global = (page_dir_entry & PAGE_TABLE_GLOBAL_MASK) == PAGE_TABLE_GLOBAL_MASK;
        }
        else {
            uint32_t page_table_addr =
                ((uint32_t)page_dir_entry & 0xFFFFF000u) + ((((uint32_t)addr >> 12) & 0x3FFu) << 2);
            int32_t page_table_entry = read32s(page_table_addr);

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
    /* upstream: has_code = !is_in_mapped_range && jit::jit_page_has_code(...).  There
     * is no JIT, so there is no code page to track: false, and TLB_HAS_CODE is never
     * set.  (The `else { true }` arm of upstream's side_effects test exists only for
     * translate_address_read_no_side_effects, which discards this value anyway.) */
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
 * translate_address -- cpu.rs:1955-2050
 * ------------------------------------------------------------------ */

uint32_t translate_address(int32_t address,
                           bool for_writing,
                           bool user,
                           bool side_effects,
                           bool is_instruction_fetch)
{
    int32_t entry = tlb_data[(uint32_t)address >> 12];
    if ((entry & (TLB_VALID | (user ? TLB_NO_USER : 0) | (for_writing ? TLB_READONLY : 0) |
                  (is_instruction_fetch ? TLB_NO_EXEC : 0))) != TLB_VALID) {
        entry = do_page_walk(address, for_writing, user, side_effects, is_instruction_fetch);
        if (cpu->pagefault) {
            return 0;
        }
    }
    /* upstream: `(entry & !0xFFF ^ address) as u32 - memory::mem8 as u32`.  The XOR
     * restores high|offset; the mem8 subtraction is the wasm host-address trick and
     * is dropped (the result is the guest physical address). */
    return ((uint32_t)entry & ~0xFFFu) ^ (uint32_t)address;
}

uint32_t translate_address_read_no_side_effects(int32_t address)
{
    return translate_address(address, false, cpu->cpl == 3, false, false);
}

uint32_t translate_address_read(int32_t address)
{
    return translate_address(address, false, cpu->cpl == 3, true, false);
}

uint32_t translate_address_fetch(int32_t address)
{
    return translate_address(address, false, cpu->cpl == 3, true, true);
}

uint32_t translate_address_write(int32_t address)
{
    return translate_address(address, true, cpu->cpl == 3, true, false);
}

uint32_t translate_address_system_read(int32_t address)
{
    return translate_address(address, false, false, true, false);
}

uint32_t translate_address_system_write(int32_t address)
{
    return translate_address(address, true, false, true, false);
}

uint32_t translate_address_write_and_can_skip_dirty(int32_t address)
{
    int32_t entry = tlb_data[(uint32_t)address >> 12];
    bool user = cpu->cpl == 3;
    if ((entry & (TLB_VALID | (user ? TLB_NO_USER : 0) | TLB_READONLY)) != TLB_VALID) {
        entry = do_page_walk(address, true, user, true, false);
        if (cpu->pagefault) {
            return 0;
        }
    }
    return ((uint32_t)entry & ~0xFFFu) ^ (uint32_t)address;
}

/* ------------------------------------------------------------------ *
 * readable/writable_or_pagefault -- cpu.rs:1921-1953
 * ------------------------------------------------------------------ */

void readable_or_pagefault(int32_t addr, int32_t size)
{
    DBG_ASSERT(size < 0x1000);
    DBG_ASSERT(size > 0);

    bool user = cpu->cpl == 3;
    translate_address(addr, false, user, true, false);
    if (cpu->pagefault) {
        return;
    }

    int32_t end = (addr + size - 1) & ~0xFFF;
    if ((addr & ~0xFFF) != (end & ~0xFFF)) {
        translate_address(end, false, user, true, false);
    }
}

void writable_or_pagefault(int32_t addr, int32_t size)
{
    writable_or_pagefault_cpl(cpu->cpl, addr, size);
}

void writable_or_pagefault_cpl(uint8_t other_cpl, int32_t addr, int32_t size)
{
    DBG_ASSERT(size < 0x1000);
    DBG_ASSERT(size > 0);

    bool user = other_cpl == 3;
    translate_address(addr, true, user, true, false);
    if (cpu->pagefault) {
        return;
    }

    int32_t end = (addr + size - 1) & ~0xFFF;
    if ((addr & ~0xFFF) != (end & ~0xFFF)) {
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

    cpu->cr[0] = cr0;
    cpu->cr[0] |= CR0_ET;

    if ((old_cr0 & (CR0_PG | CR0_WP)) != (cr0 & (CR0_PG | CR0_WP))) {
        full_clear_tlb();
    }

    if ((cpu->cr[4] & CR4_PAE) != 0 && (cr0 & CR0_PG) != 0 &&
        (old_cr0 & (CR0_CD | CR0_NW | CR0_PG)) != (cr0 & (CR0_CD | CR0_NW | CR0_PG))) {
        load_pdpte(cpu->cr[3]);
    }

    cpu->protected_mode = (cpu->cr[0] & CR0_PE) == CR0_PE;
    cpu->segment_access_bytes[CS] = 0x80 | 0x10 | 0x08 | 0x02; /* P dpl0 S E RW */
}

void set_cr3(int32_t cr3)
{
    if ((cpu->cr[4] & CR4_PAE) != 0) {
        cr3 &= ~0xF;
        if ((cpu->cr[0] & CR0_PG) != 0) {
            load_pdpte(cr3);
        }
    }
    else {
        cr3 &= ~0x7E7;
        DBG_ASSERT((cr3 & 0xFFF) == 0); /* upstream dbg_assert!(..., "TODO") */
    }
    cpu->cr[3] = cr3;
    clear_tlb();
}

void load_pdpte(int32_t cr3)
{
    /* cpu.rs:2977-2997 fills reg_pdpte from the PDPT at cr3 and the PAE walk then
     * interprets 64-bit entries.  Neither is ported (plan ss3): this traps at the
     * first moment the emulator would be forced to misread a 64-bit PTE, rather than
     * silently later.  reg_pdpte remains in cpu_state_t (global_pointers.rs:61) and
     * stays 0. */
    (void)cr3;
    pae_unimplemented();
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
