/*
 * cpu_state.c -- the CPU state singleton, its reset, the cached-state byte and the
 * register accessors.  Port of v86/src/rust/cpu/cpu.rs:4128-4196 (accessors),
 * cpu.rs:3346-3363 (update_state_flags/has_flat_segmentation), cpu.rs:4671-4760
 * (reset_cpu) and global_pointers.rs (the layout).
 */

#include <stdio.h> /* the invariant's named stop (S3) */

#include "cpu_state.h"
#include "cpu.h"   /* unimplemented_instruction -- S3's high-half stop */
#include "modrm.h" /* S5a: the PREFIX_REX_* bits the byte-register mapping tests */
#include "paging.h" /* reset_cpu must flush the TLB (full_clear_tlb) */

/* global_pointers.rs used absolute offsets into a shared wasm memory; the C port has
 * one CPU, so the plan's "one global `cpu` pointer" (ss1) is the whole story. */
cpu_state_t *cpu;

/* ------------------------------------------------------------------ *
 * register accessors -- cpu.rs:4128-4196, ported verbatim
 * ------------------------------------------------------------------ */

/* reg8 is the byte view of the register file: index -> byte offset mapping.  S1 CHANGED
 * THE FORMULA (results/x86-64-plan.md ss4) because the element is now 8 bytes:
 *
 *     (index << 2 & 12) | (index >> 2 & 1)   -- the old, flat 4-byte layout
 *     (index & 3) * 8 + (index >> 2 & 1)     -- the widened one
 *
 * AL/CL/DL/BL (indices 0-3) are the first byte of reg64[0..3] and AH/CH/DH/BH (4-7) the
 * SECOND byte of the same slots.  A formula left unchanged here would put AH on ESP's
 * first byte -- the port made exactly that mistake once already in the JIT's register
 * mapping (jit_codegen.c:198-201) and the guest's stack pointer came out corrupted.
 *
 * S5a ADDED THE THIRD AND FOURTH CASES, and they are the ones the SDM's REX rule is about:
 *
 *   index 8-15            -> the first byte of reg64[index]      (SPL/BPL/SIL/DIL and
 *                                                                  R8B-R15B)
 *   4-7 WITH A REX BYTE   -> the first byte of reg64[index]      (SPL/BPL/SIL/DIL instead of
 *                                                                  AH/CH/DH/BH)
 *   4-7 WITHOUT ONE       -> the second byte of reg64[index-4]   (AH/CH/DH/BH, unchanged)
 *
 * "WITH A REX BYTE" is PREFIX_REX_PRESENT, not PREFIX_REX_W: `40 B4 xx` is `mov spl, imm8`,
 * MEASURED against the host; testing W there would read AH and silently corrupt the byte the
 * corpus's mov-rex-byte-regs case compares.  Nothing outside long mode sets the bit, so the
 * first two arms are unreachable from the 32-bit battery. */
static int32_t get_reg8_index(int32_t index)
{
    if (index >= 8) return index * 8;
    if (index >= 4 && (cpu->prefixes & PREFIX_REX_PRESENT) != 0) return index * 8;
    return (index & 3) * 8 + (index >> 2 & 1);
}

int32_t read_reg8(int32_t index)
{
    return ((const uint8_t *)cpu->reg64)[get_reg8_index(index)];
}

void write_reg8(int32_t index, int32_t value)
{
    ((uint8_t *)cpu->reg64)[get_reg8_index(index)] = (uint8_t)value;
}

/* get_reg16_index(index) == index << 1 in upstream's u16 UNITS (cpu.rs:4142); with 8-byte
 * elements the element index is 2*index in U32 units, i.e. index << 2 in u16 units, and
 * the fill order inside a slot is unchanged.  Getting this wrong silently aliases DX to CX,
 * which is why the S1 selftest pins a 16-bit write against a neighbouring slot. */
int32_t read_reg16(int32_t index) { return ((const uint16_t *)cpu->reg64)[index << 2]; }

void write_reg16(int32_t index, int32_t value)
{
    ((uint16_t *)cpu->reg64)[index << 2] = (uint16_t)value;
}

/* THE 32-BIT ACCESSORS KEEP THEIR WIDTH (ss4's "81 references, all mechanical"): a read
 * returns the low half, and a write ZEROES the high half -- x86-64's own rule for a
 * 32-bit destination register, so the file is long-mode-correct in advance and the
 * interpreter and the JIT agree about what a register holds. */
