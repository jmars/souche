/*
 * exceptions.h -- the machine's control machinery: the segment-descriptor API, the
 * selector lookup, exception delivery, the far control transfers, iret and the task
 * switch.
 *
 * Port of v86/src/rust/cpu/cpu.rs:380-451 (SegmentSelector/SegmentDescriptor),
 * 473-1900 (switch_cs_real_mode, get_tss_ss_esp, iret, call_interrupt_vector, far_jump,
 * far_return, do_task_switch), 2638-2906 (lookup_segment_selector, switch_seg, load_tr,
 * load_ldt), 3000-3006 (cpl_changed, update_cs_size) and 3427-3474 + 4546-4568
 * (trigger_de/ud/nm/gp/np/ss).  Plan ss7 step 17, and ss8 R3 (fault delivery order) /
 * R4 (the segment machinery -- boot-critical).
 *
 * WHAT LIVES HERE AND WHY.  Upstream splits this code between cpu.rs and misc_instr.rs
 * and reaches it from five files; the port puts the whole machinery in ONE translation
 * unit because every part of it shares the descriptor API below (lar/lsl/verr/verw in
 * misc_instr.c, lldt/ltr in instructions_0f.c), and the API is declared in this header
 * so those files can use it without a cross-file dependency on the implementation.
 *
 * THE FAULT CONTRACT (cpu_state.h's plumbing note) applies to lookup_segment_selector
 * and load_ldt exactly as upstream's `OrPageFault`: on a fault they SET cpu->pagefault
 * and their return value must not be used.  The callers in this file check the flag
 * immediately, in upstream's `return_on_pagefault!` position.
 *
 * FIDELITY NOTE (plan ss8 R3, the trap this unit exists for): upstream's `Result`
 * plumbing means a fault raised INSIDE a delivery or a far transfer ABORTS the in-flight
 * operation and leaves the partial effects in place -- the instruction/interrupt is
 * re-executed after the handler's iret, it is never undone.  Every `return` below is at
 * upstream's `?`/`return_on_pagefault!` position for that reason, and cpu->pagefault is
 * set-only (nothing here clears it).
 */

#ifndef V86C_EXCEPTIONS_H
#define V86C_EXCEPTIONS_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu_state.h"

/* ------------------------------------------------------------------ *
 * The exception record for the fixture contract (main.c).
 *
 * WAS: "the vector the guest took -- upstream DELIVERS it and needs no record, the port
 * cannot deliver so it reports instead" (the M1c seat, exec_boundary.c).
 *
 * IS: the vector of a fault the machine STOPPED on because the delivery could not
 * proceed -- the paths where upstream `panic!`s (an empty/too-small IDT, an invalid
 * gate, the unimplemented handler arms).  A fault that a guest CAN have delivered is
 * delivered and never recorded here; the tail of this header says which paths those are.
 * main.c's fixture mode maps the vector to the host signal the native harness would have
 * taken (exception_signal), which is what the nasm differential compares, so the record
 * exists for the SAME reason the seat's did: a fault the reference cannot produce a
 * state for.
 * ------------------------------------------------------------------ */
extern int32_t guest_exception;

/* ------------------------------------------------------------------ *
 * cpu.rs:380-451 -- SegmentSelector / SegmentDescriptor.
 *
 * Upstream's `impl` blocks become `static inline` accessors in the header: the layout is
 * part of the interface (lar/lsl return `(desc.raw >> 32) & 0x00FFFF00` and
 * instructions_0f.c's task gates read `info.raw >> 16`), and inlining in the header is
 * how the port keeps the SAME bits reachable from every file that needs them without an
 * ABI for every field.  Names are upstream's methods with the receiver as the first
 * argument; the bodies are upstream's expressions verbatim.
 * ------------------------------------------------------------------ */
typedef struct seg_selector {
    uint16_t raw;
} seg_selector_t;

typedef struct seg_descriptor {
    uint64_t raw;
} seg_descriptor_t;

static inline seg_selector_t seg_selector_of_u16(uint16_t raw)
{
    seg_selector_t s;
    s.raw = raw;
    return s;
}
static inline uint8_t seg_selector_rpl(seg_selector_t s) { return (uint8_t)(s.raw & 3); }
static inline bool seg_selector_is_gdt(seg_selector_t s) { return (s.raw & 4) == 0; }
static inline uint16_t seg_selector_descriptor_offset(seg_selector_t s) { return (uint16_t)(s.raw & ~7); }
static inline bool seg_selector_is_null(seg_selector_t s)
{
    return seg_selector_is_gdt(s) && seg_selector_descriptor_offset(s) == 0;
}

