/*
 * cpu_state.h -- the CPU state, the architectural constants and the register
 * accessors.  C port of v86's state block.
 *
 * Reference (READ-ONLY): v86/src/rust/cpu/{global_pointers.rs, cpu.rs, regs.rs},
 * v86/src/rust/state_flags.rs, v86/src/cpu.js:66-218, v86/src/rust/softfloat.rs.
 *
 * DIVERGENCE FROM UPSTREAM (the one the plan's DECISION 2 predicts): upstream keeps
 * the whole state at hardcoded absolute offsets in one flat wasm linear memory
 * (global_pointers.rs:3-65) because the JS half shares that memory.  The C port has
 * no JS peer, so the offsets are simply gone and the members are named.  The members
 * are declared in global_pointers.rs ORDER so that the port stays 1:1 (`*cr.offset(3)`
 * -> `cpu->cr[3]`).  Every constant below mirrors cpu.rs:80-300 verbatim -- one place,
 * like upstream, so later units cannot disagree about a value.
 *
 * NOT in this struct (per the plan, ss2):
 *   - tlb_data        -- a separate 4 MiB array (cpu.rs:329), see paging.c
 *   - mem8            -- a separate allocation (memory.rs:24-38),     see memory.c
 *   - tlb_code, Code  -- JIT-only, dropped (interpreter-only port)
 */

#ifndef V86C_CPU_STATE_H
#define V86C_CPU_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Upstream's `dbg_assert!` (dbg.rs:63) is `debug_assert!`: active only when
 * debug_assertions are on, i.e. never in the shipped wasm (handoff-v86c-result, NOTE
 * FOR LATER UNITS: reading the release wasm's silence as a satisfied invariant is a
 * trap).  The C port keeps the checks in non-NDEBUG builds -- the selftest runs with
 * them on, which is how the flags invariants are pinned.
 */
#ifndef DBG_ASSERT
#ifdef NDEBUG
#define DBG_ASSERT(cond) ((void)0)
#else
#include <assert.h>
#define DBG_ASSERT(cond) assert(cond)
#endif
#endif

/* ------------------------------------------------------------------ *
 * Constants -- mirror of cpu.rs:80-300 (cpu.rs:80-180, 180-300).
 * ------------------------------------------------------------------ */

/* flags (cpu.rs:83-119) */
#define FLAG_SUB        ((int32_t)-0x80000000) /* bit 31 of flags_changed: add vs sub */
#define FLAG_CARRY      1
#define FLAG_PARITY     4
#define FLAG_ADJUST     16
#define FLAG_ZERO       64
#define FLAG_SIGN       128
#define FLAG_TRAP       256
#define FLAG_INTERRUPT  512
#define FLAG_DIRECTION  1024
#define FLAG_OVERFLOW   2048
#define FLAG_IOPL       ((1 << 12) | (1 << 13))
#define FLAG_NT         (1 << 14)
#define FLAG_RF         (1 << 16)
#define FLAG_VM         (1 << 17)
#define FLAG_AC         (1 << 18)
#define FLAG_VIF        (1 << 19)
#define FLAG_VIP        (1 << 20)
#define FLAG_ID         (1 << 21)
#define FLAGS_DEFAULT   (1 << 1) /* always-1 bit */
#define FLAGS_MASK      (FLAG_CARRY | FLAG_PARITY | FLAG_ADJUST | FLAG_ZERO | FLAG_SIGN | \
                         FLAG_TRAP | FLAG_INTERRUPT | FLAG_DIRECTION | FLAG_OVERFLOW | \
                         FLAG_IOPL | FLAG_NT | FLAG_RF | FLAG_VM | FLAG_AC | FLAG_VIF | \
                         FLAG_VIP | FLAG_ID)
#define FLAGS_ALL       (FLAG_CARRY | FLAG_PARITY | FLAG_ADJUST | FLAG_ZERO | FLAG_SIGN | \
                         FLAG_OVERFLOW)

