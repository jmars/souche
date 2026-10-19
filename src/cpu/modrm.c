/*
 * modrm.c -- addressing: the prefix decoder, the segment-offset accessors, the
 * instruction-fetch path and modrm_resolve.  See modrm.h for the interface contract,
 * the ported cpu.rs line ranges and the fault behaviour.
 */

#include "modrm.h"

#include <stdio.h>

#include "cpu.h"
#include "memory.h"
#include "paging.h"

/* ------------------------------------------------------------------ *
 * prefix decode -- cpu.rs:2633-2640
 * ------------------------------------------------------------------ */

bool is_osize_32(void) { return cpu->is_32 != ((cpu->prefixes & PREFIX_MASK_OPSIZE) == PREFIX_MASK_OPSIZE); }

bool is_asize_32(void) { return cpu->is_32 != ((cpu->prefixes & PREFIX_MASK_ADDRSIZE) == PREFIX_MASK_ADDRSIZE); }

/* ------------------------------------------------------------------ *
 * the segment offsets and the segment-override prefix -- cpu.rs:3083-3116, 2918-2931
 * ------------------------------------------------------------------ */

uint64_t get_seg_cs(void) { return cpu->segment_offsets[CS]; }

uint64_t get_seg_ss(void) { return cpu->segment_offsets[SS]; }

/* cpu.rs:3088-3096.  Unlike get_seg_prefix this one asserts instead of returning 0 for
 * the "segment prefix zero" encoding; it is what instructions.rs calls for the string
 * ops' source segment (segment_prefix(DS)). */
int32_t segment_prefix(int32_t default_segment)
{
    int32_t prefix = cpu->prefixes & PREFIX_MASK_SEGMENT;
    if (prefix != 0) {
        DBG_ASSERT(prefix != SEG_PREFIX_ZERO);
        return prefix - 1;
    }
    else {
        return default_segment;
    }
}

int32_t get_seg_prefix(int32_t default_segment)
{
    int32_t prefix = cpu->prefixes & PREFIX_MASK_SEGMENT;
    if (prefix != 0) {
        if (prefix == SEG_PREFIX_ZERO) {
            return 0;
        }
        else {
            return get_seg(prefix - 1);
        }
    }
    else {
        return get_seg(default_segment);
    }
}

/* upstream: `Ok(get_seg_prefix(DS)? + offset)` -- the `?` means the offset is NOT added
 * when the segment lookup faulted (get_seg's #gp or a fault deeper down), so a faulting
 * call yields the 0 sentinel and not "0 + offset", which would be a plausible-looking
 * address built out of a failed lookup. */
int32_t get_seg_prefix_ds(int32_t offset)
{
    int32_t base = get_seg_prefix(DS);
    if (cpu->pagefault) {
        return 0;
    }
    return base + offset;
}

int32_t get_seg_prefix_ss(int32_t offset)
{
    int32_t base = get_seg_prefix(SS);
    if (cpu->pagefault) {
        return 0;
    }
    return base + offset;
}

int32_t get_seg(int32_t segment)
{
    DBG_ASSERT(segment >= 0 && segment < 8);
    /* read through a local: gcc -O2 cannot see the range the DBG_ASSERT establishes and
     * warns about `cpu->segment_is_null[segment]` (a false positive -- every caller
     * passes a segment index in [0,7]) */
    bool *is_null = cpu->segment_is_null;
    if (is_null[segment]) {
        DBG_ASSERT(segment != CS && segment != SS);
        /* the dbg_log! and dbg_trace() are dropped -- no log channel in this port */
        trigger_gp(0);
        /* upstream: `return Err(())`.  The #gp has already been *delivered*; the Err
         * only tells the instruction to abort, and cpu->pagefault is this port's abort
         * channel (cpu_state.h) -- so the flag is set here although the cause is a #gp
         * and not a page fault.  Set AFTER trigger_gp, never before. */
        cpu->pagefault = true;
        return 0;
    }
    /* S3: the SLOT is uint64_t, the ACCESSOR stays int32_t (modrm.h): this is the base of a
     * 32-bit ADDRESSING FORM, whose arithmetic is upstream's i32 and wraps.  The projection
     * is stated here rather than left to an implicit narrowing. */
    return (int32_t)(uint32_t)cpu->segment_offsets[segment];
}

