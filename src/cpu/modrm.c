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
    /* S5a: THE 32-BIT-ARM ENTRY POINT IS MODE-AWARE, and it has to be.  Every modrm-bearing
     * instruction in long mode generates its address with 64-BIT rules (RIP-relative, a 64-bit
     * SIB base, a full-width displacement) -- INCLUDING the instructions with no REX.W, which
     * dispatch to the 0x1NN/0xNN arms.  `movdqa xmm0, [rip+disp]` (66 0F 6F, no REX) or `push
     * qword [mem]` (FF /6 with REX, whose arm is S7's) land here, and computing their address
     * with the 32-bit resolver turns RIP-relative into DISP32-ABSOLUTE: a valid-looking address
     * in the wrong page, i.e. the silent-wrong class again.
     *
     * MEASURED as the reason this exists: before it, four corpus cases that use only
     * no-REX.W encodings (`simd-movdqa-64`, `simd-fpu-mmx-64`, `simd-fild-fistp64`,
     * `ctrl-push-pop-64`) reported MISMATCHES instead of the named stops their slices should
     * produce -- the bodies ran and read the wrong addresses.
     *
     * THE BOUND, stated: this returns int32_t because every 32-bit-arm body takes an int32_t
     * address.  In long mode the resolver's answer is 64-bit, so a linear address above 4 GiB
     * STOPS BY NAME instead of being truncated (S7/S10 widen the bodies; the port's physical
     * model is 32 bits, so the address bound is the model's own -- plan ss5.1).  The 0x2NN
     * arms call modrm_resolve64 directly and are not affected by the bound. */
    if (long_mode_64()) {
        int64_t a = modrm_resolve64(modrm_byte);
        if (cpu->pagefault) return 0;
        if ((uint64_t)a > 0xFFFFFFFFull) {
            unimplemented_instruction(
                "modrm: a 64-bit effective address above 4 GiB in a 32-bit-operand form"
                " (S6-S10 widen the bodies; results/x86-64-plan.md ss5.1)");
            return 0;
        }
        return (int32_t)(uint32_t)a;
    }
    if (is_asize_32()) {
        return resolve_modrm32(modrm_byte);
    }
    else {
        return resolve_modrm16(modrm_byte);
    }
}

/* ================================================================== *
 * S5a -- THE 64-BIT DECODE SIDE (plan ss3.6.1's three-valued key, ss3.6.5's
 * read_imm64, and plan correction W1's 64-bit address generator)
 *
 * Everything in this section is reachable ONLY when the machine is in IA-32e 64-BIT mode
 * (`long_mode_64()`: EFER.LMA and CS.L), which outside `--long-mode on` never happens, and
 * each function keeps its 32-bit twin untouched beside it.  That is the S5a staging rule: the
 * 32-bit machine dispatches to exactly the cases it always did.
 * ================================================================== */

/* ------------------------------------------------------------------ *
 * the three-valued dispatch key
 * ------------------------------------------------------------------ */

int32_t dispatch_os_index(int32_t opcode)
{
    if (long_mode_64()) {
        /* 0x40-0x4F ARE REX IN 64-BIT MODE.  They are dispatched as OPCODES -- the fetch loop
         * takes one byte at a time and the prefix is only recognised when its own byte comes
         * up -- so the byte must reach its own case, and it must reach it whichever legacy
         * prefix preceded it.  The overlay emits exactly that one arm (16 REX cases), and this
         * rule is what selects it.  Without this line the byte would be an INC/DEC at
         * os_index 0 or 1 and the instruction LENGTH of everything after it would be wrong. */
        if (opcode >= 0x40 && opcode <= 0x4F) return 2;
        /* the operand size: REX.W wins, then the 0x66 override (16-bit), then the encodings
         * whose long-mode default is 64, then the 32-bit default */
        if ((cpu->prefixes & PREFIX_REX_W) != 0) return 2;
        if ((cpu->prefixes & PREFIX_MASK_OPSIZE) != 0) return 0;
        if (long_mode_default64(opcode)) return 2;
        return 1;
    }
    /* compatibility mode and every 32-bit mode: the two arms the machine has always had.  Note
     * that `is_osize_32()` is deliberately NOT used in the long-mode branch above -- in a
     * 64-bit code segment CS.D is 0, so is_osize_32() answers `true` there and would route
     * every instruction to the 32-bit arm. */
    return is_osize_32() ? 1 : 0;
}

/* ------------------------------------------------------------------ *
 * the 8-byte immediate and the moffs offset
 * ------------------------------------------------------------------ */

