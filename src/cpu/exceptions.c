/*
 * exceptions.c -- the machine's control machinery.  See exceptions.h for the interface,
 * the ported cpu.rs line ranges and the fault contract.
 *
 * TRANSCRIPTION RULES (the same three the landed files use, restated here because this
 * file is where they are most load-bearing):
 *   - `Result` -> cpu->pagefault (cpu_state.h).  The check sits exactly at upstream's
 *     `?` / `return_on_pagefault!` position, so a fault raised in the middle of a
 *     delivery or a far transfer ABORTS the operation with its partial effects KEPT
 *     (plan ss8/R3: the instruction is re-executed after the handler's iret, never
 *     undone).  Nothing in this file clears the flag.
 *   - `dbg_assert!` is `debug_assert!` (dbg.rs:63) and is COMPILED OUT of the pinned
 *     release wasm the oracle compares against, so it is dropped (the rule
 *     instructions_0f.c's header spells out: aborting on a property of the GUEST's
 *     bytes is a divergence from the reference, not one of the port's stops).
 *     MEASURED, and the reason this file keeps NO such assert: the first draft kept
 *     upstream's `dbg_assert!(offset <= cs_descriptor.effective_limit())` (cpu.rs:899) as
 *     a DBG_ASSERT and the crafted test binary ABORTED on it -- for a gate whose selector
 *     points at a descriptor that is not a code segment, which is legal guest input the
 *     reference simply carries on from.  The assert was the divergence, not the guest.
 *     The ONLY DBG_ASSERTs left are upstream's `x.unwrap()` on a range the code has
 *     already proved fault-free: those become DBG_ASSERT(!cpu->pagefault) with the
 *     reading misc_instr.c records -- upstream panics in BOTH builds, the port aborts in
 *     an assertions-on build (the one the Makefile builds) and continues in NDEBUG.
 *   - `panic!` is upstream's trap: the emulator cannot proceed.  The port stops the
 *     machine with the reason named (cpu.h's unimplemented_instruction -- the same stop
 *     a missing opcode uses, so a run that reached one produces NO state rather than a
 *     plausible wrong one).  Where the panic happens while a GUEST EXCEPTION is being
 *     delivered, the vector is recorded in `guest_exception` first: that is the state
 *     the fixture contract reports (exceptions.h, main.c).
 *
 * WHAT A GUEST OBSERVES WHEN A `panic!` ARM IS REACHED: the machine stops.  The
 * reachable ones for the gates of this unit are all "the guest's IDT/GDT is not set up
 * well enough for the emulator to deliver" -- MEASURED for the nasm corpus, which boots
 * a multiboot image (cr0.PE set by the loader, idtr_size 0) and takes #DE/#UD.
 */

#include "exceptions.h"

#include "debug.h"

#include <stdio.h> /* snprintf, for the stop messages that carry their evidence */

#include "cpu.h"
#include "flags.h"
#include "instructions.h" /* handle_irqs */
#include "memory.h"
#include "misc_instr.h"
#include "modrm.h" /* get_seg_cs */
#include "paging.h"
#include "string.h" /* test_privileges_for_io's declaration */
#include "instructions_64.h" /* push64 -- the 8-byte interrupt frame (this unit) */

/* the TSS offsets (cpu.rs:192-212) are in exceptions.h: they are part of the TSS
 * layout contract a task-switch caller (and the M1c test binary's fixture) needs. */

/* exceptions.h -- the fixture's signal record.  See the header note for what it does
 * and does not mean since delivery became real. */
int32_t guest_exception = -1;

/* ------------------------------------------------------------------ *
 * the two stop helpers
 * ------------------------------------------------------------------ */

/* upstream's `panic!`: the emulator cannot proceed (a wasm trap in the reference). */
static void panic_stop(const char *what)
{
    unimplemented_instruction(what);
}

/* The stop taken while a fault the guest took cannot be DELIVERED (an empty or
 * too-small IDT, an invalid gate, an unimplemented handler arm -- every one of them a
 * `panic!` upstream).  The vector is recorded because this is the machine's last state
 * and main.c's fixture mode reports it as the host signal the native harness took. */
static void stop_undeliverable(int32_t vector, const char *what)
{
    if (guest_exception < 0) {
        guest_exception = vector;
    }
    unimplemented_instruction(what);
}

/* ------------------------------------------------------------------ *
 * the triggers -- cpu.rs:3427-3474, 4546-4568
 *
 * Each REWINDS the instruction pointer first: the faulting instruction is the one that
 * re-executes after the handler's iret (plan ss8/R3).  `trigger_gp`/`trigger_np`/
 * `trigger_ss` pass an error code (upstream's `Some(code)`), the other three pass none
 * (upstream's `None`), which is the only thing the Option decides (paging.h).
 * ------------------------------------------------------------------ */
void trigger_de(void)
{
    cpu->instruction_pointer = cpu->previous_ip;
    call_interrupt_vector(CPU_EXCEPTION_DE, false, false, 0);
}

void trigger_ud(void)
{
    cpu->instruction_pointer = cpu->previous_ip;
    call_interrupt_vector(CPU_EXCEPTION_UD, false, false, 0);
}

void trigger_nm(void)
{
    cpu->instruction_pointer = cpu->previous_ip;
    call_interrupt_vector(CPU_EXCEPTION_NM, false, false, 0);
}

void trigger_gp(int32_t code)
{
    cpu->instruction_pointer = cpu->previous_ip;
    call_interrupt_vector(CPU_EXCEPTION_GP, false, true, code);
}

void trigger_np(int32_t code)
{
    cpu->instruction_pointer = cpu->previous_ip;
    call_interrupt_vector(CPU_EXCEPTION_NP, false, true, code);
}

void trigger_ss(int32_t code)
{
    cpu->instruction_pointer = cpu->previous_ip;
    call_interrupt_vector(CPU_EXCEPTION_SS, false, true, code);
}

/* ------------------------------------------------------------------ *
 * cpu.rs:473-480
 * ------------------------------------------------------------------ */
void switch_cs_real_mode(int32_t selector)
{

    cpu->sreg[CS] = (uint16_t)selector;
    cpu->segment_is_null[CS] = false;
    /* S3: the slot is 64-bit and a selector<<4 is an ADDRESS; the shift is done in the
     * address's own (unsigned) width so a large selector cannot sign-extend */
    cpu->segment_offsets[CS] = (uint32_t)selector << 4;
    /* S5a: a real-mode CS is never 64-bit (and this is also the reset path) */
    update_cs_size_long(false, false);
}

/* ------------------------------------------------------------------ *
 * cpu.rs:482-505 -- the stack a privilege change switches to, read out of the TSS.
 *
 * Upstream returns OrPageFault<(i32, i32)>.  The port returns false instead: the two
 * ways out are "a fault (flag set)" and "upstream's #TS panic", and both mean the same
 * thing to every caller -- return from the transfer.
 * ------------------------------------------------------------------ */
static bool get_tss_ss_esp(uint8_t dpl, int32_t *ss_out, int32_t *esp_out)
{
    if (cpu->tss_size_32) {
        uint32_t tss_stack_offset = (uint32_t)((dpl << 3) + 4);
        if (tss_stack_offset + 7 > cpu->segment_limits[TR]) {
            panic_stop("get_tss_ss_esp: #TS handler (upstream panic, cpu.rs:486)");
            return false;
        }
        /* S3: the base and the offset are both unsigned now; adding them before the
         * projection (translate_address, uint64_t) is exact where the old `(int32_t)` cast
         * of an unsigned offset was already the same value */
        uint32_t addr = translate_address_system_read(cpu->segment_offsets[TR] + tss_stack_offset);
        if (cpu->pagefault) {
            return false;
        }
        *esp_out = read32s(addr);
        *ss_out = read16(addr + 4);
    }
    else {
        uint32_t tss_stack_offset = (uint32_t)((dpl << 2) + 2);
        if (tss_stack_offset + 3 > cpu->segment_limits[TR]) {
            panic_stop("get_tss_ss_esp: #TS handler (upstream panic, cpu.rs:497)");
            return false;
        }
        /* S3: the base and the offset are both unsigned now; adding them before the
         * projection (translate_address, uint64_t) is exact where the old `(int32_t)` cast
         * of an unsigned offset was already the same value */
        uint32_t addr = translate_address_system_read(cpu->segment_offsets[TR] + tss_stack_offset);
        if (cpu->pagefault) {
            return false;
        }
        *ss_out = read16(addr + 2);
        *esp_out = read16(addr);
    }
    return true;
}

/* ------------------------------------------------------------------ *
 * cpu.rs:507-797 -- iret
 * ------------------------------------------------------------------ */
static void iret(bool is_16);

void iret16(void) { iret(true); }
void iret32(void) { iret(false); }