/* ------------------------------------------------------------------ *
 * the fetch path -- cpu.rs:2584-2631
 * ------------------------------------------------------------------ */

int32_t read_imm8(void)
{
    /* S3: the cursor is 64 bits now; the fetch is still the page-relative XOR trick, and the
     * BOOKKEEPING value (`eip`) is kept at the field's width so a long-mode eip is not
     * projected by a local's type. */
    uint64_t eip = cpu->instruction_pointer;
    /* upstream's `0 != eip & !0xFFF ^ *last_virt_eip`: in Rust `&`/`^` bind TIGHTER than
     * `!=`, the reverse of C, hence the parentheses */
    if (DISABLE_EIP_TRANSLATION_OPTIMISATION ||
        (0 != ((eip & ~(uint64_t)0xFFF) ^ cpu->last_virt_eip))) {
        uint32_t phys = translate_address_fetch(eip);
        if (cpu->pagefault) {
            /* upstream's `?`: instruction_pointer, eip_phys and last_virt_eip are all
             * left untouched on a fault */
            return 0;
        }
        cpu->eip_phys = (uint64_t)phys ^ eip;
        cpu->last_virt_eip = eip & ~(uint64_t)0xFFF;
    }
    /* AUDIT-1 S1: the same resolution as cpu.c's fetch (get_phys_eip) -- the old
     * DBG_ASSERT was a guest-reachable abort; an in-RAM physical address (the VGA hole
     * included) is one the release reference executes from the raw bytes, and the raw
     * mem8 read below stands.  Past the guest's RAM the port names the stop instead of
     * reading host memory (the release crashes there; nasm_skip's jump fixture). */
    /* the projection at the mem8 boundary (paging.c's rule; plan ss5.1) */
    uint32_t phys = (uint32_t)(cpu->eip_phys ^ eip);
    if (phys >= cpu->memory_size) {
        char stop[160];
        snprintf(stop, sizeof(stop),
                 "instruction fetch at physical 0x%x, past the end of guest RAM"
                 " (0x%x) -- the release reference crashes here",
                 (unsigned)phys, (unsigned)cpu->memory_size);
        unimplemented_instruction(stop);
    }
    /* the physical address was translated above, so this is the raw mem8 read that
     * upstream does (memory::mem8.offset(..)), not the mmap-checked router */
    int32_t data8 = (int32_t)mem8[phys];
    set_instruction_pointer(eip + 1);
    return data8;
}

int32_t read_imm8s(void)
{
    int32_t v = read_imm8();
    if (cpu->pagefault) {
        return 0;
    }
    return ((int32_t)((uint32_t)v << 24)) >> 24;
}

int32_t read_imm16(void)
{
    /* two checks in one comparison: did the high 20 bits of eip change, or do the low
     * 12 bits cross a page boundary (cpu.rs:2598-2602) */
    /* S3: the test is at the field's width.  For every 32-bit eip the low 32 bits decide it
     * exactly as the old `(uint32_t)` cast of the XOR did; the widening is what lets a 64-bit
     * eip be tested without a projection. */
    if (DISABLE_EIP_TRANSLATION_OPTIMISATION ||
        (cpu->instruction_pointer ^ cpu->last_virt_eip) > 0xFFE) {
        /* upstream `read_imm8()? | read_imm8()? << 8` -- sequenced explicitly, because
         * the order of `|`'s operands is unspecified in C and both calls advance eip */
        int32_t low = read_imm8();
        if (cpu->pagefault) {
            return 0;
        }
        int32_t high = read_imm8();
        if (cpu->pagefault) {
            return 0;
        }
        return low | (high << 8);
    }
    else {
        int32_t data16 = read16((uint32_t)(cpu->eip_phys ^ cpu->instruction_pointer));
        set_instruction_pointer(cpu->instruction_pointer + 2);
        return data16;
    }
}

int32_t read_imm32s(void)
{
    if (DISABLE_EIP_TRANSLATION_OPTIMISATION ||
        (cpu->instruction_pointer ^ cpu->last_virt_eip) > 0xFFC) {
        int32_t low = read_imm16();
        if (cpu->pagefault) {
            return 0;
        }
        int32_t high = read_imm16();
        if (cpu->pagefault) {
            return 0;
        }
        return low | (high << 16);
    }
    else {
        int32_t data32 = read32s((uint32_t)(cpu->eip_phys ^ cpu->instruction_pointer));
        set_instruction_pointer(cpu->instruction_pointer + 4);
        return data32;
    }
}

