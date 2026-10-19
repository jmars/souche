/*
 * instructions.c -- the BASE opcode table: the hand-written semantics the generated
 * dispatcher calls by name.  Port of v86/src/rust/cpu/instructions.rs (2504 lines).
 *
 * THE NAMING CONTRACT (the reason this file exists in this shape).  Every function below
 * is named by v86/gen/generate_interpreter.js:87-101 `make_instruction_name`, kept
 * verbatim:
 *
 *     `${module}::instr${os ? size : ""}_${second_prefix}${first_prefix}${opcode_hex}${fixed_g}`
 *
 * with size in {16,32} (the operand-size prefix case of the table), first_prefix in
 * {"", "0F", "F2", "F3"} (opcode bits 8-15), second_prefix in {"", "66", "F2", "F3"}
 * (bits 16-23) and `_${g}` for the fixed_g (modrm reg-field) groups.  For the BASE table
 * the module is `instructions` and no encoding has a 0F or 66 prefix -- those are
 * instructions_0f.c's (M1b) and the only M1b-side names carry 0F/66/660F -- so the C
 * symbol is the upstream Rust name with the module path stripped:
 *
 *     instructions::instr16_F3A5   ->   instr16_F3A5
 *
 * The modrm split is `_mem(addr, r[, imm])` / `_reg(r1, r[, imm])` (the generator emits
 * `modrm_byte < 0xC0` -> `_mem`, else `_reg`), and the `_mem` address comes from
 * `modrm_resolve` (the generator emits the fault check around it).  Every function is
 * therefore `void` and every fallible call sets cpu->pagefault (cpu_state.h) -- the
 * emitter prints `if (cpu->pagefault) return;` after each of them.
 *
 * THE D8..DF GROUP used to be absent here (M1a-3 deferred it: every one of the 160
 * functions is a thin call into fpu.rs, and their arguments are the F80/softfloat types,
 * so porting them HERE would have frozen fpu.c's interface from outside the file that owns
 * it -- the same refusal M1a-2 recorded for lar/lsl/verr/verw).  M1b-3 has since landed
 * both sides: fpu.c defines the operations, and this file's TAIL defines the 160 entry
 * points (and src/cpu/instructions.h declares them).  The group is last in the file, in
 * its own delimited section, so the M1a-3-reviewed integer table stays untouched above it.
 *
 * Porting rules carried in from M1a-2 (results/m1a2-arith-modrm-misc.md, ss5):
 *   - Rust's `<<` on an i32 is defined on the bit pattern; C's is UB once bits reach the
 *     sign bit.  Every such site here shifts on uint32_t (SEX8/SEX16, the cbw/cwd family).
 *   - a Rust `&|x| expr` closure passed to safe_read_write* becomes the statement macro of
 *     paging.h with `x_` bound to the loaded value (SAFE_READ_WRITE8/16/32).
 *   - dbg_log!/dbg_assert!/dbg_trace() -> dropped / DBG_ASSERT (M1a-2's D19); `in_jit` is
 *     JIT-only and dropped; a `let ... = match` that only selects a value is a plain C
 *     if-chain.
 *   - THE 16-BIT ip CLASS (PERF-FIX, closing PERF-3's finding 3): the 14 control transfers
 *     below do NOT carry upstream's `dbg_assert!(*is_32 || value < 0x10000)`
 *     (instructions.rs:1319/1326/1339/1404/1412/1420/2060/2069/2086/2091/2390/2412/2447/
 *     2474).  Those are `debug_assert!`s (dbg.rs:63), COMPILED OUT of the pinned release
 *     reference, which therefore simply executes; the port shipped them live and aborted on
 *     guest bytes the reference runs (MEASURED: `66 E8 24 78 56 34` in a 16-bit code
 *     segment, aborting inside instr32_E8).  The condition is a property of the GUEST's
 *     input -- a 66-prefixed (32-bit operand size) form in a 16-bit code segment -- not an
 *     invariant of the port, so each is dropped AT ITS SITE, exactly as R3's N1 and R4's N3
 *     were, and NOT by -DNDEBUG (which would silence the port's own invariants: the stop
 *     `make nasm`'s `jump` test ends on, src/cpu/cpu.c:83).  Five of the 14 cannot fire at
 *     all -- their operand is a 16-bit read, or jmp_rel16 masks it (instr16_C2, instr16_CB,
 *     instr16_EB, instr16_FF_2_helper, instr16_FF_4_helper) -- so their removal is provably
 *     inert; the other nine are reachable, and the value is USED, not indexed, so the guest
 *     is simply allowed to set a >=64 KiB eip.  Probe: results/perf-fix-n3.sh; the per-site
 *     enumeration: results/perf-fix.md ITEM 2.
 *   - ... AND THE SAME CLASS'S SECOND FORM (PERF-FIX-2, the tree-wide sweep the line above
 *     asked for).  Upstream expresses the identical intent as a bare `dbg_assert!(false)`
 *     under the SAME guest-input guard -- `if !*protected_mode || vm86_mode() { if 0 !=
 *     new_ip as u32 & 0xFFFF0000 { dbg_assert!(false); } }` at instructions.rs:1001-1005
 *     (instr32_9A), 2462-2469 (instr32_FF_3_mem, callf m16:32) and 2490-2494
 *     (instr32_FF_5_mem, jmpf m16:32).  Those are `debug_assert!`s too, so the release
 *     reference far_jumps with the full 32-bit new_ip; the port shipped a live
 *     `DBG_ASSERT(((uint32_t)new_ip & 0xFFFF0000u) == 0)` at each and aborted on ordinary
 *     guest bytes (`66 FF 1E <addr>` in a 16-bit code segment with a >=64 KiB fword).
 *     Dropped at their three sites for the same reason, by the same rule.  Probe:
 *     results/perf-fix-2-probe.sh.  The class's inventory (all 345 DBG_ASSERT sites, one
 *     verdict each) is results/perf-fix-2-sweep.md.
 */

#include "instructions.h"

#include "arith.h"
#include "flags.h"  /* get_eflags/update_eflags/get of the BCD guards */
#include "fpu.h"    /* the D8..DF group's x87 operations (see the file tail) */
#include "memory.h"
#include "misc_instr.h"
#include "modrm.h"
#include "paging.h"
#include "string.h"

/* cpu.rs:3131: `run_instruction(opcode) = gen::interpreter::run(opcode)` -- the generated
 * dispatcher, declared in instructions.h (the emitter unit defines it; the opcode argument
 * is `read_imm8() | is_osize_32() << 8`: the low byte is the opcode, bit 8 the operand
 * size, which is exactly the generator's `0xNN` / `0x1NN` case split). */

/* cpu.rs:3366-3376 -- the prefix re-dispatch.  Upstream keeps both in cpu.rs, but their
 * only callers anywhere in the port are this file's instr_26/2E/36/3E/64/65/66/67/F0/F2/F3
 * (and they need nothing but read_imm8/is_osize_32/run_instruction), so they live here
 * next to those callers.  Declared in instructions.h for M1c's cpu.c. */
void run_prefix_instruction(void)
{
    int32_t opcode = read_imm8();
    if (cpu->pagefault) return;
    run_instruction(opcode | (is_osize_32() ? 1 : 0) << 8);
}

void segment_prefix_op(int32_t seg)
{
    DBG_ASSERT(seg <= 5 && seg >= 0);
    cpu->prefixes = (uint8_t)((cpu->prefixes & ~PREFIX_MASK_SEGMENT) | (uint8_t)(seg + 1));
    run_prefix_instruction();
    cpu->prefixes = 0;
}

/* sign extension without C's signed-shift UB (M1a-2 D5): Rust `x << 24 >> 24` / `<< 16 >> 16` */
#define SEX8(x)  ((int32_t)((uint32_t)(x) << 24) >> 24)
#define SEX16(x) ((int32_t)((uint32_t)(x) << 16) >> 16)

/* ================================================================== *
 * 0x00-0x3F -- the ALU group, the segment prefixes and the BCD ops
 * ================================================================== */

void instr_00_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE8(addr, add8(x_, read_reg8(r))); }
void instr_00_reg(int32_t r1, int32_t r) { write_reg8(r1, add8(read_reg8(r1), read_reg8(r))); }
void instr16_01_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE16(addr, add16(x_, read_reg16(r))); }
void instr16_01_reg(int32_t r1, int32_t r) { write_reg16(r1, add16(read_reg16(r1), read_reg16(r))); }
void instr32_01_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE32(addr, add32(x_, read_reg32(r))); }
void instr32_01_reg(int32_t r1, int32_t r) { write_reg32(r1, add32(read_reg32(r1), read_reg32(r))); }

void instr_02_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read8(addr);
    if (cpu->pagefault) return;
    write_reg8(r, add8(read_reg8(r), x));
}
void instr_02_reg(int32_t r1, int32_t r) { write_reg8(r, add8(read_reg8(r), read_reg8(r1))); }
void instr16_03_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    write_reg16(r, add16(read_reg16(r), x));
}
void instr16_03_reg(int32_t r1, int32_t r) { write_reg16(r, add16(read_reg16(r), read_reg16(r1))); }
void instr32_03_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    write_reg32(r, add32(read_reg32(r), x));
}
void instr32_03_reg(int32_t r1, int32_t r) { write_reg32(r, add32(read_reg32(r), read_reg32(r1))); }

void instr_04(int32_t imm8) { write_reg8(AL, add8(read_reg8(AL), imm8)); }
void instr16_05(int32_t imm16) { write_reg16(AX, add16(read_reg16(AX), imm16)); }
void instr32_05(int32_t imm32) { write_reg32(EAX, add32(read_reg32(EAX), imm32)); }

void instr16_06(void) { push16((int32_t)cpu->sreg[ES]); if (cpu->pagefault) return; }
void instr32_06(void) { push32_sreg(ES); if (cpu->pagefault) return; }

void instr16_07(void)
{
    int32_t selector = safe_read16(get_stack_pointer(0));
    if (cpu->pagefault) return;
    if (!switch_seg(ES, selector)) {
        return;
    }
    adjust_stack_reg(2);
}
void instr32_07(void)
{
    int32_t selector = safe_read32s(get_stack_pointer(0));
    if (cpu->pagefault) return;
    if (!switch_seg(ES, selector & 0xFFFF)) {
        return;
    }
    adjust_stack_reg(4);
}

void instr_08_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE8(addr, or8(x_, read_reg8(r))); }
void instr_08_reg(int32_t r1, int32_t r) { write_reg8(r1, or8(read_reg8(r1), read_reg8(r))); }
void instr16_09_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE16(addr, or16(x_, read_reg16(r))); }
void instr16_09_reg(int32_t r1, int32_t r) { write_reg16(r1, or16(read_reg16(r1), read_reg16(r))); }
void instr32_09_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE32(addr, or32(x_, read_reg32(r))); }
void instr32_09_reg(int32_t r1, int32_t r) { write_reg32(r1, or32(read_reg32(r1), read_reg32(r))); }

void instr_0A_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read8(addr);
    if (cpu->pagefault) return;
    write_reg8(r, or8(read_reg8(r), x));
}
void instr_0A_reg(int32_t r1, int32_t r) { write_reg8(r, or8(read_reg8(r), read_reg8(r1))); }
void instr16_0B_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    write_reg16(r, or16(read_reg16(r), x));
}
void instr16_0B_reg(int32_t r1, int32_t r) { write_reg16(r, or16(read_reg16(r), read_reg16(r1))); }
void instr32_0B_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    write_reg32(r, or32(read_reg32(r), x));
}
void instr32_0B_reg(int32_t r1, int32_t r) { write_reg32(r, or32(read_reg32(r), read_reg32(r1))); }

void instr_0C(int32_t imm8) { write_reg8(AL, or8(read_reg8(AL), imm8)); }
void instr16_0D(int32_t imm16) { write_reg16(AX, or16(read_reg16(AX), imm16)); }
void instr32_0D(int32_t imm32) { write_reg32(EAX, or32(read_reg32(EAX), imm32)); }

void instr16_0E(void) { push16((int32_t)cpu->sreg[CS]); if (cpu->pagefault) return; }
void instr32_0E(void) { push32_sreg(CS); if (cpu->pagefault) return; }

void instr16_0F(void)
{
    int32_t opcode = read_imm8();
    if (cpu->pagefault) return;
    run_instruction0f_16(opcode);
}
void instr32_0F(void)
{
    int32_t opcode = read_imm8();
    if (cpu->pagefault) return;
    run_instruction0f_32(opcode);
}

void instr_10_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE8(addr, adc8(x_, read_reg8(r))); }
void instr_10_reg(int32_t r1, int32_t r) { write_reg8(r1, adc8(read_reg8(r1), read_reg8(r))); }
void instr16_11_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE16(addr, adc16(x_, read_reg16(r))); }
void instr16_11_reg(int32_t r1, int32_t r) { write_reg16(r1, adc16(read_reg16(r1), read_reg16(r))); }
void instr32_11_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE32(addr, adc32(x_, read_reg32(r))); }
void instr32_11_reg(int32_t r1, int32_t r) { write_reg32(r1, adc32(read_reg32(r1), read_reg32(r))); }

void instr_12_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read8(addr);
    if (cpu->pagefault) return;
    write_reg8(r, adc8(read_reg8(r), x));
}
void instr_12_reg(int32_t r1, int32_t r) { write_reg8(r, adc8(read_reg8(r), read_reg8(r1))); }
void instr16_13_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    write_reg16(r, adc16(read_reg16(r), x));
}
void instr16_13_reg(int32_t r1, int32_t r) { write_reg16(r, adc16(read_reg16(r), read_reg16(r1))); }
void instr32_13_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    write_reg32(r, adc32(read_reg32(r), x));
}
void instr32_13_reg(int32_t r1, int32_t r) { write_reg32(r, adc32(read_reg32(r), read_reg32(r1))); }

void instr_14(int32_t imm8) { write_reg8(AL, adc8(read_reg8(AL), imm8)); }
void instr16_15(int32_t imm16) { write_reg16(AX, adc16(read_reg16(AX), imm16)); }
void instr32_15(int32_t imm32) { write_reg32(EAX, adc32(read_reg32(EAX), imm32)); }

void instr16_16(void) { push16((int32_t)cpu->sreg[SS]); if (cpu->pagefault) return; }
void instr32_16(void) { push32_sreg(SS); if (cpu->pagefault) return; }

void instr16_17(void)
{
    int32_t selector = safe_read16(get_stack_pointer(0));
    if (cpu->pagefault) return;
    if (!switch_seg(SS, selector)) {
        return;
    }
    adjust_stack_reg(2);
}
void instr32_17(void)
{
    int32_t selector = safe_read32s(get_stack_pointer(0));
    if (cpu->pagefault) return;
    if (!switch_seg(SS, selector & 0xFFFF)) {
        return;
    }
    adjust_stack_reg(4);
}

void instr_18_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE8(addr, sbb8(x_, read_reg8(r))); }
void instr_18_reg(int32_t r1, int32_t r) { write_reg8(r1, sbb8(read_reg8(r1), read_reg8(r))); }
void instr16_19_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE16(addr, sbb16(x_, read_reg16(r))); }
void instr16_19_reg(int32_t r1, int32_t r) { write_reg16(r1, sbb16(read_reg16(r1), read_reg16(r))); }
void instr32_19_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE32(addr, sbb32(x_, read_reg32(r))); }
void instr32_19_reg(int32_t r1, int32_t r) { write_reg32(r1, sbb32(read_reg32(r1), read_reg32(r))); }

void instr_1A_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read8(addr);
    if (cpu->pagefault) return;
    write_reg8(r, sbb8(read_reg8(r), x));
}
void instr_1A_reg(int32_t r1, int32_t r) { write_reg8(r, sbb8(read_reg8(r), read_reg8(r1))); }
void instr16_1B_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    write_reg16(r, sbb16(read_reg16(r), x));
}
void instr16_1B_reg(int32_t r1, int32_t r) { write_reg16(r, sbb16(read_reg16(r), read_reg16(r1))); }
void instr32_1B_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    write_reg32(r, sbb32(read_reg32(r), x));
}
void instr32_1B_reg(int32_t r1, int32_t r) { write_reg32(r, sbb32(read_reg32(r), read_reg32(r1))); }

void instr_1C(int32_t imm8) { write_reg8(AL, sbb8(read_reg8(AL), imm8)); }
void instr16_1D(int32_t imm16) { write_reg16(AX, sbb16(read_reg16(AX), imm16)); }
void instr32_1D(int32_t imm32) { write_reg32(EAX, sbb32(read_reg32(EAX), imm32)); }

void instr16_1E(void) { push16((int32_t)cpu->sreg[DS]); if (cpu->pagefault) return; }
void instr32_1E(void) { push32_sreg(DS); if (cpu->pagefault) return; }

void instr16_1F(void)
{
    int32_t selector = safe_read16(get_stack_pointer(0));
    if (cpu->pagefault) return;
    if (!switch_seg(DS, selector)) {
        return;
    }
    adjust_stack_reg(2);
}
void instr32_1F(void)
{
    int32_t selector = safe_read32s(get_stack_pointer(0));
    if (cpu->pagefault) return;
    if (!switch_seg(DS, selector & 0xFFFF)) {
        return;
    }
    adjust_stack_reg(4);
}