int32_t read_reg32(int32_t index) { return (int32_t)(uint32_t)cpu->reg64[index]; }

void write_reg32(int32_t index, int32_t value) { cpu->reg64[index] = (uint32_t)value; }

/* S5a: the 64-bit operand form.  There is nothing to extend or mask -- in long mode a
 * 64-bit operand IS the whole slot -- but the pair exists so a 64-bit body never indexes
 * reg64[] directly (the 32-bit write above zeroes the high half, and a body that bypassed
 * the accessor would be the one place that rule could be forgotten). */
uint64_t read_reg64(int32_t index) { return cpu->reg64[index]; }

void write_reg64(int32_t index, uint64_t value) { cpu->reg64[index] = value; }

/* mmx registers are the low 64 bits of the fpu_st slots (cpu.rs:4162-4173). */
int32_t read_mmx32s(int32_t r) { return (int32_t)cpu->fpu_st[r].mantissa; }

uint64_t read_mmx64s(int32_t r) { return cpu->fpu_st[r].mantissa; }

void write_mmx_reg64(int32_t r, uint64_t data)
{
    cpu->fpu_st[r].mantissa = data;
    cpu->fpu_st[r].sign_exponent = 0xFFFF; /* cpu.rs:4168 */
}

float read_xmm_f32(int32_t r) { return cpu->reg_xmm[r].f32[0]; }
int32_t read_xmm32(int32_t r) { return (int32_t)cpu->reg_xmm[r].u32[0]; }
uint64_t read_xmm64s(int32_t r) { return cpu->reg_xmm[r].u64[0]; }
reg128 read_xmm128s(int32_t r) { return cpu->reg_xmm[r]; }

void write_xmm_f32(int32_t r, float data) { cpu->reg_xmm[r].f32[0] = data; }
void write_xmm32(int32_t r, int32_t data) { cpu->reg_xmm[r].i32[0] = data; }
void write_xmm64(int32_t r, uint64_t data) { cpu->reg_xmm[r].u64[0] = data; }
void write_xmm_f64(int32_t r, double data) { cpu->reg_xmm[r].f64[0] = data; }

void write_xmm128(int32_t r, int32_t i0, int32_t i1, int32_t i2, int32_t i3)
{
    reg128 x;
    x.u32[0] = (uint32_t)i0;
    x.u32[1] = (uint32_t)i1;
    x.u32[2] = (uint32_t)i2;
    x.u32[3] = (uint32_t)i3;
    cpu->reg_xmm[r] = x;
}

void write_xmm128_2(int32_t r, uint64_t i0, uint64_t i1)
{
    reg128 x;
    x.u64[0] = i0;
    x.u64[1] = i1;
    cpu->reg_xmm[r] = x;
}

void write_xmm_reg128(int32_t r, reg128 data) { cpu->reg_xmm[r] = data; }

/* ------------------------------------------------------------------ *
 * cached state byte -- state_flags.rs:3-26 + cpu.rs:3345-3363
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 * S3: the widened address fields' high-half invariant -- see cpu_state.h for the claim
 * being asserted and why it is a DETECTOR rather than a type fact.
 * ------------------------------------------------------------------ */
bool cpu_address_widening_check_enabled = true;

const char *cpu_address_widening_leak(void)
{
    /* IN LONG MODE a linear address is 64 bits and there is nothing to assert: the high
     * half is where the address lives.  (Before S5 nothing can set EFER.LMA, so this arm is
     * unreachable today -- stated rather than implied; S5's mode transition is what makes it
     * live, and the S2 selftest already exercises the predicate directly.) */
    if ((cpu->efer & EFER_LMA) != 0) return NULL;

    if (cpu->instruction_pointer >> 32) return "instruction_pointer";
    if (cpu->previous_ip >> 32) return "previous_ip";
    if (cpu->eip_phys >> 32) return "eip_phys";
    /* last_virt_eip's sentinel is high-half clean by construction (VIRT_EIP_NONE) */
    if (cpu->last_virt_eip >> 32) return "last_virt_eip";
    if (cpu->idtr_offset >> 32) return "idtr_offset";
    if (cpu->gdtr_offset >> 32) return "gdtr_offset";
    for (int i = 0; i < 8; i++) {
        if (cpu->segment_offsets[i] >> 32) {
            /* one name per slot, so the report says WHICH base leaked */
            static const char *const names[8] = {
                "segment_offsets[ES]", "segment_offsets[CS]", "segment_offsets[SS]",
                "segment_offsets[DS]", "segment_offsets[FS]", "segment_offsets[GS]",
                "segment_offsets[TR]", "segment_offsets[LDTR]",
            };
            return names[i];
        }
    }
    return NULL;
}