/* ------------------------------------------------------------------ *
 * modrm.mem -- modrm.rs:4-133
 *
 * The arm-per-form shape of upstream's `match modrm_byte & !0o070` is kept, but the
 * table only computes `offset` (and the default segment); the single
 * get_seg_prefix_ds/_ss application at the end is the same call upstream makes in
 * every arm, and the fault ORDER is preserved: a disp read that faults returns before
 * the segment is consulted, exactly as Rust's `?` inside the argument does.
 * ------------------------------------------------------------------ */

#define MODRM_MASK (~0x38) /* modrm_byte & !0o070 */

static int32_t resolve_sib(bool with_imm); /* modrm.rs:67-100, defined below */

int32_t resolve_modrm16(int32_t modrm_byte)
{
    int32_t offset;
    int32_t default_segment = DS;

    switch (modrm_byte & MODRM_MASK) {
    case 0x00: offset = (read_reg16(BX) + read_reg16(SI)) & 0xFFFF; break;
    case 0x40: {
        int32_t d = read_imm8s();
        if (cpu->pagefault) return 0;
        offset = (read_reg16(BX) + read_reg16(SI) + d) & 0xFFFF;
        break;
    }
    case 0x80: {
        int32_t d = read_imm16();
        if (cpu->pagefault) return 0;
        offset = (read_reg16(BX) + read_reg16(SI) + d) & 0xFFFF;
        break;
    }
    case 0x01: offset = (read_reg16(BX) + read_reg16(DI)) & 0xFFFF; break;
    case 0x41: {
        int32_t d = read_imm8s();
        if (cpu->pagefault) return 0;
        offset = (read_reg16(BX) + read_reg16(DI) + d) & 0xFFFF;
        break;
    }
    case 0x81: {
        int32_t d = read_imm16();
        if (cpu->pagefault) return 0;
        offset = (read_reg16(BX) + read_reg16(DI) + d) & 0xFFFF;
        break;
    }
    case 0x02:
        default_segment = SS;
        offset = (read_reg16(BP) + read_reg16(SI)) & 0xFFFF;
        break;
    case 0x42: {
        int32_t d = read_imm8s();
        if (cpu->pagefault) return 0;
        default_segment = SS;
        offset = (read_reg16(BP) + read_reg16(SI) + d) & 0xFFFF;
        break;
    }
    case 0x82: {
        int32_t d = read_imm16();
        if (cpu->pagefault) return 0;
        default_segment = SS;
        offset = (read_reg16(BP) + read_reg16(SI) + d) & 0xFFFF;
        break;
    }
    case 0x03:
        default_segment = SS;
        offset = (read_reg16(BP) + read_reg16(DI)) & 0xFFFF;
        break;
    case 0x43: {
        int32_t d = read_imm8s();
        if (cpu->pagefault) return 0;
        default_segment = SS;
        offset = (read_reg16(BP) + read_reg16(DI) + d) & 0xFFFF;
        break;
    }
    case 0x83: {
        int32_t d = read_imm16();
        if (cpu->pagefault) return 0;
        default_segment = SS;
        offset = (read_reg16(BP) + read_reg16(DI) + d) & 0xFFFF;
        break;
    }
    case 0x04: offset = read_reg16(SI) & 0xFFFF; break;
    case 0x44: {
        int32_t d = read_imm8s();
        if (cpu->pagefault) return 0;
        offset = (read_reg16(SI) + d) & 0xFFFF;
        break;
    }
    case 0x84: {
        int32_t d = read_imm16();
        if (cpu->pagefault) return 0;
        offset = (read_reg16(SI) + d) & 0xFFFF;
        break;
    }
    case 0x05: offset = read_reg16(DI) & 0xFFFF; break;
    case 0x45: {
        int32_t d = read_imm8s();
        if (cpu->pagefault) return 0;
        offset = (read_reg16(DI) + d) & 0xFFFF;
        break;
    }
    case 0x85: {
        int32_t d = read_imm16();
        if (cpu->pagefault) return 0;
        offset = (read_reg16(DI) + d) & 0xFFFF;
        break;
    }
    case 0x06: {
        int32_t d = read_imm16(); /* not masked: upstream returns it as read */
        if (cpu->pagefault) return 0;
        offset = d;
        break;
    }
    case 0x46: {
        int32_t d = read_imm8s();
        if (cpu->pagefault) return 0;
        default_segment = SS;
        offset = (read_reg16(BP) + d) & 0xFFFF;
        break;
    }
    case 0x86: {
        int32_t d = read_imm16();
        if (cpu->pagefault) return 0;
        default_segment = SS;
        offset = (read_reg16(BP) + d) & 0xFFFF;
        break;
    }
    case 0x07: offset = read_reg16(BX) & 0xFFFF; break;
    case 0x47: {
        int32_t d = read_imm8s();
        if (cpu->pagefault) return 0;
        offset = (read_reg16(BX) + d) & 0xFFFF;
        break;
    }
    case 0x87: {
        int32_t d = read_imm16();
        if (cpu->pagefault) return 0;
        offset = (read_reg16(BX) + d) & 0xFFFF;
        break;
    }
    default:
        /* modrm.rs:30-33: unreachable (every value of `modrm_byte & !0o070` is
         * covered); upstream asserts and then hits unreachable_unchecked */
        DBG_ASSERT(false);
        return 0;
    }

    return default_segment == DS ? get_seg_prefix_ds(offset) : get_seg_prefix_ss(offset);
}