void instr_20_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE8(addr, and8(x_, read_reg8(r))); }
void instr_20_reg(int32_t r1, int32_t r) { write_reg8(r1, and8(read_reg8(r1), read_reg8(r))); }
void instr16_21_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE16(addr, and16(x_, read_reg16(r))); }
void instr16_21_reg(int32_t r1, int32_t r) { write_reg16(r1, and16(read_reg16(r1), read_reg16(r))); }
void instr32_21_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE32(addr, and32(x_, read_reg32(r))); }
void instr32_21_reg(int32_t r1, int32_t r) { write_reg32(r1, and32(read_reg32(r1), read_reg32(r))); }

void instr_22_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read8(addr);
    if (cpu->pagefault) return;
    write_reg8(r, and8(read_reg8(r), x));
}
void instr_22_reg(int32_t r1, int32_t r) { write_reg8(r, and8(read_reg8(r), read_reg8(r1))); }
void instr16_23_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    write_reg16(r, and16(read_reg16(r), x));
}
void instr16_23_reg(int32_t r1, int32_t r) { write_reg16(r, and16(read_reg16(r), read_reg16(r1))); }
void instr32_23_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    write_reg32(r, and32(read_reg32(r), x));
}
void instr32_23_reg(int32_t r1, int32_t r) { write_reg32(r, and32(read_reg32(r), read_reg32(r1))); }

void instr_24(int32_t imm8) { write_reg8(AL, and8(read_reg8(AL), imm8)); }
void instr16_25(int32_t imm16) { write_reg16(AX, and16(read_reg16(AX), imm16)); }
void instr32_25(int32_t imm32) { write_reg32(EAX, and32(read_reg32(EAX), imm32)); }

void instr_26(void) { segment_prefix_op(ES); }
void instr_27(void) { bcd_daa(); }

void instr_28_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE8(addr, sub8(x_, read_reg8(r))); }
void instr_28_reg(int32_t r1, int32_t r) { write_reg8(r1, sub8(read_reg8(r1), read_reg8(r))); }
void instr16_29_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE16(addr, sub16(x_, read_reg16(r))); }
void instr16_29_reg(int32_t r1, int32_t r) { write_reg16(r1, sub16(read_reg16(r1), read_reg16(r))); }
void instr32_29_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE32(addr, sub32(x_, read_reg32(r))); }
void instr32_29_reg(int32_t r1, int32_t r) { write_reg32(r1, sub32(read_reg32(r1), read_reg32(r))); }

void instr_2A_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read8(addr);
    if (cpu->pagefault) return;
    write_reg8(r, sub8(read_reg8(r), x));
}
void instr_2A_reg(int32_t r1, int32_t r) { write_reg8(r, sub8(read_reg8(r), read_reg8(r1))); }
void instr16_2B_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    write_reg16(r, sub16(read_reg16(r), x));
}
void instr16_2B_reg(int32_t r1, int32_t r) { write_reg16(r, sub16(read_reg16(r), read_reg16(r1))); }
void instr32_2B_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    write_reg32(r, sub32(read_reg32(r), x));
}
void instr32_2B_reg(int32_t r1, int32_t r) { write_reg32(r, sub32(read_reg32(r), read_reg32(r1))); }

void instr_2C(int32_t imm8) { write_reg8(AL, sub8(read_reg8(AL), imm8)); }
void instr16_2D(int32_t imm16) { write_reg16(AX, sub16(read_reg16(AX), imm16)); }
void instr32_2D(int32_t imm32) { write_reg32(EAX, sub32(read_reg32(EAX), imm32)); }

void instr_2E(void) { segment_prefix_op(CS); }
void instr_2F(void) { bcd_das(); }

void instr_30_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE8(addr, xor8(x_, read_reg8(r))); }
void instr_30_reg(int32_t r1, int32_t r) { write_reg8(r1, xor8(read_reg8(r1), read_reg8(r))); }
void instr16_31_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE16(addr, xor16(x_, read_reg16(r))); }
void instr16_31_reg(int32_t r1, int32_t r) { write_reg16(r1, xor16(read_reg16(r1), read_reg16(r))); }
void instr32_31_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE32(addr, xor32(x_, read_reg32(r))); }
void instr32_31_reg(int32_t r1, int32_t r) { write_reg32(r1, xor32(read_reg32(r1), read_reg32(r))); }

void instr_32_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read8(addr);
    if (cpu->pagefault) return;
    write_reg8(r, xor8(read_reg8(r), x));
}
void instr_32_reg(int32_t r1, int32_t r) { write_reg8(r, xor8(read_reg8(r), read_reg8(r1))); }
void instr16_33_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    write_reg16(r, xor16(read_reg16(r), x));
}
void instr16_33_reg(int32_t r1, int32_t r) { write_reg16(r, xor16(read_reg16(r), read_reg16(r1))); }
void instr32_33_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    write_reg32(r, xor32(read_reg32(r), x));
}
void instr32_33_reg(int32_t r1, int32_t r) { write_reg32(r, xor32(read_reg32(r), read_reg32(r1))); }

void instr_34(int32_t imm8) { write_reg8(AL, xor8(read_reg8(AL), imm8)); }
void instr16_35(int32_t imm16) { write_reg16(AX, xor16(read_reg16(AX), imm16)); }
void instr32_35(int32_t imm32) { write_reg32(EAX, xor32(read_reg32(EAX), imm32)); }

void instr_36(void) { segment_prefix_op(SS); }
void instr_37(void) { bcd_aaa(); }

void instr_38_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read8(addr);
    if (cpu->pagefault) return;
    cmp8(x, read_reg8(r));
}
void instr_38_reg(int32_t r1, int32_t r) { cmp8(read_reg8(r1), read_reg8(r)); }
void instr16_39_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    cmp16(x, read_reg16(r));
}
void instr16_39_reg(int32_t r1, int32_t r) { cmp16(read_reg16(r1), read_reg16(r)); }
void instr32_39_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    cmp32(x, read_reg32(r));
}
void instr32_39_reg(int32_t r1, int32_t r) { cmp32(read_reg32(r1), read_reg32(r)); }

void instr_3A_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read8(addr);
    if (cpu->pagefault) return;
    cmp8(read_reg8(r), x);
}
void instr_3A_reg(int32_t r1, int32_t r) { cmp8(read_reg8(r), read_reg8(r1)); }
void instr16_3B_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    cmp16(read_reg16(r), x);
}
void instr16_3B_reg(int32_t r1, int32_t r) { cmp16(read_reg16(r), read_reg16(r1)); }
void instr32_3B_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    cmp32(read_reg32(r), x);
}
void instr32_3B_reg(int32_t r1, int32_t r) { cmp32(read_reg32(r), read_reg32(r1)); }

void instr_3C(int32_t imm8) { cmp8(read_reg8(AL), imm8); }
void instr16_3D(int32_t imm16) { cmp16(read_reg16(AX), imm16); }
void instr32_3D(int32_t imm32) { cmp32(read_reg32(EAX), imm32); }

void instr_3E(void) { segment_prefix_op(DS); }
void instr_3F(void) { bcd_aas(); }

/* ================================================================== *
 * 0x40-0x6F -- inc/dec, push/pop, pusha/popa, bound/arpl, the prefixes,
 * imm push, imul, ins/outs
 * ================================================================== */

void instr16_40(void) { write_reg16(AX, inc16(read_reg16(AX))); }
void instr32_40(void) { write_reg32(EAX, inc32(read_reg32(EAX))); }
void instr16_41(void) { write_reg16(CX, inc16(read_reg16(CX))); }
void instr32_41(void) { write_reg32(ECX, inc32(read_reg32(ECX))); }
void instr16_42(void) { write_reg16(DX, inc16(read_reg16(DX))); }
void instr32_42(void) { write_reg32(EDX, inc32(read_reg32(EDX))); }
void instr16_43(void) { write_reg16(BX, inc16(read_reg16(BX))); }
void instr32_43(void) { write_reg32(EBX, inc32(read_reg32(EBX))); }
void instr16_44(void) { write_reg16(SP, inc16(read_reg16(SP))); }
void instr32_44(void) { write_reg32(ESP, inc32(read_reg32(ESP))); }
void instr16_45(void) { write_reg16(BP, inc16(read_reg16(BP))); }
void instr32_45(void) { write_reg32(EBP, inc32(read_reg32(EBP))); }
void instr16_46(void) { write_reg16(SI, inc16(read_reg16(SI))); }
void instr32_46(void) { write_reg32(ESI, inc32(read_reg32(ESI))); }
void instr16_47(void) { write_reg16(DI, inc16(read_reg16(DI))); }
void instr32_47(void) { write_reg32(EDI, inc32(read_reg32(EDI))); }

void instr16_48(void) { write_reg16(AX, dec16(read_reg16(AX))); }
void instr32_48(void) { write_reg32(EAX, dec32(read_reg32(EAX))); }
void instr16_49(void) { write_reg16(CX, dec16(read_reg16(CX))); }
void instr32_49(void) { write_reg32(ECX, dec32(read_reg32(ECX))); }
void instr16_4A(void) { write_reg16(DX, dec16(read_reg16(DX))); }
void instr32_4A(void) { write_reg32(EDX, dec32(read_reg32(EDX))); }
void instr16_4B(void) { write_reg16(BX, dec16(read_reg16(BX))); }
void instr32_4B(void) { write_reg32(EBX, dec32(read_reg32(EBX))); }
void instr16_4C(void) { write_reg16(SP, dec16(read_reg16(SP))); }
void instr32_4C(void) { write_reg32(ESP, dec32(read_reg32(ESP))); }
void instr16_4D(void) { write_reg16(BP, dec16(read_reg16(BP))); }
void instr32_4D(void) { write_reg32(EBP, dec32(read_reg32(EBP))); }
void instr16_4E(void) { write_reg16(SI, dec16(read_reg16(SI))); }
void instr32_4E(void) { write_reg32(ESI, dec32(read_reg32(ESI))); }
void instr16_4F(void) { write_reg16(DI, dec16(read_reg16(DI))); }
void instr32_4F(void) { write_reg32(EDI, dec32(read_reg32(EDI))); }

void push16_reg(int32_t r)
{
    push16(read_reg16(r));
    if (cpu->pagefault) return;
}
void push32_reg(int32_t r)
{
    push32(read_reg32(r));
    if (cpu->pagefault) return;
}

void instr16_50(void) { push16_reg(AX); }
void instr32_50(void) { push32_reg(EAX); }
void instr16_51(void) { push16_reg(CX); }
void instr32_51(void) { push32_reg(ECX); }
void instr16_52(void) { push16_reg(DX); }
void instr32_52(void) { push32_reg(EDX); }
void instr16_53(void) { push16_reg(BX); }
void instr32_53(void) { push32_reg(EBX); }
void instr16_54(void) { push16_reg(SP); }
void instr32_54(void) { push32_reg(ESP); }
void instr16_55(void) { push16_reg(BP); }
void instr32_55(void) { push32_reg(EBP); }
void instr16_56(void) { push16_reg(SI); }
void instr32_56(void) { push32_reg(ESI); }
void instr16_57(void) { push16_reg(DI); }
void instr32_57(void) { push32_reg(EDI); }

/* the pop family: upstream's `write_regX(reg, return_on_pagefault!(popX()))` must not write
 * the register when the pop faults, so the value is fetched into a local first (the
 * fault-aborts-the-instruction contract of cpu_state.h) */
void instr16_58(void) { int32_t v = pop16(); if (cpu->pagefault) return; write_reg16(AX, v); }
void instr32_58(void) { int32_t v = pop32s(); if (cpu->pagefault) return; write_reg32(EAX, v); }
void instr16_59(void) { int32_t v = pop16(); if (cpu->pagefault) return; write_reg16(CX, v); }
void instr32_59(void) { int32_t v = pop32s(); if (cpu->pagefault) return; write_reg32(ECX, v); }
void instr16_5A(void) { int32_t v = pop16(); if (cpu->pagefault) return; write_reg16(DX, v); }
void instr32_5A(void) { int32_t v = pop32s(); if (cpu->pagefault) return; write_reg32(EDX, v); }
void instr16_5B(void) { int32_t v = pop16(); if (cpu->pagefault) return; write_reg16(BX, v); }
void instr32_5B(void) { int32_t v = pop32s(); if (cpu->pagefault) return; write_reg32(EBX, v); }

void instr16_5C(void)
{
    int32_t v = safe_read16(get_stack_pointer(0));
    if (cpu->pagefault) return;
    write_reg16(SP, v);
}
void instr32_5C(void)
{
    int32_t v = safe_read32s(get_stack_pointer(0));
    if (cpu->pagefault) return;
    write_reg32(ESP, v);
}

void instr16_5D(void) { int32_t v = pop16(); if (cpu->pagefault) return; write_reg16(BP, v); }
void instr32_5D(void) { int32_t v = pop32s(); if (cpu->pagefault) return; write_reg32(EBP, v); }
void instr16_5E(void) { int32_t v = pop16(); if (cpu->pagefault) return; write_reg16(SI, v); }
void instr32_5E(void) { int32_t v = pop32s(); if (cpu->pagefault) return; write_reg32(ESI, v); }
void instr16_5F(void) { int32_t v = pop16(); if (cpu->pagefault) return; write_reg16(DI, v); }
void instr32_5F(void) { int32_t v = pop32s(); if (cpu->pagefault) return; write_reg32(EDI, v); }

void instr16_60(void) { pusha16(); }
void instr32_60(void) { pusha32(); }
void instr16_61(void) { popa16(); }
void instr32_61(void) { popa32(); }

void instr_62_reg(int32_t _r2, int32_t _r)
{
    /* bound -- dbg_log!("Unimplemented BOUND instruction") dropped (no log channel) */
    (void)_r2;
    (void)_r;
    DBG_ASSERT(false);
}
void instr_62_mem(int32_t _addr, int32_t _r)
{
    (void)_addr;
    (void)_r;
    DBG_ASSERT(false);
}

/* instructions.rs:536-548 */
int32_t arpl(int32_t seg, int32_t r16)
{
    cpu->flags_changed &= ~FLAG_ZERO;

    if ((seg & 3) < (r16 & 3)) {
        cpu->flags |= FLAG_ZERO;
        return (seg & ~3) | (r16 & 3);
    }
    else {
        cpu->flags &= ~FLAG_ZERO;
        return seg;
    }
}

void instr_63_mem(int32_t addr, int32_t r)
{
    if (!cpu->protected_mode || vm86_mode()) {
        /* dbg_log!("arpl #ud") dropped */
        trigger_ud();
        return;
    }
    SAFE_READ_WRITE16(addr, arpl(x_, read_reg16(r)));
}
void instr_63_reg(int32_t r1, int32_t r)
{
    if (!cpu->protected_mode || vm86_mode()) {
        trigger_ud();
        return;
    }
    write_reg16(r1, arpl(read_reg16(r1), read_reg16(r)));
}

void instr_64(void) { segment_prefix_op(FS); }
void instr_65(void) { segment_prefix_op(GS); }

void instr_66(void)
{
    /* Operand-size override prefix */
    cpu->prefixes |= PREFIX_MASK_OPSIZE;
    run_prefix_instruction();
    cpu->prefixes = 0;
}
void instr_67(void)
{
    /* Address-size override prefix */
    cpu->prefixes |= PREFIX_MASK_ADDRSIZE;
    run_prefix_instruction();
    cpu->prefixes = 0;
}

void instr16_68(int32_t imm16)
{
    push16(imm16);
    if (cpu->pagefault) return;
}
void instr32_68(int32_t imm32)
{
    push32(imm32);
    if (cpu->pagefault) return;
}

void instr16_69_mem(int32_t addr, int32_t r, int32_t imm)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    write_reg16(r, imul_reg16(x, imm));
}
void instr16_69_reg(int32_t r1, int32_t r, int32_t imm)
{
    write_reg16(r, imul_reg16(read_reg16(r1), imm));
}
void instr32_69_mem(int32_t addr, int32_t r, int32_t imm)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    write_reg32(r, imul_reg32(x, imm));
}
void instr32_69_reg(int32_t r1, int32_t r, int32_t imm)
{
    write_reg32(r, imul_reg32(read_reg32(r1), imm));
}

void instr16_6A(int32_t imm8)
{
    push16(imm8 & 0xFFFF);
    if (cpu->pagefault) return;
}
void instr32_6A(int32_t imm8)
{
    push32(imm8);
    if (cpu->pagefault) return;
}

void instr16_6B_mem(int32_t addr, int32_t r, int32_t imm)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    write_reg16(r, imul_reg16(x, imm));
}
void instr16_6B_reg(int32_t r1, int32_t r, int32_t imm)
{
    write_reg16(r, imul_reg16(read_reg16(r1), imm));
}
void instr32_6B_mem(int32_t addr, int32_t r, int32_t imm)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    write_reg32(r, imul_reg32(x, imm));
}
void instr32_6B_reg(int32_t r1, int32_t r, int32_t imm)
{
    write_reg32(r, imul_reg32(read_reg32(r1), imm));
}

void instr_6C(void) { insb_no_rep(is_asize_32()); }
void instr_F26C(void) { insb_rep(is_asize_32()); }
void instr_F36C(void) { insb_rep(is_asize_32()); }
void instr16_6D(void) { insw_no_rep(is_asize_32()); }
void instr32_6D(void) { insd_no_rep(is_asize_32()); }
void instr16_F26D(void) { insw_rep(is_asize_32()); }
void instr16_F36D(void) { insw_rep(is_asize_32()); }
void instr32_F26D(void) { insd_rep(is_asize_32()); }
void instr32_F36D(void) { insd_rep(is_asize_32()); }

