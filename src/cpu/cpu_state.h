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

/* the "op size" encodings (cpu.rs:120-123): a bit position, not a byte count.
 *
 * OPSIZE_64 is the port's own (S1, results/x86-64-plan.md ss4): long mode's 64-bit
 * operand.  It is 63 for the same reason the others are 7/15/31 -- the getters extract
 * the sign/carry bit with `>> last_op_size & 1`, and the mask is `(2ull << op_size) - 1`
 * (flags.h).  It is UNUSED until the first 64-bit semantic body lands (S6); the state
 * it names is defined and tested now so the flag cores are not widened twice. */
#define OPSIZE_8  7
#define OPSIZE_16 15
#define OPSIZE_32 31
#define OPSIZE_64 63

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
/* S2 (results/x86-64-plan.md ss3.5.2, C5): the three long-mode control bits of the SAME
 * register.  EFER_NXE was the only defined bit, which is why a guest's EFER.LME was dead
 * data: nothing read it, and the machine walked a 4-level table as if it were 3-level.
 * LME is the guest's REQUEST (WRMSR), LMA is the machine's state (set on the CR0.PG edge
 * once a backend can run long mode -- S5/S11) and SCE enables SYSCALL/SYSRET (S11).  The
 * bit positions are the SDM's (Vol 4, IA32_EFER). */
