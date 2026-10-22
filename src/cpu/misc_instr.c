/*
 * misc_instr.c -- the misc instruction helpers.  Port of v86/src/rust/cpu/misc_instr.rs
 * (613 lines) plus the cpu.rs helpers listed in misc_instr.h.
 *
 * The two functions this file does NOT port are fxsave/fxrstor (M1b's fpu.c, named with
 * their upstream lines in misc_instr.h).  lar/lsl/verr/verw were M1a-2's deferral to
 * M1c's segment-descriptor API and are ported at the BOTTOM of this file (M1c) -- here,
 * where misc_instr.rs has them -- now that exceptions.h provides that API.
 *
 * Two transcription rules worth knowing while reading:
 *   - `Result<(), ()>` / `Result<i32, ()>` become `void` / `int32_t` plus the
 *     cpu->pagefault flag (cpu_state.h): the fault check is placed exactly where
 *     upstream's `?` short-circuits, so the register/state effects on the fault path
 *     match upstream's (usually: no effect at all).
 *   - upstream's `.unwrap()` on a result it has already proved fault-free becomes
 *     DBG_ASSERT(!cpu->pagefault).  In a debug build that is upstream's panic; in a
 *     release build upstream panics and the port continues.  Every such site is on a
 *     pre-checked range (pusha/enter), i.e. unreachable for a guest whose stack fits
 *     in its segment.
 */

#include "misc_instr.h"

#include "exceptions.h" /* the segment-descriptor API + lookup_segment_selector (lar/lsl/verr/verw) */
#include "flags.h" /* the lazy-flag getters the test_* predicates use */
#include "cpu.h"   /* S5a: unimplemented_instruction -- the moffs bound's named stop */
#include "memory.h"
#include "modrm.h"  /* S5a: long_mode_64 / read_moffs64 / PREFIX_MASK_ADDRSIZE */
#include "paging.h"

/* ------------------------------------------------------------------ *
 * condition predicates -- misc_instr.rs:75-92
 * ------------------------------------------------------------------ */

bool test_o(void) { return getof(); }
bool test_b(void) { return getcf(); }
bool test_z(void) { return getzf(); }
bool test_s(void) { return getsf(); }
bool test_p(void) { return getpf(); }
bool test_be(void) { return getcf() || getzf(); }
bool test_l(void) { return getsf() != getof(); }
bool test_le(void) { return getzf() || getsf() != getof(); }
bool test_no(void) { return !test_o(); }
bool test_nb(void) { return !test_b(); }
bool test_nz(void) { return !test_z(); }
bool test_ns(void) { return !test_s(); }
bool test_np(void) { return !test_p(); }
bool test_nbe(void) { return !test_be(); }
bool test_nl(void) { return !test_l(); }
bool test_nle(void) { return !test_le(); }

/* ------------------------------------------------------------------ *
 * jumps, loops, cmov, setcc -- misc_instr.rs:94-131, 364-367
 * ------------------------------------------------------------------ */

void jmp_rel16(int32_t rel16)
{
    uint64_t cs_offset = get_seg_cs();
    /* limit ip to 16 bit */
    set_instruction_pointer(cs_offset +
                            ((cpu->instruction_pointer - cs_offset + rel16) & 0xFFFF));
}

void jmpcc16(bool condition, int32_t imm16)
{
    if (condition) {
        jmp_rel16(imm16);
    }
}

void jmpcc32(bool condition, int32_t imm32)
{
    if (condition) {
        set_instruction_pointer(cpu->instruction_pointer + imm32);
    }
}

