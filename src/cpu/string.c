/*
 * string.c -- the string instructions.  Port of v86/src/rust/cpu/string.rs (701 lines).
 *
 *       cmp  si  di
 * movs   0    1   1/w    A4
 * cmps   1    1   1/r    A6
 * stos   0    0   1/w    AA
 * lods   0    1   0      AC
 * scas   1    0   1/r    AE
 * ins    0    0   1/w
 * outs   0    1   0
 *
 * The structure is upstream's (string.rs:60-514), including the two paths:
 *
 *   - the FAST path, which translates the source and the destination ONCE, copies or
 *     accumulates a whole page worth of iterations directly through mem8 (or the VGA LFB)
 *     and then rewinds instruction_pointer so the next call continues at the page
 *     boundary (the "go back to the current instruction, since this loop just handles a
 *     single page" of string.rs:380-383);
 *   - the SLOW path, one iteration per call through safe_read and safe_write, which is what
 *     makes a page fault land mid-string with the registers advanced by exactly the
 *     iterations that completed (plan ss8/R3: the partial effects are KEPT).
 *
 * DIVERGENCES (see the unit report for the full table):
 *   - `jit::jit_dirty_page` and the `can_skip_dirty_page`/`skip_dirty_page` bookkeeping
 *     that exists only to gate it are DROPPED (string.rs:155, 168, 175, 181, 247-249):
 *     memory.c already dropped the same callbacks, and the pages are not (re)compiled in
 *     an interpreter-only port.  The control flow is unchanged; only the callback is gone.
 *   - the local variable holding the per-page iteration count is named
 *     `count_until_page_end` because C has one namespace: upstream shadows the helper
 *     `count_until_end_of_page` with a local of the same name (string.rs:160 vs :195), which
 *     is legal in Rust only because the initializer is evaluated before the binding exists.
 *     The helper keeps upstream's name.
 *   - `memory::memcpy_into_svga_lfb` is ported into memory.c (memory.rs:254-264, its
 *     owner) -- M1a-1 dropped it because nothing called it; this file's movs fast path is
 *     its first caller.
 *   - dbg_assert!/dbg_log! become DBG_ASSERT / are dropped, as in every prior unit.
 */

#include <string.h>

#include "string.h"
#include "uop_cache.h" /* PERF-4/M2: the fast path's write-side invalidation (plan ss4.1) */

#include "arith.h"   /* cmp8/cmp16/cmp32/cmp64 */
#include "flags.h"   /* getzf -- the 64-bit path's repe/repne exit */
#include "memory.h"  /* read*_no_mmap_check, write*_no_mmap_or_dirty_check, in_mapped_range,
                      * memcpy_into_svga_lfb, in_svga_lfb */
#include "misc_instr.h" /* set_reg_asize */
#include "modrm.h"   /* get_seg + the register indices of cpu_state.h */
#include "paging.h"  /* translate_address_*, safe_read*, safe_write*, writable_or_pagefault */
#include "debug.h"   /* the toolkit's RANGE watch: a bulk copy is one store (results/x86-64-reach.md) */

/* string.rs:25-32 */
static uint32_t count_until_end_of_page(int32_t direction, int32_t size, uint32_t addr)
{
    if (direction == 1) {
        return (0x1000 - (addr & 0xFFF)) / (uint32_t)size;
    }
    else {
        return (addr & 0xFFF) / (uint32_t)size + 1;
    }
}

/* string.rs:34-55 */
enum string_op {
    STR_MOVS,
    STR_LODS,
    STR_STOS,
    STR_SCAS,
    STR_CMPS,
    STR_INS,
    STR_OUTS,
};
enum string_size {
    STR_B,
    STR_W,
    STR_D,
    STR_Q, /* 8 bytes -- REX.W on A5/A7/AB/AD/AF in 64-bit mode.  UNREACHABLE outside long
            * mode: only the 0x2NN arms pass it, and dispatch_os_index answers 2 only when
            * EFER.LMA and CS.L are both set (modrm.c). */
};
enum string_rep {
    STR_REP_NONE,
    STR_REP_Z,
    STR_REP_NZ,
};