/* the three "op size" encodings (cpu.rs:120-123): a bit position, not a byte count */
#define OPSIZE_8  7
#define OPSIZE_16 15
#define OPSIZE_32 31

/* register indices (v86/src/rust/regs.rs:1-30 and cpu.rs:98-147) */
#define EAX 0
#define ECX 1
#define EDX 2
#define EBX 3
#define ESP 4
#define EBP 5
#define ESI 6
#define EDI 7

#define AX 0
#define CX 1
#define DX 2
#define BX 3
#define SP 4
#define BP 5
#define SI 6
#define DI 7

#define AL 0
#define CL 1
#define DL 2
#define BL 3
#define AH 4
#define CH 5
#define DH 6
#define BH 7

#define ES 0
#define CS 1
#define SS 2
#define DS 3
#define FS 4
#define GS 5
#define TR 6
#define LDTR 7

/* control-register bits (cpu.rs:171-192) */
#define PAGE_TABLE_PRESENT_MASK   (1 << 0)
#define PAGE_TABLE_RW_MASK        (1 << 1)
#define PAGE_TABLE_USER_MASK      (1 << 2)
#define PAGE_TABLE_ACCESSED_MASK  (1 << 5)
#define PAGE_TABLE_DIRTY_MASK     (1 << 6)
#define PAGE_TABLE_PSE_MASK       (1 << 7)
#define PAGE_TABLE_GLOBAL_MASK    (1 << 8)
#define PAGE_TABLE_NX_MASK        ((uint64_t)1 << 63)
#define MMAP_BLOCK_BITS           17
#define MMAP_BLOCK_SIZE           (1 << MMAP_BLOCK_BITS)
#define CR0_PE   1
#define CR0_MP   (1 << 1)
#define CR0_EM   (1 << 2)
#define CR0_TS   (1 << 3)
#define CR0_ET   (1 << 4)
#define CR0_WP   (1 << 16)
#define CR0_AM   (1 << 18)
#define CR0_NW   (1 << 29)
#define CR0_CD   (1 << 30)
#define CR0_PG   ((int32_t)1 << 31)
#define CR4_VME (1)
#define CR4_PVI (1 << 1)
#define CR4_TSD (1 << 2)
#define CR4_DE  (1 << 3)
#define CR4_PSE (1 << 4)
#define CR4_PAE (1 << 5)
#define CR4_PGE (1 << 7)
#define CR4_OSFXSR (1 << 9)
#define CR4_OSXMMEXCPT (1 << 10)
#define CR4_SMEP (1 << 20)
#define MSR_EFER        0xC0000080u /* as int32 this is negative: keep unsigned */
#define EFER_NXE        ((uint64_t)1 << 11)

/* exception vectors (cpu.rs:276-295) */
#define IVT_SIZE            0x400
#define CPU_EXCEPTION_DE    0
#define CPU_EXCEPTION_DB    1
#define CPU_EXCEPTION_NMI   2
#define CPU_EXCEPTION_BP    3
#define CPU_EXCEPTION_OF    4
#define CPU_EXCEPTION_BR    5
#define CPU_EXCEPTION_UD    6
#define CPU_EXCEPTION_NM    7
#define CPU_EXCEPTION_DF    8
#define CPU_EXCEPTION_TS    10
#define CPU_EXCEPTION_NP    11
#define CPU_EXCEPTION_SS    12
#define CPU_EXCEPTION_GP    13
#define CPU_EXCEPTION_PF    14
#define CPU_EXCEPTION_MF    16
#define CPU_EXCEPTION_AC    17
#define CPU_EXCEPTION_MC    18
#define CPU_EXCEPTION_XM    19
#define CPU_EXCEPTION_VE    20

/* TLB entry bits + bookkeeping (cpu.rs:261-267, 329-332) */
#define VALID_TLB_ENTRY_MAX 10000
#define TLB_VALID           (1 << 0)
#define TLB_READONLY        (1 << 1)
#define TLB_NO_USER         (1 << 2)
#define TLB_IN_MAPPED_RANGE (1 << 3)
#define TLB_GLOBAL          (1 << 4)
#define TLB_HAS_CODE        (1 << 5) /* JIT-only: never set in this port */
#define TLB_NO_EXEC         (1 << 6)