void loope16(int32_t imm8s) { jmpcc16(decr_ecx_asize(is_asize_32()) != 0 && getzf(), imm8s); }
void loopne16(int32_t imm8s) { jmpcc16(decr_ecx_asize(is_asize_32()) != 0 && !getzf(), imm8s); }
void loop16(int32_t imm8s) { jmpcc16(decr_ecx_asize(is_asize_32()) != 0, imm8s); }
void jcxz16(int32_t imm8s) { jmpcc16(get_reg_asize(ECX) == 0, imm8s); }
void loope32(int32_t imm8s) { jmpcc32(decr_ecx_asize(is_asize_32()) != 0 && getzf(), imm8s); }
void loopne32(int32_t imm8s) { jmpcc32(decr_ecx_asize(is_asize_32()) != 0 && !getzf(), imm8s); }
void loop32(int32_t imm8s) { jmpcc32(decr_ecx_asize(is_asize_32()) != 0, imm8s); }
void jcxz32(int32_t imm8s) { jmpcc32(get_reg_asize(ECX) == 0, imm8s); }

void cmovcc16(bool condition, int32_t value, int32_t r)
{
    if (condition) {
        write_reg16(r, value);
    }
}

void cmovcc32(bool condition, int32_t value, int32_t r)
{
    if (condition) {
        write_reg32(r, value);
    }
}

void setcc_reg(bool condition, int32_t r) { write_reg8(r, condition ? 1 : 0); }

void setcc_mem(bool condition, int32_t addr)
{
    safe_write8(addr, condition ? 1 : 0);
    if (cpu->pagefault) {
        return;
    }
}

/* ------------------------------------------------------------------ *
 * the stack pointer -- misc_instr.rs:133-148, cpu.rs:4296-4320
 * ------------------------------------------------------------------ */

int64_t get_stack_pointer(int32_t offset)
{
    /* THE STACK ADDRESS IS 64 BITS IN LONG MODE (this unit).  MEASURED: the amd64 kernel runs
     * with rsp = 0xFFFFFFFF9BC03F40 -- a kernel VIRTUAL address -- and the old 32-bit
     * projection sent every frame read to 0x9BC03F40, past the end of guest RAM
     * (results/x86-64-stack64.md).  In long mode SS's base is 0 and RSP is the whole 64-bit
     * register, so the sum is carried at full width into the resolver (safe_* take int64_t
     * since the "widen the bodies" unit).  The 16/32-bit forms keep their arithmetic exactly:
     * their 32-bit sum is the low 32 bits of the same expression. */
    if (long_mode_64()) {
        return (int64_t)(get_seg_ss() + read_reg64(ESP) + (uint64_t)(int64_t)offset);
    }
    if (cpu->stack_size_32) {
        return (int32_t)(uint32_t)(get_seg_ss() + read_reg32(ESP) + offset);
    }
    else {
        return (int32_t)(uint32_t)(get_seg_ss() + ((read_reg16(SP) + offset) & 0xFFFF));
    }
}

void adjust_stack_reg(int32_t adjustment)
{
    if (long_mode_64()) {
        write_reg64(ESP, read_reg64(ESP) + (uint64_t)(int64_t)adjustment);
    }
    else if (cpu->stack_size_32) {
        write_reg32(ESP, read_reg32(ESP) + adjustment);
    }
    else {
        write_reg16(SP, read_reg16(SP) + adjustment);
    }
}

/* cpu.rs:4296-4320 */
int64_t get_stack_reg(void)
{
    if (long_mode_64()) {
        return (int64_t)read_reg64(ESP);
    }
    else if (cpu->stack_size_32) {
        return read_reg32(ESP);
    }
    else {
        return read_reg16(SP);
    }
}

void set_stack_reg(int64_t value)
{
    if (long_mode_64()) {
        write_reg64(ESP, (uint64_t)value);
    }
    else if (cpu->stack_size_32) {
        write_reg32(ESP, value);
    }
    else {
        write_reg16(SP, value);
    }
}

int32_t get_reg_asize(int32_t reg)
{
    DBG_ASSERT(reg == ECX || reg == ESI || reg == EDI);
    int32_t r = read_reg32(reg);
    if (is_asize_32()) {
        return r;
    }
    else {
        return r & 0xFFFF;
    }
}

void set_reg_asize(bool addr_size_32, int32_t reg, int32_t value)
{
    DBG_ASSERT(reg == ECX || reg == ESI || reg == EDI);
    if (addr_size_32) {
        write_reg32(reg, value);
    }
    else {
        write_reg16(reg, value);
    }
}