static void iret(bool is_16)
{
    if (vm86_mode() && getiopl() < 3) {
        /* vm86 mode, iopl != 3 */
        trigger_gp(0);
        return;
    }

    int32_t new_eip, new_cs, new_flags;
    if (is_16) {
        new_eip = safe_read16(get_stack_pointer(0));
        if (cpu->pagefault) return;
        new_cs = safe_read16(get_stack_pointer(2));
        if (cpu->pagefault) return;
        new_flags = safe_read16(get_stack_pointer(4));
        if (cpu->pagefault) return;
    }
    else {
        new_eip = safe_read32s(get_stack_pointer(0));
        if (cpu->pagefault) return;
        new_cs = safe_read16(get_stack_pointer(4));
        if (cpu->pagefault) return;
        new_flags = safe_read32s(get_stack_pointer(8));
        if (cpu->pagefault) return;
    }

    if (!cpu->protected_mode || (vm86_mode() && getiopl() == 3)) {
        /* upstream: `if new_eip as u32 & 0xFFFF0000 != 0 { panic!("#GP handler") }` */
        if (((uint32_t)new_eip & 0xFFFF0000u) != 0) {
            stop_undeliverable(CPU_EXCEPTION_GP, "iret: real-mode eip above 0xffff (upstream panic \"#GP handler\", cpu.rs:535)");
            return;
        }

        switch_cs_real_mode(new_cs);
        set_instruction_pointer(get_seg_cs() + new_eip);

        if (is_16) {
            update_eflags(new_flags | (cpu->flags & ~0xFFFF));
            adjust_stack_reg(3 * 2);
        }
        else {
            if (!cpu->protected_mode) {
                update_eflags((new_flags & 0x257FD5) | (cpu->flags & 0x1A0000));
            }
            else {
                update_eflags(new_flags);
            }
            adjust_stack_reg(3 * 4);
        }

        update_state_flags();
        handle_irqs();
        return;
    }


    if ((cpu->flags & FLAG_NT) != 0) {
        /* nested task: return to the task linked through the back-link field of the
         * current tss */
        uint64_t tss_offset = cpu->segment_offsets[TR]; /* S3: widened with the array */
        int32_t backlink = safe_read16(tss_offset + TSR_BACKLINK);
        if (cpu->pagefault) return;
        do_task_switch(backlink, false, 0, TASK_SWITCH_IRET);
        return;
    }

    if ((new_flags & FLAG_VM) != 0) {
        if (cpu->cpl == 0) {
            /* return to virtual 8086 mode */

            int32_t temp_esp = safe_read32s(get_stack_pointer(12));
            if (cpu->pagefault) return;
            int32_t temp_ss = safe_read16(get_stack_pointer(16));
            if (cpu->pagefault) return;

            int32_t new_es = safe_read16(get_stack_pointer(20));
            if (cpu->pagefault) return;
            int32_t new_ds = safe_read16(get_stack_pointer(24));
            if (cpu->pagefault) return;
            int32_t new_fs = safe_read16(get_stack_pointer(28));
            if (cpu->pagefault) return;
            int32_t new_gs = safe_read16(get_stack_pointer(32));
            if (cpu->pagefault) return;

            /* no exceptions below */

            update_eflags(new_flags);
            cpu->flags |= FLAG_VM;

            switch_cs_real_mode(new_cs);
            set_instruction_pointer(get_seg_cs() + (new_eip & 0xFFFF));

            if (!switch_seg(ES, new_es) || !switch_seg(DS, new_ds) || !switch_seg(FS, new_fs) ||
                !switch_seg(GS, new_gs)) {
                /* XXX: Should be checked before side effects (upstream dbg_assert(false),
                 * dropped) */
            }

            adjust_stack_reg(9 * 4); /* 9 dwords: eip, cs, flags, esp, ss, es, ds, fs, gs */

            write_reg32(ESP, temp_esp);
            if (!switch_seg(SS, temp_ss)) {
                /* XXX (upstream dbg_assert(false), dropped) */
            }

            cpu->cpl = 3;
            cpl_changed();

            update_cs_size_long(false, false); /* S5a: an iret's 16-bit CS is not 64-bit */
            update_state_flags();

            /* iret end */
            return;
        }
        else {
            /* upstream dbg_log!("vm86 flag ignored because cpl != 0") */
            new_flags &= ~FLAG_VM;
        }
    }

    /* protected mode return */
    seg_selector_t cs_selector = seg_selector_of_u16((uint16_t)new_cs);
    seg_descriptor_t cs_descriptor;
    int32_t cs_descriptor_address;
    selector_lookup_t lookup = lookup_segment_selector(cs_selector, &cs_descriptor, &cs_descriptor_address);
    if (cpu->pagefault) {
        return;
    }
    switch (lookup) {
    case SELECTOR_LOOKUP_IS_NULL:
        panic_stop("iret: CS selector is null (upstream panic \"Unimplemented: CS selector is null\", cpu.rs:630)");
        return;
    case SELECTOR_LOOKUP_OUTSIDE:
        panic_stop("iret: CS selector is invalid (upstream panic \"Unimplemented: CS selector is invalid\", cpu.rs:632)");
        return;
    case SELECTOR_LOOKUP_OK:
        break;
    }

    if ((uint32_t)new_eip > seg_descriptor_effective_limit(cs_descriptor)) {
        /* upstream dbg_log! then #gp */
        trigger_gp(new_cs & ~3);
        return;
    }

    if (!seg_descriptor_is_present(cs_descriptor)) {
        panic_stop("iret: CS not present (upstream panic, cpu.rs:647)");
        return;
    }
    if (!seg_descriptor_is_executable(cs_descriptor)) {
        panic_stop("iret: CS not executable (upstream panic, cpu.rs:650)");
        return;
    }
    if (seg_selector_rpl(cs_selector) < cpu->cpl) {
        panic_stop("iret: rpl < cpl (upstream panic, cpu.rs:653)");
        return;
    }
    if (seg_descriptor_is_dc(cs_descriptor) && seg_descriptor_dpl(cs_descriptor) > seg_selector_rpl(cs_selector)) {
        panic_stop("iret: conforming and dpl > rpl (upstream panic, cpu.rs:656)");
        return;
    }

    if (!seg_descriptor_is_dc(cs_descriptor) && seg_selector_rpl(cs_selector) != seg_descriptor_dpl(cs_descriptor)) {
        /* upstream dbg_log! then #gp */
        trigger_gp(new_cs & ~3);
        return;
    }

    if (seg_selector_rpl(cs_selector) > cpu->cpl) {
        /* outer privilege return */
        int32_t temp_esp, temp_ss;
        if (is_16) {
            temp_esp = safe_read16(get_stack_pointer(6));
            if (cpu->pagefault) return;
            temp_ss = safe_read16(get_stack_pointer(8));
            if (cpu->pagefault) return;
        }
        else {
            temp_esp = safe_read32s(get_stack_pointer(12));
            if (cpu->pagefault) return;
            temp_ss = safe_read16(get_stack_pointer(16));
            if (cpu->pagefault) return;
        }

        seg_selector_t ss_selector = seg_selector_of_u16((uint16_t)temp_ss);
        seg_descriptor_t ss_descriptor;
        int32_t ss_descriptor_address;
        lookup = lookup_segment_selector(ss_selector, &ss_descriptor, &ss_descriptor_address);
        if (cpu->pagefault) {
            return;
        }
        if (lookup == SELECTOR_LOOKUP_IS_NULL) {
            trigger_gp(0);
            return;
        }
        if (lookup == SELECTOR_LOOKUP_OUTSIDE) {
            trigger_gp(temp_ss & ~3);
            return;
        }

        uint8_t new_cpl = seg_selector_rpl(cs_selector);

        if (seg_descriptor_is_system(ss_descriptor) || seg_selector_rpl(ss_selector) != new_cpl ||
            !seg_descriptor_is_writable(ss_descriptor) || seg_descriptor_dpl(ss_descriptor) != new_cpl) {
            trigger_gp(temp_ss & ~3);
            return;
        }

        if (!seg_descriptor_is_present(ss_descriptor)) {
            trigger_ss(temp_ss & ~3);
            return;
        }

        /* no exceptions below */

        if (is_16) {
            update_eflags(new_flags | (cpu->flags & ~0xFFFF));
        }
        else {
            update_eflags(new_flags);
        }

        cpu->cpl = new_cpl;
        cpl_changed();

        if (!switch_seg(SS, temp_ss)) {
            /* XXX (upstream dbg_assert(false), dropped) */
        }

        set_stack_reg(temp_esp);

        if (cpu->cpl == 0 && !is_16) {
            cpu->flags = (cpu->flags & ~FLAG_VIF & ~FLAG_VIP) | (new_flags & (FLAG_VIF | FLAG_VIP));
        }

        /* upstream: `for reg in [ES, DS, FS, GS]` -- the FOUR DATA SEGMENTS.  CS and SS
         * are NOT in the list; they were loaded by this instruction's own selector handling
         * above.
         *
         * CORRECTED IN M2-3 (HANDOFF=v86c): this loop was transcribed as
         * `for (reg = ES; reg <= GS; reg++)`, which in v86's index order (ES=0, CS=1, SS=2,
         * DS=3, FS=4, GS=5) also visits CS and SS, so a data segment whose DPL was below the
         * new CPL ZEROED sreg[CS]/sreg[SS] and set segment_is_null on them.  MEASURED on a
         * real Linux guest: the first iret32 back to userspace did exactly that, and the
         * port then aborted at src/cpu/modrm.c:89's DBG_ASSERT(segment != CS && segment !=
         * SS) on the guest's next stack access, where the reference (whose dbg_assert is
         * compiled out and whose list is the four data segments) runs on.  The regression
         * probe is m1c_selftest.c's "the privilege-changing iret32 nulls the four DATA
         * segments and leaves CS and SS alone". */
        static const int iret_data_segments[4] = { ES, DS, FS, GS };
        for (unsigned iret_reg_i = 0; iret_reg_i < 4; iret_reg_i++) {
            int reg = iret_data_segments[iret_reg_i];
            uint8_t access = cpu->segment_access_bytes[reg];
            uint8_t dpl = (uint8_t)(access >> 5 & 3);
            bool executable = (access & 8) == 8;
            bool conforming = (access & 4) == 4;
            if (dpl < cpu->cpl && !(executable && conforming)) {
                /* upstream dbg_log! dropped */
                cpu->segment_is_null[reg] = true;
                cpu->sreg[reg] = 0;
            }
        }
    }
    else if (seg_selector_rpl(cs_selector) == cpu->cpl) {
        /* same privilege return: no exceptions below */
        if (is_16) {
            adjust_stack_reg(3 * 2);
            update_eflags(new_flags | (cpu->flags & ~0xFFFF));
        }
        else {
            adjust_stack_reg(3 * 4);
            update_eflags(new_flags);
        }

        /* update vip and vif, which are not changed by update_eflags */
        if (cpu->cpl == 0 && !is_16) {
            cpu->flags = (cpu->flags & ~FLAG_VIF & ~FLAG_VIP) | (new_flags & (FLAG_VIF | FLAG_VIP));
        }
    }
    else {
        /* upstream dbg_assert!(false), dropped */
    }

    cpu->sreg[CS] = (uint16_t)new_cs;

    update_cs_size_long(seg_descriptor_is_32(cs_descriptor), seg_descriptor_is_long(cs_descriptor));

    cpu->segment_limits[CS] = seg_descriptor_effective_limit(cs_descriptor);
    cpu->segment_offsets[CS] = seg_descriptor_base(cs_descriptor);
    cpu->segment_access_bytes[CS] = seg_descriptor_access_byte(cs_descriptor);

    set_instruction_pointer(new_eip + get_seg_cs());

    update_state_flags();

    /* iret end */
    handle_irqs();
}

/* ------------------------------------------------------------------ *
 * cpu.rs:799-1126 -- call_interrupt_vector
 *
 * The nested-fault shape of plan ss8/R3 lives here: `return_on_pagefault!` at
 * upstream's position means a fault while reading the IDT (or the TSS, or the target
 * stack) ABORTS the in-flight delivery and leaves the new fault recorded and delivered
 * -- the `return`s below are that abort, and none of them undoes the delivery's partial
 * effects (upstream has none of its own yet at those points except the ones kept).
 * ------------------------------------------------------------------ */
/* THE NESTED-DELIVERY BOUND.  MEASURED (results/x86-64-walkoob2.md): the amd64 kernel's #PF
 * delivery reads its own IDT at 0x9BC04000 -- a linear address above 4 GiB -- and something in
 * that delivery faults again, so the port RE-ENTERED call_interrupt_vector without bound and
 * died of a HOST STACK OVERFLOW (SIGSEGV with no output).  Hardware answers a fault raised
 * while delivering a fault with #DF (and a fault during #DF with a triple fault); this port
 * must not recurse into the host's stack.  The bound is on the EMULATOR's recursion, not a
 * refusal of the guest's deliveries: depth 4 is far above anything the machine legitimately
 * nests, and the stop names what happened. */
static int delivery_depth;

static void call_interrupt_vector_inner(int32_t interrupt, bool is_int, bool has_error_code,
                                        int32_t error_code);

void call_interrupt_vector(int32_t interrupt, bool is_int, bool has_error_code, int32_t error_code)
{
    if (delivery_depth >= v86c_dbg_delivery_bound) {
        unimplemented_instruction(
            "a fault raised while delivering a fault: the delivery re-entered four times"
            " (hardware would raise #DF; the port stops rather than recursing)");
        return;
    }
    delivery_depth++;
    call_interrupt_vector_inner(interrupt, is_int, has_error_code, error_code);
    delivery_depth--;
}