void instr_6E(void) { outsb_no_rep(is_asize_32(), segment_prefix(DS)); }
void instr_F26E(void) { outsb_rep(is_asize_32(), segment_prefix(DS)); }
void instr_F36E(void) { outsb_rep(is_asize_32(), segment_prefix(DS)); }
void instr16_6F(void) { outsw_no_rep(is_asize_32(), segment_prefix(DS)); }
void instr32_6F(void) { outsd_no_rep(is_asize_32(), segment_prefix(DS)); }
void instr16_F26F(void) { outsw_rep(is_asize_32(), segment_prefix(DS)); }
void instr16_F36F(void) { outsw_rep(is_asize_32(), segment_prefix(DS)); }
void instr32_F26F(void) { outsd_rep(is_asize_32(), segment_prefix(DS)); }
void instr32_F36F(void) { outsd_rep(is_asize_32(), segment_prefix(DS)); }

/* ================================================================== *
 * 0x70-0x7F -- the conditional jumps
 * ================================================================== */

void instr16_70(int32_t imm8) { jmpcc16(test_o(), imm8); }
void instr16_71(int32_t imm8) { jmpcc16(!test_o(), imm8); }
void instr16_72(int32_t imm8) { jmpcc16(test_b(), imm8); }
void instr16_73(int32_t imm8) { jmpcc16(!test_b(), imm8); }
void instr16_74(int32_t imm8) { jmpcc16(test_z(), imm8); }
void instr16_75(int32_t imm8) { jmpcc16(!test_z(), imm8); }
void instr16_76(int32_t imm8) { jmpcc16(test_be(), imm8); }
void instr16_77(int32_t imm8) { jmpcc16(!test_be(), imm8); }
void instr16_78(int32_t imm8) { jmpcc16(test_s(), imm8); }
void instr16_79(int32_t imm8) { jmpcc16(!test_s(), imm8); }
void instr16_7A(int32_t imm8) { jmpcc16(test_p(), imm8); }
void instr16_7B(int32_t imm8) { jmpcc16(!test_p(), imm8); }
void instr16_7C(int32_t imm8) { jmpcc16(test_l(), imm8); }
void instr16_7D(int32_t imm8) { jmpcc16(!test_l(), imm8); }
void instr16_7E(int32_t imm8) { jmpcc16(test_le(), imm8); }
void instr16_7F(int32_t imm8) { jmpcc16(!test_le(), imm8); }

void instr32_70(int32_t imm8) { jmpcc32(test_o(), imm8); }
void instr32_71(int32_t imm8) { jmpcc32(!test_o(), imm8); }
void instr32_72(int32_t imm8) { jmpcc32(test_b(), imm8); }
void instr32_73(int32_t imm8) { jmpcc32(!test_b(), imm8); }
void instr32_74(int32_t imm8) { jmpcc32(test_z(), imm8); }
void instr32_75(int32_t imm8) { jmpcc32(!test_z(), imm8); }
void instr32_76(int32_t imm8) { jmpcc32(test_be(), imm8); }
void instr32_77(int32_t imm8) { jmpcc32(!test_be(), imm8); }
void instr32_78(int32_t imm8) { jmpcc32(test_s(), imm8); }
void instr32_79(int32_t imm8) { jmpcc32(!test_s(), imm8); }
void instr32_7A(int32_t imm8) { jmpcc32(test_p(), imm8); }
void instr32_7B(int32_t imm8) { jmpcc32(!test_p(), imm8); }
void instr32_7C(int32_t imm8) { jmpcc32(test_l(), imm8); }
void instr32_7D(int32_t imm8) { jmpcc32(!test_l(), imm8); }
void instr32_7E(int32_t imm8) { jmpcc32(test_le(), imm8); }
void instr32_7F(int32_t imm8) { jmpcc32(!test_le(), imm8); }

/* ================================================================== *
 * 0x80-0x83 -- the immediate ALU groups (fixed_g: 0 add/1 or/2 adc/3 sbb/
 * 4 and/5 sub/6 xor/7 cmp)
 * ================================================================== */

void instr_80_0_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE8(addr, add8(x_, imm)); }
void instr_80_0_reg(int32_t r1, int32_t imm) { write_reg8(r1, add8(read_reg8(r1), imm)); }
void instr_80_1_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE8(addr, or8(x_, imm)); }
void instr_80_1_reg(int32_t r1, int32_t imm) { write_reg8(r1, or8(read_reg8(r1), imm)); }
void instr_80_2_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE8(addr, adc8(x_, imm)); }
void instr_80_2_reg(int32_t r1, int32_t imm) { write_reg8(r1, adc8(read_reg8(r1), imm)); }
void instr_80_3_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE8(addr, sbb8(x_, imm)); }
void instr_80_3_reg(int32_t r1, int32_t imm) { write_reg8(r1, sbb8(read_reg8(r1), imm)); }
void instr_80_4_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE8(addr, and8(x_, imm)); }
void instr_80_4_reg(int32_t r1, int32_t imm) { write_reg8(r1, and8(read_reg8(r1), imm)); }
void instr_80_5_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE8(addr, sub8(x_, imm)); }
void instr_80_5_reg(int32_t r1, int32_t imm) { write_reg8(r1, sub8(read_reg8(r1), imm)); }
void instr_80_6_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE8(addr, xor8(x_, imm)); }
void instr_80_6_reg(int32_t r1, int32_t imm) { write_reg8(r1, xor8(read_reg8(r1), imm)); }
void instr_80_7_reg(int32_t r, int32_t imm) { cmp8(read_reg8(r), imm); }
void instr_80_7_mem(int32_t addr, int32_t imm)
{
    int32_t x = safe_read8(addr);
    if (cpu->pagefault) return;
    cmp8(x, imm);
}

void instr16_81_0_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE16(addr, add16(x_, imm)); }
void instr16_81_0_reg(int32_t r1, int32_t imm) { write_reg16(r1, add16(read_reg16(r1), imm)); }
void instr16_81_1_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE16(addr, or16(x_, imm)); }
void instr16_81_1_reg(int32_t r1, int32_t imm) { write_reg16(r1, or16(read_reg16(r1), imm)); }
void instr16_81_2_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE16(addr, adc16(x_, imm)); }
void instr16_81_2_reg(int32_t r1, int32_t imm) { write_reg16(r1, adc16(read_reg16(r1), imm)); }
void instr16_81_3_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE16(addr, sbb16(x_, imm)); }
void instr16_81_3_reg(int32_t r1, int32_t imm) { write_reg16(r1, sbb16(read_reg16(r1), imm)); }
void instr16_81_4_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE16(addr, and16(x_, imm)); }
void instr16_81_4_reg(int32_t r1, int32_t imm) { write_reg16(r1, and16(read_reg16(r1), imm)); }
void instr16_81_5_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE16(addr, sub16(x_, imm)); }
void instr16_81_5_reg(int32_t r1, int32_t imm) { write_reg16(r1, sub16(read_reg16(r1), imm)); }
void instr16_81_6_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE16(addr, xor16(x_, imm)); }
void instr16_81_6_reg(int32_t r1, int32_t imm) { write_reg16(r1, xor16(read_reg16(r1), imm)); }
void instr16_81_7_reg(int32_t r, int32_t imm) { cmp16(read_reg16(r), imm); }
void instr16_81_7_mem(int32_t addr, int32_t imm)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    cmp16(x, imm);
}

void instr32_81_0_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE32(addr, add32(x_, imm)); }
void instr32_81_0_reg(int32_t r1, int32_t imm) { write_reg32(r1, add32(read_reg32(r1), imm)); }
void instr32_81_1_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE32(addr, or32(x_, imm)); }
void instr32_81_1_reg(int32_t r1, int32_t imm) { write_reg32(r1, or32(read_reg32(r1), imm)); }
void instr32_81_2_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE32(addr, adc32(x_, imm)); }
void instr32_81_2_reg(int32_t r1, int32_t imm) { write_reg32(r1, adc32(read_reg32(r1), imm)); }
void instr32_81_3_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE32(addr, sbb32(x_, imm)); }
void instr32_81_3_reg(int32_t r1, int32_t imm) { write_reg32(r1, sbb32(read_reg32(r1), imm)); }
void instr32_81_4_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE32(addr, and32(x_, imm)); }
void instr32_81_4_reg(int32_t r1, int32_t imm) { write_reg32(r1, and32(read_reg32(r1), imm)); }
void instr32_81_5_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE32(addr, sub32(x_, imm)); }
void instr32_81_5_reg(int32_t r1, int32_t imm) { write_reg32(r1, sub32(read_reg32(r1), imm)); }
void instr32_81_6_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE32(addr, xor32(x_, imm)); }
void instr32_81_6_reg(int32_t r1, int32_t imm) { write_reg32(r1, xor32(read_reg32(r1), imm)); }
void instr32_81_7_reg(int32_t r, int32_t imm) { cmp32(read_reg32(r), imm); }
void instr32_81_7_mem(int32_t addr, int32_t imm)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    cmp32(x, imm);
}

/* 0x82 is the undocumented alias of 0x80 (same bodies upstream) */
void instr_82_0_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE8(addr, add8(x_, imm)); }
void instr_82_0_reg(int32_t r1, int32_t imm) { write_reg8(r1, add8(read_reg8(r1), imm)); }
void instr_82_1_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE8(addr, or8(x_, imm)); }
void instr_82_1_reg(int32_t r1, int32_t imm) { write_reg8(r1, or8(read_reg8(r1), imm)); }
void instr_82_2_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE8(addr, adc8(x_, imm)); }
void instr_82_2_reg(int32_t r1, int32_t imm) { write_reg8(r1, adc8(read_reg8(r1), imm)); }
void instr_82_3_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE8(addr, sbb8(x_, imm)); }
void instr_82_3_reg(int32_t r1, int32_t imm) { write_reg8(r1, sbb8(read_reg8(r1), imm)); }
void instr_82_4_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE8(addr, and8(x_, imm)); }
void instr_82_4_reg(int32_t r1, int32_t imm) { write_reg8(r1, and8(read_reg8(r1), imm)); }
void instr_82_5_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE8(addr, sub8(x_, imm)); }
void instr_82_5_reg(int32_t r1, int32_t imm) { write_reg8(r1, sub8(read_reg8(r1), imm)); }
void instr_82_6_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE8(addr, xor8(x_, imm)); }
void instr_82_6_reg(int32_t r1, int32_t imm) { write_reg8(r1, xor8(read_reg8(r1), imm)); }
void instr_82_7_reg(int32_t r, int32_t imm) { cmp8(read_reg8(r), imm); }
void instr_82_7_mem(int32_t addr, int32_t imm)
{
    int32_t x = safe_read8(addr);
    if (cpu->pagefault) return;
    cmp8(x, imm);
}

/* the 8-bit sign-extended immediate forms: the immediate is masked to 16 bits in the
 * 16-bit case (upstream's `imm & 0xFFFF`) and used whole in the 32-bit case */
void instr16_83_0_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE16(addr, add16(x_, imm & 0xFFFF)); }
void instr16_83_0_reg(int32_t r1, int32_t imm) { write_reg16(r1, add16(read_reg16(r1), imm & 0xFFFF)); }
void instr16_83_1_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE16(addr, or16(x_, imm & 0xFFFF)); }
void instr16_83_1_reg(int32_t r1, int32_t imm) { write_reg16(r1, or16(read_reg16(r1), imm & 0xFFFF)); }
void instr16_83_2_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE16(addr, adc16(x_, imm & 0xFFFF)); }
void instr16_83_2_reg(int32_t r1, int32_t imm) { write_reg16(r1, adc16(read_reg16(r1), imm & 0xFFFF)); }
void instr16_83_3_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE16(addr, sbb16(x_, imm & 0xFFFF)); }
void instr16_83_3_reg(int32_t r1, int32_t imm) { write_reg16(r1, sbb16(read_reg16(r1), imm & 0xFFFF)); }
void instr16_83_4_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE16(addr, and16(x_, imm & 0xFFFF)); }
void instr16_83_4_reg(int32_t r1, int32_t imm) { write_reg16(r1, and16(read_reg16(r1), imm & 0xFFFF)); }
void instr16_83_5_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE16(addr, sub16(x_, imm & 0xFFFF)); }
void instr16_83_5_reg(int32_t r1, int32_t imm) { write_reg16(r1, sub16(read_reg16(r1), imm & 0xFFFF)); }
void instr16_83_6_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE16(addr, xor16(x_, imm & 0xFFFF)); }
void instr16_83_6_reg(int32_t r1, int32_t imm) { write_reg16(r1, xor16(read_reg16(r1), imm & 0xFFFF)); }
void instr16_83_7_reg(int32_t r, int32_t imm) { cmp16(read_reg16(r), imm & 0xFFFF); }
void instr16_83_7_mem(int32_t addr, int32_t imm)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    cmp16(x, imm & 0xFFFF);
}

void instr32_83_0_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE32(addr, add32(x_, imm)); }
void instr32_83_0_reg(int32_t r1, int32_t imm) { write_reg32(r1, add32(read_reg32(r1), imm)); }
void instr32_83_1_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE32(addr, or32(x_, imm)); }
void instr32_83_1_reg(int32_t r1, int32_t imm) { write_reg32(r1, or32(read_reg32(r1), imm)); }
void instr32_83_2_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE32(addr, adc32(x_, imm)); }
void instr32_83_2_reg(int32_t r1, int32_t imm) { write_reg32(r1, adc32(read_reg32(r1), imm)); }
void instr32_83_3_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE32(addr, sbb32(x_, imm)); }
void instr32_83_3_reg(int32_t r1, int32_t imm) { write_reg32(r1, sbb32(read_reg32(r1), imm)); }
void instr32_83_4_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE32(addr, and32(x_, imm)); }
void instr32_83_4_reg(int32_t r1, int32_t imm) { write_reg32(r1, and32(read_reg32(r1), imm)); }
void instr32_83_5_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE32(addr, sub32(x_, imm)); }
void instr32_83_5_reg(int32_t r1, int32_t imm) { write_reg32(r1, sub32(read_reg32(r1), imm)); }
void instr32_83_6_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE32(addr, xor32(x_, imm)); }
void instr32_83_6_reg(int32_t r1, int32_t imm) { write_reg32(r1, xor32(read_reg32(r1), imm)); }
void instr32_83_7_reg(int32_t r, int32_t imm) { cmp32(read_reg32(r), imm); }
void instr32_83_7_mem(int32_t addr, int32_t imm)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    cmp32(x, imm);
}

/* ================================================================== *
 * 0x84-0x8F -- test, xchg, mov, lea, mov sreg, pop mem
 * ================================================================== */

void instr_84_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read8(addr);
    if (cpu->pagefault) return;
    test8(x, read_reg8(r));
}
void instr_84_reg(int32_t r1, int32_t r) { test8(read_reg8(r1), read_reg8(r)); }
void instr16_85_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    test16(x, read_reg16(r));
}
void instr16_85_reg(int32_t r1, int32_t r) { test16(read_reg16(r1), read_reg16(r)); }
void instr32_85_mem(int32_t addr, int32_t r)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    test32(x, read_reg32(r));
}
void instr32_85_reg(int32_t r1, int32_t r) { test32(read_reg32(r1), read_reg32(r)); }

void instr_86_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE8(addr, xchg8(x_, r)); }
void instr_86_reg(int32_t r1, int32_t r) { write_reg8(r1, xchg8(read_reg8(r1), r)); }
void instr16_87_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE16(addr, xchg16(x_, r)); }
void instr16_87_reg(int32_t r1, int32_t r) { write_reg16(r1, xchg16(read_reg16(r1), r)); }
void instr32_87_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE32(addr, xchg32(x_, r)); }
void instr32_87_reg(int32_t r1, int32_t r) { write_reg32(r1, xchg32(read_reg32(r1), r)); }

void instr_88_reg(int32_t r2, int32_t r) { write_reg8(r2, read_reg8(r)); }
void instr_88_mem(int32_t addr, int32_t r)
{
    safe_write8(addr, read_reg8(r));
    if (cpu->pagefault) return;
}
void instr16_89_reg(int32_t r2, int32_t r) { write_reg16(r2, read_reg16(r)); }
void instr16_89_mem(int32_t addr, int32_t r)
{
    safe_write16(addr, read_reg16(r));
    if (cpu->pagefault) return;
}
void instr32_89_reg(int32_t r2, int32_t r) { write_reg32(r2, read_reg32(r)); }
void instr32_89_mem(int32_t addr, int32_t r)
{
    safe_write32(addr, read_reg32(r));
    if (cpu->pagefault) return;
}

void instr_8A_mem(int32_t addr, int32_t r)
{
    int32_t v = safe_read8(addr);
    if (cpu->pagefault) return;
    write_reg8(r, v);
}
void instr_8A_reg(int32_t r1, int32_t r) { write_reg8(r, read_reg8(r1)); }
void instr16_8B_mem(int32_t addr, int32_t r)
{
    int32_t v = safe_read16(addr);
    if (cpu->pagefault) return;
    write_reg16(r, v);
}
void instr16_8B_reg(int32_t r1, int32_t r) { write_reg16(r, read_reg16(r1)); }
void instr32_8B_mem(int32_t addr, int32_t r)
{
    int32_t v = safe_read32s(addr);
    if (cpu->pagefault) return;
    write_reg32(r, v);
}
void instr32_8B_reg(int32_t r1, int32_t r) { write_reg32(r, read_reg32(r1)); }