/* ================================================================== *
 * THE 64-BIT PATH -- the same five string operations at long mode's widths
 *
 * WHY A SECOND PATH RATHER THAN A WIDER FIRST ONE: every register and every address in
 * `string_instruction` below is int32_t, and the 32-bit machine must stay byte-identical
 * (the M0 transcript is the contract).  So long mode gets its own implementation and the
 * 32-bit one is not touched.  What the 64-bit path must get right, and the corpus cases
 * that judge each (tests/nasm64/str-*):
 *
 *   - RSI/RDI/RCX are 64-BIT (`str/movs-64`, `str/cmps-64`), and 0x67 narrows them to
 *     32 bits with the ZERO-EXTENDING write-back the architecture specifies (`str/asize-67`);
 *   - DF selects the direction and the step is the OPERAND width (`str/movs-64`'s DF=1 arm);
 *   - REX.W makes the operand 8 bytes -- MOVSQ/CMPSQ/STOSQ/LODSQ/SCASQ (`str/stos-64`,
 *     `str/lods-64`, `str/scas-64`);
 *   - repe/repne exit on ZF after a comparison, and the count written back is the number of
 *     iterations NOT performed (`str/cmps-64` runs RCX above 2^32 for exactly this);
 *   - A FAULT KEEPS THE PARTIAL EFFECTS: the registers reflect the iterations that
 *     completed, and RCX still counts the faulting one, because the instruction is
 *     re-executed after the handler's iret (plan ss8/R3).
 *
 * WHAT IS *NOT* HERE, NAMED: the page-bounded FAST path (string_instruction's bulk
 * memcpy/memset).  Long mode runs one iteration per loop pass through the safe accessors,
 * which is the SLOW path's semantics exactly; the fast path is an optimisation whose
 * absence costs speed, never correctness.  results/x86-64-rep.md ss6 records it as a gap
 * with the reason (no instrument can distinguish the two paths -- they must agree).
 * ================================================================== */

static uint64_t string64_reg(bool asize_32, int32_t reg)
{
    return asize_32 ? (uint64_t)(uint32_t)read_reg32(reg) : read_reg64(reg);
}

static void string64_set_reg(bool asize_32, int32_t reg, uint64_t value)
{
    /* a 32-bit address size ZERO-EXTENDS the write-back, as every 32-bit write does */
    if (asize_32) write_reg32(reg, (int32_t)(uint32_t)value);
    else write_reg64(reg, value);
}

static int64_t string64_read(int64_t addr, int32_t size_bytes)
{
    switch (size_bytes) {
    case 1: return (int64_t)safe_read8_addr(addr);
    case 2: return (int64_t)safe_read16_addr(addr);
    case 4: return (int64_t)safe_read32_addr(addr);
    default: return (int64_t)safe_read64_addr(addr);
    }
}

static void string64_write(int64_t addr, int32_t size_bytes, uint64_t value)
{
    switch (size_bytes) {
    case 1: safe_write8_addr(addr, (int32_t)(value & 0xFF)); break;
    case 2: safe_write16_addr(addr, (int32_t)(value & 0xFFFF)); break;
    case 4: safe_write32_addr(addr, (int32_t)(uint32_t)value); break;
    default: safe_write64_addr(addr, value); break;
    }
}

static void string_instruction64(int32_t ds_or_prefix,
                                 enum string_op instruction,
                                 enum string_size size,
                                 enum string_rep rep)
{
    bool asize_32 = is_asize_32();
    int32_t size_bytes = size == STR_B ? 1 : size == STR_W ? 2 : size == STR_D ? 4 : 8;
    uint64_t asize_mask = asize_32 ? 0xFFFFFFFFull : ~(uint64_t)0;
    int64_t increment = ((cpu->flags & FLAG_DIRECTION) != 0 ? -1 : 1) * size_bytes;
    bool do_rep = rep == STR_REP_Z || rep == STR_REP_NZ;

    /* TWO DIFFERENT QUESTIONS, and conflating them is the bug this comment exists to stop:
     * "has an ES:(E)DI operand" (CMPS and SCAS READ through it and still advance DI) is not
     * "writes memory" (only MOVS/STOS/INS store).  MEASURED: the first draft keyed the DI
     * side off the STORE, so `repe cmpsb` compared against address 0 and left RDI alone --
     * tests/nasm64/str-cmps-count-64 caught it (expected rdi 0x100121, got 0x100120). */
    bool uses_esi = instruction == STR_MOVS || instruction == STR_CMPS ||
                    instruction == STR_LODS || instruction == STR_OUTS;
    bool uses_edi = instruction == STR_MOVS || instruction == STR_CMPS ||
                    instruction == STR_STOS || instruction == STR_SCAS || instruction == STR_INS;