static void call_interrupt_vector_inner(int32_t interrupt, bool is_int, bool has_error_code,
                                        int32_t error_code)
{
    if (cpu->protected_mode) {
        if (vm86_mode() && (cpu->cr[4] & CR4_VME) != 0) {
            panic_stop("call_interrupt_vector: VME (upstream panic \"Unimplemented: VME\", cpu.rs:806)");
            return;
        }

        if (vm86_mode() && is_int && getiopl() < 3) {
            trigger_gp(0);
            return;
        }

        /* THE 64-BIT GATE IS 16 BYTES WIDE, so the stride is `vector << 4` -- the 8-byte
         * stride below is the 32-bit gate's.  MEASURED, and it CORRECTS
         * results/x86-64-idt64.md's claim that the IDT at the stop was all-null (see the
         * offset64 note further down): the stride is the reason a real #PF gate at vector 14
         * was read as slot 7's null descriptor. */
        bool lm64 = long_mode_64();
        int32_t gate_size = lm64 ? 16 : 8;

        if ((interrupt << 3 | 7) > cpu->idtr_size) {
            /* THE reachable-path stop for the differential corpus: a multiboot guest
             * (cr0.PE set by the loader, idtr_size 0) taking #DE/#UD lands exactly here,
             * which upstream answers with `panic!("Unimplemented: #GP handler")`.  The
             * vector is recorded for main.c's fixture mode. */
            stop_undeliverable(interrupt, "call_interrupt_vector: idtr_size too small for the vector (upstream panic \"Unimplemented: #GP handler\", cpu.rs:819)");
            return;
        }

        int32_t descriptor_address =
            (int32_t)translate_address_system_read(
                cpu->idtr_offset + (uint32_t)(interrupt * gate_size));
        if (cpu->pagefault) return;

        intr_descriptor_t descriptor = intr_descriptor_of_u64((uint64_t)read64s((uint32_t)descriptor_address));

        int32_t offset = intr_descriptor_offset(descriptor);
        /* THE 64-BIT HANDLER ADDRESS: offset_low (bytes 0-1) | offset_middle (bytes 6-7)
         * << 16 | offset_high (bytes 8-11) << 32.  The low 32 bits are exactly what
         * intr_descriptor_offset composes (it reads the middle field from bits 32-47 of the
         * first qword, which is the same place), so only the HIGH half is new here.
         * MEASURED: the decompressor's #PF gate decodes to 0x4F3F870 with selector 0x0010
         * and type 0xF (trap, present, DPL 0) -- a deliverable gate, which is what makes the
         * 64-bit interrupt frame reachable at all. */
        int64_t offset64 = offset;
        if (lm64) {
            uint64_t high_half = (uint64_t)read64s((uint32_t)(descriptor_address + 8));
            offset64 = (int64_t)((uint64_t)(uint32_t)offset | (high_half << 32));
        }
        int32_t selector = intr_descriptor_selector(descriptor);
        uint8_t dpl = intr_descriptor_dpl(descriptor);
        uint8_t gate_type = intr_descriptor_gate_type(descriptor);

        if (is_int && dpl < cpu->cpl) {
            trigger_gp(interrupt << 3 | 2);
            return;
        }

        if (gate_type != INTR_GATE_TRAP && gate_type != INTR_GATE_INTERRUPT && gate_type != INTR_GATE_TASK) {
            /* S11/S12 GAP: the message carries the EVIDENCE, not just the verdict -- the vector,
             * the IDT base/limit, the descriptor address and the descriptor itself.  Without them
             * "invalid gate type" is a symptom a reader cannot act on (it cost this lane one
             * investigation to learn that the IDT had never been loaded at the high base). */
            char stop[384];
            snprintf(stop, sizeof(stop),
                     "call_interrupt_vector: invalid gate type %u for vector %d (idtr base 0x%llx"
                     " limit %d, descriptor at 0x%x = 0x%llx, faulting rip 0x%llx bytes %02x %02x %02x %02x %02x %02x,"
                     " cpl %d cr2 0x%llx cr3 0x%llx)",
                     (unsigned)gate_type, (int)interrupt,
                     (unsigned long long)cpu->idtr_offset, (int)cpu->idtr_size,
                     (unsigned)descriptor_address, (unsigned long long)descriptor.raw,
                     (unsigned long long)cpu->instruction_pointer,
                     (unsigned)read8((uint32_t)translate_address_read(cpu->instruction_pointer)),
                     (unsigned)read8((uint32_t)translate_address_read(cpu->instruction_pointer + 1)),
                     (unsigned)read8((uint32_t)translate_address_read(cpu->instruction_pointer + 2)),
                     (unsigned)read8((uint32_t)translate_address_read(cpu->instruction_pointer + 3)),
                     (unsigned)read8((uint32_t)translate_address_read(cpu->instruction_pointer + 4)),
                     (unsigned)read8((uint32_t)translate_address_read(cpu->instruction_pointer + 5)),
                     (int)cpu->cpl, (unsigned long long)(uint32_t)cpu->cr[2],
                     (unsigned long long)cpu->cr[3]);
            panic_stop(stop);
            return;
        }

        if (!intr_descriptor_reserved_zeros_are_valid(descriptor)) {
            panic_stop("call_interrupt_vector: reserved-zeros violated (upstream panic \"Unimplemented: #GP handler\", cpu.rs:861)");
            return;
        }

        if (!intr_descriptor_is_present(descriptor)) {
            trigger_np(interrupt << 3 | 2);
            return;
        }

        if (gate_type == INTR_GATE_TASK) {
            /* task gate */
            do_task_switch(selector, has_error_code, error_code, TASK_SWITCH_CALL_OR_INT);
            return;
        }

        seg_descriptor_t cs_descriptor;
        int32_t cs_descriptor_address;
        selector_lookup_t lookup =
            lookup_segment_selector(seg_selector_of_u16((uint16_t)selector), &cs_descriptor, &cs_descriptor_address);
        if (cpu->pagefault) {
            return;
        }
        if (lookup == SELECTOR_LOOKUP_IS_NULL) {
            panic_stop("call_interrupt_vector: target CS is null (upstream panic \"Unimplemented: #GP handler\", cpu.rs:891)");
            return;
        }
        if (lookup == SELECTOR_LOOKUP_OUTSIDE) {
            panic_stop("call_interrupt_vector: target CS outside the table limit (upstream panic \"Unimplemented: #GP handler (error code)\", cpu.rs:895)");
            return;
        }


        if (!seg_descriptor_is_executable(cs_descriptor) || seg_descriptor_dpl(cs_descriptor) > cpu->cpl) {
            panic_stop("call_interrupt_vector: target CS not executable (upstream panic \"Unimplemented: #GP handler\", cpu.rs:903)");
            return;
        }
        if (!seg_descriptor_is_present(cs_descriptor)) {
            /* kvm-unit-test */
            trigger_np(interrupt << 3 | 2);
            return;
        }

        int32_t old_flags = get_eflags();
        uint8_t cs_dpl = seg_descriptor_dpl(cs_descriptor);

        if (!seg_descriptor_is_dc(cs_descriptor) && cs_dpl < cpu->cpl) {
            /* inter-privilege-level interrupt (interrupt from vm86 mode) */
            if (lm64) {
                /* NAMED STOP: the 64-bit frame's privilege switch (an 8-byte SS/RSP pair from
                 * the TSS, and the IST stack when the gate selects one) is not implemented.
                 * The decompressor's faults all arrive at CPL 0 on the same stack, so this is
                 * a boundary, not a shortcut. */
                panic_stop("call_interrupt_vector: long mode: a CPL-changing interrupt delivery"
                           " (the 8-byte SS/RSP frame and the IST stack are not implemented)");
                return;
            }
            if ((old_flags & FLAG_VM) != 0 && cs_dpl != 0) {
                panic_stop("call_interrupt_vector: non-0 CS dpl in vm86 mode (upstream panic \"Unimplemented: #GP handler for non-0 cs segment dpl when in vm86 mode\", cpu.rs:919)");
                return;
            }

            int32_t new_ss, new_esp;
            if (!get_tss_ss_esp(cs_dpl, &new_ss, &new_esp)) {
                return;
            }

            seg_selector_t ss_segment_selector = seg_selector_of_u16((uint16_t)new_ss);
            seg_descriptor_t ss_segment_descriptor;
            int32_t ss_descriptor_address;
            lookup = lookup_segment_selector(ss_segment_selector, &ss_segment_descriptor, &ss_descriptor_address);
            if (cpu->pagefault) {
                return;
            }
            if (lookup != SELECTOR_LOOKUP_OK) {
                panic_stop("call_interrupt_vector: #TS handler, SS selector null or outside the table limit (upstream panic, cpu.rs:932)");
                return;
            }

            /* upstream dbg_asserts on the new stack's limit/system/writable bits and its
             * rpl/dpl against the gate's cs -- dropped (they are properties of the
             * guest's bytes). */
            if (seg_selector_rpl(ss_segment_selector) != cs_dpl) {
                panic_stop("call_interrupt_vector: #TS handler, SS rpl (upstream panic, cpu.rs:945)");
                return;
            }
            if (seg_descriptor_dpl(ss_segment_descriptor) != cs_dpl || !seg_descriptor_is_rw(ss_segment_descriptor)) {
                panic_stop("call_interrupt_vector: #TS handler, SS dpl/rw (upstream panic, cpu.rs:950)");
                return;
            }
            if (!seg_descriptor_is_present(ss_segment_descriptor)) {
                panic_stop("call_interrupt_vector: #TS handler, SS not present (upstream panic, cpu.rs:953)");
                return;
            }

            int32_t old_esp = read_reg32(ESP);
            int32_t old_ss = (int32_t)cpu->sreg[SS];

            int32_t error_code_space = has_error_code ? 1 : 0;
            int32_t vm86_space = (old_flags & FLAG_VM) == FLAG_VM ? 4 : 0;
            int32_t bytes_per_arg = intr_descriptor_is_32(descriptor) ? 4 : 2;

            int32_t stack_space = bytes_per_arg * (5 + error_code_space + vm86_space);
            int32_t new_stack_pointer =
                seg_descriptor_base(ss_segment_descriptor) +
                (seg_descriptor_is_32(ss_segment_descriptor) ? new_esp - stack_space
                                                             : ((new_esp - stack_space) & 0xFFFF));

            (void)translate_address_system_write(new_stack_pointer);
            if (cpu->pagefault) return;
            (void)translate_address_system_write(seg_descriptor_base(ss_segment_descriptor) + new_esp - 1);
            if (cpu->pagefault) return;

            /* no exceptions below */
            cpu->cpl = cs_dpl;
            cpl_changed();

            update_cs_size_long(seg_descriptor_is_32(cs_descriptor), seg_descriptor_is_long(cs_descriptor));

            cpu->flags &= ~FLAG_VM & ~FLAG_RF;

            if (!switch_seg(SS, new_ss)) {
                /* XXX (upstream dbg_assert(false), dropped) */
            }
            set_stack_reg(new_esp);

            /* XXX: #SS if stack would cross stack limit */

            if ((old_flags & FLAG_VM) != 0) {
                /* upstream: `if !descriptor.is_32() { dbg_assert!(false) } else { push }` --
                 * the debug assert is dropped, so only the 32-bit arm carries the pushes */
                if (intr_descriptor_is_32(descriptor)) {
                    push32((int32_t)cpu->sreg[GS]);
                    DBG_ASSERT(!cpu->pagefault);
                    push32((int32_t)cpu->sreg[FS]);
                    DBG_ASSERT(!cpu->pagefault);
                    push32((int32_t)cpu->sreg[DS]);
                    DBG_ASSERT(!cpu->pagefault);
                    push32((int32_t)cpu->sreg[ES]);
                    DBG_ASSERT(!cpu->pagefault);
                }
            }

            if (intr_descriptor_is_32(descriptor)) {
                push32(old_ss);
                DBG_ASSERT(!cpu->pagefault);
                push32(old_esp);
                DBG_ASSERT(!cpu->pagefault);
            }
            else {
                push16(old_ss);
                DBG_ASSERT(!cpu->pagefault);
                push16(old_esp & 0xFFFF);
                DBG_ASSERT(!cpu->pagefault);
            }
        }
        else if (seg_descriptor_is_dc(cs_descriptor) || cs_dpl == cpu->cpl) {
            /* intra-privilege-level interrupt */
            if ((cpu->flags & FLAG_VM) != 0) {
                /* upstream `dbg_assert!(false, "check error code")` dropped */
                trigger_gp(selector & ~3);
                return;
            }

            int32_t bytes_per_arg = lm64 ? 8 : (intr_descriptor_is_32(descriptor) ? 4 : 2);
            int32_t error_code_space = has_error_code ? 1 : 0;

            int32_t stack_space = bytes_per_arg * (3 + error_code_space);

            /* XXX: with current cpl or with cpl 0? */
            writable_or_pagefault(get_stack_pointer(-stack_space), stack_space);
            if (cpu->pagefault) return;

            /* no exceptions below */
        }
        else {
            panic_stop("call_interrupt_vector: #GP handler (upstream panic \"Unimplemented: #GP handler\", cpu.rs:1042)");
            return;
        }

        /* XXX: #SS if stack would cross stack limit */
        if (lm64) {
            /* THE 64-BIT FRAME: RFLAGS, CS and RIP are EIGHT bytes each (and the error code,
             * when the vector pushes one, is eight too), which is why the 32-bit arm's
             * `is_32` test cannot be reused -- a 64-bit gate has the SAME type bit set (0xE/0xF
             * carry bit 3) and would take the 4-byte path.  MEASURED: this is the frame the
             * `stop-is-the-64bit-interrupt-frame` step in tests/gate-x86-64.sh could only
             * assert the STOP of; results/x86-64-rep.md records the delivered frame. */
            push64(old_flags);
            DBG_ASSERT(!cpu->pagefault);
            push64((int32_t)cpu->sreg[CS]);
            DBG_ASSERT(!cpu->pagefault);
            push64(cpu->instruction_pointer);
            DBG_ASSERT(!cpu->pagefault);

            if (has_error_code) {
                push64(error_code);
                DBG_ASSERT(!cpu->pagefault);
            }
        }
        else if (intr_descriptor_is_32(descriptor)) {
            push32(old_flags);
            DBG_ASSERT(!cpu->pagefault);
            push32((int32_t)cpu->sreg[CS]);
            DBG_ASSERT(!cpu->pagefault);
            push32(get_real_eip());
            DBG_ASSERT(!cpu->pagefault);

            if (has_error_code) {
                push32(error_code);
                DBG_ASSERT(!cpu->pagefault);
            }
        }
        else {
            push16(old_flags & 0xFFFF);
            DBG_ASSERT(!cpu->pagefault);
            push16((int32_t)cpu->sreg[CS]);
            DBG_ASSERT(!cpu->pagefault);
            push16(get_real_eip() & 0xFFFF);
            DBG_ASSERT(!cpu->pagefault);

            if (has_error_code) {
                push16(error_code);
                DBG_ASSERT(!cpu->pagefault);
            }

            offset &= 0xFFFF;
        }

        if ((old_flags & FLAG_VM) != 0) {
            if (!switch_seg(GS, 0) || !switch_seg(FS, 0) || !switch_seg(DS, 0) || !switch_seg(ES, 0)) {
                /* can't fail (upstream dbg_assert(false), dropped) */
            }
        }

        cpu->sreg[CS] = (uint16_t)(((uint16_t)selector & ~3) | cpu->cpl);

        update_cs_size_long(seg_descriptor_is_32(cs_descriptor), seg_descriptor_is_long(cs_descriptor));

        cpu->segment_limits[CS] = seg_descriptor_effective_limit(cs_descriptor);
        cpu->segment_offsets[CS] = seg_descriptor_base(cs_descriptor);
        cpu->segment_access_bytes[CS] = seg_descriptor_access_byte(cs_descriptor);

        set_instruction_pointer(get_seg_cs() + (uint64_t)(lm64 ? offset64 : offset));

        cpu->flags &= ~FLAG_NT & ~FLAG_VM & ~FLAG_RF & ~FLAG_TRAP;

        if (gate_type == INTR_GATE_INTERRUPT) {
            /* clear int flag for interrupt gates */
            cpu->flags &= ~FLAG_INTERRUPT;
        }
        else {
            if ((cpu->flags & FLAG_INTERRUPT) != 0 && (old_flags & FLAG_INTERRUPT) == 0) {
                handle_irqs();
            }
        }

        update_state_flags();
    }
    else {
        /* call the 4-byte cs:ip interrupt vector from the IVT at cpu.memory 0 */
        uint32_t index = (uint32_t)(interrupt << 2);
        int32_t new_ip = read16(index);
        int32_t new_cs = read16(index + 2);


        /* XXX: #SS if stack would cross stack limit */

        /* push flags, cs:ip */
        push16(get_eflags() & 0xFFFF);
        DBG_ASSERT(!cpu->pagefault);
        push16((int32_t)cpu->sreg[CS]);
        DBG_ASSERT(!cpu->pagefault);
        push16(get_real_eip() & 0xFFFF);
        DBG_ASSERT(!cpu->pagefault);

        cpu->flags &= ~FLAG_INTERRUPT & ~FLAG_AC & ~FLAG_TRAP;

        switch_cs_real_mode(new_cs);
        set_instruction_pointer(get_seg_cs() + new_ip);
        update_state_flags();
    }
}