static inline seg_descriptor_t seg_descriptor_of_u64(uint64_t raw)
{
    seg_descriptor_t d;
    d.raw = raw;
    return d;
}
static inline int32_t seg_descriptor_base(seg_descriptor_t d)
{
    return (int32_t)(((d.raw >> 16) & 0xffff) | ((d.raw & 0xff00000000ull) >> 16) |
                     ((d.raw >> 56) << 24));
}
static inline uint32_t seg_descriptor_limit(seg_descriptor_t d)
{
    return (uint32_t)((d.raw & 0xffff) | (((d.raw >> 48) & 0xf) << 16));
}
static inline uint8_t seg_descriptor_access_byte(seg_descriptor_t d)
{
    return (uint8_t)((d.raw >> 40) & 0xff);
}
static inline uint8_t seg_descriptor_flags(seg_descriptor_t d)
{
    return (uint8_t)(((d.raw >> 48) >> 4) & 0xf);
}
static inline bool seg_descriptor_is_system(seg_descriptor_t d)
{
    return (seg_descriptor_access_byte(d) & 0x10) == 0;
}
static inline uint8_t seg_descriptor_system_type(seg_descriptor_t d)
{
    return (uint8_t)(seg_descriptor_access_byte(d) & 0xF);
}
static inline bool seg_descriptor_accessed(seg_descriptor_t d)
{
    return (seg_descriptor_access_byte(d) & 1) == 1;
}
static inline bool seg_descriptor_is_rw(seg_descriptor_t d)
{
    return (seg_descriptor_access_byte(d) & 2) == 2;
}
static inline bool seg_descriptor_is_dc(seg_descriptor_t d)
{
    return (seg_descriptor_access_byte(d) & 4) == 4;
}
static inline bool seg_descriptor_is_executable(seg_descriptor_t d)
{
    return (seg_descriptor_access_byte(d) & 8) == 8;
}
static inline bool seg_descriptor_is_present(seg_descriptor_t d)
{
    return (seg_descriptor_access_byte(d) & 0x80) == 0x80;
}
static inline bool seg_descriptor_is_writable(seg_descriptor_t d)
{
    return seg_descriptor_is_rw(d) && !seg_descriptor_is_executable(d);
}
static inline bool seg_descriptor_is_readable(seg_descriptor_t d)
{
    return seg_descriptor_is_rw(d) || !seg_descriptor_is_executable(d);
}
static inline bool seg_descriptor_is_conforming_executable(seg_descriptor_t d)
{
    return seg_descriptor_is_dc(d) && seg_descriptor_is_executable(d);
}
static inline uint8_t seg_descriptor_dpl(seg_descriptor_t d)
{
    return (uint8_t)((seg_descriptor_access_byte(d) >> 5) & 3);
}
static inline bool seg_descriptor_is_32(seg_descriptor_t d)
{
    return (seg_descriptor_flags(d) & 4) == 4;
}
static inline uint32_t seg_descriptor_effective_limit(seg_descriptor_t d)
{
    if ((seg_descriptor_flags(d) & 8) == 8) {
        return seg_descriptor_limit(d) << 12 | 0xFFF;
    }
    else {
        return seg_descriptor_limit(d);
    }
}
static inline seg_descriptor_t seg_descriptor_set_busy(seg_descriptor_t d)
{
    d.raw |= 2ull << 40;
    return d;
}
static inline seg_descriptor_t seg_descriptor_clear_busy(seg_descriptor_t d)
{
    d.raw &= ~(2ull << 40);
    return d;
}
static inline seg_descriptor_t seg_descriptor_set_accessed(seg_descriptor_t d)
{
    d.raw |= 1ull << 40;
    return d;
}

/* cpu.rs:453-471 -- InterruptDescriptor, the IDT gate.  A DIFFERENT layout from
 * SegmentDescriptor (the offset is split around the selector, bits 8-11 of the access
 * byte are the gate's operand size rather than the descriptor's `flags` nibble), which
 * is why upstream has a second struct and the port a second set of accessors. */
typedef struct intr_descriptor {
    uint64_t raw;
} intr_descriptor_t;

#define INTR_GATE_TASK      5
#define INTR_GATE_INTERRUPT 6
#define INTR_GATE_TRAP      7

static inline intr_descriptor_t intr_descriptor_of_u64(uint64_t raw)
{
    intr_descriptor_t d;
    d.raw = raw;
    return d;
}
static inline int32_t intr_descriptor_offset(intr_descriptor_t d)
{
    return (int32_t)((d.raw & 0xffff) | ((d.raw >> 32) & 0xffff0000));
}
static inline int32_t intr_descriptor_selector(intr_descriptor_t d)
{
    return (int32_t)((d.raw >> 16) & 0xffff);
}
static inline uint8_t intr_descriptor_access_byte(intr_descriptor_t d)
{
    return (uint8_t)((d.raw >> 40) & 0xff);
}
static inline uint8_t intr_descriptor_dpl(intr_descriptor_t d)
{
    return (uint8_t)((intr_descriptor_access_byte(d) >> 5) & 3);
}
static inline uint8_t intr_descriptor_gate_type(intr_descriptor_t d)
{
    return (uint8_t)(intr_descriptor_access_byte(d) & 7);
}
static inline bool intr_descriptor_is_32(intr_descriptor_t d)
{
    return (intr_descriptor_access_byte(d) & 8) == 8;
}
static inline bool intr_descriptor_is_present(intr_descriptor_t d)
{
    return (intr_descriptor_access_byte(d) & 0x80) == 0x80;
}
static inline bool intr_descriptor_reserved_zeros_are_valid(intr_descriptor_t d)
{
    return (intr_descriptor_access_byte(d) & 16) == 0;
}