    uint64_t count = do_rep ? (string64_reg(asize_32, ECX) & asize_mask) : 1;
    if (do_rep && count == 0) return;

    int64_t ds = 0;
    if (uses_esi) {
        ds = get_seg(ds_or_prefix);
        if (cpu->pagefault) return;
    }
    int64_t es = 0;
    if (uses_edi) {
        es = get_seg(ES);
        if (cpu->pagefault) return;
    }

    int32_t port = 0;
    if (instruction == STR_INS || instruction == STR_OUTS) {
        port = read_reg16(DX);
        if (!test_privileges_for_io(port, size_bytes)) return;
    }

    uint64_t src = uses_esi ? (string64_reg(asize_32, ESI) & asize_mask) : 0;
    uint64_t dst = uses_edi ? (string64_reg(asize_32, EDI) & asize_mask) : 0;
    uint64_t data = (instruction == STR_STOS || instruction == STR_SCAS) ? read_reg64(EAX) : 0;

    for (;;) {
        int64_t src_val = 0;
        int64_t dst_val = 0;

        if (instruction == STR_INS) {
            /* check the fault *before* reading the port (upstream's order).  The address is
             * NOT narrowed here: the store below (string64_write) uses the full 64-bit
             * `es + dst`, so a 32-bit pre-check would fault on a page the store never
             * touches -- a spurious #PF with a truncated cr2 for an INS buffer above 4 GiB
             * (an insw into a physmap buffer).  results/x86-64-addrwidth.md finding 4. */
            writable_or_pagefault((uint64_t)(es + (int64_t)dst), size_bytes);
            if (cpu->pagefault) break;
        }
        if (uses_esi) {
            src_val = string64_read(ds + (int64_t)src, size_bytes);
            if (cpu->pagefault) break;
        }
        else if (instruction == STR_STOS || instruction == STR_SCAS) {
            src_val = (int64_t)data;
        }
        else { /* STR_INS */
            src_val = size_bytes == 1 ? io_port_read8(port)
                    : size_bytes == 2 ? io_port_read16(port)
                                      : io_port_read32(port);
        }

        if (instruction == STR_CMPS || instruction == STR_SCAS) {
            dst_val = string64_read(es + (int64_t)dst, size_bytes);
            if (cpu->pagefault) break;
        }
        else if (instruction == STR_OUTS) {
            if (size_bytes == 1) io_port_write8(port, src_val);
            else if (size_bytes == 2) io_port_write16(port, src_val);
            else io_port_write32(port, src_val);
        }
        else if (instruction == STR_LODS) {
            /* the ACCUMULATOR is the destination and it keeps the operand width: a LODSQ
             * fills all eight bytes of RAX, a LODSD zero-extends (write_reg32's rule) */
            if (size_bytes == 1) write_reg8(AL, src_val);
            else if (size_bytes == 2) write_reg16(AX, src_val);
            else if (size_bytes == 4) write_reg32(EAX, src_val);
            else write_reg64(EAX, (uint64_t)src_val);
        }
        else { /* STR_MOVS | STR_STOS | STR_INS */
            string64_write(es + (int64_t)dst, size_bytes, (uint64_t)src_val);
            if (cpu->pagefault) break;
        }

        /* the step is the OPERAND width, in the DF direction, at the address size */
        dst = uses_edi ? (uint64_t)((int64_t)dst + increment) & asize_mask : dst;
        src = uses_esi ? (uint64_t)((int64_t)src + increment) & asize_mask : src;

        if (instruction == STR_CMPS || instruction == STR_SCAS) {
            if (size_bytes == 1) cmp8(src_val, dst_val);
            else if (size_bytes == 2) cmp16(src_val, dst_val);
            else if (size_bytes == 4) cmp32(src_val, dst_val);
            else cmp64(src_val, dst_val);
        }

        count -= 1;

        bool finished;
        if (!do_rep) finished = true;
        else if (count == 0) finished = true;
        else if ((instruction == STR_CMPS || instruction == STR_SCAS) && rep == STR_REP_Z) {
            finished = !getzf();
        }
        else if ((instruction == STR_CMPS || instruction == STR_SCAS) && rep == STR_REP_NZ) {
            finished = getzf();
        }
        else finished = false;
        if (finished) break;
    }

    if (uses_edi) string64_set_reg(asize_32, EDI, dst);
    if (uses_esi) string64_set_reg(asize_32, ESI, src);
    if (do_rep) string64_set_reg(asize_32, ECX, count);
}