/* ------------------------------------------------------------------ *
 * cpu.rs:2638-2673 -- lookup_segment_selector
 * ------------------------------------------------------------------ */
selector_lookup_t lookup_segment_selector(seg_selector_t selector, seg_descriptor_t *desc,
                                          int32_t *descriptor_address)
{
    if (seg_selector_is_null(selector)) {
        return SELECTOR_LOOKUP_IS_NULL;
    }

    uint32_t table_offset;
    uint32_t table_limit;
    if (seg_selector_is_gdt(selector)) {
        table_offset = (uint32_t)cpu->gdtr_offset; /* S3: the 32-bit model's table base */
        table_limit = (uint32_t)cpu->gdtr_size;
    }
    else {
        table_offset = (uint32_t)cpu->segment_offsets[LDTR];
        table_limit = cpu->segment_limits[LDTR];
    }

    if ((uint32_t)seg_selector_descriptor_offset(selector) > table_limit) {
        /* upstream dbg_log! dropped */
        return SELECTOR_LOOKUP_OUTSIDE;
    }

    int32_t address = (int32_t)seg_selector_descriptor_offset(selector) + (int32_t)table_offset;

    uint32_t physical = translate_address_system_read(address);
    if (cpu->pagefault) {
        /* the return value is meaningless, as in Rust's Err(()) arm -- the caller must
         * read cpu->pagefault first (exceptions.h) */
        return SELECTOR_LOOKUP_OK;
    }

    *desc = seg_descriptor_of_u64((uint64_t)read64s(physical));
    *descriptor_address = address;
    return SELECTOR_LOOKUP_OK;
}

/* ------------------------------------------------------------------ *
 * cpu.rs:2676-2801 -- switch_seg
 *
 * Declared in misc_instr.h (lss16/lss32 were its first callers); DEFINED here, the unit
 * the plan assigns it to (ss7 step 17).
 * ------------------------------------------------------------------ */
bool switch_seg(int32_t reg, int32_t selector_raw)
{

    if (vm86_mode()) {
        /* TODO: Should set segment_limits and segment_access_bytes if ever implemented in
         *       get_seg (only vm86, not in real mode) */
    }

    if (!cpu->protected_mode || vm86_mode()) {
        cpu->sreg[reg] = (uint16_t)selector_raw;
        cpu->segment_is_null[reg] = false;
        cpu->segment_offsets[reg] = (uint32_t)selector_raw << 4;

        if (reg == SS) {
            cpu->stack_size_32 = false;
        }
        update_state_flags();
        return true;
    }

    seg_selector_t selector = seg_selector_of_u16((uint16_t)selector_raw);
    seg_descriptor_t descriptor;
    int32_t descriptor_address;
    selector_lookup_t lookup = lookup_segment_selector(selector, &descriptor, &descriptor_address);
    if (cpu->pagefault) {
        return false;
    }
    if (lookup == SELECTOR_LOOKUP_IS_NULL) {
        /* THE 64-BIT-MODE EXCEPTION TO THE NULL-SELECTOR RULE.  In protected mode SS cannot be
         * loaded with a null selector (the arm below), and this port kept that rule in 64-bit
         * mode too -- which STOPS A REAL GUEST (MEASURED, this unit's finding): the amd64
         * decompressor's 0x200 entry point -- the ABI-immutable 64-bit entry -- begins
         *
         *     cld / cli / xorl %eax,%eax / movl %eax,%ds / movl %eax,%es / movl %eax,%ss ...
         *
         * (linux v6.1 arch/x86/boot/compressed/head_64.S:334-356, READ) and the port raised
         * #GP(0) on the `movl %eax,%ss` at guest 0x100208, whose IDT is still null: the guest
         * could not take the fault and the machine stopped.  Hardware runs that sequence, so
         * the null-selector rule cannot apply to SS in 64-bit mode at CPL 0.  The condition
         * below is QEMU's (target/i386/tcg/seg_helper.c:1293-1302, READ):
         * `seg_reg == R_SS && (!(hflags & HF_CS64_MASK) || cpl == 3)` -> #GP.  Note the
         * CPL-3 half: at CPL 3 in 64-bit mode a null SS IS a #GP, so this is an exception for
         * the kernel-mode case only, not a general licence. */
        if (reg == SS && !(long_mode_64() && cpu->cpl == 0)) {
            /* upstream dbg_log! dropped */
            trigger_gp(0);
            return false;
        }
        else {
            /* es, ds, fs, gs -- and SS in 64-bit mode at CPL 0 */
            cpu->sreg[reg] = (uint16_t)selector_raw;
            cpu->segment_is_null[reg] = true;
            /* In 64-bit mode CS/DS/ES/SS bases are architecturally 0, and modrm_resolve64 adds
             * segment_offsets[] unconditionally, so the base must be CLEARED here rather than
             * left at whatever the previous selector loaded.  FS/GS are NOT cleared: their
             * base is the MSR's (cpu_state.h:428) and a selector load does not write it. */
            if (long_mode_64() && reg != FS && reg != GS) {
                cpu->segment_offsets[reg] = 0;
            }
            update_state_flags();
            return true;
        }
    }
    if (lookup == SELECTOR_LOOKUP_OUTSIDE) {
        /* upstream dbg_log! + dbg_trace! dropped */
        trigger_gp(selector_raw & ~3);
        return false;
    }

    if (reg == SS) {
        if (seg_descriptor_is_system(descriptor) || seg_selector_rpl(selector) != cpu->cpl ||
            !seg_descriptor_is_writable(descriptor) || seg_descriptor_dpl(descriptor) != cpu->cpl) {
            trigger_gp(selector_raw & ~3);
            return false;
        }

        if (!seg_descriptor_is_present(descriptor)) {
            trigger_ss(selector_raw & ~3);
            return false;
        }

        cpu->stack_size_32 = seg_descriptor_is_32(descriptor);
    }
    else {
        if (seg_descriptor_is_system(descriptor) || !seg_descriptor_is_readable(descriptor) ||
            (!seg_descriptor_is_conforming_executable(descriptor) &&
             (seg_selector_rpl(selector) > seg_descriptor_dpl(descriptor) ||
              cpu->cpl > seg_descriptor_dpl(descriptor)))) {
            /* upstream dbg_log! (with the same fields) + dbg_trace! dropped */
            trigger_gp(selector_raw & ~3);
            return false;
        }

        if (!seg_descriptor_is_present(descriptor)) {
            trigger_np(selector_raw & ~3);
            return false;
        }
    }

    if (!seg_descriptor_accessed(descriptor)) {
        descriptor = seg_descriptor_set_accessed(descriptor);

        int32_t address = (int32_t)translate_address_system_write(descriptor_address + 5);
        DBG_ASSERT(!cpu->pagefault); /* upstream .unwrap() */
        write8((uint32_t)address, (int32_t)seg_descriptor_access_byte(descriptor));
    }

    cpu->segment_is_null[reg] = false;
    cpu->segment_limits[reg] = seg_descriptor_effective_limit(descriptor);
    cpu->segment_offsets[reg] = seg_descriptor_base(descriptor);
    cpu->segment_access_bytes[reg] = seg_descriptor_access_byte(descriptor);
    cpu->sreg[reg] = (uint16_t)selector_raw;

    update_state_flags();

    return true;
}

