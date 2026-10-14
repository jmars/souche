/*
 * cpu_state.c -- the CPU state singleton, its reset, the cached-state byte and the
 * register accessors.  Port of v86/src/rust/cpu/cpu.rs:4128-4196 (accessors),
 * cpu.rs:3346-3363 (update_state_flags/has_flat_segmentation), cpu.rs:4671-4760
 * (reset_cpu) and global_pointers.rs (the layout).
 */

#include "cpu_state.h"
#include "paging.h" /* reset_cpu must flush the TLB (full_clear_tlb) */

/* global_pointers.rs used absolute offsets into a shared wasm memory; the C port has
 * one CPU, so the plan's "one global `cpu` pointer" (ss1) is the whole story. */
cpu_state_t *cpu;

/* ------------------------------------------------------------------ *
 * register accessors -- cpu.rs:4128-4196, ported verbatim
 * ------------------------------------------------------------------ */

/* reg8 is the byte view of reg32 (global_pointers.rs:8): index -> byte offset
 * mapping "index << 2 & 12 | index >> 2 & 1" (cpu.rs:4128). */
static int32_t get_reg8_index(int32_t index) { return (index << 2 & 12) | (index >> 2 & 1); }

int32_t read_reg8(int32_t index)
{
    return ((const uint8_t *)cpu->reg32)[get_reg8_index(index)];
}

void write_reg8(int32_t index, int32_t value)
{
    ((uint8_t *)cpu->reg32)[get_reg8_index(index)] = (uint8_t)value;
}

/* get_reg16_index(index) == index << 1 (cpu.rs:4142) -- and the pointer arithmetic in
 * upstream is in u16 UNITS, so the element index is 2*index (reg32[1]'s low half is
 * u16[2], i.e. CX).  Getting this wrong silently aliases DX to CX. */
int32_t read_reg16(int32_t index) { return ((const uint16_t *)cpu->reg32)[index << 1]; }

void write_reg16(int32_t index, int32_t value)
{
    ((uint16_t *)cpu->reg32)[index << 1] = (uint16_t)value;
}

int32_t read_reg32(int32_t index) { return cpu->reg32[index]; }

void write_reg32(int32_t index, int32_t value) { cpu->reg32[index] = value; }

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
                                 ((uint32_t)(has_flat_segmentation() != 0) << 3));
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

        cpu->reg32[i] = 0;
        cpu->sreg[i] = 0;
        cpu->dreg[i] = 0;

        write_xmm128_2(i, 0, 0);
        cpu->fpu_st[i] = F80_ZERO;
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
    cpu->dreg[6] = (int32_t)0xFFFF0FF0u;
    cpu->dreg[7] = 0x400;
    cpu->cpl = 0;

    cpu->is_32 = false;
    cpu->stack_size_32 = false;
    cpu->prefixes = 0;

    cpu->last_virt_eip = -1;

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

    /* switch_seg(SS, 0x30) (cpu.rs:4757) -- real-mode arm of cpu.rs:2685-2697 */
    cpu->sreg[SS] = 0x30;
    cpu->segment_is_null[SS] = false;
    cpu->segment_offsets[SS] = 0x30 << 4;
    cpu->stack_size_32 = false;
    update_state_flags();
    write_reg32(ESP, 0x100);

    update_state_flags();

    /* jit::jit_clear_cache_js() -- dropped (interpreter-only port). */

    cpu->pagefault = false;
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