#define EFER_SCE        ((uint64_t)1 << 0)
#define EFER_LME        ((uint64_t)1 << 8)
#define EFER_LMA        ((uint64_t)1 << 10)

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
    /* S1 (results/x86-64-plan.md ss4): WIDENED IN PLACE from `int32_t reg32[8]`.  The
     * name is the point -- a field called reg32 holding a 64-bit value is a comment that
     * lies.  The READERS are still 32-bit: read_reg32/write_reg32 are the accessors every
     * 32-bit body uses, and they keep their names and their width (a 32-bit read returns
     * the low half; a 32-bit WRITE ZEROES the high half, which is what x86-64 hardware
     * does and what the JIT's spill/reload pair was made to agree with).  The byte/word
     * accessors' index formulas changed with the element size -- see cpu_state.c.
     *
     * S5a (plan correction GAP 1, results/x86-64-s5.md ss2.1): SIXTEEN SLOTS, not eight.
     * ss4's inventory said `reg64[8]`, and S4's report read that as "r8-r15 do not exist
     * yet, so the wide document's slots 8-15 print zero until S5".  That seat cannot be
     * filled by S5 while it is eight slots: the corpus's SHARED PROLOGUE -- which every
     * one of the 57 images executes -- zeroes r8d..r15d (`45 31 c0`-class, MEASURED), and
     * the v2 document's sixteen register slots are compared against the native leg's real
     * r8-r15.  So the array widens to 16 and NOTHING else changes: indices 0-7 keep their
     * meaning byte for byte, and an index of 8-15 is only ever produced by a REX.R/REX.B
     * bit, which cannot be set outside long mode (prefixes' REX bits are written only by
     * the long-mode REX prefix handler).  The last eight slots are therefore ZERO for the
     * whole 32-bit battery -- the poison probe's claim, and the reason the M0 transcript
     * does not move. */
    uint64_t reg64[16]; /* 64 -- was reg32[8]; 8-15 are r8-r15 (S5a) */

    int32_t last_op_size;  /* 96  */
    int32_t flags_changed; /* 100 -- stays 32-bit: bit 31 IS FLAG_SUB (flags.h) */
    /* S1: WIDENED to int64_t (ss4's "widen the two operands to int64_t").  The getters
     * read them with `>> last_op_size`, so the shift is in the operand's own width; the
     * value may be stored zero- or sign-extended by the setter, which is why getcf masks
     * before comparing (flags.c). */
    int64_t last_op1;      /* 104 */
    uint8_t state_flags;   /* 108 -- CachedStateFlags, state_flags.rs:3-26 */
    int64_t last_result;   /* 112 */
    int32_t flags;         /* 120 */

    uint8_t segment_access_bytes[8]; /* 512 */

    bool apic_enabled; /* 548 -> APIC deferral flag, plan ss3 */
    bool acpi_enabled; /* 552 */

    /* S3 (results/x86-64-plan.md ss4, ss6): WIDENED IN PLACE to uint64_t, and the comment
     * numbers on the right are UPSTREAM's wasm offsets (global_pointers.rs), kept for
     * parity -- the C struct has not had those offsets since the singleton replaced the
     * shared wasm block, and they are not re-derived here.
     *
     * SIGNEDNESS: these hold a FLAT LINEAR ADDRESS (segment base + offset), and they are
     * UNSIGNED.  Upstream's i32 made 0xFFFF0000 a negative number; a widened field that
     * sign-extends a 32-bit base becomes 0xFFFFFFFFFFFF0000, which is a different address,
     * so every producer of a 32-bit base casts through uint32_t (see get_seg in modrm.c).
     *
     * THE PROJECTION RULE (S3's whole correctness story): a field may hold 64 bits, but
     * every consumer that leaves the model's 32-bit PHYSICAL space -- a mem8 index, an
     * `in_mapped_range` test, the page walk's table indexing -- takes `(uint32_t)` and says
     * why at the site (plan ss5.1: the port cannot represent a physical address above
     * 4 GiB; a 64-bit PTE that names one is S11's named refusal, never a silent truncation).
     * Outside long mode the HIGH HALF MUST BE ZERO, and cpu_assert_address_widening()
     * (below) asserts exactly that at the mode boundaries and at every batch boundary, so a
     * widened arithmetic that leaks bits above 32 is a named stop rather than a wrong
     * address. */
    uint64_t instruction_pointer; /* 556 */
    uint64_t previous_ip;         /* 560 */
    int32_t idtr_size;            /* 564 */
    uint64_t idtr_offset;         /* 568 */
    int32_t gdtr_size;            /* 572 */
    uint64_t gdtr_offset;         /* 576 */
    int32_t cr[8];                /* 580 -- NOT widened: the physical model is 32-bit
                                   * (plan ss5.1), so a CR3 above 4 GiB is the case that
                                   * path REFUSES by name; widening the field would widen
                                   * a bound the machine does not have. */
    uint8_t cpl;                  /* 612 */
    bool in_hlt;                  /* 616 */
    uint64_t last_virt_eip;       /* 620 -- the cached virtual PAGE; VIRT_EIP_NONE when
                                   * no translation is cached (it was `-1`, whose 64-bit
                                   * sign extension would itself be a high-half leak) */
    uint64_t eip_phys;            /* 624 -- `phys ^ eip` (cpu.c:get_phys_eip); not a
                                   * physical address itself, but it carries the eip's
                                   * page bits, so it is a uint64_t like the eip */

    int32_t sysenter_cs;  /* 636 */
    int32_t sysenter_esp; /* 640 */
    int32_t sysenter_eip; /* 644 */
    /* S5a (plan ss3.6.5): WIDENED from `uint8_t`.  The legacy prefix bits keep their
     * positions (0x07 segment, 0x08/0x10 rep, 0x20 operand size, 0x40 address size) and the
     * REX nibble lives in bits 8-11 with a PRESENT bit at 12 -- see modrm.h's PREFIX_REX_*.
     * The REX bits are written by exactly one place (the long-mode REX prefix handler,
     * instructions_64.c) so every 32-bit path sees the same 8-bit value it always did. */
    uint16_t prefixes;    /* 648 */
    /* upstream is a u32 at wasm offset 664 (global_pointers.rs:39) and WRAPS at
     * 4,294,967,296; the port widens it to u64 so the virtual clock (microtick, TSC, PIT,
     * PM timer) and the instruction limit do not jump backwards mid-run.  The `664` in the
     * trailing comment is upstream's offset, kept for parity -- the C struct has no wasm
     * peer. */
    uint64_t instruction_counter; /* 664 */

    uint16_t sreg[8]; /* 668 -- selector registers ES..GS, TR, LDTR */
    /* S1: WIDENED to int64_t (ss4): DR0-DR7 are 64-bit in long mode.  The debug-register
     * moves (0F 21/0F 23) still read and write 32 bits, zero-extended. */
    int64_t dreg[8];  /* 684 */

    uint32_t svga_dirty_bitmap_min_offset; /* 716 -- VGA-only, kept for state parity */
    uint32_t svga_dirty_bitmap_max_offset; /* 720 */

    bool segment_is_null[8];   /* 724 */
    /* S3: WIDENED (ss4) -- FS/GS keep real 64-bit bases in long mode (the MSRs), which is
     * the whole reason this array widens; CS/DS/ES/SS bases are 0 there and limits are
     * ignored, so `segment_limits` stays 32-bit.  UNSIGNED for the reason in the address
     * block above: `selector << 4` for selector 0xF000 is a positive address, not a
     * negative i32. */
    uint64_t segment_offsets[8];/* 736 */
    uint32_t segment_limits[8];/* 768 */

    bool protected_mode;   /* 800 */
    bool is_32;            /* 804 */
    /* S5a (plan correction GAP 2): CS.L, THE BIT THAT MAKES IA-32e's TWO MODES.  The plan
     * placed CS.L's decode in S11 ("it arrives with the far-transfer work in S11",
     * cpu_backend.c's note), but S5 cannot do "stub -> LMA -> 64-bit fetch" without it:
     * after the stub's `mov cr0` sets CR0.PG the machine IS in IA-32e with CS.L still 0 --
     * COMPATIBILITY mode -- and the very next instruction is the legacy `.byte 0xEA` far
     * jump that loads the 64-bit CS.  Treating LMA as 64-bit on the instant it is set would
     * turn that far jump into the overlay's #UD and no case would ever reach its body.
     *
     * It is a property of the CS DESCRIPTOR (Appendix A's `efer.LMA && CS.L`), so it is set
     * wherever a CS descriptor is loaded (update_cs_size's two call shapes, exceptions.h)
     * and cleared by switch_cs_real_mode.  Outside long mode it stays false, which is what
     * keeps every 32-bit path's behaviour identical. */
    bool is_64;            /* CS.L: 1 in IA-32e 64-bit mode, 0 in compat */
    bool stack_size_32;    /* 808 */
    uint32_t memory_size;  /* 812 */
    uint8_t fpu_stack_empty; /* 816 */
    int32_t mxcsr;         /* 824 */

    reg128 reg_xmm[8];     /* 832 */
    uint64_t current_tsc;  /* 960 */
    uint64_t reg_pdpte[4]; /* 968 -- loaded by load_pdpte; interpreted by the PAE walk */

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
/* S5a: the 64-bit half of the register file.  In long mode a 64-bit operand read/write is
 * the whole slot; outside it nothing calls these (the 64-bit bodies are only reachable
 * through the overlay's 0x2NN arms, which only a long-mode dispatch key selects). */