int32_t decr_ecx_asize(bool addr_size_32)
{
    if (addr_size_32) {
        write_reg32(ECX, read_reg32(ECX) - 1);
        return read_reg32(ECX);
    }
    else {
        write_reg16(CX, read_reg16(CX) - 1);
        return read_reg16(CX);
    }
}

/* cpu.rs:4300-4304 -- added by M1a-3.
 *
 * The 'real' instruction pointer, without the segment offset.  It sits immediately
 * before get_stack_reg upstream and is the cpu.rs-helper family this file already owns
 * (get_stack_reg/set_stack_reg/read_moffs); its callers here are instructions.c's call/
 * ret/ff-group and M1c's cpu.c (far_jump, call_interrupt_vector, do_task_switch). */
int32_t get_real_eip(void)
{
    /* S3: the difference of two widened values, projected at the return type -- the eip
     * WITHOUT its segment base is a 32-bit offset in every mode this port can run (a
     * long-mode RIP-relative offset is still the difference, and S7 owns the 64-bit form). */
    return (int32_t)(uint32_t)(cpu->instruction_pointer - get_seg_cs());
}

/* cpu.rs:4292-4300 */
int32_t read_moffs(void)
{
    /* S5a: THE TWO LONG-MODE ARMS, and they are NOT the ones the 32-bit test would pick.
     * In 64-bit mode an moffs OFFSET is EIGHT bytes wide (MEASURED: `A3` + 8 offset bytes is
     * `movabs ds:0x100230,eax` -- the operand is 32-bit, the OFFSET is not); a 0x67
     * address-size override narrows it to a zero-extended 32.  `is_asize_32()` answers
     * `true` for a 64-bit segment WITHOUT 0x67 (CS.D is 0 there), so reusing it would read a
     * 2-byte offset and desynchronise.  The truncation guard is the port's physical bound
     * (plan ss5.1) applied to the RETURN TYPE: this function returns int32_t because every
     * 32-bit caller is an int32_t address, and a 64-bit offset that does not fit is a NAMED
     * STOP here rather than a silent wrap -- the 0x2NN arms read the full 64 bits through
     * read_moffs64() and never come through this function. */
    if (long_mode_64()) {
        if ((cpu->prefixes & PREFIX_MASK_ADDRSIZE) != 0) {
            return read_imm32s();
        }
        int64_t off = read_moffs64();
        if (cpu->pagefault) return 0;
        if ((uint64_t)off > 0xFFFFFFFFull) {
            unimplemented_instruction(
                "moffs: a 64-bit offset above the port's 32-bit physical model in a 32-bit"
                " operand form (results/x86-64-plan.md ss5.1)");
            return 0;
        }
        return (int32_t)(uint32_t)off;
    }
    /* read 2 or 4 bytes from ip, depending on the address-size attribute */
    if (is_asize_32()) {
        return read_imm32s();
    }
    else {
        return read_imm16();
    }
}

/* ------------------------------------------------------------------ *
 * push / pop -- misc_instr.rs:150-280
 * ------------------------------------------------------------------ */

void push16_ss16(int32_t imm16)
{
    int32_t sp = get_seg_ss() + ((read_reg16(SP) - 2) & 0xFFFF);
    safe_write16(sp, imm16);
    if (cpu->pagefault) {
        return; /* SP is untouched, as upstream's `?` does */
    }
    write_reg16(SP, read_reg16(SP) - 2);
}

void push16_ss32(int32_t imm16)
{
    int32_t sp = get_seg_ss() + read_reg32(ESP) - 2;
    safe_write16(sp, imm16);
    if (cpu->pagefault) {
        return;
    }
    write_reg32(ESP, read_reg32(ESP) - 2);
}

void push16_ss16_mem(int32_t addr)
{
    int32_t v = safe_read16(addr);
    if (cpu->pagefault) {
        return;
    }
    push16_ss16(v);
}