void cpu_assert_address_widening(const char *where)
{
    if (!cpu_address_widening_check_enabled) return;
    const char *leak = cpu_address_widening_leak();
    if (leak != NULL) {
        /* NOT a silent mask and not a wrap: the field's high half is a value the 32-bit
         * model cannot form, so the machine stops BY NAME (cpu.h's stop channel). */
        char stop[160];
        snprintf(stop, sizeof(stop),
                 "widened address field %s has a non-zero high half outside long mode"
                 " (at %s)", leak, where);
        unimplemented_instruction(stop);
    }
}

bool has_flat_segmentation(void)
{
    /* cs/ss can't be null (cpu.rs:3355-3361) */
    return cpu->segment_offsets[SS] == 0 && !cpu->segment_is_null[DS] &&
           cpu->segment_offsets[DS] == 0 && cpu->segment_offsets[CS] == 0;
}

void update_state_flags(void)
{
    /* state_flags.rs:17-26 dbg_asserts that no bit outside the four masks is set;
     * in the C port the value is composed from the masks, so that holds by
     * construction. */
    cpu->state_flags = (uint8_t)(((uint32_t)(cpu->is_32 != 0) << 0) |
                                 ((uint32_t)(cpu->stack_size_32 != 0) << 1) |
                                 ((uint32_t)(cpu->cpl == 3) << 2) |
                                 ((uint32_t)(has_flat_segmentation() != 0) << 3) |
                                 /* S5a: CACHED_MASK_LONG is now COMPOSED, not just
                                  * defined -- it is the fifth bit of the decoded-block key,
                                  * so a block decoded in long mode can never be reused in a
                                  * 32-bit one (or vice versa) even if the physical address
                                  * and the other four bits agree.  False in every 32-bit
                                  * mode, so the four-bit value the key has always been is
                                  * unchanged there (pinned by the M0 transcript). */
                                 ((uint32_t)(cpu->is_64 != 0) << 4));
}

int32_t getiopl(void) { return cpu->flags >> 12 & 3; } /* cpu.rs:4415 */

bool vm86_mode(void) { return (cpu->flags & FLAG_VM) == FLAG_VM; } /* cpu.rs:3159 */

/* ------------------------------------------------------------------ *
 * reset -- cpu.rs:4671-4760, plus the cpu.js:180-199 initial values
 * ------------------------------------------------------------------ */

