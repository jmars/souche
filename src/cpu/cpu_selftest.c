/*
 * cpu_selftest.c -- the M1a unit tests for the foundation (plan ss2/ss3/ss4 gates).
 *
 * Not in tests/: that directory belongs to the oracle unit (P0).  Built by
 * `make selftest`.
 *
 *   1. reset_cpu produces the exact documented init values (cpu.rs:4671-4760,
 *      cpu.js:180-199);
 *   2. the lazy-flag getters against a (op, a, b, op_size, cf_in) -> flags table;
 *   3. hand-built 32-bit and 4 MiB PSE page tables driven through
 *      translate_address/safe_read/safe_write, asserting the values, the fault flag,
 *      the interrupt delivery and the trigger_pagefault side effects (cr2, eip).
 *
 * The expected flag values were derived from the Intel SDM flag rules by an
 * independent model (not from v86's code) and hand-checked case by case; see the unit
 * report for the cross-check against the reference emulator.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arith.h"
#include "cpu_backend.h" /* S0: the engine seam's resolver, its instances and its dispatch */
#include "cpu.h"   /* S3: the stop channel the high-half invariant reports through */
#include "cpu_state.h"
#include "exceptions.h" /* M1c: the segment-descriptor types the stand-ins use */
#include "flags.h"
#include "fpu.h" /* M1b-3: the x87 interface under test */
#include "memory.h"
#include "misc_instr.h"
#include "instructions.h"
#include "instructions_0f.h"
#include "modrm.h"
#include "paging.h"
#include "string.h"
#include "uop_cache.h" /* S0: uop_run -- the symbol `uopjit`'s run_batch must BE */
#include "jit_mem.h"   /* S0: jit_enabled -- the compiled path's half of an engine's identity */

static int checks = 0;
static int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        checks++;                                                             \
        long long _a = (long long)(a), _b = (long long)(b);                    \
        if (_a != _b) {                                                        \
            failures++;                                                        \
            fprintf(stderr,                                                    \
                    "FAIL %s:%d: %s == %s (got 0x%llx, want 0x%llx)\n",        \
                    __FILE__, __LINE__, #a, #b, (unsigned long long)_a,         \
                    (unsigned long long)_b);                                   \
        }                                                                      \
    } while (0)

/* ------------------------------------------------------------------ *
 * The interrupt-delivery stub.
 *
 * exceptions.c (M1c) owns call_interrupt_vector.  Until it lands, this binary is the
 * only definition of trigger_pagefault's delivery, and it records what it received.
 * ------------------------------------------------------------------ */
static int32_t delivered_interrupt = -1;
static int32_t delivered_error_code = -1;
static bool delivered_has_error_code = false;
static bool delivered_is_int = false;
static int32_t delivery_count = 0;

/* M1a-3 widened this to carry upstream's Option<i32> (paging.h): `has_error_code` false
 * is upstream's `None`, which is what INT3 / INT n / INTO pass and what decides whether an
 * error code is pushed.  M1c's exceptions.c defines this signature. */
void call_interrupt_vector(int32_t interrupt, bool is_int, bool has_error_code, int32_t error_code)
{
    delivered_interrupt = interrupt;
    delivered_is_int = is_int;
    delivered_has_error_code = has_error_code;
    delivered_error_code = error_code;
    delivery_count++;
}

static void reset_delivery(void)
{
    delivered_interrupt = -1;
    delivered_error_code = -1;
    delivered_has_error_code = false;
    delivered_is_int = false;
    delivery_count = 0;
}

/* The PAE paging cases (defined below, with the other paging tests) read these
 * recording globals, so they are declared here -- before that section -- while the
 * trigger_* stand-ins that WRITE them live with the other M1b/M1c stand-ins further
 * down. */
static int32_t exc_vector = -1;
static int32_t exc_code = -1;
static int32_t exc_count = 0;

/* defined below (with the other M1b/M1c stand-ins); the PAE cases use it before
 * that point */
static void reset_exceptions(void);

/* ------------------------------------------------------------------ *
 * gate 1 -- reset values
 * ------------------------------------------------------------------ */

static void test_reset(void)
{
    cpu_state_init();

    /* cpu.js:180-199 (the JS constructor) */
    CHECK_EQ(cpu->fpu_control_word, 0x37F);
    CHECK_EQ(cpu->fpu_status_word, 0);
    CHECK_EQ(cpu->fpu_stack_empty, 0xFF);
    CHECK_EQ(cpu->fpu_stack_ptr, 0);
    CHECK_EQ(cpu->fpu_opcode, 0);
    CHECK_EQ(cpu->fpu_ip, 0);
    CHECK_EQ(cpu->fpu_ip_selector, 0);
    CHECK_EQ(cpu->fpu_dp, 0);
    CHECK_EQ(cpu->fpu_dp_selector, 0);
    CHECK_EQ(cpu->mxcsr, 0x1F80);
    for (int i = 0; i < 8; i++) {
        CHECK_EQ(cpu->reg_xmm[i].u64[0], 0);
        CHECK_EQ(cpu->reg_xmm[i].u64[1], 0);
    }

    /* reset_cpu (cpu.rs:4671-4760) */
    CHECK_EQ(cpu->flags, FLAGS_DEFAULT);
    CHECK_EQ(FLAGS_DEFAULT, 2);
    CHECK_EQ(cpu->flags_changed, 0);
    CHECK_EQ(cpu->last_result, 0);
    CHECK_EQ(cpu->last_op1, 0);
    CHECK_EQ(cpu->last_op_size, 0);
    CHECK_EQ(cpu->cr[0], RESET_CR0);
    CHECK_EQ((uint32_t)RESET_CR0, 0x60000010u); /* CD|NW|ET, cpu.rs:4751 */
    CHECK(cpu->cr[1] == 0 && cpu->cr[2] == 0 && cpu->cr[3] == 0 && cpu->cr[4] == 0);
    CHECK_EQ(cpu->dreg[6], RESET_DREG6);
    CHECK_EQ(cpu->dreg[7], RESET_DREG7);
    CHECK_EQ(cpu->cpl, 0);
    CHECK_EQ(cpu->instruction_pointer, RESET_EIP);
    CHECK_EQ(cpu->previous_ip, 0);
    CHECK_EQ(cpu->instruction_counter, 0);
    CHECK(!cpu->in_hlt);
    CHECK_EQ(cpu->prefixes, 0);
    /* S3: THE SENTINEL MOVED BY DESIGN.  It was `-1` (0xFFFFFFFF in an i32 field); with a
     * 64-bit field that literal would sign-extend to 0xFFFFFFFFFFFFFFFF, which is itself a
     * non-zero high half -- i.e. the very leak the S3 invariant exists to catch, written by
     * the reset path.  VIRT_EIP_NONE keeps the meaning ("no page cached": its low 12 bits
     * are set, which no page value can be) and the high half clean.  Every reader compares
     * at page granularity or through (uint32_t), so the verdict at each of them is
     * unchanged -- MEASURED by the M0 transcript unmoved. */
    CHECK_EQ(cpu->last_virt_eip, VIRT_EIP_NONE);
    CHECK_EQ(cpu->pat, RESET_PAT);
    CHECK_EQ(cpu->efer, 0);
    CHECK_EQ(cpu->idtr_size, 0);
    CHECK_EQ(cpu->idtr_offset, 0);
    CHECK_EQ(cpu->gdtr_size, 0);
    CHECK_EQ(cpu->gdtr_offset, 0);
    CHECK(!cpu->protected_mode);
    CHECK(!cpu->is_32);
    CHECK(!cpu->stack_size_32);
    CHECK(!cpu->pagefault);
    CHECK_EQ(cpu->state_flags, 0); /* not is_32, not ssize_32, not cpl3, no flat segs */
    CHECK(!cached_state_is_32() && !cached_state_ssize_32() && !cached_state_cpl3());
    CHECK(!cached_state_has_flat_segmentation());
    CHECK(!has_flat_segmentation());

    for (int i = 0; i < 8; i++) {
        /* reset_cpu leaves reg64[ESP]=0x100 (cpu.rs:4758), dreg[6]/dreg[7] and the
         * CS/SS slots at their switch values -- those are checked separately */
        if (i != ESP) {
            CHECK_EQ(cpu->reg64[i], 0);
        }
        if (i != 6 && i != 7) {
            CHECK_EQ(cpu->dreg[i], 0);
        }
        CHECK(!cpu->segment_is_null[i]);
        CHECK_EQ(cpu->segment_limits[i], 0);
        CHECK_EQ(cpu->fpu_st[i].mantissa, 0);
        CHECK_EQ(cpu->fpu_st[i].sign_exponent, 0);
        CHECK_EQ(cpu->segment_access_bytes[i],
                 (i == CS) ? RESET_SEG_ACCESS_CS : RESET_SEG_ACCESS_DATA);
    }
    for (int i = 0; i < 6; i++) {
        if (i != SS) {
            CHECK_EQ(cpu->segment_offsets[i], (i == CS) ? (RESET_CS_SHIFT << 4) : 0);
            CHECK_EQ(cpu->sreg[i], (i == CS) ? 0xF000 : 0);
        }
    }
    CHECK_EQ(cpu->sreg[SS], RESET_SS_SEL);
    CHECK_EQ(cpu->segment_offsets[SS], RESET_SS_SEL << 4);
    CHECK_EQ(read_reg32(ESP), RESET_ESP);
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(cpu->reg_pdpte[i], 0);
    }

    /* update_state_flags: is_32/ssize_32/cpl3/flat-segs (cpu.rs:3346-3352) */
    cpu->is_32 = true;
    cpu->stack_size_32 = true;
    cpu->cpl = 3;
    update_state_flags();
    CHECK_EQ(cpu->state_flags, CACHED_MASK_IS_32 | CACHED_MASK_SS32 | CACHED_MASK_CPL3);
    CHECK(cached_state_is_32() && cached_state_ssize_32() && cached_state_cpl3());
    cpu->cpl = 0;
    cpu->segment_offsets[SS] = 0;
    cpu->segment_offsets[DS] = 0;
    cpu->segment_offsets[CS] = 0;
    update_state_flags();
    CHECK(cached_state_has_flat_segmentation());
    cpu->segment_is_null[DS] = true;
    update_state_flags();
    CHECK(!cached_state_has_flat_segmentation()); /* has_flat_segmentation: !is_null(DS) */
    cpu->segment_is_null[DS] = false;
    cpu_state_init();

    /* the register accessors, including the AH/CH/DH/BH byte mapping (cpu.rs:4128) */
    write_reg32(EAX, 0x11223344);
    CHECK_EQ(read_reg32(EAX), 0x11223344);
    CHECK_EQ(read_reg16(AX), 0x3344);
    CHECK_EQ(read_reg8(AL), 0x44); /* 0x11223344: AL = 0x44, AH = 0x33 */
    CHECK_EQ(read_reg8(AH), 0x33);
    write_reg8(BH, 0xAB);
    CHECK_EQ(read_reg32(EBX), 0x0000AB00);
    write_reg8(CL, 0x99);
    CHECK_EQ(read_reg8(CL), 0x99);
    CHECK_EQ(read_reg32(ECX), 0x99);
    /* the 16-bit accessor indexes the u16 view (get_reg16_index: index << 1), so DX
     * must land in EDX and not in ECX */
    write_reg16(DX, 0xBEEF);
    CHECK_EQ(read_reg32(EDX), 0x0000BEEF);
    CHECK_EQ(read_reg32(ECX), 0x99);
    write_reg16(BX, 0x1234);
    CHECK_EQ(read_reg32(EBX), 0x00001234);
    write_reg16(CX, 0x5678);
    CHECK_EQ(read_reg32(ECX), 0x00005678);
    write_reg16(SP, 0xBEEF);
    CHECK_EQ(read_reg16(SP), 0xBEEF);
    CHECK_EQ(read_reg32(ESP), 0x0000BEEF);

    /* mmx/xmm accessors */
    write_mmx_reg64(3, 0x0123456789ABCDEFull);
    CHECK_EQ(read_mmx64s(3), 0x0123456789ABCDEFull);
    CHECK_EQ(read_mmx32s(3), (int32_t)0x89ABCDEF);
    CHECK_EQ(read_mmx64s(3 + 4), 0); /* separate slots: fpu_st[3] != fpu_st[7] */
    write_xmm128(2, 0x11223344, 0x55667788, 0x99AABBCC, 0xDDEEFF00);
    CHECK_EQ(read_xmm32(2), (int32_t)0x11223344);
    CHECK_EQ(read_xmm64s(2), 0x5566778811223344ull);
    write_xmm128_2(2, 0xDEADBEEFCAFEBABEull, 1);
    CHECK_EQ(read_xmm128s(2).u64[0], 0xDEADBEEFCAFEBABEull);

    printf("PASS reset/init values, state_flags, register accessors\n");
}

/* ------------------------------------------------------------------ *
 * gate 2 -- the lazy flags
 * ------------------------------------------------------------------ */

enum { OP_ADD, OP_ADC, OP_SUB, OP_CMP, OP_SBB, OP_INC, OP_DEC, OP_NEG };

typedef struct {
    int op;
    int32_t a;
    int32_t b;
    int32_t op_size;
    int cf_in;
    int exp_cf, exp_pf, exp_af, exp_zf, exp_sf, exp_of;
} flag_case;

/* Derived from the SDM flag rules (PF = even parity of the low byte, AF = carry out of
 * bit 3, OF = signed overflow), op_size one of 8/16/32, cf_in the carry into adc/sbb
 * or the carry that inc/dec must preserve. */
static const flag_case flag_cases[] = {
    /*   op     a            b        size cf  CF PF AF ZF SF OF */
    {OP_ADD, 0x0F, 0x01, 8, 0, 0, 0, 1, 0, 0, 0},
    {OP_ADD, 0xFF, 0x01, 8, 0, 1, 1, 1, 1, 0, 0},
    {OP_ADD, 0x7FFFFFFF, 0x00000001, 32, 0, 0, 1, 1, 0, 1, 1},
    {OP_ADD, 0xFFFFFFFF, 0x00000001, 32, 0, 1, 1, 1, 1, 0, 0},
    {OP_ADD, 0x7FFF, 0x0001, 16, 0, 0, 1, 1, 0, 1, 1},
    {OP_ADD, 0xFFFF, 0x0001, 16, 0, 1, 1, 1, 1, 0, 0},
    {OP_SUB, 0x00000000, 0x00000001, 32, 0, 1, 1, 1, 0, 1, 0},
    {OP_SUB, 0x00000005, 0x00000005, 32, 0, 0, 1, 0, 1, 0, 0},
    {OP_SUB, 0x80, 0x01, 8, 0, 0, 0, 1, 0, 0, 1},
    {OP_SUB, 0x8000, 0x0001, 16, 0, 0, 1, 1, 0, 0, 1},
    {OP_CMP, 0x12345678, 0x12345678, 32, 0, 0, 1, 0, 1, 0, 0},
    {OP_CMP, 0x00000001, 0x00000002, 32, 0, 1, 1, 1, 0, 1, 0},
    {OP_ADC, 0x7F, 0x00, 8, 1, 0, 0, 1, 0, 1, 1},
    {OP_ADC, 0xFFFFFFFF, 0x00000000, 32, 1, 1, 1, 1, 1, 0, 0},
    {OP_SBB, 0x00000000, 0x00000000, 32, 1, 1, 1, 1, 0, 1, 0},
    {OP_SBB, 0x00000100, 0x00000001, 32, 0, 0, 1, 1, 0, 0, 0},
    {OP_INC, 0x7FFFFFFF, 0, 32, 1, 1, 1, 1, 0, 1, 1},
    {OP_INC, 0x7F, 0, 8, 0, 0, 0, 1, 0, 1, 1},
    {OP_INC, 0xFFFFFFFF, 0, 32, 0, 0, 1, 1, 1, 0, 0},
    {OP_DEC, 0x80, 0, 8, 1, 1, 0, 1, 0, 0, 1},
    {OP_DEC, 0x00000001, 0, 32, 1, 1, 1, 0, 1, 0, 0},
    {OP_NEG, 0x00000001, 0, 32, 0, 1, 1, 1, 0, 1, 0},
    {OP_NEG, 0x80000000, 0, 32, 0, 1, 1, 0, 0, 1, 1},
    {OP_NEG, 0x00, 0, 8, 0, 0, 1, 0, 1, 0, 0},
};

static int32_t op_size_of(int size)
{
    return size == 8 ? OPSIZE_8 : size == 16 ? OPSIZE_16 : OPSIZE_32;
}

static void test_flags(void)
{
    size_t n = sizeof(flag_cases) / sizeof(flag_cases[0]);
    for (size_t i = 0; i < n; i++) {
        const flag_case *c = &flag_cases[i];
        int32_t os = op_size_of(c->op_size);

        cpu_state_init();
        /* set the incoming carry through the materialized-`flags` path: the
         * flags_changed bits are clear, so getcf() reads bit 0 of `flags` */
        cpu->flags = FLAGS_DEFAULT | (c->cf_in ? FLAG_CARRY : 0);

        switch (c->op) {
        case OP_ADD: flags_add(c->a, c->b, os); break;
        case OP_ADC: flags_adc(c->a, c->b, os); break;
        case OP_SUB: flags_sub(c->a, c->b, os); break;
        case OP_CMP: flags_sub(c->a, c->b, os); break;
        case OP_SBB: flags_sbb(c->a, c->b, os); break;
        case OP_INC: flags_inc(c->a, os); break;
        case OP_DEC: flags_dec(c->a, os); break;
        case OP_NEG: flags_neg(c->a, os); break;
        default: CHECK(0); continue;
        }

        int got_cf = getcf(), got_pf = getpf(), got_af = getaf();
        int got_zf = getzf(), got_sf = getsf(), got_of = getof();

        /* the machine-readable per-case line: the reference cross-check (unit report)
         * parses exactly this format and compares it with the reference emulator's
         * stored EFLAGS for the same (op,a,b,size,cf_in) */
        printf("CASE %2zu op=%d size=%2d a=%08x b=%08x cf_in=%d -> flags=0x%03x table=0x%03x\n",
               i, c->op, c->op_size, (unsigned)c->a, (unsigned)c->b, c->cf_in,
               (got_cf ? 1 : 0) | (got_pf ? 4 : 0) | (got_af ? 16 : 0) | (got_zf ? 64 : 0) |
                   (got_sf ? 128 : 0) | (got_of ? 2048 : 0),
               (c->exp_cf ? 1 : 0) | (c->exp_pf ? 4 : 0) | (c->exp_af ? 16 : 0) |
                   (c->exp_zf ? 64 : 0) | (c->exp_sf ? 128 : 0) | (c->exp_of ? 2048 : 0));

        checks += 6;
        if (got_cf != c->exp_cf || got_pf != c->exp_pf || got_af != c->exp_af ||
            got_zf != c->exp_zf || got_sf != c->exp_sf || got_of != c->exp_of) {
            failures++;
            fprintf(stderr,
                    "FAIL flags case %zu (op=%d a=0x%x b=0x%x size=%d cf_in=%d): "
                    "got CF=%d PF=%d AF=%d ZF=%d SF=%d OF=%d, want %d%d%d%d%d%d\n",
                    i, c->op, (unsigned)c->a, (unsigned)c->b, c->op_size, c->cf_in, got_cf, got_pf,
                    got_af, got_zf, got_sf, got_of, c->exp_cf, c->exp_pf, c->exp_af, c->exp_zf,
                    c->exp_sf, c->exp_of);
        }
    }

    /* get_eflags materializes through the getters (cpu.rs:1911-1918): 0x0F + 0x01 has
     * AF=1 and nothing else, so the word is FLAGS_DEFAULT | FLAG_ADJUST */
    cpu_state_init();
    flags_add(0x0F, 0x01, OPSIZE_8);
    CHECK_EQ(get_eflags(), FLAGS_DEFAULT | FLAG_ADJUST);
    CHECK_EQ(get_eflags(), 0x00000012);

    /* ... and an untouched flag word round-trips: get_eflags of a fresh reset state */
    cpu_state_init();
    CHECK_EQ(get_eflags(), FLAGS_DEFAULT);

    /* update_eflags: materializes, clears flags_changed, honours the don't-update set,
     * and always forces FLAGS_DEFAULT (cpu.rs:4478-4520) */
    cpu_state_init();
    update_eflags(0x00000202); /* IF set, bit 9 only */
    CHECK_EQ(cpu->flags, 0x00000202 | FLAGS_DEFAULT);
    CHECK_EQ(cpu->flags_changed, 0);
    CHECK_EQ(get_eflags(), 0x00000202 | FLAGS_DEFAULT);
    cpu_state_init();
    update_eflags(0x00013257); /* TF | IOPL=3 | RF | VIP | IF */
    CHECK_EQ(cpu->flags & FLAG_TRAP, 0); /* TF is dropped after the merge */
    /* dont_update = RF|VM|VIP|VIF means "take the OLD bit", and the old value here is
     * the reset word (2): so these keep 0/0/0, they are not copied from new_flags */
    CHECK_EQ(cpu->flags & FLAG_RF, 0);
    CHECK_EQ(cpu->flags & FLAG_VIP, 0);
    CHECK_EQ(cpu->flags & FLAG_IOPL, 0x3000); /* cpl == 0: IOPL is updatable */
    CHECK_EQ(cpu->flags & FLAG_INTERRUPT, FLAG_INTERRUPT);

    /* ... and the converse: an old dont_update bit survives a new value */
    cpu_state_init();
    cpu->flags = FLAGS_DEFAULT | FLAG_RF;
    update_eflags(0x00000202);
    CHECK_EQ(cpu->flags & FLAG_RF, FLAG_RF);
    CHECK_EQ(cpu->flags & FLAG_INTERRUPT, FLAG_INTERRUPT);

    /* cpl > 0 cannot change IOPL (cpu.rs:4494-4503) */
    cpu_state_init();
    cpu->cpl = 2;
    cpu->protected_mode = true;
    cpu->flags = FLAGS_DEFAULT | 0x3000; /* IOPL = 3 */
    update_eflags(0x00000000);
    CHECK_EQ(cpu->flags & FLAG_IOPL, 0x3000);

    /* cpl > iopl additionally freezes IF */
    cpu_state_init();
    cpu->cpl = 2;
    cpu->protected_mode = true;
    cpu->flags = FLAGS_DEFAULT | 0x1000; /* IOPL = 1 < cpl */
    update_eflags(0x00000200);           /* try to set IF */
    CHECK_EQ(cpu->flags & FLAG_INTERRUPT, 0);

    printf("PASS lazy flags: %zu (op,a,b,cf_in) cases + get_eflags/update_eflags\n", n);
}

/* ==================================================================== *
 * S1 (results/x86-64-plan.md ss4, slice S1): THE REGISTER FILE AT 64 BITS
 *
 * The slice widens `int32_t reg32[8]` to `uint64_t reg64[8]` in place.  Two things can go
 * wrong silently and neither is visible to the 32-bit battery, which is why this test
 * exists:
 *
 *   1. THE BYTE/WORD INDEX FORMULA moves with the element size.  The old formula
 *      (index << 2 & 12) | (index >> 2 & 1) indexes a flat 4-byte layout; applied to
 *      8-byte slots it puts AH on RAX's 5th byte and CL on RAX's 5th.. the shape of the
 *      bug the JIT's own register mapping hit once (jit_codegen.c:198-201) -- a corrupted
 *      ESP and a boot that printed one message forever.  So the mapping is asserted
 *      register by register, and a high-byte write is required to leave EVERY other slot
 *      untouched;
 *   2. a 32-bit write must leave a DEFINED high half.  write_reg32 zeroes it (x86-64's
 *      own rule for a 32-bit destination), and the JIT's spill was changed to agree, so
 *      the two engines cannot disagree about what a register holds.
 * ==================================================================== */
static void test_widened_register_file(void)
{
    cpu_state_init();

    /* (1) the byte map: AL/CL/DL/BL are the low bytes, AH/CH/DH/BH the second bytes, of
     * slots 0..3 -- and nothing else. */
    write_reg32(EAX, (int32_t)0x11223344u);
    write_reg32(ECX, (int32_t)0x55667788u);
    write_reg32(EDX, (int32_t)0x99AABBCCu);
    write_reg32(EBX, (int32_t)0xDDEEFF11u);
    write_reg32(ESP, (int32_t)0xDEADBEEFu);
    CHECK_EQ(read_reg8(AL), 0x44);
    CHECK_EQ(read_reg8(AH), 0x33);
    CHECK_EQ(read_reg8(CL), 0x88);
    CHECK_EQ(read_reg8(CH), 0x77);
    CHECK_EQ(read_reg8(DL), 0xCC);
    CHECK_EQ(read_reg8(DH), 0xBB);
    CHECK_EQ(read_reg8(BL), 0x11);
    CHECK_EQ(read_reg8(BH), 0xFF);

    write_reg8(AH, 0xAB);
    CHECK_EQ(read_reg32(EAX), (int32_t)0x1122AB44u);
    CHECK_EQ(read_reg32(ECX), (int32_t)0x55667788u);
    CHECK_EQ(read_reg32(EDX), (int32_t)0x99AABBCCu);
    CHECK_EQ(read_reg32(EBX), (int32_t)0xDDEEFF11u);
    CHECK_EQ(read_reg32(ESP), (int32_t)0xDEADBEEFu); /* NOT 0x1122ABEF: the old-formula bug */
    write_reg8(BH, 0x01);
    CHECK_EQ(read_reg32(EBX), (int32_t)0xDDEE0111u);
    CHECK_EQ(read_reg32(EAX), (int32_t)0x1122AB44u);
    write_reg8(DL, 0x5A);
    CHECK_EQ(read_reg32(EDX), (int32_t)0x99AABB5Au);
    CHECK_EQ(read_reg8(DH), 0xBB);

    /* (2) the 16-bit view is the slot's low half (`index << 2` in u16 units); a CX write
     * must not alias DX (the failure the old comment names). */
    write_reg32(ECX, (int32_t)0x11112222u);
    write_reg32(EDX, (int32_t)0x33334444u);
    write_reg16(CX, 0xBEEF);
    CHECK_EQ(read_reg16(CX), 0xBEEF);
    CHECK_EQ(read_reg32(ECX), (int32_t)0x1111BEEFu);
    CHECK_EQ(read_reg32(EDX), (int32_t)0x33334444u);
    write_reg16(DX, 0x1234);
    CHECK_EQ(read_reg32(EDX), (int32_t)0x33331234u);
    CHECK_EQ(read_reg32(ECX), (int32_t)0x1111BEEFu);

    /* (3) the field is 64 bits, a 32-bit write ZEROES the high half, and the 32-bit reader
     * returns the low half as a signed int32 */
    CHECK_EQ(cpu->reg64[EAX], 0x1122AB44ull);
    write_reg32(EAX, -1);
    CHECK_EQ(cpu->reg64[EAX], 0xFFFFFFFFull); /* zero-extended, not sign-extended */
    CHECK_EQ(read_reg32(EAX), -1);            /* ... and read back as the int32 it was */
    cpu->reg64[ESI] = 0x0123456789ABCDEFull;
    CHECK_EQ(read_reg32(ESI), (int32_t)0x89ABCDEFu);
    CHECK_EQ(read_reg16(SI), 0xCDEF);
    /* a byte write touches ONE byte of the widened field, high half included */
    cpu->reg64[EAX] = 0x0123456789ABCDEFull;
    write_reg8(AL, 0x77);
    CHECK_EQ(cpu->reg64[EAX], 0x0123456789ABCD77ull);
    write_reg8(AH, 0x66);
    CHECK_EQ(cpu->reg64[EAX], 0x0123456789AB6677ull);
}

/* S1's flags half.  Two claims, and the second is the one the slice is worth having for:
 *
 *   - OPSIZE_64 exists and behaves: the mask is all ones (2ull << 63 wraps to 0, so
 *     (2ull << 63) - 1 is all ones -- the same trick upstream plays one width down),
 *     the 64-bit carry/overflow/sign bits are at 63, and the 32-bit behaviour at the
 *     0xFFFFFFFF boundary is unchanged;
 *   - EVERY GETTER IS BLIND TO THE HIGH HALF.  The operands are int64_t now, and a getter
 *     that compared them as int64 without masking would answer CF differently for a
 *     negative 32-bit operand (0xFFFF...FFFB vs 0x0000...FFFB).  So each case is computed,
 *     the two operands are then OR'd with 0xA5A5A5A500000000, and all six getters are
 *     required to answer EXACTLY as before.  That is what makes `make poison-probe`'s
 *     negative result mean something where it can be checked directly.
 */
static void test_widened_flags(void)
{
    /* (1) the masks */
    CHECK_EQ(opsize_to_mask(OPSIZE_8), 0xFF);
    CHECK_EQ(opsize_to_mask(OPSIZE_16), 0xFFFF);
    CHECK_EQ(opsize_to_mask(OPSIZE_32), (int64_t)(uint32_t)0xFFFFFFFFu);
    CHECK_EQ(opsize_to_mask(OPSIZE_64), -1);

    /* (2) hand-checked 64-bit cases.  The rule per case: res = (a op b) & mask64; CF for
     * add is "the sum did not fit", for sub "a < b unsigned"; OF is the carry INTO the
     * sign bit being different from the carry OUT; SF is bit 63 of the result. */
    cpu_state_init();
    CHECK_EQ(flags_add(0xFFFFFFFFll, 1, OPSIZE_64), 0x100000000ll); /* no 32-bit wrap */
    CHECK(!getcf());
    CHECK(!getzf());
    CHECK(!getof());
    cpu_state_init();
    CHECK_EQ(flags_add(-1, 1, OPSIZE_64), 0); /* the 64-bit wrap */
    CHECK(getcf());
    CHECK(getzf());
    CHECK(!getsf());
    CHECK(!getof());
    cpu_state_init();
    CHECK_EQ(flags_add(0x7FFFFFFFFFFFFFFFll, 1, OPSIZE_64), (int64_t)0x8000000000000000ull);
    CHECK(!getcf());
    CHECK(getof()); /* positive + positive = negative */
    CHECK(getsf()); /* bit 63 */
    cpu_state_init();
    CHECK_EQ(flags_sub(0, 1, OPSIZE_64), -1);
    CHECK(getcf());
    CHECK(getsf());
    CHECK(!getof());
    CHECK(getaf());
    /* ... and the same boundary one width down is unchanged: 0xFFFFFFFF + 1 wraps at 32 */
    cpu_state_init();
    CHECK_EQ(flags_add(-1, 1, OPSIZE_32), 0);
    CHECK(getcf());
    CHECK(getzf());

    /* (3) THE POISON ARM.  Two poison pattens, because they test DIFFERENT things: with the
     * SAME high half in both operands a comparison that forgot to mask still ranks the low
     * halves correctly (MEASURED: the first version of this arm passed against a getcf with
     * its mask removed), so the second pattern gives the two operands' high halves
     * DIFFERENT values -- which is the only manipulation that can expose a comparison
     * happening at 64 bits instead of at the operand's width. */
    static const struct {
        int32_t a, b;
        int32_t op_size;
        bool sub;
    } poison_cases[] = {
        { 0, 1, OPSIZE_8, true },        { 0x7F, 1, OPSIZE_8, false },
        { (int32_t)0xFFFFFFFFu, 1, OPSIZE_32, false },
        { (int32_t)0x80000000u, 1, OPSIZE_32, false },
        { 5, (int32_t)0xFFFFFFFBu, OPSIZE_32, true },
        { (int32_t)0xFFFFFFFFu, (int32_t)0xFFFFFFFFu, OPSIZE_32, true },
        { 0x7FFFFFFF, 1, OPSIZE_32, false },
        { 0, 1, OPSIZE_16, true },
    };
    for (size_t i = 0; i < sizeof(poison_cases) / sizeof(poison_cases[0]); i++) {
        int32_t os = poison_cases[i].op_size;
        cpu_state_init();
        if (poison_cases[i].sub) flags_sub(poison_cases[i].a, poison_cases[i].b, os);
        else flags_add(poison_cases[i].a, poison_cases[i].b, os);

        bool cf = getcf(), pf = getpf(), af = getaf(), zf = getzf(), sf = getsf(), of = getof();
        int64_t saved_op1 = cpu->last_op1, saved_result = cpu->last_result;

        static const struct {
            uint64_t op1_high, result_high;
            const char *what;
        } poisons[] = {
            { 0xA5A5A5A500000000ull, 0xA5A5A5A500000000ull, "the same high half in both" },
            { 0xA5A5A5A500000000ull, 0x5A5A5A5A00000000ull, "the high halves differ" },
        };
        for (size_t p = 0; p < sizeof(poisons) / sizeof(poisons[0]); p++) {
            cpu->last_op1 = saved_op1 | (int64_t)poisons[p].op1_high;
            cpu->last_result = saved_result | (int64_t)poisons[p].result_high;

            checks += 6;
            if (getcf() != cf || getpf() != pf || getaf() != af || getzf() != zf ||
                getsf() != sf || getof() != of) {
                failures++;
                fprintf(stderr,
                        "FAIL S1 poison case %zu (os=%d a=0x%x b=0x%x sub=%d, %s): the high half"
                        " moved a flag: CF %d->%d PF %d->%d AF %d->%d ZF %d->%d SF %d->%d"
                        " OF %d->%d\n",
                        i, os, (unsigned)poison_cases[i].a, (unsigned)poison_cases[i].b,
                        poison_cases[i].sub, poisons[p].what, cf, getcf(), pf, getpf(), af,
                        getaf(), zf, getzf(), sf, getsf(), of, getof());
            }
            cpu->last_op1 = saved_op1;
            cpu->last_result = saved_result;
        }
    }

    /* (4) dreg[] is 64 bits, and the debug-register moves are still 32-bit ones */
    cpu_state_init();
    cpu->dreg[0] = 0x0123456789ABCDEFll;
    instr_0F21(EAX, 0); /* mov eax, dr0 */
    CHECK_EQ(read_reg32(EAX), (int32_t)0x89ABCDEFu);
    CHECK_EQ(cpu->dreg[0], 0x0123456789ABCDEFll); /* the read did not disturb it */
    cpu->reg64[EBX] = 0xAAAA5555BBBB4444ull;
    instr_0F23(EBX, 0); /* mov dr0, ebx */
    CHECK_EQ(cpu->dreg[0], 0xBBBB4444ull); /* zero-extended, not sign-extended */

    printf("PASS S1: the widened register file (byte/word/dword slot map + the zero-extended"
           " high half), the 64-bit flag cores and the high-half poison arm\n");
}

/* S2's named-stop recorder (defined with the stub further down, used by the mode/refusal
 * test and by S3's invariant test below). */
static int unimplemented_calls;
static char unimplemented_last[160];
static bool unimplemented_allowed;
static void reset_unimplemented(void);

/* ------------------------------------------------------------------ *
 * S3 -- THE ADDRESS/IP WIDENING'S HIGH-HALF INVARIANT (plan ss6 S3, risk R2)
 *
 * The instrument's CLAIM is that outside long mode none of the widened address fields can
 * hold a value above 2^32, and the shipped code establishes it by construction (every store
 * of a fetch cursor goes through cpu_state.h's set_instruction_pointer, every projection is a
 * named cast).  An invariant that holds by construction is worth exactly as much as its
 * ability to FAIL, so this test does what S1's flag arm had to learn the hard way: it makes
 * the invariant fail, field by field, and requires it to name the field it caught.  Each arm
 * seeds ONE field's high half into an otherwise reset machine and demands its own name back;
 * the clean machine must answer NULL; and the long-mode arm must answer NULL with the field
 * still poisoned (in IA-32e the high half is where the address lives).
 * ------------------------------------------------------------------ */
static void test_address_widening_invariant(void)
{
    const uint64_t high = 0xA5A5A5A5ull << 32;

    /* (1) the reset state is clean -- or RESET_EIP / the 0xF000<<4 base is itself a leak */
    cpu_state_init();
    CHECK(cpu_address_widening_leak() == NULL);
    CHECK(!cpu->efer);

    /* (2) ONE FIELD AT A TIME.  A detector that names the wrong field is worse than one
     * that stays silent: it sends the reader to the wrong arithmetic. */
    struct { const char *name; int which; } cases[] = {
        { "instruction_pointer", 0 }, { "previous_ip", 1 }, { "eip_phys", 2 },
        { "last_virt_eip", 3 }, { "idtr_offset", 4 }, { "gdtr_offset", 5 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cpu_state_init();
        switch (cases[i].which) {
        case 0: cpu->instruction_pointer |= high; break;
        case 1: cpu->previous_ip |= high; break;
        case 2: cpu->eip_phys |= high; break;
        case 3: cpu->last_virt_eip |= high; break;
        case 4: cpu->idtr_offset |= high; break;
        default: cpu->gdtr_offset |= high; break;
        }
        const char *leak = cpu_address_widening_leak();
        if (leak == NULL || strcmp(leak, cases[i].name) != 0) {
            fprintf(stderr, "FAIL S3 invariant arm %s: leak detector said %s (want that name)\n",
                    cases[i].name, leak ? leak : "(clean)");
        }
        CHECK(leak != NULL);
        CHECK(strcmp(leak ? leak : "", cases[i].name) == 0);
    }

    /* (3) all eight segment slots, each with its own name: the slot an off-by-one stride
     * lands on is exactly what a widening gets wrong (the JIT's 4-byte stride caught this
     * unit, jit_codegen.c) */
    static const char *const seg_names[8] = {
        "segment_offsets[ES]", "segment_offsets[CS]", "segment_offsets[SS]",
        "segment_offsets[DS]", "segment_offsets[FS]", "segment_offsets[GS]",
        "segment_offsets[TR]", "segment_offsets[LDTR]",
    };
    for (int i = 0; i < 8; i++) {
        cpu_state_init();
        cpu->segment_offsets[i] |= high;
        const char *leak = cpu_address_widening_leak();
        if (leak == NULL || strcmp(leak, seg_names[i]) != 0) {
            fprintf(stderr, "FAIL S3 invariant segment slot %d: leak detector said %s (want %s)\n",
                    i, leak ? leak : "(clean)", seg_names[i]);
        }
        CHECK(leak != NULL && strcmp(leak, seg_names[i]) == 0);
    }

    /* (4) THE LONG-MODE ARM: with EFER.LMA set the high half IS the address, so the same
     * poisoned state must be accepted.  Before S5 nothing can SET LMA, so this arm puts the
     * state there directly -- stated rather than implied, exactly as S2's cpu_mode_of arm
     * does.  Without this arm, the invariant's first act in long mode (S5) would be a stop. */
    cpu_state_init();
    cpu->instruction_pointer |= high;
    cpu->efer |= EFER_LMA;
    CHECK(cpu_address_widening_leak() == NULL);

    /* (5) the run-time form is a no-op when DISARMED: `--poison` seeds these very fields on
     * purpose (src/main.c), and the probe's claim is the opposite direction -- that a present
     * high half moves nothing.  The two instruments stay each other's control. */
    cpu_state_init();
    cpu->instruction_pointer |= high;
    reset_unimplemented();
    unimplemented_allowed = true;
    cpu_address_widening_check_enabled = false;
    CHECK(cpu_address_widening_leak() != NULL); /* the detector still sees it ... */
    cpu_assert_address_widening("selftest");
    CHECK_EQ(unimplemented_calls, 0);            /* ... but the run-time form was disarmed */
    cpu_address_widening_check_enabled = true;

    /* (6) and, ARMED, it reports through the machine's stop channel (cpu.h) -- with the
     * field's name in the message, which is what the user reads.  The stop is what makes a
     * leak a named stop instead of a wrong address in one guest. */
    cpu_state_init();
    cpu->gdtr_offset |= high;
    reset_unimplemented();
    unimplemented_allowed = true;
    cpu_assert_address_widening("selftest");
    unimplemented_allowed = false;
    CHECK_EQ(unimplemented_calls, 1);
    CHECK(strstr(unimplemented_last, "gdtr_offset") != NULL);
    reset_unimplemented();

    /* (7) THE THREE MODE-BOUNDARY SEATS ARE CALLED BY THE GUEST-REACHABLE PATHS, not merely
     * written down (round 2's S1: the comment claimed them and only reset/batch had them, so
     * they are exercised here through the same bodies a guest reaches).  Each arm poisons
     * gdtr_offset, drives one transition, and requires the stop to name the field -- which can
     * only happen if that function calls the invariant AFTER committing. */
    /* (7a) WRMSR(EFER) -- the write that asks for (or leaves) IA-32e */
    cpu_state_init();
    cpu->cpl = 0;
    cpu->gdtr_offset |= high;
    write_reg32(ECX, (int32_t)MSR_EFER);
    write_reg32(EAX, (int32_t)EFER_SCE);
    write_reg32(EDX, 0);
    reset_unimplemented();
    unimplemented_allowed = true;
    instr_0F30();
    unimplemented_allowed = false;
    CHECK_EQ(unimplemented_calls, 1);
    CHECK(strstr(unimplemented_last, "gdtr_offset") != NULL);
    CHECK(strstr(unimplemented_last, "WRMSR(EFER)") != NULL); /* and WHERE it was seen */
    CHECK_EQ(cpu->efer, (uint64_t)EFER_SCE); /* the commit happened first: the check is on
                                              * the state the write produced */
    reset_unimplemented();

    /* (7b) CR0's write */
    cpu_state_init();
    cpu->gdtr_offset |= high;
    reset_unimplemented();
    unimplemented_allowed = true;
    set_cr0(CR0_PE | CR0_WP);
    unimplemented_allowed = false;
    CHECK_EQ(unimplemented_calls, 1);
    CHECK(strstr(unimplemented_last, "gdtr_offset") != NULL);
    CHECK(strstr(unimplemented_last, "set_cr0") != NULL);
    CHECK_EQ(cpu->cr[0], CR0_PE | CR0_WP | CR0_ET);
    reset_unimplemented();

    /* (7c) CR4's write */
    cpu_state_init();
    cpu->cpl = 0;
    cpu->cr[0] = CR0_PE;
    cpu->gdtr_offset |= high;
    write_reg32(EBX, CR4_PGE);
    reset_unimplemented();
    unimplemented_allowed = true;
    instr_0F22(EBX, 4); /* mov cr4, ebx */
    unimplemented_allowed = false;
    CHECK_EQ(unimplemented_calls, 1);
    CHECK(strstr(unimplemented_last, "gdtr_offset") != NULL);
    CHECK(strstr(unimplemented_last, "MOV CR4") != NULL);
    CHECK_EQ(cpu->cr[4], CR4_PGE);
    reset_unimplemented();

    /* the state is left POISONED above, and reset_cpu asserts the invariant: a test inserted
     * after this one would abort the binary on the stale high half (round 2's N5).  Clear it
     * here, not in the next test. */
    cpu_state_init();

    printf("PASS S3: the high-half invariant names each widened address field (6 scalars + 8"
           " segment slots) and is reached through ALL FOUR mode boundaries (reset, CR0, CR4,"
           " EFER -- each driven by its guest-reachable body), is clean at reset, is a no-op in"
           " long mode and is the poison probe's opposite control\n");
}

/* ------------------------------------------------------------------ *
 * gate 3 -- paging, the TLB and safe_* access
 * ------------------------------------------------------------------ */

#define MEM_SIZE  (8u << 20)
#define PD_PHYS   0x1000u
#define PT_PHYS   0x2000u
#define DATA_PHYS 0x3000u

static void paging_setup(void)
{
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    zero_memory(0, 0x8000);
    cpu->cr[0] = CR0_PE | CR0_PG;
    cpu->cr[1] = 0;
    cpu->cr[2] = 0;
    cpu->cr[3] = (int32_t)PD_PHYS;
    cpu->cr[4] = 0;
    cpu->protected_mode = true;
    cpu->cpl = 0;
    cpu->previous_ip = 0;
    cpu->instruction_pointer = 0;
    cpu->pagefault = false;
    reset_delivery();
}

/* map a 4 KiB page: pd_index/pt_index -> phys, with the given PDE/PTE flags */
static void map4k(uint32_t pd_index, uint32_t pt_index, uint32_t phys, int32_t pte_flags,
                  int32_t pde_flags)
{
    write32(PD_PHYS + 4 * pd_index, (int32_t)(PT_PHYS | PAGE_TABLE_PRESENT_MASK | pde_flags));
    write32(PT_PHYS + 4 * pt_index, (int32_t)(phys | PAGE_TABLE_PRESENT_MASK | pte_flags));
}

static void test_paging_4k(void)
{
    paging_setup();
    /* virtual 0x400000 (PD index 1, PT index 0) -> phys 0x3000, RW, supervisor */
    map4k(1, 0, DATA_PHYS, PAGE_TABLE_RW_MASK, PAGE_TABLE_RW_MASK);
    write32(DATA_PHYS, 0xCAFEBABE);

    uint32_t phys = translate_address_read_no_side_effects(0x400000);
    CHECK_EQ(phys, DATA_PHYS);
    CHECK(!cpu->pagefault);
    CHECK_EQ(safe_read32s(0x400000), (int32_t)0xCAFEBABE);
    CHECK(!cpu->pagefault);

    /* side_effects: the accessed bit is set on the PTE and on the PDE (cpu.rs:2250-2264) */
    CHECK_EQ(read32s(PT_PHYS + 0), (int32_t)(DATA_PHYS | 3 | PAGE_TABLE_ACCESSED_MASK));
    CHECK_EQ(read32s(PD_PHYS + 4), (int32_t)(PT_PHYS | 3 | PAGE_TABLE_ACCESSED_MASK));

    /* a READ caches an entry a WRITE must not accept (the TLB_READONLY bit) */
    CHECK_EQ(safe_read32s(0x400000), (int32_t)0xCAFEBABE);
    safe_write32(0x400000, 0x11223344);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read32s(DATA_PHYS), (int32_t)0x11223344);
    CHECK_EQ(safe_read32s(0x400000), (int32_t)0x11223344);
    /* the dirty bit is set on the PTE only */
    CHECK_EQ(read32s(PT_PHYS + 0), (int32_t)(DATA_PHYS | 3 | 0x20 | 0x40));
    CHECK_EQ(read32s(PD_PHYS + 4), (int32_t)(PT_PHYS | 3 | 0x20));

    /* paging disabled: identity (cpu.rs:2084-2087) */
    cpu->cr[0] = CR0_PE;
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    CHECK_EQ(translate_address_read_no_side_effects(0x12345), 0x12345u);
    CHECK(!cpu->pagefault);
    cpu->cr[0] = CR0_PE | CR0_PG;

    printf("PASS paging: 4 KiB translation, A/D bits, TLB read-only entry\n");
}

static void test_paging_faults(void)
{
    /* physical page bases must be 4 KiB aligned: do_page_walk masks the PTE
     * (cpu.rs:2268) and the TLB entry XORs the page base with the virtual address,
     * both of which silently truncate a misaligned page */
    paging_setup();
    map4k(1, 0, 0x3000, PAGE_TABLE_RW_MASK, PAGE_TABLE_RW_MASK); /* 0x400000 RW */
    map4k(1, 3, 0x4000, 1, PAGE_TABLE_RW_MASK);                  /* 0x403000 RO */
    map4k(1, 4, 0x5000, PAGE_TABLE_RW_MASK | PAGE_TABLE_USER_MASK,
          PAGE_TABLE_RW_MASK | PAGE_TABLE_USER_MASK); /* 0x404000 user, both levels */
    map4k(1, 5, 0x6000, PAGE_TABLE_RW_MASK, PAGE_TABLE_RW_MASK | PAGE_TABLE_USER_MASK);
    /* 0x405000: user PDE, supervisor PTE */
    write32(0x5000, 0x42); /* the data behind the user page */
    write32(0x6000, 0x24); /* the data behind the supervisor PTE */

    /* Each simulated instruction starts by clearing the fault flag: that is exactly the
     * dispatcher's job under the plumbing contract (cpu_state.h). */
    cpu->previous_ip = 0xDEAD;
    cpu->instruction_pointer = 0x1234;

    /* (a) absent PTE (0x402000): fault, cr2, eip reset, delivery, present bit clear */
    cpu->pagefault = false;
    reset_delivery();
    int32_t v = safe_read32s(0x402000);
    CHECK(cpu->pagefault);
    CHECK_EQ(v, 0);
    CHECK_EQ(cpu->cr[2], 0x402000);
    CHECK_EQ(cpu->instruction_pointer, 0xDEAD); /* *instruction_pointer = *previous_ip */
    CHECK_EQ(delivery_count, 1);
    CHECK_EQ(delivered_interrupt, CPU_EXCEPTION_PF);
    CHECK_EQ(delivered_error_code, 0); /* P=0 W=0 U=0 */
    CHECK_EQ(tlb_data[0x402], 0);      /* the faulting entry is invalidated */

    /* no_side_effects does NOT deliver, but still reports the fault (upstream's Err) */
    cpu->pagefault = false;
    reset_delivery();
    translate_address_read_no_side_effects(0x402000);
    CHECK(cpu->pagefault);
    CHECK_EQ(delivery_count, 0);

    /* the fault flag is SET-ONLY and gates the returned value: a later call on a GOOD
     * address yields its sentinel while the flag is still set ... */
    cpu->pagefault = false;
    reset_delivery();
    safe_read32s(0x402000);
    CHECK(cpu->pagefault);
    CHECK_EQ(translate_address_read_no_side_effects(0x400000), 0x0u);
    CHECK(cpu->pagefault);
    /* ... and the value is correct once the dispatcher clears it */
    cpu->pagefault = false;
    CHECK_EQ(translate_address_read_no_side_effects(0x400000), 0x3000u);
    CHECK(!cpu->pagefault);

    /* (b) a supervisor write to a read-only page with CR0.WP=0 is ALLOWED
     * (kernel_write_override, cpu.rs:2155) */
    cpu->cpl = 0;
    cpu->cr[0] = CR0_PE | CR0_PG;
    cpu->pagefault = false;
    reset_delivery();
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    safe_write32(0x403000, 0x55);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read32s(0x4000), 0x55);
    CHECK_EQ(delivery_count, 0);

    /* (c) the same write with CR0.WP=1 faults: P=1 W=1 U=0 -> error code 3 */
    cpu->cr[0] = CR0_PE | CR0_PG | CR0_WP;
    cpu->pagefault = false;
    reset_delivery();
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    safe_write32(0x403000, 0x66);
    CHECK(cpu->pagefault);
    CHECK_EQ(delivered_error_code, 3);
    CHECK_EQ(cpu->cr[2], 0x403000);
    CHECK_EQ(read32s(0x4000), 0x55); /* no partial effect */

    /* (d) user accesses: a read of a user page succeeds; a WRITE to a supervisor page
     * faults with P|W|U = 7 (the W bit describes the ACCESS, not the page) and a READ
     * of the same page with P|U = 5 */
    cpu->cr[0] = CR0_PE | CR0_PG;
    cpu->cpl = 3;
    cpu->pagefault = false;
    reset_delivery();
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    CHECK_EQ(safe_read32s(0x404000), 0x42);
    CHECK(!cpu->pagefault);
    CHECK_EQ(delivery_count, 0);

    cpu->pagefault = false;
    reset_delivery();
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    safe_write32(0x405000, 0x77); /* user PDE, supervisor PTE */
    CHECK(cpu->pagefault);
    CHECK_EQ(delivered_error_code, 7);
    CHECK_EQ(cpu->cr[2], 0x405000);

    cpu->pagefault = false;
    reset_delivery();
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    CHECK_EQ(safe_read32s(0x405000), 0);
    CHECK(cpu->pagefault);
    CHECK_EQ(delivered_error_code, 5); /* P=1 W=0 U=1 */
    CHECK_EQ(cpu->cr[2], 0x405000);

    /* (e) a supervisor read of the same page is fine */
    cpu->cpl = 0;
    cpu->pagefault = false;
    reset_delivery();
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    CHECK_EQ(safe_read32s(0x405000), 0x24);
    CHECK(!cpu->pagefault);

    /* (f) an instruction fetch is a read (there is no NX without PAE/EFER.NXE) */
    cpu->pagefault = false;
    reset_delivery();
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    CHECK_EQ(translate_address_fetch(0x400000), 0x3000u);
    CHECK(!cpu->pagefault);

    printf("PASS paging: faults (absent, read-only/WP, user vs supervisor) + cr2/eip/"
           "delivery/error codes\n");
}

static void test_paging_pse(void)
{
    paging_setup();
    cpu->cr[4] = CR4_PSE;
    /* PD index 2 covers virtual 0x800000-0xBFFFFF; a 4 MiB page at phys 0x400000 */
    write32(PD_PHYS + 4 * 2, (int32_t)(0x00400000u | PAGE_TABLE_PSE_MASK |
                                       PAGE_TABLE_PRESENT_MASK | PAGE_TABLE_RW_MASK));

    CHECK_EQ(translate_address_read_no_side_effects(0x800000), 0x400000u);
    CHECK_EQ(translate_address_read_no_side_effects(0x801234), 0x401234u);
    CHECK_EQ(translate_address_read_no_side_effects(0x9FFFFF), 0x5FFFFFu);

    safe_write32(0x801234, 0xDEADBEEF);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read32s(0x401234), (int32_t)0xDEADBEEF);
    CHECK_EQ(safe_read32s(0x801234), (int32_t)0xDEADBEEF);
    /* A/D are set in the PDE for a 4 MiB page (cpu.rs:2163-2172) */
    CHECK_EQ(read32s(PD_PHYS + 4 * 2), (int32_t)(0x00400000u | 0x80 | 3 | 0x20 | 0x40));

    /* the page covers PD index 2 only: 0x7FFFFF is a different, absent PDE */
    cpu->pagefault = false;
    reset_delivery();
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    safe_read32s(0x7FFFFF);
    CHECK(cpu->pagefault);

    printf("PASS paging: 4 MiB PSE page (translation, A/D, page boundary)\n");
}

/* ------------------------------------------------------------------ *
 * PAE paging (CR4.PAE): the port's 3-level walk.  Upstream cpu.rs:2054-2336.
 *
 * PAE geometry: 2 bits PDPT | 9 bits PD | 9 bits PT | 12 bits offset, so a
 * 4 KiB page at PDPT[0]/PD[1]/PT[0] is virtual 0x200000, and a 2 MiB page at
 * PDPT[0]/PD[2] is virtual 0x400000..0x5FFFFF.  Entries are 64-bit; the walk
 * requires the high half to be 0 except NX (cpu.rs:2058), which the selftest
 * pins through the reserved-bit and NX cases.
 * ------------------------------------------------------------------ */

#define PAE_PDPT_PHYS 0x1000u
#define PAE_PD_PHYS   0x2000u
#define PAE_PT_PHYS   0x4000u
#define PAE_DATA_PHYS 0x5000u

static void pae_setup(void)
{
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    zero_memory(0, 0x8000);
    cpu->cr[0] = CR0_PE | CR0_PG;
    cpu->cr[1] = 0;
    cpu->cr[2] = 0;
    cpu->cr[3] = (int32_t)PAE_PDPT_PHYS;
    cpu->cr[4] = CR4_PAE;
    cpu->efer = 0;
    cpu->protected_mode = true;
    cpu->cpl = 0;
    cpu->previous_ip = 0;
    cpu->instruction_pointer = 0;
    cpu->pagefault = false;
    reset_delivery();
    reset_exceptions();
}

static void test_pae_4k(void)
{
    pae_setup();
    write64_no_mmap_or_dirty_check(PAE_PDPT_PHYS + 0,
                                   (uint64_t)(PAE_PD_PHYS | PAGE_TABLE_PRESENT_MASK));
    write64_no_mmap_or_dirty_check(PAE_PD_PHYS + 8 * 1,
                                   (uint64_t)(PAE_PT_PHYS | PAGE_TABLE_PRESENT_MASK |
                                              PAGE_TABLE_RW_MASK));
    write64_no_mmap_or_dirty_check(PAE_PT_PHYS + 8 * 0,
                                   (uint64_t)(PAE_DATA_PHYS | PAGE_TABLE_PRESENT_MASK |
                                              PAGE_TABLE_RW_MASK));
    load_pdpte((int32_t)PAE_PDPT_PHYS);
    write32(PAE_DATA_PHYS, 0xCAFEBABE);

    CHECK_EQ(translate_address_read_no_side_effects(0x200000), PAE_DATA_PHYS);
    CHECK(!cpu->pagefault);
    CHECK_EQ(safe_read32s(0x200000), (int32_t)0xCAFEBABE);
    CHECK(!cpu->pagefault);

    /* the A/D updates write ONE byte (write8), so a 64-bit entry's high 32 bits
     * are untouched -- read64s proves the value end to end */
    CHECK_EQ(read64s(PAE_PD_PHYS + 8 * 1),
             (int64_t)(PAE_PT_PHYS | 3 | PAGE_TABLE_ACCESSED_MASK));
    CHECK_EQ(read64s(PAE_PT_PHYS + 8 * 0),
             (int64_t)(PAE_DATA_PHYS | 3 | PAGE_TABLE_ACCESSED_MASK));

    safe_write32(0x200000, 0x11223344);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read32s(PAE_DATA_PHYS), (int32_t)0x11223344);
    CHECK_EQ(read64s(PAE_PT_PHYS + 8 * 0),
             (int64_t)(PAE_DATA_PHYS | 3 | PAGE_TABLE_ACCESSED_MASK | PAGE_TABLE_DIRTY_MASK));

    printf("PASS paging: PAE 4 KiB translation + A/D bits (high 32 bits untouched)\n");
}

static void test_pae_2mb(void)
{
    pae_setup();
    write64_no_mmap_or_dirty_check(PAE_PDPT_PHYS + 0,
                                   (uint64_t)(PAE_PD_PHYS | PAGE_TABLE_PRESENT_MASK));
    write64_no_mmap_or_dirty_check(PAE_PD_PHYS + 8 * 2,
                                   (uint64_t)(0x00600000u | PAGE_TABLE_PSE_MASK |
                                              PAGE_TABLE_PRESENT_MASK | PAGE_TABLE_RW_MASK));
    load_pdpte((int32_t)PAE_PDPT_PHYS);

    /* the 0x401234 offset is the discriminator for the 2 MiB high composition
     * (0xFFE00000 | 0x1FF000): a 4 MiB mask (0xFFC00000 | 0x3FF000) yields 0x40234 */
    CHECK_EQ(translate_address_read_no_side_effects(0x400000), 0x600000u);
    CHECK_EQ(translate_address_read_no_side_effects(0x401234), 0x601234u);
    CHECK_EQ(translate_address_read_no_side_effects(0x5FFFFF), 0x7FFFFFu);

    safe_write32(0x401234, 0xDEADBEEF);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read32s(0x601234), (int32_t)0xDEADBEEF);
    CHECK_EQ(read64s(PAE_PD_PHYS + 8 * 2),
             (int64_t)(0x00600000u | 0x80 | 3 | PAGE_TABLE_ACCESSED_MASK |
                       PAGE_TABLE_DIRTY_MASK));

    /* the next 2 MiB (PD[3]) is absent */
    cpu->pagefault = false;
    reset_delivery();
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    CHECK_EQ(safe_read32s(0x600000), 0);
    CHECK(cpu->pagefault);
    CHECK_EQ(delivered_error_code, 0); /* present=0 */

    printf("PASS paging: PAE 2 MiB translation (0x1FF000 boundary), A/D, next-page fault\n");
}

static void test_pae_reserved(void)
{
    pae_setup();
    write64_no_mmap_or_dirty_check(PAE_PDPT_PHYS + 0,
                                   (uint64_t)(PAE_PD_PHYS | PAGE_TABLE_PRESENT_MASK));
    /* bits 13-20 are reserved in a 2 MiB PDE: bit 13 -> rsvd fault (bit 3) */
    write64_no_mmap_or_dirty_check(PAE_PD_PHYS + 8 * 0,
                                   (uint64_t)(0x00600000u | PAGE_TABLE_PSE_MASK |
                                              PAGE_TABLE_PRESENT_MASK | 0x2000));
    load_pdpte((int32_t)PAE_PDPT_PHYS);

    cpu->previous_ip = 0xDEAD;
    cpu->instruction_pointer = 0x1234;
    cpu->pagefault = false;
    reset_delivery();
    CHECK_EQ(safe_read32s(0x000000), 0);
    CHECK(cpu->pagefault);
    CHECK_EQ(delivered_error_code, 9); /* rsvd(1<<3) | present(1) */
    CHECK_EQ(cpu->cr[2], 0x000000);

    /* a 4 KiB PTE with the high half set (bit 32): reserved -> rsvd fault */
    pae_setup();
    write64_no_mmap_or_dirty_check(PAE_PDPT_PHYS + 0,
                                   (uint64_t)(PAE_PD_PHYS | PAGE_TABLE_PRESENT_MASK));
    write64_no_mmap_or_dirty_check(PAE_PD_PHYS + 8 * 1,
                                   (uint64_t)(PAE_PT_PHYS | PAGE_TABLE_PRESENT_MASK |
                                              PAGE_TABLE_RW_MASK));
    write64_no_mmap_or_dirty_check(PAE_PT_PHYS + 8 * 0, 0x0000000100000003ull);
    load_pdpte((int32_t)PAE_PDPT_PHYS);

    cpu->previous_ip = 0xDEAD;
    cpu->instruction_pointer = 0x1234;
    cpu->pagefault = false;
    reset_delivery();
    CHECK_EQ(safe_read32s(0x200000), 0);
    CHECK(cpu->pagefault);
    CHECK_EQ(delivered_error_code, 9);
    CHECK_EQ(cpu->cr[2], 0x200000);

    /* a 4 KiB PDE with the high half set (bit 32): reserved -> rsvd fault.  This is
     * the silent-wrong shape the uint64_t reserved check guards: an int32_t
     * truncation would drop bit 32, and the walk would silently translate through
     * the (valid) PTE behind it instead of faulting. */
    pae_setup();
    write64_no_mmap_or_dirty_check(PAE_PDPT_PHYS + 0,
                                   (uint64_t)(PAE_PD_PHYS | PAGE_TABLE_PRESENT_MASK));
    write64_no_mmap_or_dirty_check(PAE_PD_PHYS + 8 * 1,
                                   0x0000000100004003ull); /* bit 32 + PT + RW + present */
    write64_no_mmap_or_dirty_check(PAE_PT_PHYS + 8 * 0,
                                   (uint64_t)(PAE_DATA_PHYS | PAGE_TABLE_PRESENT_MASK |
                                              PAGE_TABLE_RW_MASK));
    load_pdpte((int32_t)PAE_PDPT_PHYS);

    cpu->previous_ip = 0xDEAD;
    cpu->instruction_pointer = 0x1234;
    cpu->pagefault = false;
    reset_delivery();
    CHECK_EQ(safe_read32s(0x200000), 0);
    CHECK(cpu->pagefault);
    CHECK_EQ(delivered_error_code, 9);
    CHECK_EQ(cpu->cr[2], 0x200000);

    printf("PASS paging: PAE reserved bits (2 MiB bits 13-20, PDE/PTE high half) -> rsvd fault\n");
}

static void test_pae_pdpte(void)
{
    /* an absent PDPTE is a page fault with present=0 */
    pae_setup();
    write64_no_mmap_or_dirty_check(PAE_PDPT_PHYS + 0, 0ull);
    load_pdpte((int32_t)PAE_PDPT_PHYS);

    cpu->previous_ip = 0xDEAD;
    cpu->instruction_pointer = 0x1234;
    cpu->pagefault = false;
    reset_delivery();
    CHECK_EQ(safe_read32s(0x000000), 0);
    CHECK(cpu->pagefault);
    CHECK_EQ(delivered_error_code, 0); /* present=0 */
    CHECK_EQ(cpu->cr[2], 0x000000);

    printf("PASS paging: PAE absent PDPTE -> #PF present=0\n");
}

static void test_pae_nx(void)
{
    /* NX in the PTE: with NXE off it is reserved; with NXE on, data reads
     * succeed and instruction fetches fault with the I/D bit (bit 4). */
    pae_setup();
    cpu->efer = 0; /* NXE off */
    write64_no_mmap_or_dirty_check(PAE_PDPT_PHYS + 0,
                                   (uint64_t)(PAE_PD_PHYS | PAGE_TABLE_PRESENT_MASK));
    write64_no_mmap_or_dirty_check(PAE_PD_PHYS + 8 * 1,
                                   (uint64_t)(PAE_PT_PHYS | PAGE_TABLE_PRESENT_MASK |
                                              PAGE_TABLE_RW_MASK));
    write64_no_mmap_or_dirty_check(PAE_PT_PHYS + 8 * 0,
                                   (uint64_t)(PAE_DATA_PHYS | PAGE_TABLE_PRESENT_MASK |
                                              PAGE_TABLE_RW_MASK | PAGE_TABLE_NX_MASK));
    load_pdpte((int32_t)PAE_PDPT_PHYS);
    write32(PAE_DATA_PHYS, 0x11223344);

    cpu->previous_ip = 0xDEAD;
    cpu->instruction_pointer = 0x1234;
    cpu->pagefault = false;
    reset_delivery();
    CHECK_EQ(safe_read32s(0x200000), 0);
    CHECK(cpu->pagefault);
    CHECK_EQ(delivered_error_code, 9); /* NX is reserved when NXE is off */

    cpu->efer = EFER_NXE;
    cpu->pagefault = false;
    reset_delivery();
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    CHECK_EQ(safe_read32s(0x200000), (int32_t)0x11223344);
    CHECK(!cpu->pagefault);
    CHECK((tlb_data[0x200] & TLB_NO_EXEC) != 0);

    cpu->pagefault = false;
    reset_delivery();
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    CHECK_EQ(translate_address_fetch(0x200000), 0u);
    CHECK(cpu->pagefault);
    CHECK_EQ(delivered_error_code, 17); /* I/D(1<<4) | present(1) */

    printf("PASS paging: PAE NX in PTE (rsvd w/o NXE, fetch-fault w/ NXE, TLB_NO_EXEC)\n");
}

static void test_pae_nx_pde(void)
{
    /* NX on the PDE of a 4 KiB mapping must carry to the fetch: the 4 KiB arm
     * ORs the PTE's NX into no_exec; an overwrite (=) would drop the PDE's. */
    pae_setup();
    cpu->efer = EFER_NXE;
    write64_no_mmap_or_dirty_check(PAE_PDPT_PHYS + 0,
                                   (uint64_t)(PAE_PD_PHYS | PAGE_TABLE_PRESENT_MASK));
    write64_no_mmap_or_dirty_check(PAE_PD_PHYS + 8 * 1,
                                   (uint64_t)(PAE_PT_PHYS | PAGE_TABLE_PRESENT_MASK |
                                              PAGE_TABLE_RW_MASK | PAGE_TABLE_NX_MASK));
    write64_no_mmap_or_dirty_check(PAE_PT_PHYS + 8 * 0,
                                   (uint64_t)(PAE_DATA_PHYS | PAGE_TABLE_PRESENT_MASK |
                                              PAGE_TABLE_RW_MASK));
    load_pdpte((int32_t)PAE_PDPT_PHYS);
    write32(PAE_DATA_PHYS, 0x11223344);

    CHECK_EQ(safe_read32s(0x200000), (int32_t)0x11223344);
    CHECK(!cpu->pagefault);

    cpu->pagefault = false;
    reset_delivery();
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    CHECK_EQ(translate_address_fetch(0x200000), 0u);
    CHECK(cpu->pagefault);
    CHECK_EQ(delivered_error_code, 17);

    printf("PASS paging: PAE NX on the PDE of a 4 KiB mapping (no_exec carries)\n");
}

static void test_pae_gp(void)
{
    /* A sentinel CR3 that differs from any value set_cr3 would commit, so the
     * "CR3 unwritten" checks below actually discriminate (review F4: pre-setting
     * cr[3] to the committed value made "unwritten" and "written" indistinguishable
     * and the old test could not fail). */
    const int32_t cr3_sentinel = 0xDEAD0000;

    /* load_pdpte's guest-reachable asserts became #GP(0) (plan Q1): a high-half
     * PDPTE faults at MOV CR3 and CR3 stays unwritten. */
    pae_setup();
    write64_no_mmap_or_dirty_check(PAE_PDPT_PHYS + 0, 0x0000000100000001ull);
    cpu->cr[3] = cr3_sentinel;
    reset_exceptions();
    set_cr3((int32_t)PAE_PDPT_PHYS);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_GP);
    CHECK_EQ(exc_code, 0);
    CHECK_EQ(cpu->cr[3], cr3_sentinel); /* CR3 unwritten */

    /* a reserved PDPTE bit (bit 1) with present set also faults */
    pae_setup();
    write64_no_mmap_or_dirty_check(PAE_PDPT_PHYS + 0, (uint64_t)(PAE_PD_PHYS | 1 | 0x2));
    cpu->cr[3] = cr3_sentinel;
    reset_exceptions();
    set_cr3((int32_t)PAE_PDPT_PHYS);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_GP);
    CHECK_EQ(exc_code, 0);
    CHECK_EQ(cpu->cr[3], cr3_sentinel);

    /* PWT/PCD (bits 3,4) and ignored bits 9-11 are accepted and masked */
    pae_setup();
    write64_no_mmap_or_dirty_check(PAE_PDPT_PHYS + 0,
                                   (uint64_t)(PAE_PD_PHYS | 1 | 0x8 | 0x10 | 0x600));
    cpu->cr[3] = cr3_sentinel;
    reset_exceptions();
    set_cr3((int32_t)PAE_PDPT_PHYS);
    CHECK_EQ(exc_vector, -1); /* no fault */
    CHECK_EQ(cpu->cr[3], (int32_t)PAE_PDPT_PHYS); /* committed (sentinel != committed) */
    CHECK_EQ(cpu->reg_pdpte[0], (uint64_t)(PAE_PD_PHYS | 1));

    /* the set_cr0 path faults atomically too: CR0 stays uncommitted (a commit
     * would also set CR0_ET and CR0_CD) */
    pae_setup();
    write64_no_mmap_or_dirty_check(PAE_PDPT_PHYS + 0, 0x0000000100000001ull);
    reset_exceptions();
    set_cr0(CR0_PE | CR0_PG | CR0_CD); /* CD delta -> load_pdpte runs */
    CHECK_EQ(exc_vector, CPU_EXCEPTION_GP);
    CHECK_EQ(exc_code, 0);
    CHECK_EQ(cpu->cr[0], CR0_PE | CR0_PG); /* CR0 unwritten */

    /* THE CR4 WRITE PATH DOES NOT RELOAD PDPTEs ON A PSE/PGE CHANGE (corrected in the pdpte
     * unit, results/x86-64-pdpte.md).  The SDM reloads the PDPTEs when CR4.PAE is SET (a 0->1
     * transition); changing PSE, PGE or SMEP only flushes the TLB.  This test used to assert a
     * #GP here because the port reloaded on any of the four -- and in long mode that reload
     * read the guest's PML4 entries with the 32-bit PAE PDPTE rules and faulted on a legal
     * PML4E, which is what stopped the amd64 kernel.  The old assertion was the DEFECT, so it
     * is corrected rather than kept: with the bad PDPT in place and PAE already set, the write
     * now commits and no exception is recorded.  The PAE 0->1 fault path is still exercised by
     * the CR0 test above (which sets PAE and PG together) and by gate-pae-1. */
    pae_setup();
    write64_no_mmap_or_dirty_check(PAE_PDPT_PHYS + 0, 0x0000000100000001ull);
    reset_exceptions();
    write_reg32(EAX, (int32_t)(CR4_PAE | CR4_PSE));
    instr_0F22(EAX, 4);
    CHECK_EQ(exc_vector, (uint64_t)-1);          /* no fault: no PDPTE reload */
    CHECK_EQ(cpu->cr[4], CR4_PAE | CR4_PSE);     /* and the write committed */

    printf("PASS paging: PAE load_pdpte #GP (high-half, reserved bit, masking) + CR fault atomicity\n");
}

static void test_cr3_nonpae_mask(void)
{
    /* F2 (review, PRE-EXISTING, reproduced by the orchestrator): a non-PAE
     * MOV CR3 with PWT/PCD set (bits 3/4 -- legal CR3 bits) aborted the SHIPPED
     * binary on the old DBG_ASSERT((cr3 & 0xFFF) == 0); the pinned reference runs
     * it to completion.  The port now matches: mask ~0xFE7 (upstream) keeps
     * PWT/PCD, no abort.  Pre-fix this case aborted (SIGABRT) before the first
     * CHECK. */
    paging_setup();
    cpu->cr[4] = 0; /* non-PAE (already 0 from paging_setup, stated) */
    reset_delivery();
    reset_exceptions();
    set_cr3(0x1008); /* 4 KiB base + PWT (bit 3) */
    CHECK_EQ(cpu->cr[3], 0x1008); /* PWT preserved by the ~0xFE7 mask */
    CHECK_EQ(exc_vector, -1);     /* no fault -- and, crucially, no abort */
    set_cr3(0x1010); /* 4 KiB base + PCD (bit 4) */
    CHECK_EQ(cpu->cr[3], 0x1010);
    printf("PASS paging: non-PAE MOV CR3 with PWT/PCD set does not abort (mask ~0xFE7)\n");
}

static void test_paging_crossing(void)
{
    paging_setup();
    /* virtual 0x40A000 -> phys 0x4000, virtual 0x40B000 -> phys 0x5000 */
    map4k(1, 10, 0x4000, PAGE_TABLE_RW_MASK, PAGE_TABLE_RW_MASK);
    map4k(1, 11, 0x5000, PAGE_TABLE_RW_MASK, PAGE_TABLE_RW_MASK);
    mem8[0x4FFE] = 0x11;
    mem8[0x4FFF] = 0x22;
    mem8[0x5000] = 0x33;
    mem8[0x5001] = 0x44;

    CHECK_EQ(safe_read32s(0x40AFFE), (int32_t)0x44332211);
    CHECK(!cpu->pagefault);
    CHECK_EQ(safe_read16(0x40AFFF), 0x3322);
    CHECK_EQ(safe_read8(0x40AFFF), 0x22);
    CHECK_EQ(safe_read8(0x40B000), 0x33);

    /* the write halves go through virt_boundary_write16/32 (cpu.rs:3506-3530) */
    safe_write32(0x40AFFE, 0x55667788);
    CHECK(!cpu->pagefault);
    CHECK_EQ(mem8[0x4FFE], 0x88);
    CHECK_EQ(mem8[0x4FFF], 0x77);
    CHECK_EQ(mem8[0x5000], 0x66);
    CHECK_EQ(mem8[0x5001], 0x55);
    CHECK_EQ(safe_read16(0x40AFFF), 0x6677); /* 0x4FFF=0x77, 0x5000=0x66 */

    safe_write16(0x40AFFF, 0xAABB);
    CHECK(!cpu->pagefault);
    CHECK_EQ(mem8[0x4FFF], 0xBB);
    CHECK_EQ(mem8[0x5000], 0xAA);

    /* 64/128-bit straddles (thresholds `> 0x1000-8` / `> 0x1000-16`) */
    safe_write64(0x40AFFC, 0x1122334455667788ull);
    CHECK(!cpu->pagefault);
    CHECK_EQ(mem8[0x4FFC], 0x88);
    CHECK_EQ(mem8[0x4FFF], 0x55);
    CHECK_EQ(mem8[0x5000], 0x44);
    CHECK_EQ(mem8[0x5003], 0x11);
    CHECK_EQ((uint64_t)safe_read64s(0x40AFFC), 0x1122334455667788ull);

    reg128 x;
    x.u64[0] = 0x0123456789ABCDEFull;
    x.u64[1] = 0xFEDCBA9876543210ull;
    safe_write128(0x40AFF4, x);
    CHECK(!cpu->pagefault);
    reg128 y = safe_read128s(0x40AFF4);
    CHECK(!cpu->pagefault);
    CHECK_EQ(y.u64[0], x.u64[0]);
    CHECK_EQ(y.u64[1], x.u64[1]);

    /* a straddle whose SECOND page is absent faults with cr2 = the second page's
     * virtual address (safe_read16 recurses into the next page) */
    write32(PT_PHYS + 4 * 11, 0);
    cpu->pagefault = false;
    reset_delivery();
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    int32_t v = safe_read32s(0x40AFFE);
    CHECK(cpu->pagefault);
    CHECK_EQ(v, 0);
    CHECK_EQ(cpu->cr[2], 0x40B000);
    CHECK_EQ(delivered_error_code, 0);

    printf("PASS paging: page-crossing reads/writes (16/32/64/128-bit) + straddle fault\n");
}

static void test_tlb(void)
{
    paging_setup();
    map4k(1, 0, DATA_PHYS, PAGE_TABLE_RW_MASK, PAGE_TABLE_RW_MASK);

    /* a walk inserts the page into valid_tlb_entries (cpu.rs:2272-2292) */
    safe_read32s(0x400000);
    CHECK(tlb_data[0x400] != 0);
    CHECK(valid_tlb_entries_count >= 1);

    /* invlpg drops the entry from tlb_data but NOT from valid_tlb_entries
     * (cpu.rs:4469-4473), and invalidates last_virt_eip */
    invlpg(0x400000);
    CHECK_EQ(tlb_data[0x400], 0);
    CHECK_EQ(cpu->last_virt_eip, VIRT_EIP_NONE); /* S3: the sentinel's new spelling */
    CHECK(valid_tlb_entries_count >= 1);

    /* set_cr3 -> clear_tlb: non-global entries go; cr3's low bits are masked off
     * (cpu.rs:2959-2975: cr3 &= !0b111111100111; 0x2007 & ~0x7E7 == 0x2000) */
    safe_read32s(0x400000);
    CHECK(tlb_data[0x400] != 0);
    set_cr3((int32_t)PD_PHYS);
    CHECK_EQ(tlb_data[0x400], 0);
    CHECK_EQ(valid_tlb_entries_count, 0);
    CHECK_EQ(cpu->cr[3], (int32_t)PD_PHYS);
    set_cr3(0x2007);
    CHECK_EQ(cpu->cr[3], 0x2000);

    /* a global page survives clear_tlb (CR4.PGE + PTE.G) */
    paging_setup();
    cpu->cr[4] = CR4_PGE;
    map4k(1, 0, DATA_PHYS, PAGE_TABLE_RW_MASK | PAGE_TABLE_GLOBAL_MASK, PAGE_TABLE_RW_MASK);
    safe_read32s(0x400000);
    CHECK((tlb_data[0x400] & TLB_GLOBAL) != 0);
    clear_tlb();
    CHECK(tlb_data[0x400] != 0);
    CHECK_EQ(valid_tlb_entries_count, 1);
    full_clear_tlb();
    CHECK_EQ(tlb_data[0x400], 0);
    CHECK_EQ(valid_tlb_entries_count, 0);

    /* set_cr0 with a PG/WP change flushes the whole TLB (cpu.rs:2944-2946) */
    paging_setup();
    map4k(1, 0, DATA_PHYS, PAGE_TABLE_RW_MASK, PAGE_TABLE_RW_MASK);
    safe_read32s(0x400000);
    CHECK(tlb_data[0x400] != 0);
    set_cr0(CR0_PE | CR0_PG); /* same value: no flush */
    CHECK(tlb_data[0x400] != 0);
    set_cr0(CR0_PE | CR0_PG | CR0_WP); /* WP change: flush */
    CHECK_EQ(tlb_data[0x400], 0);
    CHECK(!cpu->pagefault);
    CHECK((cpu->cr[0] & CR0_ET) != 0); /* CR0_ET is forced on (cpu.rs:2942) */

    printf("PASS TLB: insert, invlpg, clear_tlb/global, set_cr3/set_cr0 flushes\n");
}

/* ------------------------------------------------------------------ *
 * gate 3b -- the memory router and the device hook table
 * ------------------------------------------------------------------ */

static int32_t hook_reads = 0;
static int32_t hook_writes = 0;
static int32_t hook_last_value = 0;

static int32_t hook_read32(uint32_t addr)
{
    (void)addr;
    hook_reads++;
    return 0x42424242;
}
static void hook_write32(uint32_t addr, int32_t value)
{
    (void)addr;
    hook_writes++;
    hook_last_value = value;
}

static void test_memory_router(void)
{
    zero_memory(0, 0x1000);
    write8(0x100, 0xAB);
    CHECK_EQ(read8(0x100), 0xAB);
    write16(0x102, 0xBEEF);
    CHECK_EQ(read16(0x102), 0xBEEF);
    write32(0x104, 0xDEADBEEF);
    CHECK_EQ(read32s(0x104), (int32_t)0xDEADBEEF);
    CHECK_EQ(read64s(0x104), 0xDEADBEEFu);

    /* the +16-byte slack past memory_size (plan ss3, R7): a 4-byte load starting at
     * the LAST byte of guest RAM runs 3 bytes into the slack instead of off the end */
    mem8[MEM_SIZE - 1] = 0x7F;
    CHECK_EQ(read32_no_mmap_check(MEM_SIZE - 1), (int32_t)0x0000007F);
    mem8[MEM_SIZE] = 0x11; /* inside the 16-byte slack, past memory_size */
    CHECK_EQ(read32_no_mmap_check(MEM_SIZE - 1), (int32_t)0x0000117F);
    /* the slack is 16 bytes because read32_no_mmap_check loads unconditionally, and
     * the bound is readable so the claim is testable rather than a comment */
    CHECK_EQ(mem8_alloc_size(), MEM_SIZE + 16);
    mem8[MEM_SIZE + 15] = 0x5A; /* the LAST byte the allocation covers */
    CHECK_EQ(mem8[MEM_SIZE + 15], 0x5A);
    mem8[MEM_SIZE + 15] = 0;
    mem8[MEM_SIZE] = 0;
    mem8[MEM_SIZE - 1] = 0;

    /* is_memory_zeroed + zero_memory */
    CHECK(is_memory_zeroed(0x200, 0x100));
    write32(0x204, 1);
    CHECK(!is_memory_zeroed(0x200, 0x100));
    zero_memory(0x200, 0x100);
    CHECK(is_memory_zeroed(0x200, 0x100));

    /* in_mapped_range (memory.rs:64-66) */
    CHECK(!in_mapped_range(0x1000));
    CHECK(in_mapped_range(0xA0000));
    CHECK(in_mapped_range(0xBFFFF));
    CHECK(!in_mapped_range(0xC0000));
    CHECK(in_mapped_range(MEM_SIZE));
    CHECK(!in_svga_lfb(0xE0000000u)); /* vga_memory_size == 0 */

    /* the default device table answers the io.js:40-61 unclaimed-memory values and
     * swallows writes.  F1 REGRESSION PROBE: these six checks all FAILED before the
     * fix, when the default hooks returned 0 for every address. */
    CHECK_EQ(read8(MEM_SIZE + 0x100), 0xFF);
    CHECK_EQ(read16(MEM_SIZE + 0x100), 0xFFFF);
    CHECK_EQ(read32s(MEM_SIZE + 0x100), -1);
    write32(MEM_SIZE + 0x100, 0x1234);
    CHECK_EQ(read8(MEM_SIZE + 0x100), 0xFF);
    CHECK_EQ(read32s(MEM_SIZE + 0x100), -1);

    /* the VGA window and the LFB keep reading the zeroed VGA buffer the reference
     * always has at boot (MEASURED: read32 [0xB8000] = 0, [0xE0000000] = 0,
     * [0xE07FFFFF] = 0) -- only the rest of the unclaimed range takes 0xFF/-1 */
    CHECK_EQ(read8(0xB8000), 0);
    CHECK_EQ(read32s(0xB8000), 0);
    CHECK_EQ(read8(0xE0000000u), 0);
    CHECK_EQ(read32s(0xE0000000u), 0);
    CHECK_EQ(read32s(0xE07FFFFCu), 0);  /* last dword of the default 8 MiB LFB */
    CHECK_EQ(read32s(0xE0800000u), -1); /* one past it: unclaimed again */

    /* an installed hook is used, and NULL restores the default table */
    memory_device_hooks_t hooks = *memory_default_device_hooks();
    hooks.read32 = hook_read32;
    hooks.write32 = hook_write32;
    memory_set_device_hooks(&hooks);
    CHECK_EQ(read32s(MEM_SIZE + 0x100), 0x42424242);
    CHECK_EQ(hook_reads, 1);
    CHECK_EQ(hook_writes, 0);
    write32(MEM_SIZE + 0x200, 0x1337);
    CHECK_EQ(hook_writes, 1);
    CHECK_EQ(hook_last_value, 0x1337);
    memory_set_device_hooks(NULL);
    CHECK_EQ(read32s(MEM_SIZE + 0x100), -1);

    /* the APIC/IOAPIC windows keep their addresses and are stubbed (plan ss3) */
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x100), 0);
    CHECK_EQ(read32s(IOAPIC_MEM_ADDRESS), 0);
    write32(APIC_MEM_ADDRESS + 0x100, 0x99);
    write32(IOAPIC_MEM_ADDRESS, 0x99);

    printf("PASS memory: router, slack at memory_size, device hook table, mmio windows\n");
}

/* ================================================================== *
 * M1a-2: arith.c, modrm.c, misc_instr.c
 *
 * The tables below are NOT hand-derived: every expected value in them was produced by
 * executing the same operation on the HOST CPU and captured from its real result and
 * flags (see results/m1a2-arith-modrm-misc.md, "gate 2b" and results/m1a2-oracle.txt).
 * Rows with exp_of == -1 are cases where the SDM leaves OF undefined (any count != 1);
 * the port's OF there is upstream's formula transcribed, and is asserted against
 * nothing -- deliberately, because there is nothing to assert it against.
 * ================================================================== */

/* ------------------------------------------------------------------ *
 * The M1b/M1c stand-ins.
 *
 * exceptions.c (M1c) owns trigger_de/trigger_ud/trigger_nm/trigger_gp and switch_seg;
 * they do not exist until M1c lands, so this binary supplies recording definitions --
 * exactly what M1a-1 did for call_interrupt_vector, for the same reason (nothing
 * links without them).  M1c replaces them; nothing here shadows a real definition.
 * ------------------------------------------------------------------ */
void trigger_de(void)
{
    exc_vector = CPU_EXCEPTION_DE;
    exc_count++;
}
void trigger_ud(void)
{
    exc_vector = CPU_EXCEPTION_UD;
    exc_count++;
}
void trigger_nm(void)
{
    exc_vector = CPU_EXCEPTION_NM;
    exc_count++;
}
void trigger_gp(int32_t code)
{
    exc_vector = CPU_EXCEPTION_GP;
    exc_code = code;
    exc_count++;
}

static int32_t switch_seg_calls = 0;
static bool switch_seg_answer = true;
static int32_t switch_seg_last_reg = -1;
static int32_t switch_seg_last_selector = -1;

bool switch_seg(int32_t reg, int32_t selector_raw)
{
    switch_seg_calls++;
    switch_seg_last_reg = reg;
    switch_seg_last_selector = selector_raw;
    return switch_seg_answer;
}

static void reset_exceptions(void)
{
    exc_vector = -1;
    exc_code = -1;
    exc_count = 0;
}

/* ------------------------------------------------------------------ *
 * M1c's additions to the stand-ins.
 *
 * instructions_0f.c (linked here since M1b-1) gained M1c's 0F 00 /2,/3 (lldt/ltr) and
 * 0F 30/31/32 (wrmsr/rdtsc/rdmsr), whose bodies call exceptions.c's
 * lookup_segment_selector/load_ldt/load_tr and cpu.c's read_tsc/set_tsc.  This binary
 * links neither file (exceptions.c supplies the recording definitions above, and cpu.c's
 * run loop is covered by the transcript gate instead -- see the Makefile), so those five
 * symbols need recording definitions here, exactly as switch_seg and trigger_* do.
 * The REAL implementations are exercised by the M1c test binary (m1c_selftest.c), which
 * links exceptions.c/pic.c/cpu.c and adds the crafted IDT/#PF/#GP/iret cases.
 * ------------------------------------------------------------------ */
static int32_t lookup_segment_selector_calls = 0;
static seg_descriptor_t lookup_segment_selector_answer; /* raw = 0 by default */

selector_lookup_t lookup_segment_selector(seg_selector_t selector, seg_descriptor_t *desc,
                                          int32_t *descriptor_address)
{
    lookup_segment_selector_calls++;
    *desc = lookup_segment_selector_answer;
    *descriptor_address = 0;
    (void)selector;
    return SELECTOR_LOOKUP_OK;
}

static int32_t load_ldt_calls = 0;
static int32_t load_ldt_last_selector = -1;
void load_ldt(int32_t selector)
{
    load_ldt_calls++;
    load_ldt_last_selector = selector;
}

static int32_t load_tr_calls = 0;
static int32_t load_tr_last_selector = -1;
void load_tr(int32_t selector)
{
    load_tr_calls++;
    load_tr_last_selector = selector;
}

/* cpu.rs:4357-4412.  read_tsc is the real machine's only nondeterministic input, so the
 * binary records instead of computing: a test can pin the register pair rdtsc writes. */
static int32_t read_tsc_calls = 0;
static uint64_t read_tsc_answer = 0;
static int32_t set_tsc_calls = 0;
static uint32_t set_tsc_last_low = 0;
static uint32_t set_tsc_last_high = 0;
uint64_t read_tsc(void)
{
    read_tsc_calls++;
    return read_tsc_answer;
}
void set_tsc(uint32_t low, uint32_t high)
{
    set_tsc_calls++;
    set_tsc_last_low = low;
    set_tsc_last_high = high;
}

/* ------------------------------------------------------------------ *
 * M1a-3's stand-ins.
 *
 * instructions.c and string.c are the first callers of the device layer's port I/O
 * (M1c/M2), of M1c's far control transfers / iret / interrupt poll (exceptions.c, pic.c),
 * of the timer interface (the M0/PIT layer), of M1b's fwait, and of the emitter's
 * generated dispatcher (plan ss1 step 12).  None of those exists yet, so this binary
 * supplies recording definitions -- the same arrangement as call_interrupt_vector above.
 * Nothing here shadows a real definition: each owner defines its own, and this file is a
 * test binary, never linked with them.
 * ------------------------------------------------------------------ */

/* the port-I/O seat (cpu.rs:3009-3055, 4613-4649) */
static bool io_privileges_ok = true;
static int32_t io_last_port = -1;
static int32_t io_read_value = 0;
static int32_t io_read_count = 0;
static int32_t io_write_count = 0;
static int32_t io_last_write_value = 0;

int32_t io_port_read8(int32_t port) { io_last_port = port; io_read_count++; return io_read_value; }
int32_t io_port_read16(int32_t port) { io_last_port = port; io_read_count++; return io_read_value; }
int32_t io_port_read32(int32_t port) { io_last_port = port; io_read_count++; return io_read_value; }
void io_port_write8(int32_t port, int32_t value) { io_last_port = port; io_write_count++; io_last_write_value = value; }
void io_port_write16(int32_t port, int32_t value) { io_last_port = port; io_write_count++; io_last_write_value = value; }
void io_port_write32(int32_t port, int32_t value) { io_last_port = port; io_write_count++; io_last_write_value = value; }
bool test_privileges_for_io(int32_t port, int32_t size) { (void)port; (void)size; return io_privileges_ok; }

/* the timer/halt interface -- upstream's js:: imports (cpu.rs:37-41) */
static int32_t microtick_calls = 0;
static int32_t run_hardware_timers_calls = 0;
static int32_t cpu_event_halt_calls = 0;
double microtick(void) { microtick_calls++; return 1.5; }
double run_hardware_timers(bool acpi_enabled, double t) { (void)acpi_enabled; run_hardware_timers_calls++; return t; }
void cpu_event_halt(void) { cpu_event_halt_calls++; }

/* the interrupt poll (pic.rs / cpu.rs:4573, M1c) */
static int32_t handle_irqs_calls = 0;
void handle_irqs(void) { handle_irqs_calls++; }

/* APIC-1: the four mmio entry points memory.c routes to.  The real bodies live in
 * apic.c/ioapic.c, which this binary does NOT link (the same arrangement as the
 * io_port_* bridge above); these return the acpi-off answers the mmio-window cases
 * here assert (0 on a read, a discarded write). */
int32_t apic_read32(uint32_t addr) { (void)addr; return 0; }
void apic_write32(uint32_t addr, uint32_t value) { (void)addr; (void)value; }
int32_t ioapic_read32(uint32_t addr) { (void)addr; return 0; }
void ioapic_write32(uint32_t addr, uint32_t value) { (void)addr; (void)value; }

/* M1c's far control transfers and iret (cpu.rs:507-508, 1128, 1507) */
static int32_t far_jump_calls = 0;
static int32_t far_jump_last_eip = -1, far_jump_last_selector = -1;
static bool far_jump_last_is_call = false, far_jump_last_is_osize_32 = false;
static int32_t far_return_calls = 0;
static int32_t far_return_last_eip = -1, far_return_last_selector = -1, far_return_last_adjust = -1;
static bool far_return_last_is_osize_32 = false;
static int32_t iret16_calls = 0, iret32_calls = 0;

void far_jump(int32_t eip, int32_t selector, bool is_call, bool is_osize_32)
{
    far_jump_calls++;
    far_jump_last_eip = eip;
    far_jump_last_selector = selector;
    far_jump_last_is_call = is_call;
    far_jump_last_is_osize_32 = is_osize_32;
}
void far_return(int64_t eip, int32_t selector, int32_t stack_adjust, bool is_osize_32,
                bool is_64)
{
    (void)is_64;
    far_return_calls++;
    far_return_last_eip = eip;
    far_return_last_selector = selector;
    far_return_last_adjust = stack_adjust;
    far_return_last_is_osize_32 = is_osize_32;
}
void iret16(void) { iret16_calls++; }
void iret32(void) { iret32_calls++; }

/* M1b's FWAIT (fpu.rs:803) */
static int32_t fwait_calls = 0;
void fwait(void) { fwait_calls++; }

/* NOTE (M1b-1 / M1b-3, measured): 0F AE /0,/1 call fpu.c's fxsave/fxrstor through
 * instructions_0f.h's cross-unit contract.  M1b-3 made fpu.c a member of SELFTEST_SRC
 * (Makefile) while this unit was in flight, so this binary links the REAL definitions --
 * the recording stubs M1b-1 had drafted here are gone, and the 0F AE test below drives the
 * real x87 state. */

/* the generated dispatchers (the emitter unit + M1b).  The stub records the argument, so
 * the prefix instructions' re-dispatch (66/67/F0/F2/F3 and STI) is observable -- including
 * the operand-size bit 8 and the prefix byte still in effect at the moment of the call. */
/* instructions_0f.c is linked into this binary from M1b-1 on (Makefile SELFTEST_SRC), so
 * its run_instruction0f_16/32 wrappers (cpu.rs:3132-3133) are the REAL definitions here and
 * their recording stubs are gone.  The function they wrap lives in the generated dispatcher,
 * which this binary does not link, so run_instruction0f IS recorded -- the operand-size bit
 * 8 that the 32-bit wrapper ORs in is what the 0F-escape test observes. */
static int32_t dispatch_calls = 0;
static int32_t dispatch_last_opcode = -1;
static uint8_t dispatch_last_prefixes = 0;
static int32_t dispatch0f_calls = 0;
static int32_t dispatch0f_last_opcode = -1;
static bool dispatch0f_last_is_32 = false;

void run_instruction(int32_t opcode)
{
    dispatch_calls++;
    dispatch_last_opcode = opcode;
    dispatch_last_prefixes = cpu->prefixes;
}
void run_instruction0f(int32_t opcode)
{
    dispatch0f_calls++;
    dispatch0f_last_opcode = opcode & 0xFF;
    dispatch0f_last_is_32 = (opcode & 0x100) != 0;
}

/* PERF-4/M2: src/cpu/uop_cache.c is linked into this binary too (Makefile SELFTEST_SRC's
 * files call its invalidation hook, so its OBJECT is a prerequisite), and its uop_run names
 * the reference's own batch loop.  This binary does not link cpu.c (its run_instruction stub
 * above is the whole point), so that name has to exist -- and it is a RECORDING-AND-ABORT
 * stub in the same sense as run_instruction: uop_cache_enabled is left FALSE here (main()
 * never turns it on) and cycle_internal -- the only caller of uop_run -- is cpu.c's, i.e.
 * not in this binary at all, so this body is unreachable by construction.  If it ever runs,
 * the abort is the finding: it means this suite started executing the machine's run loop,
 * which it does not have. */
static int interpreted_run_calls = 0;
static uint32_t interpreted_run_last_phys = 0;

/* S0: ONE test (test_cpu_backend, below) drives the engine seam's dispatch end to end, and
 * the dispatch calls exactly what cycle_internal calls -- which for the `interp` instance IS
 * this function.  So the stub records and returns while that test runs, and stays the
 * abort-if-reached seat it has always been for everything else: this binary still has no run
 * loop, and a call from anywhere else is still the finding. */
static bool interpreted_run_allowed = false;

void interpreted_run(uint32_t phys_addr)
{
    interpreted_run_calls++;
    interpreted_run_last_phys = phys_addr;
    if (interpreted_run_allowed) return;
    fprintf(stderr, "cpu_selftest: interpreted_run called (%d) -- this binary has no run loop\n",
            interpreted_run_calls);
    abort();
}

/* The rest of the run loop's vocabulary that the same object mentions.  cpu.c -- the only
 * definer of these -- is deliberately absent from this binary (see the header note on
 * SELFTEST_SRC in the Makefile), so each is a seat here; none of them is READ by any test in
 * this file, and `get_phys_eip` is the same abort-if-reached seat as interpreted_run. */
bool jit_block_boundary = false;
bool interpreted_unimplemented = false;
/* PERF-4/M2: do_many_cycles_native's LOOP_COUNTER window end (cpu.h), written by cpu.c and read
 * only by the uop cache.  This binary has neither, but the OBJECT it links names it -- and the
 * value here is the same "unreachable window" its own default is, so even if something read it
 * the cache would not be cut short. */
uint32_t cpu_cycle_budget_end = 0xFFFFFFFFu;

uint32_t get_phys_eip(void)
{
    fprintf(stderr, "cpu_selftest: get_phys_eip called -- this binary has no fetch path\n");
    abort();
}

/* ==================================================================== *
 * S0 (results/x86-64-plan.md ss3.4, slice S0): THE ENGINE SEAM
 *
 * WHAT THIS PROVES, and what it cannot.  The seam's whole job is that
 * cycle_internal's engine half is the SELECTED backend's ->run_batch.  A
 * selftest that only compared the `name` string would pass with all three
 * instances pointing at one function, so it asserts the things that would
 * have to change for a selection to be a no-op:
 *
 *   1. the resolver: each of the three names selects an instance whose
 *      ->name is that name, and cpu_backend_current() agrees;
 *   2. the RUN_BATCH SYMBOL, by POINTER IDENTITY against the real symbols
 *      (interpreted_run, uop_run) -- not against a table of strings:
 *      interp IS interpreted_run and uopjit IS uop_run, so "the dispatcher
 *      calls uop_run" is a fact about the pointer, not a claim;
 *   3. THE ROUND TRIP, both directions: instance -> the two flags (what an
 *      explicit selection forces) -> cpu_backend_select("auto") -> the same
 *      instance.  A selection that resolved to a fixed instance fails here;
 *   4. THE DISPATCH IS CALLED: the same cpu_backend_dispatch() cycle_internal
 *      calls is invoked with the interpreter selected and the address
 *      asserted to arrive at interpreted_run.  A seat that is never called
 *      is indistinguishable from an absent seat;
 *   5. the modes capability, including that NO instance claims long mode --
 *      EFER_LME does not exist in this tree yet (plan finding 5), so an
 *      engine that grew the bit before S2 would be a finding, not a feature.
 *
 * WHAT IT CANNOT PROVE: that the uop and uopjit instances EXECUTE here.  This
 * binary has no run loop (SELFTEST_SRC's premise), so their ->run_batch is
 * identified by pointer (2) and exercised by the shipped binary's own runs --
 * `--cpu-engine interp|uop|uopjit` with --uop-cache-stats/--jit-stats move the
 * counters in exactly the three ways results/x86-64-s0.md records.  The mode
 * REFUSAL is likewise inert in S0 by construction (no engine declares a mode
 * the machine can be in): S2 owns the probe that makes it fire.
 * ==================================================================== */
static void test_cpu_backend(void)
{
    static const char *const names[3] = { "interp", "uop", "uopjit" };
    const cpu_backend_t *instance[3];

    for (int i = 0; i < 3; i++) {
        instance[i] = cpu_backend_select(names[i]);
        CHECK(instance[i] != NULL);
        if (instance[i] == NULL) return;
        CHECK(cpu_backend_current() == instance[i]);
        CHECK(strcmp(instance[i]->name, names[i]) == 0);
        CHECK_EQ(instance[i]->modes, CPU_MODES_32BIT);
        CHECK(instance[i]->run_batch != NULL);
        CHECK(instance[i]->clear_caches != NULL);
    }

    /* (2) the run_batch SYMBOL, by pointer identity */
    CHECK(instance[0]->run_batch == interpreted_run);   /* `interp` IS the interpreter */
    CHECK(instance[2]->run_batch == uop_run);           /* `uopjit` IS uop_run, compiled path on */
    CHECK(instance[1]->run_batch != uop_run);           /* `uop` holds the compiled path down */
    CHECK(instance[1]->run_batch != interpreted_run);
    CHECK(instance[0]->run_batch != instance[1]->run_batch);
    CHECK(instance[1]->run_batch != instance[2]->run_batch);
    CHECK(instance[0]->run_batch != instance[2]->run_batch);

    /* the cascade the refusal path walks (ss3.4) */
    CHECK(cpu_backend_fallback(instance[2]) == instance[1]);
    CHECK(cpu_backend_fallback(instance[1]) == instance[0]);
    CHECK(cpu_backend_fallback(instance[0]) == NULL);

    /* (3) THE ROUND TRIP: instance -> the two flags -> `auto` -> the same instance */
    for (int i = 0; i < 3; i++) {
        cpu_backend_adopt(instance[i]);
        CHECK_EQ(uop_cache_enabled, i != 0);
        CHECK_EQ(jit_enabled, i == 2);
        CHECK(cpu_backend_current() == instance[i]);
        CHECK(cpu_backend_select("auto") == instance[i]);
    }
    /* an unknown name is refused by name (the resolver's NULL), and it does not half-select */
    {
        const cpu_backend_t *before = cpu_backend_current();
        CHECK(cpu_backend_select("kvm") == NULL);
        CHECK(cpu_backend_current() == before);
    }

    /* (5) the capability word: the three 32-bit modes, and NO long mode anywhere */
    cpu_backend_select("uopjit");
    CHECK(cpu_backend_can(CPU_MODE_REAL));
    CHECK(cpu_backend_can(CPU_MODE_PROT16));
    CHECK(cpu_backend_can(CPU_MODE_PROT32));
    CHECK(cpu_backend_can(CPU_MODES_32BIT));
    CHECK(!cpu_backend_can(CPU_MODE_LONG));
    CHECK(!cpu_backend_can(CPU_MODE_COMPAT));
    if (cpu->cr[0] & CR0_PE) CHECK(cpu_mode_of(cpu) == (cpu->is_32 ? CPU_MODE_PROT32
                                                                  : CPU_MODE_PROT16));
    else CHECK(cpu_mode_of(cpu) == CPU_MODE_REAL);
    CHECK(cpu_backend_can(cpu_mode_of(cpu)));

    /* (4) THE DISPATCH, CALLED -- the function cycle_internal calls, with the address
     * arriving at the interpreter.  This is the end-to-end half of the seam. */
    CHECK(cpu_backend_select("interp") == instance[0]);
    {
        int calls_before = interpreted_run_calls;
        interpreted_run_allowed = true;
        cpu_backend_dispatch(0x00012345u);
        interpreted_run_allowed = false;
        CHECK_EQ(interpreted_run_calls, calls_before + 1);
        CHECK_EQ(interpreted_run_last_phys, 0x00012345u);
    }

    /* the enumeration the loop above did not use */
    for (uint32_t i = 0; i < 3; i++) CHECK(cpu_backend_by_index(i) == instance[i]);
    CHECK(cpu_backend_by_index(3) == NULL);

    /* leave the binary as it found it: the shipped default, and no engine named */
    cpu_backend_select("auto");
    CHECK(cpu_backend_current() != NULL);
}

/* ==================================================================== *
 * S2 (results/x86-64-plan.md ss3.5, slice S2): THE MODE PREDICATE AND THE REFUSALS
 *
 * This is the slice that closes the plan's C5 -- a LIVE defect: EFER_LME was defined
 * nowhere, so a guest that wrote EFER.LME and then set CR0.PG with CR4.PAE had its PML4
 * read as if it were a PDPT and a 3-level walk run over 4-level tables, with nothing in
 * the machine naming which.  Four claims, each able to fail:
 *
 *   1. the mode predicate has its IA-32e arm (Appendix A), and EFER.LMA is what selects it;
 *   2. S0's dispatcher seat now FIRES: with the machine in long mode and a 32-bit-only
 *      engine selected, cpu_backend_dispatch() stops BY NAME and does NOT run a batch (S0
 *      could not test this -- no engine declares a mode the machine could be in);
 *   3. all three writes that can ENTER IA-32e refuse by name and COMMIT NOTHING -- WRMSR of
 *      EFER carrying LME, CR0 setting PG, and CR4 setting PAE -- and the CR0 case is proved
 *      to have stopped BEFORE the page-table read (the PDPTE cache is untouched, which a
 *      validation-first load_pdpte would have refreshed);
 *   4. CPUID's long-mode declaration is COUPLED to the capability word, bit for bit --
 *      asserted as an invariant, because no engine declares long mode yet, so the arm that
 *      returns the bits cannot be reached in this build (named as a gap in the report).
 * ==================================================================== */
static void test_cpu_mode_and_refusals(void)
{
    /* (1) THE MODE PREDICATE, arm by arm (plan Appendix A) */
    cpu_state_init();
    cpu->cr[0] = CR0_CD | CR0_NW | CR0_ET; /* PE clear */
    CHECK_EQ(cpu_mode_of(cpu), CPU_MODE_REAL);
    cpu->cr[0] = CR0_PE;
    cpu->is_32 = false;
    CHECK_EQ(cpu_mode_of(cpu), CPU_MODE_PROT16);
    cpu->is_32 = true;
    CHECK_EQ(cpu_mode_of(cpu), CPU_MODE_PROT32);
    /* S5a CLOSED THE SPLIT THIS ARM USED TO COLLAPSE.  Until CS.L was decoded, both IA-32e
     * modes answered CPU_MODE_LONG and the predicate could not tell them apart (the S2 report
     * named that as the arm that could not exist).  It exists now, and it is asserted in BOTH
     * directions -- because the distinction is what makes the corpus's entry path decodable:
     * the machine is in COMPATIBILITY mode from the CR0.PG edge until the far jump loads a
     * 64-bit CS (plan correction GAP 2). */
    cpu->efer = EFER_LMA;
    cpu->is_64 = false;
    CHECK_EQ(cpu_mode_of(cpu), CPU_MODE_COMPAT);
    cpu->is_64 = true;
    CHECK_EQ(cpu_mode_of(cpu), CPU_MODE_LONG);
    CHECK_EQ(cpu_mode_of(cpu) & CPU_MODES_32BIT, 0);
    cpu->is_64 = false;

    /* (2) S0's DISPATCH SEAT FIRES.  The mode refusal could not fire before this slice
     * (no engine declares a mode the machine could be in), so it was an instrument that
     * could not fail; the machine is put in long mode directly, which is exactly the state
     * S2 makes unreachable through the guest's own writes. */
    CHECK(cpu_backend_select("interp") == cpu_backend_by_index(0));
    {
        int run_before = interpreted_run_calls;
        reset_unimplemented();
        unimplemented_allowed = true;
        cpu->cr[0] = CR0_PE | CR0_PG;
        cpu->is_32 = true;
        cpu->efer = EFER_LMA;
        cpu->is_64 = true; /* the 64-bit arm: the refusal must name long mode */
        cpu_backend_dispatch(0x00012345u);
        unimplemented_allowed = false;
        CHECK_EQ(interpreted_run_calls, run_before); /* run_batch did NOT run */
        CHECK_EQ(unimplemented_calls, 1);
        CHECK(interpreted_unimplemented);
        CHECK(strstr(unimplemented_last, "cannot run long mode") != NULL);
        CHECK(strstr(unimplemented_last, "'interp'") != NULL);
        reset_unimplemented();

        /* S5a: and the COMPATIBILITY arm, which the predicate can name now -- a backend that
         * cannot run IA-32e is refused in either mode, and the message says WHICH (a refusal
         * that only knew "long mode" would misname the mode the machine was actually in). */
        cpu->is_64 = false;
        unimplemented_allowed = true;
        cpu_backend_dispatch(0x00012345u);
        unimplemented_allowed = false;
        CHECK_EQ(interpreted_run_calls, run_before);
        CHECK_EQ(unimplemented_calls, 1);
        CHECK(strstr(unimplemented_last, "cannot run compatibility mode") != NULL);
        reset_unimplemented();
    }

    /* (3a) WRMSR of EFER carrying LME: refused by name, EFER NOT written */
    cpu_state_init();
    cpu->cpl = 0;
    cpu->efer = EFER_NXE; /* a state the guest legitimately reaches first */
    write_reg32(ECX, (int32_t)MSR_EFER);
    write_reg32(EAX, (int32_t)(EFER_SCE | EFER_LME | EFER_NXE));
    write_reg32(EDX, 0);
    reset_unimplemented();
    unimplemented_allowed = true;
    instr_0F30();
    unimplemented_allowed = false;
    CHECK_EQ(unimplemented_calls, 1);
    CHECK(strstr(unimplemented_last, "EFER.LME") != NULL);
    CHECK_EQ(cpu->efer, EFER_NXE); /* committed nothing, not even the SCE bit beside LME */
    CHECK_EQ(cpu->pagefault, 0);   /* a refusal, not a fault */
    reset_unimplemented();

    /* ... and every OTHER EFER bit is still stored unchecked (the R5F regression, pinned in
     * m1c_selftest too): a write without LME goes through, reserved bits and all */
    write_reg32(EAX, (int32_t)(EFER_SCE | EFER_NXE));
    write_reg32(EDX, 0);
    unimplemented_allowed = true;
    instr_0F30();
    unimplemented_allowed = false;
    CHECK_EQ(cpu->efer, (uint64_t)(EFER_SCE | EFER_NXE));
    CHECK_EQ(unimplemented_calls, 0); /* no stop: nothing asked for IA-32e */

    /* ... and a write that CLEARS LME is always allowed, even from a state that has LME
     * (a checkpoint restored from a machine that could run it): the refusal is about
     * ENTERING long mode, never about leaving it. */
    cpu->efer = EFER_LME;
    write_reg32(EAX, 0);
    write_reg32(EDX, 0);
    instr_0F30();
    CHECK_EQ(cpu->efer, 0);
    CHECK_EQ(unimplemented_calls, 0);

    /* (3b) CR0.PG with EFER.LME and CR4.PAE: refused by name, BEFORE the walk, nothing
     * committed.  The PDPTE cache is seeded with a sentinel; a validation-first load_pdpte
     * (the path C5's guest took) would replace it, so "unchanged" is the assertion that the
     * refusal came first -- and the PDPT at 0x5000 is VALID, so the load would have
     * succeeded had it been reached. */
    cpu_state_init();
    cpu->cr[0] = CR0_PE;
    cpu->cr[4] = CR4_PAE;
    cpu->efer = EFER_LME;
    cpu->cr[3] = 0x5000;
    for (int i = 0; i < 4; i++) {
        write32(0x5000 + 4 * i, 0);
        write32(0x5000 + 4 * i + 4, 0);
        cpu->reg_pdpte[i] = 0xDEADBEEF00000000ull | (uint64_t)i;
    }
    reset_unimplemented();
    unimplemented_allowed = true;
    set_cr0(CR0_PE | CR0_PG);
    unimplemented_allowed = false;
    CHECK_EQ(unimplemented_calls, 1);
    CHECK(strstr(unimplemented_last, "CR0.PG") != NULL);
    CHECK_EQ(cpu->cr[0], CR0_PE); /* PG not set */
    CHECK(!cpu->protected_mode);  /* ... and the refused write never reached the derived flag */
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(cpu->reg_pdpte[i], 0xDEADBEEF00000000ull | (uint64_t)i); /* no walk to 4 levels */
    }
    reset_unimplemented();

    /* ... and the same CR0 write is ACCEPTED once the machine has no LME (the 32-bit PAE
     * machine the port ships): the refusal is the transition, not paging */
    cpu->efer = 0;
    set_cr0(CR0_PE | CR0_PG);
    CHECK_EQ(cpu->cr[0], CR0_PE | CR0_PG | CR0_ET);
    CHECK_EQ(unimplemented_calls, 0);

    /* (3c) CR4.PAE with PG already set and LME set -- the SDM's OTHER entry trigger, and
     * the third load_pdpte() door C5 named two of */
    cpu_state_init();
    cpu->cr[0] = CR0_PE | CR0_PG;
    cpu->cr[4] = 0;
    cpu->efer = EFER_LME;
    cpu->cpl = 0;
    for (int i = 0; i < 4; i++) cpu->reg_pdpte[i] = 0xDEADBEEF00000000ull | (uint64_t)i;
    write_reg32(EBX, CR4_PAE);
    reset_unimplemented();
    unimplemented_allowed = true;
    instr_0F22(EBX, 4); /* mov cr4, ebx */
    unimplemented_allowed = false;
    CHECK_EQ(unimplemented_calls, 1);
    CHECK(strstr(unimplemented_last, "CR4.PAE") != NULL);
    CHECK_EQ(cpu->cr[4], 0); /* not committed */
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(cpu->reg_pdpte[i], 0xDEADBEEF00000000ull | (uint64_t)i);
    }
    reset_unimplemented();

    /* (4) THE CPUID DECLARATION IS COUPLED TO THE CAPABILITY -- an invariant, because no
     * engine in this build declares long mode (S5 flips it), so the arm that RETURNS the
     * bits cannot be reached here; what can be asserted is that the two can never disagree,
     * which is what a false declaration would be. */
    cpu_backend_select("uopjit");
    {
        bool can_long = cpu_backend_can(CPU_MODE_LONG);

        write_reg32(EAX, (int32_t)0x80000001u);
        instr_0FA2();
        int32_t edx = read_reg32(EDX);
        CHECK_EQ(((uint32_t)edx >> 29) & 1, can_long ? 1 : 0); /* lm */
        CHECK_EQ(((uint32_t)edx >> 11) & 1, can_long ? 1 : 0); /* syscall */
        /* MEASURED, and the pinned reference's own value: no lm, no syscall today */
        if (!can_long) CHECK_EQ((uint32_t)edx, 0x0190A179u);

        write_reg32(EAX, (int32_t)0x80000008u);
        instr_0FA2();
        int32_t eax = read_reg32(EAX);
        CHECK_EQ((uint32_t)eax >> 8, can_long ? 48u : 32u); /* the LINEAR width */
        CHECK_EQ((uint32_t)eax & 0xFF, 32u);                /* the port's physical bound */
        if (!can_long) CHECK_EQ((uint32_t)eax, 0x2020u);
    }

    /* (5) the EFER bit POSITIONS are the SDM's (Vol 4, IA32_EFER) -- a guest's kernel reads
     * and writes them by name, and a transposed bit would be a silent mode mismatch */
    CHECK_EQ(EFER_SCE, 0x1);
    CHECK_EQ(EFER_LME, 0x100);
    CHECK_EQ(EFER_LMA, 0x400);
    CHECK_EQ(EFER_NXE, 0x800);
    CHECK_EQ(MSR_EFER, 0xC0000080u);

    /* leave the machine as the other tests expect it */
    cpu_state_init();
    cpu_backend_select("auto");
    reset_unimplemented();
    printf("PASS S2: the mode predicate's IA-32e arm, the dispatcher's mode refusal FIRING,"
           " the three IA-32e entry writes refused by name (EFER.LME, CR0.PG, CR4.PAE) with"
           " nothing committed and no page-table read, and CPUID's declaration coupled to the"
           " capability word\n");
}

/* AUDIT-1 S1: modrm.c's read_imm8 now names the run loop's stop for a fetch past guest
 * RAM; cpu.c is deliberately absent from this binary, so the stop gets the same
 * abort-if-reached seat as get_phys_eip's above -- no test here drives a fetch. */
/* S2: THE MACHINE'S NAMED STOP CHANNEL, RECORDED.  The long-mode refusals and the
 * dispatcher's mode refusal stop the machine through this function, so the suite has to be
 * able to SEE a stop instead of aborting on one -- the same arrangement interpreted_run got
 * in S0 (interpreted_run_allowed).  `unimplemented_allowed` is set only by the tests that
 * expect a stop; from anywhere else the abort is still the finding.  The real cpu.c sets
 * `interpreted_unimplemented`, which is the flag both run loops check, so this seat sets it
 * too and the tests can assert against the same predicate the machine uses. */
void unimplemented_instruction(const char *name)
{
    unimplemented_calls++;
    snprintf(unimplemented_last, sizeof(unimplemented_last), "%s", name);
    interpreted_unimplemented = true;
    if (unimplemented_allowed) return;
    fprintf(stderr, "cpu_selftest: unimplemented_instruction(%s) called -- this binary has"
                    " no run loop\n", name);
    abort();
}

static void reset_unimplemented(void)
{
    unimplemented_calls = 0;
    unimplemented_last[0] = 0;
    interpreted_unimplemented = false;
}

static void reset_stubs(void)
{
    reset_exceptions();
    reset_delivery();
    switch_seg_calls = 0;
    switch_seg_answer = true;
    lookup_segment_selector_calls = 0;
    lookup_segment_selector_answer.raw = 0;
    load_ldt_calls = 0;
    load_ldt_last_selector = -1;
    load_tr_calls = 0;
    load_tr_last_selector = -1;
    read_tsc_calls = 0;
    read_tsc_answer = 0;
    set_tsc_calls = 0;
    set_tsc_last_low = 0;
    set_tsc_last_high = 0;
    io_privileges_ok = true;
    io_last_port = -1;
    io_read_value = 0;
    io_read_count = 0;
    io_write_count = 0;
    io_last_write_value = 0;
    microtick_calls = 0;
    run_hardware_timers_calls = 0;
    cpu_event_halt_calls = 0;
    handle_irqs_calls = 0;
    far_jump_calls = 0;
    far_return_calls = 0;
    iret16_calls = 0;
    iret32_calls = 0;
    fwait_calls = 0;
    dispatch_calls = 0;
    dispatch_last_opcode = -1;
    dispatch_last_prefixes = 0;
    dispatch0f_calls = 0;
    dispatch0f_last_opcode = -1;
}

/* ------------------------------------------------------------------ *
 * gate 4a -- the shift/rotate family (arith.rs:440-996)
 * ------------------------------------------------------------------ */

enum { OP_rol, OP_ror, OP_rcl, OP_rcr, OP_shl, OP_shr, OP_sar, OP_shld, OP_shrd };

typedef struct {
    int op;
    int size;
    uint32_t v;
    uint32_t src;
    int count;
    int cf_in;
    uint32_t exp_res;
    int exp_cf;
    int exp_of; /* -1: undefined by the SDM for this count */
} shift_case;

static const shift_case shift_cases[] = {
    {OP_rol, 8, 0x01, 0,  1, 0, 0x02, 0, 0},
    {OP_rol, 8, 0x80, 0,  1, 0, 0x01, 1, 1},
    {OP_rol, 8, 0xFF, 0,  1, 0, 0xFF, 1, 0},
    {OP_rol, 8, 0x01, 0,  7, 0, 0x80, 0, -1},
    {OP_rol, 8, 0x80, 0,  7, 0, 0x40, 0, -1},
    {OP_rol, 8, 0xFF, 0,  7, 0, 0xFF, 1, -1},
    {OP_rol, 16, 0x0001, 0,  1, 0, 0x0002, 0, 0},
    {OP_rol, 16, 0x8000, 0,  1, 0, 0x0001, 1, 1},
    {OP_rol, 16, 0xFFFF, 0,  1, 0, 0xFFFF, 1, 0},
    {OP_rol, 16, 0x0001, 0, 15, 0, 0x8000, 0, -1},
    {OP_rol, 16, 0x8000, 0, 15, 0, 0x4000, 0, -1},
    {OP_rol, 16, 0xFFFF, 0, 15, 0, 0xFFFF, 1, -1},
    {OP_rol, 32, 0x00000001, 0,  1, 0, 0x00000002, 0, 0},
    {OP_rol, 32, 0x80000000, 0,  1, 0, 0x00000001, 1, 1},
    {OP_rol, 32, 0xFFFFFFFF, 0,  1, 0, 0xFFFFFFFF, 1, 0},
    {OP_rol, 32, 0x00000001, 0, 31, 0, 0x80000000, 0, -1},
    {OP_rol, 32, 0x80000000, 0, 31, 0, 0x40000000, 0, -1},
    {OP_rol, 32, 0xFFFFFFFF, 0, 31, 0, 0xFFFFFFFF, 1, -1},
    {OP_ror, 8, 0x01, 0,  1, 0, 0x80, 1, 1},
    {OP_ror, 8, 0x80, 0,  1, 0, 0x40, 0, 1},
    {OP_ror, 8, 0xFF, 0,  1, 0, 0xFF, 1, 0},
    {OP_ror, 8, 0x01, 0,  7, 0, 0x02, 0, -1},
    {OP_ror, 8, 0x80, 0,  7, 0, 0x01, 0, -1},
    {OP_ror, 8, 0xFF, 0,  7, 0, 0xFF, 1, -1},
    {OP_ror, 16, 0x0001, 0,  1, 0, 0x8000, 1, 1},
    {OP_ror, 16, 0x8000, 0,  1, 0, 0x4000, 0, 1},
    {OP_ror, 16, 0xFFFF, 0,  1, 0, 0xFFFF, 1, 0},
    {OP_ror, 16, 0x0001, 0, 15, 0, 0x0002, 0, -1},
    {OP_ror, 16, 0x8000, 0, 15, 0, 0x0001, 0, -1},
    {OP_ror, 16, 0xFFFF, 0, 15, 0, 0xFFFF, 1, -1},
    {OP_ror, 32, 0x00000001, 0,  1, 0, 0x80000000, 1, 1},
    {OP_ror, 32, 0x80000000, 0,  1, 0, 0x40000000, 0, 1},
    {OP_ror, 32, 0xFFFFFFFF, 0,  1, 0, 0xFFFFFFFF, 1, 0},
    {OP_ror, 32, 0x00000001, 0, 31, 0, 0x00000002, 0, -1},
    {OP_ror, 32, 0x80000000, 0, 31, 0, 0x00000001, 0, -1},
    {OP_ror, 32, 0xFFFFFFFF, 0, 31, 0, 0xFFFFFFFF, 1, -1},
    {OP_rcl, 8, 0x01, 0,  1, 0, 0x02, 0, 0},
    {OP_rcl, 8, 0x01, 0,  1, 1, 0x03, 0, 0},
    {OP_rcl, 8, 0x80, 0,  1, 0, 0x00, 1, 1},
    {OP_rcl, 8, 0x80, 0,  1, 1, 0x01, 1, 1},
    {OP_rcl, 8, 0xFF, 0,  1, 0, 0xFE, 1, 0},
    {OP_rcl, 8, 0xFF, 0,  1, 1, 0xFF, 1, 0},
    {OP_rcl, 8, 0x01, 0,  7, 0, 0x80, 0, -1},
    {OP_rcl, 8, 0x01, 0,  7, 1, 0xC0, 0, -1},
    {OP_rcl, 8, 0x80, 0,  7, 0, 0x20, 0, -1},
    {OP_rcl, 8, 0x80, 0,  7, 1, 0x60, 0, -1},
    {OP_rcl, 8, 0xFF, 0,  7, 0, 0xBF, 1, -1},
    {OP_rcl, 8, 0xFF, 0,  7, 1, 0xFF, 1, -1},
    {OP_rcl, 16, 0x0001, 0,  1, 0, 0x0002, 0, 0},
    {OP_rcl, 16, 0x0001, 0,  1, 1, 0x0003, 0, 0},
    {OP_rcl, 16, 0x8000, 0,  1, 0, 0x0000, 1, 1},
    {OP_rcl, 16, 0x8000, 0,  1, 1, 0x0001, 1, 1},
    {OP_rcl, 16, 0xFFFF, 0,  1, 0, 0xFFFE, 1, 0},
    {OP_rcl, 16, 0xFFFF, 0,  1, 1, 0xFFFF, 1, 0},
    {OP_rcl, 16, 0x0001, 0, 15, 0, 0x8000, 0, -1},
    {OP_rcl, 16, 0x0001, 0, 15, 1, 0xC000, 0, -1},
    {OP_rcl, 16, 0x8000, 0, 15, 0, 0x2000, 0, -1},
    {OP_rcl, 16, 0x8000, 0, 15, 1, 0x6000, 0, -1},
    {OP_rcl, 16, 0xFFFF, 0, 15, 0, 0xBFFF, 1, -1},
    {OP_rcl, 16, 0xFFFF, 0, 15, 1, 0xFFFF, 1, -1},
    {OP_rcl, 32, 0x00000001, 0,  1, 0, 0x00000002, 0, 0},
    {OP_rcl, 32, 0x00000001, 0,  1, 1, 0x00000003, 0, 0},
    {OP_rcl, 32, 0x80000000, 0,  1, 0, 0x00000000, 1, 1},
    {OP_rcl, 32, 0x80000000, 0,  1, 1, 0x00000001, 1, 1},
    {OP_rcl, 32, 0xFFFFFFFF, 0,  1, 0, 0xFFFFFFFE, 1, 0},
    {OP_rcl, 32, 0xFFFFFFFF, 0,  1, 1, 0xFFFFFFFF, 1, 0},
    {OP_rcl, 32, 0x00000001, 0, 31, 0, 0x80000000, 0, -1},
    {OP_rcl, 32, 0x00000001, 0, 31, 1, 0xC0000000, 0, -1},
    {OP_rcl, 32, 0x80000000, 0, 31, 0, 0x20000000, 0, -1},
    {OP_rcl, 32, 0x80000000, 0, 31, 1, 0x60000000, 0, -1},
    {OP_rcl, 32, 0xFFFFFFFF, 0, 31, 0, 0xBFFFFFFF, 1, -1},
    {OP_rcl, 32, 0xFFFFFFFF, 0, 31, 1, 0xFFFFFFFF, 1, -1},
    {OP_rcr, 8, 0x01, 0,  1, 0, 0x00, 1, 0},
    {OP_rcr, 8, 0x01, 0,  1, 1, 0x80, 1, 1},
    {OP_rcr, 8, 0x80, 0,  1, 0, 0x40, 0, 1},
    {OP_rcr, 8, 0x80, 0,  1, 1, 0xC0, 0, 0},
    {OP_rcr, 8, 0xFF, 0,  1, 0, 0x7F, 1, 1},
    {OP_rcr, 8, 0xFF, 0,  1, 1, 0xFF, 1, 0},
    {OP_rcr, 8, 0x01, 0,  7, 0, 0x04, 0, -1},
    {OP_rcr, 8, 0x01, 0,  7, 1, 0x06, 0, -1},
    {OP_rcr, 8, 0x80, 0,  7, 0, 0x01, 0, -1},
    {OP_rcr, 8, 0x80, 0,  7, 1, 0x03, 0, -1},
    {OP_rcr, 8, 0xFF, 0,  7, 0, 0xFD, 1, -1},
    {OP_rcr, 8, 0xFF, 0,  7, 1, 0xFF, 1, -1},
    {OP_rcr, 16, 0x0001, 0,  1, 0, 0x0000, 1, 0},
    {OP_rcr, 16, 0x0001, 0,  1, 1, 0x8000, 1, 1},
    {OP_rcr, 16, 0x8000, 0,  1, 0, 0x4000, 0, 1},
    {OP_rcr, 16, 0x8000, 0,  1, 1, 0xC000, 0, 0},
    {OP_rcr, 16, 0xFFFF, 0,  1, 0, 0x7FFF, 1, 1},
    {OP_rcr, 16, 0xFFFF, 0,  1, 1, 0xFFFF, 1, 0},
    {OP_rcr, 16, 0x0001, 0, 15, 0, 0x0004, 0, -1},
    {OP_rcr, 16, 0x0001, 0, 15, 1, 0x0006, 0, -1},
    {OP_rcr, 16, 0x8000, 0, 15, 0, 0x0001, 0, -1},
    {OP_rcr, 16, 0x8000, 0, 15, 1, 0x0003, 0, -1},
    {OP_rcr, 16, 0xFFFF, 0, 15, 0, 0xFFFD, 1, -1},
    {OP_rcr, 16, 0xFFFF, 0, 15, 1, 0xFFFF, 1, -1},
    {OP_rcr, 32, 0x00000001, 0,  1, 0, 0x00000000, 1, 0},
    {OP_rcr, 32, 0x00000001, 0,  1, 1, 0x80000000, 1, 1},
    {OP_rcr, 32, 0x80000000, 0,  1, 0, 0x40000000, 0, 1},
    {OP_rcr, 32, 0x80000000, 0,  1, 1, 0xC0000000, 0, 0},
    {OP_rcr, 32, 0xFFFFFFFF, 0,  1, 0, 0x7FFFFFFF, 1, 1},
    {OP_rcr, 32, 0xFFFFFFFF, 0,  1, 1, 0xFFFFFFFF, 1, 0},
    {OP_rcr, 32, 0x00000001, 0, 31, 0, 0x00000004, 0, -1},
    {OP_rcr, 32, 0x00000001, 0, 31, 1, 0x00000006, 0, -1},
    {OP_rcr, 32, 0x80000000, 0, 31, 0, 0x00000001, 0, -1},
    {OP_rcr, 32, 0x80000000, 0, 31, 1, 0x00000003, 0, -1},
    {OP_rcr, 32, 0xFFFFFFFF, 0, 31, 0, 0xFFFFFFFD, 1, -1},
    {OP_rcr, 32, 0xFFFFFFFF, 0, 31, 1, 0xFFFFFFFF, 1, -1},
    {OP_shl, 8, 0x01, 0,  1, 0, 0x02, 0, 0},
    {OP_shl, 8, 0x80, 0,  1, 0, 0x00, 1, 1},
    {OP_shl, 8, 0xFF, 0,  1, 0, 0xFE, 1, 0},
    {OP_shl, 8, 0x01, 0,  7, 0, 0x80, 0, -1},
    {OP_shl, 8, 0x80, 0,  7, 0, 0x00, 0, -1},
    {OP_shl, 8, 0xFF, 0,  7, 0, 0x80, 1, -1},
    {OP_shl, 16, 0x0001, 0,  1, 0, 0x0002, 0, 0},
    {OP_shl, 16, 0x8000, 0,  1, 0, 0x0000, 1, 1},
    {OP_shl, 16, 0xFFFF, 0,  1, 0, 0xFFFE, 1, 0},
    {OP_shl, 16, 0x0001, 0, 15, 0, 0x8000, 0, -1},
    {OP_shl, 16, 0x8000, 0, 15, 0, 0x0000, 0, -1},
    {OP_shl, 16, 0xFFFF, 0, 15, 0, 0x8000, 1, -1},
    {OP_shl, 32, 0x00000001, 0,  1, 0, 0x00000002, 0, 0},
    {OP_shl, 32, 0x80000000, 0,  1, 0, 0x00000000, 1, 1},
    {OP_shl, 32, 0xFFFFFFFF, 0,  1, 0, 0xFFFFFFFE, 1, 0},
    {OP_shl, 32, 0x00000001, 0, 31, 0, 0x80000000, 0, -1},
    {OP_shl, 32, 0x80000000, 0, 31, 0, 0x00000000, 0, -1},
    {OP_shl, 32, 0xFFFFFFFF, 0, 31, 0, 0x80000000, 1, -1},
    {OP_shr, 8, 0x01, 0,  1, 0, 0x00, 1, 0},
    {OP_shr, 8, 0x80, 0,  1, 0, 0x40, 0, 1},
    {OP_shr, 8, 0xFF, 0,  1, 0, 0x7F, 1, 1},
    {OP_shr, 8, 0x01, 0,  7, 0, 0x00, 0, -1},
    {OP_shr, 8, 0x80, 0,  7, 0, 0x01, 0, -1},
    {OP_shr, 8, 0xFF, 0,  7, 0, 0x01, 1, -1},
    {OP_shr, 16, 0x0001, 0,  1, 0, 0x0000, 1, 0},
    {OP_shr, 16, 0x8000, 0,  1, 0, 0x4000, 0, 1},
    {OP_shr, 16, 0xFFFF, 0,  1, 0, 0x7FFF, 1, 1},
    {OP_shr, 16, 0x0001, 0, 15, 0, 0x0000, 0, -1},
    {OP_shr, 16, 0x8000, 0, 15, 0, 0x0001, 0, -1},
    {OP_shr, 16, 0xFFFF, 0, 15, 0, 0x0001, 1, -1},
    {OP_shr, 32, 0x00000001, 0,  1, 0, 0x00000000, 1, 0},
    {OP_shr, 32, 0x80000000, 0,  1, 0, 0x40000000, 0, 1},
    {OP_shr, 32, 0xFFFFFFFF, 0,  1, 0, 0x7FFFFFFF, 1, 1},
    {OP_shr, 32, 0x00000001, 0, 31, 0, 0x00000000, 0, -1},
    {OP_shr, 32, 0x80000000, 0, 31, 0, 0x00000001, 0, -1},
    {OP_shr, 32, 0xFFFFFFFF, 0, 31, 0, 0x00000001, 1, -1},
    {OP_sar, 8, 0x01, 0,  1, 0, 0x00, 1, 0},
    {OP_sar, 8, 0x80, 0,  1, 0, 0xC0, 0, 0},
    {OP_sar, 8, 0xFF, 0,  1, 0, 0xFF, 1, 0},
    {OP_sar, 8, 0x01, 0,  7, 0, 0x00, 0, -1},
    {OP_sar, 8, 0x80, 0,  7, 0, 0xFF, 0, -1},
    {OP_sar, 8, 0xFF, 0,  7, 0, 0xFF, 1, -1},
    {OP_sar, 16, 0x0001, 0,  1, 0, 0x0000, 1, 0},
    {OP_sar, 16, 0x8000, 0,  1, 0, 0xC000, 0, 0},
    {OP_sar, 16, 0xFFFF, 0,  1, 0, 0xFFFF, 1, 0},
    {OP_sar, 16, 0x0001, 0, 15, 0, 0x0000, 0, -1},
    {OP_sar, 16, 0x8000, 0, 15, 0, 0xFFFF, 0, -1},
    {OP_sar, 16, 0xFFFF, 0, 15, 0, 0xFFFF, 1, -1},
    {OP_sar, 32, 0x00000001, 0,  1, 0, 0x00000000, 1, 0},
    {OP_sar, 32, 0x80000000, 0,  1, 0, 0xC0000000, 0, 0},
    {OP_sar, 32, 0xFFFFFFFF, 0,  1, 0, 0xFFFFFFFF, 1, 0},
    {OP_sar, 32, 0x00000001, 0, 31, 0, 0x00000000, 0, -1},
    {OP_sar, 32, 0x80000000, 0, 31, 0, 0xFFFFFFFF, 0, -1},
    {OP_sar, 32, 0xFFFFFFFF, 0, 31, 0, 0xFFFFFFFF, 1, -1},
    {OP_shld, 16, 0x1234, 0x8000,  1, 0, 0x2469, 0, 0},
    {OP_shld, 16, 0x1234, 0x8000, 15, 0, 0x4000, 0, -1},
    {OP_shld, 32, 0x12345678, 0x80000000,  1, 0, 0x2468ACF1, 0, 0},
    {OP_shld, 32, 0x12345678, 0x80000000, 31, 0, 0x40000000, 0, -1},
    {OP_shrd, 16, 0x1234, 0x8000,  1, 0, 0x091A, 0, 0},
    {OP_shrd, 16, 0x1234, 0x8000, 15, 0, 0x0000, 0, -1},
    {OP_shrd, 32, 0x12345678, 0x80000000,  1, 0, 0x091A2B3C, 0, 0},
    {OP_shrd, 32, 0x12345678, 0x80000000, 31, 0, 0x00000000, 0, -1},
};

static int32_t shift_apply(const shift_case *c)
{
    switch (c->op) {
    case OP_rol:
        return c->size == 8 ? rol8(c->v, c->count)
                            : c->size == 16 ? rol16(c->v, c->count) : rol32(c->v, c->count);
    case OP_ror:
        return c->size == 8 ? ror8(c->v, c->count)
                            : c->size == 16 ? ror16(c->v, c->count) : ror32(c->v, c->count);
    case OP_rcl:
        return c->size == 8 ? rcl8(c->v, c->count)
                            : c->size == 16 ? rcl16(c->v, c->count) : rcl32(c->v, c->count);
    case OP_rcr:
        return c->size == 8 ? rcr8(c->v, c->count)
                            : c->size == 16 ? rcr16(c->v, c->count) : rcr32(c->v, c->count);
    case OP_shl:
        return c->size == 8 ? shl8(c->v, c->count)
                            : c->size == 16 ? shl16(c->v, c->count) : shl32(c->v, c->count);
    case OP_shr:
        return c->size == 8 ? shr8(c->v, c->count)
                            : c->size == 16 ? shr16(c->v, c->count) : shr32(c->v, c->count);
    case OP_sar:
        return c->size == 8 ? sar8(c->v, c->count)
                            : c->size == 16 ? sar16(c->v, c->count) : sar32(c->v, c->count);
    case OP_shld:
        return c->size == 16 ? shld16(c->v, c->src, c->count) : shld32(c->v, c->src, c->count);
    default:
        return c->size == 16 ? shrd16(c->v, c->src, c->count) : shrd32(c->v, c->src, c->count);
    }
}

/* even parity of the low byte (the SDM's PF rule for an arithmetic result) */
static int parity_even(uint32_t v)
{
    int n = 0;
    for (int i = 0; i < 8; i++) {
        n += (int)(v >> i) & 1;
    }
    return (n % 2) == 0;
}

static void test_arith_shift(void)
{
    size_t n = sizeof(shift_cases) / sizeof(shift_cases[0]);
    int bad = 0;

    for (size_t i = 0; i < n; i++) {
        const shift_case *c = &shift_cases[i];
        uint32_t mask = c->size == 8 ? 0xFFu : c->size == 16 ? 0xFFFFu : 0xFFFFFFFFu;

        cpu_state_init();
        cpu->flags = FLAGS_DEFAULT | (c->cf_in ? FLAG_CARRY : 0);
        cpu->flags_changed = 0; /* nothing stale: getcf() must read the carry-in */

        uint32_t got = (uint32_t)shift_apply(c);

        checks++;
        if (got != (c->exp_res & mask)) {
            failures++;
            bad++;
            fprintf(stderr, "FAIL shift case %zu (op=%d size=%d v=0x%x count=%d): got 0x%x want 0x%x\n",
                    i, c->op, c->size, c->v, c->count, got, c->exp_res & mask);
            continue;
        }
        CHECK_EQ(getcf(), c->exp_cf);
        if (c->exp_of >= 0) {
            CHECK_EQ(getof(), c->exp_of);
        }
        if (c->op <= OP_rcr) {
            /* rotates move CF/OF only: ZF/SF/PF/AF keep the values they had */
            CHECK(!getzf() && !getsf() && !getpf() && !getaf());
        }
        else {
            /* shl/shr/sar/shld/shrd set ZF/SF/PF from the result (the SDM rule,
             * re-derived here rather than read out of the port) */
            uint32_t r = got & mask;
            CHECK_EQ(getzf(), r == 0);
            CHECK_EQ(getsf(), (r >> (c->size - 1)) & 1);
            CHECK_EQ(getpf(), parity_even(r));
        }
    }
    if (bad) {
        fprintf(stderr, "FAIL shift table: %d/%zu rows wrong\n", bad, n);
    }

    /* count == 0: the operand and EVERY flag are untouched (arith.rs's early return) */
    cpu_state_init();
    cpu->flags = FLAGS_DEFAULT | FLAG_ZERO | FLAG_CARRY;
    cpu->flags_changed = 0;
    CHECK_EQ(rol8(0x81, 0), 0x81);
    CHECK_EQ(ror16(0x8001, 0), 0x8001);
    CHECK_EQ(shl32(0x12345678, 0), 0x12345678);
    CHECK_EQ(sar8(0x80, 0), 0x80);
    CHECK_EQ(rcl8(0x55, 0), 0x55);
    CHECK_EQ(shld16(0x1234, 0x5678, 0), 0x1234);
    CHECK_EQ(cpu->flags, FLAGS_DEFAULT | FLAG_ZERO | FLAG_CARRY);
    CHECK(!getof() && !getaf());

    printf("PASS arith: %zu shift/rotate/shld/shrd cases (host-CPU-derived) + count==0\n", n);
}

/* ------------------------------------------------------------------ *
 * gate 4b -- mul/imul/div/idiv (arith.rs:130-272, 638-753)
 * ------------------------------------------------------------------ */

enum { MUL8, IMUL8, MUL16, IMUL16, MUL32, IMUL32, IMULR16, IMULR32 };

typedef struct {
    int op;
    int size;
    uint32_t a;
    uint32_t b;
    uint32_t exp_lo;
    uint32_t exp_hi;
    int exp_cf;
    int exp_of;
} mul_case;

static const mul_case mul_cases[] = {
{MUL8     ,  8, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0, 0},
    {IMUL8    ,  8, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0, 0},
    {MUL8     ,  8, 0x00000001, 0x00000001, 0x00000001, 0x00000000, 0, 0},
    {IMUL8    ,  8, 0x00000001, 0x00000001, 0x00000001, 0x00000000, 0, 0},
    {MUL8     ,  8, 0x0000007F, 0x00000002, 0x000000FE, 0x00000000, 0, 0},
    {IMUL8    ,  8, 0x0000007F, 0x00000002, 0x000000FE, 0x00000000, 1, 1},
    {MUL8     ,  8, 0x00000080, 0x00000002, 0x00000100, 0x00000000, 1, 1},
    {IMUL8    ,  8, 0x00000080, 0x00000002, 0x0000FF00, 0x00000000, 1, 1},
    {MUL8     ,  8, 0x00000081, 0x00000002, 0x00000102, 0x00000000, 1, 1},
    {IMUL8    ,  8, 0x00000081, 0x00000002, 0x0000FF02, 0x00000000, 1, 1},
    {MUL8     ,  8, 0x000000FF, 0x000000FF, 0x0000FE01, 0x00000000, 1, 1},
    {IMUL8    ,  8, 0x000000FF, 0x000000FF, 0x00000001, 0x00000000, 0, 0},
    {MUL8     ,  8, 0x00000002, 0x0000007F, 0x000000FE, 0x00000000, 0, 0},
    {IMUL8    ,  8, 0x00000002, 0x0000007F, 0x000000FE, 0x00000000, 1, 1},
    {MUL16    , 16, 0x00000001, 0x00000001, 0x00000001, 0x00000000, 0, 0},
    {IMUL16   , 16, 0x00000001, 0x00000001, 0x00000001, 0x00000000, 0, 0},
    {MUL16    , 16, 0x00007FFF, 0x00000002, 0x0000FFFE, 0x00000000, 0, 0},
    {IMUL16   , 16, 0x00007FFF, 0x00000002, 0x0000FFFE, 0x00000000, 1, 1},
    {MUL16    , 16, 0x00008000, 0x00000002, 0x00000000, 0x00000001, 1, 1},
    {IMUL16   , 16, 0x00008000, 0x00000002, 0x00000000, 0x0000FFFF, 1, 1},
    {MUL16    , 16, 0x0000FFFF, 0x0000FFFF, 0x00000001, 0x0000FFFE, 1, 1},
    {IMUL16   , 16, 0x0000FFFF, 0x0000FFFF, 0x00000001, 0x00000000, 0, 0},
    {MUL16    , 16, 0x00001234, 0x00000100, 0x00003400, 0x00000012, 1, 1},
    {IMUL16   , 16, 0x00001234, 0x00000100, 0x00003400, 0x00000012, 1, 1},
    {MUL16    , 16, 0x00008001, 0x00000001, 0x00008001, 0x00000000, 0, 0},
    {IMUL16   , 16, 0x00008001, 0x00000001, 0x00008001, 0x0000FFFF, 0, 0},
    {MUL32    , 32, 0x00000001, 0x00000001, 0x00000001, 0x00000000, 0, 0},
    {IMUL32   , 32, 0x00000001, 0x00000001, 0x00000001, 0x00000000, 0, 0},
    {MUL32    , 32, 0x7FFFFFFF, 0x00000002, 0xFFFFFFFE, 0x00000000, 0, 0},
    {IMUL32   , 32, 0x7FFFFFFF, 0x00000002, 0xFFFFFFFE, 0x00000000, 1, 1},
    {MUL32    , 32, 0x80000000, 0x00000002, 0x00000000, 0x00000001, 1, 1},
    {IMUL32   , 32, 0x80000000, 0x00000002, 0x00000000, 0xFFFFFFFF, 1, 1},
    {MUL32    , 32, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000001, 0xFFFFFFFE, 1, 1},
    {IMUL32   , 32, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000001, 0x00000000, 0, 0},
    {MUL32    , 32, 0x12345678, 0x00010000, 0x56780000, 0x00001234, 1, 1},
    {IMUL32   , 32, 0x12345678, 0x00010000, 0x56780000, 0x00001234, 1, 1},
    {MUL32    , 32, 0x80000000, 0x80000000, 0x00000000, 0x40000000, 1, 1},
    {IMUL32   , 32, 0x80000000, 0x80000000, 0x00000000, 0x40000000, 1, 1},
    {IMULR16  , 16, 0x00001234, 0x00000002, 0x00002468, 0x00000000, 0, 0},
    {IMULR16  , 16, 0x00008000, 0x00000002, 0x00000000, 0x00000000, 1, 1},
    {IMULR16  , 16, 0x0000FFFF, 0x0000FFFF, 0x00000001, 0x00000000, 0, 0},
    {IMULR16  , 16, 0x00007FFF, 0x00000001, 0x00007FFF, 0x00000000, 0, 0},
    {IMULR32  , 32, 0x12345678, 0x00010000, 0x56780000, 0x00000000, 1, 1},
    {IMULR32  , 32, 0x80000000, 0xFFFFFFFF, 0x80000000, 0x00000000, 1, 1},
    {IMULR32  , 32, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000001, 0x00000000, 0, 0},
    {IMULR32  , 32, 0x7FFFFFFF, 0x00000002, 0xFFFFFFFE, 0x00000000, 1, 1},
};

enum { DIV8, IDIV8, DIV16, IDIV16, DIV32, IDIV32 };

typedef struct {
    int op;
    int size;
    uint64_t target;
    uint32_t divisor;
    uint32_t exp_q;
    uint32_t exp_r;
    int de; /* 1: the case must raise #de on the hardware and in the port */
} div_case;

/* the div/idiv rows use a different shape; they are in div_cases below */
static const div_case div_cases[] = {
        {DIV8     ,  8, 0x0000000000000000, 0x00000002, 0x00000000, 0x00000000, 0},
    {DIV8     ,  8, 0x0000000000000001, 0x00000002, 0x00000000, 0x00000001, 0},
    {DIV8     ,  8, 0x00000000000000FE, 0x00000002, 0x0000007F, 0x00000000, 0},
    {DIV8     ,  8, 0x00000000000000FF, 0x00000010, 0x0000000F, 0x0000000F, 0},
    {DIV8     ,  8, 0x00000000000000FF, 0x00000001, 0x000000FF, 0x00000000, 0},
    {DIV8     ,  8, 0x0000000000000100, 0x00000001, 0x00000000, 0x00000000, 1},
    {DIV8     ,  8, 0x0000000000001234, 0x00000007, 0x00000000, 0x00000000, 1},
    {DIV8     ,  8, 0x0000000000000100, 0x00000000, 0x00000000, 0x00000000, 1},
    {IDIV8    ,  8, 0x0000000000000000, 0x00000002, 0x00000000, 0x00000000, 0},
    {IDIV8    ,  8, 0x000000000000FF00, 0x000000FF, 0x00000000, 0x00000000, 1},
    {IDIV8    ,  8, 0x0000000000000080, 0x00000001, 0x00000000, 0x00000000, 1},
    {IDIV8    ,  8, 0x000000000000FF7F, 0x00000002, 0x000000C0, 0x000000FF, 0},
    {IDIV8    ,  8, 0x000000000000FF81, 0x00000002, 0x000000C1, 0x000000FF, 0},
    {IDIV8    ,  8, 0x0000000000007FFF, 0x00000002, 0x00000000, 0x00000000, 1},
    {IDIV8    ,  8, 0x0000000000000040, 0x00000002, 0x00000020, 0x00000000, 0},
    {IDIV8    ,  8, 0x0000000000000001, 0x00000000, 0x00000000, 0x00000000, 1},
    {DIV16    , 16, 0x0000000000000000, 0x00000002, 0x00000000, 0x00000000, 0},
    {DIV16    , 16, 0x000000000001FFFF, 0x00000002, 0x0000FFFF, 0x00000001, 0},
    {DIV16    , 16, 0x0000000000010000, 0x00000002, 0x00008000, 0x00000000, 0},
    {DIV16    , 16, 0x0000000000010000, 0x00000001, 0x00000000, 0x00000000, 1},
    {DIV16    , 16, 0x00000000FFFFFFFF, 0x0000FFFF, 0x00000000, 0x00000000, 1},
    {DIV16    , 16, 0x0000000080000000, 0x00008000, 0x00000000, 0x00000000, 1},
    {DIV16    , 16, 0x0000000012345678, 0x00000100, 0x00000000, 0x00000000, 1},
    {DIV16    , 16, 0x000000000000FFFF, 0x00000000, 0x00000000, 0x00000000, 1},
    {IDIV16   , 16, 0x0000000000000000, 0x00000002, 0x00000000, 0x00000000, 0},
    {IDIV16   , 16, 0xFFFFFFFFFFFFFFFF, 0x00000001, 0x0000FFFF, 0x00000000, 0},
    {IDIV16   , 16, 0x0000000000008000, 0x00000001, 0x00000000, 0x00000000, 1},
    {IDIV16   , 16, 0x0000000000008000, 0x0000FFFF, 0x00008000, 0x00000000, 0},
    {IDIV16   , 16, 0x00000000FFFF8000, 0x00000002, 0x0000C000, 0x00000000, 0},
    {IDIV16   , 16, 0x0000000000007FFF, 0x00000002, 0x00003FFF, 0x00000001, 0},
    {IDIV16   , 16, 0xFFFFFFFFFFFFFFFF, 0x0000FFFF, 0x00000001, 0x00000000, 0},
    {IDIV16   , 16, 0x0000000000008001, 0x0000FFFF, 0x00000000, 0x00000000, 1},
    {DIV32    , 32, 0x0000000100000001, 0x00000003, 0x55555555, 0x00000002, 0},
    {DIV32    , 32, 0x0000000180000000, 0x00000007, 0x36DB6DB6, 0x00000006, 0},
    {DIV32    , 32, 0x0000000100000000, 0x00000001, 0x00000000, 0x00000000, 1},
    {DIV32    , 32, 0xFFFFFFFFFFFFFFFF, 0x00000002, 0x00000000, 0x00000000, 1},
    {DIV32    , 32, 0x0000000000000000, 0xFFFFFFFF, 0x00000000, 0x00000000, 0},
    {DIV32    , 32, 0x0000000200000000, 0x00000002, 0x00000000, 0x00000000, 1},
    {IDIV32   , 32, 0x00000000FFFFFFFF, 0x00000001, 0x00000000, 0x00000000, 1},
    {IDIV32   , 32, 0xFFFFFFFFFFFFFFFF, 0x00000001, 0xFFFFFFFF, 0x00000000, 0},
    {IDIV32   , 32, 0x0000000200000000, 0x00000002, 0x00000000, 0x00000000, 1},
    {IDIV32   , 32, 0x7FFFFFFFFFFFFFFF, 0x00000002, 0x00000000, 0x00000000, 1},
    {IDIV32   , 32, 0x8000000000000000, 0xFFFFFFFF, 0x00000000, 0x00000000, 1},
    {IDIV32   , 32, 0x00000000FFFFFFFF, 0xFFFFFFFF, 0x00000000, 0x00000000, 1},
};

static void test_arith_muldiv(void)
{
    size_t n = sizeof(mul_cases) / sizeof(mul_cases[0]);
    for (size_t i = 0; i < n; i++) {
        const mul_case *c = &mul_cases[i];
        cpu_state_init();
        reset_exceptions();
        cpu->reg64[EAX] = (int32_t)c->a;

        switch (c->op) {
        case MUL8:
            mul8((int32_t)c->b);
            CHECK_EQ(read_reg16(AX), c->exp_lo);
            break;
        case IMUL8:
            /* upstream's callers sign-extend the source byte (instructions.rs:
             * `imul8(read_reg8(r1) << 24 >> 24)`), so the test must too */
            imul8((int32_t)(int8_t)c->b);
            CHECK_EQ(read_reg16(AX), c->exp_lo);
            break;
        case MUL16:
            mul16(c->b);
            CHECK_EQ((uint32_t)read_reg16(AX), c->exp_lo);
            CHECK_EQ((uint32_t)read_reg16(DX), c->exp_hi);
            break;
        case IMUL16:
            imul16((int32_t)(int16_t)c->b); /* sign-extended by the caller, as above */
            CHECK_EQ((uint32_t)read_reg16(AX), c->exp_lo);
            CHECK_EQ((uint32_t)read_reg16(DX), c->exp_hi);
            break;
        case MUL32:
            mul32((int32_t)c->b);
            CHECK_EQ((uint32_t)read_reg32(EAX), c->exp_lo);
            CHECK_EQ((uint32_t)read_reg32(EDX), c->exp_hi);
            break;
        case IMUL32:
            imul32((int32_t)c->b);
            CHECK_EQ((uint32_t)read_reg32(EAX), c->exp_lo);
            CHECK_EQ((uint32_t)read_reg32(EDX), c->exp_hi);
            break;
        case IMULR16:
            /* the two-operand form returns the FULL product; the caller truncates it
             * with write_reg16/write_reg32, which is where the mask belongs */
            CHECK_EQ((uint32_t)imul_reg16((int32_t)c->a, (int32_t)c->b) & 0xFFFFu, c->exp_lo);
            break;
        default:
            CHECK_EQ((uint32_t)imul_reg32((int32_t)c->a, (int32_t)c->b), c->exp_lo);
            break;
        }
        CHECK_EQ(exc_count, 0);
        CHECK_EQ(getcf(), c->exp_cf);
        CHECK_EQ(getof(), c->exp_of);
    }

    size_t m = sizeof(div_cases) / sizeof(div_cases[0]);
    for (size_t i = 0; i < m; i++) {
        const div_case *c = &div_cases[i];
        cpu_state_init();
        reset_exceptions();

        switch (c->op) {
        case DIV8:
        case IDIV8:
            cpu->reg64[EAX] = (int32_t)(c->target & 0xFFFF);
            if (c->op == DIV8) div8(c->divisor);
            else idiv8((int32_t)(int8_t)c->divisor);
            break;
        case DIV16:
        case IDIV16:
            cpu->reg64[EAX] = (int32_t)(c->target & 0xFFFF);
            cpu->reg64[EDX] = (int32_t)((c->target >> 16) & 0xFFFF);
            if (c->op == DIV16) div16(c->divisor);
            else idiv16((int32_t)(int16_t)c->divisor);
            break;
        default:
            cpu->reg64[EAX] = (int32_t)(uint32_t)c->target;
            cpu->reg64[EDX] = (int32_t)(uint32_t)(c->target >> 32);
            if (c->op == DIV32) div32(c->divisor);
            else idiv32((int32_t)c->divisor);
            break;
        }

        if (c->de) {
            CHECK_EQ(exc_count, 1);
            CHECK_EQ(exc_vector, CPU_EXCEPTION_DE);
            continue;
        }
        CHECK_EQ(exc_count, 0);
        if (c->op <= IDIV8) {
            CHECK_EQ(read_reg8(AL), c->exp_q & 0xFF);
            CHECK_EQ(read_reg8(AH), c->exp_r & 0xFF);
        }
        else if (c->op <= IDIV16) {
            CHECK_EQ(read_reg16(AX), c->exp_q & 0xFFFF);
            CHECK_EQ(read_reg16(DX), c->exp_r & 0xFFFF);
        }
        else {
            CHECK_EQ((uint32_t)read_reg32(EAX), c->exp_q);
            CHECK_EQ((uint32_t)read_reg32(EDX), c->exp_r);
        }
    }

    /* xadd / cmpxchg (arith.rs:256-305) */
    cpu_state_init();
    write_reg32(EAX, 0x10);
    CHECK_EQ(xadd32(0x01, EAX), 0x11);
    CHECK_EQ(read_reg32(EAX), 0x01);
    cpu_state_init();
    write_reg16(AX, 0x1000);
    CHECK_EQ(cmpxchg16(0x1000, BX), read_reg16(BX));
    CHECK(getzf());
    cpu_state_init();
    write_reg8(AL, 0x42);
    CHECK_EQ(cmpxchg8(0x99, BL), 0x99);
    CHECK_EQ(read_reg8(AL), 0x99);
    CHECK(!getzf());

    printf("PASS arith: %zu mul/imul + %zu div/idiv cases (host-CPU-derived) + xadd/cmpxchg\n", n, m);
}

/* ------------------------------------------------------------------ *
 * gate 4c -- the BCD family (arith.rs:307-399)
 *
 * The host CPU cannot execute DAA/DAS/AAA/AAS/AAM/AAD in 64-bit mode (#UD), so these
 * rows come from a 32-bit (i386) probe: results/m1a2-oracle.txt has the probe and the
 * 1320-row diff.  AAD's CF/AF are left undefined by the SDM and the port follows
 * upstream's rule there, so those two fields are not asserted for AAD (see the report).
 * ------------------------------------------------------------------ */

enum { BCD_DAA, BCD_DAS, BCD_AAA, BCD_AAS, BCD_AAM, BCD_AAD };

typedef struct {
    int op;
    uint8_t al;
    uint8_t ah;
    int cf_in;
    int af_in;
    uint8_t exp_al;
    uint8_t exp_ah;
    int exp_cf;
    int exp_af;
    int flags_defined; /* AAD: CF/AF undefined by the SDM */
} bcd_case;

static const bcd_case bcd_cases[] = {
{BCD_DAA, 0x00, 0x00, 0, 0, 0x00, 0x00, 0, 0, 1},
    {BCD_DAA, 0x00, 0x00, 0, 1, 0x06, 0x00, 0, 1, 1},
    {BCD_DAA, 0x00, 0x00, 1, 1, 0x66, 0x00, 1, 1, 1},
    {BCD_DAA, 0x00, 0x0F, 0, 0, 0x00, 0x0F, 0, 0, 1},
    {BCD_DAA, 0x00, 0x0F, 1, 1, 0x66, 0x0F, 1, 1, 1},
    {BCD_DAA, 0x0A, 0x00, 0, 0, 0x10, 0x00, 0, 1, 1},
    {BCD_DAA, 0x0A, 0x00, 0, 1, 0x10, 0x00, 0, 1, 1},
    {BCD_DAA, 0x0A, 0x00, 1, 1, 0x70, 0x00, 1, 1, 1},
    {BCD_DAA, 0x0A, 0x0F, 0, 0, 0x10, 0x0F, 0, 1, 1},
    {BCD_DAA, 0x0A, 0x0F, 1, 1, 0x70, 0x0F, 1, 1, 1},
    {BCD_DAA, 0x60, 0x00, 0, 0, 0x60, 0x00, 0, 0, 1},
    {BCD_DAA, 0x60, 0x00, 0, 1, 0x66, 0x00, 0, 1, 1},
    {BCD_DAA, 0x60, 0x00, 1, 1, 0xC6, 0x00, 1, 1, 1},
    {BCD_DAA, 0x60, 0x0F, 0, 0, 0x60, 0x0F, 0, 0, 1},
    {BCD_DAA, 0x60, 0x0F, 1, 1, 0xC6, 0x0F, 1, 1, 1},
    {BCD_DAA, 0x99, 0x00, 0, 0, 0x99, 0x00, 0, 0, 1},
    {BCD_DAA, 0x99, 0x00, 0, 1, 0x9F, 0x00, 0, 1, 1},
    {BCD_DAA, 0x99, 0x00, 1, 1, 0xFF, 0x00, 1, 1, 1},
    {BCD_DAA, 0x99, 0x0F, 0, 0, 0x99, 0x0F, 0, 0, 1},
    {BCD_DAA, 0x99, 0x0F, 1, 1, 0xFF, 0x0F, 1, 1, 1},
    {BCD_DAA, 0x9A, 0x00, 0, 0, 0x00, 0x00, 1, 1, 1},
    {BCD_DAA, 0x9A, 0x00, 0, 1, 0x00, 0x00, 1, 1, 1},
    {BCD_DAA, 0x9A, 0x00, 1, 1, 0x00, 0x00, 1, 1, 1},
    {BCD_DAA, 0x9A, 0x0F, 0, 0, 0x00, 0x0F, 1, 1, 1},
    {BCD_DAA, 0x9A, 0x0F, 1, 1, 0x00, 0x0F, 1, 1, 1},
    {BCD_DAA, 0xFA, 0x00, 0, 0, 0x60, 0x00, 1, 1, 1},
    {BCD_DAA, 0xFA, 0x00, 0, 1, 0x60, 0x00, 1, 1, 1},
    {BCD_DAA, 0xFA, 0x00, 1, 1, 0x60, 0x00, 1, 1, 1},
    {BCD_DAA, 0xFA, 0x0F, 0, 0, 0x60, 0x0F, 1, 1, 1},
    {BCD_DAA, 0xFA, 0x0F, 1, 1, 0x60, 0x0F, 1, 1, 1},
    {BCD_DAS, 0x00, 0x00, 0, 0, 0x00, 0x00, 0, 0, 1},
    {BCD_DAS, 0x00, 0x00, 0, 1, 0xFA, 0x00, 1, 1, 1},
    {BCD_DAS, 0x00, 0x00, 1, 1, 0x9A, 0x00, 1, 1, 1},
    {BCD_DAS, 0x00, 0x0F, 0, 0, 0x00, 0x0F, 0, 0, 1},
    {BCD_DAS, 0x00, 0x0F, 1, 1, 0x9A, 0x0F, 1, 1, 1},
    {BCD_DAS, 0x0A, 0x00, 0, 0, 0x04, 0x00, 0, 1, 1},
    {BCD_DAS, 0x0A, 0x00, 0, 1, 0x04, 0x00, 0, 1, 1},
    {BCD_DAS, 0x0A, 0x00, 1, 1, 0xA4, 0x00, 1, 1, 1},
    {BCD_DAS, 0x0A, 0x0F, 0, 0, 0x04, 0x0F, 0, 1, 1},
    {BCD_DAS, 0x0A, 0x0F, 1, 1, 0xA4, 0x0F, 1, 1, 1},
    {BCD_DAS, 0x60, 0x00, 0, 0, 0x60, 0x00, 0, 0, 1},
    {BCD_DAS, 0x60, 0x00, 0, 1, 0x5A, 0x00, 0, 1, 1},
    {BCD_DAS, 0x60, 0x00, 1, 1, 0xFA, 0x00, 1, 1, 1},
    {BCD_DAS, 0x60, 0x0F, 0, 0, 0x60, 0x0F, 0, 0, 1},
    {BCD_DAS, 0x60, 0x0F, 1, 1, 0xFA, 0x0F, 1, 1, 1},
    {BCD_DAS, 0x99, 0x00, 0, 0, 0x99, 0x00, 0, 0, 1},
    {BCD_DAS, 0x99, 0x00, 0, 1, 0x93, 0x00, 0, 1, 1},
    {BCD_DAS, 0x99, 0x00, 1, 1, 0x33, 0x00, 1, 1, 1},
    {BCD_DAS, 0x99, 0x0F, 0, 0, 0x99, 0x0F, 0, 0, 1},
    {BCD_DAS, 0x99, 0x0F, 1, 1, 0x33, 0x0F, 1, 1, 1},
    {BCD_DAS, 0x9A, 0x00, 0, 0, 0x34, 0x00, 1, 1, 1},
    {BCD_DAS, 0x9A, 0x00, 0, 1, 0x34, 0x00, 1, 1, 1},
    {BCD_DAS, 0x9A, 0x00, 1, 1, 0x34, 0x00, 1, 1, 1},
    {BCD_DAS, 0x9A, 0x0F, 0, 0, 0x34, 0x0F, 1, 1, 1},
    {BCD_DAS, 0x9A, 0x0F, 1, 1, 0x34, 0x0F, 1, 1, 1},
    {BCD_DAS, 0xFA, 0x00, 0, 0, 0x94, 0x00, 1, 1, 1},
    {BCD_DAS, 0xFA, 0x00, 0, 1, 0x94, 0x00, 1, 1, 1},
    {BCD_DAS, 0xFA, 0x00, 1, 1, 0x94, 0x00, 1, 1, 1},
    {BCD_DAS, 0xFA, 0x0F, 0, 0, 0x94, 0x0F, 1, 1, 1},
    {BCD_DAS, 0xFA, 0x0F, 1, 1, 0x94, 0x0F, 1, 1, 1},
    {BCD_AAA, 0x00, 0x00, 0, 0, 0x00, 0x00, 0, 0, 1},
    {BCD_AAA, 0x00, 0x00, 0, 1, 0x06, 0x01, 1, 1, 1},
    {BCD_AAA, 0x00, 0x00, 1, 1, 0x06, 0x01, 1, 1, 1},
    {BCD_AAA, 0x00, 0x0F, 0, 0, 0x00, 0x0F, 0, 0, 1},
    {BCD_AAA, 0x00, 0x0F, 1, 1, 0x06, 0x10, 1, 1, 1},
    {BCD_AAA, 0x0A, 0x00, 0, 0, 0x00, 0x01, 1, 1, 1},
    {BCD_AAA, 0x0A, 0x00, 0, 1, 0x00, 0x01, 1, 1, 1},
    {BCD_AAA, 0x0A, 0x00, 1, 1, 0x00, 0x01, 1, 1, 1},
    {BCD_AAA, 0x0A, 0x0F, 0, 0, 0x00, 0x10, 1, 1, 1},
    {BCD_AAA, 0x0A, 0x0F, 1, 1, 0x00, 0x10, 1, 1, 1},
    {BCD_AAA, 0x60, 0x00, 0, 0, 0x00, 0x00, 0, 0, 1},
    {BCD_AAA, 0x60, 0x00, 0, 1, 0x06, 0x01, 1, 1, 1},
    {BCD_AAA, 0x60, 0x00, 1, 1, 0x06, 0x01, 1, 1, 1},
    {BCD_AAA, 0x60, 0x0F, 0, 0, 0x00, 0x0F, 0, 0, 1},
    {BCD_AAA, 0x60, 0x0F, 1, 1, 0x06, 0x10, 1, 1, 1},
    {BCD_AAA, 0x99, 0x00, 0, 0, 0x09, 0x00, 0, 0, 1},
    {BCD_AAA, 0x99, 0x00, 0, 1, 0x0F, 0x01, 1, 1, 1},
    {BCD_AAA, 0x99, 0x00, 1, 1, 0x0F, 0x01, 1, 1, 1},
    {BCD_AAA, 0x99, 0x0F, 0, 0, 0x09, 0x0F, 0, 0, 1},
    {BCD_AAA, 0x99, 0x0F, 1, 1, 0x0F, 0x10, 1, 1, 1},
    {BCD_AAA, 0x9A, 0x00, 0, 0, 0x00, 0x01, 1, 1, 1},
    {BCD_AAA, 0x9A, 0x00, 0, 1, 0x00, 0x01, 1, 1, 1},
    {BCD_AAA, 0x9A, 0x00, 1, 1, 0x00, 0x01, 1, 1, 1},
    {BCD_AAA, 0x9A, 0x0F, 0, 0, 0x00, 0x10, 1, 1, 1},
    {BCD_AAA, 0x9A, 0x0F, 1, 1, 0x00, 0x10, 1, 1, 1},
    {BCD_AAA, 0xFA, 0x00, 0, 0, 0x00, 0x02, 1, 1, 1},
    {BCD_AAA, 0xFA, 0x00, 0, 1, 0x00, 0x02, 1, 1, 1},
    {BCD_AAA, 0xFA, 0x00, 1, 1, 0x00, 0x02, 1, 1, 1},
    {BCD_AAA, 0xFA, 0x0F, 0, 0, 0x00, 0x11, 1, 1, 1},
    {BCD_AAA, 0xFA, 0x0F, 1, 1, 0x00, 0x11, 1, 1, 1},
    {BCD_AAS, 0x00, 0x00, 0, 0, 0x00, 0x00, 0, 0, 1},
    {BCD_AAS, 0x00, 0x00, 0, 1, 0x0A, 0xFE, 1, 1, 1},
    {BCD_AAS, 0x00, 0x00, 1, 1, 0x0A, 0xFE, 1, 1, 1},
    {BCD_AAS, 0x00, 0x0F, 0, 0, 0x00, 0x0F, 0, 0, 1},
    {BCD_AAS, 0x00, 0x0F, 1, 1, 0x0A, 0x0D, 1, 1, 1},
    {BCD_AAS, 0x0A, 0x00, 0, 0, 0x04, 0xFF, 1, 1, 1},
    {BCD_AAS, 0x0A, 0x00, 0, 1, 0x04, 0xFF, 1, 1, 1},
    {BCD_AAS, 0x0A, 0x00, 1, 1, 0x04, 0xFF, 1, 1, 1},
    {BCD_AAS, 0x0A, 0x0F, 0, 0, 0x04, 0x0E, 1, 1, 1},
    {BCD_AAS, 0x0A, 0x0F, 1, 1, 0x04, 0x0E, 1, 1, 1},
    {BCD_AAS, 0x60, 0x00, 0, 0, 0x00, 0x00, 0, 0, 1},
    {BCD_AAS, 0x60, 0x00, 0, 1, 0x0A, 0xFF, 1, 1, 1},
    {BCD_AAS, 0x60, 0x00, 1, 1, 0x0A, 0xFF, 1, 1, 1},
    {BCD_AAS, 0x60, 0x0F, 0, 0, 0x00, 0x0F, 0, 0, 1},
    {BCD_AAS, 0x60, 0x0F, 1, 1, 0x0A, 0x0E, 1, 1, 1},
    {BCD_AAS, 0x99, 0x00, 0, 0, 0x09, 0x00, 0, 0, 1},
    {BCD_AAS, 0x99, 0x00, 0, 1, 0x03, 0xFF, 1, 1, 1},
    {BCD_AAS, 0x99, 0x00, 1, 1, 0x03, 0xFF, 1, 1, 1},
    {BCD_AAS, 0x99, 0x0F, 0, 0, 0x09, 0x0F, 0, 0, 1},
    {BCD_AAS, 0x99, 0x0F, 1, 1, 0x03, 0x0E, 1, 1, 1},
    {BCD_AAS, 0x9A, 0x00, 0, 0, 0x04, 0xFF, 1, 1, 1},
    {BCD_AAS, 0x9A, 0x00, 0, 1, 0x04, 0xFF, 1, 1, 1},
    {BCD_AAS, 0x9A, 0x00, 1, 1, 0x04, 0xFF, 1, 1, 1},
    {BCD_AAS, 0x9A, 0x0F, 0, 0, 0x04, 0x0E, 1, 1, 1},
    {BCD_AAS, 0x9A, 0x0F, 1, 1, 0x04, 0x0E, 1, 1, 1},
    {BCD_AAS, 0xFA, 0x00, 0, 0, 0x04, 0xFF, 1, 1, 1},
    {BCD_AAS, 0xFA, 0x00, 0, 1, 0x04, 0xFF, 1, 1, 1},
    {BCD_AAS, 0xFA, 0x00, 1, 1, 0x04, 0xFF, 1, 1, 1},
    {BCD_AAS, 0xFA, 0x0F, 0, 0, 0x04, 0x0E, 1, 1, 1},
    {BCD_AAS, 0xFA, 0x0F, 1, 1, 0x04, 0x0E, 1, 1, 1},
    {BCD_AAM, 0x00, 0x00, 0, 0, 0x00, 0x00, 0, 0, 1},
    {BCD_AAM, 0x00, 0x00, 0, 1, 0x00, 0x00, 0, 0, 1},
    {BCD_AAM, 0x00, 0x00, 1, 1, 0x00, 0x00, 0, 0, 1},
    {BCD_AAM, 0x00, 0x0F, 0, 0, 0x00, 0x00, 0, 0, 1},
    {BCD_AAM, 0x00, 0x0F, 1, 1, 0x00, 0x00, 0, 0, 1},
    {BCD_AAM, 0x0A, 0x00, 0, 0, 0x00, 0x01, 0, 0, 1},
    {BCD_AAM, 0x0A, 0x00, 0, 1, 0x00, 0x01, 0, 0, 1},
    {BCD_AAM, 0x0A, 0x00, 1, 1, 0x00, 0x01, 0, 0, 1},
    {BCD_AAM, 0x0A, 0x0F, 0, 0, 0x00, 0x01, 0, 0, 1},
    {BCD_AAM, 0x0A, 0x0F, 1, 1, 0x00, 0x01, 0, 0, 1},
    {BCD_AAM, 0x60, 0x00, 0, 0, 0x06, 0x09, 0, 0, 1},
    {BCD_AAM, 0x60, 0x00, 0, 1, 0x06, 0x09, 0, 0, 1},
    {BCD_AAM, 0x60, 0x00, 1, 1, 0x06, 0x09, 0, 0, 1},
    {BCD_AAM, 0x60, 0x0F, 0, 0, 0x06, 0x09, 0, 0, 1},
    {BCD_AAM, 0x60, 0x0F, 1, 1, 0x06, 0x09, 0, 0, 1},
    {BCD_AAM, 0x99, 0x00, 0, 0, 0x03, 0x0F, 0, 0, 1},
    {BCD_AAM, 0x99, 0x00, 0, 1, 0x03, 0x0F, 0, 0, 1},
    {BCD_AAM, 0x99, 0x00, 1, 1, 0x03, 0x0F, 0, 0, 1},
    {BCD_AAM, 0x99, 0x0F, 0, 0, 0x03, 0x0F, 0, 0, 1},
    {BCD_AAM, 0x99, 0x0F, 1, 1, 0x03, 0x0F, 0, 0, 1},
    {BCD_AAM, 0x9A, 0x00, 0, 0, 0x04, 0x0F, 0, 0, 1},
    {BCD_AAM, 0x9A, 0x00, 0, 1, 0x04, 0x0F, 0, 0, 1},
    {BCD_AAM, 0x9A, 0x00, 1, 1, 0x04, 0x0F, 0, 0, 1},
    {BCD_AAM, 0x9A, 0x0F, 0, 0, 0x04, 0x0F, 0, 0, 1},
    {BCD_AAM, 0x9A, 0x0F, 1, 1, 0x04, 0x0F, 0, 0, 1},
    {BCD_AAM, 0xFA, 0x00, 0, 0, 0x00, 0x19, 0, 0, 1},
    {BCD_AAM, 0xFA, 0x00, 0, 1, 0x00, 0x19, 0, 0, 1},
    {BCD_AAM, 0xFA, 0x00, 1, 1, 0x00, 0x19, 0, 0, 1},
    {BCD_AAM, 0xFA, 0x0F, 0, 0, 0x00, 0x19, 0, 0, 1},
    {BCD_AAM, 0xFA, 0x0F, 1, 1, 0x00, 0x19, 0, 0, 1},
    {BCD_AAD, 0x00, 0x00, 0, 0, 0x00, 0x00, 0, 0, 0},
    {BCD_AAD, 0x00, 0x00, 0, 1, 0x00, 0x00, 0, 0, 0},
    {BCD_AAD, 0x00, 0x00, 1, 1, 0x00, 0x00, 0, 0, 0},
    {BCD_AAD, 0x00, 0x0F, 0, 0, 0x96, 0x00, 0, 0, 0},
    {BCD_AAD, 0x00, 0x0F, 1, 1, 0x96, 0x00, 0, 0, 0},
    {BCD_AAD, 0x0A, 0x00, 0, 0, 0x0A, 0x00, 0, 0, 0},
    {BCD_AAD, 0x0A, 0x00, 0, 1, 0x0A, 0x00, 0, 0, 0},
    {BCD_AAD, 0x0A, 0x00, 1, 1, 0x0A, 0x00, 0, 0, 0},
    {BCD_AAD, 0x0A, 0x0F, 0, 0, 0xA0, 0x00, 0, 1, 0},
    {BCD_AAD, 0x0A, 0x0F, 1, 1, 0xA0, 0x00, 0, 1, 0},
    {BCD_AAD, 0x60, 0x00, 0, 0, 0x60, 0x00, 0, 0, 0},
    {BCD_AAD, 0x60, 0x00, 0, 1, 0x60, 0x00, 0, 0, 0},
    {BCD_AAD, 0x60, 0x00, 1, 1, 0x60, 0x00, 0, 0, 0},
    {BCD_AAD, 0x60, 0x0F, 0, 0, 0xF6, 0x00, 0, 0, 0},
    {BCD_AAD, 0x60, 0x0F, 1, 1, 0xF6, 0x00, 0, 0, 0},
    {BCD_AAD, 0x99, 0x00, 0, 0, 0x99, 0x00, 0, 0, 0},
    {BCD_AAD, 0x99, 0x00, 0, 1, 0x99, 0x00, 0, 0, 0},
    {BCD_AAD, 0x99, 0x00, 1, 1, 0x99, 0x00, 0, 0, 0},
    {BCD_AAD, 0x99, 0x0F, 0, 0, 0x2F, 0x00, 1, 0, 0},
    {BCD_AAD, 0x99, 0x0F, 1, 1, 0x2F, 0x00, 1, 0, 0},
    {BCD_AAD, 0x9A, 0x00, 0, 0, 0x9A, 0x00, 0, 0, 0},
    {BCD_AAD, 0x9A, 0x00, 0, 1, 0x9A, 0x00, 0, 0, 0},
    {BCD_AAD, 0x9A, 0x00, 1, 1, 0x9A, 0x00, 0, 0, 0},
    {BCD_AAD, 0x9A, 0x0F, 0, 0, 0x30, 0x00, 1, 1, 0},
    {BCD_AAD, 0x9A, 0x0F, 1, 1, 0x30, 0x00, 1, 1, 0},
    {BCD_AAD, 0xFA, 0x00, 0, 0, 0xFA, 0x00, 0, 0, 0},
    {BCD_AAD, 0xFA, 0x00, 0, 1, 0xFA, 0x00, 0, 0, 0},
    {BCD_AAD, 0xFA, 0x00, 1, 1, 0xFA, 0x00, 0, 0, 0},
    {BCD_AAD, 0xFA, 0x0F, 0, 0, 0x90, 0x00, 1, 1, 0},
    {BCD_AAD, 0xFA, 0x0F, 1, 1, 0x90, 0x00, 1, 1, 0},
};

static void test_arith_bcd(void)
{
    size_t n = sizeof(bcd_cases) / sizeof(bcd_cases[0]);
    for (size_t i = 0; i < n; i++) {
        const bcd_case *c = &bcd_cases[i];
        cpu_state_init();
        cpu->flags = FLAGS_DEFAULT | (c->cf_in ? FLAG_CARRY : 0) |
                     (c->af_in ? FLAG_ADJUST : 0);
        cpu->flags_changed = 0;
        cpu->reg64[EAX] = ((int32_t)c->ah << 8) | c->al;

        switch (c->op) {
        case BCD_DAA: bcd_daa(); break;
        case BCD_DAS: bcd_das(); break;
        case BCD_AAA: bcd_aaa(); break;
        case BCD_AAS: bcd_aas(); break;
        case BCD_AAM: bcd_aam(10); break;
        default: bcd_aad(10); break;
        }

        CHECK_EQ(read_reg8(AL), c->exp_al);
        CHECK_EQ(read_reg8(AH), c->exp_ah);
        if (c->flags_defined) {
            CHECK_EQ(getcf(), c->exp_cf);
            CHECK_EQ(getaf(), c->exp_af);
        }
    }

    /* AAM with imm8 == 0 raises #de (measured on the hardware: SIGFPE) */
    cpu_state_init();
    reset_exceptions();
    bcd_aam(0);
    CHECK_EQ(exc_count, 1);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_DE);

    printf("PASS arith: %zu BCD cases (32-bit host probe) + aam 0 -> #de\n", n);
}

/* ------------------------------------------------------------------ *
 * gate 4d -- bit test / scan / logic / saturating helpers
 * ------------------------------------------------------------------ */

static void test_arith_bits(void)
{
    /* bt/bts/btr/btc on a register: CF is the tested bit, and the update is masked */
    cpu_state_init();
    bt_reg(0x80000000, 31);
    CHECK_EQ(getcf(), 1);
    bt_reg(0x80000000, 30);
    CHECK_EQ(getcf(), 0);
    CHECK_EQ((uint32_t)bts_reg(0, 31), 0x80000000u); /* 1 << 31 must not be UB-laden */
    CHECK_EQ(getcf(), 0);
    CHECK_EQ((uint32_t)btc_reg(0xFFFFFFFF, 0), 0xFFFFFFFEu);
    CHECK_EQ(getcf(), 1);
    CHECK_EQ((uint32_t)btr_reg(0xFFFFFFFF, 31), 0x7FFFFFFFu);
    CHECK_EQ(getcf(), 1);

    /* the memory forms: CF from the byte, then the bit is written back */
    cpu->cr[0] = RESET_CR0; /* paging off: identity */
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    write8(0x2000, 0x80);
    bt_mem(0x2000, 8); /* bit 8 -> byte 1, bit 0 */
    CHECK_EQ(getcf(), 0);
    bt_mem(0x2000, 7);
    CHECK_EQ(getcf(), 1);
    bts_mem(0x2000, 0);
    CHECK_EQ(read8(0x2000), 0x81);
    btr_mem(0x2000, 7);
    CHECK_EQ(read8(0x2000), 0x01);
    btc_mem(0x2000, 1);
    CHECK_EQ(read8(0x2000), 0x03);
    CHECK_EQ(getcf(), 0);

    /* bsf/bsr: ZF is the "no bit set" marker, and the destination is left alone then */
    cpu_state_init();
    CHECK_EQ(bsf16(0xDEAD, 0x1000), 12);
    CHECK(!getzf());
    CHECK_EQ(bsf32(0xDEAD, 0), 0xDEAD);
    CHECK(getzf());
    CHECK_EQ(bsr32(0xDEAD, 0x80000000), 31);
    CHECK(!getzf());
    CHECK_EQ(bsr16(0xDEAD, 0), 0xDEAD);
    CHECK(getzf());
    CHECK_EQ(bsf32(0xDEAD, 0xFFFF0000), 16);

    cpu_state_init();
    CHECK_EQ(popcnt(0), 0);
    CHECK(getzf());
    CHECK_EQ(popcnt(0xFFFFFFFF), 32);
    CHECK(!getzf() && !getcf() && !getof());

    /* not / neg / and / or / xor / test */
    cpu_state_init();
    CHECK_EQ((uint32_t)not8(0x0F), 0xFFFFFFF0u); /* `not` is the BITWISE complement */
    CHECK_EQ((uint32_t)neg8(0x01), 0xFFu);
    CHECK(getcf()); /* 0 - 1 borrows */
    CHECK_EQ(neg16(0x8000), 0x8000);
    CHECK(getof());
    cpu_state_init();
    CHECK_EQ(and32(0x0F0F0F0F, 0x00FF00FF), 0x000F000F);
    CHECK(!getcf() && !getof() && !getaf());
    CHECK_EQ(or8(0x00, 0x80), 0x80);
    CHECK(!getcf() && !getof() && !getaf());
    cpu_state_init();
    CHECK_EQ(xor16(0x1234, 0x1234), 0);
    CHECK(getzf() && getpf()); /* a zero low byte has EVEN parity */
    test8(0x0F, 0x01); /* AF/CF/OF cleared even though the AND is nonzero */
    CHECK(!getcf() && !getof() && !getaf());
    CHECK(!getpf()); /* 0x0F & 0x01 == 1: one bit set, odd parity */

    /* the saturating pack helpers (arith.rs:1127-1206) */
    CHECK_EQ(saturate_sw_to_ub(0xFFFF), 0);
    CHECK_EQ(saturate_sw_to_ub(0x8000), 0);
    CHECK_EQ(saturate_sw_to_ub(0x0100), 255);
    CHECK_EQ(saturate_sw_to_ub(0x00FF), 255);
    CHECK_EQ(saturate_sw_to_sb(0x0080), 127);
    CHECK_EQ(saturate_sw_to_sb(0x8000), 128);
    CHECK_EQ(saturate_sw_to_sb(0xFF80), 0x80);
    CHECK_EQ(saturate_sd_to_sw(0x00008000), 32767);
    CHECK_EQ(saturate_sd_to_sw(0x80000000u), 32768);
    CHECK_EQ(saturate_sd_to_sb(0x00000080u), 127);
    CHECK_EQ(saturate_sd_to_sb(0x80000000u), (int8_t)128); /* the return type is i8 */
    CHECK_EQ(saturate_sd_to_ub(-1), 0);
    CHECK_EQ(saturate_sd_to_ub(0x7F), 0x7F);
    CHECK_EQ(saturate_ud_to_ub(0x100), 255);
    CHECK_EQ(saturate_uw(0x80000000u), 0);
    CHECK_EQ(saturate_uw(0x10000), 0xFFFF);

    printf("PASS arith: bt/bsf/bsr/popcnt, logic flag clears, saturating helpers\n");
}

/* ------------------------------------------------------------------ *
 * gate 5 -- modrm addressing (modrm.rs:4-133, cpu.rs:3122-3130)
 * ------------------------------------------------------------------ */

static void data_segments(void)
{
    /* DS and SS are made DIFFERENT so a wrong default segment shows up in the result;
     * CS is zeroed so the jump/loop tests are not entangled with reset_cpu's 0xF000 */
    cpu->segment_offsets[CS] = 0;
    cpu->segment_offsets[DS] = 0;
    cpu->segment_offsets[SS] = 0x1000;
    cpu->segment_offsets[ES] = 0x2000;
    cpu->segment_is_null[DS] = false;
    cpu->prefixes = 0;
    cpu->pagefault = false;
}

static void test_modrm_addressing(void)
{
    cpu->cr[0] = RESET_CR0;
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    cpu_state_init();
    data_segments();
    cpu->is_32 = true;

    write_reg32(EAX, 0x11111111);
    write_reg32(ECX, 0x00000200);
    write_reg32(EDX, 0x00000100);
    write_reg32(EBX, 0x33333333);
    write_reg32(ESP, 0x3000);
    write_reg32(EBP, 0x4000);
    write_reg32(ESI, 0x5000);
    write_reg32(EDI, 0x6000);

    /* mod 00: [EAX] */
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(modrm_resolve(0x00), 0x11111111);
    CHECK(!cpu->pagefault);
    CHECK_EQ(cpu->instruction_pointer, 0x8000); /* no displacement consumed */

    /* mod 01: [EAX + disp8], disp read from the instruction stream */
    mem8[0x8000] = 0x10;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(modrm_resolve(0x40), 0x11111121);
    CHECK_EQ(cpu->instruction_pointer, 0x8001);

    /* mod 10: [EAX + disp32] */
    write32(0x8000, 0x00002345);
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(modrm_resolve(0x80), 0x11113456);
    CHECK_EQ(cpu->instruction_pointer, 0x8004);

    /* rm 101 selects [disp32] (DS) at mod 00 and [EBP + disp] (SS) at mod 01/10 */
    write32(0x8000, 0x00001234);
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(modrm_resolve(0x05), 0x1234); /* absolute, DS */
    CHECK_EQ(cpu->instruction_pointer, 0x8004);
    mem8[0x8000] = 0x34;
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(modrm_resolve(0x45), 0x1000 + 0x4000 + 0x34); /* [EBP + disp8], SS */
    write32(0x8000, 0x00002345);
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(modrm_resolve(0x85), 0x1000 + 0x4000 + 0x2345); /* [EBP + disp32], SS */

    /* SIB: rm 100, mod 00, sib 0x24 -> base ESP, index none (index==100), SS */
    mem8[0x8000] = 0x24;
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(modrm_resolve(0x04), 0x1000 + 0x3000);

    /* SIB with a scaled index: sib 0x91 -> scale 2, index EDX (<< 2), base ECX (DS) */
    mem8[0x8000] = 0x91;
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(modrm_resolve(0x04), 0x00000200 + (0x00000100 << 2));

    /* SIB, base 101 with mod 00 -> disp32 (DS), not EBP */
    mem8[0x8000] = 0x25; /* scale 0, index 100 (none), base 101 */
    write32(0x8001, 0x00007777);
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(modrm_resolve(0x04), 0x7777);
    CHECK_EQ(cpu->instruction_pointer, 0x8005);

    /* SIB, base 101 with mod 01 -> [EBP + disp8] (SS) */
    mem8[0x8000] = 0x25;
    mem8[0x8001] = 0x08;
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(modrm_resolve(0x44), 0x1000 + 0x4000 + 0x08);

    /* sib 0x64: scale 1, index 100 (none), base 100 (ESP) -> SS + ESP, no index */
    mem8[0x8000] = 0x64;
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(modrm_resolve(0x04), 0x1000 + 0x3000);

    /* the segment-override prefix: ES (1), SS (2) and the "zero" encoding (7) */
    cpu->prefixes = ES + 1;
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(modrm_resolve(0x00), 0x2000 + 0x11111111);
    cpu->prefixes = SS + 1;
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(modrm_resolve(0x00), 0x1000 + 0x11111111);
    cpu->prefixes = SEG_PREFIX_ZERO;
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(modrm_resolve(0x00), 0x11111111); /* no segment base at all */
    cpu->prefixes = 0;

    /* segment_prefix() differs from get_seg_prefix() by asserting instead of returning
     * 0 for the zero encoding, so only the two non-zero arms are exercised here */
    CHECK_EQ(segment_prefix(DS), DS);
    cpu->prefixes = ES + 1;
    CHECK_EQ(segment_prefix(DS), ES);
    cpu->prefixes = 0;

    /* 16-bit addressing (is_32 false -> is_asize_32 false -> resolve_modrm16) */
    cpu->is_32 = false;
    write_reg16(BX, 0x0100);
    write_reg16(BP, 0x0200);
    write_reg16(SI, 0x0030);
    write_reg16(DI, 0x0040);
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK(!is_asize_32());
    CHECK_EQ(modrm_resolve(0x00), 0x0130); /* [BX + SI] */
    CHECK_EQ(modrm_resolve(0x01), 0x0140); /* [BX + DI] */
    CHECK_EQ(modrm_resolve(0x02), 0x1000 + 0x0230); /* [BP + SI] is SS-relative */
    mem8[0x8000] = 0x10;
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(modrm_resolve(0x40), 0x0140); /* [BX + SI + disp8] */
    write16(0x8000, 0x2345);
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(modrm_resolve(0x80), 0x0130 + 0x2345); /* [BX + SI + disp16] */
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(modrm_resolve(0x87), 0x0100 + 0x2345); /* [BX + disp16] */
    mem8[0x8000] = 0xF0;
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(modrm_resolve(0x46), 0x1000 + 0x01F0); /* [BP + disp8] = BP-16, SS */
    CHECK_EQ(modrm_resolve(0x04), 0x0030);          /* [SI] */
    CHECK_EQ(modrm_resolve(0x05), 0x0040);          /* [DI] */
    CHECK_EQ(modrm_resolve(0x07), 0x0100);          /* [BX] */
    write16(0x8000, 0x1000);
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(modrm_resolve(0x06), 0x1000); /* [disp16] is DS-relative and absolute */
    CHECK_EQ(get_seg_cs(), cpu->segment_offsets[CS]);
    CHECK_EQ(get_seg_ss(), 0x1000);

    /* resolve_modrm32_ is upstream-dead but public: exercise it directly so it is not
       an untested code path (modrm.rs:37-66) */
    cpu->is_32 = true;
    write_reg32(EAX, 0x11111111);
    write_reg32(EBP, 0x4000); /* the 16-bit section overwrote BP */
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(resolve_modrm32_(0x00), 0x11111111);
    mem8[0x8000] = 0x10;
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(resolve_modrm32_(0x40), 0x11111121);
    mem8[0x8000] = 0x24;
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(resolve_modrm32_(0x04), 0x1000 + 0x3000);
    write32(0x8000, 0x00007777);
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(resolve_modrm32_(0x05), 0x7777);
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    mem8[0x8000] = 0x08;
    CHECK_EQ(resolve_modrm32_(0x45), 0x1000 + 0x4000 + 0x08);

    printf("PASS modrm: 32- and 16-bit addressing, SIB, segment defaults and overrides\n");
}

static void test_modrm_fetch_and_faults(void)
{
    /* the fetch path with paging off: identity, and instruction_pointer advances */
    cpu->cr[0] = RESET_CR0;
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    cpu_state_init();
    data_segments();
    cpu->is_32 = true;
    mem8[0x8000] = 0x78;
    mem8[0x8001] = 0x56;
    mem8[0x8002] = 0x34;
    mem8[0x8003] = 0x12;

    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK_EQ(read_imm8(), 0x78);
    CHECK_EQ(cpu->instruction_pointer, 0x8001);
    CHECK_EQ(cpu->eip_phys, (int32_t)((uint32_t)0x8000 ^ (uint32_t)0x8000));
    CHECK_EQ(cpu->last_virt_eip, 0x8000);

    /* read_imm16 takes the cached fast path when the page is unchanged.  The stream is
     * [78 56 34 12 ...] and ip is now 0x8001, so the word read there is 0x3456 */
    write16(0x8000, 0x5678);
    write16(0x8002, 0x1234);
    cpu->instruction_pointer = 0x8001;
    cpu->last_virt_eip = 0x8000;
    CHECK_EQ(read_imm16(), 0x3456);
    CHECK_EQ(cpu->instruction_pointer, 0x8003);
    write32(0x8003, 0x11223344);
    CHECK_EQ(read_imm32s(), 0x11223344);
    CHECK_EQ(cpu->instruction_pointer, 0x8007);

    /* ... and read_imm8s sign-extends */
    mem8[0x8008] = 0xFF;
    cpu->instruction_pointer = 0x8008;
    CHECK_EQ(read_imm8s(), -1);
    CHECK_EQ(cpu->instruction_pointer, 0x8009);

    /* a page-crossing imm16 re-translates per byte: force it with the low-12-bit test.
     * With paging OFF the fast and slow paths read the same bytes (identity), so this
     * case is done under a page table whose two virtual pages map to non-adjacent
     * PHYSICAL pages: only the per-byte re-translation finds the second byte. */
    paging_setup();
    cpu->cr[0] = CR0_PE | CR0_PG;
    cpu->cr[3] = (int32_t)PD_PHYS;
    map4k(0, 8, 0x3000, PAGE_TABLE_RW_MASK, PAGE_TABLE_RW_MASK); /* 0x8000 -> 0x3000 */
    map4k(0, 9, 0x5000, PAGE_TABLE_RW_MASK, PAGE_TABLE_RW_MASK); /* 0x9000 -> 0x5000 */
    mem8[0x3000] = 0x5A; /*  virtual 0x8000 (the page's first byte) */
    mem8[0x3FFF] = 0x11; /* last byte of 0x8000's page */
    mem8[0x4000] = 0x22; /* the physical neighbour a stale cache would read */
    mem8[0x5000] = 0x33; /* the first byte of 0x9000's page */
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    cpu->pagefault = false;
    CHECK_EQ(read_imm8(), 0x5A); /* caches the translation of the 0x8000 page */
    cpu->instruction_pointer = 0x8FFF;
    CHECK_EQ((uint32_t)(cpu->instruction_pointer ^ cpu->last_virt_eip) > 0xFFE, 1);
    CHECK_EQ(read_imm16(), 0x3311); /* NOT 0x2211: the second byte is re-translated */
    CHECK_EQ(cpu->instruction_pointer, 0x9001);
    CHECK(!cpu->pagefault);
    cpu->cr[0] = RESET_CR0;
    full_clear_tlb();
    valid_tlb_entries_count = 0;

    /* read_moffs follows the address-size attribute */
    cpu->is_32 = true;
    cpu->prefixes = 0;
    write32(0x8000, 0x11223344);
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK(is_asize_32());
    CHECK_EQ(read_moffs(), 0x11223344);
    cpu->prefixes = PREFIX_67; /* address-size override: 16-bit form */
    cpu->instruction_pointer = 0x8000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    CHECK(!is_asize_32());
    CHECK_EQ(read_moffs(), 0x3344);
    cpu->prefixes = 0;

    /* is_osize_32 flips on the 66 prefix */
    cpu->is_32 = true;
    CHECK(is_osize_32());
    cpu->prefixes = PREFIX_66;
    CHECK(!is_osize_32());
    cpu->is_32 = false;
    CHECK(is_osize_32());
    cpu->prefixes = 0;

    /* a fault in the fetch path leaves the instruction pointer and the cache untouched */
    paging_setup();
    cpu->cr[0] = CR0_PE | CR0_PG;
    cpu->cr[3] = (int32_t)PD_PHYS;
    map4k(1, 0, DATA_PHYS, PAGE_TABLE_RW_MASK, PAGE_TABLE_RW_MASK);
    mem8[DATA_PHYS] = 0x5A;
    cpu->instruction_pointer = 0x400000; /* mapped -> phys 0x3000 */
    cpu->last_virt_eip = VIRT_EIP_NONE;
    cpu->pagefault = false;
    CHECK_EQ(read_imm8(), 0x5A);
    CHECK(!cpu->pagefault);
    CHECK_EQ(cpu->instruction_pointer, 0x400001);

    cpu->instruction_pointer = 0x401000; /* unmapped */
    cpu->last_virt_eip = VIRT_EIP_NONE;
    cpu->pagefault = false;
    reset_delivery();
    CHECK_EQ(read_imm8(), 0);
    CHECK(cpu->pagefault);
    CHECK_EQ(cpu->last_virt_eip, VIRT_EIP_NONE); /* not cached (S3: the sentinel's spelling) */
    CHECK_EQ(delivered_interrupt, CPU_EXCEPTION_PF);
    /* trigger_pagefault rewrites eip to previous_ip (cpu.rs:2480-2484), so the fault
     * leaves the instruction pointer at previous_ip, NOT at 0x401000 */
    CHECK_EQ(cpu->previous_ip, 0);
    CHECK_EQ(cpu->instruction_pointer, 0);

    /* a fault inside a modrm displacement aborts before the segment is consulted */
    cpu->prefixes = 0;
    cpu->is_32 = true;
    cpu->instruction_pointer = 0x401000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    cpu->pagefault = false;
    reset_delivery();
    CHECK_EQ(resolve_modrm32(0x40), 0); /* [EAX + disp8] with the disp unmapped */
    CHECK(cpu->pagefault);
    CHECK_EQ(cpu->instruction_pointer, 0); /* previous_ip, as above */

    /* get_seg's #gp on a null segment sets the abort flag and leaves the sentinel */
    paging_setup();
    cpu->cr[0] = RESET_CR0;
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    data_segments();
    reset_exceptions();
    cpu->pagefault = false;
    cpu->segment_is_null[DS] = true;
    CHECK_EQ(get_seg_prefix_ds(0x1234), 0);
    CHECK(cpu->pagefault);
    CHECK_EQ(exc_count, 1);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_GP);
    cpu->segment_is_null[DS] = false;

    printf("PASS modrm: read_imm8/16/32s fetch, cross-page imm, moffs, faults, get_seg #gp\n");
}

/* ------------------------------------------------------------------ *
 * gate 6 -- misc_instr: the stack, the jumps, the task-switch tests
 * ------------------------------------------------------------------ */

static void test_misc_stack(void)
{
    cpu->cr[0] = RESET_CR0;
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    cpu_state_init();
    data_segments();

    /* get_stack_pointer / adjust_stack_reg in both stack sizes */
    cpu->stack_size_32 = true;
    write_reg32(ESP, 0x1000);
    CHECK_EQ(get_stack_pointer(-16), 0x1000 + 0x1000 - 16);
    adjust_stack_reg(-8);
    CHECK_EQ(read_reg32(ESP), 0x0FF8);
    cpu->stack_size_32 = false;
    write_reg16(SP, 0x0004);
    CHECK_EQ(get_stack_pointer(-16), 0x1000 + ((0x0004 - 16) & 0xFFFF)); /* wraps */
    adjust_stack_reg(-8); /* 4 - 8 wraps inside the 16-bit stack pointer */
    CHECK_EQ(read_reg16(SP), 0xFFFC);
    CHECK_EQ(get_stack_reg(), 0xFFFC);
    set_stack_reg(0x1234);
    CHECK_EQ(read_reg16(SP), 0x1234);

    /* push16 with a 32-bit stack: SS-relative, SP moves by 2, value little-endian */
    cpu->stack_size_32 = true;
    write_reg32(ESP, 0x1000);
    push16(0xABCD);
    CHECK_EQ(read_reg32(ESP), 0x0FFE);
    CHECK_EQ(read16(0x1000 + 0x0FFE), 0xABCD);
    CHECK_EQ(pop16(), 0xABCD);
    CHECK_EQ(read_reg32(ESP), 0x1000);

    /* push32 with a 16-bit stack: SP moves by 4, but the WRITE is still 32 bits wide
     * (push32_ss16, misc_instr.rs:175-180) */
    cpu->stack_size_32 = false;
    write_reg16(SP, 0x1000);
    push32(0x11223344);
    CHECK_EQ(read_reg16(SP), 0x0FFC);
    CHECK_EQ(read32s(0x1000 + 0x0FFC), 0x11223344);
    CHECK_EQ(pop32s(), 0x11223344);
    CHECK_EQ(read_reg16(SP), 0x1000);

    /* push32_sreg stores 16 bits and still moves the stack pointer by 4 */
    cpu_state_init();
    data_segments();
    cpu->stack_size_32 = true;
    write_reg32(ESP, 0x1000);
    cpu->sreg[ES] = 0x1234;
    push32_sreg(ES);
    CHECK_EQ(read_reg32(ESP), 0x0FFC);
    write16(0x1000 + 0x0FFE, 0xFFFF); /* poison: the push must not touch this word */
    CHECK_EQ(read_reg32(ESP), 0x0FFC);
    CHECK_EQ(read16(0x1000 + 0x0FFC), 0x1234);
    CHECK_EQ(read16(0x1000 + 0x0FFE), 0xFFFF); /* only 2 bytes written */

    /* the var-args pushes: push16_ss16_mem reads first, then pushes */
    cpu_state_init();
    data_segments();
    cpu->stack_size_32 = false;
    write_reg16(SP, 0x1000);
    write16(0x2000, 0xBEEF);
    push16_ss16_mem(0x2000);
    CHECK_EQ(read_reg16(SP), 0x0FFE);
    CHECK_EQ(read16(0x1000 + 0x0FFE), 0xBEEF);
    push32_ss16_mem(0x2000); /* reads 32 bits, but only 16 are there */
    CHECK_EQ(read_reg16(SP), 0x0FFA);

    /* pusha16: SP is pushed as it was BEFORE the first push (upstream's temp) */
    cpu_state_init();
    data_segments();
    cpu->stack_size_32 = false;
    write_reg16(SP, 0x1000);
    write_reg16(AX, 0x0001);
    write_reg16(CX, 0x0002);
    write_reg16(DX, 0x0003);
    write_reg16(BX, 0x0004);
    write_reg16(BP, 0x0005);
    write_reg16(SI, 0x0006);
    write_reg16(DI, 0x0007);
    pusha16();
    CHECK_EQ(read_reg16(SP), 0x0FF0);
    CHECK_EQ(read16(0x1000 + 0x0FFE), 0x0001); /* AX pushed first: highest address */
    CHECK_EQ(read16(0x1000 + 0x0FFC), 0x0002);
    CHECK_EQ(read16(0x1000 + 0x0FFA), 0x0003);
    CHECK_EQ(read16(0x1000 + 0x0FF8), 0x0004);
    CHECK_EQ(read16(0x1000 + 0x0FF6), 0x1000); /* the ORIGINAL SP, not the decremented one */
    CHECK_EQ(read16(0x1000 + 0x0FF4), 0x0005);
    CHECK_EQ(read16(0x1000 + 0x0FF2), 0x0006);
    CHECK_EQ(read16(0x1000 + 0x0FF0), 0x0007); /* DI pushed last: lowest address */

    /* pusha32 */
    cpu_state_init();
    data_segments();
    cpu->stack_size_32 = true;
    write_reg32(ESP, 0x2000);
    write_reg32(EAX, 0x11); write_reg32(ECX, 0x22); write_reg32(EDX, 0x33);
    write_reg32(EBX, 0x44); write_reg32(EBP, 0x55); write_reg32(ESI, 0x66);
    write_reg32(EDI, 0x77);
    pusha32();
    CHECK_EQ(read_reg32(ESP), 0x1FE0);
    CHECK_EQ(read32s(0x1000 + 0x1FFC), 0x11);   /* EAX first: highest address */
    CHECK_EQ(read32s(0x1000 + 0x1FEC), 0x2000); /* the ORIGINAL ESP */
    CHECK_EQ(read32s(0x1000 + 0x1FE0), 0x77);   /* EDI last: lowest address */

    /* the fault path: an unwritable stack page leaves SP UNCHANGED */
    cpu_state_init();
    data_segments();
    cpu->stack_size_32 = false;
    write_reg16(SP, 0x1000);
    paging_setup();
    cpu->cr[0] = CR0_PE | CR0_PG;
    cpu->cr[3] = (int32_t)PD_PHYS;
    cpu->segment_offsets[SS] = 0;
    map4k(1, 0, DATA_PHYS, PAGE_TABLE_RW_MASK, PAGE_TABLE_RW_MASK); /* 0x400000 only */
    write_reg16(SP, 0x1234);
    cpu->stack_size_32 = false;
    cpu->pagefault = false;
    reset_delivery();
    push16(0x1111);
    CHECK(cpu->pagefault);
    CHECK_EQ(read_reg16(SP), 0x1234); /* unchanged: upstream's `?` returns first */
    CHECK_EQ(delivered_interrupt, CPU_EXCEPTION_PF);

    cpu->pagefault = false;
    reset_delivery();
    write_reg16(SP, 0x1234);
    CHECK_EQ(pop16(), 0);
    CHECK(cpu->pagefault);
    CHECK_EQ(read_reg16(SP), 0x1234);

    /* enter16 with nesting level 0: the frame pointer is stored and BP is set to it */
    cpu->cr[0] = RESET_CR0;
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    cpu_state_init();
    data_segments();
    cpu->stack_size_32 = false;
    write_reg32(ESP, 0x1000);
    write_reg16(BP, 0x7777);
    enter16(8, 0);
    CHECK_EQ(read_reg16(BP), 0x0FFE);          /* frame_temp = ESP - 2 */
    CHECK_EQ(read16(0x1000 + 0x0FFE), 0x7777); /* the old BP is stored there */
    CHECK_EQ(read_reg16(SP), 0x0FF6);          /* adjust_stack_reg(-size - 2) from ESP */

    /* enter16 with nesting level 1 ALSO pushes frame_temp -- at the very same address
     * the BP store then uses, so the pushed word is overwritten by the old BP.  That is
     * upstream's behaviour (misc_instr.rs:320-330), asserted here so it cannot drift. */
    cpu_state_init();
    data_segments();
    cpu->stack_size_32 = false;
    write_reg32(ESP, 0x1000);
    write_reg16(BP, 0x7777);
    enter16(0, 1);
    CHECK_EQ(read_reg16(BP), 0x0FFE);
    CHECK_EQ(read16(0x1000 + 0x0FFE), 0x7777); /* the frame_temp push was overwritten */
    CHECK_EQ(read_reg16(SP), 0x0FFC);

    /* lss16/lss32 take both halves only when switch_seg succeeds */
    cpu_state_init();
    data_segments();
    write32(0x2000, 0x00001234);
    write16(0x2004, 0x0088);
    switch_seg_calls = 0;
    switch_seg_answer = true;
    write_reg32(EAX, 0);
    lss32(0x2000, EAX, ES);
    CHECK_EQ(switch_seg_calls, 1);
    CHECK_EQ(read_reg32(EAX), 0x1234);
    switch_seg_answer = false;
    write_reg32(EAX, 0x99);
    lss32(0x2000, EAX, ES);
    CHECK_EQ(read_reg32(EAX), 0x99); /* abandoned: the register keeps its value */
    write32(0x3000, 0x00001234);
    write16(0x3002, 0x0088);
    switch_seg_answer = true;
    write_reg16(BX, 0);
    lss16(0x3000, BX, ES);
    CHECK_EQ(read_reg16(BX), 0x1234);

    printf("PASS misc: push/pop/pusha/enter/lss, stack-pointer helpers, fault paths\n");
}

static void test_misc_jumps(void)
{
    cpu_state_init();
    data_segments();
    cpu->is_32 = true;

    /* jmp_rel16 limits ip to 16 bits around the CS offset */
    cpu->segment_offsets[CS] = 0x10000;
    cpu->instruction_pointer = 0x1FFF0; /* CS-relative ip = 0xFFF0 */
    jmp_rel16(0x0020);
    CHECK_EQ(cpu->instruction_pointer, 0x10000 + 0x0010);
    jmp_rel16(-0x30);
    CHECK_EQ(cpu->instruction_pointer, 0x10000 + 0xFFE0); /* wrapped inside the CS */

    /* jmpcc16 / jmpcc32 */
    cpu->segment_offsets[CS] = 0;
    cpu->instruction_pointer = 0x1000;
    jmpcc16(false, 0x100);
    CHECK_EQ(cpu->instruction_pointer, 0x1000);
    jmpcc16(true, 0x100);
    CHECK_EQ(cpu->instruction_pointer, 0x1100);
    cpu->instruction_pointer = 0x2000;
    jmpcc32(false, 0x100);
    CHECK_EQ(cpu->instruction_pointer, 0x2000);
    jmpcc32(true, 0x100);
    CHECK_EQ(cpu->instruction_pointer, 0x2100);

    /* loop/loope/loopne decrement ECX as the address size dictates and test ZF */
    cpu_state_init();
    data_segments();
    cpu->is_32 = true;
    cpu->prefixes = 0;
    write_reg32(ECX, 3);
    cpu->instruction_pointer = 0x1000;
    loop16(0x10);
    CHECK_EQ(read_reg32(ECX), 2);
    CHECK_EQ(cpu->instruction_pointer, 0x1010);
    write_reg32(ECX, 1);
    cpu->instruction_pointer = 0x1000;
    loop16(0x10);
    CHECK_EQ(read_reg32(ECX), 0);
    CHECK_EQ(cpu->instruction_pointer, 0x1000); /* no jump when the count hits zero */

    /* with a 16-bit address size the count is CX, and the surrounding ECX bits stay */
    cpu->prefixes = PREFIX_67;
    write_reg32(ECX, 0x12340002);
    cpu->instruction_pointer = 0x1000;
    loop16(0x10);
    CHECK_EQ(read_reg16(CX), 1);
    CHECK_EQ(read_reg32(ECX) & 0xFFFF0000, 0x12340000);
    cpu->prefixes = 0;

    /* loope16 requires ZF as well */
    cpu_state_init();
    data_segments();
    cpu->is_32 = true;
    write_reg32(ECX, 2);
    flags_add(0, 0, OPSIZE_32); /* ZF = 1 */
    cpu->instruction_pointer = 0x1000;
    loope16(0x10);
    CHECK(getzf());
    CHECK_EQ(cpu->instruction_pointer, 0x1010);
    write_reg32(ECX, 2);
    flags_add(1, 0, OPSIZE_32); /* ZF = 0 */
    cpu->instruction_pointer = 0x1000;
    loope16(0x10);
    CHECK_EQ(cpu->instruction_pointer, 0x1000);
    write_reg32(ECX, 2);
    loopne16(0x10);
    CHECK_EQ(cpu->instruction_pointer, 0x1010);

    /* jcxzxx looks at the address-sized CX only */
    cpu_state_init();
    data_segments();
    cpu->is_32 = true;
    cpu->prefixes = 0;
    write_reg32(ECX, 0x12340000);
    cpu->instruction_pointer = 0x1000;
    jcxz16(0x10);
    CHECK_EQ(cpu->instruction_pointer, 0x1000); /* ECX != 0: not taken */
    cpu->prefixes = PREFIX_67; /* 16-bit address size: the test uses CX only */
    write_reg32(ECX, 0x12340000);
    cpu->instruction_pointer = 0x1000;
    jcxz16(0x10);
    CHECK_EQ(cpu->instruction_pointer, 0x1010); /* CX == 0 -> taken */
    write_reg32(ECX, 0x12340001);
    cpu->instruction_pointer = 0x1000;
    jcxz16(0x10);
    CHECK_EQ(cpu->instruction_pointer, 0x1000); /* CX != 0 -> not taken */
    cpu->prefixes = 0;

    /* get_reg_asize / set_reg_asize */
    cpu_state_init();
    data_segments();
    cpu->is_32 = true;
    cpu->prefixes = PREFIX_67;
    write_reg32(ESI, 0x12345678);
    CHECK_EQ(get_reg_asize(ESI), 0x5678);
    set_reg_asize(false, ESI, 0xABCD);
    CHECK_EQ(read_reg32(ESI), 0x1234ABCD);
    set_reg_asize(true, ESI, 0xABCD);
    CHECK_EQ(read_reg32(ESI), 0x0000ABCD);

    /* setcc / cmov */
    cpu_state_init();
    data_segments();
    write_reg8(BL, 0x55);
    setcc_reg(true, BL);
    CHECK_EQ(read_reg8(BL), 1);
    setcc_reg(false, BL);
    CHECK_EQ(read_reg8(BL), 0);
    write8(0x2000, 0xAA);
    setcc_mem(true, 0x2000);
    CHECK_EQ(read8(0x2000), 1);
    write_reg16(AX, 0x1111);
    cmovcc16(false, 0x2222, AX);
    CHECK_EQ(read_reg16(AX), 0x1111);
    cmovcc16(true, 0x2222, AX);
    CHECK_EQ(read_reg16(AX), 0x2222);
    write_reg32(EBX, 0x11111111);
    cmovcc32(true, 0x22222222, EBX);
    CHECK_EQ(read_reg32(EBX), 0x22222222);

    /* xchg / bswap */
    cpu_state_init();
    write_reg8(CL, 0x11);
    CHECK_EQ(xchg8(0x22, CL), 0x11);
    CHECK_EQ(read_reg8(CL), 0x22);
    write_reg16(DX, 0x1234);
    CHECK_EQ(xchg16(0x5678, DX), 0x1234);
    CHECK_EQ(read_reg16(DX), 0x5678);
    write_reg16(AX, 0x1111);
    write_reg16(BX, 0x2222);
    xchg16r(BX);
    CHECK_EQ(read_reg16(AX), 0x2222);
    CHECK_EQ(read_reg16(BX), 0x1111);
    write_reg32(EAX, 0x11223344);
    write_reg32(ECX, 0x55667788);
    xchg32r(ECX);
    CHECK_EQ(read_reg32(EAX), 0x55667788);
    CHECK_EQ(read_reg32(ECX), 0x11223344);
    write_reg32(EDX, 0xDEADBEEF);
    CHECK_EQ(xchg32(0x0BADF00D, EDX), (int32_t)0xDEADBEEF);
    CHECK_EQ(read_reg32(EDX), 0x0BADF00D);
    write_reg32(ESI, 0x11223344);
    bswap(ESI);
    CHECK_EQ(read_reg32(ESI), 0x44332211);

    /* the condition predicates are the flag getters, spelled out */
    cpu_state_init();
    flags_sub(1, 2, OPSIZE_32); /* 1 - 2 == 0xFFFFFFFF: CF=1, ZF=0, SF=1, OF=0 */
    CHECK(test_b() && !test_z() && test_s() && !test_o());
    CHECK(test_be() && !test_nb() && !test_z());
    CHECK(test_l());  /* SF != OF */
    CHECK(test_le()); /* ... and ZF is not needed for it */
    CHECK(test_nl() == false && test_nle() == false);
    cpu_state_init();
    flags_sub(2, 2, OPSIZE_32); /* equal: ZF=1 */
    CHECK(test_z() && test_be() && test_le());
    cpu_state_init();
    flags_add(0x7F, 0x01, OPSIZE_8); /* signed overflow: OF=1, SF=1, ZF=0 */
    CHECK(test_o() && test_s() && test_l() == false); /* SF == OF here */
    CHECK(test_le() == false && test_nle());
    CHECK(test_no() == false && test_ns() == false);

    printf("PASS misc: jmp/jcc/loop/jcxz, setcc/cmov, xchg/bswap, condition predicates\n");
}

static void test_misc_task_switch(void)
{
    /* task_switch_test: CR0.EM or CR0.TS -> #nm, and no state change on the way out */
    cpu_state_init();
    cpu->cr[0] = 0;
    reset_exceptions();
    CHECK(task_switch_test());
    CHECK_EQ(exc_count, 0);

    cpu->cr[0] = CR0_EM;
    CHECK(!task_switch_test());
    CHECK_EQ(exc_count, 1);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_NM);

    cpu->cr[0] = CR0_TS;
    reset_exceptions();
    CHECK(!task_switch_test());
    CHECK_EQ(exc_vector, CPU_EXCEPTION_NM);

    cpu->cr[0] = CR0_EM | CR0_TS;
    reset_exceptions();
    CHECK(!task_switch_test());
    CHECK_EQ(exc_vector, CPU_EXCEPTION_NM);

    /* task_switch_test_mmx: EM -> #ud, TS -> #nm, neither -> allowed */
    cpu_state_init();
    cpu->cr[0] = 0;
    cpu->cr[4] = CR4_OSFXSR;
    reset_exceptions();
    CHECK(task_switch_test_mmx());
    CHECK_EQ(exc_count, 0);

    cpu->cr[0] = CR0_EM | CR0_TS; /* EM is tested FIRST */
    reset_exceptions();
    CHECK(!task_switch_test_mmx());
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);

    cpu->cr[0] = CR0_TS;
    reset_exceptions();
    CHECK(!task_switch_test_mmx());
    CHECK_EQ(exc_vector, CPU_EXCEPTION_NM);

    /* set_mxcsr stores the word (the four dbg_log warnings are not observable) */
    cpu_state_init();
    set_mxcsr(0x1F80 | MXCSR_FZ);
    CHECK_EQ(cpu->mxcsr, 0x1F80 | MXCSR_FZ);

    printf("PASS misc: task_switch_test/-_mmx (CR0.EM/TS -> #ud/#nm), set_mxcsr\n");
}


/* ================================================================== *
 * M1a-3: the string instructions (string.rs) and the base opcode table
 * (instructions.rs).
 *
 * Expected values are the semantic invariants of the cited upstream lines, not a
 * re-reading of the C: which SEGMENT supplies each operand, how far the iteration limit
 * runs in one call, what a rep-terminating comparison leaves in EFLAGS, and -- for the
 * slow paths -- that a fault leaves exactly the effects of the iterations that completed.
 * ================================================================== */

/* the flat fixture: paging off, real-mode segment offsets, 32-bit operand and address
 * size.  CS/DS/SS/ES carry DIFFERENT offsets on purpose (data_segments), so an operand
 * fetched through the wrong segment lands somewhere else and the test sees it. */
static void string_setup(void)
{
    cpu->cr[0] = RESET_CR0;
    full_clear_tlb();
    valid_tlb_entries_count = 0;
    cpu_state_init();
    data_segments(); /* CS=0, DS=0, SS=0x1000, ES=0x2000 */
    cpu->is_32 = true;
    cpu->stack_size_32 = true;
    cpu->cpl = 0;
    cpu->prefixes = 0;
    cpu->pagefault = false;
    cpu->instruction_pointer = 0x9000;
    cpu->previous_ip = 0x9000;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    zero_memory(0x1000, 0x8000);
    reset_stubs();
}

/* ------------------------------------------------------------------ *
 * gate 8 -- the rep/no-rep string instructions
 * ------------------------------------------------------------------ */

static void test_string_movs(void)
{
    string_setup();

    /* (a) rep movsb through the FAST path: ES:EDI is the destination and DS:ESI the
     * source (string.rs:82-97), the registers advance by the bytes moved (string.rs:493-506)
     * and ECX counts down to 0 (string.rs:508-513) */
    for (int i = 0; i < 8; i++) write8(0x1000 + i, 0xA0 + i);
    write_reg32(ESI, 0x1000);
    write_reg32(EDI, 0x3000);
    write_reg32(ECX, 8);
    cpu->flags &= ~FLAG_DIRECTION;
    movsb_rep(true, DS);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read_reg32(ESI), 0x1008);
    CHECK_EQ(read_reg32(EDI), 0x3008);
    CHECK_EQ(read_reg32(ECX), 0);
    for (int i = 0; i < 8; i++) CHECK_EQ(read8(0x5000 + i), 0xA0 + i); /* ES=0x2000 + EDI */
    /* the same address through DS would be 0x3000: the destination is ES, not DS */
    CHECK_EQ(read8(0x3000), 0);
    /* the whole rep fitted in the source and destination pages, so the instruction was not
     * rewound (string.rs:380-383) */
    CHECK_EQ(cpu->instruction_pointer, 0x9000);

    /* (b) the per-page iteration limit.  The destination is 0x10 bytes from the page end,
     * so this call moves 0x10 bytes and REWINDS instruction_pointer to previous_ip so the
     * dispatcher re-enters the instruction for the rest (string.rs:195-209, 380-383) */
    for (int i = 0; i < 0x20; i++) write8(0x1FF0 + i, 0x10 + i);
    write_reg32(ESI, 0x1FF0);
    write_reg32(EDI, 0x2FF0);
    write_reg32(ECX, 0x20);
    cpu->instruction_pointer = 0xDEAD;
    cpu->previous_ip = 0x1234;
    movsb_rep(true, DS);
    CHECK_EQ(read_reg32(ESI), 0x2000);
    CHECK_EQ(read_reg32(EDI), 0x3000);
    CHECK_EQ(read_reg32(ECX), 0x10);
    CHECK_EQ(cpu->instruction_pointer, 0x1234); /* rewound */
    CHECK_EQ(read8(0x4FF0), 0x10);              /* the first half landed ... */
    CHECK_EQ(read8(0x4FFF), 0x1F);
    CHECK_EQ(read8(0x5000), 0xA0);              /* ... and the second did not (case (a)) */

    /* the re-entry finishes it; nothing is left, so this call does NOT rewind */
    cpu->instruction_pointer = 0xDEAD;
    movsb_rep(true, DS);
    CHECK_EQ(read_reg32(ESI), 0x2010);
    CHECK_EQ(read_reg32(EDI), 0x3010);
    CHECK_EQ(read_reg32(ECX), 0);
    CHECK_EQ(cpu->instruction_pointer, 0xDEAD);
    for (int i = 0; i < 0x20; i++) CHECK_EQ(read8(0x4FF0 + i), 0x10 + i);

    /* (c) DF=1: ESI points at the byte moved FIRST and the pointers move DOWN, so the
     * range moved is [ESI-15, ESI] -> [EDI-15, EDI] (string.rs:110, 297-300, 385-386) */
    for (int i = 0; i < 0x11; i++) write8(0x1020 + i, 0x30 + i);
    uint8_t backwards[0x10];
    for (int i = 0; i < 0x10; i++) backwards[i] = (uint8_t)read8(0x1021 + i);
    write_reg32(ESI, 0x1030);
    write_reg32(EDI, 0x3030);
    write_reg32(ECX, 0x10);
    cpu->flags |= FLAG_DIRECTION;
    movsb_rep(true, DS);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read_reg32(ESI), 0x1020);
    CHECK_EQ(read_reg32(EDI), 0x3020);
    CHECK_EQ(read_reg32(ECX), 0);
    for (int i = 0; i < 0x10; i++) CHECK_EQ(read8(0x5021 + i), backwards[i]);
    CHECK_EQ(read8(0x5019), 0); /* nothing below the moved range */
    cpu->flags &= ~FLAG_DIRECTION;

    /* (d) a 16-bit address size disqualifies the fast path entirely (string.rs:146-151):
     * the SLOW path runs one iteration per loop with safe_read8/safe_write8, and the
     * write-back to ESI/EDI/ECX is 16 bits wide (string.rs:493-513) */
    for (int i = 0; i < 4; i++) write8(0x1040 + i, 0x60 + i);
    write_reg32(ESI, 0x1040);
    write_reg32(EDI, 0x3100);
    write_reg32(ECX, 4);
    movsb_rep(false, DS);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read_reg32(ESI), 0x1044);
    CHECK_EQ(read_reg32(EDI), 0x3104);
    CHECK_EQ(read_reg32(ECX), 0);
    for (int i = 0; i < 4; i++) CHECK_EQ(read8(0x5100 + i), 0x60 + i);

    /* ... and the 16-bit mask wraps the pointers (string.rs:444/449) */
    write8(0xFFFE, 0xEE);
    write8(0xFFFF, 0xFF);
    write_reg32(ESI, 0xFFFE);
    write_reg32(EDI, 0x3200);
    write_reg32(ECX, 2);
    movsb_rep(false, DS);
    CHECK_EQ(read_reg32(ESI), 0x0000); /* wrapped past 0xFFFF */
    CHECK_EQ(read_reg32(EDI), 0x3202);
    CHECK_EQ(read_reg32(ECX), 0);
    CHECK_EQ(read8(0x5200), 0xEE);
    CHECK_EQ(read8(0x5201), 0xFF);

    /* ... and the mask applies to the REGISTER READ, so the upper half of ESI/EDI must not
     * take part in the address at all (string.rs:73, 119, 128): 0x00110000 is address 0 in
     * a 16-bit address size, and 0x00113B00 is destination 0x3B00 */
    write8(0x0000, 0xC1);
    write8(0x110000, 0xC2);
    write_reg32(ESI, 0x00110000);
    write_reg32(EDI, 0x00113B00);
    write_reg32(ECX, 1);
    movsb_rep(false, DS);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read8(0x5B00), 0xC1);      /* ES:0x3B00 = 0x2000 + 0x3B00 */
    CHECK_EQ(read8(0x110000), 0xC2);    /* the unmasked destination was NOT written */
    CHECK_EQ(read_reg32(ESI), 0x00110001); /* only the 16-bit half is written back */
    CHECK_EQ(read_reg32(EDI), 0x00113B01);
    CHECK_EQ(read_reg32(ECX), 0);

    /* (e) rep movsb in a 32-bit address size stops at the SOURCE page end too: with the
     * source 8 bytes from its page end and the destination well inside its own page, only
     * 8 bytes move (string.rs:198-201 takes the min of both) */
    for (int i = 0; i < 0x20; i++) write8(0x1FF8 + i, 0x70 + i);
    write_reg32(ESI, 0x1FF8);
    write_reg32(EDI, 0x1000 + 0x200);
    write_reg32(ECX, 0x20);
    cpu->instruction_pointer = 0xDEAD;
    cpu->previous_ip = 0xBEEF;
    movsb_rep(true, DS);
    CHECK_EQ(read_reg32(ESI), 0x2000);
    CHECK_EQ(read_reg32(EDI), 0x1208); /* 0x1200 + 8, one byte per iteration */
    CHECK_EQ(read_reg32(ECX), 0x18);
    CHECK_EQ(cpu->instruction_pointer, 0xBEEF);

    printf("PASS string: rep movsb fast path, the per-page iteration limit + rewind, "
           "DF=1, the 16-bit slow path\n");
}


static void test_string_cmps_scas(void)
{
    string_setup();

    /* (a) rep cmps: the loop stops at the first DIFFERING pair and the final comparison is
     * the one whose flags survive (string.rs:353-374) */
    write8(0x1000, 1); write8(0x1001, 2); write8(0x1002, 3); write8(0x1003, 4);
    write8(0x1004, 5); write8(0x1005, 6); write8(0x1006, 7); write8(0x1007, 8);
    write8(0x5100, 1); write8(0x5101, 2); write8(0x5102, 3); write8(0x5103, 9);
    write8(0x5104, 5); write8(0x5105, 6); write8(0x5106, 7); write8(0x5107, 8);
    write_reg32(ESI, 0x1000);
    write_reg32(EDI, 0x3100); /* ES:0x3100 = physical 0x5100 */
    write_reg32(ECX, 8);
    cpu->flags &= ~FLAG_DIRECTION;
    cmpsb_repz(true, DS);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read_reg32(ESI), 0x1004); /* four pairs compared */
    CHECK_EQ(read_reg32(EDI), 0x3104);
    CHECK_EQ(read_reg32(ECX), 4);      /* 8 - 4 */
    CHECK(!getzf());                   /* 4 != 9 */
    CHECK(getcf());                    /* src - dst borrowed: 4 - 9 */
    CHECK(getsf());
    /* the DS-relative destination was not touched (the destination of cmps is ES) */
    CHECK_EQ(read8(0x3100), 0);

    /* ... and when every pair matches, the loop ends on count == 0 with ZF set by the
     * final comparison (string.rs:363-368) */
    write8(0x5110, 0x20); write8(0x5111, 0x20);
    write8(0x1010, 0x20); write8(0x1011, 0x20);
    write_reg32(ESI, 0x1010);
    write_reg32(EDI, 0x3110);
    write_reg32(ECX, 2);
    cmpsb_repz(true, DS);
    CHECK_EQ(read_reg32(ESI), 0x1012);
    CHECK_EQ(read_reg32(EDI), 0x3112);
    CHECK_EQ(read_reg32(ECX), 0);
    CHECK(getzf());
    CHECK(!getcf());

    /* (b) repnz cmps stops as soon as the pair MATCHES (string.rs:355-362) */
    write8(0x1020, 0x31); write8(0x1021, 0x31);
    write8(0x5120, 0x40); write8(0x5121, 0x31);
    write_reg32(ESI, 0x1020);
    write_reg32(EDI, 0x3120);
    write_reg32(ECX, 2);
    cmpsb_repnz(true, DS);
    CHECK_EQ(read_reg32(ESI), 0x1022);
    CHECK_EQ(read_reg32(EDI), 0x3122);
    CHECK_EQ(read_reg32(ECX), 0);
    CHECK(getzf()); /* the terminator: equal */

    /* (c) rep scas compares AL against ES:EDI (string.rs:112-115, 456-461) */
    write8(0x5200, 0x42); write8(0x5201, 0x42); write8(0x5202, 0x42);
    write_reg8(AL, 0x42);
    write_reg32(EDI, 0x3200);
    write_reg32(ECX, 3);
    scasb_repz(true);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read_reg32(EDI), 0x3203);
    CHECK_EQ(read_reg32(ECX), 0);
    CHECK(getzf());  /* three equal comparisons, then count == 0 */
    CHECK(!getcf());

    /* repnz stops on the first EQUAL pair (string.rs:355-362, 460-461): 0x42 != 0x01 is
     * "keep going", the 0x42 in the next byte is the terminator */
    write8(0x5210, 0x01); write8(0x5211, 0x42);
    write_reg8(AL, 0x42);
    write_reg32(EDI, 0x3210);
    write_reg32(ECX, 4);
    scasb_repnz(true);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read_reg32(EDI), 0x3212);
    CHECK_EQ(read_reg32(ECX), 2);
    CHECK(getzf());  /* 0x42 == 0x42 */
    CHECK(!getcf());

    /* (d) the no-rep forms do exactly one comparison and set the flags (string.rs:476) */
    string_setup();
    write8(0x1000, 0x10);
    write8(0x5100, 0x20);
    write_reg32(ESI, 0x1000);
    write_reg32(EDI, 0x3100);
    write_reg32(ECX, 0x7777); /* untouched: the rep count is not used without rep */
    cmpsb_no_rep(true, DS);
    CHECK_EQ(read_reg32(ESI), 0x1001);
    CHECK_EQ(read_reg32(EDI), 0x3101);
    CHECK_EQ(read_reg32(ECX), 0x7777);
    CHECK(!getzf());
    CHECK(getcf()); /* 0x10 - 0x20 borrows */

    write_reg8(AL, 0x30);
    write_reg32(EDI, 0x3100);
    scasb_no_rep(true);
    CHECK_EQ(read_reg32(EDI), 0x3101);
    CHECK(!getzf());
    CHECK(!getcf()); /* 0x30 - 0x20 */

    printf("PASS string: rep cmps/scas termination, the surviving comparison's flags, "
           "no-rep forms\n");
}

static void test_string_stos_lods(void)
{
    string_setup();

    /* rep stosb: a byte fill from AL, EDI advanced, ECX drained (string.rs:318-330) */
    write_reg8(AL, 0x41);
    write_reg32(EDI, 0x3300); /* ES:0x3300 = physical 0x5300 */
    write_reg32(ECX, 0x20);
    stosb_rep(true);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read_reg32(EDI), 0x3320);
    CHECK_EQ(read_reg32(ECX), 0);
    for (int i = 0; i < 0x20; i++) CHECK_EQ(read8(0x5300 + i), 0x41);

    /* rep stosd writes 32-bit units and stops at the page end: EDI = 0x3FFC leaves one
     * 4-byte unit in the page (string.rs:202-204 counts in units of the operand size) */
    write_reg32(EAX, 0x11223344);
    write_reg32(EDI, 0x3FFC);
    write_reg32(ECX, 4);
    cpu->instruction_pointer = 0xDEAD;
    cpu->previous_ip = 0xFEED;
    stosd_rep(true);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read_reg32(EDI), 0x4000);
    CHECK_EQ(read_reg32(ECX), 3);
    CHECK_EQ(read32s(0x5FFC), 0x11223344);
    CHECK_EQ(read32s(0x6000), 0);          /* the re-entry has not run */
    CHECK_EQ(cpu->instruction_pointer, 0xFEED);

    /* rep lodsw: the last loaded unit survives in AX (string.rs:286-290) */
    string_setup();
    write8(0x1000, 0xDE); write8(0x1001, 0xAD);
    write8(0x1002, 0xBE); write8(0x1003, 0xEF);
    write_reg32(ESI, 0x1000);
    write_reg32(ECX, 2);
    lodsw_rep(true, DS);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read_reg16(AX), 0xEFBE);
    CHECK_EQ(read_reg32(ESI), 0x1004);
    CHECK_EQ(read_reg32(ECX), 0);

    /* rep lodsb: AL holds the last byte, ESI the source end */
    write_reg32(ESI, 0x1000);
    write_reg32(ECX, 4);
    lodsb_rep(true, DS);
    CHECK_EQ(read_reg8(AL), 0xEF);
    CHECK_EQ(read_reg32(ESI), 0x1004);
    CHECK_EQ(read_reg32(ECX), 0);

    /* the no-rep forms advance by one unit and do not touch ECX */
    string_setup();
    write8(0x1000, 0x77);
    write_reg32(ESI, 0x1000);
    write_reg32(ECX, 0x1234);
    lodsb_no_rep(true, DS);
    CHECK_EQ(read_reg8(AL), 0x77);
    CHECK_EQ(read_reg32(ESI), 0x1001);
    CHECK_EQ(read_reg32(ECX), 0x1234);

    write_reg32(EDI, 0x3400);
    write_reg8(AL, 0x5A);
    stosw_no_rep(true);
    CHECK_EQ(read_reg32(EDI), 0x3402);
    CHECK_EQ(read16(0x5400), 0x005A);

    printf("PASS string: rep/no-rep stos and lods (page-bounded stosd, last-unit capture)\n");
}

static void test_string_ins_outs(void)
{
    string_setup();

    /* rep outsb: one port write per byte, from DS:ESI to DX (string.rs:281-284) */
    write8(0x1000, 0x11); write8(0x1001, 0x22); write8(0x1002, 0x33);
    write_reg32(ESI, 0x1000);
    write_reg32(ECX, 3);
    write_reg16(DX, 0x3F8);
    outsb_rep(true, DS);
    CHECK(!cpu->pagefault);
    CHECK_EQ(io_write_count, 3);
    CHECK_EQ(io_last_write_value, 0x33);
    CHECK_EQ(io_last_port, 0x3F8);
    CHECK_EQ(read_reg32(ESI), 0x1003);
    CHECK_EQ(read_reg32(ECX), 0);

    /* rep insb: DX is the port, ES:EDI the memory side (string.rs:266-270, 291-295) */
    reset_stubs();
    io_read_value = 0x77;
    write_reg32(EDI, 0x3500);
    write_reg32(ECX, 2);
    insb_rep(true);
    CHECK(!cpu->pagefault);
    CHECK_EQ(io_read_count, 2);
    CHECK_EQ(read_reg32(EDI), 0x3502);
    CHECK_EQ(read_reg32(ECX), 0);
    CHECK_EQ(read8(0x5500), 0x77);
    CHECK_EQ(read8(0x5501), 0x77);

    /* ins/outs refuse to run when the IOPL check fails: port untouched, registers too
     * (string.rs:132-141) */
    reset_stubs();
    io_privileges_ok = false;
    write_reg32(ESI, 0x1000);
    write_reg32(ECX, 4);
    outsb_rep(true, DS);
    CHECK_EQ(io_write_count, 0);
    CHECK_EQ(read_reg32(ESI), 0x1000);
    CHECK_EQ(read_reg32(ECX), 4);

    /* the IN/OUT instructions themselves (instr_E4/E6, EC/EE) */
    reset_stubs();
    io_read_value = 0xA5;
    write_reg8(AL, 0);
    instr_E4(0x60);
    CHECK_EQ(read_reg8(AL), 0xA5);
    CHECK_EQ(io_last_port, 0x60);
    write_reg16(DX, 0x3F9);
    instr32_ED();
    CHECK_EQ(io_last_port, 0x3F9);
    instr_E6(0x61);
    CHECK_EQ(io_write_count, 1);
    CHECK_EQ(io_last_port, 0x61);

    /* ... and the same instructions with the privilege check failing change nothing */
    reset_stubs();
    io_privileges_ok = false;
    write_reg8(AL, 0x5A);
    instr_E4(0x60);
    CHECK_EQ(read_reg8(AL), 0x5A);
    CHECK_EQ(io_read_count, 0);

    printf("PASS string: rep ins/outs through the port-I/O seat, the IOPL abort, "
           "IN/OUT instructions\n");
}


/* ------------------------------------------------------------------ *
 * gate 9 -- the base opcode table: a representative slice of instructions.c
 * ------------------------------------------------------------------ */

/* defined with gate 10 below; the RMW fault case needs it here */
static void paging_fixture_two_pages(void);

static void test_instr_alu_memory(void)
{
    string_setup();

    /* add r/m8, r8 (instr_00_mem): the memory operand is both read and written back */
    write8(0x2000, 0x10);
    write_reg8(CL, 5);
    instr_00_mem(0x2000, CL);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read8(0x2000), 0x15);
    CHECK(!getzf()); CHECK(!getpf()); CHECK(!getcf()); CHECK(!getsf()); /* 0x15: odd parity */

    /* add r8, r/m8 (instr_02_mem): the REGISTER operand is the destination and memory is
     * only read -- the distinction the 02/03 family exists for */
    write8(0x2004, 0x10);
    write_reg8(DL, 0x20);
    instr_02_mem(0x2004, DL);
    CHECK_EQ(read_reg8(DL), 0x30);
    CHECK_EQ(read8(0x2004), 0x10); /* untouched */
    instr_02_reg(CL, DL);          /* reg form: DL += CL */
    CHECK_EQ(read_reg8(DL), 0x35);

    /* 16-bit and 32-bit forms */
    write16(0x2010, 0x1234);
    write_reg16(CX, 0x0001);
    instr16_01_mem(0x2010, CX);
    CHECK_EQ(read16(0x2010), 0x1235);
    write32(0x2014, 0xFFFFFFFE);
    write_reg32(EDX, 3);
    instr32_01_mem(0x2014, EDX);
    CHECK_EQ(read32s(0x2014), 1);
    CHECK(getcf()); /* 0xFFFFFFFE + 3 carries out */

    /* the immediate groups: 80 /0 (add r/m8, imm8) with a wraparound and its flags */
    write8(0x2020, 0x81);
    instr_80_0_mem(0x2020, 0x7F);
    CHECK_EQ(read8(0x2020), 0x00);
    CHECK(getcf()); CHECK(getzf()); CHECK(getaf()); CHECK(!getsf());

    /* 81 /5 (sub r/m16, imm16) and 83 /0 (the sign-extended imm8 form: the 16-bit form
     * masks the immediate, string.rs has no analogue -- instructions.rs:749) */
    write16(0x2024, 0x0001);
    instr16_81_5_mem(0x2024, 0x0002);
    CHECK_EQ(read16(0x2024), 0xFFFF);
    CHECK(getcf()); CHECK(getsf());
    write16(0x2028, 0x0000);
    instr16_83_0_mem(0x2028, -1); /* the immediate is masked with 0xFFFF */
    CHECK_EQ(read16(0x2028), 0xFFFF);
    write32(0x202C, 0x00000000);
    instr32_83_0_mem(0x202C, -1);
    CHECK_EQ(read32s(0x202C), -1);

    /* 84 / 85: test r/m, r sets the flags without writing memory */
    write8(0x2030, 0x0F);
    write_reg8(CL, 0xF0);
    instr_84_mem(0x2030, CL);
    CHECK_EQ(read8(0x2030), 0x0F);
    CHECK(getzf());

    /* F6 /2 and /3: not and neg through the read-modify-write path */
    write8(0x2034, 0x0F);
    instr_F6_2_mem(0x2034);
    CHECK_EQ(read8(0x2034), 0xF0); /* Rust's `!x & 0xFF` is the BITWISE complement */
    write8(0x2035, 0x01);
    instr_F6_3_mem(0x2035);
    CHECK_EQ(read8(0x2035), 0xFF);
    CHECK(getcf()); /* neg sets CF unless the operand was 0 */

    /* F6 /4 /6: mul and div use AX and write both halves */
    write_reg8(AL, 0x10);
    write8(0x2036, 0x10);
    instr_F6_4_mem(0x2036);
    CHECK_EQ(read_reg16(AX), 0x0100);
    CHECK(getcf()); /* the high half is non-zero -> CF = OF = 1 */
    write_reg16(AX, 0x0011);
    write8(0x2037, 4);
    instr_F6_6_mem(0x2037);
    CHECK_EQ(read_reg8(AL), 4);
    CHECK_EQ(read_reg8(AH), 1);

    /* F7 /2 /3 /4 /6 on 32 bits */
    write32(0x2040, 0x0000000F);
    instr32_F7_2_mem(0x2040);
    CHECK_EQ(read32s(0x2040), (int32_t)0xFFFFFFF0);
    write32(0x2044, 0x00000001);
    instr32_F7_3_mem(0x2044);
    CHECK_EQ(read32s(0x2044), -1);
    write_reg32(EAX, 0x00010000);
    write32(0x2048, 0x00010000);
    instr32_F7_4_mem(0x2048);
    CHECK_EQ(read_reg32(EAX), 0);
    CHECK_EQ(read_reg32(EDX), 1);
    write_reg32(EAX, 0x00000010);
    write_reg32(EDX, 0);
    write32(0x204C, 3);
    instr32_F7_6_mem(0x204C);
    CHECK_EQ(read_reg32(EAX), 5); /* 0x10 / 3 */
    CHECK_EQ(read_reg32(EDX), 1); /* remainder */

    /* the hoisted-read forms of the reversed family (12/1A/2B/3B write a REGISTER from
     * memory, so the fault check sits between the read and the write) and the cmp family,
     * whose operand ORDER is observable only in the flags: 38/39 compare mem - reg, 3A/3B
     * compare reg - mem, so a swap inverts CF */
    write8(0x2070, 0x01);
    write_reg8(DL, 0x00);
    cpu->flags = FLAGS_DEFAULT | FLAG_CARRY; /* CF = 1: adc must include it */
    cpu->flags_changed = 0;
    instr_12_mem(0x2070, DL); /* adc dl, [0x2070]: 0 + 1 + CF */
    CHECK_EQ(read_reg8(DL), 0x02);
    CHECK_EQ(read8(0x2070), 0x01); /* memory is only read */
    instr32_2B_mem(0x2074, EDX); /* sub edx, [0x2074] */
    write32(0x2074, 0x00000005);
    write_reg32(EDX, 0x00000003);
    instr32_2B_mem(0x2074, EDX);
    CHECK_EQ(read_reg32(EDX), (int32_t)0xFFFFFFFE);
    CHECK(getcf()); /* 3 - 5 borrows */
    write16(0x2078, 0x0001);
    write_reg16(CX, 0x0002);
    instr16_39_mem(0x2078, CX); /* cmp [0x2078], cx -> 1 - 2 */
    CHECK(getcf());
    CHECK(!getzf());
    instr16_3B_mem(0x2078, CX); /* cmp cx, [0x2078] -> 2 - 1 */
    CHECK(!getcf());
    CHECK(!getzf());
    write8(0x207C, 0x10);
    write_reg8(CL, 0x20);
    instr_38_mem(0x207C, CL); /* mem - reg = 0x10 - 0x20 */
    CHECK(getcf());
    instr_3A_mem(0x207C, CL); /* reg - mem = 0x20 - 0x10 */
    CHECK(!getcf());

    /* the imul-with-immediate forms read memory once and write the register (69/6B) */
    write16(0x2080, 0x0003);
    write_reg16(CX, 0x0004);
    instr16_69_mem(0x2080, CX, 5); /* cx = 3 * 5 */
    CHECK_EQ(read_reg16(CX), 15);
    write32(0x2084, 0x00000007);
    instr32_6B_mem(0x2084, EDX, -2); /* edx = 7 * -2 */
    CHECK_EQ(read_reg32(EDX), -14);
    instr32_6B_reg(ECX, EDX, 3);
    /* ... and a faulting read leaves the destination register alone */
    paging_fixture_two_pages();
    write_reg32(EDX, 0x12345678);
    instr32_69_mem(0x402000, EDX, 4); /* virtual 0x402000 is unmapped by default */
    CHECK(cpu->pagefault);
    CHECK_EQ(read_reg32(EDX), 0x12345678);
    string_setup();

    /* the shift/rotate immediate groups take the same read-modify-write path */
    write8(0x2050, 0x81);
    instr_C0_0_mem(0x2050, 1); /* rol8 by 1 */
    CHECK_EQ(read8(0x2050), 0x03);
    CHECK(getcf()); /* the rotated-out MSB lands in CF */
    write16(0x2054, 0x8001);
    instr16_D1_7_mem(0x2054); /* sar16 by 1 */
    CHECK_EQ(read16(0x2054), 0xC000);
    write_reg8(CL, 2);
    write32(0x2058, 0x0000000F);
    instr32_D3_4_mem(0x2058); /* shl32 by CL */
    CHECK_EQ(read32s(0x2058), 0x0000003C);

    /* inc/dec preserve CF (40/48 and the FE/FF memory forms) */
    cpu->flags |= FLAG_CARRY;
    cpu->flags_changed &= ~FLAG_CARRY;
    write_reg32(EAX, 0xFFFFFFFF);
    instr32_40();
    CHECK_EQ(read_reg32(EAX), 0);
    CHECK(getcf());
    write16(0x2060, 0xFFFF);
    instr16_FF_0_mem(0x2060);
    CHECK_EQ(read16(0x2060), 0);
    CHECK(getcf());
    write8(0x2062, 0x00);
    instr_FE_1_mem(0x2062);
    CHECK_EQ(read8(0x2062), 0xFF);
    CHECK(getcf());

    printf("PASS instr: ALU memory forms, the immediate groups, not/neg/mul/div, "
           "shift groups, inc/dec CF\n");
}

static void test_instr_mov_xchg_lea(void)
{
    string_setup();

    /* xchg r/m8, r8 (instr_86_mem): BOTH directions are written -- the memory gets the
     * register's old value and the register gets the memory's */
    write8(0x2100, 0x11);
    write_reg8(CL, 0x22);
    instr_86_mem(0x2100, CL);
    CHECK_EQ(read8(0x2100), 0x22);
    CHECK_EQ(read_reg8(CL), 0x11);
    write16(0x2102, 0x3344);
    write_reg16(DX, 0x5566);
    instr16_87_mem(0x2102, DX);
    CHECK_EQ(read16(0x2102), 0x5566);
    CHECK_EQ(read_reg16(DX), 0x3344);

    /* mov r/m, r and mov r, r/m */
    write_reg8(DL, 0x7A);
    instr_88_mem(0x2110, DL);
    CHECK_EQ(read8(0x2110), 0x7A);
    write32(0x2114, 0xDEADBEEF);
    instr32_8B_mem(0x2114, ECX);
    CHECK_EQ(read_reg32(ECX), (int32_t)0xDEADBEEF);
    write_reg16(CX, 0xABCD);
    instr16_89_mem(0x2118, CX);
    CHECK_EQ(read16(0x2118), 0xABCD);
    write8(0x211A, 0x5A);
    instr_8A_mem(0x211A, CL);
    CHECK_EQ(read_reg8(CL), 0x5A);

    /* mov r/m16, sreg (8C): the SELECTOR is stored (not the offset), and a reg field >= 6
     * is a #ud */
    reset_stubs();
    cpu->sreg[ES] = 0x1234;
    instr16_8C_reg(EAX, ES);
    CHECK_EQ(read_reg16(AX), 0x1234);
    instr16_8C_mem(0x2120, ES);
    CHECK_EQ(read16(0x2120), 0x1234);
    instr32_8C_mem(0x2124, ES);
    CHECK_EQ(read16(0x2124), 0x1234); /* always a 16-bit store */
    CHECK_EQ(read16(0x2126), 0);      /* ... exactly two bytes */
    instr16_8C_reg(EAX, 6);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);

    /* mov sreg, r/m16 (8E) goes through switch_seg with the SELECTOR */
    reset_stubs();
    write16(0x2130, 0x0033);
    instr_8E_mem(0x2130, DS);
    CHECK_EQ(switch_seg_calls, 1);
    CHECK_EQ(switch_seg_last_reg, DS);
    CHECK_EQ(switch_seg_last_selector, 0x33);
    reset_stubs();
    instr_8E_mem(0x2130, TR); /* not one of ES/SS/DS/FS/GS */
    CHECK_EQ(switch_seg_calls, 0);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);

    /* lea (8D): the SEGMENT PREFIX ZERO makes modrm_resolve return the offset WITHOUT the
     * segment base -- with DS != 0 the two answers differ, which is the whole point of the
     * SEG_PREFIX_ZERO barrier (instructions.rs:901-903) */
    cpu->segment_offsets[DS] = 0x10000;
    write_reg32(EBX, 0x2100);
    write_reg32(EAX, 0);
    instr32_8D_mem(0x03, EAX); /* modrm 0x03 = [EBX] */
    CHECK_EQ(read_reg32(EAX), 0x2100); /* the offset, not 0x12100 */
    CHECK(!cpu->pagefault);
    CHECK_EQ(cpu->prefixes, 0); /* the prefix bit is restored */
    cpu->segment_offsets[DS] = 0;
    /* a reg-form lea is #ud */
    reset_stubs();
    instr16_8D_reg(EAX, EBX);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);

    /* pop r/m16 (8F /0): ESP is adjusted BEFORE the address is resolved and restored when
     * the resolve faults, then the popped word is stored through the address */
    string_setup();
    write_reg32(EBX, 0x2200);
    write_reg32(ESP, 0x1000); /* SS=0x1000 -> stack at 0x2000 */
    write16(0x2000, 0xBEEF);
    instr16_8F_0_mem(0x03); /* [EBX] */
    CHECK(!cpu->pagefault);
    CHECK_EQ(read16(0x2200), 0xBEEF);
    CHECK_EQ(read_reg32(ESP), 0x1002);
    /* the reg form is a plain pop */
    write_reg32(ESP, 0x1000);
    write16(0x2000, 0x1234);
    instr16_8F_0_reg(CX);
    CHECK_EQ(read_reg16(CX), 0x1234);
    CHECK_EQ(read_reg32(ESP), 0x1002);

    printf("PASS instr: xchg both directions, mov r/m<->reg, mov sreg both ways, "
           "lea's offset-only address, pop r/m16\n");
}

static void test_instr_control(void)
{
    string_setup();
    cpu->instruction_pointer = 0x8100;

    /* call rel16 (E8): pushes the return offset and jumps within the CS offset */
    write_reg32(ESP, 0x1000);
    instr16_E8(0x10);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read_reg32(ESP), 0x0FFE); /* push16 on a 32-bit stack moves SP by 2 */
    CHECK_EQ(read16(0x1FFE), 0x8100);  /* SS:SP-2 */
    CHECK_EQ(cpu->instruction_pointer, 0x8110);

    /* call rel32 (E8): pushes the full eip and adds the displacement */
    string_setup();
    cpu->instruction_pointer = 0x8100;
    write_reg32(ESP, 0x1000);
    instr32_E8(0x100);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read_reg32(ESP), 0x0FFC);
    CHECK_EQ(read32s(0x1FFC), 0x8100);
    CHECK_EQ(cpu->instruction_pointer, 0x8200);

    /* jmp rel (E9/EB) does not touch the stack */
    string_setup();
    cpu->instruction_pointer = 0x8300;
    instr16_E9(0x20);
    CHECK_EQ(cpu->instruction_pointer, 0x8320);
    instr32_EB(0x30);
    CHECK_EQ(cpu->instruction_pointer, 0x8350);

    /* ret (C3) takes the target from the top of the stack */
    string_setup();
    write_reg32(ESP, 0x1000);
    write32(0x2000, 0x8400);
    instr32_C3();
    CHECK(!cpu->pagefault);
    CHECK_EQ(cpu->instruction_pointer, 0x8400);
    CHECK_EQ(read_reg32(ESP), 0x1004);
    /* ret imm16 (C2) additionally releases the arguments */
    write_reg32(ESP, 0x1000);
    write32(0x2000, 0x8500);
    instr32_C2(8);
    CHECK_EQ(cpu->instruction_pointer, 0x8500);
    CHECK_EQ(read_reg32(ESP), 0x100C);

    /* retf (CA/CB) hands ip/selector to M1c's far_return, with the stack adjustment */
    reset_stubs();
    write_reg32(ESP, 0x1000);
    write16(0x2000, 0x1234);
    write16(0x2002, 0x0058);
    instr16_CA(4);
    CHECK(!cpu->pagefault);
    CHECK_EQ(far_return_calls, 1);
    CHECK_EQ(far_return_last_eip, 0x1234);
    CHECK_EQ(far_return_last_selector, 0x58);
    CHECK_EQ(far_return_last_adjust, 4);
    CHECK(!far_return_last_is_osize_32);
    instr32_CB();
    CHECK_EQ(far_return_calls, 2);
    CHECK_EQ(far_return_last_adjust, 0);
    CHECK(far_return_last_is_osize_32);

    /* far call/jmp (9A/EA and the FF /3 and /5 memory forms) */
    reset_stubs();
    instr16_9A(0x8600, 0x0060);
    CHECK_EQ(far_jump_calls, 1);
    CHECK_EQ(far_jump_last_eip, 0x8600);
    CHECK_EQ(far_jump_last_selector, 0x60);
    CHECK(far_jump_last_is_call);
    CHECK(!far_jump_last_is_osize_32);
    instr32_EA(0x8700, 0x0068);
    CHECK_EQ(far_jump_calls, 2);
    CHECK(!far_jump_last_is_call);
    CHECK(far_jump_last_is_osize_32);

    reset_stubs();
    write16(0x2200, 0x8800);
    write16(0x2202, 0x0070);
    instr16_FF_3_mem(0x2200);
    CHECK_EQ(far_jump_calls, 1);
    CHECK_EQ(far_jump_last_eip, 0x8800);
    CHECK_EQ(far_jump_last_selector, 0x70);
    CHECK(far_jump_last_is_call);
    write32(0x2210, 0x8900);
    write16(0x2214, 0x0078);
    instr32_FF_5_mem(0x2210);
    CHECK_EQ(far_jump_calls, 2);
    CHECK(!far_jump_last_is_call);
    CHECK(far_jump_last_is_osize_32);
    reset_stubs();
    instr16_FF_3_reg(EAX); /* the register form of a far call is #ud */
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);

    /* call/jmp near through the FF group: a push (for the call) and ip = cs + data */
    string_setup();
    cpu->instruction_pointer = 0x8A00;
    write_reg32(ESP, 0x1000);
    write_reg32(ECX, 0x8B00);
    instr32_FF_2_reg(ECX);
    CHECK(!cpu->pagefault);
    CHECK_EQ(cpu->instruction_pointer, 0x8B00);
    CHECK_EQ(read_reg32(ESP), 0x0FFC);
    CHECK_EQ(read32s(0x1FFC), 0x8A00);
    write32(0x2220, 0x8C00);
    instr32_FF_4_mem(0x2220);
    CHECK_EQ(cpu->instruction_pointer, 0x8C00);
    CHECK_EQ(read_reg32(ESP), 0x0FFC); /* a jmp pushes nothing */

    /* push r/m16 (FF /6) reads the memory operand and pushes it */
    string_setup();
    write_reg32(ESP, 0x1000);
    write16(0x2230, 0xCAFE);
    instr16_FF_6_mem(0x2230);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read_reg32(ESP), 0x0FFE);
    CHECK_EQ(read16(0x1FFE), 0xCAFE);

    /* int3 / int imm8 / into deliver through call_interrupt_vector with NO error code
     * (upstream's `None`, cpu.rs:2393-2426) */
    reset_stubs();
    instr_CC();
    CHECK_EQ(delivery_count, 1);
    CHECK_EQ(delivered_interrupt, 3);
    CHECK(delivered_is_int);
    CHECK(!delivered_has_error_code);
    instr_CD(0x21);
    CHECK_EQ(delivery_count, 2);
    CHECK_EQ(delivered_interrupt, 0x21);
    CHECK(delivered_is_int);
    CHECK(!delivered_has_error_code);
    reset_stubs();
    cpu->flags &= ~FLAG_OVERFLOW;
    cpu->flags_changed &= ~FLAG_OVERFLOW;
    instr_CE();
    CHECK_EQ(delivery_count, 0); /* OF clear: INTO does nothing */
    cpu->flags |= FLAG_OVERFLOW;
    instr_CE();
    CHECK_EQ(delivery_count, 1);
    CHECK_EQ(delivered_interrupt, CPU_EXCEPTION_OF);
    CHECK(!delivered_has_error_code);

    /* iret hands over to M1c */
    reset_stubs();
    instr16_CF();
    CHECK_EQ(iret16_calls, 1);
    instr32_CF();
    CHECK_EQ(iret32_calls, 1);

    /* the 0F escape re-dispatches to the 0F table's own entry points */
    reset_stubs();
    write8(0x9030, 0xB6);
    cpu->instruction_pointer = 0x9030;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    cpu->pagefault = false;
    instr16_0F();
    CHECK_EQ(dispatch0f_calls, 1);
    CHECK_EQ(dispatch0f_last_opcode, 0xB6); /* the wrapper's `| 0x100` is its owner's */
    CHECK(!dispatch0f_last_is_32);
    write8(0x9034, 0xB7);
    cpu->instruction_pointer = 0x9034;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    cpu->pagefault = false;
    instr32_0F();
    CHECK_EQ(dispatch0f_calls, 2);
    CHECK_EQ(dispatch0f_last_opcode, 0xB7);
    CHECK(dispatch0f_last_is_32);

    printf("PASS instr: call/jmp/ret/retf, the FF group, int3/int/into with no error code, "
           "iret, the 0F escape\n");
}


static void test_instr_flags_and_prefixes(void)
{
    string_setup();

    /* 9B (fwait): CR0.MP|CR0.TS together raise #NM (unlike task_switch_test, the EM bit is
     * ignored); otherwise the FPU unit's fwait runs (instructions.rs:1009-1018) */
    reset_stubs();
    cpu->cr[0] = CR0_MP | CR0_TS;
    instr_9B();
    CHECK_EQ(exc_vector, CPU_EXCEPTION_NM);
    CHECK_EQ(fwait_calls, 0);
    reset_stubs();
    cpu->cr[0] = RESET_CR0;
    instr_9B();
    CHECK_EQ(fwait_calls, 1);
    CHECK_EQ(exc_count, 0);

    /* pushf/popf (9C/9D) round-trip the 16-bit eflags image through the stack */
    string_setup();
    write_reg32(ESP, 0x1000);
    cpu->flags = FLAGS_DEFAULT | FLAG_CARRY;
    cpu->flags_changed = 0;
    int32_t pushed = get_eflags() & 0xFFFF;
    instr16_9C();
    CHECK(!cpu->pagefault);
    CHECK_EQ(read_reg32(ESP), 0x0FFE); /* push16 on a 32-bit stack: 2 bytes, SP - 2 */
    CHECK_EQ(read16(0x1FFE), pushed);
    cpu->flags = FLAGS_DEFAULT | FLAG_ZERO;
    cpu->flags_changed = 0;
    instr16_9D();
    CHECK(!cpu->pagefault);
    CHECK_EQ(read_reg32(ESP), 0x1000);
    CHECK(getcf());
    CHECK(!getzf());

    /* popf with VM set and IOPL < 3 is a #gp and pops nothing (instructions.rs:1046-1071) */
    reset_stubs();
    write_reg32(ESP, 0x1000);
    write16(0x2000, 0x0006);
    cpu->protected_mode = true;
    cpu->cpl = 3;
    cpu->flags = FLAG_VM | FLAGS_DEFAULT; /* IOPL = 0 */
    cpu->flags_changed = 0;
    instr16_9D();
    CHECK_EQ(exc_vector, CPU_EXCEPTION_GP);
    CHECK_EQ(read_reg32(ESP), 0x1000);
    CHECK_EQ(cpu->flags, FLAG_VM | FLAGS_DEFAULT);

    /* the 32-bit pushf clears VM and RF in the stored image (instructions.rs:1032-1044) */
    string_setup();
    write_reg32(ESP, 0x1000);
    /* VM set is only not-a-#gp when IOPL == 3 (instr_pushf_popf_check), which is exactly
     * the privileged case where the stored image's VM/RF clearing is observable */
    cpu->flags = FLAGS_DEFAULT | FLAG_VM | FLAG_RF | FLAG_CARRY | (3 << 12);
    cpu->flags_changed = 0;
    cpu->protected_mode = true;
    int32_t full = get_eflags();
    instr32_9C();
    CHECK(!cpu->pagefault);
    CHECK_EQ(read32s(0x1FFC), full & 0xFCFFFF);
    CHECK_EQ(read32s(0x1FFC) & FLAG_VM, 0);
    CHECK_EQ(read32s(0x1FFC) & FLAG_RF, 0);

    /* sahf/lahf (9E/9F) */
    string_setup();
    write_reg8(AH, 0xD7); /* bits 0,2,4,6,7 -> CF PF AF ZF SF */
    instr_9E();
    CHECK(getcf());
    CHECK(getpf());
    CHECK(getaf());
    CHECK(getzf());
    CHECK(getsf());
    instr_9F();
    CHECK_EQ(read_reg8(AH), 0xD7 & 0xFF);

    /* clc/stc/cmc and cld/std (F8/F9/F5/FC/FD) -- the flag and its "changed" bit */
    string_setup();
    cpu->flags = FLAGS_DEFAULT;
    cpu->flags_changed = 0;
    instr_F9();
    CHECK(getcf());
    CHECK_EQ(cpu->flags_changed & 1, 0);
    instr_F5();
    CHECK(!getcf());
    instr_F5();
    CHECK(getcf());
    instr_F8();
    CHECK(!getcf());
    instr_FD();
    CHECK((cpu->flags & FLAG_DIRECTION) != 0);
    instr_FC();
    CHECK((cpu->flags & FLAG_DIRECTION) == 0);

    /* cli/sti in real mode at cpl 0 */
    string_setup();
    cpu->flags = FLAGS_DEFAULT | FLAG_INTERRUPT;
    cpu->flags_changed = 0;
    instr_FA();
    CHECK((cpu->flags & FLAG_INTERRUPT) == 0);
    CHECK_EQ(exc_count, 0);

    /* sti re-dispatches the NEXT instruction immediately (the interrupt shadow) and polls
     * the PIC at the end (instructions.rs:2354-2367).  `prefixes = 0` runs BEFORE the
     * opcode fetch, so the 66 prefix in flight is gone and the operand size is 32-bit --
     * observable as bit 8 of the argument. */
    reset_stubs();
    write8(0x9040, 0x90);
    cpu->instruction_pointer = 0x9040;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    cpu->flags = FLAGS_DEFAULT;
    cpu->flags_changed = 0;
    cpu->prefixes = PREFIX_MASK_OPSIZE;
    instr_FB();
    CHECK((cpu->flags & FLAG_INTERRUPT) != 0);
    CHECK_EQ(dispatch_calls, 1);
    CHECK_EQ(dispatch_last_opcode, 0x190); /* 0x90 | is_osize_32() << 8 */
    CHECK_EQ(dispatch_last_prefixes, 0);
    CHECK_EQ(handle_irqs_calls, 1);
    CHECK_EQ(cpu->instruction_pointer, 0x9041);

    /* ... and with IOPL < cpl it is a #gp instead (instructions.rs:2296-2367) */
    reset_stubs();
    cpu->protected_mode = true;
    cpu->cpl = 3;
    cpu->flags = FLAGS_DEFAULT; /* IOPL = 0 */
    cpu->flags_changed = 0;
    cpu->prefixes = 0;
    instr_FA();
    CHECK_EQ(exc_vector, CPU_EXCEPTION_GP);
    CHECK((cpu->flags & FLAG_INTERRUPT) == 0);
    reset_stubs();
    instr_FB();
    CHECK_EQ(exc_vector, CPU_EXCEPTION_GP);
    CHECK((cpu->flags & FLAG_INTERRUPT) == 0);
    CHECK_EQ(dispatch_calls, 0);
    CHECK_EQ(handle_irqs_calls, 0);

    /* hlt (F4): in_hlt, then either the timer poll (IF set) or the halt event */
    string_setup();
    cpu->flags = FLAGS_DEFAULT | FLAG_INTERRUPT;
    cpu->flags_changed = 0;
    instr_F4();
    CHECK(cpu->in_hlt);
    CHECK_EQ(microtick_calls, 1);
    CHECK_EQ(run_hardware_timers_calls, 1);
    CHECK_EQ(handle_irqs_calls, 1);
    CHECK_EQ(cpu_event_halt_calls, 0);
    string_setup();
    cpu->flags = FLAGS_DEFAULT;
    cpu->flags_changed = 0;
    instr_F4();
    CHECK(cpu->in_hlt);
    CHECK_EQ(cpu_event_halt_calls, 1);
    CHECK_EQ(handle_irqs_calls, 0);
    reset_stubs();
    cpu->cpl = 3;
    instr_F4();
    CHECK_EQ(exc_vector, CPU_EXCEPTION_GP);

    /* aam / aad with an immediate (D4/D5) */
    string_setup();
    write_reg8(AL, 0x0B);
    instr_D4(10); /* aam 10: AH = AL / 10, AL = AL % 10 */
    CHECK_EQ(read_reg8(AL), 1);
    CHECK_EQ(read_reg8(AH), 1);
    write_reg8(AL, 0x01);
    write_reg8(AH, 0x02);
    instr_D5(10); /* aad 10: AL = AL + AH * 10 */
    CHECK_EQ(read_reg8(AL), 21);
    CHECK_EQ(read_reg8(AH), 0);

    /* salc (D6): AL = 0xFF with CF set, 0x00 with CF clear */
    cpu->flags = FLAGS_DEFAULT | FLAG_CARRY;
    cpu->flags_changed = 0;
    instr_D6();
    CHECK_EQ(read_reg8(AL), 0xFF);
    cpu->flags = FLAGS_DEFAULT;
    cpu->flags_changed = 0;
    instr_D6();
    CHECK_EQ(read_reg8(AL), 0x00);

    /* xlat (D7): AL = DS:[EBX + AL] in a 32-bit address size ... */
    string_setup();
    write_reg32(EBX, 0x2400);
    write_reg8(AL, 3);
    write8(0x2403, 0x9C);
    instr_D7();
    CHECK(!cpu->pagefault);
    CHECK_EQ(read_reg8(AL), 0x9C);
    /* ... and DS:[(BX + AL) & 0xFFFF] in a 16-bit one */
    cpu->is_32 = false;
    write_reg16(BX, 0xFFFF);
    write_reg8(AL, 2);
    write8(0x0001, 0x5A);
    instr_D7();
    CHECK_EQ(read_reg8(AL), 0x5A);
    cpu->is_32 = true;

    /* cbw/cwde and cwd/cdq (98/99): the sign-extension pair */
    string_setup();
    write_reg8(AL, 0x80);
    instr16_98();
    CHECK_EQ(read_reg16(AX), 0xFF80);
    write_reg16(AX, 0x8000);
    instr32_98();
    CHECK_EQ(read_reg32(EAX), (int32_t)0xFFFF8000);
    write_reg16(AX, 0x8000);
    instr16_99();
    CHECK_EQ(read_reg16(DX), 0xFFFF);
    write_reg16(AX, 0x7FFF);
    instr16_99();
    CHECK_EQ(read_reg16(DX), 0x0000);
    write_reg32(EAX, (int32_t)0x80000000);
    instr32_99();
    CHECK_EQ(read_reg32(EDX), -1);

    /* test r/m, imm (A8/A9) and inc/dec (40/48) */
    string_setup();
    write_reg8(AL, 0x0F);
    instr_A8(0xF0);
    CHECK(getzf());
    CHECK(!getcf());
    write_reg32(EAX, 0xFFFFFFFF);
    instr32_40();
    CHECK_EQ(read_reg32(EAX), 0);
    instr32_48();
    CHECK_EQ(read_reg32(EAX), -1);

    /* the conditional jumps (70-7F) */
    string_setup();
    cpu->instruction_pointer = 0x8E00;
    cpu->flags = FLAGS_DEFAULT | FLAG_OVERFLOW;
    cpu->flags_changed = 0; /* OF not stale -> the getter reads `flags` */
    instr16_70(0x10); /* jo, taken: 0x8E00 + 0x10 */
    CHECK_EQ(cpu->instruction_pointer, 0x8E10);
    cpu->flags = FLAGS_DEFAULT; /* OF now clear */
    instr16_70(0x20); /* jo, not taken */
    CHECK_EQ(cpu->instruction_pointer, 0x8E10);
    instr32_75(0x40); /* jnz, taken (ZF clear): 0x8E10 + 0x40 */
    CHECK_EQ(cpu->instruction_pointer, 0x8E50);
    cpu->flags = FLAGS_DEFAULT | FLAG_ZERO;
    instr32_75(0x40); /* jnz, not taken */
    CHECK_EQ(cpu->instruction_pointer, 0x8E50);

    /* the loop family (E0-E3) and its ECX handling */
    string_setup();
    cpu->instruction_pointer = 0x8F00;
    write_reg32(ECX, 2);
    instr16_E2(4); /* loop: ECX 2 -> 1, taken */
    CHECK_EQ(read_reg32(ECX), 1);
    CHECK_EQ(cpu->instruction_pointer, 0x8F04);
    instr16_E2(4); /* ECX 1 -> 0, not taken */
    CHECK_EQ(read_reg32(ECX), 0);
    CHECK_EQ(cpu->instruction_pointer, 0x8F04);
    instr16_E3(8); /* jcxz: CX == 0, taken */
    CHECK_EQ(cpu->instruction_pointer, 0x8F0C);
    write_reg32(ECX, 1);
    instr16_E3(8); /* not taken */
    CHECK_EQ(cpu->instruction_pointer, 0x8F0C);

    printf("PASS instr: fwait/#NM, pushf/popf (and its #gp), sahf/lahf, the flag ops, "
           "cli/sti (+ the shadow re-dispatch), hlt, aam/aad/salc/xlat, cbw/cwd, "
           "jcc/loop\n");
}

static void test_instr_string_opcodes(void)
{
    string_setup();

    /* A0-A3: the moffs forms address through the default segment (DS) */
    write8(0x1060, 0x99);
    write_reg8(AL, 0);
    instr_A0(0x1060);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read_reg8(AL), 0x99);
    instr_A2(0x1064);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read8(0x1064), 0x99);
    write16(0x1068, 0x1234);
    instr16_A1(0x1068);
    CHECK_EQ(read_reg16(AX), 0x1234);
    instr32_A3(0x106C);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read32s(0x106C), 0x1234);

    /* the A4-AF wrappers resolve the default segment themselves and take is_asize_32()
     * from the prefix byte -- they are what the generated dispatcher calls */
    write8(0x1070, 0x11); write8(0x1071, 0x22);
    write_reg32(ESI, 0x1070);
    write_reg32(EDI, 0x3700); /* ES:0x3700 = physical 0x5700 */
    write_reg32(ECX, 2);
    cpu->flags &= ~FLAG_DIRECTION;
    cpu->prefixes = 0;
    instr_F3A4(); /* rep movsb */
    CHECK(!cpu->pagefault);
    CHECK_EQ(read_reg32(ESI), 0x1072);
    CHECK_EQ(read_reg32(EDI), 0x3702);
    CHECK_EQ(read_reg32(ECX), 0);
    CHECK_EQ(read8(0x5700), 0x11);
    CHECK_EQ(read8(0x5701), 0x22);

    /* a segment override reaches the string op through segment_prefix(DS): with FS
     * overridden and given an offset, the SOURCE moves and the destination does not */
    cpu->segment_offsets[FS] = 0x4000;
    cpu->segment_is_null[FS] = false;
    write8(0x6000, 0x77); /* FS:0x2000 = 0x4000 + 0x2000 */
    write_reg32(ESI, 0x2000);
    write_reg32(EDI, 0x3800); /* ES:0x3800 = physical 0x5800 */
    cpu->prefixes = PREFIX_MASK_SEGMENT & (FS + 1); /* the encoding is seg + 1 */
    instr_A4();                                     /* movsb, no rep */
    CHECK(!cpu->pagefault);
    CHECK_EQ(read8(0x5800), 0x77); /* the byte from FS, not from DS */
    CHECK_EQ(read_reg32(ESI), 0x2001);
    CHECK_EQ(read_reg32(EDI), 0x3801);
    cpu->prefixes = 0;

    /* no-rep stos/lods/scas/cmps */
    write_reg8(AL, 0x44);
    write_reg32(EDI, 0x3900);
    instr_AA(); /* stosb */
    CHECK_EQ(read8(0x5900), 0x44);
    CHECK_EQ(read_reg32(EDI), 0x3901);
    write8(0x1080, 0x55);
    write_reg32(ESI, 0x1080);
    instr_AC(); /* lodsb */
    CHECK_EQ(read_reg8(AL), 0x55);
    CHECK_EQ(read_reg32(ESI), 0x1081);
    write8(0x5910, 0x55);
    write_reg8(AL, 0x55);
    write_reg32(EDI, 0x3910);
    instr_AE(); /* scasb: equal -> ZF */
    CHECK(getzf());
    CHECK(!getcf());
    CHECK_EQ(read_reg32(EDI), 0x3911);

    /* rep stosb, whose wrapper takes no segment argument at all */
    write_reg8(AL, 0x66);
    write_reg32(EDI, 0x3A00);
    write_reg32(ECX, 3);
    instr_F3AA();
    CHECK(!cpu->pagefault);
    CHECK_EQ(read_reg32(ECX), 0);
    CHECK_EQ(read_reg32(EDI), 0x3A03);
    CHECK_EQ(read8(0x5A00), 0x66);
    CHECK_EQ(read8(0x5A02), 0x66);

    /* the 66/67/F0/F2/F3 prefixes re-enter the dispatcher with the prefix set, and clear it
     * afterwards (instructions.rs:571-580, 2137-2170).  The re-dispatched opcode is read
     * AFTER the prefix was installed, so a 66 prefix makes the operand size 16. */
    reset_stubs();
    write8(0x9050, 0x40); /* inc ax/eax */
    cpu->instruction_pointer = 0x9050;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    cpu->prefixes = 0;
    cpu->pagefault = false;
    instr_66();
    CHECK_EQ(dispatch_calls, 1);
    CHECK_EQ(dispatch_last_opcode, 0x40); /* is_osize_32() is now FALSE: bit 8 clear */
    CHECK_EQ(dispatch_last_prefixes, PREFIX_MASK_OPSIZE);
    CHECK_EQ(cpu->prefixes, 0); /* cleared after the re-entry */
    CHECK_EQ(cpu->instruction_pointer, 0x9051);

    write8(0x9054, 0x40);
    cpu->instruction_pointer = 0x9054;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    cpu->pagefault = false;
    instr_67(); /* the address-size prefix does not change the operand size */
    CHECK_EQ(dispatch_last_opcode, 0x140);
    CHECK_EQ(dispatch_last_prefixes, PREFIX_MASK_ADDRSIZE);
    CHECK_EQ(cpu->prefixes, 0);

    write8(0x9058, 0x90);
    cpu->instruction_pointer = 0x9058;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    cpu->pagefault = false;
    instr_F3(); /* repz */
    CHECK_EQ(dispatch_last_opcode, 0x190);
    CHECK_EQ(dispatch_last_prefixes, PREFIX_REPZ);
    CHECK_EQ(cpu->prefixes, 0);
    write8(0x905C, 0x90);
    cpu->instruction_pointer = 0x905C;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    cpu->pagefault = false;
    instr_F2(); /* repnz */
    CHECK_EQ(dispatch_last_prefixes, PREFIX_REPNZ);
    CHECK_EQ(cpu->prefixes, 0);
    write8(0x9060, 0x90);
    cpu->instruction_pointer = 0x9060;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    cpu->pagefault = false;
    instr_F0(); /* lock is a no-op prefix here */
    CHECK_EQ(dispatch_last_opcode, 0x190);
    CHECK_EQ(cpu->prefixes, 0);

    /* the segment prefixes 26/2E/36/3E/64/65 set the segment index + 1 (cpu.rs:3370-3376) */
    reset_stubs();
    write8(0x9064, 0x90);
    cpu->instruction_pointer = 0x9064;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    cpu->pagefault = false;
    instr_26(); /* ES prefix */
    CHECK_EQ(dispatch_calls, 1);
    CHECK_EQ(dispatch_last_prefixes, ES + 1);
    CHECK_EQ(cpu->prefixes, 0);
    write8(0x9068, 0x90);
    cpu->instruction_pointer = 0x9068;
    cpu->last_virt_eip = VIRT_EIP_NONE;
    cpu->pagefault = false;
    instr_65(); /* GS prefix */
    CHECK_EQ(dispatch_last_prefixes, GS + 1);

    /* arpl (63): in protected mode it raises the RPL of the destination selector and
     * reports whether it changed in ZF (instructions.rs:536-566); in real mode it is #ud */
    string_setup();
    reset_stubs();
    cpu->protected_mode = true;
    write_reg16(CX, 0x0004); /* RPL 0 */
    write_reg16(DX, 0x0067); /* RPL 3 */
    instr_63_reg(CX, DX);
    CHECK_EQ(read_reg16(CX), 0x0007);
    CHECK(getzf());
    /* the other arm: no change, ZF cleared */
    write_reg16(CX, 0x0007);
    write_reg16(DX, 0x0060); /* RPL 0 */
    instr_63_reg(CX, DX);
    CHECK_EQ(read_reg16(CX), 0x0007);
    CHECK(!getzf());
    /* the memory form goes through the read-modify-write path */
    write16(0x2260, 0x0004);
    write_reg16(DX, 0x0062); /* RPL 2 */
    instr_63_mem(0x2260, DX);
    CHECK_EQ(read16(0x2260), 0x0006); /* (0x4 & ~3) | 2 */
    CHECK(getzf());
    /* real mode: #ud, nothing written */
    cpu->protected_mode = false;
    reset_stubs();
    write16(0x2264, 0x0004);
    instr_63_mem(0x2264, DX);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);
    CHECK_EQ(read16(0x2264), 0x0004);

    printf("PASS instr: the moffs forms, the A4-AF string wrappers (incl. a segment "
           "override), arpl, the 66/67/F0/F2/F3 prefixes and the segment prefixes\n");
}

/* ------------------------------------------------------------------ *
 * gate 10 -- the read-modify-write helpers and the string slow path under a page
 * table: the page-boundary path (virt_boundary_*) and the fault contract
 * ------------------------------------------------------------------ */

static void paging_fixture_two_pages(void)
{
    cpu_state_init();
    paging_setup();
    map4k(1, 0, 0x3000, PAGE_TABLE_RW_MASK, PAGE_TABLE_RW_MASK); /* 0x400000 -> 0x3000 */
    map4k(1, 1, 0x5000, PAGE_TABLE_RW_MASK, PAGE_TABLE_RW_MASK); /* 0x401000 -> 0x5000 */
    cpu->segment_offsets[DS] = 0;
    cpu->segment_offsets[ES] = 0;
    cpu->segment_offsets[SS] = 0;
    cpu->segment_is_null[DS] = false;
    cpu->segment_is_null[ES] = false;
    cpu->segment_is_null[SS] = false;
    cpu->is_32 = true;
    cpu->stack_size_32 = true;
    cpu->prefixes = 0;
    cpu->pagefault = false;
    cpu->flags = FLAGS_DEFAULT;
    cpu->flags_changed = 0;
    reset_stubs();
}

static void test_instr_rmw_and_faults(void)
{
    /* The two virtual pages 0x400000 and 0x401000 map to the NON-ADJACENT physical pages
     * 0x3000 and 0x5000, so a read-modify-write straddling them can only be right if it goes
     * through virt_boundary_read16/32s and virt_boundary_write16/32 -- the helpers M1a-2's
     * P-NOT-3 correction established are reachable ONLY from here (cpu.rs:4078, 4107). */
    paging_fixture_two_pages();

    /* a 16-bit add at 0x400FFF: low byte in the first page, high byte in the second */
    write8(0x3FFF, 0xFF);
    write8(0x5000, 0xFF);
    write_reg16(CX, 1);
    instr16_01_mem(0x400FFF, CX);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read8(0x3FFF), 0x00);
    CHECK_EQ(read8(0x5000), 0x00);
    CHECK(getzf());
    CHECK(getcf());

    /* the 32-bit form at 0x400FFD: three bytes from the first physical page, one byte from
     * the second */
    write8(0x3FFD, 0xFF); write8(0x3FFE, 0xFF); write8(0x3FFF, 0xFF); write8(0x5000, 0xFF);
    write_reg32(ECX, 1);
    instr32_01_mem(0x400FFD, ECX);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read8(0x3FFD), 0x00);
    CHECK_EQ(read8(0x3FFE), 0x00);
    CHECK_EQ(read8(0x3FFF), 0x00);
    CHECK_EQ(read8(0x5000), 0x00);
    CHECK(getzf());
    CHECK(getcf());

    /* an RMW whose SECOND page is absent: the fault is delivered and the readable first
     * byte is NOT written -- "the instruction aborts with its partial effects kept" does
     * not include a write that never happened */
    write8(0x5FFF, 0xAA);
    cpu->pagefault = false;
    reset_delivery();
    write_reg16(CX, 1);
    instr16_01_mem(0x401FFF, CX);
    CHECK(cpu->pagefault);
    CHECK_EQ(read8(0x5FFF), 0xAA); /* untouched */
    CHECK_EQ(cpu->cr[2], 0x402000);
    CHECK_EQ(delivered_interrupt, CPU_EXCEPTION_PF);

    /* the same for the 8-bit form on a wholly absent page */
    write8(0x5FF0, 0x55);
    cpu->pagefault = false;
    instr_00_mem(0x402000, CX);
    CHECK(cpu->pagefault);
    CHECK_EQ(read8(0x5FF0), 0x55);

    /* an address whose PHYSICAL address lands in the mmap window takes the OTHER branch of
     * the helper: the read goes to the device table and the write is handed to mmap_write8,
     * so it must not reach mem8 (paging is on, so this needs a mapping to 0xB0000) */
    paging_fixture_two_pages();
    map4k(1, 2, 0xB0000, PAGE_TABLE_RW_MASK, PAGE_TABLE_RW_MASK); /* 0x402000 -> 0xB0000 */
    cpu->pagefault = false;
    mem8[0xB0000] = 0x00;
    write_reg8(CL, 1);
    instr_00_mem(0x402000, CL);
    CHECK(!cpu->pagefault);
    CHECK_EQ(mem8[0xB0000], 0x00); /* not written to RAM */

    /* the string FAST path translates once: an absent destination page aborts the whole
     * instruction before a single byte moves and before any register changes
     * (string.rs:160-184) */
    paging_fixture_two_pages();
    write8(0x3001, 0x11);
    write_reg32(ESI, 0x400001);
    write_reg32(EDI, 0x002000); /* virtual 0x2000: no page table entry */
    write_reg32(ECX, 8);
    cpu->flags &= ~FLAG_DIRECTION;
    movsb_rep(true, DS);
    CHECK(cpu->pagefault);
    CHECK_EQ(read_reg32(ESI), 0x400001);
    CHECK_EQ(read_reg32(EDI), 0x002000);
    CHECK_EQ(read_reg32(ECX), 8);

    /* the movs fast path is bounded by the DESTINATION page: with two more dwords than
     * fit before 0x402000 it moves exactly those two, rewinds instruction_pointer and
     * leaves the rest for the re-entry (string.rs:195-242) */
    paging_fixture_two_pages();
    write8(0x3001, 0x11); write8(0x3002, 0x22); write8(0x3003, 0x33); write8(0x3004, 0x44);
    write8(0x3005, 0x55); write8(0x3006, 0x66); write8(0x3007, 0x77); write8(0x3008, 0x88);
    write_reg32(ESI, 0x400001); /* unaligned: allowed for movs (string.rs:145-151) */
    write_reg32(EDI, 0x401FF8); /* two dwords fit before the page end */
    write_reg32(ECX, 4);
    cpu->flags &= ~FLAG_DIRECTION;
    cpu->instruction_pointer = 0xDEAD;
    cpu->previous_ip = 0xBEEF;
    movsd_rep(true, DS);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read_reg32(ESI), 0x400009);
    CHECK_EQ(read_reg32(EDI), 0x402000);
    CHECK_EQ(read_reg32(ECX), 2);              /* 4 - 2 moved */
    CHECK_EQ(cpu->instruction_pointer, 0xBEEF); /* re-enter for the rest */
    CHECK_EQ(read32s(0x5FF8), 0x44332211);
    CHECK_EQ(read32s(0x5FFC), (int32_t)0x88776655);

    /* ... and the re-entry faults on the absent page before moving anything further */
    reset_delivery();
    cpu->pagefault = false;
    movsd_rep(true, DS);
    CHECK(cpu->pagefault);
    CHECK_EQ(delivered_interrupt, CPU_EXCEPTION_PF);
    CHECK_EQ(cpu->cr[2], 0x402000);
    CHECK_EQ(read_reg32(ESI), 0x400009);
    CHECK_EQ(read_reg32(ECX), 2);
    CHECK_EQ(read32s(0x6000), 0); /* still nothing past the boundary */

    /* the string SLOW path (an unaligned NON-movs operand disqualifies the fast path)
     * commits one iteration at a time, so a fault in the middle leaves EXACTLY the
     * completed iterations' effects: the register advance and the data that moved
     * (string.rs:388-491, plan ss8/R3) */
    paging_fixture_two_pages();
    write_reg32(EAX, 0x11223344);
    write_reg32(EDI, 0x401FF9); /* unaligned -> the slow path */
    write_reg32(ECX, 4);
    reset_delivery();
    stosd_rep(true);
    CHECK(cpu->pagefault);
    CHECK_EQ(delivered_interrupt, CPU_EXCEPTION_PF);
    CHECK_EQ(cpu->cr[2], 0x402000);
    CHECK_EQ(read_reg32(EDI), 0x401FFD); /* one iteration done */
    CHECK_EQ(read_reg32(ECX), 3);
    CHECK_EQ(read8(0x5FF9), 0x44);       /* the dword that did move, little endian */
    CHECK_EQ(read8(0x5FFA), 0x33);
    CHECK_EQ(read8(0x5FFB), 0x22);
    CHECK_EQ(read8(0x5FFC), 0x11);
    CHECK_EQ(read32s(0x6000), 0);

    printf("PASS instr: read-modify-write across a page boundary (virt_boundary), "
           "no write on a fault, the string slow path's partial effects\n");
}

/* ------------------------------------------------------------------ *
 * gate 11 (M1b-1) -- the 0F table's NON-MMX/SSE groups
 *
 * A representative slice of the 359 symbols M1b-1 added.  The cases are chosen where a
 * transcription slip is plausible and OBSERVABLE: the argument order of the pair-wise
 * hadd/hsub forms, the sign-extended 16-bit bit offset of the bt group, the conditional
 * FAULT of cmovcc (the load is evaluated even when the condition is false), the
 * stack-pointer movement of pop fs only on switch_seg success, and cpuid's dependence on
 * acpi_enabled.
 * ------------------------------------------------------------------ */

static void test_instr_0f_nonsse(void)
{
    /* ---- cpuid (0F A2) ---- */
    string_setup();
    write_reg32(EAX, 0);
    instr_0FA2();
    CHECK_EQ(read_reg32(EAX), 0x16);       /* the maximum basic level */
    CHECK_EQ(read_reg32(EBX), 0x756E6547); /* "Genu" */
    CHECK_EQ(read_reg32(EDX), 0x49656E69); /* "ineI" */
    CHECK_EQ(read_reg32(ECX), 0x6C65746E); /* "ntel" */

    write_reg32(EAX, 1);
    instr_0FA2();
    CHECK_EQ(read_reg32(EAX), 3 | 7 << 4 | 6 << 8); /* the pentium3 signature */
    CHECK(read_reg32(EDX) & 1);                     /* fpu */
    CHECK(read_reg32(EDX) & (1 << 4));              /* tsc */
    CHECK(read_reg32(EDX) & (1 << 5));              /* msr */
    CHECK(read_reg32(EDX) & (1 << 15));             /* cmov */
    CHECK(read_reg32(ECX) & (1 << 31));             /* VMWARE_HYPERVISOR_PORT is true */
    /* the APIC bit tracks acpi_enabled, not a constant */
    CHECK(!(read_reg32(EDX) & (1 << 9)));
    cpu->acpi_enabled = true;
    write_reg32(EAX, 1);
    instr_0FA2();
    CHECK(read_reg32(EDX) & (1 << 9));
    cpu->acpi_enabled = false;

    /* leaf 0x16 is TSC_RATE/1000 MHz, and 0x15's ECX is TSC_RATE*1000 */
    write_reg32(EAX, 0x16);
    instr_0FA2();
    CHECK_EQ(read_reg32(EAX), 1000);
    CHECK_EQ(read_reg32(EBX), 1000);
    CHECK_EQ(read_reg32(ECX), 10);
    write_reg32(EAX, 0x15);
    instr_0FA2();
    CHECK_EQ(read_reg32(ECX), 1000000000);
    write_reg32(EAX, 0x80000000);
    instr_0FA2();
    CHECK_EQ(read_reg32(EAX), (int32_t)0x80000008);

    /* ---- the control/debug register moves (0F 20-23) ---- */
    string_setup();
    cpu->cr[3] = 0x00123000;
    write_reg32(EAX, 0x11111111);
    instr_0F20(EAX, 3); /* mov eax, cr3 */
    CHECK_EQ(read_reg32(EAX), 0x00123000);
    write_reg32(EAX, 0x00456000);
    instr_0F22(EAX, 3); /* mov cr3, eax */
    CHECK_EQ(cpu->cr[3], 0x00456000);
    write_reg32(EAX, 0x0BADF00D);
    instr_0F22(EAX, 2); /* mov cr2, eax: a plain store */
    CHECK_EQ(cpu->cr[2], (int32_t)0x0BADF00D);

    /* an invalid cr4 bit is #gp with NO load */
    reset_exceptions();
    cpu->cr[4] = 0;
    write_reg32(EAX, 1 << 11);
    instr_0F22(EAX, 4);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_GP);
    CHECK_EQ(cpu->cr[4], 0);

    /* cpl != 0 -> #gp, and the register keeps its value */
    cpu->cpl = 3;
    reset_exceptions();
    write_reg32(EAX, 0xFFFFFFFF);
    instr_0F20(EAX, 3);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_GP);
    CHECK_EQ(read_reg32(EAX), (int32_t)0xFFFFFFFF);
    cpu->cpl = 0;

    /* DR4/DR5 alias DR6/DR7 while cr4.DE is clear, and #ud once it is set */
    cpu->dreg[6] = (int32_t)0xFFFF0FF0;
    write_reg32(EAX, 0);
    instr_0F21(EAX, 6);
    CHECK_EQ(read_reg32(EAX), (int32_t)0xFFFF0FF0);
    write_reg32(EAX, 0);
    instr_0F21(EAX, 4);
    CHECK_EQ(read_reg32(EAX), (int32_t)0xFFFF0FF0);
    cpu->cr[4] = CR4_DE;
    reset_exceptions();
    instr_0F21(EAX, 4);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);
    cpu->cr[4] = 0;

    /* ---- 0F 00: sldt / str ---- */
    string_setup();
    cpu->protected_mode = true;
    cpu->sreg[LDTR] = 0x0007;
    cpu->sreg[TR] = 0x0010;
    write_reg32(EAX, 0);
    instr16_0F00_0_reg(AX);
    CHECK_EQ(read_reg16(AX), 0x0007);
    write_reg32(EBX, 0);
    instr32_0F00_1_reg(EBX);
    CHECK_EQ(read_reg32(EBX), 0x0010);
    instr16_0F00_0_mem(0x2000);
    CHECK_EQ(read16(0x2000), 0x0007);
    instr32_0F00_1_mem(0x2004); /* both _mem forms store 16 bits */
    CHECK_EQ(read16(0x2004), 0x0010);
    CHECK_EQ(read16(0x2006), 0x0000);
    /* real mode and vm86 are #ud */
    cpu->protected_mode = false;
    reset_exceptions();
    instr16_0F00_0_reg(AX);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);
    cpu->protected_mode = true;
    cpu->flags |= FLAG_VM;
    reset_exceptions();
    instr16_0F00_1_reg(EBX);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);
    cpu->flags &= ~FLAG_VM;

    /* ---- 0F 01: sgdt/sidt/lgdt/lidt/smsw/lmsw/invlpg ---- */
    string_setup();
    cpu->cpl = 0;
    cpu->gdtr_size = 0x1234;
    cpu->gdtr_offset = 0x0A0B0C0D;
    instr32_0F01_0_mem(0x2000); /* sgdt [0x2000] */
    CHECK_EQ(read16(0x2000), 0x1234);
    CHECK_EQ(read32s(0x2002), 0x0A0B0C0D);
    instr16_0F01_0_mem(0x2010); /* the 16-bit form keeps 24 bits of the base */
    CHECK_EQ(read16(0x2010), 0x1234);
    CHECK_EQ(read32s(0x2012), 0x0B0C0D);

    cpu->idtr_size = 0x0020;
    cpu->idtr_offset = 0x00001000;
    instr32_0F01_1_mem(0x2020); /* sidt reads the IDTR, not the GDTR */
    CHECK_EQ(read16(0x2020), 0x0020);
    CHECK_EQ(read32s(0x2022), 0x00001000);

    write16(0x2030, 0x00FF);
    write32(0x2032, 0x00002000);
    instr32_0F01_2_mem(0x2030); /* lgdt */
    CHECK_EQ(cpu->gdtr_size, 0x00FF);
    CHECK_EQ(cpu->gdtr_offset, 0x00002000);
    write16(0x2040, 0x0040);
    write32(0x2042, 0x00003000);
    instr32_0F01_3_mem(0x2040); /* lidt */
    CHECK_EQ(cpu->idtr_size, 0x0040);
    CHECK_EQ(cpu->idtr_offset, 0x00003000);

    /* lgdt at cpl != 0 is #gp and loads nothing */
    cpu->cpl = 3;
    reset_exceptions();
    write16(0x2050, 0x0001);
    write32(0x2052, 0x00004000);
    instr32_0F01_2_mem(0x2050);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_GP);
    CHECK_EQ(cpu->gdtr_offset, 0x00002000);
    cpu->cpl = 0;

    /* smsw writes cr0 (low 16 bits to memory); lmsw takes ONLY cr0's low 4 bits */
    cpu->cr[0] = RESET_CR0;
    write_reg32(EAX, 0);
    instr32_0F01_4_reg(EAX);
    CHECK_EQ(read_reg32(EAX), RESET_CR0);
    write16(0x2060, 0);
    instr16_0F01_4_mem(0x2060);
    CHECK_EQ(read16(0x2060), RESET_CR0 & 0xFFFF);

    cpu->protected_mode = false;
    write_reg32(EBX, 0xFFFFFFFF);
    instr16_0F01_6_reg(BX); /* lmsw bx: bits 0-3 only -> PE|MP|EM|TS */
    CHECK_EQ(cpu->cr[0], (RESET_CR0 & ~15) | 0xF);
    CHECK(cpu->protected_mode); /* PE came from the low nibble */
    reset_exceptions();
    cpu->cpl = 3;
    instr16_0F01_6_reg(BX);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_GP);
    cpu->cpl = 0;

    /* invlpg: v86 does NOT track the guest's page-table writes, so a remapped page keeps
     * answering from the TLB until invlpg -- which is exactly what makes the two reads
     * below differ */
    paging_fixture_two_pages();
    write32(0x3000, 0x11111111); /* physical page 0x3000 = virtual 0x400000 */
    write32(0x5000, 0x22222222); /* what the remapped page will hold */
    CHECK_EQ(safe_read32s(0x400000), 0x11111111); /* fills the TLB */
    map4k(1, 0, 0x5000, PAGE_TABLE_RW_MASK, PAGE_TABLE_RW_MASK); /* 0x400000 -> 0x5000 */
    CHECK_EQ(safe_read32s(0x400000), 0x11111111); /* STALE: the TLB still answers */
    instr16_0F01_7_mem(0x400000);                 /* invlpg */
    CHECK_EQ(safe_read32s(0x400000), 0x22222222); /* the new mapping answers */
    /* and invlpg at cpl != 0 is #gp */
    reset_exceptions();
    cpu->cpl = 3;
    instr16_0F01_7_mem(0x400000);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_GP);
    cpu->cpl = 0;

    /* ---- setcc (0F 90-9F) ---- */
    string_setup();
    cpu->flags = FLAGS_DEFAULT | FLAG_ZERO;
    cpu->flags_changed = 0;
    write_reg32(EAX, 0);
    instr_0F94_reg(AL, 0); /* sete al */
    CHECK_EQ(read_reg8(AL), 1);
    instr_0F95_reg(AL, 0); /* setne al */
    CHECK_EQ(read_reg8(AL), 0);
    instr_0F9F_reg(AL, 0); /* setg al: ZF=1 -> 0 */
    CHECK_EQ(read_reg8(AL), 0);
    write8(0x2000, 0xAA);
    instr_0F94_mem(0x2000, 0); /* sete byte [0x2000] */
    CHECK_EQ(read8(0x2000), 1);
    /* the memory form propagates a fault */
    paging_fixture_two_pages();
    cpu->flags = FLAGS_DEFAULT | FLAG_ZERO;
    cpu->flags_changed = 0;
    instr_0F94_mem(0x402000, 0);
    CHECK(cpu->pagefault);

    /* ---- cmovcc (0F 40-4F), including the fault that a cmov cannot skip ---- */
    paging_fixture_two_pages();
    write32(0x3000, 0x00112233);
    cpu->flags = FLAGS_DEFAULT | FLAG_ZERO;
    cpu->flags_changed = 0;
    write_reg32(EDX, (int32_t)0xAABBCCDD);
    instr32_0F44_mem(0x400000, EDX); /* cmove edx, [0x400000] */
    CHECK(!cpu->pagefault);
    CHECK_EQ(read_reg32(EDX), 0x00112233);
    write_reg32(EDX, (int32_t)0xAABBCCDD);
    instr32_0F45_mem(0x400000, EDX); /* cmovne: ZF=1 -> not taken */
    CHECK_EQ(read_reg32(EDX), (int32_t)0xAABBCCDD);
    write_reg32(ECX, 0x000055AA);
    instr32_0F44_reg(ECX, EDX); /* cmove edx, ecx */
    CHECK_EQ(read_reg32(EDX), 0x000055AA);

    /* the LOAD happens even when the condition is false: an unmapped page faults and the
     * destination keeps its value */
    string_setup();
    paging_fixture_two_pages();
    cpu->flags = FLAGS_DEFAULT; /* ZF=0 */
    cpu->flags_changed = 0;
    write_reg32(EDX, (int32_t)0xAABBCCDD);
    instr32_0F45_mem(0x402000, EDX); /* cmovne edx, [absent page] */
    CHECK(cpu->pagefault);
    CHECK_EQ(read_reg32(EDX), (int32_t)0xAABBCCDD);
    cpu->pagefault = false;
    write_reg32(EDX, (int32_t)0xAABBCCDD);
    instr32_0F44_mem(0x402000, EDX); /* cmove, condition TRUE and still faults */
    CHECK(cpu->pagefault);
    CHECK_EQ(read_reg32(EDX), (int32_t)0xAABBCCDD);

    /* ---- the bt/bts/btr/btc family (0F A3, 0F AB, 0F B3, 0F BB, 0F BA) ---- */
    string_setup();
    write_reg32(EAX, 0);
    instr32_0FBA_5_reg(EAX, 10); /* bts eax, 10 */
    CHECK_EQ(read_reg32(EAX), 0x00000400);
    CHECK(!getcf()); /* the bit was clear */
    instr32_0FBA_4_reg(EAX, 10); /* bt eax, 10 */
    CHECK(getcf());
    CHECK_EQ(read_reg32(EAX), 0x00000400); /* bt does not write */
    instr32_0FBA_7_reg(EAX, 10); /* btc eax, 10 */
    CHECK(getcf());
    CHECK_EQ(read_reg32(EAX), 0);
    instr32_0FBA_6_reg(EAX, 10); /* btr on a clear bit */
    CHECK(!getcf());

    /* the immediate is masked with 31 for a 32-bit operand and 15 for a 16-bit one:
     * 34 -> bit 2, while 18 stays bit 18 */
    write_reg32(EAX, 0);
    instr32_0FBA_5_reg(EAX, 34);
    CHECK_EQ(read_reg32(EAX), 0x00000004);
    write_reg32(EAX, 0);
    instr32_0FBA_5_reg(EAX, 18);
    CHECK_EQ(read_reg32(EAX), 0x00040000);
    /* ... and 16-bit forms mask with 15 */
    write_reg32(EAX, 0);
    instr16_0FBA_5_reg(AX, 18);
    CHECK_EQ(read_reg16(AX), 0x0004);

    /* the register forms */
    write_reg32(EAX, 0);
    write_reg32(ECX, 5);
    instr32_0FAB_reg(EAX, ECX); /* bts eax, ecx */
    CHECK_EQ(read_reg32(EAX), 0x00000020);
    instr32_0FB3_reg(EAX, ECX); /* btr eax, ecx */
    CHECK_EQ(read_reg32(EAX), 0);
    instr32_0FBB_reg(EAX, ECX); /* btc eax, ecx */
    CHECK_EQ(read_reg32(EAX), 0x00000020);
    instr32_0FA3_reg(EAX, ECX); /* bt eax, ecx */
    CHECK(getcf());

    /* THE SIGN-EXTENDING 16-BIT MEMORY FORM: upstream computes the bit offset as
     * `read_reg16(r) << 16 >> 16`, so a selector of 0x8000 means bit -32768 -- i.e. 4 KiB
     * BELOW the effective address, not 4 KiB above it. */
    string_setup();
    write32(0x2000, 0);
    write32(0x3000, 0);
    write_reg32(EBX, 0x8000);
    instr16_0FAB_mem(0x3000, EBX); /* bts word [0x3000], bx */
    CHECK_EQ(read8(0x2000), 0x01); /* bit 0 of the byte at 0x3000 - 4096 */
    CHECK_EQ(read32s(0x3000), 0);  /* nothing at the effective address */
    CHECK_EQ(read8(0x4000), 0);    /* ... nor 4 KiB above it */

    /* the SAME signed offset reaches the other 16-bit memory forms: bt (0F A3) only
     * reports the bit through CF, btr (0F B3) clears it -- and in both the bit must be
     * taken from the byte 4 KiB BELOW, so a mutation that drops the SEX16 changes the
     * answer in a way CF and the byte at 0x2000 both see */
    write8(0x2000, 0x01);
    write8(0x3000, 0x00);
    write8(0x4000, 0x00);
    write_reg32(EBX, 0x8000);
    cpu->flags = FLAGS_DEFAULT;
    cpu->flags_changed = 0;
    instr16_0FA3_mem(0x3000, EBX); /* bt word [0x3000], bx */
    CHECK(getcf());
    CHECK_EQ(read8(0x2000), 0x01); /* bt writes nothing */
    cpu->flags = FLAGS_DEFAULT;
    cpu->flags_changed = 0;
    instr16_0FB3_mem(0x3000, EBX); /* btr word [0x3000], bx */
    CHECK(getcf());                /* the bit was set */
    CHECK_EQ(read8(0x2000), 0x00); /* cleared at 0x3000 - 4096 */
    CHECK_EQ(read8(0x4000), 0x00); /* nothing 4 KiB above */
    cpu->flags = FLAGS_DEFAULT;
    cpu->flags_changed = 0;
    instr16_0FBB_mem(0x3000, EBX); /* btc word [0x3000], bx */
    CHECK(!getcf());               /* the bit was clear */
    CHECK_EQ(read8(0x2000), 0x01); /* toggled back on, again 4 KiB below */
    /* the 32-bit memory form passes the register unmasked, so bit 15 lands inside the
     * same address */
    write32(0x2000, 0);
    write_reg32(EBX, 15);
    instr32_0FAB_mem(0x2000, EBX);
    CHECK_EQ(read32s(0x2000), 0x00008000);

    /* the memory write forms are read-modify-writes: a bt_mem does not write at all */
    write8(0x2010, 0xFF);
    write_reg32(EDX, 0);
    instr16_0FB3_mem(0x2010, EDX); /* btr word [0x2010], dx (bit 0) */
    CHECK_EQ(read8(0x2010), 0xFE);

    /* ---- bsf/bsr (0F BC/BD) ---- */
    string_setup();
    write_reg32(EDX, (int32_t)0xDEADBEEF);
    write_reg32(ECX, 0x00000010);
    instr32_0FBC_reg(ECX, EDX); /* bsf edx, ecx -> 4 */
    CHECK_EQ(read_reg32(EDX), 4);
    CHECK(!getzf());
    write_reg32(ECX, 0x00010000);
    instr32_0FBD_reg(ECX, EDX); /* bsr edx, ecx -> 16 */
    CHECK_EQ(read_reg32(EDX), 16);
    CHECK(!getzf());
    /* a zero source sets ZF and leaves the destination where it was */
    write_reg32(EDX, (int32_t)0xDEADBEEF);
    write_reg32(ECX, 0);
    instr32_0FBC_reg(ECX, EDX);
    CHECK(getzf());
    CHECK_EQ(read_reg32(EDX), (int32_t)0xDEADBEEF);
    write16(0x2000, 0x0008);
    write_reg32(EDX, 0);
    instr16_0FBC_mem(0x2000, EDX); /* bsf dx, [0x2000] -> 3 */
    CHECK_EQ(read_reg16(DX), 3);

    /* ---- cmpxchg (0F B0/B1) ---- */
    string_setup();
    write_reg32(EAX, 0x11223344);
    write_reg32(ECX, 0x55667788);
    write32(0x2000, 0x11223344);
    instr32_0FB1_mem(0x2000, ECX); /* equal -> ZF=1 and the memory takes ECX */
    CHECK(getzf());
    CHECK_EQ(read32s(0x2000), 0x55667788);
    CHECK_EQ(read_reg32(EAX), 0x11223344);
    write32(0x2004, (int32_t)0x99AABBCC);
    instr32_0FB1_mem(0x2004, ECX); /* unequal -> ZF=0 and EAX takes the memory */
    CHECK(!getzf());
    CHECK_EQ(read_reg32(EAX), (int32_t)0x99AABBCC);
    CHECK_EQ(read32s(0x2004), (int32_t)0x99AABBCC);

    /* the byte form compares AL */
    write_reg8(AL, 0x10);
    write_reg8(CL, 0x20);
    write8(0x2010, 0x30);
    instr_0FB0_mem(0x2010, CL);
    CHECK_EQ(read_reg8(AL), 0x30);
    write_reg8(AL, 0x30);
    instr_0FB0_mem(0x2010, CL);
    CHECK_EQ(read8(0x2010), 0x20);
    /* the register form writes the register back */
    write_reg32(EAX, 0x11111111);
    write_reg32(EBX, 0x22222222);
    write_reg32(ECX, 0x33333333);
    instr32_0FB1_reg(EBX, ECX); /* unequal */
    CHECK_EQ(read_reg32(EBX), 0x22222222);
    write_reg32(EAX, 0x22222222);
    instr32_0FB1_reg(EBX, ECX); /* equal -> EBX takes ECX */
    CHECK_EQ(read_reg32(EBX), 0x33333333);

    /* ---- xadd (0F C0/C1) ---- */
    string_setup();
    write_reg32(EAX, 0x00000010);
    write_reg32(ECX, 0x00000020);
    instr32_0FC1_reg(EAX, ECX); /* xadd eax, ecx */
    CHECK_EQ(read_reg32(EAX), 0x00000030); /* the sum */
    CHECK_EQ(read_reg32(ECX), 0x00000010); /* the OLD destination */
    write32(0x2000, 0x00000011);
    write_reg32(ECX, 0x00000001);
    instr32_0FC1_mem(0x2000, ECX);
    CHECK_EQ(read32s(0x2000), 0x00000012);
    CHECK_EQ(read_reg32(ECX), 0x00000011);

    /* ---- bswap (0F C8-CF) ---- */
    string_setup();
    write_reg32(EAX, 0x11223344);
    instr_0FC8();
    CHECK_EQ(read_reg32(EAX), 0x44332211);
    write_reg32(EDI, 0xDEADBEEF);
    instr_0FCF();
    CHECK_EQ(read_reg32(EDI), (int32_t)0xEFBEADDE);
    /* bswap clears the upper half first: a 16-bit register's value does not come back */
    write_reg32(EBX, 0x0000ABCD);
    instr_0FCB();
    CHECK_EQ(read_reg32(EBX), (int32_t)0xCDAB0000);

    /* ---- shld/shrd (0F A4/A5, 0F AC/AD) ---- */
    string_setup();
    write_reg32(EAX, 0x12345678);
    write_reg32(ECX, 0x9ABCDEF0);
    instr32_0FA4_reg(EAX, ECX, 4); /* shld eax, ecx, 4 */
    CHECK_EQ(read_reg32(EAX), 0x23456789);
    write_reg32(EAX, 0x12345678);
    write_reg32(ECX, 0x0000000F);
    instr32_0FAC_reg(EAX, ECX, 4); /* shrd eax, ecx, 4 */
    CHECK_EQ(read_reg32(EAX), (int32_t)0xF1234567);
    /* count 0 leaves the destination alone */
    write_reg32(EAX, 0x12345678);
    write_reg32(ECX, 0x9ABCDEF0);
    instr32_0FA4_reg(EAX, ECX, 0);
    CHECK_EQ(read_reg32(EAX), 0x12345678);
    /* the CL forms shift by CL & 31 */
    write_reg8(CL, 4);
    write_reg32(EAX, 0x12345678);
    write_reg32(EDX, 0x9ABCDEF0);
    instr32_0FA5_reg(EDX, EAX); /* shld edx, eax, cl */
    CHECK_EQ(read_reg32(EDX), (int32_t)0xABCDEF01);
    /* the memory forms are read-modify-writes */
    write32(0x2000, 0x12345678);
    write_reg32(ECX, 0x9ABCDEF0);
    instr32_0FA4_mem(0x2000, ECX, 4);
    CHECK_EQ(read32s(0x2000), 0x23456789);
    write16(0x2010, 0x1234);
    write_reg16(CX, 0xABCD);
    instr16_0FAC_mem(0x2010, CX, 4); /* shrd word [0x2010], cx, 4 */
    CHECK_EQ(read16(0x2010), 0xD123); /* (0x1234 >> 4) | (0xABCD << 12 & 0xFFFF) */
    /* the mem forms straddle a page boundary through the SAFE_READ_WRITE path */
    write32(0x2FFE, 0x12345678);
    write_reg32(ECX, 0x9ABCDEF0);
    instr32_0FA4_mem(0x2FFE, ECX, 4);
    CHECK_EQ(read32s(0x2FFE), 0x23456789);

    /* ---- popcnt (F3 0F B8) ---- */
    string_setup();
    write_reg32(ECX, (int32_t)0xF0F0F0F0);
    instr32_F30FB8_reg(ECX, EDX);
    CHECK_EQ(read_reg32(EDX), 16);
    CHECK(!getzf());
    write_reg32(ECX, 0);
    instr32_F30FB8_reg(ECX, EDX);
    CHECK_EQ(read_reg32(EDX), 0);
    CHECK(getzf());
    write16(0x2000, 0x0007);
    instr16_F30FB8_mem(0x2000, EDX);
    CHECK_EQ(read_reg16(DX), 3);
    /* the plain 0F B8 and 0F B9 are #ud on this model */
    reset_exceptions();
    instr32_0FB8_reg(ECX, EDX);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);
    reset_exceptions();
    instr_0FB9();
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);

    /* ---- movzx/movsx (0F B6/B7/BE/BF) ---- */
    string_setup();
    write_reg32(EBX, 0x00000080);
    write_reg32(EAX, 0);
    instr32_0FBE_reg(BL, EAX); /* movsx eax, bl */
    CHECK_EQ(read_reg32(EAX), (int32_t)0xFFFFFF80);
    write_reg8(BL, 0xFF);
    instr32_0FB6_reg(BL, EAX); /* movzx eax, bl */
    CHECK_EQ(read_reg32(EAX), 0xFF);
    write16(0x2000, 0x8001);
    instr16_0FBF_mem(0x2000, EAX); /* movsx ax, [0x2000] */
    CHECK_EQ(read_reg16(AX), 0x8001);
    instr32_0FB7_mem(0x2000, EAX); /* movzx eax, [0x2000] */
    CHECK_EQ(read_reg32(EAX), 0x00008001);

    /* ---- imul (0F AF) ---- */
    string_setup();
    write_reg32(EDX, 2);
    write_reg32(ECX, 3);
    instr32_0FAF_reg(ECX, EDX); /* imul edx, ecx */
    CHECK_EQ(read_reg32(EDX), 6);
    CHECK(!getcf());
    CHECK(!getof());
    write_reg32(EDX, (int32_t)0x80000000);
    write_reg32(ECX, 2);
    instr32_0FAF_reg(ECX, EDX); /* the product does not fit -> CF = OF = 1 */
    CHECK_EQ(read_reg32(EDX), 0);
    CHECK(getcf());
    CHECK(getof());
    write16(0x2000, 0xFFFE);
    write_reg16(DX, 2);
    instr16_0FAF_mem(0x2000, EDX); /* imul dx, [0x2000] */
    CHECK_EQ(read_reg16(DX), 0xFFFC);

    /* ---- push/pop fs and gs (0F A0/A1, 0F A8/A9) ---- */
    string_setup();
    cpu->sreg[FS] = 0x0033;
    write_reg32(ESP, 0x1000);
    instr32_0FA0(); /* push fs */
    /* a segment push stores 16 bits but moves SP by 4 (misc_instr.c:311) */
    CHECK_EQ(read_reg32(ESP), 0x0FFC);
    CHECK_EQ(read16(0x1FFC), 0x0033);
    reset_stubs();
    instr32_0FA1(); /* pop fs hands the selector to switch_seg (M1c's) */
    CHECK_EQ(switch_seg_calls, 1);
    CHECK_EQ(switch_seg_last_reg, FS);
    CHECK_EQ(switch_seg_last_selector, 0x0033);
    CHECK_EQ(read_reg32(ESP), 0x1000);
    /* a refused switch_seg must NOT move the stack pointer */
    write_reg32(ESP, 0x1000);
    reset_stubs();
    switch_seg_answer = false;
    instr32_0FA1();
    CHECK_EQ(read_reg32(ESP), 0x1000);
    switch_seg_answer = true;

    cpu->sreg[GS] = 0x002B;
    write_reg32(ESP, 0x1000);
    instr16_0FA8(); /* push gs (16-bit push on a 32-bit stack) */
    CHECK_EQ(read_reg32(ESP), 0x0FFE);
    CHECK_EQ(read16(0x1FFE), 0x002B);
    reset_stubs();
    instr16_0FA9(); /* pop gs */
    CHECK_EQ(switch_seg_last_reg, GS);
    CHECK_EQ(read_reg32(ESP), 0x1000);

    /* ---- lss (0F B2) ---- */
    string_setup();
    write32(0x2000, 0x00400000);
    write16(0x2004, 0x0018);
    reset_stubs();
    write_reg32(EDX, 0);
    instr32_0FB2_mem(0x2000, EDX);
    CHECK_EQ(read_reg32(EDX), 0x00400000);
    CHECK_EQ(switch_seg_last_reg, SS);
    CHECK_EQ(switch_seg_last_selector, 0x0018);
    /* the register form is #ud */
    reset_exceptions();
    instr32_0FB2_reg(EDX, ECX);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);
    write16(0x2010, 0x1234);
    write16(0x2014, 0x0018);
    write_reg16(DX, 0);
    instr16_0FB5_mem(0x2010, EDX); /* lgs dx, [0x2010] */
    CHECK_EQ(read_reg16(DX), 0x1234);
    CHECK_EQ(switch_seg_last_reg, GS);

    /* ---- the 0F AE group ---- */
    string_setup();
    cpu->mxcsr = 0x1F80;
    instr_0FAE_3_mem(0x2000); /* stmxcsr */
    CHECK_EQ(read32s(0x2000), 0x1F80);
    cpu->mxcsr = 0;
    instr_0FAE_2_mem(0x2000); /* ldmxcsr */
    CHECK_EQ(cpu->mxcsr, 0x1F80);
    /* reserved MXCSR bits are #gp and the register keeps its value */
    write32(0x2004, (int32_t)0xFFFF0000);
    reset_exceptions();
    instr_0FAE_2_mem(0x2004);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_GP);
    CHECK_EQ(cpu->mxcsr, 0x1F80);
    /* the fences are nops; xsave/xrstor/xsaveopt are undefined */
    instr_0FAE_5_reg(ECX);
    instr_0FAE_6_reg(ECX);
    instr_0FAE_7_reg(ECX);
    reset_exceptions();
    instr_0FAE_5_mem(0x2008);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);
    /* clflush runs the operand through a READ translation */
    paging_fixture_two_pages();
    instr_0FAE_7_mem(0x400000);
    CHECK(!cpu->pagefault);
    instr_0FAE_7_mem(0x402000);
    CHECK(cpu->pagefault);
    /* /0 and /1 drive fpu.c's fxsave/fxrstor through the cross-unit contract.  The
     * subject HERE is the dispatch: the image is at the FXSAVE offsets and a round trip
     * through the real x87 state restores what was saved. */
    string_setup();
    zero_memory(0x2000, 0x200);
    cpu->fpu_control_word = 0x057F;
    cpu->fpu_status_word = 0x0000;
    cpu->fpu_ip = 0x00112233;
    cpu->mxcsr = 0x1F80;
    instr_0FAE_0_mem(0x2000);
    CHECK(!cpu->pagefault);
    CHECK_EQ(read16(0x2000), 0x057F);  /* FCW at +0 */
    CHECK_EQ(read32s(0x2018), 0x1F80); /* MXCSR at +24 */
    cpu->fpu_control_word = 0;
    cpu->fpu_ip = 0;
    cpu->mxcsr = 0;
    instr_0FAE_1_mem(0x2000);
    CHECK(!cpu->pagefault);
    CHECK_EQ(cpu->fpu_control_word, 0x057F);
    CHECK_EQ(cpu->fpu_ip, 0x00112233);
    CHECK_EQ(cpu->mxcsr, 0x1F80);
    /* both forms translate their operand, so an absent page faults (fxsave's own
     * writable_or_pagefault, reached through this dispatch) */
    paging_fixture_two_pages();
    instr_0FAE_0_mem(0x402000);
    CHECK(cpu->pagefault);
    cpu->pagefault = false;
    instr_0FAE_1_mem(0x402000);
    CHECK(cpu->pagefault);
    /* /0../1 _reg is #ud */
    string_setup();
    reset_exceptions();
    instr_0FAE_0_reg(ECX);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);
    reset_exceptions();
    instr_0FAE_1_reg(ECX);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);

    /* ---- cmpxchg8b (0F C7 /1) ---- */
    string_setup();
    write_reg32(EAX, 0x11223344);
    write_reg32(EDX, 0x55667788);
    write32(0x2000, 0x11223344);
    write32(0x2004, 0x55667788);
    write_reg32(EBX, (int32_t)0xAABBCCDD);
    write_reg32(ECX, (int32_t)0xEEFF0011);
    instr32_0FC7_1_mem(0x2000); /* equal -> ZF=1 and ECX:EBX replaces the memory */
    CHECK(getzf());
    CHECK_EQ(read32s(0x2000), (int32_t)0xAABBCCDD);
    CHECK_EQ(read32s(0x2004), (int32_t)0xEEFF0011);
    write32(0x2000, 0x01020304);
    write32(0x2004, 0x05060708);
    instr32_0FC7_1_mem(0x2000); /* unequal -> EDX:EAX takes the memory */
    CHECK(!getzf());
    CHECK_EQ(read_reg32(EAX), 0x01020304);
    CHECK_EQ(read_reg32(EDX), 0x05060708);
    CHECK_EQ(read32s(0x2000), 0x01020304); /* nothing written */
    /* the register forms are #ud */
    reset_exceptions();
    instr32_0FC7_1_reg(ECX);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);

    /* ---- the hadd/hsub slice (66/F2 0F 7C/7D) ---- */
    string_setup();
    /* xmm1 = {1,2,3,4}, xmm2 = {10,20,30,40}: haddps xmm2, xmm1 sums PAIRWISE, and the
     * DESTINATION's pair comes first -- the order is not commutative */
    write_xmm128(1, 0x3F800000, 0x40000000, 0x40400000, 0x40800000);
    write_xmm128(2, 0x41200000, 0x41A00000, 0x41F00000, 0x42200000);
    instr_F20F7C_reg(1, 2);
    CHECK_EQ(read_xmm128s(2).f32[0], 30.0f);
    CHECK_EQ(read_xmm128s(2).f32[1], 70.0f);
    CHECK_EQ(read_xmm128s(2).f32[2], 3.0f);
    CHECK_EQ(read_xmm128s(2).f32[3], 7.0f);
    CHECK_EQ(read_xmm128s(1).f32[0], 1.0f); /* the source is untouched */
    /* hsubpd xmm2, xmm1 */
    write_xmm128_2(1, 0x3FF0000000000000ull, 0x4000000000000000ull); /* 1.0, 2.0 */
    write_xmm128_2(2, 0x4024000000000000ull, 0x4034000000000000ull); /* 10.0, 20.0 */
    instr_660F7D_reg(1, 2);
    CHECK_EQ(read_xmm128s(2).f64[0], 10.0 - 20.0);
    CHECK_EQ(read_xmm128s(2).f64[1], 1.0 - 2.0);
    /* the memory forms read the source operand */
    write32(0x2000, 0x3F800000); /* 1.0 */
    write32(0x2004, 0x40000000); /* 2.0 */
    write32(0x2008, 0x40400000); /* 3.0 */
    write32(0x200C, 0x40800000); /* 4.0 */
    write_xmm128(2, 0x41200000, 0x41A00000, 0x41F00000, 0x42200000); /* 10,20,30,40 */
    instr_F20F7C_mem(0x2000, 2); /* haddps xmm2, [0x2000] */
    CHECK_EQ(read_xmm128s(2).f32[0], 30.0f);
    CHECK_EQ(read_xmm128s(2).f32[2], 3.0f);

    /* ---- clts (0F 06) and the undefined encodings ---- */
    string_setup();
    cpu->cr[0] |= CR0_TS;
    cpu->cpl = 0;
    instr_0F06();
    CHECK(!(cpu->cr[0] & CR0_TS));
    cpu->cpl = 3;
    cpu->cr[0] |= CR0_TS;
    reset_exceptions();
    instr_0F06();
    CHECK_EQ(exc_vector, CPU_EXCEPTION_GP);
    CHECK(cpu->cr[0] & CR0_TS); /* nothing cleared */
    /* wbinvd is a cpl-0 no-op */
    reset_exceptions();
    instr_0F09();
    CHECK_EQ(exc_vector, CPU_EXCEPTION_GP);
    cpu->cpl = 0;
    instr_0F09();
    CHECK_EQ(exc_vector, CPU_EXCEPTION_GP); /* still the recorded one */

    /* UD2 and the undefined encodings raise #UD -- they are NOT the emitter's stop */
    reset_exceptions();
    instr_0F0B();
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);
    reset_exceptions();
    instr_0F0D();
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);
    reset_exceptions();
    instr_0F1A();
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);
    reset_exceptions();
    instr_0F33();
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);
    reset_exceptions();
    instr_0F38(); /* the escape range is #UD in the reference, not an SSE stop */
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);
    reset_exceptions();
    instr_0F7C();
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);
    reset_exceptions();
    instr_0FA6(); /* the obsolete cmpxchg */
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);
    reset_exceptions();
    instr_0FAA(); /* rsm */
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);
    reset_exceptions();
    instr_0FFF(); /* Windows 98 */
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD);

    reset_stubs();
    cpu_state_init();
    printf("PASS instr: the non-MMX/SSE 0F table -- cpuid, mov cr/dr, sldt/str, the 01 "
           "group and invlpg, setcc, cmovcc (incl. its fault), bt/bts/btr/btc, bsf/bsr, "
           "cmpxchg, xadd, bswap, shld/shrd, popcnt, movzx/movsx, imul, push/pop fs/gs, "
           "lss, the 0F AE group, cmpxchg8b, hadd/hsub, clts and the undefined encodings\n");
}

/* ================================================================== *
 * M1b-3 -- THE x87 UNIT (fpu.c + softfloat.c).
 *
 * Covers what the brief's gate G2 names: the tag word, the stack pointers, the
 * environment save/restore, the rounding modes and the status bits -- plus the
 * softfloat conversion round-trips, which are the MEASUREMENT behind softfloat.h's ABI
 * claim (the SmallFloat entry points are declared with the plain integer types their
 * single-member-uint structs are passed as) and therefore the one thing that would make
 * every FPU number wrong at once if it were false.
 * ================================================================== */

#define FPU_SCRATCH 0x400000u /* inside the 8 MiB flat memory, clear of every other test */

static void fpu_reset(void)
{
    cpu_state_init();
    cpu->memory_size = MEM_SIZE;
    cpu->pagefault = false;
}

static uint64_t f64_bits(double v)
{
    uint64_t b;
    memcpy(&b, &v, sizeof(b));
    return b;
}

static f80_t f80_of_double(double v) { return f80_of_f64(f64_bits(v)); }

static double f80_as_double(f80_t v)
{
    uint64_t b = f80_convert_to_f64(v);
    double d;
    memcpy(&d, &b, sizeof(d));
    return d;
}

/* f80_eq is SoftFloat's ==, which is FALSE whenever either side is a NaN -- so the
 * indefinite NaN must be compared by its bits, not by equality (measured: the first
 * version of this test used f80_eq and failed on exactly that). */
static bool f80_bits_eq(f80_t a, f80_t b)
{
    return a.mantissa == b.mantissa && a.sign_exponent == b.sign_exponent;
}

/* gate 5a -- the words and the register stack (fpu.rs:25-85, 160-164, 225-309, 444-454,
 * 582-597) */
static void test_fpu_words_and_stack(void)
{
    fpu_reset();

    /* The engine's compiled-in defaults are asserted at the top of main() (before any
     * test can call set_control_word); HERE the mapping 0x37F -> near-even/P80 is checked
     * after the fact, which is the form that stays true however the tests are ordered. */
    set_control_word(0x37F);
    CHECK_EQ(softfloat_roundingMode, F80_ROUND_NEAR_EVEN);
    CHECK_EQ(extF80_roundingPrecision, F80_PREC_P80);
    CHECK_EQ(softfloat_exceptionFlags, 0);

    /* fpu_load_status_word: TOP lives in bits 11-13, re-applied on every read */
    CHECK_EQ(fpu_load_status_word(), 0);
    cpu->fpu_stack_ptr = 5;
    CHECK_EQ(fpu_load_status_word(), 5 << 11);
    cpu->fpu_stack_ptr = 0;

    /* fpu_load_tag_word: reset's stack_empty 0xFF means every slot is EMPTY -> 0xFFFF */
    CHECK_EQ(fpu_load_tag_word(), 0xFFFF);

    /* fpu_set_tag_word: the ONLY tag value that means "empty" is 3 (fpu.rs:447) */
    fpu_set_tag_word(0xFFFF);
    CHECK_EQ(cpu->fpu_stack_empty, 0xFF);
    fpu_set_tag_word(0x0000);
    CHECK_EQ(cpu->fpu_stack_empty, 0x00);
    fpu_set_tag_word(0x0003); /* slot 0's field is 3 -> EMPTY */
    CHECK_EQ(cpu->fpu_stack_empty, 0x01);
    fpu_set_tag_word(0x0001); /* slot 0's field is 1 (zero) -> NOT empty */
    CHECK_EQ(cpu->fpu_stack_empty, 0x00);
    fpu_set_tag_word(0x00F0); /* slots 2 and 3 */
    CHECK_EQ(cpu->fpu_stack_empty, 0x0C);

    /* fpu_set_status_word: TOP is taken, the exception/result bits are stored masked */
    fpu_set_status_word(0x3800 | 0x3F);
    CHECK_EQ(cpu->fpu_stack_ptr, 7);
    CHECK_EQ(cpu->fpu_status_word, 0x3F);
    CHECK_EQ(fpu_load_status_word(), 0x3800 | 0x3F);

    /* fpu_finit (fpu.rs:272-280) */
    fpu_finit();
    CHECK_EQ(cpu->fpu_control_word, 0x37F);
    CHECK_EQ(cpu->fpu_status_word, 0);
    CHECK_EQ(cpu->fpu_stack_empty, 0xFF);
    CHECK_EQ(cpu->fpu_stack_ptr, 0);
    CHECK_EQ(cpu->fpu_ip, 0);
    CHECK_EQ(cpu->fpu_dp, 0);
    CHECK_EQ(cpu->fpu_opcode, 0);

    /* the stack pointers (fpu.rs:225-271): a push moves TOP DOWN and CLEARS that slot's
     * empty bit; a pop sets it and moves TOP UP */
    fpu_reset();
    fpu_push(f80_of_double(1.5));
    CHECK_EQ(cpu->fpu_stack_ptr, 7);
    CHECK_EQ(cpu->fpu_stack_empty, 0x7F);
    CHECK(f80_eq(fpu_get_st0(), f80_of_double(1.5)));
    CHECK(f80_eq(fpu_get_sti(0), fpu_get_st0()));
    CHECK(!fpu_sti_empty(0));
    CHECK(fpu_sti_empty(1));
    CHECK_EQ(fpu_get_sti_f64(0), 1.5);

    fpu_push(f80_of_double(2.25));
    CHECK_EQ(cpu->fpu_stack_ptr, 6);
    CHECK(f80_eq(fpu_get_sti(1), f80_of_double(1.5)));
    CHECK(f80_eq(fpu_get_st0(), f80_of_double(2.25)));

    fpu_pop();
    CHECK_EQ(cpu->fpu_stack_ptr, 7);
    CHECK_EQ(cpu->fpu_stack_empty, 0x7F);

    /* reading an EMPTY slot: C1 is CLEARED, the stack fault (SF|I) is raised, and the
     * result is the indefinite NaN (fpu.rs:32-42) */
    fpu_reset();
    cpu->fpu_status_word = FPU_C1;
    CHECK(f80_bits_eq(fpu_get_st0(), F80_INDEFINITE_NAN));
    CHECK_EQ(cpu->fpu_status_word, FPU_EX_SF | FPU_EX_I);

    /* PUSHING onto a full stack: C1 is SET, the stack fault is raised, and the slot is
     * overwritten with the indefinite NaN (fpu.rs:258-271) */
    fpu_reset();
    for (int i = 0; i < 8; i++) {
        fpu_push(f80_of_double(1.0));
    }
    CHECK_EQ(cpu->fpu_stack_empty, 0x00);
    CHECK_EQ(cpu->fpu_stack_ptr, 0);
    cpu->fpu_status_word = 0;
    fpu_push(f80_of_double(9.0));
    CHECK_EQ(cpu->fpu_stack_ptr, 7);
    CHECK_EQ(cpu->fpu_status_word, FPU_C1 | FPU_EX_SF | FPU_EX_I);
    CHECK(f80_bits_eq(fpu_get_st0(), F80_INDEFINITE_NAN));

    set_control_word(0x37F);
    printf("PASS fpu: the tag/status/control words, TOP, push/pop and both stack-fault "
           "paths\n");
}

/* gate 5b -- the softfloat conversions, including the ABI round-trip that softfloat.h's
 * header claims (fpu.rs:88-158, softfloat.rs:93-125) */
static void test_fpu_softfloat_roundtrip(void)
{
    static const uint32_t f32s[] = {
        0x3F800000u, /* 1.0 */
        0xC0490FDBu, /* -3.1415927 */
        0x00000001u, /* the smallest subnormal */
        0x7F800000u, /* +inf */
        0xFF800000u, /* -inf */
        0x7FC00000u, /* a quiet NaN */
        0x00000000u, /* +0 */
        0x80000000u, /* -0 */
    };
    static const uint64_t f64s[] = {
        0x3FF0000000000000ull, /* 1.0 */
        0xC013243F6A8885A3ull, /* -pi */
        0x0000000000000001ull, /* the smallest subnormal */
        0x7FF0000000000000ull, /* +inf */
        0xFFF0000000000000ull, /* -inf */
        0x7FF8000000000000ull, /* a quiet NaN */
        0x0000000000000000ull, /* +0 */
        0x8000000000000000ull, /* -0 */
    };
    unsigned i;

    fpu_reset();

    /* THE ABI PROOF.  f32_to_extF80M/extF80M_to_f32 are DEFINED over a
     * `struct { uint32_t v; }` and declared in softfloat.h as `uint32_t`; if that were
     * not ABI-identical these round trips would corrupt every value. */
    for (i = 0; i < sizeof(f32s) / sizeof(f32s[0]); i++) {
        CHECK_EQ(f80_convert_to_f32(f80_of_f32(f32s[i])), f32s[i]);
    }
    for (i = 0; i < sizeof(f64s) / sizeof(f64s[0]); i++) {
        CHECK_EQ(f80_convert_to_f64(f80_of_f64(f64s[i])), f64s[i]);
    }

    /* the integer conversions and the F80 constants */
    CHECK_EQ(f80_to_i32(f80_of_i32(-123456)), -123456);
    CHECK_EQ(f80_to_i64(f80_of_i64(-0x123456789ABCDEFLL)), -0x123456789ABCDEFLL);
    CHECK(f80_eq(f80_of_double(1.0), F80_ONE));
    CHECK_EQ(f80_as_double(F80_ONE), 1.0);
    CHECK(f80_as_double(F80_LN_2) == 0.6931471805599453);
    CHECK(f80_as_double(F80_LOG2_E) == 1.4426950408889634);
    CHECK(f80_as_double(F80_PI) == 3.141592653589793);
    /* LN_10 / LN_2 is what fldl2t pushes (fpu.rs:151-158): log2(10) */
    CHECK(f80_as_double(f80_div(F80_LN_10, F80_LN_2)) > 3.321928094887362 &&
          f80_as_double(f80_div(F80_LN_10, F80_LN_2)) < 3.321928094887363);

    /* the arithmetic operators route to the engine (softfloat.rs:213-265) */
    CHECK_EQ(f80_as_double(f80_add(f80_of_double(2.0), f80_of_double(3.0))), 5.0);
    CHECK_EQ(f80_as_double(f80_sub(f80_of_double(2.0), f80_of_double(3.0))), -1.0);
    CHECK_EQ(f80_as_double(f80_mul(f80_of_double(2.0), f80_of_double(3.0))), 6.0);
    CHECK_EQ(f80_as_double(f80_div(f80_of_double(10.0), f80_of_double(4.0))), 2.5);
    CHECK_EQ(f80_as_double(f80_sqrt(f80_of_double(4.0))), 2.0);
    CHECK_EQ(f80_as_double(f80_rem(f80_of_double(7.0), f80_of_double(3.0))), 1.0);
    CHECK_EQ(f80_as_double(f80_rem(f80_of_double(-7.0), f80_of_double(3.0))), -1.0);
    CHECK(f80_eq(f80_neg(f80_of_double(2.0)), f80_of_double(-2.0)));
    CHECK(f80_eq(f80_abs(f80_of_double(-2.0)), f80_of_double(2.0)));

    printf("PASS fpu: softfloat conversions -- f32/f64/i32/i64 round trips (the ABI "
           "claim), the F80 constants and the arithmetic operators\n");
}

/* gate 5c -- the environment save/restore and FXSAVE/FXRSTOR (fpu.rs:412-439, 521-580,
 * misc_instr.rs:369-428) */
static void test_fpu_env_save_restore(void)
{
    int32_t addr = (int32_t)FPU_SCRATCH;
    uint16_t saved_cw, saved_sw;
    int32_t saved_tag;

    fpu_reset();

    /* a distinctive state: cw with a non-default rounding and precision, TOP=3, some
     * status bits, and slots 3..7 NON-EMPTY (so the tag word is not degenerate) */
    set_control_word(0x063F); /* rc=1 (floor), pc=2 (64-bit) -- see the derivation below */
    cpu->fpu_status_word = FPU_C0 | FPU_C2 | 0x3F;
    cpu->fpu_stack_ptr = 3;
    for (int i = 0; i < 8; i++) {
        cpu->fpu_st[i] = F80_ZERO;
    }
    for (int i = 3; i < 8; i++) {
        cpu->fpu_st[i] = f80_of_double((double)i + 0.5);
    }
    cpu->fpu_stack_empty = 0x07; /* slots 0,1,2 empty; 3..7 valid */
    cpu->fpu_ip = 0x12345678;
    cpu->fpu_ip_selector = 0x33;
    cpu->fpu_opcode = 0x9B;
    cpu->fpu_dp = 0x0BADF00D;
    cpu->fpu_dp_selector = 0x2B;

    saved_cw = cpu->fpu_control_word;
    saved_sw = cpu->fpu_status_word;
    saved_tag = fpu_load_tag_word();

    /* fstenv32 / fldenv32 round trip */
    fpu_fstenv32(addr);
    CHECK_EQ(safe_read16(addr + 0) & 0xFFFF, saved_cw);
    CHECK_EQ(safe_read16(addr + 4) & 0xFFFF, (saved_sw & ~(7 << 11)) | (3 << 11));
    CHECK_EQ(safe_read32s(addr + 12), 0x12345678);
    CHECK_EQ(safe_read16(addr + 18), 0x9B);
    CHECK_EQ(safe_read32s(addr + 20), 0x0BADF00D);

    fpu_finit();
    CHECK_EQ(cpu->fpu_control_word, 0x37F);
    CHECK_EQ(cpu->fpu_stack_ptr, 0);

    fpu_fldenv32(addr);
    CHECK_EQ(cpu->fpu_control_word, saved_cw);
    CHECK_EQ(cpu->fpu_status_word, saved_sw & ~(7 << 11));
    CHECK_EQ(cpu->fpu_stack_ptr, 3);
    CHECK_EQ(cpu->fpu_ip, 0x12345678);
    CHECK_EQ(cpu->fpu_ip_selector, 0x33);
    CHECK_EQ(cpu->fpu_opcode, 0x9B);
    CHECK_EQ(cpu->fpu_dp, 0x0BADF00D);
    CHECK_EQ(cpu->fpu_dp_selector, 0x2B);
    CHECK_EQ(fpu_load_tag_word(), saved_tag);
    CHECK(f80_eq(fpu_get_st0(), f80_of_double(3.5)));
    /* the control word restored the ENGINE's mode too, not just the stored word */
    CHECK_EQ(softfloat_roundingMode, F80_ROUND_MIN);
    CHECK_EQ(extF80_roundingPrecision, F80_PREC_P64);

    /* fsave32 / frstor32: the 108-byte image, and fsave's implicit finit */
    fpu_fsave32(addr);
    CHECK_EQ(cpu->fpu_control_word, 0x37F); /* finit ran */
    CHECK_EQ(cpu->fpu_stack_empty, 0xFF);
    CHECK_EQ(cpu->fpu_stack_ptr, 0);
    CHECK_EQ(safe_read64s(addr + 28) != 0, 1); /* the first register image is at +28 */

    fpu_frstor32(addr);
    CHECK_EQ(cpu->fpu_control_word, saved_cw);
    CHECK_EQ(cpu->fpu_stack_ptr, 3);
    CHECK(f80_eq(fpu_get_st0(), f80_of_double(3.5)));
    CHECK(f80_eq(fpu_get_sti(1), f80_of_double(4.5)));
    CHECK_EQ(cpu->fpu_ip, 0x12345678);
    CHECK_EQ(fpu_load_tag_word(), saved_tag);

    /* FXSAVE / FXRSTOR (misc_instr.rs:369-428): the whole 512-byte image incl. the xmm
     * registers and mxcsr */
    fpu_reset();
    set_control_word(0x063F);
    cpu->fpu_status_word = FPU_C3 | 0x3F;
    cpu->fpu_stack_ptr = 5;
    cpu->fpu_st[5] = f80_of_double(42.0);
    cpu->fpu_stack_empty = 0x1F;
    cpu->fpu_ip = 0x11112222;
    cpu->fpu_opcode = 0xD9;
    cpu->mxcsr = 0x1F80;
    for (int i = 0; i < 8; i++) {
        write_xmm128_2(i, 0x1000000000000000ull + (uint64_t)i, 0x0F00000000000000ull + (uint64_t)i);
    }

    fxsave(addr);
    CHECK_EQ(safe_read16(addr + 0) & 0xFFFF, 0x063F);
    CHECK_EQ(safe_read8(addr + 4), (int32_t)(~0x1Fu & 0xFF)); /* the abridged FTW */
    CHECK_EQ(safe_read32s(addr + 8), 0x11112222);
    CHECK_EQ(safe_read32s(addr + 24), 0x1F80);
    CHECK_EQ(safe_read32s(addr + 28), MXCSR_MASK);
    CHECK_EQ(safe_read64s(addr + 32 + (0 << 4)), cpu->fpu_st[5].mantissa); /* slot 0 is st(0) */
    CHECK_EQ(safe_read128s(addr + 160).u64[0], 0x1000000000000000ull);
    CHECK_EQ(safe_read128s(addr + 160 + 7 * 16).u64[1], 0x0F00000000000007ull);

    fpu_finit();
    for (int i = 0; i < 8; i++) {
        write_xmm128_2(i, 0, 0);
    }
    cpu->mxcsr = 0;
    cpu->fpu_ip = 0;

    fxrstor(addr);
    CHECK_EQ(cpu->fpu_control_word, 0x063F);
    CHECK_EQ(cpu->fpu_stack_ptr, 5);
    CHECK_EQ(cpu->fpu_stack_empty, 0x1F);
    CHECK_EQ(cpu->fpu_ip, 0x11112222);
    CHECK_EQ(cpu->fpu_opcode, 0xD9);
    CHECK_EQ(cpu->mxcsr, 0x1F80);
    CHECK(f80_eq(fpu_get_st0(), f80_of_double(42.0)));
    CHECK_EQ(cpu->reg_xmm[0].u64[0], 0x1000000000000000ull);
    CHECK_EQ(cpu->reg_xmm[7].u64[1], 0x0F00000000000007ull);

    /* fxrstor's #gp on an invalid mxcsr (misc_instr.rs:401-406) */
    reset_exceptions();
    safe_write32(addr + 24, 0x1F80 | 0x10000);
    fxrstor(addr);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_GP);
    reset_exceptions();

    set_control_word(0x37F);
    printf("PASS fpu: fstenv32/fldenv32, fsave32/frstor32 and FXSAVE/FXRSTOR round trips "
           "(incl. the #gp on an invalid mxcsr)\n");
}

/* gate 5d -- the rounding modes, the precision control, the status bits and the
 * operations that raise them (fpu.rs:166-250, 282-309, 354-415, 677-763) */
static void test_fpu_rounding_and_status(void)
{
    int32_t addr = (int32_t)FPU_SCRATCH + 0x800;
    f80_t two_five = f80_of_double(2.5);
    f80_t neg_two_five = f80_of_double(-2.5);

    fpu_reset();

    /* THE CONTROL WORD -> THE ENGINE'S ROUNDING MODE (fpu.rs:286-296, softfloat.rs:169-178),
     * checked FUNCTIONALLY: ceil(2.5)=3 and floor(-2.5)=-3 pin the odd two arms, and
     * near-even vs trunc need -2.5 -> -2 vs 2.5 -> 2 (they agree), so 2.5 under rc=2
     * (ceil) is the discriminator. */
    set_control_word(0x37F);
    CHECK_EQ(fpu_convert_to_i32(two_five), 2);      /* rc=0 near-even */
    CHECK_EQ(fpu_convert_to_i32(neg_two_five), -2);

    set_control_word(0x37F | (1u << 10));           /* rc=1 floor */
    CHECK_EQ(softfloat_roundingMode, F80_ROUND_MIN);
    CHECK_EQ(fpu_convert_to_i32(two_five), 2);
    CHECK_EQ(fpu_convert_to_i32(neg_two_five), -3);

    set_control_word(0x37F | (2u << 10));           /* rc=2 ceil */
    CHECK_EQ(softfloat_roundingMode, F80_ROUND_MAX);
    CHECK_EQ(fpu_convert_to_i32(two_five), 3);
    CHECK_EQ(fpu_convert_to_i32(neg_two_five), -2);

    set_control_word(0x37F | (3u << 10));           /* rc=3 trunc */
    CHECK_EQ(softfloat_roundingMode, F80_ROUND_MINMAG);
    CHECK_EQ(fpu_convert_to_i32(two_five), 2);
    CHECK_EQ(fpu_convert_to_i32(neg_two_five), -2);
    /* FISTTP is the truncating conversion REGARDLESS of rc (fpu.rs:336-351) */
    fpu_push(two_five);
    fpu_fisttpm32(addr);
    CHECK_EQ(safe_read32s(addr), 2);
    CHECK_EQ(cpu->fpu_stack_ptr, 0); /* it popped (TOP was 7) */

    /* THE PRECISION CONTROL (fpu.rs:298-308): 1.0 + 2^-30 is exact in 80-bit precision
     * and rounds back to exactly 1.0 at 32-bit precision. */
    set_control_word(0x37F); /* pc=3 -> P80 */
    CHECK_EQ(extF80_roundingPrecision, F80_PREC_P80);
    CHECK(!f80_eq(f80_add(F80_ONE, f80_of_double(0x1p-30)), F80_ONE));
    set_control_word(0x37F & ~(3u << 8)); /* pc=0 -> P32 */
    CHECK_EQ(extF80_roundingPrecision, F80_PREC_P32);
    CHECK(f80_eq(f80_add(F80_ONE, f80_of_double(0x1p-30)), F80_ONE));

    set_control_word(0x37F);

    /* fcom's result flags (fpu.rs:189-200) */
    fpu_reset();
    fpu_push(f80_of_double(1.0));
    fpu_push(f80_of_double(2.0)); /* st0 = 2, st1 = 1 */
    fpu_fcom(f80_of_double(1.0));
    CHECK_EQ(cpu->fpu_status_word & FPU_RESULT_FLAGS, 0);
    fpu_fcom(f80_of_double(3.0));
    CHECK_EQ(cpu->fpu_status_word & FPU_RESULT_FLAGS, FPU_C0);
    fpu_fcom(f80_of_double(2.0));
    CHECK_EQ(cpu->fpu_status_word & FPU_RESULT_FLAGS, FPU_C3);
    fpu_fcom(F80_INDEFINITE_NAN);
    CHECK_EQ(cpu->fpu_status_word & FPU_RESULT_FLAGS, FPU_C0 | FPU_C2 | FPU_C3);

    /* fcomi writes the INTEGER flags and re-materialises them (fpu.rs:202-216) */
    fpu_fcomi(1); /* st0=2 vs st1=1 -> greater */
    CHECK_EQ(cpu->flags_changed, 0);
    CHECK_EQ(cpu->flags & FLAGS_ALL, 0);
    fpu_fucomi(1);
    CHECK_EQ(cpu->flags & FLAGS_ALL, 0);
    fpu_write_st(cpu->fpu_stack_ptr, F80_INDEFINITE_NAN); /* st(0) = NaN -> unordered */
    fpu_fucomi(1);
    CHECK_EQ(cpu->flags & FLAGS_ALL, FLAG_CARRY | FLAG_PARITY | FLAG_ZERO);

    /* ftst / fxam (fpu.rs:677-690, 742-763) */
    fpu_reset();
    fpu_push(F80_ZERO);
    cpu->fpu_status_word = FPU_C1;
    fpu_ftst();
    CHECK_EQ(cpu->fpu_status_word & FPU_RESULT_FLAGS, FPU_C3);
    fpu_write_st(cpu->fpu_stack_ptr, f80_of_double(-1.0));
    cpu->fpu_status_word = 0;
    fpu_ftst();
    CHECK_EQ(cpu->fpu_status_word & FPU_RESULT_FLAGS, FPU_C0);
    cpu->fpu_status_word = 0;
    fpu_fxam(); /* finite, nonzero, sign=1 -> C1 and C2 */
    CHECK_EQ(cpu->fpu_status_word & FPU_RESULT_FLAGS, FPU_C1 | FPU_C2);

    /* division by zero raises ZE and yields infinity (fpu.rs:238-243) */
    fpu_reset();
    fpu_push(F80_ONE);
    cpu->fpu_status_word = 0;
    fpu_fdiv(0, F80_ZERO);
    CHECK(cpu->fpu_status_word & FPU_EX_Z);
    CHECK(f80_eq(fpu_get_st0(), F80_POS_INFINITY));

    /* the integer stores: rounding vs truncation, and the #I of an out-of-range store */
    fpu_reset();
    set_control_word(0x37F); /* near-even */
    fpu_push(f80_of_double(2.5));
    fpu_fistm32(addr);                 /* rounds -> 2 */
    CHECK_EQ(safe_read32s(addr), 2);
    fpu_pop();
    fpu_push(f80_of_double(-2.5));
    fpu_fistm32(addr);
    CHECK_EQ(safe_read32s(addr), -2);  /* near-even */
    fpu_pop();
    fpu_push(f80_of_double(2.5));
    fpu_fisttpm32(addr);               /* truncates -> 2 */
    CHECK_EQ(safe_read32s(addr), 2);
    CHECK_EQ(cpu->fpu_stack_ptr, 0); /* fisttpm32 popped (TOP was 7) */

    fpu_reset();
    fpu_push(f80_of_double(40000.0));
    cpu->fpu_status_word = 0;
    fpu_fistm16(addr);
    CHECK(cpu->fpu_status_word & FPU_EX_I); /* out of i16 range */
    CHECK_EQ(safe_read16(addr), 0x8000);    /* the indefinite integer */
    fpu_pop();
    fpu_push(f80_of_double(100.0));
    fpu_fistm16(addr);
    CHECK_EQ(safe_read16(addr), 100);

    /* fbstp: packed BCD, low digit pair first, the sign in byte 9 (fpu.rs:644-664) */
    fpu_reset();
    fpu_push(f80_of_double(1234.0));
    writable_or_pagefault(addr, 10);
    fpu_fbstp(addr);
    CHECK_EQ(safe_read8(addr + 0), 0x34);
    CHECK_EQ(safe_read8(addr + 1), 0x12);
    CHECK_EQ(safe_read8(addr + 2), 0x00);
    CHECK_EQ(safe_read8(addr + 9), 0x00);
    CHECK_EQ(cpu->fpu_stack_ptr, 0);  /* it popped (TOP was 7) */
    fpu_push(f80_of_double(-5678.0));
    fpu_fbstp(addr);
    CHECK_EQ(safe_read8(addr + 0), 0x78);
    CHECK_EQ(safe_read8(addr + 1), 0x56);
    CHECK_EQ(safe_read8(addr + 9), 0x80);

    /* a few of the transcendentals the nasm corpus exercises in full */
    fpu_reset();
    fpu_push(f80_of_double(4.0));
    fpu_fsqrt();
    CHECK_EQ(f80_as_double(fpu_get_st0()), 2.0);
    fpu_write_st(cpu->fpu_stack_ptr, F80_ZERO);
    fpu_f2xm1();                        /* 2^0 - 1 = 0 */
    CHECK(f80_eq(fpu_get_st0(), F80_ZERO));
    fpu_reset();
    fpu_push(f80_of_double(3.0));
    fpu_push(f80_of_double(7.0));       /* st0 = 7, st1 = 3 */
    set_control_word(0x37F);
    fpu_fprem(false);
    CHECK(f80_eq(fpu_get_st0(), f80_of_double(1.0)));       /* 7 mod 3 */
    /* the quotient is 2: bit 0 -> C1, bit 1 -> C3, bit 2 -> C0 (fpu.rs:498-506) */
    CHECK_EQ(cpu->fpu_status_word & (FPU_C0 | FPU_C1 | FPU_C3), FPU_C3);

    set_control_word(0x37F);
    printf("PASS fpu: rounding modes, precision control, the result/exception status "
           "bits, the integer stores and fprem/fbstp/f2xm1/fsqrt\n");
}

/* ------------------------------------------------------------------ *
 * gate 11 -- the MMX/SSE groups of the 0F table (M1b-2 / HANDOFF=v86c)
 * ------------------------------------------------------------------ */

static void test_instr_0f_mmxsse(void)
{
    /* softfloat_exceptionFlags is a global accumulator that upstream's engine raises on
     * every operation; nothing in the guest state reads it, but test_fpu_words_and_stack
     * asserts it is 0, so this test restores it rather than leaving its own conversions'
     * flags behind for a later test to trip over. */
    int32_t saved_exception_flags = softfloat_exceptionFlags;

    /* ---- 1. THE ALIASING (the trap this unit was warned about) ----
     *
     * MMX registers ARE the x87 register file's low 64 bits (cpu.rs:4162-4173;
     * cpu_state.c:50-57 -- read_mmx64s is `fpu_st[r].mantissa`).  An MMX write therefore
     * does not merely store bytes: it runs transition_fpu_to_mmx, which sets the tag word
     * to "all eight slots valid" and re-anchors TOP to 0 (cpu.rs:4201-4207).  These checks
     * pin BOTH halves of that -- the register file is shared, and the tag/TOP state records
     * which reading of it is in force. */
    string_setup();
    CHECK_EQ(cpu->fpu_stack_empty, 0xFF);
    CHECK_EQ(cpu->fpu_stack_ptr, 0);

    fpu_push(F80_ONE); /* one x87 value: TOP moves to 7 and slot 7 is marked non-empty */
    CHECK_EQ(cpu->fpu_stack_ptr, 7);
    CHECK_EQ(cpu->fpu_stack_empty, 0x7F);

    /* the tag word BEFORE the MMX write: slots 0-6 empty (3), slot 7 the pushed value (0) */
    CHECK_EQ(fpu_load_tag_word(), 0x3FFF);
    write_reg32(EAX, 0x11223344);
    instr_0F6E_reg(EAX, 3); /* movd mm3, eax */
    CHECK_EQ(read_mmx64s(3), 0x11223344ull);
    CHECK_EQ(cpu->fpu_stack_ptr, 0);      /* the MMX transition re-anchored TOP */
    CHECK_EQ(cpu->fpu_stack_empty, 0x00); /* and marked all eight slots valid */
    /* the transition does NOT clear the register file: the x87 value pushed a moment ago
     * is still there, and reads as mm7 */
    CHECK_EQ(read_mmx64s(7), 0x8000000000000000ull); /* F80_ONE's mantissa */
    /* and AFTER the MMX write: slot 3 -- the MMX register -- reads VALID (0) where it
     * read EMPTY (3) a moment ago, while slot 7 keeps its x87 value.  MEASURED, and worth
     * stating exactly: upstream's own tag computation calls an MMX value "valid" because
     * softfloat's is_finite is the weak `x != POS_INFINITY && x != NEG_INFINITY`
     * (softfloat.c:240-246), and f80_eq(x, F80_ZERO) is false for MMX's 0xFFFF exponent.
     * Slots 0-2 and 4-6 read "zero" (1): they are the untouched F80_ZERO of the reset. */
    CHECK_EQ(fpu_load_tag_word(), 0x1515);
    CHECK_EQ((fpu_load_tag_word() >> 6) & 3, 0);
    CHECK_EQ((fpu_load_tag_word() >> 14) & 3, 0);

    /* x87 AFTER MMX: a pop marks slot 0 empty and moves TOP to 1 -- from the x87 side,
     * slot 0 (which the MMX write had declared valid) becomes empty again */
    fpu_pop();
    CHECK_EQ(cpu->fpu_stack_ptr, 1);
    CHECK_EQ(cpu->fpu_stack_empty, 0x01);

    /* MMX AFTER x87: an x87 push writes slot 0 and mm0 sees it */
    fpu_push(F80_ONE);
    CHECK_EQ(cpu->fpu_stack_ptr, 0);
    CHECK_EQ(cpu->fpu_stack_empty, 0x00);
    CHECK_EQ(read_mmx64s(0), 0x8000000000000000ull);
    CHECK_EQ(fpu_get_sti_f64(0), 1.0);
    CHECK_EQ(read_mmx64s(3), 0x11223344ull); /* the x87 push did not disturb mm3 */

    /* emms (0F 77): the tag word to ALL EMPTY with the register content PRESERVED -- the
     * opposite end of the same transition, and the reason it is called with 0xFFFF here
     * and 0 there */
    instr_0F77();
    CHECK_EQ(cpu->fpu_stack_empty, 0xFF);
    CHECK_EQ(fpu_load_tag_word(), 0xFFFF);
    CHECK_EQ(cpu->fpu_stack_ptr, 0); /* emms moves the tag word only, not TOP */
    CHECK_EQ(read_mmx64s(3), 0x11223344ull);
    CHECK_EQ(read_mmx64s(0), 0x8000000000000000ull);

    /* ---- 2. the mov family, movmsk and the shuffles ---- */
    string_setup();
    write_xmm128(1, 0x11111111, 0x22222222, 0x33333333, 0x44444444);
    write_xmm128(2, 0, 0, 0, 0);
    instr_660F28_reg(1, 2); /* movapd xmm2, xmm1 */
    CHECK_EQ(read_xmm128s(2).u32[0], 0x11111111);
    CHECK_EQ(read_xmm128s(2).u32[3], 0x44444444);
    CHECK_EQ(read_xmm128s(1).u32[0], 0x11111111); /* the source is untouched */
    instr_0F10_reg(2, 1); /* movups xmm1, xmm2 */
    CHECK_EQ(read_xmm128s(1).u32[0], 0x11111111);
    CHECK_EQ(read_xmm128s(1).u32[1], 0x22222222);

    /* movmskps/movmskpd: bit i is the SIGN of dword i (of dwords 1 and 3 only, for pd) */
    write_xmm128(1, (int32_t)0x80000000u, (int32_t)0x80000000u, (int32_t)0xFFFFFFFFu, 0);
    write_reg32(EAX, 0x55555555);
    instr_0F50_reg(1, EAX);
    CHECK_EQ(read_reg32(EAX), 1 | 2 | 4); /* the sign of dwords 0, 1 and 2 */
    instr_660F50_reg(1, EAX);
    CHECK_EQ(read_reg32(EAX), 1); /* only dwords 1 and 3 are consulted; dword 3 is 0 */
    reset_exceptions();
    instr_0F50_mem(0x1000, EAX);
    CHECK_EQ(exc_vector, CPU_EXCEPTION_UD); /* the memory form is #UD */

    /* pshufd (660F70): imm8 selects a dword per lane, the low two bits first */
    write_xmm128(1, 0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC, 0xDDDDDDDD);
    instr_660F70_reg(1, 2, 0xE4); /* 11 10 01 00 -> the identity */
    CHECK_EQ(read_xmm128s(2).u32[0], 0xAAAAAAAAu);
    CHECK_EQ(read_xmm128s(2).u32[3], 0xDDDDDDDDu);
    instr_660F70_reg(1, 2, 0x00); /* every lane from dword 0 */
    CHECK_EQ(read_xmm128s(2).u32[0], 0xAAAAAAAAu);
    CHECK_EQ(read_xmm128s(2).u32[1], 0xAAAAAAAAu);
    CHECK_EQ(read_xmm128s(2).u32[3], 0xAAAAAAAAu);
    instr_660F70_reg(1, 2, 0x1B); /* 00 01 10 11 -> reversed */
    CHECK_EQ(read_xmm128s(2).u32[0], 0xDDDDDDDDu);
    CHECK_EQ(read_xmm128s(2).u32[1], 0xCCCCCCCCu);
    CHECK_EQ(read_xmm128s(2).u32[2], 0xBBBBBBBBu);
    CHECK_EQ(read_xmm128s(2).u32[3], 0xAAAAAAAAu);

    /* pshufw (0F70, MMX): the same selection over the four WORD lanes, and the MMX form
     * runs the aliasing transition */
    write_mmx_reg64(1, 0x0004000300020001ull);
    instr_0F70_reg(1, 2, 0xE4);
    CHECK_EQ(read_mmx64s(2), 0x0004000300020001ull);
    instr_0F70_reg(1, 2, 0x00);
    CHECK_EQ(read_mmx64s(2), 0x0001000100010001ull);
    CHECK_EQ(cpu->fpu_stack_empty, 0x00);
    CHECK_EQ(cpu->fpu_stack_ptr, 0);

    /* pshuflw (F20F70) takes the LOW half of the source from the immediate and copies the
     * HIGH half through unchanged */
    write_xmm128(1, 0x44443333, 0x22221111, 0x88887777, 0x66665555);
    instr_F20F70_reg(1, 2, 0xE4); /* lanes u16[0],u16[1] | u16[2],u16[3]: the identity */
    CHECK_EQ(read_xmm128s(2).u32[0], 0x44443333u);
    CHECK_EQ(read_xmm128s(2).u32[1], 0x22221111u);
    CHECK_EQ(read_xmm128s(2).u32[2], 0x88887777u); /* the high half is untouched */
    CHECK_EQ(read_xmm128s(2).u32[3], 0x66665555u);
    instr_F20F70_reg(1, 2, 0x00); /* every low word from u16[0] */
    CHECK_EQ(read_xmm128s(2).u32[0], 0x33333333u);
    CHECK_EQ(read_xmm128s(2).u32[1], 0x33333333u);

    /* ---- 3. punpck, pack, the compare-masks, and the wraparounds ---- */
    string_setup();
    /* punpcklbw xmm (660F60): interleave the LOW 8 bytes of each, destination first */
    write_xmm128(2, 0x0F0E0D0C, 0x0B0A0908, 0x07060504, 0x03020100);
    write_xmm128(1, 0xF0E0D0C0, 0xB0A09080, 0x70605040, 0x30201000);
    instr_660F60_reg(1, 2);
    CHECK_EQ(read_xmm128s(2).u8[0], 0x0C); /* dest byte 0 */
    CHECK_EQ(read_xmm128s(2).u8[1], 0xC0); /* src  byte 0 */
    CHECK_EQ(read_xmm128s(2).u8[2], 0x0D);
    CHECK_EQ(read_xmm128s(2).u8[3], 0xD0);
    CHECK_EQ(read_xmm128s(2).u8[14], 0x0B); /* dest byte 7 */
    CHECK_EQ(read_xmm128s(2).u8[15], 0xB0); /* src  byte 7 */

    /* punpckhbw xmm (660F68): the same over the HIGH 8 bytes */
    write_xmm128(2, 0x0F0E0D0C, 0x0B0A0908, 0x07060504, 0x03020100);
    write_xmm128(1, 0xF0E0D0C0, 0xB0A09080, 0x70605040, 0x30201000);
    instr_660F68_reg(1, 2);
    CHECK_EQ(read_xmm128s(2).u8[0], 0x04);  /* dest byte 8 */
    CHECK_EQ(read_xmm128s(2).u8[1], 0x40);  /* src  byte 8 */
    CHECK_EQ(read_xmm128s(2).u8[14], 0x03); /* dest byte 15 */
    CHECK_EQ(read_xmm128s(2).u8[15], 0x30); /* src  byte 15 */

    /* packsswb (660F63): SIGNED saturation of each operand's eight u16 lanes */
    write_xmm128(2, 0x00000100, 0x0000FF00, 0x0000007F, 0xFFFFFF80);
    write_xmm128(1, 0x0000FFFF, 0x00008000, 0x00000005, 0x00000000);
    instr_660F63_reg(1, 2);
    CHECK_EQ(read_xmm128s(2).i8[0], 127);   /* dest u16 0x0100 = 256 -> +127 */
    CHECK_EQ(read_xmm128s(2).i8[1], 0);     /* dest u16 0x0000 */
    CHECK_EQ(read_xmm128s(2).i8[2], -128);  /* dest u16 0xFF00 = -256 -> -128 */
    CHECK_EQ(read_xmm128s(2).i8[3], 0);
    CHECK_EQ(read_xmm128s(2).i8[4], 127);   /* dest u16 0x007F */
    CHECK_EQ(read_xmm128s(2).i8[6], -128);  /* dest u16 0xFF80 = -128 */
    CHECK_EQ(read_xmm128s(2).i8[7], -1);    /* dest u16 0xFFFF = -1 */
    CHECK_EQ(read_xmm128s(2).i8[8], -1);    /* src u16 0xFFFF */
    CHECK_EQ(read_xmm128s(2).i8[10], -128); /* src u16 0x8000 = -32768 */
    CHECK_EQ(read_xmm128s(2).i8[12], 5);    /* src u16 0x0005 */
    CHECK_EQ(read_xmm128s(2).i8[13], 0);

    /* packuswb (660F67): UNSIGNED saturation, so a negative u16 lane becomes 0 */
    write_xmm128(2, 0x00000100, 0x0000FF00, 0x0000007F, 0x00000000);
    write_xmm128(1, 0x0000FFFF, 0x00008000, 0x00000005, 0x00000000);
    instr_660F67_reg(1, 2);
    CHECK_EQ(read_xmm128s(2).u8[0], 255);  /* dest u16 0x0100 */
    CHECK_EQ(read_xmm128s(2).u8[1], 0);    /* dest u16 0x0000 */
    CHECK_EQ(read_xmm128s(2).u8[2], 0);    /* dest u16 0xFF00 is negative -> 0 */
    CHECK_EQ(read_xmm128s(2).u8[4], 127);  /* dest u16 0x007F */
    CHECK_EQ(read_xmm128s(2).u8[8], 0);    /* src u16 0xFFFF -> 0 */
    CHECK_EQ(read_xmm128s(2).u8[12], 5);   /* src u16 0x0005 */

    /* pcmpeqb (660F74): an all-ones mask where the bytes are equal */
    write_xmm128(1, 0x01010101, 0x01010101, 0x01010101, 0x01010101); /* src: all 0x01 */
    write_xmm128(2, 0x00010101, 0x01010101, 0x01010101, 0x01010101); /* dest: byte 3 = 0 */
    instr_660F74_reg(1, 2);
    CHECK_EQ(read_xmm128s(2).u8[0], 0xFF);
    CHECK_EQ(read_xmm128s(2).u8[2], 0xFF);
    CHECK_EQ(read_xmm128s(2).u8[3], 0x00);
    CHECK_EQ(read_xmm128s(2).u8[15], 0xFF);

    /* pcmpgtb (660F64) is SIGNED: the destination byte 0x80 (-128) is NOT greater than the
     * source byte 0x00, although it is greater as an unsigned byte -- which is the check
     * that the i8 view, not the u8 view, is the one compared */
    write_xmm128(1, 0x00000001, 0, 0, 0); /* src bytes: 01 00 00 00 ... */
    write_xmm128(2, 0x7F800302, 0, 0, 0); /* dest bytes: 02 03 80 7F ... */
    instr_660F64_reg(1, 2);
    CHECK_EQ(read_xmm128s(2).u8[0], 0xFF); /* 2 > 1 */
    CHECK_EQ(read_xmm128s(2).u8[1], 0xFF); /* 3 > 0 */
    CHECK_EQ(read_xmm128s(2).u8[2], 0x00); /* -128 > 0 is FALSE (0x80 > 0 unsigned would be true) */
    CHECK_EQ(read_xmm128s(2).u8[3], 0xFF); /* 127 > 0 */

    /* paddd (660FFE) WRAPS instead of saturating: the reference's release profile makes
     * `+` two's-complement, and C's signed `+` would be UB, so this checks the uint32_t
     * spelling is in place */
    write_xmm128(2, (int32_t)0x7FFFFFFFu, 0, 0, 0);
    write_xmm128(1, 1, 0, 0, 0);
    instr_660FFE_reg(1, 2);
    CHECK_EQ(read_xmm128s(2).i32[0], (int32_t)0x80000000u);
    write_xmm128(2, 0, 0, 0, (int32_t)0xFFFFFFFFu);
    write_xmm128(1, 0, 0, 0, 2);
    instr_660FFE_reg(1, 2);
    CHECK_EQ(read_xmm128s(2).u32[3], 1u);

    /* pmaddwd (660FF5): the two products of each 64-bit half are SUMMED, and the sum can
     * reach the sign bit (2 * 2^30), so it too is formed on the uint32_t pattern */
    write_xmm128(2, (int32_t)0x80008000u, 0, 0, 0); /* i16 lanes 0 and 1 = -32768 */
    write_xmm128(1, (int32_t)0x80008000u, 0, 0, 0);
    instr_660FF5_reg(1, 2);
    CHECK_EQ(read_xmm128s(2).i32[0], (int32_t)0x80000000u); /* 2 * 2^30 wraps to INT_MIN */
    CHECK_EQ(read_xmm128s(2).i32[1], 0);
    /* the ordinary case, which must not have been broken by the uint32_t spelling */
    write_xmm128(2, 0x00010002, 0, 0, 0); /* i16 lanes: 2, 1 */
    write_xmm128(1, 0x00030004, 0, 0, 0); /* i16 lanes: 4, 3 */
    instr_660FF5_reg(1, 2);
    CHECK_EQ(read_xmm128s(2).i32[0], 2 * 4 + 1 * 3);

    /* pmulhuw (660FE4): the product of two large lanes does not fit an i32 (0xFFFF*0xFFFF
     * = 4294836225); the port forms it on the uint32_t pattern and keeps the high 16 bits.
     * 0xFFFF * 0xFFFF -> 0xFFFE, which is the exact pmulhuw result too -- a shift right by
     * 16 differs between the arithmetic and logical readings only above bit 15. */
    write_xmm128(2, (int32_t)0xFFFFFFFFu, (int32_t)0xFFFFFFFFu, 0, 0);
    write_xmm128(1, (int32_t)0xFFFFFFFFu, (int32_t)0xFFFFFFFFu, 0, 0);
    instr_660FE4_reg(1, 2);
    CHECK_EQ(read_xmm128s(2).u16[0], 0xFFFE);
    CHECK_EQ(read_xmm128s(2).u16[3], 0xFFFE);
    /* and a small case where no wrapping is involved */
    write_xmm128(2, 0x00001000, 0, 0, 0); /* u16 lane 0 = 0x1000 */
    write_xmm128(1, 0x00000010, 0, 0, 0); /* u16 lane 0 = 0x0010 */
    instr_660FE4_reg(1, 2);
    CHECK_EQ(read_xmm128s(2).u16[0], 1); /* 0x1000 * 0x0010 = 0x10000, >> 16 = 1 */
    CHECK_EQ(read_xmm128s(2).u16[1], 0); /* 0 * 0 */

    /* psadbw (660FF6): the sum of the absolute byte differences, per 64-bit half */
    write_xmm128(2, 0x05050505, 0x05050505, 0x00000000, 0x00000000);
    write_xmm128(1, 0x00000000, 0x00000000, 0x00000000, 0x00000000);
    instr_660FF6_reg(1, 2);
    CHECK_EQ(read_xmm128s(2).i32[0], 8 * 5); /* eight bytes, each |5 - 0| */
    CHECK_EQ(read_xmm128s(2).i32[1], 0);
    CHECK_EQ(read_xmm128s(2).i32[2], 0); /* the high half is all zero on both sides */
    CHECK_EQ(read_xmm128s(2).i32[3], 0);

    /* the MMX forms (0F D4-D5, FE, FA, F5, E4): the same lane arithmetic on the SHARED
     * register file, followed by the aliasing transition.  They are not redundant with the
     * 66-prefixed forms above -- they are separate bodies in the port and in upstream. */
    string_setup();
    write_mmx_reg64(2, (uint64_t)(uint32_t)0x7FFFFFFF | 0x7FFFFFFFull << 32);
    write_mmx_reg64(1, 0x0000000100000001ull);
    instr_0FFE_reg(2, 1); /* paddd mm1, mm2 */
    CHECK_EQ(read_mmx64s(1), 0x8000000080000000ull); /* each lane wrapped to INT_MIN */
    CHECK_EQ(cpu->fpu_stack_empty, 0x00);            /* the MMX form ran the transition */

    write_mmx_reg64(2, 0x0000000100000001ull); /* source */
    write_mmx_reg64(1, 0x0000000000000000ull); /* destination */
    instr_0FFA_reg(2, 1);                      /* psubd mm1, mm2 */
    CHECK_EQ(read_mmx64s(1), 0xFFFFFFFFFFFFFFFFull); /* 0 - 1 = -1 in each lane */

    write_mmx_reg64(2, (uint64_t)(uint32_t)0xFFFFFFFFu | (uint64_t)(uint32_t)0xFFFFFFFFu << 32);
    instr_0FE4_reg(2, 1); /* pmulhuw mm1, mm2 */
    /* every lane: 0xFFFF * 0xFFFF = 0xFFFE0001 as a 32-bit product, high half 0xFFFE */
    CHECK_EQ(read_mmx64s(1), 0xFFFEFFFEFFFEFFFEull);

    write_mmx_reg64(2, 0x8000800080008000ull); /* source: four lanes of -32768 */
    write_mmx_reg64(1, 0x8000800080008000ull); /* destination: the same */
    instr_0FF5_reg(2, 1);                      /* pmaddwd mm1, mm2 */
    CHECK_EQ(read_mmx64s(1), 0x8000000080000000ull); /* 2 * 2^30 wraps in each 64-bit half */

    /* pmullw mm1, mm2 takes the LOW 16 bits of each product, which for these lanes is
     * exactly the two's-complement truncation C's narrowing store gives for free */
    write_mmx_reg64(2, 0x000000007FFF8000ull); /* source lanes: 0x8000 (-32768), 0x7FFF */
    write_mmx_reg64(1, 0x00000000FFFFFFFFull); /* dest lanes:   0xFFFF (-1), 0xFFFF */
    instr_0FD5_reg(2, 1);
    CHECK_EQ(read_mmx64s(1), 0x0000000080018000ull); /* 32768 -> 0x8000; -32767 -> 0x8001 */

    /* ---- 4. the shifts, including the per-instruction CLAMP ---- */
    string_setup();
    write_mmx_reg64(2, 0xFFFFFFFFFFFFFFFFull);
    instr_0F71_2_reg(2, 20); /* psrlw mm, 20 -> clamped to 16 -> every lane 0 */
    CHECK_EQ(read_mmx64s(2), 0ull);
    CHECK_EQ(cpu->fpu_stack_empty, 0x00); /* the MMX shift ran the transition */

    write_mmx_reg64(2, 0x8000800080008000ull); /* four lanes of -32768 */
    instr_0F71_4_reg(2, 20); /* psraw mm, 20 -> the clamp to 16 SIGN-FILLS */
    CHECK_EQ(read_mmx64s(2), 0xFFFFFFFFFFFFFFFFull);

    write_mmx_reg64(2, 0xFFFFFFFFFFFFFFFFull);
    instr_0F71_6_reg(2, 20); /* psllw mm, 20 -> out of range -> ZERO, not a clamp */
    CHECK_EQ(read_mmx64s(2), 0ull);

    write_mmx_reg64(2, 0xFFFFFFFFFFFFFFFFull);
    instr_0F72_6_reg(2, 32); /* pslld mm, 32 -> out of range -> zero */
    CHECK_EQ(read_mmx64s(2), 0ull);
    write_mmx_reg64(2, 0xFFFFFFFFFFFFFFFFull);
    instr_0F72_2_reg(2, 32); /* psrld mm, 32 -> out of range -> zero */
    CHECK_EQ(read_mmx64s(2), 0ull);
    write_mmx_reg64(2, 0x8000000080000000ull);
    instr_0F72_4_reg(2, 40); /* psrad mm, 40 -> clamped to 31 -> sign fill */
    CHECK_EQ(read_mmx64s(2), 0xFFFFFFFFFFFFFFFFull);

    write_mmx_reg64(2, 0xFFFFFFFFFFFFFFFFull);
    instr_0F73_2_reg(2, 64); /* psrlq mm, 64 -> out of range -> zero */
    CHECK_EQ(read_mmx64s(2), 0ull);
    write_mmx_reg64(2, 0xFFFFFFFFFFFFFFFFull);
    instr_0F73_6_reg(2, 64); /* psllq mm, 64 -> out of range -> zero */
    CHECK_EQ(read_mmx64s(2), 0ull);

    /* psrldq (660F73 /3): the shift is imm8*8 bits, and imm8 == 0 returns EARLY -- which
     * is what keeps the `64 - shift` in the byte-crossing arm defined */
    write_xmm128(2, 0x11111111, 0x22222222, 0x33333333, 0x44444444);
    instr_660F73_3_reg(2, 0); /* imm8 == 0: unchanged */
    CHECK_EQ(read_xmm128s(2).u32[0], 0x11111111);
    CHECK_EQ(read_xmm128s(2).u32[3], 0x44444444);
    instr_660F73_3_reg(2, 8); /* shift 64: the high 64 bits move into the low half */
    CHECK_EQ(read_xmm128s(2).u32[0], 0x33333333);
    CHECK_EQ(read_xmm128s(2).u32[1], 0x44444444);
    CHECK_EQ(read_xmm128s(2).u32[2], 0);
    CHECK_EQ(read_xmm128s(2).u32[3], 0);
    instr_660F73_3_reg(2, 2); /* shift 16: the byte-crossing arm */
    /* u64[0] = 0x4444444433333333 >> 16 | u64[1] << 48 = 0x0000444444443333 */
    CHECK_EQ(read_xmm128s(2).u32[0], 0x44443333);
    CHECK_EQ(read_xmm128s(2).u32[1], 0x00004444);
    CHECK_EQ(read_xmm128s(2).u32[2], 0);
    CHECK_EQ(read_xmm128s(2).u32[3], 0);

    write_xmm128(2, 0x11111111, 0x22222222, 0x33333333, 0x44444444);
    instr_660F73_7_reg(2, 8); /* pslldq by 64 */
    CHECK_EQ(read_xmm128s(2).u32[0], 0);
    CHECK_EQ(read_xmm128s(2).u32[2], 0x11111111);
    CHECK_EQ(read_xmm128s(2).u32[3], 0x22222222);
    write_xmm128(2, 0x11111111, 0x22222222, 0x33333333, 0x44444444);
    instr_660F73_7_reg(2, 16); /* imm8 > 15 -> shift 128 -> nothing shifts in: zeroed */
    CHECK_EQ(read_xmm128s(2).u32[0], 0);
    CHECK_EQ(read_xmm128s(2).u32[3], 0);

    /* ---- 5. the comparisons ---- */
    string_setup();
    /* cmpps (0FC2) op 0 = equal, op 4 = not-equal OR unordered: -1 or 0 per lane */
    write_xmm128(1, 0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000); /* 1.0 four times */
    write_xmm128(2, 0x3F800000, 0x40000000, 0x40400000, 0x40800000); /* 1.0 2.0 3.0 4.0 */
    instr_0FC2_reg(1, 2, 0);
    CHECK_EQ(read_xmm128s(2).i32[0], -1); /* 1.0 == 1.0 */
    CHECK_EQ(read_xmm128s(2).i32[1], 0);
    CHECK_EQ(read_xmm128s(2).i32[3], 0);
    write_xmm128(2, 0x3F800000, 0x40000000, 0x40400000, 0x40800000);
    instr_0FC2_reg(1, 2, 4); /* not-equal OR unordered */
    CHECK_EQ(read_xmm128s(2).i32[0], 0);
    CHECK_EQ(read_xmm128s(2).i32[1], -1);
    CHECK_EQ(read_xmm128s(2).i32[2], -1);
    CHECK_EQ(read_xmm128s(2).i32[3], -1);
    /* cmpsd (F20FC2) writes a 64-BIT all-ones in the low half only */
    write_xmm128_2(1, 0x3FF0000000000000ull, 0); /* 1.0 */
    write_xmm128_2(2, 0x3FF0000000000000ull, 0);
    instr_F20FC2_reg(1, 2, 0);
    CHECK_EQ(read_xmm128s(2).u64[0], 0xFFFFFFFFFFFFFFFFull);
    CHECK_EQ(read_xmm128s(2).u64[1], 0ull); /* the high half is untouched */

    /* comiss/ucomiss: ZF for equal, CF for less-than-below, ZF|PF|CF for unordered */
    cpu->flags = FLAGS_DEFAULT;
    write_xmm128(2, 0x3F800000, 0, 0, 0); /* destination 1.0 */
    write_xmm128(1, 0x3F800000, 0, 0, 0); /* source      1.0 */
    instr_0F2F_reg(1, 2); /* comiss xmm2, xmm1 */
    CHECK_EQ(cpu->flags_changed, 0);
    CHECK(getzf());
    CHECK(!getcf());
    CHECK(!getpf());
    cpu->flags = FLAGS_DEFAULT;
    write_xmm128(1, 0x40000000, 0, 0, 0); /* source 2.0; the destination is still 1.0 */
    instr_0F2F_reg(1, 2);
    CHECK(!getzf());
    CHECK(getcf());
    cpu->flags = FLAGS_DEFAULT;
    write_xmm128(1, 0x7FC00000, 0, 0, 0); /* a QNaN source */
    instr_0F2F_reg(1, 2);
    CHECK(getzf() && getpf() && getcf()); /* the unordered case */
    cpu->flags = FLAGS_DEFAULT;
    write_xmm128(1, 0x3F800000, 0, 0, 0);
    instr_0F2E_reg(1, 2); /* ucomiss takes the same path */
    CHECK(getzf() && !getcf() && !getpf());

    /* ---- 6. the conversions, the rounding and the INT_MIN sentinel ---- */
    string_setup();
    write_xmm128(1, (int32_t)0xC0200000u, 0, 0, 0); /* -2.5f */
    write_reg32(EAX, 0x55555555);
    instr_F30F2C_reg(1, EAX); /* cvttss2si: TRUNCATION, toward zero */
    CHECK_EQ(read_reg32(EAX), -2);
    cpu->mxcsr = 0x1F80; /* RC = 0, round to nearest with ties to even */
    write_reg32(EAX, 0x55555555);
    instr_F30F2D_reg(1, EAX); /* cvtss2si: sse_integer_round */
    CHECK_EQ(read_reg32(EAX), -2); /* -2.5 -> -2, not -3 */
    write_xmm128(1, 0x40200000, 0, 0, 0); /* 2.5f */
    write_reg32(EAX, 0);
    instr_F30F2D_reg(1, EAX);
    CHECK_EQ(read_reg32(EAX), 2); /* 2.5 -> 2, not 3 */
    write_xmm128(1, 0x40400000, 0, 0, 0); /* 3.0f */
    write_reg32(EAX, 0);
    instr_F30F2D_reg(1, EAX);
    CHECK_EQ(read_reg32(EAX), 3);
    /* a value that cannot fit gets the INT_MIN sentinel, not a saturated INT_MAX */
    write_xmm128(1, 0x4F000000, 0, 0, 0); /* 2147483648.0f */
    write_reg32(EAX, 0);
    instr_F30F2C_reg(1, EAX);
    CHECK_EQ(read_reg32(EAX), (int32_t)0x80000000u);
    /* the RC bits of mxcsr actually select the mode: rc = 1 (down) floors 2.5 */
    cpu->mxcsr = (1 << MXCSR_RC_SHIFT) | 0x1F80;
    write_xmm128(1, 0x40200000, 0, 0, 0);
    write_reg32(EAX, 0);
    instr_F30F2D_reg(1, EAX);
    CHECK_EQ(read_reg32(EAX), 2); /* floor(2.5) */
    write_xmm128(1, (int32_t)0xC0200000u, 0, 0, 0);
    write_reg32(EAX, 0);
    instr_F30F2D_reg(1, EAX);
    CHECK_EQ(read_reg32(EAX), -3); /* floor(-2.5) */
    cpu->mxcsr = 0x1F80;
    /* and the double form through the same helper */
    write_xmm128_2(1, 0x4004000000000000ull, 0); /* 2.5 */
    write_reg32(EAX, 0);
    instr_F20F2D_reg(1, EAX);
    CHECK_EQ(read_reg32(EAX), 2);
    write_xmm128_2(1, 0xC004000000000000ull, 0); /* -2.5 */
    write_reg32(EAX, 0);
    instr_F20F2C_reg(1, EAX); /* truncating: -2 */
    CHECK_EQ(read_reg32(EAX), -2);

    /* ---- 7. the FAULT surface: the MMX transition must NOT run on a fault ---- */
    paging_fixture_two_pages();
    write_mmx_reg64(3, 0xABCDEF0123456789ull);
    instr_0F6F_reg(3, 4);
    CHECK_EQ(read_mmx64s(4), 0xABCDEF0123456789ull);
    /* 0x402000 has no mapping: the load faults, and neither mm4 nor the tag state moves */
    cpu->fpu_stack_empty = 0xFF;
    cpu->fpu_stack_ptr = 5;
    write_mmx_reg64(4, 0x1111111111111111ull);
    cpu->pagefault = false;
    reset_delivery();
    instr_0F6F_mem(0x402000, 4); /* movq mm4, [0x402000] */
    CHECK(cpu->pagefault);
    CHECK_EQ(delivered_interrupt, CPU_EXCEPTION_PF);
    CHECK_EQ(read_mmx64s(4), 0x1111111111111111ull); /* untouched */
    CHECK_EQ(cpu->fpu_stack_empty, 0xFF);            /* no transition */
    CHECK_EQ(cpu->fpu_stack_ptr, 5);
    /* the same for a store to an absent page: movq [addr], mm */
    cpu->pagefault = false;
    reset_delivery();
    instr_0F7F_mem(0x402000, 3);
    CHECK(cpu->pagefault);
    CHECK_EQ(delivered_interrupt, CPU_EXCEPTION_PF);
    CHECK_EQ(cpu->fpu_stack_empty, 0xFF);
    CHECK_EQ(cpu->fpu_stack_ptr, 5);
    /* and for a 128-bit SSE store */
    cpu->pagefault = false;
    reset_delivery();
    instr_660F7F_mem(0x402000, 3);
    CHECK(cpu->pagefault);
    CHECK_EQ(delivered_interrupt, CPU_EXCEPTION_PF);
    /* a mapped page does NOT fault, and the store lands where the mapping says */
    cpu->pagefault = false;
    instr_0F7F_mem(0x400000, 3); /* 0x400000 -> physical 0x3000 */
    CHECK(!cpu->pagefault);
    CHECK_EQ(read_mmx64s(3), 0xABCDEF0123456789ull);
    CHECK_EQ(mem8[0x3000], 0x89);
    CHECK_EQ(mem8[0x3007], 0xAB);
    CHECK_EQ(cpu->fpu_stack_empty, 0x00); /* THIS one did run the transition */
    CHECK_EQ(cpu->fpu_stack_ptr, 0);

    softfloat_exceptionFlags = saved_exception_flags;
    printf("PASS instr: the MMX/SSE 0F groups -- the x87 aliasing transition, the mov/movmask/"
           "shuffle family, the integer group's wraps and saturations, the shift clamps, the "
           "comparisons, the conversions and the fault surface\n");
}

int main(void)
{
    if (!allocate_memory(MEM_SIZE)) {
        fprintf(stderr, "cpu_selftest: could not allocate %u bytes\n", MEM_SIZE);
        return 1;
    }
    cpu_state_init();
    /* create_memory's job upstream (cpu.js:967-995): memory_size then allocate */
    cpu->memory_size = MEM_SIZE;

    /* M1b-3, THE INIT-VALUE PARITY (plan ss2/R1): neither reset_cpu nor cpu.js writes
     * Berkeley SoftFloat's three knobs, so the x87 rounding and precision the machine
     * starts with are the ones the vendored engine compiles in -- and the reset control
     * word 0x37F (rc=0, pc=3) asks for exactly near-even and precision 80.  This is the
     * FIRST statement after the state is initialised, so it reads those values and not
     * one a test wrote (the 0F AE test calls fxrstor, which calls set_control_word). */
    CHECK_EQ(softfloat_roundingMode, F80_ROUND_NEAR_EVEN);
    CHECK_EQ(extF80_roundingPrecision, F80_PREC_P80);
    CHECK_EQ(softfloat_exceptionFlags, 0);

    test_reset();
    test_flags();
    test_widened_register_file();
    test_widened_flags();
    test_cpu_mode_and_refusals();
    test_address_widening_invariant();
    test_memory_router();
    test_paging_4k();
    test_paging_faults();
    test_paging_pse();
    test_pae_4k();
    test_pae_2mb();
    test_pae_reserved();
    test_pae_pdpte();
    test_pae_nx();
    test_pae_nx_pde();
    test_pae_gp();
    test_cr3_nonpae_mask();
    test_paging_crossing();
    test_tlb();

    test_arith_shift();
    test_arith_muldiv();
    test_arith_bcd();
    test_arith_bits();
    test_modrm_addressing();
    test_modrm_fetch_and_faults();
    test_misc_stack();
    test_misc_jumps();
    test_misc_task_switch();

    test_string_movs();
    test_string_cmps_scas();
    test_string_stos_lods();
    test_string_ins_outs();
    test_instr_alu_memory();
    test_instr_mov_xchg_lea();
    test_instr_control();
    test_instr_flags_and_prefixes();
    test_instr_string_opcodes();
    test_instr_rmw_and_faults();
    test_instr_0f_nonsse();
    test_instr_0f_mmxsse();

    test_cpu_backend();

    test_fpu_words_and_stack();
    test_fpu_softfloat_roundtrip();
    test_fpu_env_save_restore();
    test_fpu_rounding_and_status();

    printf("cpu_selftest: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