void push16_ss32_mem(int32_t addr)
{
    int32_t v = safe_read16(addr);
    if (cpu->pagefault) {
        return;
    }
    push16_ss32(v);
}

void push16(int32_t imm16)
{
    if (cpu->stack_size_32) {
        push16_ss32(imm16);
    }
    else {
        push16_ss16(imm16);
    }
}

void push32_ss16(int32_t imm32)
{
    int32_t new_sp = (read_reg16(SP) - 4) & 0xFFFF;
    safe_write32(get_seg_ss() + new_sp, imm32);
    if (cpu->pagefault) {
        return;
    }
    write_reg16(SP, new_sp);
}

void push32_ss32(int32_t imm32)
{
    int32_t new_esp = read_reg32(ESP) - 4;
    safe_write32(get_seg_ss() + new_esp, imm32);
    if (cpu->pagefault) {
        return;
    }
    write_reg32(ESP, new_esp);
}

void push32_ss16_mem(int32_t addr)
{
    int32_t v = safe_read32s(addr);
    if (cpu->pagefault) {
        return;
    }
    push32_ss16(v);
}

void push32_ss32_mem(int32_t addr)
{
    int32_t v = safe_read32s(addr);
    if (cpu->pagefault) {
        return;
    }
    push32_ss32(v);
}

void push32(int32_t imm32)
{
    if (cpu->stack_size_32) {
        push32_ss32(imm32);
    }
    else {
        push32_ss16(imm32);
    }
}

void push32_sreg(int32_t i)
{
    /* you can't make this up ... : a segment push stores 16 bits but moves SP by 4 */
    if (cpu->stack_size_32) {
        int32_t new_esp = read_reg32(ESP) - 4;
        safe_write16(get_seg_ss() + new_esp, (int32_t)cpu->sreg[i]);
        if (cpu->pagefault) {
            return;
        }
        write_reg32(ESP, new_esp);
    }
    else {
        int32_t new_sp = (read_reg16(SP) - 4) & 0xFFFF;
        safe_write16(get_seg_ss() + new_sp, (int32_t)cpu->sreg[i]);
        if (cpu->pagefault) {
            return;
        }
        write_reg16(SP, new_sp);
    }
}

int32_t pop16(void)
{
    if (cpu->stack_size_32) {
        return pop16_ss32();
    }
    else {
        return pop16_ss16();
    }
}

int32_t pop16_ss16(void)
{
    int32_t sp = get_seg_ss() + read_reg16(SP);
    int32_t result = safe_read16(sp);
    if (cpu->pagefault) {
        return 0;
    }
    write_reg16(SP, read_reg16(SP) + 2);
    return result;
}

int32_t pop16_ss32(void)
{
    int32_t esp = get_seg_ss() + read_reg32(ESP);
    int32_t result = safe_read16(esp);
    if (cpu->pagefault) {
        return 0;
    }
    write_reg32(ESP, read_reg32(ESP) + 2);
    return result;
}

int32_t pop32s(void)
{
    if (cpu->stack_size_32) {
        return pop32s_ss32();
    }
    else {
        return pop32s_ss16();
    }
}

int32_t pop32s_ss16(void)
{
    int32_t sp = read_reg16(SP);
    int32_t result = safe_read32s(get_seg_ss() + sp);
    if (cpu->pagefault) {
        return 0;
    }
    write_reg16(SP, sp + 4);
    return result;
}

int32_t pop32s_ss32(void)
{
    int32_t esp = read_reg32(ESP);
    int32_t result = safe_read32s(get_seg_ss() + esp);
    if (cpu->pagefault) {
        return 0;
    }
    write_reg32(ESP, read_reg32(ESP) + 4);
    return result;
}