/* emulated-device mmio windows (cpu.rs:250-254) */
#define IOAPIC_MEM_ADDRESS 0xFEC00000u
#define IOAPIC_MEM_SIZE    32
#define APIC_MEM_ADDRESS   0xFEE00000u
#define APIC_MEM_SIZE      0x1000

/* ------------------------------------------------------------------ *
 * Types
 * ------------------------------------------------------------------ */

/* cpu.rs:56-69: a 16-byte aligned union of 8/16/32/64-bit lanes, used for xmm
 * registers and (as 4 x i32) for the fpu_st slots by the JS half (cpu.js:176). */
typedef union __attribute__((aligned(16))) reg128 {
    int8_t i8[16];
    int16_t i16[8];
    int32_t i32[4];
    int64_t i64[2];
    uint8_t u8[16];
    uint16_t u16[8];
    uint32_t u32[4];
    uint64_t u64[2];
    float f32[4];
    double f64[2];
} reg128;

/*
 * F80 -- the x87 80-bit extended format.
 *
 * READ softfloat.rs:1-45: the arithmetic core is Berkeley SoftFloat, called as
 * `extF80M_*` over an `F80` value; softfloat.rs:47-56 declares
 *     #[repr(C)] struct F80 { mantissa: u64, sign_exponent: u16 }
 * which is exactly Berkeley's `extFloat80M { uint64_t signif; uint16_t signExp; }`
 * memory layout.  size_of::<F80>() == 16 (8 + 2 + 6 tail padding), which is why the
 * state block's fpu_st slots are 16 bytes apart: global_pointers.rs:83-84 has
 * fpu_st@1152 and efer@1280 = 1152 + 8*16.
 *
 * CHOICE: reproduce that layout byte-for-byte, so M1b's softfloat.c can hand a
 * pointer to fpu_st[i] straight to the extF80M_* routines with no marshalling.
 * Rejected alternatives:
 *   - the host `long double` (x86's own 80-bit type): same bytes on x86 but it
 *     silently rounds to 64-bit double on aarch64, which is an APE target;
 *   - a 128-bit struct: the JS view reads 4 x i32 per slot but only the low 10
 *     bytes are ever meaningful.
 * The selftest pins sizeof/offsetof so a compiler that disagrees fails the build.
 */
typedef struct f80 {
    uint64_t mantissa;
    uint16_t sign_exponent;
} f80_t;

#define F80_ZERO ((f80_t){ .mantissa = 0, .sign_exponent = 0 }) /* softfloat.rs:57-60 */
#define F80_ONE  ((f80_t){ .mantissa = 0x8000000000000000ull, .sign_exponent = 0x3FFF })

_Static_assert(sizeof(reg128) == 16, "reg128 must be 16 bytes");
_Static_assert(_Alignof(reg128) == 16, "reg128 must be 16-byte aligned");
_Static_assert(sizeof(f80_t) == 16, "f80_t must occupy a 16-byte state slot");
_Static_assert(offsetof(f80_t, sign_exponent) == 8, "f80_t layout must match extFloat80M");

/* ------------------------------------------------------------------ *
 * The state
 * ------------------------------------------------------------------ */

/* global_pointers.rs:3-112, in order.  `bool` members are upstream `*mut bool`
 * (an i32 slot in the wasm block); the C type is `bool` since the offsets are gone. */