void reset_cpu(void)
{
    for (int i = 0; i < 8; i++) {
        cpu->segment_is_null[i] = false;
        cpu->segment_limits[i] = 0;
        cpu->segment_offsets[i] = 0;
        cpu->segment_access_bytes[i] = 0x80 | (0 << 5) | 0x10 | 0x02; /* P dpl0 S RW */

        cpu->reg64[i] = 0;
        cpu->sreg[i] = 0;
        cpu->dreg[i] = 0;

        write_xmm128_2(i, 0, 0);
        cpu->fpu_st[i] = F80_ZERO;
    }
    /* S5a: the eight REX-only slots.  reset's own loop above is indexed by the SEGMENT set
     * (8 entries) and used to be the only zeroing of reg64; r8-r15 need their own, or a
     * second machine in the same process inherits the first one's high registers (the
     * selftest's per-arm cpu_state_init is exactly that case). */
    for (int i = 8; i < 16; i++) {
        cpu->reg64[i] = 0;
    }
    cpu->segment_access_bytes[CS] = 0x80 | (0 << 5) | 0x10 | 0x08 | 0x02; /* P dpl0 S E RW */

    for (int i = 0; i < 4; i++) {
        cpu->reg_pdpte[i] = 0;
    }

    cpu->fpu_stack_empty = 0xFF;
    cpu->fpu_stack_ptr = 0;
    cpu->fpu_control_word = 0x37F;
    cpu->fpu_status_word = 0;
    cpu->fpu_ip = 0;
    cpu->fpu_ip_selector = 0;
    cpu->fpu_opcode = 0;
    cpu->fpu_dp = 0;
    cpu->fpu_dp_selector = 0;

    cpu->mxcsr = 0x1F80;

    full_clear_tlb(); /* paging.c */

    cpu->protected_mode = false;

    /* http://www.sandpile.org/x86/initial.htm */
    cpu->idtr_size = 0;
    cpu->idtr_offset = 0;

    cpu->gdtr_size = 0;
    cpu->gdtr_offset = 0;

    cpu->cr[0] = (int32_t)((1u << 30) | (1u << 29) | (1u << 4));
    cpu->cr[2] = 0;
    cpu->cr[3] = 0;
    cpu->cr[4] = 0;
    cpu->dreg[6] = RESET_DREG6;
    cpu->dreg[7] = RESET_DREG7;
    cpu->cpl = 0;

    cpu->is_32 = false;
    cpu->is_64 = false;
    cpu->stack_size_32 = false;
    cpu->prefixes = 0;

    cpu->last_virt_eip = VIRT_EIP_NONE;
    /* S3: the OTHER half of the fetch-translation cache.  Upstream's reset leaves it alone
     * (it is dead until last_virt_eip matches an eip, and VIRT_EIP_NONE never does), so this
     * is unobservable -- but it is what makes the reset state canonically clean, which is
     * the premise the mode boundary below asserts.  MEASURED: without it the invariant at
     * reset_cpu names a stale eip_phys from a PREVIOUS machine in the same process (the
     * selftest's per-arm reset is exactly that case). */
    cpu->eip_phys = 0;

    cpu->instruction_counter = 0;
    cpu->previous_ip = 0;
    cpu->in_hlt = false;

    cpu->sysenter_cs = 0;
    cpu->sysenter_esp = 0;
    cpu->sysenter_eip = 0;

    cpu->flags = FLAGS_DEFAULT;
    cpu->flags_changed = 0;
    cpu->last_result = 0;
    cpu->last_op1 = 0;
    cpu->last_op_size = 0;

    cpu->pat = 0x0007040600070406ull;
    cpu->efer = 0;

    /* set_tsc(0, 0) (cpu.rs:4744) sets an offset against the host clock; the C port
     * has no host-clock TSC yet (M1c's run loop owns microtick/rdtsc), so the state
     * starts at 0 rather than at an offset -- see the unit report. */
    cpu->current_tsc = 0;

    cpu->instruction_pointer = 0xFFFF0;
    /* switch_cs_real_mode(0xF000) (cpu.rs:4755) -- inlined: M1c's exceptions.c owns
     * switch_cs_real_mode/switch_seg, and at reset protected_mode is false, so only
     * the real-mode arm (cpu.rs:473-481) is reachable. */
    cpu->sreg[CS] = 0xF000;
    cpu->segment_is_null[CS] = false;
    cpu->segment_offsets[CS] = 0xF000 << 4;
    cpu->is_32 = false;

    /* switch_seg(SS, 0x30) (cpu.rs:4752) -- real-mode arm of cpu.rs:2685-2697 */
    cpu->sreg[SS] = 0x30;
    cpu->segment_is_null[SS] = false;
    cpu->segment_offsets[SS] = 0x30 << 4;
    cpu->stack_size_32 = false;
    update_state_flags();
    write_reg32(ESP, 0x100);

    update_state_flags();

    /* jit::jit_clear_cache_js() -- dropped (interpreter-only port). */

    cpu->pagefault = false;

    /* S3 mode boundary #1 (reset): the reset state must satisfy the high-half invariant, or
     * RESET_EIP / the 0xF000<<4 base is itself a widened leak. */
    cpu_assert_address_widening("reset_cpu");
}

/* Allocate the singleton and take it to the reset state.  The wasm reference gets
 * its state block from a zeroed linear memory (v86/src/rust/cpu/cpu.rs:1-40's
 * `memory` is zero-initialised wasm linear memory, and the JS views index into it),
 * so the C port starts from zeroed storage, then reset_cpu() overwrites every
 * member that reset_cpu defines. */
void cpu_state_init(void)
{
    static cpu_state_t state;
    cpu = &state;
    reset_cpu();
}