void pusha16(void)
{
    int32_t temp = read_reg16(SP);
    /* make sure we don't get a pagefault after having pushed several registers already */
    writable_or_pagefault(get_stack_pointer(-16), 16);
    if (cpu->pagefault) {
        return;
    }
    push16(read_reg16(AX));
    DBG_ASSERT(!cpu->pagefault);
    push16(read_reg16(CX));
    DBG_ASSERT(!cpu->pagefault);
    push16(read_reg16(DX));
    DBG_ASSERT(!cpu->pagefault);
    push16(read_reg16(BX));
    DBG_ASSERT(!cpu->pagefault);
    push16(temp);
    DBG_ASSERT(!cpu->pagefault);
    push16(read_reg16(BP));
    DBG_ASSERT(!cpu->pagefault);
    push16(read_reg16(SI));
    DBG_ASSERT(!cpu->pagefault);
    push16(read_reg16(DI));
    DBG_ASSERT(!cpu->pagefault);
}

void pusha32(void)
{
    int32_t temp = read_reg32(ESP);
    writable_or_pagefault(get_stack_pointer(-32), 32);
    if (cpu->pagefault) {
        return;
    }
    push32(read_reg32(EAX));
    DBG_ASSERT(!cpu->pagefault);
    push32(read_reg32(ECX));
    DBG_ASSERT(!cpu->pagefault);
    push32(read_reg32(EDX));
    DBG_ASSERT(!cpu->pagefault);
    push32(read_reg32(EBX));
    DBG_ASSERT(!cpu->pagefault);
    push32(temp);
    DBG_ASSERT(!cpu->pagefault);
    push32(read_reg32(EBP));
    DBG_ASSERT(!cpu->pagefault);
    push32(read_reg32(ESI));
    DBG_ASSERT(!cpu->pagefault);
    push32(read_reg32(EDI));
    DBG_ASSERT(!cpu->pagefault);
}

/* cpu.rs:3054-3078 -- popa16/popa32, added by M1a-3.
 *
 * Upstream keeps the popa pair in cpu.rs (not misc_instr.rs, where pusha lives), and
 * M1a-2's port covered pusha but not popa, so instructions.c's instr16_61/instr32_61
 * (its first callers anywhere in the port) found them missing.  Ported here, next to
 * pusha, in the same style: the whole 16/32-byte range is pre-checked, then each pop is
 * asserted fault-free (upstream's `.unwrap()`), so a mid-sequence fault cannot half-update
 * the register file. */
void popa16(void)
{
    readable_or_pagefault(get_stack_pointer(0), 16);
    if (cpu->pagefault) {
        return;
    }

    int32_t v = pop16();
    DBG_ASSERT(!cpu->pagefault);
    write_reg16(DI, v);
    v = pop16();
    DBG_ASSERT(!cpu->pagefault);
    write_reg16(SI, v);
    v = pop16();
    DBG_ASSERT(!cpu->pagefault);
    write_reg16(BP, v);
    adjust_stack_reg(2);
    v = pop16();
    DBG_ASSERT(!cpu->pagefault);
    write_reg16(BX, v);
    v = pop16();
    DBG_ASSERT(!cpu->pagefault);
    write_reg16(DX, v);
    v = pop16();
    DBG_ASSERT(!cpu->pagefault);
    write_reg16(CX, v);
    v = pop16();
    DBG_ASSERT(!cpu->pagefault);
    write_reg16(AX, v);
}

void popa32(void)
{
    readable_or_pagefault(get_stack_pointer(0), 32);
    if (cpu->pagefault) {
        return;
    }

    int32_t v = pop32s();
    DBG_ASSERT(!cpu->pagefault);
    write_reg32(EDI, v);
    v = pop32s();
    DBG_ASSERT(!cpu->pagefault);
    write_reg32(ESI, v);
    v = pop32s();
    DBG_ASSERT(!cpu->pagefault);
    write_reg32(EBP, v);
    adjust_stack_reg(4);
    v = pop32s();
    DBG_ASSERT(!cpu->pagefault);
    write_reg32(EBX, v);
    v = pop32s();
    DBG_ASSERT(!cpu->pagefault);
    write_reg32(EDX, v);
    v = pop32s();
    DBG_ASSERT(!cpu->pagefault);
    write_reg32(ECX, v);
    v = pop32s();
    DBG_ASSERT(!cpu->pagefault);
    write_reg32(EAX, v);
}