typedef struct cpu_state {
    int32_t reg32[8]; /* 64 */

    int32_t last_op_size;  /* 96  */
    int32_t flags_changed; /* 100 */
    int32_t last_op1;      /* 104 */
    uint8_t state_flags;   /* 108 -- CachedStateFlags, state_flags.rs:3-26 */
    int32_t last_result;   /* 112 */
    int32_t flags;         /* 120 */

    uint8_t segment_access_bytes[8]; /* 512 */

    bool apic_enabled; /* 548 -> APIC deferral flag, plan ss3 */
    bool acpi_enabled; /* 552 */

    int32_t instruction_pointer; /* 556 */
    int32_t previous_ip;         /* 560 */
    int32_t idtr_size;           /* 564 */
    int32_t idtr_offset;         /* 568 */
    int32_t gdtr_size;           /* 572 */
    int32_t gdtr_offset;         /* 576 */
    int32_t cr[8];               /* 580 */
    uint8_t cpl;                 /* 612 */
    bool in_hlt;                 /* 616 */
    int32_t last_virt_eip;       /* 620 */
    int32_t eip_phys;            /* 624 */

    int32_t sysenter_cs;  /* 636 */
    int32_t sysenter_esp; /* 640 */
    int32_t sysenter_eip; /* 644 */
    uint8_t prefixes;     /* 648 */
    uint32_t instruction_counter; /* 664 */

    uint16_t sreg[8]; /* 668 -- selector registers ES..GS, TR, LDTR */
    int32_t dreg[8];  /* 684 */

    uint32_t svga_dirty_bitmap_min_offset; /* 716 -- VGA-only, kept for state parity */
    uint32_t svga_dirty_bitmap_max_offset; /* 720 */

    bool segment_is_null[8];   /* 724 */
    int32_t segment_offsets[8];/* 736 */
    uint32_t segment_limits[8];/* 768 */

    bool protected_mode;   /* 800 */
    bool is_32;            /* 804 */
    bool stack_size_32;    /* 808 */
    uint32_t memory_size;  /* 812 */
    uint8_t fpu_stack_empty; /* 816 */
    int32_t mxcsr;         /* 824 */

    reg128 reg_xmm[8];     /* 832 */
    uint64_t current_tsc;  /* 960 */
    uint64_t reg_pdpte[4]; /* 968 -- storage only: the PAE walk is not ported */

    uint8_t fpu_stack_ptr;    /* 1032 */
    uint16_t fpu_control_word;/* 1036 */
    uint16_t fpu_status_word; /* 1040 */
    int32_t fpu_opcode;       /* 1044 */
    int32_t fpu_ip;           /* 1048 */
    int32_t fpu_ip_selector;  /* 1052 */
    int32_t fpu_dp;           /* 1056 */
    int32_t fpu_dp_selector;  /* 1060 */
    bool tss_size_32;         /* 1128 */

    reg128 sse_scratch_register; /* 1136 */

    f80_t fpu_st[8]; /* 1152 */
    uint64_t efer;   /* 1280 */
    uint64_t pat;    /* 1288 */

    /* ---- M1a addition: the page-fault flag (see the plumbing note below). ---- */
    bool pagefault;

    /* ---- REBOOT-1 addition: the machine reboot request.  Set by machine_reboot()
     * (src/hw/machine.c) -- NOT a upstream cpu.js field: upstream's reboot_internal
     * simply resets state and keeps running, because its boot re-entry (load_bios)
     * is emulated inside the machine.  The port's boot re-entry is HOST-side and
     * boot-mode-specific (main.c owns the multiboot entry state, the firmware and
     * the option ROM), so the device handler only REQUESTS the reboot; the run
     * loop unwinds (cpu.c's main_loop returns promptly, uop_cache.c aborts the
     * current block) and main.c re-establishes the boot before any further guest
     * instruction executes.  Readers: cpu.c main_loop, uop_cache.c's two block
     * exits, main.c run().  Cleared by main.c when it consumes the request. ---- */
    bool reboot_requested;
} cpu_state_t;

extern cpu_state_t *cpu;

