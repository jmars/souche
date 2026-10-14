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

#include "arith.h"   /* cmp8/cmp16/cmp32 */
#include "memory.h"  /* read*_no_mmap_check, write*_no_mmap_or_dirty_check, in_mapped_range,
                      * memcpy_into_svga_lfb, in_svga_lfb */
#include "misc_instr.h" /* set_reg_asize */
#include "modrm.h"   /* get_seg + the register indices of cpu_state.h */
#include "paging.h"  /* translate_address_*, safe_read*, safe_write*, writable_or_pagefault */

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
};
enum string_rep {
    STR_REP_NONE,
    STR_REP_Z,
    STR_REP_NZ,
};

/* We implement all string instructions here and rely on the inliner on doing its job of
 * optimising away anything known at compile time (check with `wasm-dis build/v86.wasm`) */
static void string_instruction(bool is_asize_32,
                               int32_t ds_or_prefix,
                               enum string_op instruction,
                               enum string_size size,
                               enum string_rep rep)
{
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
    default:    size_bytes = 4; size_mask = -1; break;
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
