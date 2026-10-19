/*
 * paging.h -- address translation, the TLB, the page-walk and safe_* access.
 *
 * Port of v86/src/rust/cpu/cpu.rs:1921-2385, 2461-2512, 2931-2997, 3477-3530,
 * 3538-3585, 3941-4040, 4467-4476.  Plan ss3 (DECISION 3).
 *
 * THE ONE INVARIANT THE PORT MUST NOT "IMPROVE" (plan ss3, VERIFIED upstream): v86
 * never tracks guest writes to page tables.  Correctness rests on the guest's own
 * TLB-shootdown discipline, exactly like real hardware.  TLB invalidation therefore
 * happens ONLY in invlpg, set_cr0, set_cr3 and the clear_tlb paths -- adding "smart"
 * write tracking would be new semantics, not a port.
 *
 * Two addressed divergences from the Rust:
 *   - upstream's `jit: bool` parameter and its jit_exit_reason branch
 *     (cpu.rs:2475-2509) are JIT-only and are DROPPED from translate_address/
 *     do_page_walk/trigger_pagefault: there is no JIT, so no caller could request the
 *     non-delivering form;
 *   - upstream's TLB entry bakes in the HOST address of mem8
 *     (`tlb_entry = (high + mem8) ^ page << 12 | bits`, cpu.rs:2323-2325) and
 *     translate_address subtracts mem8 back off.  That is a wasm-specific trick
 *     (32-bit linear memory); the C port stores the XOR of the GUEST physical page
 *     and returns the GUEST physical address, which is what indexes mem8 here.
 *     The information content is identical; the "offset" is simply not a thing in C.
 */

#ifndef V86C_PAGING_H
#define V86C_PAGING_H

#include "cpu_state.h"
#include "memory.h"
#include "uop_cache.h" /* PERF-FIX-3: the RMW macros below must invalidate (plan ss4.1) */

/* cpu.rs:329-332.  tlb_data is NOT part of cpu_state_t (plan ss2): 1M entries x 4
 * bytes = 4 MiB, held separately exactly as upstream does. */
extern int32_t tlb_data[0x100000];
extern int32_t valid_tlb_entries[VALID_TLB_ENTRY_MAX];
extern int32_t valid_tlb_entries_count;

/*
 * exceptions.c (M1c) owns the definition -- the IDT lookup and vector dispatch of
 * cpu.rs:735-830.  It is declared here because trigger_pagefault is its first caller.
 *
 * Upstream's error_code is an `Option<i32>` and the ONLY thing it controls is whether an
 * error code is pushed at all (`if let Some(ec) = error_code`, cpu.rs:1051/1889): the
 * software-interrupt paths (INT3 / INT n / INTO, cpu.rs's instruction callers) pass
 * `None`, the exception paths (trigger_pagefault here, and M1c's trigger_*) pass
 * `Some(code)`.  M1a-1 declared this as a bare `int32_t` with the comment "M1c may extend
 * it"; M1a-3's files are the first callers that need the ABSENT case, so the Option is
 * spelled faithfully now instead of being replaced by a sentinel that a real code could
 * collide with.  M1c must DEFINE this signature (not re-declare it differently).
 */
void call_interrupt_vector(int32_t interrupt, bool is_int, bool has_error_code, int32_t error_code);

/*
 * Every function below that can fault follows the plumbing contract in cpu_state.h:
 * on a fault it sets cpu->pagefault and returns the sentinel 0 (or a zeroed reg128);
 * the CALLER must check cpu->pagefault before using the result.
 */

/* S3 (results/x86-64-plan.md ss4, ss6): THE ADDRESS PARAMETERS ARE uint64_t and the
 * RESULT IS STILL uint32_t.  That asymmetry IS the model (plan ss5.1): the virtual address
 * is 64 bits wide in long mode, the physical space this port can represent is 32, and a
 * 4-level PTE that names a physical address above 4 GiB is S11's named refusal -- never a
 * silent truncation.  The projection happens ONCE, inside translate_address (paging.c), and
 * it is exactly upstream's i32 wrap for every value a 32-bit guest can form (the low 32 bits
 * of a sum are the wrapped sum).
 *
 * ITS GUARD IS THE LONG-MODE STOP AT THE TOP OF THE BODY, not the mode-boundary invariant
 * (round 2's N4; this comment previously said the invariant guarded it, and that was wrong:
 * cpu_address_widening_leak() answers NULL when EFER.LMA is set, by design, so in long mode
 * nothing was checking the projection at all).  Until S11 lands the 4-level walk, a long-mode
 * translation stops BY NAME -- and sets the abort flag, get_seg's precedent -- rather than
 * returning the low 32 bits of a 64-bit address.  Unreachable today: S2 refuses the three
 * writes that could enter IA-32e and nothing sets LMA. */
uint32_t translate_address(uint64_t address,
                           bool for_writing,
                           bool user,
                           bool side_effects,
                           bool is_instruction_fetch);