/* instructions.rs:857-865 -- upstream keeps it `pub`; the emitter never calls it */
bool instr_8C_check_sreg(int32_t seg)
{
    if (seg >= 6) {
        /* dbg_log!("mov sreg #ud") dropped */
        trigger_ud();
        return false;
    }
    return true;
}
void instr16_8C_reg(int32_t r, int32_t seg)
{
    if (instr_8C_check_sreg(seg)) {
        write_reg16(r, (int32_t)cpu->sreg[seg]);
    }
}
void instr16_8C_mem(int32_t addr, int32_t seg)
{
    if (instr_8C_check_sreg(seg)) {
        safe_write16(addr, (int32_t)cpu->sreg[seg]);
        if (cpu->pagefault) return;
    }
}
void instr32_8C_reg(int32_t r, int32_t seg)
{
    if (instr_8C_check_sreg(seg)) {
        write_reg32(r, (int32_t)cpu->sreg[seg]);
    }
}
void instr32_8C_mem(int32_t addr, int32_t seg)
{
    if (instr_8C_check_sreg(seg)) {
        safe_write16(addr, (int32_t)cpu->sreg[seg]);
        if (cpu->pagefault) return;
    }
}

void instr16_8D_reg(int32_t _r, int32_t _r2)
{
    /* dbg_log!("lea #ud") dropped */
    (void)_r;
    (void)_r2;
    trigger_ud();
}
void instr16_8D_mem(int32_t modrm_byte, int32_t r)
{
    /* lea */
    cpu->prefixes |= SEG_PREFIX_ZERO;
    /* upstream is `if let Ok(addr) = modrm_resolve(...) { write_reg16(r, addr) }` -- the
     * fault is NOT propagated out of lea, it only suppresses the register write (the
     * instruction is over either way; the dispatcher's pagefault flag still says so). */
    int32_t addr = modrm_resolve(modrm_byte);
    if (!cpu->pagefault) {
        write_reg16(r, addr);
    }
    cpu->prefixes = 0;
}
void instr32_8D_reg(int32_t _r, int32_t _r2)
{
    (void)_r;
    (void)_r2;
    trigger_ud();
}
void instr32_8D_mem(int32_t modrm_byte, int32_t r)
{
    /* lea */
    /* override prefix, so modrm_resolve does not return the segment part */
    cpu->prefixes |= SEG_PREFIX_ZERO;
    int32_t addr = modrm_resolve(modrm_byte);
    if (!cpu->pagefault) {
        write_reg32(r, addr);
    }
    cpu->prefixes = 0;
}

void instr_8E_mem(int32_t addr, int32_t r)
{
    if (r == ES || r == SS || r == DS || r == FS || r == GS) {
        int32_t selector = safe_read16(addr);
        if (cpu->pagefault) return;
        if (!switch_seg(r, selector)) {
            return;
        }
    }
    else {
        /* dbg_log!("mov sreg #ud") dropped */
        trigger_ud();
    }
}
void instr_8E_reg(int32_t r1, int32_t r)
{
    if (r == ES || r == SS || r == DS || r == FS || r == GS) {
        switch_seg(r, read_reg16(r1));
    }
    else {
        trigger_ud();
    }
}

void instr16_8F_0_mem(int32_t modrm_byte)
{
    /* pop */
    /* Update esp *before* resolving the address */
    adjust_stack_reg(2);
    int32_t addr = modrm_resolve(modrm_byte);
    if (cpu->pagefault) {
        /* a pagefault happened, reset esp */
        adjust_stack_reg(-2);
        return;
    }
    adjust_stack_reg(-2);
    int32_t stack_value = safe_read16(get_stack_pointer(0));
    if (cpu->pagefault) return;
    safe_write16(addr, stack_value);
    if (cpu->pagefault) return;
    adjust_stack_reg(2);
}
void instr16_8F_0_reg(int32_t r) { int32_t v = pop16(); if (cpu->pagefault) return; write_reg16(r, v); }
void instr32_8F_0_mem(int32_t modrm_byte)
{
    /* Update esp *before* resolving the address */
    adjust_stack_reg(4);
    int32_t addr = modrm_resolve(modrm_byte);
    if (cpu->pagefault) {
        /* a pagefault happened, reset esp */
        adjust_stack_reg(-4);
        return;
    }
    adjust_stack_reg(-4);
    int32_t stack_value = safe_read32s(get_stack_pointer(0));
    if (cpu->pagefault) return;
    safe_write32(addr, stack_value);
    if (cpu->pagefault) return;
    adjust_stack_reg(4);
}
void instr32_8F_0_reg(int32_t r) { int32_t v = pop32s(); if (cpu->pagefault) return; write_reg32(r, v); }

/* ================================================================== *
 * 0x90-0x9F -- nop/xchg, the sign-extension pair, far call, fwait,
 * pushf/popf, sahf/lahf
 * ================================================================== */

void instr_90(void) {}
void instr16_91(void) { xchg16r(CX); }
void instr32_91(void) { xchg32r(ECX); }
void instr16_92(void) { xchg16r(DX); }
void instr32_92(void) { xchg32r(EDX); }
void instr16_93(void) { xchg16r(BX); }
void instr32_93(void) { xchg32r(EBX); }
void instr16_94(void) { xchg16r(SP); }
void instr32_94(void) { xchg32r(ESP); }
void instr16_95(void) { xchg16r(BP); }
void instr32_95(void) { xchg32r(EBP); }
void instr16_96(void) { xchg16r(SI); }
void instr32_96(void) { xchg32r(ESI); }
void instr16_97(void) { xchg16r(DI); }
void instr32_97(void) { xchg32r(EDI); }

/* cbw/cwde and cwd/cdq.  Upstream `read_reg8(AL) << 24 >> 24` / `read_reg16(AX) as i16 as
 * i32` -- the left shift is spelled through uint32_t (M1a-2's D5 rule: C's `<<` into the
 * sign bit is UB), the right shifts stay signed (arithmetic, as Rust's are). */
void instr16_98(void) { write_reg16(AX, SEX8(read_reg8(AL))); }
void instr32_98(void) { write_reg32(EAX, (int32_t)(int16_t)read_reg16(AX)); }
void instr16_99(void) { write_reg16(DX, (int32_t)(int16_t)read_reg16(AX) >> 15); }
void instr32_99(void) { write_reg32(EDX, read_reg32(EAX) >> 31); }

void instr16_9A(int32_t new_ip, int32_t new_cs)
{
    /* callf */
    far_jump(new_ip, new_cs, true, false);
}
void instr32_9A(int32_t new_ip, int32_t new_cs)
{
    /* callf.  Upstream (instructions.rs:1001-1005) wraps this arm in
     * `if !*protected_mode || vm86_mode() { if 0 != new_ip as u32 & 0xFFFF0000 {
     * dbg_assert!(false); } }` -- a `dbg_assert!(false)`, i.e. the SECOND FORM of the
     * 16-bit ip class: compiled out of the release reference, which far_jumps with the
     * full 32-bit new_ip.  Dropped AT THE SITE by PERF-FIX-2 (the tree-wide sweep); see
     * "THE 16-BIT ip CLASS" in this file's header. */
    far_jump(new_ip, new_cs, true, true);
}

void instr_9B(void)
{
    /* fwait: check for pending fpu exceptions */
    if ((cpu->cr[0] & (CR0_MP | CR0_TS)) == (CR0_MP | CR0_TS)) {
        /* Note: Different from task_switch_test
         * Triggers when TS and MP bits are set (EM bit is ignored) */
        trigger_nm();
    }
    else {
        fwait();
    }
}

static bool instr_pushf_popf_check(void)
{
    return (cpu->flags & FLAG_VM) != 0 && getiopl() < 3;
}

void instr16_9C(void)
{
    /* pushf */
    if (instr_pushf_popf_check()) {
        DBG_ASSERT(cpu->protected_mode);
        /* dbg_log!("pushf #gp") dropped */
        trigger_gp(0);
    }
    else {
        push16(get_eflags() & 0xFFFF);
        if (cpu->pagefault) return;
    }
}
void instr32_9C(void)
{
    /* pushf */
    if (instr_pushf_popf_check()) {
        /* trap to virtual 8086 monitor */
        DBG_ASSERT(cpu->protected_mode);
        trigger_gp(0);
    }
    else {
        /* vm and rf flag are cleared in image stored on the stack */
        push32(get_eflags() & 0xFCFFFF);
        if (cpu->pagefault) return;
    }
}

void instr16_9D(void)
{
    /* popf */
    if (instr_pushf_popf_check()) {
        /* dbg_log!("popf #gp") dropped */
        trigger_gp(0);
        return;
    }
    int32_t old_eflags = cpu->flags;
    int32_t v = pop16();
    if (cpu->pagefault) return;
    update_eflags((int32_t)(((uint32_t)cpu->flags & 0xFFFF0000u) | (uint32_t)v));
    if ((old_eflags & FLAG_INTERRUPT) == 0 && (cpu->flags & FLAG_INTERRUPT) != 0) {
        handle_irqs();
    }
}
void instr32_9D(void)
{
    /* popf */
    if (instr_pushf_popf_check()) {
        trigger_gp(0);
        return;
    }
    int32_t old_eflags = cpu->flags;
    int32_t v = pop32s();
    if (cpu->pagefault) return;
    update_eflags(v);
    if ((old_eflags & FLAG_INTERRUPT) == 0 && (cpu->flags & FLAG_INTERRUPT) != 0) {
        handle_irqs();
    }
}

void instr_9E(void)
{
    /* sahf */
    cpu->flags = (cpu->flags & ~255) | read_reg8(AH);
    cpu->flags = (cpu->flags & FLAGS_MASK) | FLAGS_DEFAULT;
    cpu->flags_changed &= ~255;
}
void instr_9F(void)
{
    /* lahf */
    write_reg8(AH, get_eflags());
}

/* ================================================================== *
 * 0xA0-0xAF -- the moffs moves and the string instructions
 *
 * The string opcodes are one-line calls into string.c (its file header has the
 * fast/slow-path structure); `segment_prefix(DS)` is the default source segment the
 * string instruction resolves through the segment-override prefix.
 * ================================================================== */

void instr_A0(int32_t moffs)
{
    /* mov */
    int32_t addr = get_seg_prefix_ds(moffs);
    if (cpu->pagefault) return;
    int32_t data = safe_read8(addr);
    if (cpu->pagefault) return;
    write_reg8(AL, data);
}
void instr16_A1(int32_t moffs)
{
    int32_t addr = get_seg_prefix_ds(moffs);
    if (cpu->pagefault) return;
    int32_t data = safe_read16(addr);
    if (cpu->pagefault) return;
    write_reg16(AX, data);
}
void instr32_A1(int32_t moffs)
{
    int32_t addr = get_seg_prefix_ds(moffs);
    if (cpu->pagefault) return;
    int32_t data = safe_read32s(addr);
    if (cpu->pagefault) return;
    write_reg32(EAX, data);
}
void instr_A2(int32_t moffs)
{
    /* mov */
    int32_t addr = get_seg_prefix_ds(moffs);
    if (cpu->pagefault) return;
    safe_write8(addr, read_reg8(AL));
    if (cpu->pagefault) return;
}
void instr16_A3(int32_t moffs)
{
    int32_t addr = get_seg_prefix_ds(moffs);
    if (cpu->pagefault) return;
    safe_write16(addr, read_reg16(AX));
    if (cpu->pagefault) return;
}
void instr32_A3(int32_t moffs)
{
    int32_t addr = get_seg_prefix_ds(moffs);
    if (cpu->pagefault) return;
    safe_write32(addr, read_reg32(EAX));
    if (cpu->pagefault) return;
}

void instr_A4(void) { movsb_no_rep(is_asize_32(), segment_prefix(DS)); }
void instr_F2A4(void) { movsb_rep(is_asize_32(), segment_prefix(DS)); }
void instr_F3A4(void) { movsb_rep(is_asize_32(), segment_prefix(DS)); }

void instr16_A5(void) { movsw_no_rep(is_asize_32(), segment_prefix(DS)); }
void instr32_A5(void) { movsd_no_rep(is_asize_32(), segment_prefix(DS)); }
void instr16_F2A5(void) { movsw_rep(is_asize_32(), segment_prefix(DS)); }
void instr16_F3A5(void) { movsw_rep(is_asize_32(), segment_prefix(DS)); }
void instr32_F2A5(void) { movsd_rep(is_asize_32(), segment_prefix(DS)); }
void instr32_F3A5(void) { movsd_rep(is_asize_32(), segment_prefix(DS)); }

void instr_A6(void) { cmpsb_no_rep(is_asize_32(), segment_prefix(DS)); }
void instr_F2A6(void) { cmpsb_repnz(is_asize_32(), segment_prefix(DS)); }
void instr_F3A6(void) { cmpsb_repz(is_asize_32(), segment_prefix(DS)); }
void instr16_A7(void) { cmpsw_no_rep(is_asize_32(), segment_prefix(DS)); }
void instr32_A7(void) { cmpsd_no_rep(is_asize_32(), segment_prefix(DS)); }
void instr16_F2A7(void) { cmpsw_repnz(is_asize_32(), segment_prefix(DS)); }
void instr16_F3A7(void) { cmpsw_repz(is_asize_32(), segment_prefix(DS)); }
void instr32_F2A7(void) { cmpsd_repnz(is_asize_32(), segment_prefix(DS)); }
void instr32_F3A7(void) { cmpsd_repz(is_asize_32(), segment_prefix(DS)); }

void instr_A8(int32_t imm8) { test8(read_reg8(AL), imm8); }
void instr16_A9(int32_t imm16) { test16(read_reg16(AX), imm16); }
void instr32_A9(int32_t imm32) { test32(read_reg32(EAX), imm32); }

void instr_AA(void) { stosb_no_rep(is_asize_32()); }
void instr_F2AA(void) { stosb_rep(is_asize_32()); }
void instr_F3AA(void) { stosb_rep(is_asize_32()); }

void instr16_AB(void) { stosw_no_rep(is_asize_32()); }
void instr32_AB(void) { stosd_no_rep(is_asize_32()); }
void instr16_F2AB(void) { stosw_rep(is_asize_32()); }
void instr16_F3AB(void) { stosw_rep(is_asize_32()); }
void instr32_F2AB(void) { stosd_rep(is_asize_32()); }
void instr32_F3AB(void) { stosd_rep(is_asize_32()); }

void instr_AC(void) { lodsb_no_rep(is_asize_32(), segment_prefix(DS)); }
void instr_F2AC(void) { lodsb_rep(is_asize_32(), segment_prefix(DS)); }
void instr_F3AC(void) { lodsb_rep(is_asize_32(), segment_prefix(DS)); }

void instr16_AD(void) { lodsw_no_rep(is_asize_32(), segment_prefix(DS)); }
void instr32_AD(void) { lodsd_no_rep(is_asize_32(), segment_prefix(DS)); }
void instr16_F2AD(void) { lodsw_rep(is_asize_32(), segment_prefix(DS)); }
void instr16_F3AD(void) { lodsw_rep(is_asize_32(), segment_prefix(DS)); }
void instr32_F2AD(void) { lodsd_rep(is_asize_32(), segment_prefix(DS)); }
void instr32_F3AD(void) { lodsd_rep(is_asize_32(), segment_prefix(DS)); }

void instr_AE(void) { scasb_no_rep(is_asize_32()); }
void instr_F2AE(void) { scasb_repnz(is_asize_32()); }
void instr_F3AE(void) { scasb_repz(is_asize_32()); }

void instr16_AF(void) { scasw_no_rep(is_asize_32()); }
void instr32_AF(void) { scasd_no_rep(is_asize_32()); }
void instr16_F2AF(void) { scasw_repnz(is_asize_32()); }
void instr16_F3AF(void) { scasw_repz(is_asize_32()); }
void instr32_F2AF(void) { scasd_repnz(is_asize_32()); }
void instr32_F3AF(void) { scasd_repz(is_asize_32()); }

/* ================================================================== *
 * 0xB0-0xBF -- mov immediate to register
 * ================================================================== */

void instr_B0(int32_t imm8) { write_reg8(AL, imm8); }
void instr_B1(int32_t imm8) { write_reg8(CL, imm8); }
void instr_B2(int32_t imm8) { write_reg8(DL, imm8); }
void instr_B3(int32_t imm8) { write_reg8(BL, imm8); }
void instr_B4(int32_t imm8) { write_reg8(AH, imm8); }
void instr_B5(int32_t imm8) { write_reg8(CH, imm8); }
void instr_B6(int32_t imm8) { write_reg8(DH, imm8); }
void instr_B7(int32_t imm8) { write_reg8(BH, imm8); }
void instr16_B8(int32_t imm) { write_reg16(AX, imm); }
void instr32_B8(int32_t imm) { write_reg32(EAX, imm); }
void instr16_B9(int32_t imm) { write_reg16(CX, imm); }
void instr32_B9(int32_t imm) { write_reg32(ECX, imm); }
void instr16_BA(int32_t imm) { write_reg16(DX, imm); }
void instr32_BA(int32_t imm) { write_reg32(EDX, imm); }
void instr16_BB(int32_t imm) { write_reg16(BX, imm); }
void instr32_BB(int32_t imm) { write_reg32(EBX, imm); }
void instr16_BC(int32_t imm) { write_reg16(SP, imm); }
void instr32_BC(int32_t imm) { write_reg32(ESP, imm); }
void instr16_BD(int32_t imm) { write_reg16(BP, imm); }
void instr32_BD(int32_t imm) { write_reg32(EBP, imm); }
void instr16_BE(int32_t imm) { write_reg16(SI, imm); }
void instr32_BE(int32_t imm) { write_reg32(ESI, imm); }
void instr16_BF(int32_t imm) { write_reg16(DI, imm); }
void instr32_BF(int32_t imm) { write_reg32(EDI, imm); }