/* ------------------------------------------------------------------ *
 * cpu.rs:1128-1505 -- far_jump
 * ------------------------------------------------------------------ */
void far_jump(int32_t eip, int32_t selector, bool is_call, bool is_osize_32)
{

    if (!cpu->protected_mode || vm86_mode()) {
        if (is_call) {
            if (is_osize_32) {
                writable_or_pagefault(get_stack_pointer(-8), 8);
                if (cpu->pagefault) return;

                push32((int32_t)cpu->sreg[CS]);
                DBG_ASSERT(!cpu->pagefault);
                push32(get_real_eip());
                DBG_ASSERT(!cpu->pagefault);
            }
            else {
                writable_or_pagefault(get_stack_pointer(-4), 4);
                if (cpu->pagefault) return;

                push16((int32_t)cpu->sreg[CS]);
                DBG_ASSERT(!cpu->pagefault);
                push16(get_real_eip());
                DBG_ASSERT(!cpu->pagefault);
            }
        }
        switch_cs_real_mode(selector);
        set_instruction_pointer(get_seg_cs() + eip);
        update_state_flags();
        return;
    }

    seg_selector_t cs_selector = seg_selector_of_u16((uint16_t)selector);
    seg_descriptor_t info;
    int32_t info_address;
    selector_lookup_t lookup = lookup_segment_selector(cs_selector, &info, &info_address);
    if (cpu->pagefault) {
        return;
    }
    if (lookup == SELECTOR_LOOKUP_IS_NULL) {
        /* upstream dbg_log!("#gp null cs") */
        trigger_gp(0);
        return;
    }
    if (lookup == SELECTOR_LOOKUP_OUTSIDE) {
        trigger_gp(selector & ~3);
        return;
    }

    if (seg_descriptor_is_system(info)) {
        uint8_t system_type = seg_descriptor_system_type(info);

        if (system_type == 0xC || system_type == 4) {
            /* call gate */

            bool is_16 = system_type == 4;

            if (seg_descriptor_dpl(info) < cpu->cpl || seg_descriptor_dpl(info) < seg_selector_rpl(cs_selector)) {
                trigger_gp(selector & ~3);
                return;
            }

            if (!seg_descriptor_is_present(info)) {
                trigger_np(selector & ~3);
                return;
            }

            /* upstream shadows cs_selector with the gate's TARGET selector here; the
             * outer (gate) selector is `selector` and is no longer needed in this arm. */
            int32_t target_cs_selector = (int32_t)(uint16_t)(info.raw >> 16);

            seg_descriptor_t cs_info;
            int32_t cs_info_address;
            lookup = lookup_segment_selector(seg_selector_of_u16((uint16_t)target_cs_selector), &cs_info,
                                             &cs_info_address);
            if (cpu->pagefault) {
                return;
            }
            if (lookup == SELECTOR_LOOKUP_IS_NULL) {
                trigger_gp(0);
                return;
            }
            if (lookup == SELECTOR_LOOKUP_OUTSIDE) {
                trigger_gp(target_cs_selector & ~3);
                return;
            }

            if (seg_descriptor_is_system(cs_info)) {
                trigger_gp(target_cs_selector & ~3);
                return;
            }
            if (!seg_descriptor_is_executable(cs_info)) {
                trigger_gp(target_cs_selector & ~3);
                return;
            }
            if (seg_descriptor_dpl(cs_info) > cpu->cpl) {
                trigger_gp(target_cs_selector & ~3);
                return;
            }
            if (!seg_descriptor_is_present(cs_info)) {
                trigger_np(target_cs_selector & ~3);
                return;
            }

            if (!seg_descriptor_is_dc(cs_info) && seg_descriptor_dpl(cs_info) < cpu->cpl) {
                /* more privilege call gate */
                int32_t new_ss, new_esp;
                if (!get_tss_ss_esp(seg_descriptor_dpl(cs_info), &new_ss, &new_esp)) {
                    return;
                }

                seg_selector_t ss_selector = seg_selector_of_u16((uint16_t)new_ss);
                seg_descriptor_t ss_info;
                int32_t ss_info_address;
                lookup = lookup_segment_selector(ss_selector, &ss_info, &ss_info_address);
                if (cpu->pagefault) {
                    return;
                }
                if (lookup != SELECTOR_LOOKUP_OK) {
                    panic_stop("far_jump: call gate's SS is null or outside the table limit (upstream panic, cpu.rs:1243/1246)");
                    return;
                }

                /* upstream dbg_asserts on the new stack's limit/system/writable bits,
                 * dropped (properties of the guest's bytes). */
                if (seg_selector_rpl(ss_selector) != seg_descriptor_dpl(cs_info)) {
                    /* xxx: 0 in v86 mode */
                    panic_stop("far_jump: #TS handler, SS rpl (upstream panic, cpu.rs:1261)");
                    return;
                }
                if (seg_descriptor_dpl(ss_info) != seg_descriptor_dpl(cs_info) || !seg_descriptor_is_writable(ss_info)) {
                    panic_stop("far_jump: #TS handler, SS dpl/writable (upstream panic, cpu.rs:1264)");
                    return;
                }
                if (!seg_descriptor_is_present(ss_info)) {
                    panic_stop("far_jump: #SS handler, SS not present (upstream panic, cpu.rs:1267)");
                    return;
                }

                int32_t parameter_count = (int32_t)((info.raw >> 32) & 0x1F);
                int32_t stack_space = is_16 ? 4 : 8;
                if (is_call) {
                    stack_space += is_16 ? 4 + 2 * parameter_count : 8 + 4 * parameter_count;
                }
                if (seg_descriptor_is_32(ss_info)) {
                    writable_or_pagefault_cpl(seg_descriptor_dpl(cs_info),
                                              seg_descriptor_base(ss_info) + new_esp - stack_space, stack_space);
                }
                else {
                    writable_or_pagefault_cpl(seg_descriptor_dpl(cs_info),
                                              seg_descriptor_base(ss_info) + ((new_esp - stack_space) & 0xFFFF),
                                              stack_space);
                }
                if (cpu->pagefault) return;

                int32_t old_esp = read_reg32(ESP);
                int32_t old_ss = (int32_t)cpu->sreg[SS];
                int32_t old_stack_pointer = get_stack_pointer(0);

                cpu->cpl = seg_descriptor_dpl(cs_info);
                cpl_changed();

                update_cs_size_long(seg_descriptor_is_32(cs_info), seg_descriptor_is_long(cs_info));

                /* XXX: Should be checked before side effects (upstream dbg_assert(false)) */
                if (!switch_seg(SS, new_ss)) {
                }
                set_stack_reg(new_esp);

                if (is_16) {
                    push16(old_ss);
                    DBG_ASSERT(!cpu->pagefault);
                    push16(old_esp);
                    DBG_ASSERT(!cpu->pagefault);
                }
                else {
                    push32(old_ss);
                    DBG_ASSERT(!cpu->pagefault);
                    push32(old_esp);
                    DBG_ASSERT(!cpu->pagefault);
                }

                if (is_call) {
                    if (is_16) {
                        for (int i = parameter_count - 1; i >= 0; i--) {
                            int32_t parameter = safe_read16(old_stack_pointer + 2 * i);
                            DBG_ASSERT(!cpu->pagefault); /* upstream .unwrap() */
                            push16(parameter);
                            DBG_ASSERT(!cpu->pagefault);
                        }

                        push16((int32_t)cpu->sreg[CS]);
                        DBG_ASSERT(!cpu->pagefault);
                        push16(get_real_eip());
                        DBG_ASSERT(!cpu->pagefault);
                    }
                    else {
                        for (int i = parameter_count - 1; i >= 0; i--) {
                            int32_t parameter = safe_read32s(old_stack_pointer + 4 * i);
                            DBG_ASSERT(!cpu->pagefault); /* upstream .unwrap() */
                            push32(parameter);
                            DBG_ASSERT(!cpu->pagefault);
                        }

                        push32((int32_t)cpu->sreg[CS]);
                        DBG_ASSERT(!cpu->pagefault);
                        push32(get_real_eip());
                        DBG_ASSERT(!cpu->pagefault);
                    }
                }
            }
            else {
                /* same privilege call gate */
                if (is_call) {
                    if (is_16) {
                        writable_or_pagefault(get_stack_pointer(-4), 4);
                        if (cpu->pagefault) return;

                        push16((int32_t)cpu->sreg[CS]);
                        DBG_ASSERT(!cpu->pagefault);
                        push16(get_real_eip());
                        DBG_ASSERT(!cpu->pagefault);
                    }
                    else {
                        writable_or_pagefault(get_stack_pointer(-8), 8);
                        if (cpu->pagefault) return;

                        push32((int32_t)cpu->sreg[CS]);
                        DBG_ASSERT(!cpu->pagefault);
                        push32(get_real_eip());
                        DBG_ASSERT(!cpu->pagefault);
                    }
                }

            }

            /* Note: eip from call is ignored */
            int32_t new_eip = (int32_t)(info.raw & 0xFFFF);
            if (!is_16) {
                new_eip |= (int32_t)((info.raw >> 32) & 0xFFFF0000);
            }


            update_cs_size_long(seg_descriptor_is_32(cs_info), seg_descriptor_is_long(cs_info));

            cpu->segment_is_null[CS] = false;
            cpu->segment_limits[CS] = seg_descriptor_effective_limit(cs_info);
            cpu->segment_offsets[CS] = seg_descriptor_base(cs_info);
            cpu->segment_access_bytes[CS] = seg_descriptor_access_byte(cs_info);
            cpu->sreg[CS] = (uint16_t)(((uint16_t)target_cs_selector & ~3) | cpu->cpl);

            set_instruction_pointer(get_seg_cs() + new_eip);

            update_state_flags();
        }
        else if (system_type == 1 || system_type == 9) {
            /* available TSS */
            if (seg_descriptor_dpl(info) < cpu->cpl || seg_descriptor_dpl(info) < seg_selector_rpl(cs_selector)) {
                trigger_gp(selector & ~3);
                return;
            }

            if (!seg_descriptor_is_present(info)) {
                trigger_np(selector & ~3);
                return;
            }

            do_task_switch(selector, false, 0,
                           is_call ? TASK_SWITCH_CALL_OR_INT : TASK_SWITCH_JUMP);
        }
        else if (system_type == 5) {
            /* task gate */
            if (seg_descriptor_dpl(info) < cpu->cpl || seg_descriptor_dpl(info) < seg_selector_rpl(cs_selector)) {
                trigger_gp(selector & ~3);
                return;
            }

            if (!seg_descriptor_is_present(info)) {
                trigger_np(selector & ~3);
                return;
            }

            int32_t tss_selector = (int32_t)((info.raw >> 16) & 0xFFFF);
            do_task_switch(tss_selector, false, 0,
                           is_call ? TASK_SWITCH_CALL_OR_INT : TASK_SWITCH_JUMP);
        }
        else {
            /* upstream dbg_assert!(false, "TODO: #gp invalid system type") -- DROPPED, and
             * nothing takes its place: the release reference falls out of the transfer
             * with no state change at all, and so does the port. */
        }
    }
    else {
        if (!seg_descriptor_is_executable(info)) {
            trigger_gp(selector & ~3);
            return;
        }

        if (seg_descriptor_is_dc(info)) {
            /* conforming code segment */
            if (seg_descriptor_dpl(info) > cpu->cpl) {
                trigger_gp(selector & ~3);
                return;
            }
        }
        else {
            /* non-conforming code segment */
            if (seg_selector_rpl(cs_selector) > cpu->cpl || seg_descriptor_dpl(info) != cpu->cpl) {
                trigger_gp(selector & ~3);
                return;
            }
        }

        if (!seg_descriptor_is_present(info)) {
            trigger_np(selector & ~3);
            return;
        }

        if (is_call) {
            if (is_osize_32) {
                writable_or_pagefault(get_stack_pointer(-8), 8);
                if (cpu->pagefault) return;

                push32((int32_t)cpu->sreg[CS]);
                DBG_ASSERT(!cpu->pagefault);
                push32(get_real_eip());
                DBG_ASSERT(!cpu->pagefault);
            }
            else {
                writable_or_pagefault(get_stack_pointer(-4), 4);
                if (cpu->pagefault) return;

                push16((int32_t)cpu->sreg[CS]);
                DBG_ASSERT(!cpu->pagefault);
                push16(get_real_eip());
                DBG_ASSERT(!cpu->pagefault);
            }
        }


        update_cs_size_long(seg_descriptor_is_32(info), seg_descriptor_is_long(info));

        cpu->segment_is_null[CS] = false;
        cpu->segment_limits[CS] = seg_descriptor_effective_limit(info);
        cpu->segment_access_bytes[CS] = seg_descriptor_access_byte(info);

        cpu->segment_offsets[CS] = seg_descriptor_base(info);
        cpu->sreg[CS] = (uint16_t)(((uint16_t)selector & ~3) | cpu->cpl);

        set_instruction_pointer(get_seg_cs() + eip);

        update_state_flags();
    }
}