uint32_t translate_address_read_no_side_effects(uint64_t address);
uint32_t translate_address_read(uint64_t address);
uint32_t translate_address_fetch(uint64_t address);
uint32_t translate_address_write(uint64_t address);
uint32_t translate_address_system_read(uint64_t address);
uint32_t translate_address_system_write(uint64_t address);

/* Upstream returns (phys_addr, can_skip_dirty_page); the bool only gates
 * jit::jit_dirty_page, which this port does not have, so it is dropped. */
uint32_t translate_address_write_and_can_skip_dirty(uint64_t address);

void readable_or_pagefault(uint64_t addr, int32_t size);
void writable_or_pagefault(uint64_t addr, int32_t size);
void writable_or_pagefault_cpl(uint8_t other_cpl, uint64_t addr, int32_t size);

void full_clear_tlb(void);
void clear_tlb(void);
void invlpg(uint64_t addr);
void set_cr0(int32_t cr0);
void set_cr3(int32_t cr3);
/* cpu.rs:2977-2997.  Fills reg_pdpte from the PDPT at cr3; returns false after
 * delivering #GP(0) (the two guest-reachable dbg_asserts upstream carried here).
 * Callers must abort the instruction on false, BEFORE committing the register. */
bool load_pdpte(int32_t cr3);
/* cpu.rs:2977-2997's checks only, WITHOUT installing reg_pdpte.  Returns false after
 * delivering #GP(0) on the same two conditions as load_pdpte.  The task switch uses
 * this to validate the new task's PDPT before any register commit, while the new TSS
 * is still read under the old page tables (load_pdpte/set_cr3 installs the cache and
 * commits at the end of the switch). */
bool validate_pdpte(int32_t cr3);

/* S3: `addr` is the FAULTING LINEAR ADDRESS (64-bit since ss4); cr[2] -- the guest-visible
 * CR2 -- stays 32-bit because the physical model is (ss5.1), so the store projects with a
 * comment at the site.  Long mode's 64-bit CR2 is S11's. */
void trigger_pagefault(uint64_t addr,
                       bool present,
                       bool write,
                       bool user,
                       bool is_instruction_fetch,
                       bool rsvd);

/* the page-crossing helpers used by safe_write* (cpu.rs:3477-3530) */
int32_t virt_boundary_read16(uint32_t low, uint32_t high);
int32_t virt_boundary_read32s(uint32_t low, uint32_t high);
void virt_boundary_write16(uint32_t low, uint32_t high, int32_t value);
void virt_boundary_write32(uint32_t low, uint32_t high, int32_t value);

/* safe_* -- cpu.rs:3538-3585 (reads), 3941-4040 (writes).  Sentinel on fault. */
int32_t safe_read8(int32_t addr);
int32_t safe_read16(int32_t addr);
int32_t safe_read32s(int32_t addr);
float safe_read_f32(int32_t addr);
uint64_t safe_read64s(int32_t addr);
reg128 safe_read128s(int32_t addr);

void safe_write8(int32_t addr, int32_t value);
void safe_write16(int32_t addr, int32_t value);
void safe_write32(int32_t addr, int32_t value);
void safe_write64(int32_t addr, uint64_t value);
void safe_write128(int32_t addr, reg128 value);

/* ------------------------------------------------------------------ *
 * safe_read_write* -- the READ-MODIFY-WRITE helpers (cpu.rs:4052, 4073, 4101).
 *
 * Upstream takes a closure `&dyn Fn(i32) -> i32`; C has no closures, and every one of
 * the ~120 call sites in instructions.rs passes `&|x| <expr in x>` where <expr> is a
 * single ALU/rotate/shift call.  The port therefore spells the helper as a STATEMENT
 * MACRO: the expansion is upstream's body verbatim (with the JIT callbacks dropped, as
 * everywhere else), and the caller writes the operation inline as `value_expr`, using the
 * macro-declared `x_` as its operand -- e.g.
 *
 *     SAFE_READ_WRITE8(addr, add8(x_, read_reg8(r)));
 *
 * Properties the call sites rely on, all upstream's:
 *   - the address is translated ONCE, before the read (write translation: upstream's
 *     translate_address_write_and_can_skip_dirty);
 *   - a fault RETURNS FROM THE ENCLOSING INSTRUCTION FUNCTION with the translation's
 *     partial effects kept -- upstream's `return_on_pagefault!` returns from
 *     safe_read_write8, and safe_read_write8 is the whole body of every caller, so the
 *     effect is the same (stated rather than assumed: it is why the macro may contain a
 *     `return`, and it is checked by the selftest's fault case);
 *   - the 16/32-bit forms handle the page boundary through virt_boundary_* exactly as
 *     upstream -- those are the only callers of virt_boundary_read16/read32s (M1a-2's
 *     P-NOT-3 correction);
 *   - `addr_expr` is a plain variable at every call site (the boundary branch uses it
 *     twice, as upstream does);
 *   - the non-straddle write arm HOOKS THE UOP CACHE (uop_invalidate_page_fast, upstream's
 *     dropped jit::jit_dirty_page -- PERF-FIX-3/BLOCKER-1).  This macro family is the
 *     ENTIRE read-modify-write class (165 call sites: `xor [mem], imm` and friends), and
 *     the hook was missing here for a whole milestone because the writer is a MACRO: the
 *     reconciliation grep matches write*() calls, and a macro invocation is not one.  The
 *     straddle arms need nothing -- they route through virt_boundary_write16/32, i.e.
 *     through hooking write8;
 *   - the macro is a STATEMENT: it declares locals (phys_addr_, x_, value_) whose names
 *     must not be reused by the caller, and it cannot be used as an expression.
 * ------------------------------------------------------------------ */