int64_t read_imm64(void)
{
    /* LOW HALF FIRST: the stream's byte order is semantic (gen/interpreter_c.js's note on the
     * hoisted immediate).  Each half goes through read_imm32s, so the page-crossing handling is
     * the one the port already has. */
    uint32_t low = (uint32_t)read_imm32s();
    if (cpu->pagefault) return 0;
    uint32_t high = (uint32_t)read_imm32s();
    return (int64_t)((uint64_t)low | ((uint64_t)high << 32));
}

int64_t read_moffs64(void)
{
    /* an moffs OFFSET is unsigned and 8 bytes wide in 64-bit mode whatever the operand size
     * is; the operand (AL/AX/EAX/RAX) is chosen by the ARM, not by this read */
    uint64_t low = (uint32_t)read_imm32s();
    if (cpu->pagefault) return 0;
    uint64_t high = (uint32_t)read_imm32s();
    return (int64_t)(low | (high << 32));
}

/* ------------------------------------------------------------------ *
 * the 64-bit effective address -- see modrm.h for what it adds
 * ------------------------------------------------------------------ */

int64_t modrm_resolve64(int32_t modrm_byte)
{
    /* 0x67 ADDRESS-SIZE OVERRIDE: a 32-bit effective address, ZERO-EXTENDED.  Computed by the
     * 32-bit resolver so the wrap is the model's own (not a truncation of a 64-bit sum), which
     * is what the hardware does. */
    if ((cpu->prefixes & PREFIX_MASK_ADDRSIZE) != 0) {
        int32_t a = resolve_modrm32(modrm_byte);
        if (cpu->pagefault) return 0;
        return (int64_t)(uint64_t)(uint32_t)a;
    }

    int32_t mod = modrm_byte >> 6 & 3;
    int32_t r = modrm_byte & 7;
    int32_t default_segment = DS;
    int64_t offset = 0;
    bool rip_relative = false;

    DBG_ASSERT(mod != 3); /* the caller's `modrm_byte < 0xC0` test; a register form has no address */

    if (r == 4) {
        /* SIB */
        int32_t sib_byte = read_imm8();
        if (cpu->pagefault) return 0;
        int32_t base_i = sib_byte & 7;
        int32_t index_i = (sib_byte >> 3) & 7;
        int32_t scale = (sib_byte >> 6) & 3;

        if (mod == 0 && base_i == 5) {
            /* no base register: a sign-extended disp32 */
            int32_t d = read_imm32s();
            if (cpu->pagefault) return 0;
            offset = (int64_t)d;
        }
        else {
            offset = (int64_t)read_reg64(reg_index_b(base_i));
            /* a base of (E/R)SP or (E/R)BP selects SS in the reference's table; segment bases
             * are 0 in 64-bit mode except FS/GS, so this only decides which override applies */
            if (base_i == 4 || base_i == 5) default_segment = SS;
        }

        /* index 100b is "no index" ONLY without REX.X: with it, the index register is R12 */
        if (!(index_i == 4 && (cpu->prefixes & PREFIX_REX_X) == 0)) {
            offset += (int64_t)read_reg64(reg_index_x(index_i)) << scale;
        }

        if (mod == 1) {
            int32_t d = read_imm8s();
            if (cpu->pagefault) return 0;
            offset += (int64_t)d;
        }
        else if (mod == 2) {
            int32_t d = read_imm32s();
            if (cpu->pagefault) return 0;
            offset += (int64_t)d;
        }
    }
    else if (mod == 0 && r == 5) {
        /* RIP-RELATIVE: the base is the address of the NEXT instruction, i.e. the cursor AFTER
         * the displacement has been consumed -- which is why the read comes first. */
        int32_t d = read_imm32s();
        if (cpu->pagefault) return 0;
        offset = (int64_t)cpu->instruction_pointer + (int64_t)d;
        rip_relative = true;
    }
    else {
        offset = (int64_t)read_reg64(reg_index_b(r));
        if (mod == 0) {
            /* nothing to add */
        }
        else if (mod == 1) {
            int32_t d = read_imm8s();
            if (cpu->pagefault) return 0;
            offset += (int64_t)d;
        }
        else {
            int32_t d = read_imm32s();
            if (cpu->pagefault) return 0;
            offset += (int64_t)d;
        }
        if (r == 5) default_segment = SS; /* (E/R)BP-based */
    }

    if (rip_relative) {
        /* RIP-relative addresses do not take a segment base (DS is implied) */
        return offset;
    }

    /* THE SEGMENT BASE, at 64 bits.  In 64-bit mode CS/DS/ES/SS bases are 0; FS/GS keep real
     * 64-bit bases, which the port reads out of segment_offsets[] here (the MSRs that WRITE
     * them are S11's, so a base above 4 GiB is that slice's gap rather than a silent wrap of
     * this add). */
    int32_t seg = segment_prefix(default_segment);
    int64_t base = (int64_t)cpu->segment_offsets[seg];
    return base + offset;
}