int32_t resolve_modrm32(int32_t modrm_byte)
{
    int32_t offset;
    int32_t default_segment = DS;

    switch (modrm_byte & MODRM_MASK) {
    case 0x00: offset = read_reg32(EAX); break;
    case 0x40: {
        int32_t d = read_imm8s();
        if (cpu->pagefault) return 0;
        offset = read_reg32(EAX) + d;
        break;
    }
    case 0x80: {
        int32_t d = read_imm32s();
        if (cpu->pagefault) return 0;
        offset = read_reg32(EAX) + d;
        break;
    }
    case 0x01: offset = read_reg32(ECX); break;
    case 0x41: {
        int32_t d = read_imm8s();
        if (cpu->pagefault) return 0;
        offset = read_reg32(ECX) + d;
        break;
    }
    case 0x81: {
        int32_t d = read_imm32s();
        if (cpu->pagefault) return 0;
        offset = read_reg32(ECX) + d;
        break;
    }
    case 0x02: offset = read_reg32(EDX); break;
    case 0x42: {
        int32_t d = read_imm8s();
        if (cpu->pagefault) return 0;
        offset = read_reg32(EDX) + d;
        break;
    }
    case 0x82: {
        int32_t d = read_imm32s();
        if (cpu->pagefault) return 0;
        offset = read_reg32(EDX) + d;
        break;
    }
    case 0x03: offset = read_reg32(EBX); break;
    case 0x43: {
        int32_t d = read_imm8s();
        if (cpu->pagefault) return 0;
        offset = read_reg32(EBX) + d;
        break;
    }
    case 0x83: {
        int32_t d = read_imm32s();
        if (cpu->pagefault) return 0;
        offset = read_reg32(EBX) + d;
        break;
    }
    /* the SIB forms apply their own segment prefix (resolve_sib), so they return before
     * the shared tail below */
    case 0x04: {
        int32_t a = resolve_sib(false);
        if (cpu->pagefault) return 0;
        return a;
    }
    case 0x44: {
        int32_t a = resolve_sib(true);
        if (cpu->pagefault) return 0;
        int32_t d = read_imm8s();
        if (cpu->pagefault) return 0;
        return a + d;
    }
    case 0x84: {
        int32_t a = resolve_sib(true);
        if (cpu->pagefault) return 0;
        int32_t d = read_imm32s();
        if (cpu->pagefault) return 0;
        return a + d;
    }
    case 0x05: {
        int32_t d = read_imm32s(); /* absolute disp32, default segment DS */
        if (cpu->pagefault) return 0;
        offset = d;
        break;
    }
    case 0x45: {
        int32_t d = read_imm8s();
        if (cpu->pagefault) return 0;
        default_segment = SS;
        offset = read_reg32(EBP) + d;
        break;
    }
    case 0x85: {
        int32_t d = read_imm32s();
        if (cpu->pagefault) return 0;
        default_segment = SS;
        offset = read_reg32(EBP) + d;
        break;
    }
    case 0x06: offset = read_reg32(ESI); break;
    case 0x46: {
        int32_t d = read_imm8s();
        if (cpu->pagefault) return 0;
        offset = read_reg32(ESI) + d;
        break;
    }
    case 0x86: {
        int32_t d = read_imm32s();
        if (cpu->pagefault) return 0;
        offset = read_reg32(ESI) + d;
        break;
    }
    case 0x07: offset = read_reg32(EDI); break;
    case 0x47: {
        int32_t d = read_imm8s();
        if (cpu->pagefault) return 0;
        offset = read_reg32(EDI) + d;
        break;
    }
    case 0x87: {
        int32_t d = read_imm32s();
        if (cpu->pagefault) return 0;
        offset = read_reg32(EDI) + d;
        break;
    }
    default:
        DBG_ASSERT(false);
        return 0;
    }

    return default_segment == DS ? get_seg_prefix_ds(offset) : get_seg_prefix_ss(offset);
}