/* We implement all string instructions here and rely on the inliner on doing its job of
 * optimising away anything known at compile time (check with `wasm-dis build/v86.wasm`) */
static void string_instruction(bool is_asize_32,
                               int32_t ds_or_prefix,
                               enum string_op instruction,
                               enum string_size size,
                               enum string_rep rep)
{
    /* 64-BIT MODE: one implementation, above, because every register and address here is
     * int32_t and long mode's are not.  Outside long mode this line is dead. */
    if (long_mode_64()) {
        string_instruction64(ds_or_prefix, instruction, size, rep);
        return;
    }

    int32_t asize_mask = is_asize_32 ? -1 : 0xFFFF;

    int32_t direction = (cpu->flags & FLAG_DIRECTION) != 0 ? -1 : 1;

    uint32_t count;
    if (rep == STR_REP_Z || rep == STR_REP_NZ) {
        uint32_t c = (uint32_t)(read_reg32(ECX) & asize_mask);
        if (c == 0) {
            return;
        }
        count = c;
    }
    else {
        count = 0;
    }

    int32_t es = 0;
    if (instruction == STR_MOVS || instruction == STR_CMPS || instruction == STR_STOS ||
        instruction == STR_SCAS || instruction == STR_INS) {
        es = get_seg(ES);
        if (cpu->pagefault) return;
    }
    int32_t ds = 0;
    if (instruction == STR_MOVS || instruction == STR_CMPS || instruction == STR_LODS ||
        instruction == STR_SCAS || instruction == STR_OUTS) {
        ds = get_seg(ds_or_prefix);
        if (cpu->pagefault) return;
    }

    int32_t size_bytes;
    int32_t size_mask;
    switch (size) {
    case STR_B: size_bytes = 1; size_mask = 0xFF; break;
    case STR_W: size_bytes = 2; size_mask = 0xFFFF; break;
    case STR_D: size_bytes = 4; size_mask = -1; break;
    /* STR_Q cannot reach the 32-bit path (see the enum); naming it rather than letting it
     * fall into STR_D's arm keeps the two paths' widths from drifting silently */
    default: DBG_ASSERT(false); size_bytes = 4; size_mask = -1; break;
    }

    int32_t increment = direction * size_bytes;

    int32_t data = (instruction == STR_STOS || instruction == STR_SCAS) ? read_reg32(EAX) : 0;

    int32_t src = 0;
    if (instruction == STR_MOVS || instruction == STR_CMPS || instruction == STR_LODS ||
        instruction == STR_OUTS) {
        src = read_reg32(ESI) & asize_mask;
    }
    int32_t dst = 0;
    if (instruction == STR_MOVS || instruction == STR_CMPS || instruction == STR_STOS ||
        instruction == STR_SCAS || instruction == STR_INS) {
        dst = read_reg32(EDI) & asize_mask;
    }

    int32_t port = 0;
    if (instruction == STR_INS || instruction == STR_OUTS) {
        port = read_reg16(DX);
        if (!test_privileges_for_io(port, size_bytes)) {
            return;
        }
    }

    bool is_aligned = ((uint32_t)(ds + src) & (uint32_t)(size_bytes - 1)) == 0 &&
                      ((uint32_t)(es + dst) & (uint32_t)(size_bytes - 1)) == 0;

    /* unaligned movs is properly handled in the fast path */
    bool rep_fast = (instruction == STR_MOVS || is_aligned) && is_asize_32 /* 16-bit address wraparound */
                    && (rep == STR_REP_NZ || rep == STR_REP_Z);

    uint32_t phys_dst = 0;
    uint32_t phys_src = 0;

    bool movs_into_svga_lfb = false;
    bool movs_reenter_fast_path = false;