/* ================================================================== *
 * 0xC0-0xCF -- shift/rotate groups, ret/retf, les/lds, mov imm, enter/
 * leave, int/int3/into, iret
 * ================================================================== */

void instr_C0_0_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE8(addr, rol8(x_, imm & 31)); }
void instr_C0_0_reg(int32_t r1, int32_t imm) { write_reg8(r1, rol8(read_reg8(r1), imm & 31)); }
void instr_C0_1_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE8(addr, ror8(x_, imm & 31)); }
void instr_C0_1_reg(int32_t r1, int32_t imm) { write_reg8(r1, ror8(read_reg8(r1), imm & 31)); }
void instr_C0_2_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE8(addr, rcl8(x_, imm & 31)); }
void instr_C0_2_reg(int32_t r1, int32_t imm) { write_reg8(r1, rcl8(read_reg8(r1), imm & 31)); }
void instr_C0_3_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE8(addr, rcr8(x_, imm & 31)); }
void instr_C0_3_reg(int32_t r1, int32_t imm) { write_reg8(r1, rcr8(read_reg8(r1), imm & 31)); }
void instr_C0_4_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE8(addr, shl8(x_, imm & 31)); }
void instr_C0_4_reg(int32_t r1, int32_t imm) { write_reg8(r1, shl8(read_reg8(r1), imm & 31)); }
void instr_C0_5_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE8(addr, shr8(x_, imm & 31)); }
void instr_C0_5_reg(int32_t r1, int32_t imm) { write_reg8(r1, shr8(read_reg8(r1), imm & 31)); }
void instr_C0_6_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE8(addr, shl8(x_, imm & 31)); }
void instr_C0_6_reg(int32_t r1, int32_t imm) { write_reg8(r1, shl8(read_reg8(r1), imm & 31)); }
void instr_C0_7_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE8(addr, sar8(x_, imm & 31)); }
void instr_C0_7_reg(int32_t r1, int32_t imm) { write_reg8(r1, sar8(read_reg8(r1), imm & 31)); }

void instr16_C1_0_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE16(addr, rol16(x_, imm & 31)); }
void instr16_C1_0_reg(int32_t r1, int32_t imm) { write_reg16(r1, rol16(read_reg16(r1), imm & 31)); }
void instr16_C1_1_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE16(addr, ror16(x_, imm & 31)); }
void instr16_C1_1_reg(int32_t r1, int32_t imm) { write_reg16(r1, ror16(read_reg16(r1), imm & 31)); }
void instr16_C1_2_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE16(addr, rcl16(x_, imm & 31)); }
void instr16_C1_2_reg(int32_t r1, int32_t imm) { write_reg16(r1, rcl16(read_reg16(r1), imm & 31)); }
void instr16_C1_3_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE16(addr, rcr16(x_, imm & 31)); }
void instr16_C1_3_reg(int32_t r1, int32_t imm) { write_reg16(r1, rcr16(read_reg16(r1), imm & 31)); }
void instr16_C1_4_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE16(addr, shl16(x_, imm & 31)); }
void instr16_C1_4_reg(int32_t r1, int32_t imm) { write_reg16(r1, shl16(read_reg16(r1), imm & 31)); }
void instr16_C1_5_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE16(addr, shr16(x_, imm & 31)); }
void instr16_C1_5_reg(int32_t r1, int32_t imm) { write_reg16(r1, shr16(read_reg16(r1), imm & 31)); }
void instr16_C1_6_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE16(addr, shl16(x_, imm & 31)); }
void instr16_C1_6_reg(int32_t r1, int32_t imm) { write_reg16(r1, shl16(read_reg16(r1), imm & 31)); }
void instr16_C1_7_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE16(addr, sar16(x_, imm & 31)); }
void instr16_C1_7_reg(int32_t r1, int32_t imm) { write_reg16(r1, sar16(read_reg16(r1), imm & 31)); }

void instr32_C1_0_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE32(addr, rol32(x_, imm & 31)); }
void instr32_C1_0_reg(int32_t r1, int32_t imm) { write_reg32(r1, rol32(read_reg32(r1), imm & 31)); }
void instr32_C1_1_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE32(addr, ror32(x_, imm & 31)); }
void instr32_C1_1_reg(int32_t r1, int32_t imm) { write_reg32(r1, ror32(read_reg32(r1), imm & 31)); }
void instr32_C1_2_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE32(addr, rcl32(x_, imm & 31)); }
void instr32_C1_2_reg(int32_t r1, int32_t imm) { write_reg32(r1, rcl32(read_reg32(r1), imm & 31)); }
void instr32_C1_3_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE32(addr, rcr32(x_, imm & 31)); }
void instr32_C1_3_reg(int32_t r1, int32_t imm) { write_reg32(r1, rcr32(read_reg32(r1), imm & 31)); }
void instr32_C1_4_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE32(addr, shl32(x_, imm & 31)); }
void instr32_C1_4_reg(int32_t r1, int32_t imm) { write_reg32(r1, shl32(read_reg32(r1), imm & 31)); }
void instr32_C1_5_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE32(addr, shr32(x_, imm & 31)); }
void instr32_C1_5_reg(int32_t r1, int32_t imm) { write_reg32(r1, shr32(read_reg32(r1), imm & 31)); }
void instr32_C1_6_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE32(addr, shl32(x_, imm & 31)); }
void instr32_C1_6_reg(int32_t r1, int32_t imm) { write_reg32(r1, shl32(read_reg32(r1), imm & 31)); }
void instr32_C1_7_mem(int32_t addr, int32_t imm) { SAFE_READ_WRITE32(addr, sar32(x_, imm & 31)); }
void instr32_C1_7_reg(int32_t r1, int32_t imm) { write_reg32(r1, sar32(read_reg32(r1), imm & 31)); }

void instr16_C2(int32_t imm16)
{
    /* retn */
    uint64_t cs = get_seg_cs();
    int32_t v = pop16();
    if (cpu->pagefault) return;
    set_instruction_pointer(cs + v);
    /* no dbg_assert here: the 16-bit ip class -- see "THE 16-BIT ip CLASS" in this file's header */
    adjust_stack_reg(imm16);
}
void instr32_C2(int32_t imm16)
{
    /* retn */
    uint64_t cs = get_seg_cs();
    int32_t ip = pop32s();
    if (cpu->pagefault) return;
    /* no dbg_assert here: the 16-bit ip class -- see "THE 16-BIT ip CLASS" in this file's header */
    set_instruction_pointer(cs + ip);
    adjust_stack_reg(imm16);
}
void instr16_C3(void)
{
    /* retn */
    uint64_t cs = get_seg_cs();
    int32_t v = pop16();
    if (cpu->pagefault) return;
    set_instruction_pointer(cs + v);
}
void instr32_C3(void)
{
    /* retn */
    uint64_t cs = get_seg_cs();
    int32_t ip = pop32s();
    if (cpu->pagefault) return;
    /* no dbg_assert here: the 16-bit ip class -- see "THE 16-BIT ip CLASS" in this file's header */
    set_instruction_pointer(cs + ip);
}

void instr16_C4_reg(int32_t _unused1, int32_t _unused2) { (void)_unused1; (void)_unused2; trigger_ud(); }
void instr16_C4_mem(int32_t addr, int32_t r) { lss16(addr, r, ES); }
void instr32_C4_reg(int32_t _unused1, int32_t _unused2) { (void)_unused1; (void)_unused2; trigger_ud(); }
void instr32_C4_mem(int32_t addr, int32_t r) { lss32(addr, r, ES); }
void instr16_C5_reg(int32_t _unused1, int32_t _unused2) { (void)_unused1; (void)_unused2; trigger_ud(); }
void instr16_C5_mem(int32_t addr, int32_t r) { lss16(addr, r, DS); }
void instr32_C5_reg(int32_t _unused1, int32_t _unused2) { (void)_unused1; (void)_unused2; trigger_ud(); }
void instr32_C5_mem(int32_t addr, int32_t r) { lss32(addr, r, DS); }

void instr_C6_0_reg(int32_t r, int32_t imm) { write_reg8(r, imm); }
void instr_C6_0_mem(int32_t addr, int32_t imm)
{
    safe_write8(addr, imm);
    if (cpu->pagefault) return;
}
void instr16_C7_0_reg(int32_t r, int32_t imm) { write_reg16(r, imm); }
void instr16_C7_0_mem(int32_t addr, int32_t imm)
{
    safe_write16(addr, imm);
    if (cpu->pagefault) return;
}
void instr32_C7_0_reg(int32_t r, int32_t imm) { write_reg32(r, imm); }
void instr32_C7_0_mem(int32_t addr, int32_t imm)
{
    safe_write32(addr, imm);
    if (cpu->pagefault) return;
}

void instr16_C8(int32_t size, int32_t nesting) { enter16(size, nesting); }
void instr32_C8(int32_t size, int32_t nesting) { enter32(size, nesting); }

void instr16_C9(void)
{
    /* leave */
    int32_t old_vbp = cpu->stack_size_32 ? read_reg32(EBP) : read_reg16(BP);
    int32_t new_bp = safe_read16(get_seg_ss() + old_vbp);
    if (cpu->pagefault) return;
    set_stack_reg(old_vbp + 2);
    write_reg16(BP, new_bp);
}
void instr32_C9(void)
{
    int32_t old_vbp = cpu->stack_size_32 ? read_reg32(EBP) : read_reg16(BP);
    int32_t new_ebp = safe_read32s(get_seg_ss() + old_vbp);
    if (cpu->pagefault) return;
    set_stack_reg(old_vbp + 4);
    write_reg32(EBP, new_ebp);
}

void instr16_CA(int32_t imm16)
{
    /* retf */
    int32_t ip = safe_read16(get_stack_pointer(0));
    if (cpu->pagefault) return;
    int32_t cs = safe_read16(get_stack_pointer(2));
    if (cpu->pagefault) return;
    far_return(ip, cs, imm16, false);
}
void instr32_CA(int32_t imm16)
{
    /* retf */
    int32_t ip = safe_read32s(get_stack_pointer(0));
    if (cpu->pagefault) return;
    int32_t cs = safe_read32s(get_stack_pointer(4));
    if (cpu->pagefault) return;
    far_return(ip, cs & 0xFFFF, imm16, true);
    /* no dbg_assert here: the 16-bit ip class -- see "THE 16-BIT ip CLASS" in this file's header */
}
void instr16_CB(void)
{
    /* retf */
    int32_t ip = safe_read16(get_stack_pointer(0));
    if (cpu->pagefault) return;
    int32_t cs = safe_read16(get_stack_pointer(2));
    if (cpu->pagefault) return;
    far_return(ip, cs, 0, false);
    /* no dbg_assert here: the 16-bit ip class -- see "THE 16-BIT ip CLASS" in this file's header */
}
void instr32_CB(void)
{
    /* retf */
    int32_t ip = safe_read32s(get_stack_pointer(0));
    if (cpu->pagefault) return;
    int32_t cs = safe_read32s(get_stack_pointer(4));
    if (cpu->pagefault) return;
    far_return(ip, cs & 0xFFFF, 0, true);
    /* no dbg_assert here: the 16-bit ip class -- see "THE 16-BIT ip CLASS" in this file's header */
}

void instr_CC(void)
{
    /* INT3 */
    /* TODO: inhibit iopl checks */
    call_interrupt_vector(CPU_EXCEPTION_BP, true, false /* no error code */, 0);
}
void instr_CD(int32_t imm8)
{
    /* INT */
    call_interrupt_vector(imm8, true, false /* no error code */, 0);
}
void instr_CE(void)
{
    /* INTO */
    if (getof()) {
        /* TODO: inhibit iopl checks */
        call_interrupt_vector(CPU_EXCEPTION_OF, true, false /* no error code */, 0);
    }
}
void instr16_CF(void)
{
    /* iret */
    iret16();
}
void instr32_CF(void)
{
    iret32();
}

/* ================================================================== *
 * 0xD0-0xD7 -- the rotate/shift-by-one and by-CL groups, aam/aad,
 * salc and xlat
 * ================================================================== */

void instr_D0_0_mem(int32_t addr) { SAFE_READ_WRITE8(addr, rol8(x_, 1)); }
void instr_D0_0_reg(int32_t r1) { write_reg8(r1, rol8(read_reg8(r1), 1)); }
void instr_D0_1_mem(int32_t addr) { SAFE_READ_WRITE8(addr, ror8(x_, 1)); }
void instr_D0_1_reg(int32_t r1) { write_reg8(r1, ror8(read_reg8(r1), 1)); }
void instr_D0_2_mem(int32_t addr) { SAFE_READ_WRITE8(addr, rcl8(x_, 1)); }
void instr_D0_2_reg(int32_t r1) { write_reg8(r1, rcl8(read_reg8(r1), 1)); }
void instr_D0_3_mem(int32_t addr) { SAFE_READ_WRITE8(addr, rcr8(x_, 1)); }
void instr_D0_3_reg(int32_t r1) { write_reg8(r1, rcr8(read_reg8(r1), 1)); }
void instr_D0_4_mem(int32_t addr) { SAFE_READ_WRITE8(addr, shl8(x_, 1)); }
void instr_D0_4_reg(int32_t r1) { write_reg8(r1, shl8(read_reg8(r1), 1)); }
void instr_D0_5_mem(int32_t addr) { SAFE_READ_WRITE8(addr, shr8(x_, 1)); }
void instr_D0_5_reg(int32_t r1) { write_reg8(r1, shr8(read_reg8(r1), 1)); }
void instr_D0_6_mem(int32_t addr) { SAFE_READ_WRITE8(addr, shl8(x_, 1)); }
void instr_D0_6_reg(int32_t r1) { write_reg8(r1, shl8(read_reg8(r1), 1)); }
void instr_D0_7_mem(int32_t addr) { SAFE_READ_WRITE8(addr, sar8(x_, 1)); }
void instr_D0_7_reg(int32_t r1) { write_reg8(r1, sar8(read_reg8(r1), 1)); }

void instr16_D1_0_mem(int32_t addr) { SAFE_READ_WRITE16(addr, rol16(x_, 1)); }
void instr16_D1_0_reg(int32_t r1) { write_reg16(r1, rol16(read_reg16(r1), 1)); }
void instr16_D1_1_mem(int32_t addr) { SAFE_READ_WRITE16(addr, ror16(x_, 1)); }
void instr16_D1_1_reg(int32_t r1) { write_reg16(r1, ror16(read_reg16(r1), 1)); }
void instr16_D1_2_mem(int32_t addr) { SAFE_READ_WRITE16(addr, rcl16(x_, 1)); }
void instr16_D1_2_reg(int32_t r1) { write_reg16(r1, rcl16(read_reg16(r1), 1)); }
void instr16_D1_3_mem(int32_t addr) { SAFE_READ_WRITE16(addr, rcr16(x_, 1)); }
void instr16_D1_3_reg(int32_t r1) { write_reg16(r1, rcr16(read_reg16(r1), 1)); }
void instr16_D1_4_mem(int32_t addr) { SAFE_READ_WRITE16(addr, shl16(x_, 1)); }
void instr16_D1_4_reg(int32_t r1) { write_reg16(r1, shl16(read_reg16(r1), 1)); }
void instr16_D1_5_mem(int32_t addr) { SAFE_READ_WRITE16(addr, shr16(x_, 1)); }
void instr16_D1_5_reg(int32_t r1) { write_reg16(r1, shr16(read_reg16(r1), 1)); }
void instr16_D1_6_mem(int32_t addr) { SAFE_READ_WRITE16(addr, shl16(x_, 1)); }
void instr16_D1_6_reg(int32_t r1) { write_reg16(r1, shl16(read_reg16(r1), 1)); }
void instr16_D1_7_mem(int32_t addr) { SAFE_READ_WRITE16(addr, sar16(x_, 1)); }
void instr16_D1_7_reg(int32_t r1) { write_reg16(r1, sar16(read_reg16(r1), 1)); }

void instr32_D1_0_mem(int32_t addr) { SAFE_READ_WRITE32(addr, rol32(x_, 1)); }
void instr32_D1_0_reg(int32_t r1) { write_reg32(r1, rol32(read_reg32(r1), 1)); }
void instr32_D1_1_mem(int32_t addr) { SAFE_READ_WRITE32(addr, ror32(x_, 1)); }
void instr32_D1_1_reg(int32_t r1) { write_reg32(r1, ror32(read_reg32(r1), 1)); }
void instr32_D1_2_mem(int32_t addr) { SAFE_READ_WRITE32(addr, rcl32(x_, 1)); }
void instr32_D1_2_reg(int32_t r1) { write_reg32(r1, rcl32(read_reg32(r1), 1)); }
void instr32_D1_3_mem(int32_t addr) { SAFE_READ_WRITE32(addr, rcr32(x_, 1)); }
void instr32_D1_3_reg(int32_t r1) { write_reg32(r1, rcr32(read_reg32(r1), 1)); }
void instr32_D1_4_mem(int32_t addr) { SAFE_READ_WRITE32(addr, shl32(x_, 1)); }
void instr32_D1_4_reg(int32_t r1) { write_reg32(r1, shl32(read_reg32(r1), 1)); }
void instr32_D1_5_mem(int32_t addr) { SAFE_READ_WRITE32(addr, shr32(x_, 1)); }
void instr32_D1_5_reg(int32_t r1) { write_reg32(r1, shr32(read_reg32(r1), 1)); }
void instr32_D1_6_mem(int32_t addr) { SAFE_READ_WRITE32(addr, shl32(x_, 1)); }
void instr32_D1_6_reg(int32_t r1) { write_reg32(r1, shl32(read_reg32(r1), 1)); }
void instr32_D1_7_mem(int32_t addr) { SAFE_READ_WRITE32(addr, sar32(x_, 1)); }
void instr32_D1_7_reg(int32_t r1) { write_reg32(r1, sar32(read_reg32(r1), 1)); }