/* ------------------------------------------------------------------ *
 * lss / enter -- misc_instr.rs:282-362
 * ------------------------------------------------------------------ */

void lss16(int32_t addr, int32_t reg, int32_t seg)
{
    int32_t new_reg = safe_read16(addr);
    if (cpu->pagefault) {
        return;
    }
    int32_t new_seg = safe_read16(addr + 2);
    if (cpu->pagefault) {
        return;
    }
    if (!switch_seg(seg, new_seg)) {
        return;
    }
    write_reg16(reg, new_reg);
}

void lss32(int32_t addr, int32_t reg, int32_t seg)
{
    int32_t new_reg = safe_read32s(addr);
    if (cpu->pagefault) {
        return;
    }
    int32_t new_seg = safe_read16(addr + 4);
    if (cpu->pagefault) {
        return;
    }
    if (!switch_seg(seg, new_seg)) {
        return;
    }
    write_reg32(reg, new_reg);
}

void enter16(int32_t size, int32_t nesting_level)
{
    nesting_level &= 31;

    int32_t ss_mask = cpu->stack_size_32 ? -1 : 0xFFFF;
    int32_t ss = get_seg_ss();
    int32_t frame_temp = read_reg32(ESP) - 2;

    if (nesting_level > 0) {
        int32_t tmp_ebp = read_reg32(EBP);
        for (int32_t i = 1; i < nesting_level; i++) {
            tmp_ebp -= 2;
            int32_t v = safe_read16(ss + (tmp_ebp & ss_mask));
            DBG_ASSERT(!cpu->pagefault); /* upstream's .unwrap() */
            push16(v);
            DBG_ASSERT(!cpu->pagefault);
        }
        push16(frame_temp);
        DBG_ASSERT(!cpu->pagefault);
    }

    safe_write16(ss + (frame_temp & ss_mask), read_reg16(BP));
    if (cpu->pagefault) {
        return;
    }
    write_reg16(BP, frame_temp);
    adjust_stack_reg(-size - 2);
}

void enter32(int32_t size, int32_t nesting_level)
{
    nesting_level &= 31;

    int32_t ss_mask = cpu->stack_size_32 ? -1 : 0xFFFF;
    int32_t ss = get_seg_ss();
    int32_t frame_temp = read_reg32(ESP) - 4;

    if (nesting_level > 0) {
        int32_t tmp_ebp = read_reg32(EBP);
        for (int32_t i = 1; i < nesting_level; i++) {
            tmp_ebp -= 4;
            int32_t v = safe_read32s(ss + (tmp_ebp & ss_mask));
            DBG_ASSERT(!cpu->pagefault);
            push32(v);
            DBG_ASSERT(!cpu->pagefault);
        }
        push32(frame_temp);
        DBG_ASSERT(!cpu->pagefault);
    }

    safe_write32(ss + (frame_temp & ss_mask), read_reg32(EBP));
    if (cpu->pagefault) {
        return;
    }
    write_reg32(EBP, frame_temp);
    adjust_stack_reg(-size - 4);
}

/* ------------------------------------------------------------------ *
 * xchg / bswap -- misc_instr.rs:430-456
 * ------------------------------------------------------------------ */

int32_t xchg8(int32_t data, int32_t r8)
{
    int32_t tmp = read_reg8(r8);
    write_reg8(r8, data);
    return tmp;
}

int32_t xchg16(int32_t data, int32_t r16)
{
    int32_t tmp = read_reg16(r16);
    write_reg16(r16, data);
    return tmp;
}

void xchg16r(int32_t r16)
{
    int32_t tmp = read_reg16(AX);
    write_reg16(AX, read_reg16(r16));
    write_reg16(r16, tmp);
}

int32_t xchg32(int32_t data, int32_t r32)
{
    int32_t tmp = read_reg32(r32);
    write_reg32(r32, data);
    return tmp;
}