    uint32_t count_until_page_end = 0;
    if (rep_fast) {
        if (instruction == STR_MOVS) {
            phys_dst = translate_address_write_and_can_skip_dirty(es + dst);
            if (cpu->pagefault) return;
            movs_into_svga_lfb = in_svga_lfb(phys_dst);
            rep_fast = rep_fast && (!in_mapped_range(phys_dst) || movs_into_svga_lfb);
        }
        else if (instruction == STR_STOS || instruction == STR_INS) {
            phys_dst = translate_address_write_and_can_skip_dirty(es + dst);
            if (cpu->pagefault) return;
            rep_fast = rep_fast && !in_mapped_range(phys_dst);
        }
        else if (instruction == STR_CMPS || instruction == STR_SCAS) {
            phys_dst = translate_address_read(es + dst);
            if (cpu->pagefault) return;
            rep_fast = rep_fast && !in_mapped_range(phys_dst);
        }

        if (instruction == STR_MOVS || instruction == STR_CMPS || instruction == STR_LODS ||
            instruction == STR_OUTS) {
            phys_src = translate_address_read(ds + src);
            if (cpu->pagefault) return;
            rep_fast = rep_fast && !in_mapped_range(phys_src);
        }

        if (instruction == STR_MOVS || instruction == STR_CMPS) {
            uint32_t a = count_until_end_of_page(direction, size_bytes, phys_src);
            uint32_t b = count_until_end_of_page(direction, size_bytes, phys_dst);
            count_until_page_end = count < (a < b ? a : b) ? count : (a < b ? a : b);
        }
        else if (instruction == STR_STOS || instruction == STR_INS || instruction == STR_SCAS) {
            uint32_t a = count_until_end_of_page(direction, size_bytes, phys_dst);
            count_until_page_end = count < a ? count : a;
        }
        else {
            uint32_t a = count_until_end_of_page(direction, size_bytes, phys_src);
            count_until_page_end = count < a ? count : a;
        }

        if (instruction == STR_MOVS) {
            uint32_t c = count_until_page_end * (uint32_t)size_bytes;

            bool overlap_interferes;
            if (phys_src < phys_dst) {
                /* backward moves may overlap at the front of the destination string */
                overlap_interferes = phys_dst - phys_src < c && direction == 1;
            }
            else if (phys_src > phys_dst) {
                /* forward moves may overlap at the front of the source string */
                overlap_interferes = phys_src - phys_dst < c && direction == -1;
            }
            else {
                overlap_interferes = false;
            }
            rep_fast = rep_fast && !overlap_interferes;

            /* In case the following page-boundary check fails, re-enter instruction after
             * one iteration of the slow path */
            movs_reenter_fast_path = rep_fast;
            rep_fast = rep_fast &&
                       (phys_src & 0xFFF) <= 0x1000 - (uint32_t)size_bytes &&
                       (phys_dst & 0xFFF) <= 0x1000 - (uint32_t)size_bytes;
        }
    }
    else {
        count_until_page_end = 0; /* not used */
    }