void instr_D2_0_mem(int32_t addr) { SAFE_READ_WRITE8(addr, rol8(x_, read_reg8(CL) & 31)); }
void instr_D2_0_reg(int32_t r1) { write_reg8(r1, rol8(read_reg8(r1), read_reg8(CL) & 31)); }
void instr_D2_1_mem(int32_t addr) { SAFE_READ_WRITE8(addr, ror8(x_, read_reg8(CL) & 31)); }
void instr_D2_1_reg(int32_t r1) { write_reg8(r1, ror8(read_reg8(r1), read_reg8(CL) & 31)); }
void instr_D2_2_mem(int32_t addr) { SAFE_READ_WRITE8(addr, rcl8(x_, read_reg8(CL) & 31)); }
void instr_D2_2_reg(int32_t r1) { write_reg8(r1, rcl8(read_reg8(r1), read_reg8(CL) & 31)); }
void instr_D2_3_mem(int32_t addr) { SAFE_READ_WRITE8(addr, rcr8(x_, read_reg8(CL) & 31)); }
void instr_D2_3_reg(int32_t r1) { write_reg8(r1, rcr8(read_reg8(r1), read_reg8(CL) & 31)); }
void instr_D2_4_mem(int32_t addr) { SAFE_READ_WRITE8(addr, shl8(x_, read_reg8(CL) & 31)); }
void instr_D2_4_reg(int32_t r1) { write_reg8(r1, shl8(read_reg8(r1), read_reg8(CL) & 31)); }
void instr_D2_5_mem(int32_t addr) { SAFE_READ_WRITE8(addr, shr8(x_, read_reg8(CL) & 31)); }
void instr_D2_5_reg(int32_t r1) { write_reg8(r1, shr8(read_reg8(r1), read_reg8(CL) & 31)); }
void instr_D2_6_mem(int32_t addr) { SAFE_READ_WRITE8(addr, shl8(x_, read_reg8(CL) & 31)); }
void instr_D2_6_reg(int32_t r1) { write_reg8(r1, shl8(read_reg8(r1), read_reg8(CL) & 31)); }
void instr_D2_7_mem(int32_t addr) { SAFE_READ_WRITE8(addr, sar8(x_, read_reg8(CL) & 31)); }
void instr_D2_7_reg(int32_t r1) { write_reg8(r1, sar8(read_reg8(r1), read_reg8(CL) & 31)); }

void instr16_D3_0_mem(int32_t addr) { SAFE_READ_WRITE16(addr, rol16(x_, read_reg8(CL) & 31)); }
void instr16_D3_0_reg(int32_t r1) { write_reg16(r1, rol16(read_reg16(r1), read_reg8(CL) & 31)); }
void instr16_D3_1_mem(int32_t addr) { SAFE_READ_WRITE16(addr, ror16(x_, read_reg8(CL) & 31)); }
void instr16_D3_1_reg(int32_t r1) { write_reg16(r1, ror16(read_reg16(r1), read_reg8(CL) & 31)); }
void instr16_D3_2_mem(int32_t addr) { SAFE_READ_WRITE16(addr, rcl16(x_, read_reg8(CL) & 31)); }
void instr16_D3_2_reg(int32_t r1) { write_reg16(r1, rcl16(read_reg16(r1), read_reg8(CL) & 31)); }
void instr16_D3_3_mem(int32_t addr) { SAFE_READ_WRITE16(addr, rcr16(x_, read_reg8(CL) & 31)); }
void instr16_D3_3_reg(int32_t r1) { write_reg16(r1, rcr16(read_reg16(r1), read_reg8(CL) & 31)); }
void instr16_D3_4_mem(int32_t addr) { SAFE_READ_WRITE16(addr, shl16(x_, read_reg8(CL) & 31)); }
void instr16_D3_4_reg(int32_t r1) { write_reg16(r1, shl16(read_reg16(r1), read_reg8(CL) & 31)); }
void instr16_D3_5_mem(int32_t addr) { SAFE_READ_WRITE16(addr, shr16(x_, read_reg8(CL) & 31)); }
void instr16_D3_5_reg(int32_t r1) { write_reg16(r1, shr16(read_reg16(r1), read_reg8(CL) & 31)); }
void instr16_D3_6_mem(int32_t addr) { SAFE_READ_WRITE16(addr, shl16(x_, read_reg8(CL) & 31)); }
void instr16_D3_6_reg(int32_t r1) { write_reg16(r1, shl16(read_reg16(r1), read_reg8(CL) & 31)); }
void instr16_D3_7_mem(int32_t addr) { SAFE_READ_WRITE16(addr, sar16(x_, read_reg8(CL) & 31)); }
void instr16_D3_7_reg(int32_t r1) { write_reg16(r1, sar16(read_reg16(r1), read_reg8(CL) & 31)); }

void instr32_D3_0_mem(int32_t addr) { SAFE_READ_WRITE32(addr, rol32(x_, read_reg8(CL) & 31)); }
void instr32_D3_0_reg(int32_t r1) { write_reg32(r1, rol32(read_reg32(r1), read_reg8(CL) & 31)); }
void instr32_D3_1_mem(int32_t addr) { SAFE_READ_WRITE32(addr, ror32(x_, read_reg8(CL) & 31)); }
void instr32_D3_1_reg(int32_t r1) { write_reg32(r1, ror32(read_reg32(r1), read_reg8(CL) & 31)); }
void instr32_D3_2_mem(int32_t addr) { SAFE_READ_WRITE32(addr, rcl32(x_, read_reg8(CL) & 31)); }
void instr32_D3_2_reg(int32_t r1) { write_reg32(r1, rcl32(read_reg32(r1), read_reg8(CL) & 31)); }
void instr32_D3_3_mem(int32_t addr) { SAFE_READ_WRITE32(addr, rcr32(x_, read_reg8(CL) & 31)); }
void instr32_D3_3_reg(int32_t r1) { write_reg32(r1, rcr32(read_reg32(r1), read_reg8(CL) & 31)); }
void instr32_D3_4_mem(int32_t addr) { SAFE_READ_WRITE32(addr, shl32(x_, read_reg8(CL) & 31)); }
void instr32_D3_4_reg(int32_t r1) { write_reg32(r1, shl32(read_reg32(r1), read_reg8(CL) & 31)); }
void instr32_D3_5_mem(int32_t addr) { SAFE_READ_WRITE32(addr, shr32(x_, read_reg8(CL) & 31)); }
void instr32_D3_5_reg(int32_t r1) { write_reg32(r1, shr32(read_reg32(r1), read_reg8(CL) & 31)); }
void instr32_D3_6_mem(int32_t addr) { SAFE_READ_WRITE32(addr, shl32(x_, read_reg8(CL) & 31)); }
void instr32_D3_6_reg(int32_t r1) { write_reg32(r1, shl32(read_reg32(r1), read_reg8(CL) & 31)); }
void instr32_D3_7_mem(int32_t addr) { SAFE_READ_WRITE32(addr, sar32(x_, read_reg8(CL) & 31)); }
void instr32_D3_7_reg(int32_t r1) { write_reg32(r1, sar32(read_reg32(r1), read_reg8(CL) & 31)); }

void instr_D4(int32_t arg) { bcd_aam(arg); }
void instr_D5(int32_t arg) { bcd_aad(arg); }

void instr_D6(void)
{
    /* salc */
    write_reg8(AL, -(int32_t)getcf());
}

void instr_D7(void)
{
    /* xlat.  dbg_assert!(!in_jit) dropped (no JIT). */
    if (is_asize_32()) {
        int32_t seg = get_seg_prefix(DS);
        if (cpu->pagefault) return;
        int32_t v = safe_read8(seg + read_reg32(EBX) + read_reg8(AL));
        if (cpu->pagefault) return;
        write_reg8(AL, v);
    }
    else {
        int32_t seg = get_seg_prefix(DS);
        if (cpu->pagefault) return;
        int32_t v = safe_read8(seg + ((read_reg16(BX) + read_reg8(AL)) & 0xFFFF));
        if (cpu->pagefault) return;
        write_reg8(AL, v);
    }
}

/* ------------------------------------------------------------------ *
 * 0xD8-0xDF -- THE FPU ENTRY POINTS: DEFERRED TO M1b.
 *
 * instructions.rs:1643-2003 defines the 176 `instr*_D8..DF_*` functions, all of them
 * one-line calls into fpu.rs (`fpu_fadd`, `fpu_load_m32`, `fpu_get_sti`, ...), whose
 * argument and return types are fpu.rs's and softfloat.rs's (F80 and its constants).  They
 * are BASE-table opcodes, so the emitter will generate calls to them, but porting them in
 * this unit would mean freezing 81 `fpu_*` signatures and seven F80 constant values from
 * outside the file that owns them -- the same reason M1a-2 left lar/lsl/verr/verw to M1c.
 * M1b (plan ss7 steps 14-16: softfloat.c, fpu.c) is the owner; the unit report lists the
 * 176 names so the emitter unit and M1b can check completeness rather than discover it.
 * ------------------------------------------------------------------ */

/* ================================================================== *
 * 0xE0-0xEF -- the loop family, in/out, call/jmp rel, jmpf
 * ================================================================== */

void instr16_E0(int32_t imm8s) { loopne16(imm8s); }
void instr16_E1(int32_t imm8s) { loope16(imm8s); }
void instr16_E2(int32_t imm8s) { loop16(imm8s); }
void instr16_E3(int32_t imm8s) { jcxz16(imm8s); }
void instr32_E0(int32_t imm8s) { loopne32(imm8s); }
void instr32_E1(int32_t imm8s) { loope32(imm8s); }
void instr32_E2(int32_t imm8s) { loop32(imm8s); }
void instr32_E3(int32_t imm8s) { jcxz32(imm8s); }

void instr_E4(int32_t port)
{
    if (test_privileges_for_io(port, 1)) {
        write_reg8(AL, io_port_read8(port));
    }
}
void instr16_E5(int32_t port)
{
    if (test_privileges_for_io(port, 2)) {
        write_reg16(AX, io_port_read16(port));
    }
}
void instr32_E5(int32_t port)
{
    if (test_privileges_for_io(port, 4)) {
        write_reg32(EAX, io_port_read32(port));
    }
}
void instr_E6(int32_t port)
{
    if (test_privileges_for_io(port, 1)) {
        io_port_write8(port, read_reg8(AL));
    }
}
void instr16_E7(int32_t port)
{
    if (test_privileges_for_io(port, 2)) {
        io_port_write16(port, read_reg16(AX));
    }
}
void instr32_E7(int32_t port)
{
    if (test_privileges_for_io(port, 4)) {
        io_port_write32(port, read_reg32(EAX));
    }
}

void instr16_E8(int32_t imm16)
{
    /* call */
    push16(get_real_eip());
    if (cpu->pagefault) return;
    jmp_rel16(imm16);
}
void instr32_E8(int32_t imm32s)
{
    /* call */
    push32(get_real_eip());
    if (cpu->pagefault) return;
    set_instruction_pointer(cpu->instruction_pointer + imm32s);
    /* no dbg_assert here: the 16-bit ip class -- see "THE 16-BIT ip CLASS" in this file's header */
}
void instr16_E9(int32_t imm16)
{
    /* jmp */
    jmp_rel16(imm16);
}
void instr32_E9(int32_t imm32s)
{
    /* jmp */
    set_instruction_pointer(cpu->instruction_pointer + imm32s);
    /* no dbg_assert here: the 16-bit ip class -- see "THE 16-BIT ip CLASS" in this file's header */
}

void instr16_EA(int32_t new_ip, int32_t cs)
{
    /* jmpf */
    far_jump(new_ip, cs, false, false);
}
void instr32_EA(int32_t new_ip, int32_t cs)
{
    /* jmpf */
    far_jump(new_ip, cs, false, true);
}

void instr16_EB(int32_t imm8)
{
    /* jmp near */
    jmp_rel16(imm8);
    /* no dbg_assert here: the 16-bit ip class -- see "THE 16-BIT ip CLASS" in this file's header */
}
void instr32_EB(int32_t imm8)
{
    /* jmp near */
    set_instruction_pointer(cpu->instruction_pointer + imm8);
    /* no dbg_assert here: the 16-bit ip class -- see "THE 16-BIT ip CLASS" in this file's header */
}

void instr_EC(void)
{
    int32_t port = read_reg16(DX);
    if (test_privileges_for_io(port, 1)) {
        write_reg8(AL, io_port_read8(port));
    }
}
void instr16_ED(void)
{
    int32_t port = read_reg16(DX);
    if (test_privileges_for_io(port, 2)) {
        write_reg16(AX, io_port_read16(port));
    }
}
void instr32_ED(void)
{
    int32_t port = read_reg16(DX);
    if (test_privileges_for_io(port, 4)) {
        write_reg32(EAX, io_port_read32(port));
    }
}
void instr_EE(void)
{
    int32_t port = read_reg16(DX);
    if (test_privileges_for_io(port, 1)) {
        io_port_write8(port, read_reg8(AL));
    }
}
void instr16_EF(void)
{
    int32_t port = read_reg16(DX);
    if (test_privileges_for_io(port, 2)) {
        io_port_write16(port, read_reg16(AX));
    }
}
void instr32_EF(void)
{
    int32_t port = read_reg16(DX);
    if (test_privileges_for_io(port, 4)) {
        io_port_write32(port, read_reg32(EAX));
    }
}

/* ================================================================== *
 * 0xF0-0xFF -- the prefixes, hlt, cmc, the F6/F7 groups, the flag
 * instructions, inc/dec and the FF group
 * ================================================================== */

void instr_F0(void)
{
    /* lock */
    /* TODO
     * This triggers UD when used with
     * some instructions that don't write to memory */
    run_prefix_instruction();
}

void instr_F1(void)
{
    /* INT1
     * https://code.google.com/p/corkami/wiki/x86oddities#IceBP */
    DBG_ASSERT(false);
}

void instr_F2(void)
{
    /* repnz */
    DBG_ASSERT((cpu->prefixes & PREFIX_MASK_REP) == 0);
    cpu->prefixes |= PREFIX_REPNZ;
    run_prefix_instruction();
    cpu->prefixes = 0;
}
void instr_F3(void)
{
    /* repz */
    DBG_ASSERT((cpu->prefixes & PREFIX_MASK_REP) == 0);
    cpu->prefixes |= PREFIX_REPZ;
    run_prefix_instruction();
    cpu->prefixes = 0;
}

void instr_F4(void)
{
    if (cpu->cpl != 0) {
        /* dbg_log!("#gp hlt with cpl != 0") dropped */
        trigger_gp(0);
        return;
    }

    cpu->in_hlt = true;

    /* Try an hlt loop right now: This will run timer interrupts, and if one is
     * due it will immediately call call_interrupt_vector and continue
     * execution without an unnecessary cycle through do_run */
    if ((cpu->flags & FLAG_INTERRUPT) != 0) {
        run_hardware_timers(cpu->acpi_enabled, microtick());
        handle_irqs();
    }
    else {
        /* execution can never resume (until NMIs are supported) */
        cpu_event_halt();
    }
}
void instr_F5(void)
{
    /* cmc */
    cpu->flags = (cpu->flags | 1) ^ (int32_t)getcf();
    cpu->flags_changed &= ~1;
}

void instr_F6_0_mem(int32_t addr, int32_t imm)
{
    int32_t x = safe_read8(addr);
    if (cpu->pagefault) return;
    test8(x, imm);
}
void instr_F6_0_reg(int32_t r1, int32_t imm) { test8(read_reg8(r1), imm); }
void instr_F6_1_mem(int32_t addr, int32_t imm)
{
    int32_t x = safe_read8(addr);
    if (cpu->pagefault) return;
    test8(x, imm);
}
void instr_F6_1_reg(int32_t r1, int32_t imm) { test8(read_reg8(r1), imm); }

/* upstream writes this closure as `!x & 0xFF` -- Rust's `!` is the BITWISE complement
 * (C's `!` is logical-not; M1a-2's D3 found the same trap in arith.c's not8) */