/* ------------------------------------------------------------------ *
 * cpu.rs:1507-1655 -- far_return
 * ------------------------------------------------------------------ */
void far_return(int64_t eip, int32_t selector, int32_t stack_adjust, bool is_osize_32,
                bool is_64)
{

    if (!cpu->protected_mode) {
    }

    if (!cpu->protected_mode || vm86_mode()) {
        switch_cs_real_mode(selector);
        set_instruction_pointer(get_seg_cs() + eip);
        adjust_stack_reg(2 * (is_osize_32 ? 4 : 2) + stack_adjust);
        update_state_flags();
        return;
    }

    seg_selector_t cs_selector = seg_selector_of_u16((uint16_t)selector);
    seg_descriptor_t info;
    int32_t info_address;
    selector_lookup_t lookup = lookup_segment_selector(cs_selector, &info, &info_address);
    if (cpu->pagefault) {
        return;
    }
    if (lookup == SELECTOR_LOOKUP_IS_NULL) {
        /* upstream dbg_log!("far return: #gp null cs") */
        trigger_gp(0);
        return;
    }
    if (lookup == SELECTOR_LOOKUP_OUTSIDE) {
        trigger_gp(selector & ~3);
        return;
    }

    if (seg_descriptor_is_system(info)) {
        trigger_gp(selector & ~3);
        return;
    }

    if (!seg_descriptor_is_executable(info)) {
        trigger_gp(selector & ~3);
        return;
    }

    if (seg_selector_rpl(cs_selector) < cpu->cpl) {
        trigger_gp(selector & ~3);
        return;
    }

    if (seg_descriptor_is_dc(info) && seg_descriptor_dpl(info) > seg_selector_rpl(cs_selector)) {
        trigger_gp(selector & ~3);
        return;
    }

    if (!seg_descriptor_is_dc(info) && seg_descriptor_dpl(info) != seg_selector_rpl(cs_selector)) {
        trigger_gp(selector & ~3);
        return;
    }

    if (!seg_descriptor_is_present(info)) {
        trigger_np(selector & ~3);
        return;
    }

    if (seg_selector_rpl(cs_selector) > cpu->cpl) {
        int32_t temp_esp;
        int32_t temp_ss;
        if (is_64) {
            /* THE 64-BIT FAR RETURN'S PRIVILEGE CHANGE (SDM, RET): the frame is
             * RIP(8), CS(8), then RSP(8) and SS(8) -- SS:RSP come AFTER RIP/CS, the same
             * order as IRET, and every field is eight bytes.  MEASURED as the amd64
             * kernel's last blocker (results/x86-64-farret.md). */
            uint64_t new_rsp = safe_read64_addr(get_stack_pointer(stack_adjust + 16));
            DBG_ASSERT(!cpu->pagefault);
            temp_ss = safe_read16(get_stack_pointer(stack_adjust + 24));
            DBG_ASSERT(!cpu->pagefault);
            temp_esp = (int32_t)(uint32_t)new_rsp;
            /* a 64-bit RSP above 4 GiB cannot be carried by the port's 32-bit stack register
             * path: stop by name rather than truncate */
            if (new_rsp > 0xFFFFFFFFull) {
                unimplemented_instruction(
                    "long mode: a far return whose new RSP is at or above 4 GiB");
                return;
            }
        }
        else if (is_osize_32) {
            temp_esp = safe_read32s(get_stack_pointer(stack_adjust + 8));
            DBG_ASSERT(!cpu->pagefault); /* upstream .unwrap() */
            temp_ss = safe_read16(get_stack_pointer(stack_adjust + 12));
            DBG_ASSERT(!cpu->pagefault);
        }
        else {
            temp_esp = safe_read16(get_stack_pointer(stack_adjust + 4));
            DBG_ASSERT(!cpu->pagefault);
            temp_ss = safe_read16(get_stack_pointer(stack_adjust + 6));
            DBG_ASSERT(!cpu->pagefault);
        }

        cpu->cpl = seg_selector_rpl(cs_selector);
        cpl_changed();

        /* XXX: This failure should be checked before side effects (upstream
         * dbg_assert(false), dropped) */
        if (!switch_seg(SS, temp_ss)) {
        }
        set_stack_reg(temp_esp + stack_adjust);

        /* upstream: `for reg in [ES, DS, FS, GS]` -- the FOUR DATA SEGMENTS.  CS and SS
         * are NOT in the list; they were loaded by this instruction's own selector handling
         * above.
         *
         * CORRECTED IN M2-3 (HANDOFF=v86c): this loop was transcribed as
         * `for (reg = ES; reg <= GS; reg++)`, which in v86's index order (ES=0, CS=1, SS=2,
         * DS=3, FS=4, GS=5) also visits CS and SS, so a data segment whose DPL was below the
         * new CPL ZEROED sreg[CS]/sreg[SS] and set segment_is_null on them.  MEASURED on a
         * real Linux guest: the first iret32 back to userspace did exactly that, and the
         * port then aborted at src/cpu/modrm.c:89's DBG_ASSERT(segment != CS && segment !=
         * SS) on the guest's next stack access, where the reference (whose dbg_assert is
         * compiled out and whose list is the four data segments) runs on.  The regression
         * probe is m1c_selftest.c's "the privilege-changing iret32 nulls the four DATA
         * segments and leaves CS and SS alone". */
        static const int iret_data_segments[4] = { ES, DS, FS, GS };
        for (unsigned iret_reg_i = 0; iret_reg_i < 4; iret_reg_i++) {
            int reg = iret_data_segments[iret_reg_i];
            uint8_t access = cpu->segment_access_bytes[reg];
            uint8_t dpl = (uint8_t)(access >> 5 & 3);
            bool executable = (access & 8) == 8;
            bool conforming = (access & 4) == 4;
            if (dpl < cpu->cpl && !(executable && conforming)) {
                cpu->segment_is_null[reg] = true;
                cpu->sreg[reg] = 0;
            }
        }
    }
    else {
        if (is_64) {
            /* 2 x 8 bytes for RIP and CS, plus the caller's stack_adjust (which for
             * instr64_CB is 0: it reads the 16-byte frame itself and passes 0) */
            adjust_stack_reg(2 * 8 + stack_adjust);
        }
        else if (is_osize_32) {
            adjust_stack_reg(2 * 4 + stack_adjust);
        }
        else {
            adjust_stack_reg(2 * 2 + stack_adjust);
        }
    }

    /* upstream dbg_assert(*cpl == info.dpl) dropped */

    update_cs_size_long(seg_descriptor_is_32(info), seg_descriptor_is_long(info));

    cpu->segment_is_null[CS] = false;
    cpu->segment_limits[CS] = seg_descriptor_effective_limit(info);
    cpu->segment_access_bytes[CS] = seg_descriptor_access_byte(info);

    cpu->segment_offsets[CS] = seg_descriptor_base(info);
    cpu->sreg[CS] = (uint16_t)selector;

    set_instruction_pointer(get_seg_cs() + eip);

    update_state_flags();
}

/* ------------------------------------------------------------------ *
 * cpu.rs:1664-1899 -- do_task_switch
 *
 * Upstream's `error_code: Option<i32>` is the (has_error_code, error_code) pair of
 * paging.h's call_interrupt_vector.
 * ------------------------------------------------------------------ */