    if (rep_fast) {
        DBG_ASSERT(count_until_page_end > 0);

        bool rep_cmp_finished = false;

        uint32_t i = 0;
        while (i < count_until_page_end) {
            i += 1;

            int32_t src_val;
            if (instruction == STR_MOVS || instruction == STR_CMPS || instruction == STR_LODS ||
                instruction == STR_OUTS) {
                src_val = size == STR_B ? read8_no_mmap_check(phys_src)
                        : size == STR_W ? read16_no_mmap_check(phys_src)
                                        : read32_no_mmap_check(phys_src);
            }
            else if (instruction == STR_SCAS || instruction == STR_STOS) {
                src_val = data & size_mask;
            }
            else { /* STR_INS */
                src_val = size == STR_B ? io_port_read8(port)
                        : size == STR_W ? io_port_read16(port)
                                        : io_port_read32(port);
            }

            int32_t dst_val = 0;

            if (instruction == STR_CMPS || instruction == STR_SCAS) {
                dst_val = size == STR_B ? read8_no_mmap_check(phys_dst)
                        : size == STR_W ? read16_no_mmap_check(phys_dst)
                                        : read32_no_mmap_check(phys_dst);
            }
            else if (instruction == STR_OUTS) {
                if (size == STR_B) io_port_write8(port, src_val);
                else if (size == STR_W) io_port_write16(port, src_val);
                else io_port_write32(port, src_val);
            }
            else if (instruction == STR_LODS) {
                if (size == STR_B) write_reg8(AL, src_val);
                else if (size == STR_W) write_reg16(AX, src_val);
                else write_reg32(EAX, src_val);
            }
            else if (instruction == STR_INS) {
                /* the fast path writes RAM directly (no write8/write16/write32), so the
                 * invalidation hook the plan's ss4.1 table requires has to be here: upstream
                 * calls jit_dirty_page(phys_dst) at the same point of string.rs:248 */
                uop_invalidate_page_fast(phys_dst);
                if (size == STR_B) write8_no_mmap_or_dirty_check(phys_dst, src_val);
                else if (size == STR_W) write16_no_mmap_or_dirty_check(phys_dst, src_val);
                else write32_no_mmap_or_dirty_check(phys_dst, src_val);
            }
            else if (instruction == STR_MOVS) {
                if (direction == -1) {
                    phys_src -= (count_until_page_end - 1) * (uint32_t)size_bytes;
                    phys_dst -= (count_until_page_end - 1) * (uint32_t)size_bytes;
                }
                if (movs_into_svga_lfb) {
                    /* the destination is the VGA LFB, not guest RAM: no code can live there */
                    memcpy_into_svga_lfb(phys_src, phys_dst,
                                         count_until_page_end * (uint32_t)size_bytes);
                }
                else {
                    /* string.rs:248's jit_dirty_page(phys_dst), restored for M2 -- as a
                     * RANGE because this one memcpy writes the whole run (the page-bounded
                     * fast path keeps it inside one page; the range form is belt-and-braces
                     * and is what write_blob uses too) */
                    uop_invalidate_range(phys_dst, count_until_page_end * (uint32_t)size_bytes);
                    if (v86c_dbg_watch_on) {
                        v86c_dbg_watch_range(phys_dst, (uint64_t)count_until_page_end * (uint64_t)size_bytes,
                                             cpu->instruction_pointer);
                    }
                    memcpy_no_mmap_or_dirty_check(phys_src, phys_dst,
                                                  count_until_page_end * (uint32_t)size_bytes);
                }
                i = count_until_page_end;
                break;
            }
            else { /* STR_STOS */
                if (size == STR_B) {
                    if (direction == -1) {
                        phys_dst -= count_until_page_end - 1;
                    }
                    uop_invalidate_range(phys_dst, count_until_page_end);
                    if (v86c_dbg_watch_on) {
                        v86c_dbg_watch_range(phys_dst, (uint64_t)count_until_page_end,
                                             cpu->instruction_pointer);
                    }
                    memset_no_mmap_or_dirty_check(phys_dst, (uint8_t)src_val, count_until_page_end);
                    i = count_until_page_end;
                    break;
                }
                else if (size == STR_W) {
                    uop_invalidate_page_fast(phys_dst);
                    write16_no_mmap_or_dirty_check(phys_dst, src_val);
                }
                else {
                    uop_invalidate_page_fast(phys_dst);
                    write32_no_mmap_or_dirty_check(phys_dst, src_val);
                }
            }

            if (instruction == STR_MOVS || instruction == STR_CMPS || instruction == STR_STOS ||
                instruction == STR_SCAS || instruction == STR_INS) {
                phys_dst += (uint32_t)increment;
            }
            if (instruction == STR_MOVS || instruction == STR_CMPS || instruction == STR_LODS ||
                instruction == STR_OUTS) {
                phys_src += (uint32_t)increment;
            }

            if (instruction == STR_SCAS || instruction == STR_CMPS) {
                bool rep_cmp;
                if (rep == STR_REP_Z) rep_cmp = src_val == dst_val;
                else if (rep == STR_REP_NZ) rep_cmp = src_val != dst_val;
                else {
                    DBG_ASSERT(false);
                    rep_cmp = true;
                }
                if (!rep_cmp || count == i) {
                    if (size == STR_B) cmp8(src_val, dst_val);
                    else if (size == STR_W) cmp16(src_val, dst_val);
                    else cmp32(src_val, dst_val);
                    rep_cmp_finished = true;
                    break;
                }
            }
        }

        DBG_ASSERT(i <= count);
        count -= i;

        if (!rep_cmp_finished && count != 0) {
            /* go back to the current instruction, since this loop just handles a single page */
            cpu->instruction_pointer = cpu->previous_ip;
        }

        src += (int32_t)i * increment;
        dst += (int32_t)i * increment;
    }
    else {
        for (;;) {
            if (instruction == STR_INS) {
                /* check fault *before* reading from port
                 * (technically not necessary according to Intel manuals) */
                writable_or_pagefault(es + dst, size_bytes);
                if (cpu->pagefault) break;
            }
            int32_t src_val;
            if (instruction == STR_MOVS || instruction == STR_CMPS || instruction == STR_LODS ||
                instruction == STR_OUTS) {
                src_val = size == STR_B ? safe_read8(ds + src)
                        : size == STR_W ? safe_read16(ds + src)
                                        : safe_read32s(ds + src);
                if (cpu->pagefault) break;
            }
            else if (instruction == STR_SCAS || instruction == STR_STOS) {
                src_val = data & size_mask;
            }
            else { /* STR_INS */
                src_val = size == STR_B ? io_port_read8(port)
                        : size == STR_W ? io_port_read16(port)
                                        : io_port_read32(port);
            }

            int32_t dst_val = 0;

            if (instruction == STR_CMPS || instruction == STR_SCAS) {
                dst_val = size == STR_B ? safe_read8(es + dst)
                        : size == STR_W ? safe_read16(es + dst)
                                        : safe_read32s(es + dst);
                if (cpu->pagefault) break;
            }
            else if (instruction == STR_OUTS) {
                if (size == STR_B) io_port_write8(port, src_val);
                else if (size == STR_W) io_port_write16(port, src_val);
                else io_port_write32(port, src_val);
            }
            else if (instruction == STR_LODS) {
                if (size == STR_B) write_reg8(AL, src_val);
                else if (size == STR_W) write_reg16(AX, src_val);
                else write_reg32(EAX, src_val);
            }
            else { /* STR_MOVS | STR_STOS | STR_INS */
                if (size == STR_B) safe_write8(es + dst, src_val);
                else if (size == STR_W) safe_write16(es + dst, src_val);
                else safe_write32(es + dst, src_val);
                if (cpu->pagefault) break;
            }

            if (instruction == STR_MOVS || instruction == STR_CMPS || instruction == STR_STOS ||
                instruction == STR_SCAS || instruction == STR_INS) {
                dst = (dst + increment) & asize_mask;
            }
            if (instruction == STR_MOVS || instruction == STR_CMPS || instruction == STR_LODS ||
                instruction == STR_OUTS) {
                src = (src + increment) & asize_mask;
            }

            count -= 1;

            bool finished;
            if (rep == STR_REP_Z || rep == STR_REP_NZ) {
                if (rep == STR_REP_Z && instruction == STR_CMPS) {
                    finished = src_val != dst_val || count == 0;
                }
                else if (rep == STR_REP_Z && instruction == STR_SCAS) {
                    finished = src_val != dst_val || count == 0;
                }
                else if (rep == STR_REP_NZ && instruction == STR_CMPS) {
                    finished = src_val == dst_val || count == 0;
                }
                else if (rep == STR_REP_NZ && instruction == STR_SCAS) {
                    finished = src_val == dst_val || count == 0;
                }
                else if ((rep == STR_REP_NZ || rep == STR_REP_Z) && instruction == STR_MOVS) {
                    if (count == 0) {
                        finished = true;
                    }
                    else if (movs_reenter_fast_path) {
                        cpu->instruction_pointer = cpu->previous_ip;
                        finished = true;
                    }
                    else {
                        finished = false;
                    }
                }
                else {
                    finished = count == 0;
                }
            }
            else {
                finished = true;
            }

            if (finished) {
                if (instruction == STR_SCAS || instruction == STR_CMPS) {
                    if (size == STR_B) cmp8(src_val, dst_val);
                    else if (size == STR_W) cmp16(src_val, dst_val);
                    else cmp32(src_val, dst_val);
                }
                break;
            }
        }
    }