/* ------------------------------------------------------------------ *
 * PAGE-FAULT PLUMBING -- the shape every later unit calls.
 *
 * Upstream threads faults as `Result<T, ()>`: every faultable helper returns
 * Err(()) and each call site expands (paging.rs:3-16)
 *     return_on_pagefault!(expr)  ==  match expr { Ok(v) => v, Err(()) => return }
 * C has no such ergonomics, so the port uses the plan's shape (ss1, ss8/R3):
 *
 *   1. a faultable helper SETS `cpu->pagefault` and returns a SENTINEL (0 -- the
 *      value is never used, exactly as in Rust's Err arm);
 *   2. the caller checks `cpu->pagefault` IMMEDIATELY after the call, before using
 *      the returned value, and returns early;
 *   3. the instruction's partial effects are KEPT -- a fault aborts the instruction,
 *      never undoes it (plan ss8/R3), and the instruction re-executes after iret;
 *   4. `cpu->pagefault` is SET-ONLY inside a helper.  The dispatch loop BRACKETS each
 *      dispatch with a clear -- one before it and one after it returns -- so the flag's
 *      live window is exactly one instruction, and those two sites are the only clears.
 *      A helper must never clear it: nested faults (a fault while reading the IDT
 *      in call_interrupt_vector, cpu.rs:822-830) must not be masked by the inner
 *      return path.
 *
 *      THE POST-DISPATCH CLEAR IS PART OF THE CONTRACT, not hygiene.  Upstream's
 *      `return_on_pagefault!` returns from the generated `run` itself (cpu.rs:3321), so
 *      a fault -- delivered or not -- cannot outlive the dispatch that raised it.  The
 *      only reader after a dispatch returns is the interpreted loop's fetch check (cpu.c's
 *      `interpreted_run` tail), which must observe the FETCH's fault and nothing else; a
 *      DELIVERED
 *      fault (IDT gate resolved, frame pushed, handler entered, eip now the handler)
 *      that survived to that check aborted the run and skipped the instruction counter.
 *      Unit R5 measured exactly that on the FIRST delivered fault whose handler shares
 *      the faulting instruction's 4 KiB page -- the normal case for a small kernel; a
 *      handler on another page hid it behind the loop's page-boundary break.  Fixed by
 *      R5F: results/r5f-fixes.md, cpu.c's D1 note.
 *
 * The generated emitter prints literally
 *     x = safe_read32s(addr); if (cpu->pagefault) return;
 * (plan ss1), so `cpu->pagefault` is public and is the ONLY fault channel.
 * ------------------------------------------------------------------ */

/* Upstream `return_on_pagefault!(expr)` for the statement/value-discarding form. */
#define RETURN_ON_PAGEFAULT(expr)  \
    do {                           \
        (void)(expr);              \
        if (cpu->pagefault) return; \
    } while (0)

/* ------------------------------------------------------------------ *
 * Exception primitives -- M1a-2 addition.
 *
 * cpu.rs:3427-3472.  exceptions.c (M1c, plan ss7 step 17) OWNS the definitions;
 * they are declared here -- the spine header -- because they are the sibling
 * primitives of the page-fault plumbing above and their first callers are M1a-2's
 * files: div/idiv reach trigger_de (arith.c), a null segment reaches trigger_gp
 * (modrm.c's get_seg), and task_switch_test reaches trigger_nm/trigger_ud
 * (misc_instr.c).  M1c must DEFINE these, not shadow them, and must not re-declare
 * them with a different signature.
 *
 * Upstream's delivery order is kept by the callers: trigger_* sets
 * instruction_pointer = previous_ip and delivers the vector; a caller that upstream
 * follows with `return Err(())` (get_seg) also sets cpu->pagefault, because that flag
 * is this port's only abort channel.
 * ------------------------------------------------------------------ */
void trigger_de(void);
void trigger_ud(void);
void trigger_nm(void);
void trigger_gp(int32_t code);

/* ------------------------------------------------------------------ *
 * Register accessors (cpu.rs:4128-4196)
 * ------------------------------------------------------------------ */