void do_task_switch(int32_t selector_raw, bool has_error_code, int32_t error_code,
                    task_switch_source_t source)
{

    seg_selector_t selector = seg_selector_of_u16((uint16_t)selector_raw);
    seg_descriptor_t descriptor;
    int32_t descriptor_address;
    selector_lookup_t lookup = lookup_segment_selector(selector, &descriptor, &descriptor_address);
    if (cpu->pagefault) {
        panic_stop("do_task_switch: TODO: handle pagefault (upstream .expect() panic, cpu.rs:1671)");
        return;
    }
    if (lookup != SELECTOR_LOOKUP_OK) {
        panic_stop("do_task_switch: #GP handler, selector null or outside the table limit (upstream panic, cpu.rs:1674)");
        return;
    }


    bool tss_is_16 = seg_descriptor_system_type(descriptor) <= 3;
    bool tss_is_busy = (seg_descriptor_system_type(descriptor) & 2) == 2;

    if (source == TASK_SWITCH_IRET) {
        if (!tss_is_busy) {
            /* a task return must target a busy task */
            panic_stop("do_task_switch: #TS handler, iret to a non-busy task (upstream panic, cpu.rs:1686)");
            return;
        }
    }
    else if (tss_is_busy) {
        /* jump, call or int to a busy task */
        panic_stop("do_task_switch: #GP handler, jump/call/int to a busy task (upstream panic, cpu.rs:1691)");
        return;
    }

    if (!seg_descriptor_is_present(descriptor)) {
        panic_stop("do_task_switch: #NP handler (upstream panic, cpu.rs:1695)");
        return;
    }

    if (seg_descriptor_effective_limit(descriptor) < 103) {
        panic_stop("do_task_switch: #NP handler, TSS limit < 103 (upstream panic, cpu.rs:1699)");
        return;
    }

    /* PAE-1 atomicity (review F3/F1): validate the new task's PDPT BEFORE ANYTHING is
     * written.  A task switch to a TSS whose CR3 points at a bad PDPT must deliver #GP
     * with the old task FULLY intact -- hardware faults a task switch before any state
     * change, and a guest #GP handler that fixes the PDPT and retries must find the new
     * TSS descriptor still NOT busy (the earlier hoist left the busy-bit write and the
     * old-TSS save ahead of the validation, so a retry after a fix hit
     * panic_stop("...busy task")).  This is safe at the very top because validate_pdpte
     * reads the PDPT PHYSICALLY (read64s, memory.c:467 -- raw mem8/vga_mem8, no
     * translation, no pagefault, no reg_pdpte/TLB/CR/TR writes) and new_cr3 is a
     * physical address, so the validation does not depend on the old task's page tables
     * or on any register state. */
    int32_t new_tsr_offset = seg_descriptor_base(descriptor);
    int32_t new_cr3 = safe_read32s(new_tsr_offset + TSR_CR3);
    DBG_ASSERT(!cpu->pagefault);
    if ((cpu->cr[4] & CR4_PAE) != 0 && (cpu->cr[0] & CR0_PG) != 0 &&
        !validate_pdpte(new_cr3)) {
        return; /* #GP delivered before any write: descriptor, backlinks, registers untouched */
    }

    /* upstream's `let _tsr_size = *segment_limits.offset(TR)` is unused; dropped */
    uint64_t tsr_offset = cpu->segment_offsets[TR]; /* S3: widened with the array */

    int32_t old_eflags = get_eflags();

    if (tss_is_busy) {
        old_eflags &= ~FLAG_NT;
    }

    writable_or_pagefault(tsr_offset, 0x66);
    DBG_ASSERT(!cpu->pagefault); /* upstream .unwrap() */

    /* safe_write32(tsr_offset + TSR_CR3, *cr.offset(3)); -- commented out upstream */

    /* TODO: Write 16 bit values if old tss is 16 bit */
    safe_write32(tsr_offset + TSR_EIP, get_real_eip());
    DBG_ASSERT(!cpu->pagefault);
    safe_write32(tsr_offset + TSR_EFLAGS, old_eflags);
    DBG_ASSERT(!cpu->pagefault);

    safe_write32(tsr_offset + TSR_EAX, read_reg32(EAX));
    DBG_ASSERT(!cpu->pagefault);
    safe_write32(tsr_offset + TSR_ECX, read_reg32(ECX));
    DBG_ASSERT(!cpu->pagefault);
    safe_write32(tsr_offset + TSR_EDX, read_reg32(EDX));
    DBG_ASSERT(!cpu->pagefault);
    safe_write32(tsr_offset + TSR_EBX, read_reg32(EBX));
    DBG_ASSERT(!cpu->pagefault);

    safe_write32(tsr_offset + TSR_ESP, read_reg32(ESP));
    DBG_ASSERT(!cpu->pagefault);
    safe_write32(tsr_offset + TSR_EBP, read_reg32(EBP));
    DBG_ASSERT(!cpu->pagefault);
    safe_write32(tsr_offset + TSR_ESI, read_reg32(ESI));
    DBG_ASSERT(!cpu->pagefault);
    safe_write32(tsr_offset + TSR_EDI, read_reg32(EDI));
    DBG_ASSERT(!cpu->pagefault);

    safe_write32(tsr_offset + TSR_ES, (int32_t)cpu->sreg[ES]);
    DBG_ASSERT(!cpu->pagefault);
    safe_write32(tsr_offset + TSR_CS, (int32_t)cpu->sreg[CS]);
    DBG_ASSERT(!cpu->pagefault);
    safe_write32(tsr_offset + TSR_SS, (int32_t)cpu->sreg[SS]);
    DBG_ASSERT(!cpu->pagefault);
    safe_write32(tsr_offset + TSR_DS, (int32_t)cpu->sreg[DS]);
    DBG_ASSERT(!cpu->pagefault);
    safe_write32(tsr_offset + TSR_FS, (int32_t)cpu->sreg[FS]);
    DBG_ASSERT(!cpu->pagefault);
    safe_write32(tsr_offset + TSR_GS, (int32_t)cpu->sreg[GS]);
    DBG_ASSERT(!cpu->pagefault);

    if (source == TASK_SWITCH_JUMP || source == TASK_SWITCH_IRET) {
        /* mark the old task as not busy */
        seg_selector_t tr_selector = seg_selector_of_u16(cpu->sreg[TR]);
        seg_descriptor_t tr_descriptor;
        int32_t tr_descriptor_address;
        lookup = lookup_segment_selector(tr_selector, &tr_descriptor, &tr_descriptor_address);
        if (cpu->pagefault) {
            panic_stop("do_task_switch: TODO: handle pagefault on the TR lookup (upstream .expect() panic, cpu.rs:1742)");
            return;
        }
        if (lookup != SELECTOR_LOOKUP_OK) {
            panic_stop("do_task_switch: #TS handler, TR selector null or outside the table limit (upstream panic, cpu.rs:1745)");
            return;
        }
        safe_write64(tr_descriptor_address, seg_descriptor_clear_busy(tr_descriptor).raw);
        DBG_ASSERT(!cpu->pagefault);
    }

    if (source != TASK_SWITCH_IRET) {
        /* jump, call and int mark the new task as busy (iret would not) */
        safe_write64(descriptor_address, seg_descriptor_set_busy(descriptor).raw);
        DBG_ASSERT(!cpu->pagefault);
    }

    /* let new_tsr_size = descriptor.effective_limit; -- commented out upstream */

    if (source == TASK_SWITCH_CALL_OR_INT) {
        safe_write16(new_tsr_offset + TSR_BACKLINK, (int32_t)cpu->sreg[TR]);
        DBG_ASSERT(!cpu->pagefault);
    }

    /* new_cr3 and the PDPTE validation were hoisted to the top (above the old-TSS save
     * and the busy/backlink writes), so by this point a bad PDPT has already #GP'd and
     * returned with the descriptor, the backlinks and every register untouched. */
    cpu->flags &= ~FLAG_VM;

    int32_t new_eip = safe_read32s(new_tsr_offset + TSR_EIP);
    DBG_ASSERT(!cpu->pagefault);
    int32_t new_cs = safe_read16(new_tsr_offset + TSR_CS);
    DBG_ASSERT(!cpu->pagefault);
    int32_t new_ldt = safe_read16(new_tsr_offset + TSR_LDT);
    DBG_ASSERT(!cpu->pagefault);

    int32_t new_eflags = safe_read32s(new_tsr_offset + TSR_EFLAGS);
    DBG_ASSERT(!cpu->pagefault);
    if (source == TASK_SWITCH_CALL_OR_INT) {
        new_eflags |= FLAG_NT;
    }

    load_ldt(new_ldt);
    DBG_ASSERT(!cpu->pagefault); /* upstream .unwrap() */

    int32_t new_cpl;
    if ((new_eflags & FLAG_VM) != 0) {
        cpu->segment_is_null[CS] = false;
        cpu->segment_offsets[CS] = (uint32_t)new_cs << 4;
        cpu->sreg[CS] = (uint16_t)new_cs;
        update_cs_size_long(false, false); /* S5a: a VM86 task switch's CS is not 64-bit */
        new_cpl = 3;
    }
    else {
        seg_selector_t new_cs_selector = seg_selector_of_u16((uint16_t)new_cs);
        seg_descriptor_t new_cs_descriptor;
        int32_t new_cs_descriptor_address;
        lookup = lookup_segment_selector(new_cs_selector, &new_cs_descriptor, &new_cs_descriptor_address);
        if (cpu->pagefault) {
            panic_stop("do_task_switch: TODO: handle pagefault on the new CS (upstream .expect() panic, cpu.rs:1795)");
            return;
        }
        if (lookup != SELECTOR_LOOKUP_OK) {
            panic_stop("do_task_switch: #TS handler, new CS null or invalid (upstream panic, cpu.rs:1799/1803)");
            return;
        }

        if (seg_descriptor_is_system(new_cs_descriptor)) {
            panic_stop("do_task_switch: #TS handler, new CS is a system entry (upstream panic, cpu.rs:1808)");
            return;
        }

        if (!seg_descriptor_is_executable(new_cs_descriptor)) {
            panic_stop("do_task_switch: #TS handler, new CS is not executable (upstream panic, cpu.rs:1812)");
            return;
        }

        if (seg_descriptor_is_dc(new_cs_descriptor) &&
            seg_descriptor_dpl(new_cs_descriptor) > seg_selector_rpl(new_cs_selector)) {
            panic_stop("do_task_switch: #TS handler, conforming CS with dpl > rpl (upstream panic, cpu.rs:1817)");
            return;
        }

        if (!seg_descriptor_is_dc(new_cs_descriptor) &&
            seg_descriptor_dpl(new_cs_descriptor) != seg_selector_rpl(new_cs_selector)) {
            panic_stop("do_task_switch: #TS handler, non-conforming CS with dpl != rpl (upstream panic, cpu.rs:1822)");
            return;
        }

        if (!seg_descriptor_is_present(new_cs_descriptor)) {
            panic_stop("do_task_switch: #TS handler, new CS not present (upstream panic, cpu.rs:1827)");
            return;
        }

        cpu->segment_is_null[CS] = false;
        cpu->segment_limits[CS] = seg_descriptor_effective_limit(new_cs_descriptor);
        cpu->segment_offsets[CS] = seg_descriptor_base(new_cs_descriptor);
        cpu->segment_access_bytes[CS] = seg_descriptor_access_byte(new_cs_descriptor);
        cpu->sreg[CS] = (uint16_t)new_cs;

        update_cs_size_long(seg_descriptor_is_32(new_cs_descriptor), seg_descriptor_is_long(new_cs_descriptor));

        new_cpl = seg_selector_rpl(new_cs_selector);
    }

    cpu->cpl = 0; /* run update_eflags at cpl 0 */
    update_eflags(new_eflags);

    if ((new_eflags & FLAG_VM) != 0) {
        cpu->flags |= FLAG_VM;
    }
    cpu->cpl = (uint8_t)new_cpl;
    cpl_changed();

    if (source == TASK_SWITCH_CALL_OR_INT) {
        cpu->flags |= FLAG_NT;
    }

    write_reg32(EAX, safe_read32s(new_tsr_offset + TSR_EAX));
    DBG_ASSERT(!cpu->pagefault);
    write_reg32(ECX, safe_read32s(new_tsr_offset + TSR_ECX));
    DBG_ASSERT(!cpu->pagefault);
    write_reg32(EDX, safe_read32s(new_tsr_offset + TSR_EDX));
    DBG_ASSERT(!cpu->pagefault);
    write_reg32(EBX, safe_read32s(new_tsr_offset + TSR_EBX));
    DBG_ASSERT(!cpu->pagefault);

    write_reg32(ESP, safe_read32s(new_tsr_offset + TSR_ESP));
    DBG_ASSERT(!cpu->pagefault);
    write_reg32(EBP, safe_read32s(new_tsr_offset + TSR_EBP));
    DBG_ASSERT(!cpu->pagefault);
    write_reg32(ESI, safe_read32s(new_tsr_offset + TSR_ESI));
    DBG_ASSERT(!cpu->pagefault);
    write_reg32(EDI, safe_read32s(new_tsr_offset + TSR_EDI));
    DBG_ASSERT(!cpu->pagefault);

    {
        int32_t new_es = safe_read16(new_tsr_offset + TSR_ES);
        DBG_ASSERT(!cpu->pagefault);
        int32_t new_ss = safe_read16(new_tsr_offset + TSR_SS);
        DBG_ASSERT(!cpu->pagefault);
        int32_t new_ds = safe_read16(new_tsr_offset + TSR_DS);
        DBG_ASSERT(!cpu->pagefault);
        int32_t new_fs = safe_read16(new_tsr_offset + TSR_FS);
        DBG_ASSERT(!cpu->pagefault);
        int32_t new_gs = safe_read16(new_tsr_offset + TSR_GS);
        DBG_ASSERT(!cpu->pagefault);

        if (!switch_seg(ES, new_es) || !switch_seg(SS, new_ss) || !switch_seg(DS, new_ds) ||
            !switch_seg(FS, new_fs) || !switch_seg(GS, new_gs)) {
            /* XXX: Should be checked before side effects (upstream dbg_assert(false)) */
        }
    }

    set_instruction_pointer(
        get_seg_cs() + ((new_eflags & FLAG_VM) != 0 ? (new_eip & 0xFFFF) : new_eip));

    cpu->segment_offsets[TR] = seg_descriptor_base(descriptor);
    cpu->segment_limits[TR] = seg_descriptor_effective_limit(descriptor);
    cpu->sreg[TR] = selector.raw;

    /* The PDPTE was validated up at the top (review F3's hoist, above the segment
     * switches): by this point a bad PDPT has already #GP'd and returned, so this
     * set_cr3's load_pdpte re-validates the PDPT and commits CR3 + the TLB clear as the
     * last register commit of the switch.  The success path therefore reads the PDPT
     * TWICE (validate at the top, then load_pdpte here); for a PDPT in guest RAM the two
     * reads are byte-identical (single-threaded, no write between), and the only
     * divergence is the pre-existing self-aliasing shape -- a guest that overlaps its old
     * TSS/GDT onto the new PDPT page could be overwritten between the two reads and fault
     * the second validation after the commits, exactly as upstream's single late read
     * would.  Upstream reads once at this same late point, so the port is no worse; the
     * hoist strictly narrows the window (the first read, at the top, catches every fault
     * reachable from a non-aliased PDPT). */
    set_cr3(new_cr3);

    cpu->cr[0] |= CR0_TS;

    if (has_error_code) {
        if (tss_is_16) {
            push16(error_code & 0xFFFF);
            DBG_ASSERT(!cpu->pagefault);
        }
        else {
            push32(error_code);
            DBG_ASSERT(!cpu->pagefault);
        }
    }

    update_state_flags();
}