    if (instruction == STR_MOVS || instruction == STR_CMPS || instruction == STR_STOS ||
        instruction == STR_SCAS || instruction == STR_INS) {
        set_reg_asize(is_asize_32, EDI, dst);
    }
    if (instruction == STR_MOVS || instruction == STR_CMPS || instruction == STR_LODS ||
        instruction == STR_OUTS) {
        set_reg_asize(is_asize_32, ESI, src);
    }

    if (rep == STR_REP_Z || rep == STR_REP_NZ) {
        set_reg_asize(is_asize_32, ECX, (int32_t)count);
    }
}

/* THE 8-BYTE (REX.W) FORMS -- string.rs has none, because upstream has no long mode.  The
 * names follow its scheme so the emitter's availability scan finds them by name. */
void movsq_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_MOVS, STR_Q, STR_REP_Z); }
void movsq_no_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_MOVS, STR_Q, STR_REP_NONE); }
void cmpsq_repz(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_CMPS, STR_Q, STR_REP_Z); }
void cmpsq_repnz(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_CMPS, STR_Q, STR_REP_NZ); }
void cmpsq_no_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_CMPS, STR_Q, STR_REP_NONE); }
void stosq_rep(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_STOS, STR_Q, STR_REP_Z); }
void stosq_no_rep(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_STOS, STR_Q, STR_REP_NONE); }
void lodsq_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_LODS, STR_Q, STR_REP_Z); }
void lodsq_no_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_LODS, STR_Q, STR_REP_NONE); }
void scasq_repz(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_SCAS, STR_Q, STR_REP_Z); }
void scasq_repnz(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_SCAS, STR_Q, STR_REP_NZ); }
void scasq_no_rep(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_SCAS, STR_Q, STR_REP_NONE); }