/* read_tsc/store_current_tsc are deferred with the TSC (M1c run loop). */
int32_t read_reg8(int32_t index);
void write_reg8(int32_t index, int32_t value);
int32_t read_reg16(int32_t index);
void write_reg16(int32_t index, int32_t value);
int32_t read_reg32(int32_t index);
void write_reg32(int32_t index, int32_t value);

int32_t read_mmx32s(int32_t r);
uint64_t read_mmx64s(int32_t r);
void write_mmx_reg64(int32_t r, uint64_t data);

float read_xmm_f32(int32_t r);
int32_t read_xmm32(int32_t r);
uint64_t read_xmm64s(int32_t r);
reg128 read_xmm128s(int32_t r);
void write_xmm_f32(int32_t r, float data);
void write_xmm32(int32_t r, int32_t data);
void write_xmm64(int32_t r, uint64_t data);
void write_xmm_f64(int32_t r, double data);
void write_xmm128(int32_t r, int32_t i0, int32_t i1, int32_t i2, int32_t i3);
void write_xmm128_2(int32_t r, uint64_t i0, uint64_t i1);
void write_xmm_reg128(int32_t r, reg128 data);

/* ------------------------------------------------------------------ *
 * Cached state byte (state_flags.rs:3-26) + update_state_flags (cpu.rs:3346)
 * ------------------------------------------------------------------ */
#define CACHED_MASK_IS_32      (1 << 0)
#define CACHED_MASK_SS32       (1 << 1)
#define CACHED_MASK_CPL3       (1 << 2)
#define CACHED_MASK_FLAT_SEGS  (1 << 3)
#define CACHED_MASK_ALL        (CACHED_MASK_IS_32 | CACHED_MASK_SS32 | CACHED_MASK_CPL3 | \
                                CACHED_MASK_FLAT_SEGS)

static inline bool cached_state_is_32(void) { return (cpu->state_flags & CACHED_MASK_IS_32) != 0; }
static inline bool cached_state_ssize_32(void) { return (cpu->state_flags & CACHED_MASK_SS32) != 0; }
static inline bool cached_state_cpl3(void) { return (cpu->state_flags & CACHED_MASK_CPL3) != 0; }
static inline bool cached_state_has_flat_segmentation(void)
{
    return (cpu->state_flags & CACHED_MASK_FLAT_SEGS) != 0;
}

void update_state_flags(void);
bool has_flat_segmentation(void);

int32_t getiopl(void);
bool vm86_mode(void);

/* ------------------------------------------------------------------ *
 * reset + init
 * ------------------------------------------------------------------ */

/* reset_cpu (cpu.rs:4671-4760) -- the architectural reset state, plus the
 * cpu.js constructor's init values (cpu.js:180-199: fpu_control_word 0x37F,
 * fpu_stack_empty 0xFF, ...; the plan's ss2 gate). */
void reset_cpu(void);
void cpu_state_init(void);

/* The reset values as literal constants so the selftest (and a reader) can name
 * them without re-deriving them from the code under test. */
#define RESET_CR0        ((int32_t)((1u << 30) | (1u << 29) | (1u << 4))) /* cpu.rs:4751 */
#define RESET_DREG6      ((int32_t)0xFFFF0FF0u)
#define RESET_DREG7      0x400
#define RESET_EIP        0xFFFF0
#define RESET_CS_SHIFT   0xF000 /* switch_cs_real_mode(0xF000), cpu.rs:4755 */
#define RESET_SS_SEL     0x30   /* switch_seg(SS, 0x30), cpu.rs:4757 */
#define RESET_ESP        0x100
#define RESET_PAT        0x0007040600070406ull
#define RESET_MXCSR      0x1F80
#define RESET_FPU_CW     0x37F
#define RESET_FPU_SW     0
#define RESET_FPU_TAG    0xFF
#define RESET_SEG_ACCESS_DATA 0x92 /* P dpl0 S RW  (cpu.rs:4672) */
#define RESET_SEG_ACCESS_CS   0x9A /* P dpl0 S E RW (cpu.rs:4684) */

#endif /* V86C_CPU_STATE_H */