void instr_F6_2_mem(int32_t addr) { SAFE_READ_WRITE8(addr, ~x_ & 0xFF); }
void instr_F6_2_reg(int32_t r1) { write_reg8(r1, ~read_reg8(r1)); }
void instr_F6_3_mem(int32_t addr) { SAFE_READ_WRITE8(addr, neg8(x_)); }
void instr_F6_3_reg(int32_t r1) { write_reg8(r1, neg8(read_reg8(r1))); }
void instr_F6_4_mem(int32_t addr)
{
    int32_t x = safe_read8(addr);
    if (cpu->pagefault) return;
    mul8(x);
}
void instr_F6_4_reg(int32_t r1) { mul8(read_reg8(r1)); }
void instr_F6_5_mem(int32_t addr)
{
    int32_t x = safe_read8(addr);
    if (cpu->pagefault) return;
    imul8(SEX8(x));
}
void instr_F6_5_reg(int32_t r1) { imul8(SEX8(read_reg8(r1))); }
void instr_F6_6_mem(int32_t addr)
{
    int32_t x = safe_read8(addr);
    if (cpu->pagefault) return;
    div8((uint32_t)x);
}
void instr_F6_6_reg(int32_t r1) { div8((uint32_t)read_reg8(r1)); }
void instr_F6_7_mem(int32_t addr)
{
    int32_t x = safe_read8(addr);
    if (cpu->pagefault) return;
    idiv8(SEX8(x));
}
void instr_F6_7_reg(int32_t r1) { idiv8(SEX8(read_reg8(r1))); }

void instr16_F7_0_mem(int32_t addr, int32_t imm)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    test16(x, imm);
}
void instr16_F7_0_reg(int32_t r1, int32_t imm) { test16(read_reg16(r1), imm); }
void instr16_F7_1_mem(int32_t addr, int32_t imm)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    test16(x, imm);
}
void instr16_F7_1_reg(int32_t r1, int32_t imm) { test16(read_reg16(r1), imm); }
void instr16_F7_2_mem(int32_t addr) { SAFE_READ_WRITE16(addr, ~x_ & 0xFFFF); }
void instr16_F7_2_reg(int32_t r1) { write_reg16(r1, ~read_reg16(r1)); }
void instr16_F7_3_mem(int32_t addr) { SAFE_READ_WRITE16(addr, neg16(x_)); }
void instr16_F7_3_reg(int32_t r1) { write_reg16(r1, neg16(read_reg16(r1))); }
void instr16_F7_4_mem(int32_t addr)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    mul16((uint32_t)x);
}
void instr16_F7_4_reg(int32_t r1) { mul16((uint32_t)read_reg16(r1)); }
void instr16_F7_5_mem(int32_t addr)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    imul16(SEX16(x));
}
void instr16_F7_5_reg(int32_t r1) { imul16(SEX16(read_reg16(r1))); }
void instr16_F7_6_mem(int32_t addr)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    div16((uint32_t)x);
}
void instr16_F7_6_reg(int32_t r1) { div16((uint32_t)read_reg16(r1)); }
void instr16_F7_7_mem(int32_t addr)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    idiv16(SEX16(x));
}
void instr16_F7_7_reg(int32_t r1) { idiv16(SEX16(read_reg16(r1))); }

void instr32_F7_0_mem(int32_t addr, int32_t imm)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    test32(x, imm);
}
void instr32_F7_0_reg(int32_t r1, int32_t imm) { test32(read_reg32(r1), imm); }
void instr32_F7_1_mem(int32_t addr, int32_t imm)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    test32(x, imm);
}
void instr32_F7_1_reg(int32_t r1, int32_t imm) { test32(read_reg32(r1), imm); }
void instr32_F7_2_mem(int32_t addr) { SAFE_READ_WRITE32(addr, ~x_); }
void instr32_F7_2_reg(int32_t r1) { write_reg32(r1, ~read_reg32(r1)); }
void instr32_F7_3_mem(int32_t addr) { SAFE_READ_WRITE32(addr, neg32(x_)); }
void instr32_F7_3_reg(int32_t r1) { write_reg32(r1, neg32(read_reg32(r1))); }
void instr32_F7_4_mem(int32_t addr)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    mul32(x);
}
void instr32_F7_4_reg(int32_t r1) { mul32(read_reg32(r1)); }
void instr32_F7_5_mem(int32_t addr)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    imul32(x);
}
void instr32_F7_5_reg(int32_t r1) { imul32(read_reg32(r1)); }
void instr32_F7_6_mem(int32_t addr)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    div32((uint32_t)x);
}
void instr32_F7_6_reg(int32_t r1) { div32((uint32_t)read_reg32(r1)); }
void instr32_F7_7_mem(int32_t addr)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    idiv32(x);
}
void instr32_F7_7_reg(int32_t r1) { idiv32(read_reg32(r1)); }

void instr_F8(void)
{
    /* clc */
    cpu->flags &= ~FLAG_CARRY;
    cpu->flags_changed &= ~1;
}
void instr_F9(void)
{
    /* stc */
    cpu->flags |= FLAG_CARRY;
    cpu->flags_changed &= ~1;
}

/* cli/sti.  Upstream's VME branch is behind a literal `false &&` (cpu.rs's virtual-8086
 * extension, unimplemented); it is transcribed with the same dead reachability rather than
 * deleted, so a later VME unit sees the seat and its condition. */
bool instr_FA_without_fault(void)
{
    /* cli */
    if (!cpu->protected_mode ||
        ((cpu->flags & FLAG_VM) != 0 ? getiopl() == 3 : getiopl() >= (int32_t)cpu->cpl)) {
        cpu->flags &= ~FLAG_INTERRUPT;
        return true;
    }
    else if (false && getiopl() < 3 &&
             ((cpu->flags & FLAG_VM) != 0 ? (cpu->cr[4] & CR4_VME) != 0
                                          : cpu->cpl == 3 && (cpu->cr[4] & CR4_PVI) != 0)) {
        cpu->flags &= ~FLAG_VIF;
        return true;
    }
    else {
        /* dbg_log!("cli #gp") dropped */
        return false;
    }
}
void instr_FA(void)
{
    if (!instr_FA_without_fault()) {
        trigger_gp(0);
    }
}

bool instr_FB_without_fault(void)
{
    /* sti */
    if (!cpu->protected_mode ||
        ((cpu->flags & FLAG_VM) != 0 ? getiopl() == 3 : getiopl() >= (int32_t)cpu->cpl)) {
        cpu->flags |= FLAG_INTERRUPT;
        return true;
    }
    else if (false && getiopl() < 3 && (cpu->flags & FLAG_VIP) == 0 &&
             ((cpu->flags & FLAG_VM) != 0 ? (cpu->cr[4] & CR4_VME) != 0
                                          : cpu->cpl == 3 && (cpu->cr[4] & CR4_PVI) != 0)) {
        cpu->flags |= FLAG_VIF;
        return true;
    }
    else {
        /* dbg_log!("sti #gp") dropped */
        return false;
    }
}
void instr_FB(void)
{
    if (!instr_FB_without_fault()) {
        trigger_gp(0);
    }
    else {
        cpu->prefixes = 0;
        cpu->previous_ip = cpu->instruction_pointer;
        cpu->instruction_counter += 1;
        int32_t opcode = read_imm8();
        if (cpu->pagefault) return;
        run_instruction(opcode | (is_osize_32() ? 1 : 0) << 8);

        handle_irqs();
    }
}

void instr_FC(void)
{
    /* cld */
    cpu->flags &= ~FLAG_DIRECTION;
}
void instr_FD(void)
{
    /* std */
    cpu->flags |= FLAG_DIRECTION;
}

void instr_FE_0_mem(int32_t addr) { SAFE_READ_WRITE8(addr, inc8(x_)); }
void instr_FE_0_reg(int32_t r1) { write_reg8(r1, inc8(read_reg8(r1))); }
void instr_FE_1_mem(int32_t addr) { SAFE_READ_WRITE8(addr, dec8(x_)); }
void instr_FE_1_reg(int32_t r1) { write_reg8(r1, dec8(read_reg8(r1))); }

void instr16_FF_0_mem(int32_t addr) { SAFE_READ_WRITE16(addr, inc16(x_)); }
void instr16_FF_0_reg(int32_t r1) { write_reg16(r1, inc16(read_reg16(r1))); }
void instr16_FF_1_mem(int32_t addr) { SAFE_READ_WRITE16(addr, dec16(x_)); }
void instr16_FF_1_reg(int32_t r1) { write_reg16(r1, dec16(read_reg16(r1))); }

void instr16_FF_2_helper(int32_t data)
{
    /* call near */
    uint64_t cs = get_seg_cs();
    push16(get_real_eip());
    if (cpu->pagefault) return;
    set_instruction_pointer(cs + data);
    /* no dbg_assert here: the 16-bit ip class -- see "THE 16-BIT ip CLASS" in this file's header */
}
void instr16_FF_2_mem(int32_t addr)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    instr16_FF_2_helper(x);
}
void instr16_FF_2_reg(int32_t r1) { instr16_FF_2_helper(read_reg16(r1)); }

void instr16_FF_3_reg(int32_t _r)
{
    /* dbg_log!("callf #ud") dropped */
    (void)_r;
    trigger_ud();
}
void instr16_FF_3_mem(int32_t addr)
{
    /* callf */
    int32_t new_ip = safe_read16(addr);
    if (cpu->pagefault) return;
    int32_t new_cs = safe_read16(addr + 2);
    if (cpu->pagefault) return;
    far_jump(new_ip, new_cs, true, false);
}

void instr16_FF_4_helper(int32_t data)
{
    /* jmp near */
    set_instruction_pointer(get_seg_cs() + data);
    /* no dbg_assert here: the 16-bit ip class -- see "THE 16-BIT ip CLASS" in this file's header */
}
void instr16_FF_4_mem(int32_t addr)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    instr16_FF_4_helper(x);
}
void instr16_FF_4_reg(int32_t r1) { instr16_FF_4_helper(read_reg16(r1)); }

void instr16_FF_5_reg(int32_t _r)
{
    /* dbg_log!("jmpf #ud") dropped */
    (void)_r;
    trigger_ud();
}
void instr16_FF_5_mem(int32_t addr)
{
    /* jmpf */
    int32_t new_ip = safe_read16(addr);
    if (cpu->pagefault) return;
    int32_t new_cs = safe_read16(addr + 2);
    if (cpu->pagefault) return;
    far_jump(new_ip, new_cs, false, false);
}
void instr16_FF_6_mem(int32_t addr)
{
    int32_t x = safe_read16(addr);
    if (cpu->pagefault) return;
    push16(x);
    if (cpu->pagefault) return;
}
void instr16_FF_6_reg(int32_t r1)
{
    push16(read_reg16(r1));
    if (cpu->pagefault) return;
}

void instr32_FF_0_mem(int32_t addr) { SAFE_READ_WRITE32(addr, inc32(x_)); }
void instr32_FF_0_reg(int32_t r1) { write_reg32(r1, inc32(read_reg32(r1))); }
void instr32_FF_1_mem(int32_t addr) { SAFE_READ_WRITE32(addr, dec32(x_)); }
void instr32_FF_1_reg(int32_t r1) { write_reg32(r1, dec32(read_reg32(r1))); }

void instr32_FF_2_helper(int32_t data)
{
    /* call near */
    uint64_t cs = get_seg_cs();
    push32(get_real_eip());
    if (cpu->pagefault) return;
    /* no dbg_assert here: the 16-bit ip class -- see "THE 16-BIT ip CLASS" in this file's header */
    set_instruction_pointer(cs + data);
}
void instr32_FF_2_mem(int32_t addr)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    instr32_FF_2_helper(x);
}
void instr32_FF_2_reg(int32_t r1) { instr32_FF_2_helper(read_reg32(r1)); }

void instr32_FF_3_reg(int32_t _r)
{
    /* dbg_log!("callf #ud") dropped */
    (void)_r;
    trigger_ud();
}
void instr32_FF_3_mem(int32_t addr)
{
    /* callf */
    int32_t new_ip = safe_read32s(addr);
    if (cpu->pagefault) return;
    int32_t new_cs = safe_read16(addr + 4);
    if (cpu->pagefault) return;
    /* upstream instructions.rs:2462-2469: `if !*protected_mode || vm86_mode() { if 0 !=
     * new_ip as u32 & 0xFFFF0000 { dbg_assert!(false); } }` -- dropped AT THE SITE
     * (PERF-FIX-2; the second form of the 16-bit ip class, see this file's header). */
    far_jump(new_ip, new_cs, true, true);
}

void instr32_FF_4_helper(int32_t data)
{
    /* jmp near */
    /* no dbg_assert here: the 16-bit ip class -- see "THE 16-BIT ip CLASS" in this file's header */
    set_instruction_pointer(get_seg_cs() + data);
}
void instr32_FF_4_mem(int32_t addr)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    instr32_FF_4_helper(x);
}
void instr32_FF_4_reg(int32_t r1) { instr32_FF_4_helper(read_reg32(r1)); }

void instr32_FF_5_reg(int32_t _r)
{
    /* dbg_log!("jmpf #ud") dropped */
    (void)_r;
    trigger_ud();
}
void instr32_FF_5_mem(int32_t addr)
{
    /* jmpf */
    int32_t new_ip = safe_read32s(addr);
    if (cpu->pagefault) return;
    int32_t new_cs = safe_read16(addr + 4);
    if (cpu->pagefault) return;
    /* upstream instructions.rs:2490-2494: same guard and `dbg_assert!(false)` as callf
     * above -- dropped AT THE SITE (PERF-FIX-2, this file's header). */
    far_jump(new_ip, new_cs, false, true);
}
void instr32_FF_6_mem(int32_t addr)
{
    int32_t x = safe_read32s(addr);
    if (cpu->pagefault) return;
    push32(x);
    if (cpu->pagefault) return;
}
void instr32_FF_6_reg(int32_t r1)
{
    push32(read_reg32(r1));
    if (cpu->pagefault) return;
}

/* ================================================================== *
 * THE D8..DF GROUP -- the x87 entry points (instructions.rs:1643-2003).
 *
 * This block was deferred to M1b-3 by M1a-3 (instructions.c's file header records why:
 * the arguments are fpu.c's F80 types, so porting them there would have frozen fpu.c's
 * interface from outside its owner).  Every function below is a transcription of the
 * upstream Rust line(s) cited in the group comments; the thin calls mean the semantics
 * live in fpu.c and only the OPCODE-to-OPERATION map lives here.
 *
 * The `_mem` form's argument is the address the generated dispatcher already resolved
 * (`modrm_resolve`, with its own fault check); the `_reg` form's is `modrm_byte & 7`.
 * A fallible load is hoisted into a local and checked before use, exactly like the rest
 * of the table -- `fpu_fadd(0, fpu_load_m32(addr))` cannot be written directly for the
 * same reason `instr_04`-style instructions cannot: the sentinel must never be used.
 * ================================================================== */

/* instructions.rs:1643-1658 -- D8 /0../7, m32real */
void instr_D8_0_mem(int32_t addr) { f80_t x = fpu_load_m32(addr); if (cpu->pagefault) return; fpu_fadd(0, x); }
void instr_D8_0_reg(int32_t r) { fpu_fadd(0, fpu_get_sti(r)); }
void instr_D8_1_mem(int32_t addr) { f80_t x = fpu_load_m32(addr); if (cpu->pagefault) return; fpu_fmul(0, x); }
void instr_D8_1_reg(int32_t r) { fpu_fmul(0, fpu_get_sti(r)); }
void instr_D8_2_mem(int32_t addr) { f80_t x = fpu_load_m32(addr); if (cpu->pagefault) return; fpu_fcom(x); }
void instr_D8_2_reg(int32_t r) { fpu_fcom(fpu_get_sti(r)); }
void instr_D8_3_mem(int32_t addr) { f80_t x = fpu_load_m32(addr); if (cpu->pagefault) return; fpu_fcomp(x); }
void instr_D8_3_reg(int32_t r) { fpu_fcomp(fpu_get_sti(r)); }
void instr_D8_4_mem(int32_t addr) { f80_t x = fpu_load_m32(addr); if (cpu->pagefault) return; fpu_fsub(0, x); }
void instr_D8_4_reg(int32_t r) { fpu_fsub(0, fpu_get_sti(r)); }
void instr_D8_5_mem(int32_t addr) { f80_t x = fpu_load_m32(addr); if (cpu->pagefault) return; fpu_fsubr(0, x); }
void instr_D8_5_reg(int32_t r) { fpu_fsubr(0, fpu_get_sti(r)); }
void instr_D8_6_mem(int32_t addr) { f80_t x = fpu_load_m32(addr); if (cpu->pagefault) return; fpu_fdiv(0, x); }
void instr_D8_6_reg(int32_t r) { fpu_fdiv(0, fpu_get_sti(r)); }
void instr_D8_7_mem(int32_t addr) { f80_t x = fpu_load_m32(addr); if (cpu->pagefault) return; fpu_fdivr(0, x); }
void instr_D8_7_reg(int32_t r) { fpu_fdivr(0, fpu_get_sti(r)); }

/* instructions.rs:1660-1752 -- D9, the load/store/control group (the 16- and 32-bit
 * forms differ in the environment layout only; both take the operand-size prefix) */