void xchg32r(int32_t r32)
{
    int32_t tmp = read_reg32(EAX);
    write_reg32(EAX, read_reg32(r32));
    write_reg32(r32, tmp);
}

void bswap(int32_t r) { write_reg32(r, (int32_t)__builtin_bswap32((uint32_t)read_reg32(r))); }

/* ------------------------------------------------------------------ *
 * the CR0/CR4 tests and MXCSR -- cpu.rs:4210-4275
 * ------------------------------------------------------------------ */

bool task_switch_test(void)
{
    if ((cpu->cr[0] & (CR0_EM | CR0_TS)) != 0) {
        trigger_nm();
        return false;
    }
    else {
        return true;
    }
}

bool task_switch_test_mmx(void)
{
    /* upstream dbg_logs a warning when CR4.OSFXSR is clear; the log channel does not
     * exist in this port, so only the behavioural part below is kept */
    if ((cpu->cr[0] & CR0_EM) != 0) {
        trigger_ud();
        return false;
    }
    else if ((cpu->cr[0] & CR0_TS) != 0) {
        trigger_nm();
        return false;
    }
    else {
        return true;
    }
}

void set_mxcsr(int32_t new_mxcsr)
{
    DBG_ASSERT((new_mxcsr & ~MXCSR_MASK) == 0); /* checked by caller */
    /* the four "Unimplemented MXCSR ..." dbg_log! warnings are dropped; the assignment
     * is the only effect upstream has */
    cpu->mxcsr = new_mxcsr;
}

/* ------------------------------------------------------------------ *
 * lar / lsl / verr / verw -- misc_instr.rs:458-613
 *
 * Deferred to M1c by M1a-2 for the reason misc_instr.h's header note records: every one
 * of them is defined over cpu.rs's segment-descriptor API and lookup_segment_selector,
 * which exceptions.h now provides.  Ported HERE because this is the file upstream keeps
 * them in.
 *
 * THE FLAG PATTERN, upstream's exactly: the ZERO bit is first marked MATERIALIZED in
 * flags_changed (`&= !FLAG_ZERO`), then set or cleared in `flags` -- the opposite order
 * would let the lazy getter recompute a stale ZF over the written value.
 * ------------------------------------------------------------------ */

/* misc_instr.rs:461-463 -- 1 << type for the system types on which the selector is
 * invalid for LAR (0, 6, 7, 8, 0xA, 0xD, 0xE, 0xF). */
#define LAR_INVALID_TYPE                                                      \
    ((uint32_t)(1u << 0 | 1u << 6 | 1u << 7 | 1u << 8 | 1u << 0xA | 1u << 0xD | \
                1u << 0xE | 1u << 0xF))

/* misc_instr.rs:508-518 -- the same for LSL (0, 4, 5, 6, 7, 8, 0xA, 0xC, 0xD, 0xE, 0xF). */
#define LSL_INVALID_TYPE                                                       \
    ((uint32_t)(1u << 0 | 1u << 4 | 1u << 5 | 1u << 6 | 1u << 7 | 1u << 8 |    \
                1u << 0xA | 1u << 0xC | 1u << 0xD | 1u << 0xE | 1u << 0xF))

/* The dpl test the two share: "lar: invalid" / "lsl: invalid" (misc_instr.rs:480). */
static bool lar_lsl_dpl_bad(seg_descriptor_t desc, seg_selector_t sel)
{
    return seg_descriptor_dpl(desc) < cpu->cpl || seg_descriptor_dpl(desc) < seg_selector_rpl(sel);
}