/* ------------------------------------------------------------------ *
 * cpu.rs:2803-2855 -- load_tr
 * ------------------------------------------------------------------ */
void load_tr(int32_t selector_raw)
{
    seg_selector_t selector = seg_selector_of_u16((uint16_t)selector_raw);

    seg_descriptor_t descriptor;
    int32_t descriptor_address;
    selector_lookup_t lookup = lookup_segment_selector(selector, &descriptor, &descriptor_address);
    if (cpu->pagefault) {
        return;
    }
    if (lookup == SELECTOR_LOOKUP_IS_NULL) {
        panic_stop("load_tr: null TR (upstream panic \"TODO: null TR\", cpu.rs:2811)");
        return;
    }
    if (lookup == SELECTOR_LOOKUP_OUTSIDE) {
        panic_stop("load_tr: TR selector outside the table limit (upstream panic, cpu.rs:2814)");
        return;
    }

    if (!seg_descriptor_is_system(descriptor)) {
        panic_stop("load_tr: not a system entry (upstream panic \"#GP | ltr\", cpu.rs:2827)");
        return;
    }

    if (seg_descriptor_system_type(descriptor) != 9 && seg_descriptor_system_type(descriptor) != 1) {
        /* 0xB: busy 386 TSS (GP); 0x9: 386 TSS; 0x3: busy 286 TSS (GP); 0x1: 286 TSS (??) */
        panic_stop("load_tr: invalid type (upstream panic \"#GP | ltr\", cpu.rs:2835)");
        return;
    }

    if (!seg_descriptor_is_present(descriptor)) {
        panic_stop("load_tr: present bit not set (upstream panic \"#NT | present bit not set (ltr)\", cpu.rs:2842)");
        return;
    }

    cpu->tss_size_32 = seg_descriptor_system_type(descriptor) == 9;
    cpu->segment_limits[TR] = seg_descriptor_effective_limit(descriptor);
    cpu->segment_offsets[TR] = seg_descriptor_base(descriptor);
    cpu->sreg[TR] = selector.raw;

    /* mark task as busy */
    {
        int32_t address = (int32_t)translate_address_system_write(descriptor_address + 5);
        DBG_ASSERT(!cpu->pagefault); /* upstream .unwrap() */
        write8((uint32_t)address, (int32_t)seg_descriptor_access_byte(seg_descriptor_set_busy(descriptor)));
    }
}

/* ------------------------------------------------------------------ *
 * cpu.rs:2857-2906 -- load_ldt
 *
 * Upstream returns OrPageFault<()>; the port's fault channel is cpu->pagefault, so the
 * caller checks it (instructions_0f.c's lldt passes it through return_on_pagefault!).
 * ------------------------------------------------------------------ */
void load_ldt(int32_t selector_raw)
{
    seg_selector_t selector = seg_selector_of_u16((uint16_t)selector_raw);

    if (seg_selector_is_null(selector)) {
        /* upstream dbg_log!("lldt: null loaded") */
        cpu->segment_limits[LDTR] = 0;
        cpu->segment_offsets[LDTR] = 0;
        cpu->sreg[LDTR] = selector.raw;
        return;
    }


    seg_descriptor_t descriptor;
    int32_t descriptor_address;
    selector_lookup_t lookup = lookup_segment_selector(selector, &descriptor, &descriptor_address);
    if (cpu->pagefault) {
        return;
    }
    if (lookup == SELECTOR_LOOKUP_IS_NULL) {
        panic_stop("load_ldt: null selector (upstream panic \"TODO: null TR\", cpu.rs:2873)");
        return;
    }
    if (lookup == SELECTOR_LOOKUP_OUTSIDE) {
        panic_stop("load_ldt: selector outside the table limit (upstream panic, cpu.rs:2876)");
        return;
    }

    if (!seg_descriptor_is_present(descriptor)) {
        panic_stop("load_ldt: present bit not set (upstream panic \"#NT | present bit not set (lldt)\", cpu.rs:2881)");
        return;
    }

    if (!seg_descriptor_is_system(descriptor)) {
        panic_stop("load_ldt: not a system entry (upstream panic \"#GP | lldt\", cpu.rs:2885)");
        return;
    }

    if (seg_descriptor_system_type(descriptor) != 2) {
        panic_stop("load_ldt: invalid type (upstream panic \"#GP | lldt: invalid type\", cpu.rs:2890)");
        return;
    }

    cpu->segment_limits[LDTR] = seg_descriptor_effective_limit(descriptor);
    cpu->segment_offsets[LDTR] = seg_descriptor_base(descriptor);
    cpu->sreg[LDTR] = selector.raw;
}

/* cpu.rs:3000-3006 (cpl_changed / update_cs_size) are `static inline` in exceptions.h --
 * see the reason at their declaration. */

/* ------------------------------------------------------------------ *
 * cpu.rs:3009-3052 -- test_privileges_for_io
 *
 * Declared in string.h (in/out are its first callers).  The slow path reads the TSS's
 * I/O-permission bitmap; a page fault while reading it returns false WITHOUT the #GP --
 * upstream's `return_on_pagefault!(expr, false)` returns the false, and the fault itself
 * was already delivered by trigger_pagefault.
 * ------------------------------------------------------------------ */
bool test_privileges_for_io(int32_t port, int32_t size)
{
    if (cpu->protected_mode && ((int32_t)cpu->cpl > getiopl() || (cpu->flags & FLAG_VM) != 0)) {
        if (!cpu->tss_size_32) {
            trigger_gp(0);
            return false;
        }

        uint32_t tsr_size = cpu->segment_limits[TR];
        uint64_t tsr_offset = cpu->segment_offsets[TR]; /* S3: widened with the array */

        if (tsr_size >= 0x67) {

            int32_t iomap_base_address =
                (int32_t)translate_address_system_read(tsr_offset + 0x64 + 2);
            if (cpu->pagefault) {
                return false;
            }
            int32_t iomap_base = read16((uint32_t)iomap_base_address);
            int32_t high_port = port + size - 1;

            if (tsr_size >= (uint32_t)(iomap_base + (high_port >> 3))) {
                int32_t mask = ((1 << size) - 1) << (port & 7);
                int32_t address =
                    (int32_t)translate_address_system_read(tsr_offset + iomap_base + (port >> 3));
                if (cpu->pagefault) {
                    return false;
                }
                int32_t port_info = (mask & 0xFF00) != 0 ? read16((uint32_t)address) : read8((uint32_t)address);


                if ((port_info & mask) == 0) {
                    return true;
                }
            }
        }

        trigger_gp(0);
        return false;
    }

    return true;
}