/* string.rs:516-536 */
void movsb_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_MOVS, STR_B, STR_REP_Z); }
void movsw_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_MOVS, STR_W, STR_REP_Z); }
void movsd_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_MOVS, STR_D, STR_REP_Z); }
void movsb_no_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_MOVS, STR_B, STR_REP_NONE); }
void movsw_no_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_MOVS, STR_W, STR_REP_NONE); }
void movsd_no_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_MOVS, STR_D, STR_REP_NONE); }

/* string.rs:538-558 */
void lodsb_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_LODS, STR_B, STR_REP_Z); }
void lodsw_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_LODS, STR_W, STR_REP_Z); }
void lodsd_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_LODS, STR_D, STR_REP_Z); }
void lodsb_no_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_LODS, STR_B, STR_REP_NONE); }
void lodsw_no_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_LODS, STR_W, STR_REP_NONE); }
void lodsd_no_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_LODS, STR_D, STR_REP_NONE); }

/* string.rs:560-580 */
void stosb_rep(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_STOS, STR_B, STR_REP_Z); }
void stosw_rep(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_STOS, STR_W, STR_REP_Z); }
void stosd_rep(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_STOS, STR_D, STR_REP_Z); }
void stosb_no_rep(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_STOS, STR_B, STR_REP_NONE); }
void stosw_no_rep(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_STOS, STR_W, STR_REP_NONE); }
void stosd_no_rep(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_STOS, STR_D, STR_REP_NONE); }

/* string.rs:582-617 */
void cmpsb_repz(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_CMPS, STR_B, STR_REP_Z); }
void cmpsw_repz(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_CMPS, STR_W, STR_REP_Z); }
void cmpsd_repz(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_CMPS, STR_D, STR_REP_Z); }
void cmpsb_repnz(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_CMPS, STR_B, STR_REP_NZ); }
void cmpsw_repnz(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_CMPS, STR_W, STR_REP_NZ); }
void cmpsd_repnz(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_CMPS, STR_D, STR_REP_NZ); }
void cmpsb_no_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_CMPS, STR_B, STR_REP_NONE); }
void cmpsw_no_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_CMPS, STR_W, STR_REP_NONE); }
void cmpsd_no_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_CMPS, STR_D, STR_REP_NONE); }

/* string.rs:619-651 */
void scasb_repz(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_SCAS, STR_B, STR_REP_Z); }
void scasw_repz(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_SCAS, STR_W, STR_REP_Z); }
void scasd_repz(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_SCAS, STR_D, STR_REP_Z); }
void scasb_repnz(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_SCAS, STR_B, STR_REP_NZ); }
void scasw_repnz(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_SCAS, STR_W, STR_REP_NZ); }
void scasd_repnz(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_SCAS, STR_D, STR_REP_NZ); }
void scasb_no_rep(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_SCAS, STR_B, STR_REP_NONE); }
void scasw_no_rep(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_SCAS, STR_W, STR_REP_NONE); }
void scasd_no_rep(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_SCAS, STR_D, STR_REP_NONE); }

/* string.rs:653-676 */
void outsb_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_OUTS, STR_B, STR_REP_Z); }
void outsw_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_OUTS, STR_W, STR_REP_Z); }
void outsd_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_OUTS, STR_D, STR_REP_Z); }
void outsb_no_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_OUTS, STR_B, STR_REP_NONE); }
void outsw_no_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_OUTS, STR_W, STR_REP_NONE); }
void outsd_no_rep(bool is_asize_32, int32_t seg) { string_instruction(is_asize_32, seg, STR_OUTS, STR_D, STR_REP_NONE); }

/* string.rs:678-701 */
void insb_rep(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_INS, STR_B, STR_REP_Z); }
void insw_rep(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_INS, STR_W, STR_REP_Z); }
void insd_rep(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_INS, STR_D, STR_REP_Z); }
void insb_no_rep(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_INS, STR_B, STR_REP_NONE); }
void insw_no_rep(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_INS, STR_W, STR_REP_NONE); }
void insd_no_rep(bool is_asize_32) { string_instruction(is_asize_32, 0, STR_INS, STR_D, STR_REP_NONE); }