#define SAFE_READ_WRITE8(addr_expr, value_expr)                                       \
    do {                                                                              \
        uint32_t phys_addr_ = translate_address_write_and_can_skip_dirty(addr_expr);   \
        if (cpu->pagefault) return;                                                    \
        int32_t x_ = read8(phys_addr_);                                                \
        int32_t value_ = (value_expr);                                                  \
        DBG_ASSERT(value_ >= 0 && value_ < 0x100);                                      \
        if (in_mapped_range(phys_addr_)) {                                              \
            mmap_write8(phys_addr_, value_);                                            \
        }                                                                               \
        else {                                                                          \
            /* cpu.rs:4062's jit::jit_dirty_page, restored: this arm writes guest RAM the \
             * same way write8 does, and it was the one writer the M2 reconciliation      \
             * could not see (a MACRO invocation is not a call the grep matches).         \
             * PERF-FIX-3/BLOCKER-1; the straddle arm above needs nothing (its two        \
             * virt_boundary_write8's each hook their own page). */                       \
            uop_invalidate_page_fast(phys_addr_);                                       \
            write8_no_mmap_or_dirty_check(phys_addr_, value_);                          \
        }                                                                               \
    } while (0)

#define SAFE_READ_WRITE16(addr_expr, value_expr)                                      \
    do {                                                                              \
        uint32_t phys_addr_ = translate_address_write_and_can_skip_dirty(addr_expr);   \
        if (cpu->pagefault) return;                                                    \
        if ((phys_addr_ & 0xFFF) == 0xFFF) {                                            \
            uint32_t phys_addr_high_ = translate_address_write((addr_expr) + 1);        \
            if (cpu->pagefault) return;                                                \
            int32_t x_ = virt_boundary_read16(phys_addr_, phys_addr_high_);             \
            virt_boundary_write16(phys_addr_, phys_addr_high_, (value_expr));           \
        }                                                                               \
        else {                                                                          \
            int32_t x_ = read16(phys_addr_);                                            \
            int32_t value_ = (value_expr);                                              \
            DBG_ASSERT(value_ >= 0 && value_ < 0x10000);                                \
            if (in_mapped_range(phys_addr_)) {                                          \
                mmap_write16(phys_addr_, value_);                                       \
            }                                                                           \
            else {                                                                      \
                /* cpu.rs:4082's jit::jit_dirty_page (PERF-FIX-3/BLOCKER-1) */           \
                uop_invalidate_page_fast(phys_addr_);                                   \
                write16_no_mmap_or_dirty_check(phys_addr_, value_);                     \
            }                                                                           \
        }                                                                               \
    } while (0)

#define SAFE_READ_WRITE32(addr_expr, value_expr)                                      \
    do {                                                                              \
        uint32_t phys_addr_ = translate_address_write_and_can_skip_dirty(addr_expr);   \
        if (cpu->pagefault) return;                                                    \
        if ((phys_addr_ & 0xFFF) >= 0xFFD) {                                            \
            uint32_t phys_addr_high_ =                                                  \
                translate_address_write(((uint32_t)((addr_expr) + 3) & 0xFFFFFFFCu));   \
            if (cpu->pagefault) return;                                                \
            phys_addr_high_ |= ((uint32_t)((addr_expr) + 3) & 3);                       \
            int32_t x_ = virt_boundary_read32s(phys_addr_, phys_addr_high_);            \
            virt_boundary_write32(phys_addr_, phys_addr_high_, (value_expr));           \
        }                                                                               \
        else {                                                                          \
            int32_t x_ = read32s(phys_addr_);                                           \
            int32_t value_ = (value_expr);                                              \
            if (in_mapped_range(phys_addr_)) {                                          \
                mmap_write32(phys_addr_, value_);                                       \
            }                                                                           \
            else {                                                                      \
                /* cpu.rs:4104's jit::jit_dirty_page (PERF-FIX-3/BLOCKER-1) */           \
                uop_invalidate_page_fast(phys_addr_);                                   \
                write32_no_mmap_or_dirty_check(phys_addr_, value_);                     \
            }                                                                           \
        }                                                                               \
    } while (0)

#endif /* V86C_PAGING_H */