uint64_t read_reg64(int32_t index);
void write_reg64(int32_t index, uint64_t value);

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
 * S3: THE STORE INTO THE FETCH CURSOR, AND WHY IT IS A FUNCTION
 *
 * In the 32-bit model the guest's instruction-pointer arithmetic is 32 bits wide and WRAPS
 * -- upstream's i32 field, and the hardware's own linear-address width -- so the store must
 * project.  In long mode the same store must not.
 *
 * EVERY STORE, ENUMERATED -- the earlier version of this comment claimed this function was
 * the only producer and backed it with an `rg` that misses half of them (round 2's review:
 * the claim was wider than its evidence).  The actual population is three classes, and only
 * the first can introduce a new value:
 *
 *   1. COMPUTED HERE.  Every new eip: the sixteen `get_seg_cs() + offset` stores in
 *      exceptions.c, the far transfers and `sysenter`/`sysexit` in instructions.c /
 *      instructions_0f.c, the jumps in misc_instr.c / instructions.c, the fetch cursor in
 *      modrm.c and both loops, the uop executor's `body_ip` store (uop_cache.c), main.c's
 *      boot entry.  `rg 'instruction_pointer\s*=[^=]' src/cpu` lists exactly these plus the
 *      next two classes.
 *   2. A COPY OF AN ALREADY-PROJECTED VALUE: `instruction_pointer = previous_ip` (six in
 *      exceptions.c's trigger_*, paging.c's trigger_pagefault, two in string.c), and
 *      `cpu_state.c`'s reset constant.  A copy cannot add a high half if the source has none,
 *      and `previous_ip`'s own stores come from class 1 or from a 32-bit uop field.
 *   3. THE JIT'S FIVE EMITTED STORES (jit_codegen.c: `jitcg_emit_stores`' pair and the three
 *      recipes' ip writes).  These are COMPILE-TIME 32-bit values stored eight bytes wide
 *      (`jitcg_store_u64`), so the high half is zero by construction rather than by this
 *      function -- and `jitcg_target32` is where their arithmetic is wrapped.
 *
 * That is what lets cpu_assert_address_widening() be a DETECTOR rather than a tautology: the
 * one way left to get a non-zero high half is a store that is none of the three -- i.e. the
 * class a later slice adds.
 *
 * The long-mode arm cannot be taken today -- nothing SETS EFER.LMA before S5's mode
 * transition (S2 defines the bit and refuses the transitions that would need it) -- so the
 * mask is unconditional in fact and conditional in form, which is the inert slice's whole
 * shape.  MEASURED: `--poison` seeds the high halves of exactly these fields behind this
 * function's back and the M0 transcript does not move.
 *
 * previous_ip needs no counterpart: every store into it is either the (already projected)
 * instruction_pointer or a 32-bit uop field (uop_cache.c).  Stated, not assumed -- it is
 * checked by the invariant, which covers previous_ip too.
 * ------------------------------------------------------------------ */