/* cpu.rs:394-399 -- the two early-reject outcomes of the selector lookup.  Upstream
 * nests them as Result<Result<_, SelectorNullOrInvalid>, ()>; C has no sum type, so the
 * status is returned by value and a PAGE FAULT is reported through cpu->pagefault
 * (checked by the caller FIRST, exactly as `return_on_pagefault!` does). */
typedef enum selector_lookup {
    SELECTOR_LOOKUP_OK = 0,        /* Ok(Ok((descriptor, address))) */
    SELECTOR_LOOKUP_IS_NULL = 1,   /* Ok(Err(SelectorNullOrInvalid::IsNull)) */
    SELECTOR_LOOKUP_OUTSIDE = 2,   /* Ok(Err(SelectorNullOrInvalid::OutsideOfTableLimit)) */
} selector_lookup_t;

/* cpu.rs:2638-2673.  On OK, desc and descriptor_address are set.  On a page fault
 * cpu->pagefault is set and the return value is meaningless. */
selector_lookup_t lookup_segment_selector(seg_selector_t selector,
                                          seg_descriptor_t *desc,
                                          int32_t *descriptor_address);

/* ------------------------------------------------------------------ *
 * switch_seg (cpu.rs:2676) is declared in misc_instr.h -- lss16/lss32 were its first
 * callers (M1a-2).  far_jump/far_return/iret16/iret32 are declared in instructions.h and
 * call_interrupt_vector in paging.h, for the same reason: each was first reached from a
 * unit that landed before this one.  This header does NOT re-declare them.
 * ------------------------------------------------------------------ */

/* cpu.rs:192-212 -- the 32-bit TSS offsets.  They are the layout do_task_switch reads
 * and writes, so they are part of this header's contract (the M1c test binary's TSS
 * fixture uses them by name). */
#define TSR_BACKLINK 0x00
#define TSR_CR3      0x1c
#define TSR_EIP      0x20
#define TSR_EFLAGS   0x24
#define TSR_EAX      0x28
#define TSR_ECX      0x2c
#define TSR_EDX      0x30
#define TSR_EBX      0x34
#define TSR_ESP      0x38
#define TSR_EBP      0x3c
#define TSR_ESI      0x40
#define TSR_EDI      0x44
#define TSR_ES       0x48
#define TSR_CS       0x4c
#define TSR_SS       0x50
#define TSR_DS       0x54
#define TSR_FS       0x58
#define TSR_GS       0x5c
#define TSR_LDT      0x60

/* cpu.rs:473-480 */
void switch_cs_real_mode(int32_t selector);

/* cpu.rs:4546-4568 -- the two triggers whose declarations arrived with this unit. */
void trigger_np(int32_t code);
void trigger_ss(int32_t code);

/* cpu.rs:1657-1662 */
typedef enum task_switch_source {
    TASK_SWITCH_JUMP = 0,
    TASK_SWITCH_CALL_OR_INT = 1,
    TASK_SWITCH_IRET = 2,
} task_switch_source_t;

/* cpu.rs:1664-1899.  Upstream's `error_code: Option<i32>` is split into the pair that
 * paging.h's call_interrupt_vector already uses (has_error_code + error_code). */
void do_task_switch(int32_t selector, bool has_error_code, int32_t error_code,
                    task_switch_source_t source);

/* cpu.rs:2803-2855 (ltr) and 2857-2906 (lldt).  load_ldt can fault (lookup_segment_selector
 * inside): it sets cpu->pagefault and gives up its effects, per the contract above. */
void load_tr(int32_t selector);
void load_ldt(int32_t selector);

/* cpu.rs:3000-3006.  cpl_changed invalidates the code-page cache of the current eip
 * (`*last_virt_eip = -1`) -- the FETCH path's cache in the port (cpu.c's get_phys_eip).
 *
 * The two are `static inline` HERE rather than definitions in exceptions.c for a build
 * reason that is also a correctness reason: instructions_0f.c (the 0F 34/35 sysenter/
 * sysexit pair) calls them, and that file is in BOTH source sets -- the production
 * binary and `make selftest`, which links no exceptions.c (its own recording definitions
 * replace this unit's symbols).  An inline definition has no link dependency and, unlike
 * a stub in the test binary, there is exactly ONE copy of the logic. */
static inline void cpl_changed(void) { cpu->last_virt_eip = -1; }

static inline void update_cs_size(bool new_size)
{
    if (cpu->is_32 != new_size) {
        cpu->is_32 = new_size;
    }
}

/* lar/lsl/verr/verw are misc_instr.c's (M1a-2 deferred them; misc_instr.h carries their
 * declarations next to the other misc_instr.rs ports). */

#endif /* V86C_EXCEPTIONS_H */