int32_t lar(int32_t selector, int32_t original)
{
    seg_selector_t sel = seg_selector_of_u16((uint16_t)selector);
    seg_descriptor_t desc;
    int32_t descriptor_address;
    selector_lookup_t lookup = lookup_segment_selector(sel, &desc, &descriptor_address);
    if (cpu->pagefault) {
        /* pagefault -- the instruction returns with the original value */
        return original;
    }

    if (lookup != SELECTOR_LOOKUP_OK) {
        cpu->flags_changed &= ~FLAG_ZERO;
        cpu->flags &= ~FLAG_ZERO;
        return original;
    }

    cpu->flags_changed &= ~FLAG_ZERO;
    bool dpl_bad = lar_lsl_dpl_bad(desc, sel);

    bool invalid;
    if (seg_descriptor_is_system(desc)) {
        invalid = ((LAR_INVALID_TYPE >> seg_descriptor_system_type(desc)) & 1) == 1 || dpl_bad;
    }
    else {
        invalid = !seg_descriptor_is_conforming_executable(desc) && dpl_bad;
    }

    if (invalid) {
        cpu->flags &= ~FLAG_ZERO;
        return original;
    }
    else {
        cpu->flags |= FLAG_ZERO;
        return (int32_t)((desc.raw >> 32) & 0x00FFFF00);
    }
}

int32_t lsl(int32_t selector, int32_t original)
{
    seg_selector_t sel = seg_selector_of_u16((uint16_t)selector);
    seg_descriptor_t desc;
    int32_t descriptor_address;
    selector_lookup_t lookup = lookup_segment_selector(sel, &desc, &descriptor_address);
    if (cpu->pagefault) {
        /* pagefault */
        return original;
    }

    if (lookup != SELECTOR_LOOKUP_OK) {
        cpu->flags_changed &= ~FLAG_ZERO;
        cpu->flags &= ~FLAG_ZERO;
        return original;
    }

    cpu->flags_changed &= ~FLAG_ZERO;
    bool dpl_bad = lar_lsl_dpl_bad(desc, sel);

    bool invalid;
    if (seg_descriptor_is_system(desc)) {
        invalid = ((LSL_INVALID_TYPE >> seg_descriptor_system_type(desc)) & 1) == 1 || dpl_bad;
    }
    else {
        invalid = !seg_descriptor_is_conforming_executable(desc) && dpl_bad;
    }

    if (invalid) {
        cpu->flags &= ~FLAG_ZERO;
        return original;
    }
    else {
        cpu->flags |= FLAG_ZERO;
        return (int32_t)seg_descriptor_effective_limit(desc);
    }
}

void verr(int32_t selector)
{
    cpu->flags_changed &= ~FLAG_ZERO;
    seg_selector_t sel = seg_selector_of_u16((uint16_t)selector);
    seg_descriptor_t desc;
    int32_t descriptor_address;
    selector_lookup_t lookup = lookup_segment_selector(sel, &desc, &descriptor_address);
    if (cpu->pagefault) {
        return;
    }

    if (lookup != SELECTOR_LOOKUP_OK) {
        cpu->flags &= ~FLAG_ZERO;
    }
    else {
        if (seg_descriptor_is_system(desc) || !seg_descriptor_is_readable(desc) ||
            (!seg_descriptor_is_conforming_executable(desc) &&
             (seg_descriptor_dpl(desc) < cpu->cpl || seg_descriptor_dpl(desc) < seg_selector_rpl(sel)))) {
            cpu->flags &= ~FLAG_ZERO;
        }
        else {
            cpu->flags |= FLAG_ZERO;
        }
    }
}

void verw(int32_t selector)
{
    cpu->flags_changed &= ~FLAG_ZERO;
    seg_selector_t sel = seg_selector_of_u16((uint16_t)selector);
    seg_descriptor_t desc;
    int32_t descriptor_address;
    selector_lookup_t lookup = lookup_segment_selector(sel, &desc, &descriptor_address);
    if (cpu->pagefault) {
        return;
    }

    if (lookup != SELECTOR_LOOKUP_OK) {
        cpu->flags &= ~FLAG_ZERO;
    }
    else {
        if (seg_descriptor_is_system(desc) || !seg_descriptor_is_writable(desc) ||
            seg_descriptor_dpl(desc) < cpu->cpl || seg_descriptor_dpl(desc) < seg_selector_rpl(sel)) {
            cpu->flags &= ~FLAG_ZERO;
        }
        else {
            cpu->flags |= FLAG_ZERO;
        }
    }
}