static inline void set_instruction_pointer(uint64_t eip)
{
    cpu->instruction_pointer = (cpu->efer & EFER_LMA) ? eip : (uint64_t)(uint32_t)eip;
}

/* ------------------------------------------------------------------ *
 * S3: THE WIDENED ADDRESS FIELDS' HIGH-HALF INVARIANT (plan ss6 S3)
 *
 * Outside long mode every linear address the machine can form is 32 bits, so the fields
 * widened in ss4 must have a ZERO high half.  That is not a fact about the types, it is a
 * fact about the code that writes them -- which is exactly what makes it a detector: a
 * widened arithmetic that forgets to project (a sign-extended 32-bit segment base, a
 * `previous_ip = pfx + len` left at 64 bits, a sum upstream's i32 would have wrapped) shows
 * up here instead of as a wrong address in one guest.  RISK R2's second instrument.
 *
 * cpu_address_widening_leak() names the offending field (NULL: clean); the selftest poisons
 * each field in turn and requires the NAME, so the instrument is shown able to fail rather
 * than assumed to be live.  cpu_assert_address_widening() is the run-time form: it ABORTS
 * with the name, and is called from the mode boundaries (the three transitions that can
 * change the mode, plus reset) and once per backend batch (cpu.c's cycle_internal) -- the
 * batch call is what makes it a continuous instrument over a run instead of a boot-time one.
 * It is a no-op IN long mode, and it is disabled by `--poison` (which seeds the high halves
 * on purpose: the probe's claim is the opposite direction, that the transcript does not
 * move).
 * ------------------------------------------------------------------ */
extern bool cpu_address_widening_check_enabled;
const char *cpu_address_widening_leak(void);
void cpu_assert_address_widening(const char *where);

/* The "no translation cached" sentinel for last_virt_eip.  Upstream writes `-1`; with a
 * 64-bit field that literal sign-extends to 0xFFFFFFFFFFFFFFFF, which is BOTH a high-half
 * leak by construction and impossible to tell from a real 64-bit page.  A value whose low
 * 12 bits are set can never be a page (every page is page-aligned), so it cannot collide. */
#define VIRT_EIP_NONE 0x00000000FFFFFFFFull

/* ------------------------------------------------------------------ *
 * Cached state byte (state_flags.rs:3-26) + update_state_flags (cpu.rs:3346)
 * ------------------------------------------------------------------ */
#define CACHED_MASK_IS_32      (1 << 0)
#define CACHED_MASK_SS32       (1 << 1)
#define CACHED_MASK_CPL3       (1 << 2)
#define CACHED_MASK_FLAT_SEGS  (1 << 3)
/* S1 (ss4): the fifth bit, defined and NOT SET YET.  `state_flags` is half of the
 * decoded-block cache's key (uop_cache.h's block key), so a bit that distinguishes
 * long/compat mode invalidates any cross-mode reuse FOR FREE -- which is the whole
 * reason it exists.  Nothing enters long mode in this tree yet (EFER_LMA is defined in
 * S2 and set by S5's mode transition), so update_state_flags composes the same four
 * bits it always did and every cached-block key is unchanged. */
#define CACHED_MASK_LONG       (1 << 4)
#define CACHED_MASK_ALL        (CACHED_MASK_IS_32 | CACHED_MASK_SS32 | CACHED_MASK_CPL3 | \
                                CACHED_MASK_FLAT_SEGS | CACHED_MASK_LONG)

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
#define RESET_DREG6      ((int64_t)0xFFFF0FF0u)
#define RESET_DREG7      ((int64_t)0x400)
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