/* modrm.rs:67-100 */
static int32_t resolve_sib(bool with_imm)
{
    int32_t sib_byte = read_imm8();
    if (cpu->pagefault) {
        return 0;
    }
    int32_t r = sib_byte & 7;
    int32_t m = (sib_byte >> 3) & 7;
    int32_t base;
    int32_t seg;

    if (r == 4) {
        base = read_reg32(ESP);
        seg = SS;
    }
    else if (r == 5) {
        if (with_imm) {
            base = read_reg32(EBP);
            seg = SS;
        }
        else {
            base = read_imm32s();
            if (cpu->pagefault) {
                return 0;
            }
            seg = DS;
        }
    }
    else {
        base = read_reg32(r);
        seg = DS;
    }

    int32_t offset;
    if (m == 4) {
        offset = 0;
    }
    else {
        int32_t s = (sib_byte >> 6) & 3;
        /* upstream `read_reg32(m) << s` on i32; see arith.h's `<<` convention */
        offset = (int32_t)((uint32_t)read_reg32(m) << s);
    }

    int32_t seg_base = get_seg_prefix(seg);
    if (cpu->pagefault) {
        return 0;
    }
    return seg_base + base + offset;
}

/* modrm.rs:37-66 -- upstream-dead (no caller), kept for 1:1 parity */
int32_t resolve_modrm32_(int32_t modrm_byte)
{
    int32_t r = modrm_byte & 7;
    DBG_ASSERT(modrm_byte < 192);

    if (r == 4) {
        if (modrm_byte < 64) {
            return resolve_sib(false);
        }
        else {
            int32_t a = resolve_sib(true);
            if (cpu->pagefault) return 0;
            if (modrm_byte < 128) {
                int32_t d = read_imm8s();
                if (cpu->pagefault) return 0;
                return a + d;
            }
            else {
                int32_t d = read_imm32s();
                if (cpu->pagefault) return 0;
                return a + d;
            }
        }
    }
    else if (r == 5) {
        if (modrm_byte < 64) {
            int32_t d = read_imm32s();
            if (cpu->pagefault) return 0;
            return get_seg_prefix_ds(d);
        }
        else {
            int32_t d = (modrm_byte < 128) ? read_imm8s() : read_imm32s();
            if (cpu->pagefault) return 0;
            return get_seg_prefix_ss(read_reg32(EBP) + d);
        }
    }
    else if (modrm_byte < 64) {
        return get_seg_prefix_ds(read_reg32(r));
    }
    else {
        int32_t d = (modrm_byte < 128) ? read_imm8s() : read_imm32s();
        if (cpu->pagefault) return 0;
        return get_seg_prefix_ds(read_reg32(r) + d);
    }
}

/* cpu.rs:3122-3130 */
int32_t modrm_resolve(int32_t modrm_byte)
{
    if (is_asize_32()) {
        return resolve_modrm32(modrm_byte);
    }
    else {
        return resolve_modrm16(modrm_byte);
    }
}