void instr16_D9_0_mem(int32_t addr) { fpu_fldm32(addr); }
void instr16_D9_0_reg(int32_t r) { fpu_push(fpu_get_sti(r)); }
void instr16_D9_1_mem(int32_t addr) { (void)addr; trigger_ud(); } /* "d9/1" upstream */
void instr16_D9_1_reg(int32_t r) { fpu_fxch(r); }
void instr16_D9_2_mem(int32_t addr) { fpu_fstm32(addr); }
void instr16_D9_2_reg(int32_t r) { if (r != 0) trigger_ud(); }
void instr16_D9_3_mem(int32_t addr) { fpu_fstm32p(addr); }
void instr16_D9_3_reg(int32_t r) { fpu_fstp(r); }
void instr16_D9_4_mem(int32_t addr)
{
    readable_or_pagefault(addr, 14);
    if (cpu->pagefault) return;
    fpu_fldenv16(addr);
}
void instr32_D9_4_mem(int32_t addr)
{
    readable_or_pagefault(addr, 28);
    if (cpu->pagefault) return;
    fpu_fldenv32(addr);
}
void instr16_D9_4_reg(int32_t r)
{
    switch (r) {
    case 0: fpu_fchs(); break;
    case 1: fpu_fabs(); break;
    case 4: fpu_ftst(); break;
    case 5: fpu_fxam(); break;
    default: trigger_ud(); break;
    }
}
void instr16_D9_5_mem(int32_t addr) { fpu_fldcw(addr); }
void instr16_D9_5_reg(int32_t r)
{
    /* fld1/fldl2t/fldl2e/fldpi/fldlg2/fldln2/fldz */
    switch (r) {
    case 0: fpu_push(F80_ONE); break;
    case 1: fpu_push(f80_div(F80_LN_10, F80_LN_2)); break;
    case 2: fpu_push(F80_LOG2_E); break;
    case 3: fpu_push(F80_PI); break;
    case 4: fpu_push(f80_div(F80_LN_2, F80_LN_10)); break;
    case 5: fpu_push(F80_LN_2); break;
    case 6: fpu_push(F80_ZERO); break;
    default: trigger_ud(); break; /* r == 7 */
    }
}
void instr16_D9_6_mem(int32_t addr)
{
    writable_or_pagefault(addr, 14);
    if (cpu->pagefault) return;
    fpu_fstenv16(addr);
}
void instr32_D9_6_mem(int32_t addr)
{
    writable_or_pagefault(addr, 28);
    if (cpu->pagefault) return;
    fpu_fstenv32(addr);
}
void instr16_D9_6_reg(int32_t r)
{
    switch (r) {
    case 0: fpu_f2xm1(); break;
    case 1: fpu_fyl2x(); break;
    case 2: fpu_fptan(); break;
    case 3: fpu_fpatan(); break;
    case 4: fpu_fxtract(); break;
    case 5: fpu_fprem(true); break; /* fprem1 */
    case 6: fpu_fdecstp(); break;
    case 7: fpu_fincstp(); break;
    default: DBG_ASSERT(false); break; /* unreachable: r is modrm>>3&7 */
    }
}
void instr16_D9_7_mem(int32_t addr) { fpu_fstcw(addr); }
void instr16_D9_7_reg(int32_t r)
{
    switch (r) {
    case 0: fpu_fprem(false); break;
    case 1: fpu_fyl2xp1(); break;
    case 2: fpu_fsqrt(); break;
    case 3: fpu_fsincos(); break;
    case 4: fpu_frndint(); break;
    case 5: fpu_fscale(); break;
    case 6: fpu_fsin(); break;
    case 7: fpu_fcos(); break;
    default: DBG_ASSERT(false); break; /* unreachable */
    }
}
/* instructions.rs:1759-1778 -- the 32-bit delegation (identical bodies except the
 * environment forms above, which upstream writes out) */
void instr32_D9_0_reg(int32_t r) { instr16_D9_0_reg(r); }
void instr32_D9_1_reg(int32_t r) { instr16_D9_1_reg(r); }
void instr32_D9_2_reg(int32_t r) { instr16_D9_2_reg(r); }
void instr32_D9_3_reg(int32_t r) { instr16_D9_3_reg(r); }
void instr32_D9_4_reg(int32_t r) { instr16_D9_4_reg(r); }
void instr32_D9_5_reg(int32_t r) { instr16_D9_5_reg(r); }
void instr32_D9_6_reg(int32_t r) { instr16_D9_6_reg(r); }
void instr32_D9_7_reg(int32_t r) { instr16_D9_7_reg(r); }
void instr32_D9_0_mem(int32_t addr) { instr16_D9_0_mem(addr); }
void instr32_D9_1_mem(int32_t addr) { instr16_D9_1_mem(addr); }
void instr32_D9_2_mem(int32_t addr) { instr16_D9_2_mem(addr); }
void instr32_D9_3_mem(int32_t addr) { instr16_D9_3_mem(addr); }
void instr32_D9_5_mem(int32_t addr) { instr16_D9_5_mem(addr); }
void instr32_D9_7_mem(int32_t addr) { instr16_D9_7_mem(addr); }

/* instructions.rs:1780-1804 -- DA /0../7, m32int (and the FCMOVcc /4../7 group) */
void instr_DA_0_mem(int32_t addr) { f80_t x = fpu_load_i32(addr); if (cpu->pagefault) return; fpu_fadd(0, x); }
void instr_DA_1_mem(int32_t addr) { f80_t x = fpu_load_i32(addr); if (cpu->pagefault) return; fpu_fmul(0, x); }
void instr_DA_2_mem(int32_t addr) { f80_t x = fpu_load_i32(addr); if (cpu->pagefault) return; fpu_fcom(x); }
void instr_DA_3_mem(int32_t addr) { f80_t x = fpu_load_i32(addr); if (cpu->pagefault) return; fpu_fcomp(x); }
void instr_DA_4_mem(int32_t addr) { f80_t x = fpu_load_i32(addr); if (cpu->pagefault) return; fpu_fsub(0, x); }
void instr_DA_5_mem(int32_t addr) { f80_t x = fpu_load_i32(addr); if (cpu->pagefault) return; fpu_fsubr(0, x); }
void instr_DA_6_mem(int32_t addr) { f80_t x = fpu_load_i32(addr); if (cpu->pagefault) return; fpu_fdiv(0, x); }
void instr_DA_7_mem(int32_t addr) { f80_t x = fpu_load_i32(addr); if (cpu->pagefault) return; fpu_fdivr(0, x); }
void instr_DA_0_reg(int32_t r) { fpu_fcmovcc(test_b(), r); }
void instr_DA_1_reg(int32_t r) { fpu_fcmovcc(test_z(), r); }
void instr_DA_2_reg(int32_t r) { fpu_fcmovcc(test_be(), r); }
void instr_DA_3_reg(int32_t r) { fpu_fcmovcc(test_p(), r); }
void instr_DA_4_reg(int32_t r) { (void)r; trigger_ud(); }
void instr_DA_5_reg(int32_t r)
{
    if (r == 1) fpu_fucompp();
    else trigger_ud();
}
void instr_DA_6_reg(int32_t r) { (void)r; trigger_ud(); }
void instr_DA_7_reg(int32_t r) { (void)r; trigger_ud(); }

/* instructions.rs:1806-1860 -- DB /0../7, m32int/m80real + finit/fclex + the FCMOVcc
 * complements */
void instr_DB_0_mem(int32_t addr) { fpu_fildm32(addr); }
void instr_DB_1_mem(int32_t addr) { fpu_fisttpm32(addr); }
void instr_DB_2_mem(int32_t addr) { fpu_fistm32(addr); }
void instr_DB_3_mem(int32_t addr) { fpu_fistm32p(addr); }
void instr_DB_4_mem(int32_t addr) { (void)addr; trigger_ud(); }
void instr_DB_5_mem(int32_t addr)
{
    readable_or_pagefault(addr, 10);
    if (cpu->pagefault) return;
    fpu_fldm80(addr);
}
void instr_DB_6_mem(int32_t addr) { (void)addr; trigger_ud(); }
void instr_DB_7_mem(int32_t addr) { fpu_fst80p(addr); }
void instr_DB_0_reg(int32_t r) { fpu_fcmovcc(!test_b(), r); }
void instr_DB_1_reg(int32_t r) { fpu_fcmovcc(!test_z(), r); }
void instr_DB_2_reg(int32_t r) { fpu_fcmovcc(!test_be(), r); }
void instr_DB_3_reg(int32_t r) { fpu_fcmovcc(!test_p(), r); }
void instr_DB_4_reg(int32_t r)
{
    if (r == 3) {
        fpu_finit();
    }
    else if (r == 4 || r == 1 || r == 0) {
        /* fsetpm, fdisi, fneni; treated as nop */
    }
    else if (r == 2) {
        fpu_fclex();
    }
    else {
        trigger_ud();
    }
}
void instr_DB_5_reg(int32_t r) { fpu_fucomi(r); }
void instr_DB_6_reg(int32_t r) { fpu_fcomi(r); }
void instr_DB_7_reg(int32_t r) { (void)r; trigger_ud(); }

/* instructions.rs:1862-1878 -- DC, m64real (the reg forms are the reversed-operand
 * arithmetic on st(i)) */
void instr_DC_0_mem(int32_t addr) { f80_t x = fpu_load_m64(addr); if (cpu->pagefault) return; fpu_fadd(0, x); }
void instr_DC_1_mem(int32_t addr) { f80_t x = fpu_load_m64(addr); if (cpu->pagefault) return; fpu_fmul(0, x); }
void instr_DC_2_mem(int32_t addr) { f80_t x = fpu_load_m64(addr); if (cpu->pagefault) return; fpu_fcom(x); }
void instr_DC_3_mem(int32_t addr) { f80_t x = fpu_load_m64(addr); if (cpu->pagefault) return; fpu_fcomp(x); }
void instr_DC_4_mem(int32_t addr) { f80_t x = fpu_load_m64(addr); if (cpu->pagefault) return; fpu_fsub(0, x); }
void instr_DC_5_mem(int32_t addr) { f80_t x = fpu_load_m64(addr); if (cpu->pagefault) return; fpu_fsubr(0, x); }
void instr_DC_6_mem(int32_t addr) { f80_t x = fpu_load_m64(addr); if (cpu->pagefault) return; fpu_fdiv(0, x); }
void instr_DC_7_mem(int32_t addr) { f80_t x = fpu_load_m64(addr); if (cpu->pagefault) return; fpu_fdivr(0, x); }
void instr_DC_0_reg(int32_t r) { fpu_fadd(r, fpu_get_sti(r)); }
void instr_DC_1_reg(int32_t r) { fpu_fmul(r, fpu_get_sti(r)); }
void instr_DC_2_reg(int32_t r) { fpu_fcom(fpu_get_sti(r)); }
void instr_DC_3_reg(int32_t r) { fpu_fcomp(fpu_get_sti(r)); }
void instr_DC_4_reg(int32_t r) { fpu_fsub(r, fpu_get_sti(r)); }
void instr_DC_5_reg(int32_t r) { fpu_fsubr(r, fpu_get_sti(r)); }
void instr_DC_6_reg(int32_t r) { fpu_fdiv(r, fpu_get_sti(r)); }
void instr_DC_7_reg(int32_t r) { fpu_fdivr(r, fpu_get_sti(r)); }

/* instructions.rs:1880-1940 -- DD, m64int / m80real + fsave/frstor + fnstsw */
void instr16_DD_0_mem(int32_t addr) { fpu_fldm64(addr); }
void instr16_DD_1_mem(int32_t addr) { fpu_fisttpm64(addr); }
void instr16_DD_2_mem(int32_t addr) { fpu_fstm64(addr); }
void instr16_DD_3_mem(int32_t addr) { fpu_fstm64p(addr); }
void instr16_DD_4_mem(int32_t addr) { fpu_frstor16(addr); }
void instr32_DD_4_mem(int32_t addr) { fpu_frstor32(addr); }
void instr16_DD_5_mem(int32_t addr) { (void)addr; trigger_ud(); } /* "dd/5" upstream */
void instr16_DD_6_mem(int32_t addr) { fpu_fsave16(addr); }
void instr32_DD_6_mem(int32_t addr) { fpu_fsave32(addr); }
void instr16_DD_7_mem(int32_t addr) { fpu_fnstsw_mem(addr); }
void instr16_DD_0_reg(int32_t r) { fpu_ffree(r); }
void instr16_DD_1_reg(int32_t r) { fpu_fxch(r); }
void instr16_DD_2_reg(int32_t r) { fpu_fst(r); }
void instr16_DD_3_reg(int32_t r) { fpu_fstp(r); }
void instr16_DD_4_reg(int32_t r) { fpu_fucom(r); }
void instr16_DD_5_reg(int32_t r) { fpu_fucomp(r); }
void instr16_DD_6_reg(int32_t r) { (void)r; trigger_ud(); }
void instr16_DD_7_reg(int32_t r) { (void)r; trigger_ud(); }
void instr32_DD_0_reg(int32_t r) { instr16_DD_0_reg(r); }
void instr32_DD_1_reg(int32_t r) { instr16_DD_1_reg(r); }
void instr32_DD_2_reg(int32_t r) { instr16_DD_2_reg(r); }
void instr32_DD_3_reg(int32_t r) { instr16_DD_3_reg(r); }
void instr32_DD_4_reg(int32_t r) { instr16_DD_4_reg(r); }
void instr32_DD_5_reg(int32_t r) { instr16_DD_5_reg(r); }
void instr32_DD_6_reg(int32_t r) { instr16_DD_6_reg(r); }
void instr32_DD_7_reg(int32_t r) { instr16_DD_7_reg(r); }
void instr32_DD_0_mem(int32_t addr) { instr16_DD_0_mem(addr); }
void instr32_DD_1_mem(int32_t addr) { instr16_DD_1_mem(addr); }
void instr32_DD_2_mem(int32_t addr) { instr16_DD_2_mem(addr); }
void instr32_DD_3_mem(int32_t addr) { instr16_DD_3_mem(addr); }
void instr32_DD_5_mem(int32_t addr) { instr16_DD_5_mem(addr); }
void instr32_DD_7_mem(int32_t addr) { instr16_DD_7_mem(addr); }

/* instructions.rs:1942-1985 -- DE, m16int (the reg forms pop after operating) */
void instr_DE_0_mem(int32_t addr) { f80_t x = fpu_load_i16(addr); if (cpu->pagefault) return; fpu_fadd(0, x); }
void instr_DE_1_mem(int32_t addr) { f80_t x = fpu_load_i16(addr); if (cpu->pagefault) return; fpu_fmul(0, x); }
void instr_DE_2_mem(int32_t addr) { f80_t x = fpu_load_i16(addr); if (cpu->pagefault) return; fpu_fcom(x); }
void instr_DE_3_mem(int32_t addr) { f80_t x = fpu_load_i16(addr); if (cpu->pagefault) return; fpu_fcomp(x); }
void instr_DE_4_mem(int32_t addr) { f80_t x = fpu_load_i16(addr); if (cpu->pagefault) return; fpu_fsub(0, x); }
void instr_DE_5_mem(int32_t addr) { f80_t x = fpu_load_i16(addr); if (cpu->pagefault) return; fpu_fsubr(0, x); }
void instr_DE_6_mem(int32_t addr) { f80_t x = fpu_load_i16(addr); if (cpu->pagefault) return; fpu_fdiv(0, x); }
void instr_DE_7_mem(int32_t addr) { f80_t x = fpu_load_i16(addr); if (cpu->pagefault) return; fpu_fdivr(0, x); }
void instr_DE_0_reg(int32_t r) { fpu_fadd(r, fpu_get_sti(r)); fpu_pop(); }
void instr_DE_1_reg(int32_t r) { fpu_fmul(r, fpu_get_sti(r)); fpu_pop(); }
void instr_DE_2_reg(int32_t r) { fpu_fcom(fpu_get_sti(r)); fpu_pop(); }
void instr_DE_3_reg(int32_t r)
{
    if (r == 1) {
        fpu_fcomp(fpu_get_sti(r));
        fpu_pop();
    }
    else {
        trigger_ud();
    }
}
void instr_DE_4_reg(int32_t r) { fpu_fsub(r, fpu_get_sti(r)); fpu_pop(); }
void instr_DE_5_reg(int32_t r) { fpu_fsubr(r, fpu_get_sti(r)); fpu_pop(); }
void instr_DE_6_reg(int32_t r) { fpu_fdiv(r, fpu_get_sti(r)); fpu_pop(); }
void instr_DE_7_reg(int32_t r) { fpu_fdivr(r, fpu_get_sti(r)); fpu_pop(); }

/* instructions.rs:1987-2003 -- DF, m16int/m64int + fbld/fbstp + fnstsw ax */
void instr_DF_0_mem(int32_t addr) { fpu_fildm16(addr); }
void instr_DF_1_mem(int32_t addr) { fpu_fisttpm16(addr); }
void instr_DF_2_mem(int32_t addr) { fpu_fistm16(addr); }
void instr_DF_3_mem(int32_t addr) { fpu_fistm16p(addr); }
void instr_DF_4_mem(int32_t addr) { (void)addr; fpu_unimpl(); } /* fbld */
void instr_DF_5_mem(int32_t addr) { fpu_fildm64(addr); }
void instr_DF_6_mem(int32_t addr)
{
    writable_or_pagefault(addr, 10);
    if (cpu->pagefault) return;
    fpu_fbstp(addr);
}
void instr_DF_7_mem(int32_t addr) { fpu_fistm64p(addr); }
void instr_DF_0_reg(int32_t r) { fpu_ffree(r); fpu_pop(); }
void instr_DF_1_reg(int32_t r) { fpu_fxch(r); }
void instr_DF_2_reg(int32_t r) { fpu_fstp(r); }
void instr_DF_3_reg(int32_t r) { fpu_fstp(r); }
void instr_DF_4_reg(int32_t r)
{
    if (r == 0) fpu_fnstsw_reg();
    else trigger_ud();
}
void instr_DF_5_reg(int32_t r) { fpu_fucomip(r); }
void instr_DF_6_reg(int32_t r) { fpu_fcomip(r); }
void instr_DF_7_reg(int32_t r) { (void)r; trigger_ud(); }
