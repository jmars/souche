#!/usr/bin/env python3
"""results/perf-fix-2-verdicts.py -- PERF-FIX-2's per-site VERDICTS for the sweep.

The sweep's inventory is results/perf-fix-2-sweep.py's (every DBG_ASSERT in src/, with
its upstream counterpart).  THIS file is the judgement half: one verdict per group of
identical conditions, keyed by the group's normalised key, written out as
results/perf-fix-2-sweep-verdicts.tsv for the sweep to join in.

THE RULE (why a site is KEEP or DROP), applied to the CLASS as PERF-FIX defined it -- an
upstream `dbg_assert!`/`debug_assert!` (dbg.rs:63, COMPILED OUT of the pinned release
reference) that the port emitted as a live DBG_ASSERT:

  DROP  the class, AND a guest can legitimately make the condition false in a
        non-buggy run, AND the release reference then proceeds with the same value and
        the port does too after the drop (the value is used as an address or an
        arithmetic operand, not as an index into a host array).  This is PERF-FIX's 14
        sites plus the three PERF-FIX-2 found and closed.

  KEEP  otherwise, with one of these reasons (the `code` column):

    (a) PORT-INVARIANT   no upstream dbg_assert! at this site: the condition is on the
                         port's own state or on the device/registration API, and guest
                         input cannot falsify it.  Falsifying it means a PORT bug, and
                         stopping there is the point (MEASURED once: modrm.c's default
                         arm caught M2's own wrong addressing call, results/perf-4).
    (b) HOST-BOUND       the checked value is used as a raw `mem8` address or as an index
                         into a host array.  The reference's equivalent access is
                         bounds-checked by the wasm VM IN RELEASE (it traps -- MEASURED),
                         but the port's flat pointer is not, so dropping the check gives
                         a HOST out-of-bounds read (MEASURED: rc=139 SIGSEGV with both
                         cpu.c guards deleted, results/perf-fix-2-cpu100.txt).
    (c) UNREACHABLE      the class's condition, but the operands are normalised/masked
                         before the call, or the arm is a switch default over a bounded
                         field, so a non-buggy run cannot falsify it -- sometimes with
                         the measurement that shows it (F2/F3: `F2 F2 90` runs green,
                         because run_prefix_instruction clears the accumulator).
    (d) DELIBERATE       the port's documented policy stops where the reference's release
                         build silently does nothing (D2: an opcode with no
                         implementation).  A NAMED divergence, not this unit's to change.
    (e) UNVERIFIED       reachability not established by this unit; flagged for the
                         reviewer rather than asserted either way.

Every row names the sites it covers.  A `why` that is a reading of upstream's code says
so; a `why` that is a measurement names the artifact.
"""

import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

spec = importlib.util.spec_from_file_location("sw", os.path.join(HERE, "perf-fix-2-sweep.py"))
sw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sw)

# keyed by the NORMALISED condition (sw.normalise of the C text), so the join in the
# sweep is stable under re-ordering; the comment column in the generated TSV carries the
# readable form.
#
# (verdict, reason-code, why)
V = {}

def put(cond, verdict, code, why):
    V[sw.normalise(cond)] = (verdict, code, why)


# ---------------------------------------------------------------- the class proper
put("!cpu->pagefault", "KEEP", "(a)",
    "the port's fault-flag discipline; upstream expresses the same thing with `?`/"
    "return_on_pagefault!, which is RELEASE-LIVE.  Falsifying it is a port bug, and "
    "aborting is what keeps that bug loud: R5F MEASURED that without it the port "
    "silently executed host garbage (results/r5f-fixes.md ss3).")
put("count >= 0 && count < 32", "KEEP", "(c)",
    "every caller masks: `imm & 31` (instructions.c:1414-1459) or `read_reg8(CL) & 31` "
    "(:1678).  upstream arith.rs:442+ has the same dbg_assert! and the same callers.")
put("false", "KEEP", "(a)/(c)/(d)",
    "23 sites, three families, each named in the table's site list: (c) switch defaults "
    "over a bounded field (fpu.c:263/272 over a 2-bit RC/PC field; instructions.c:2487/"
    "2502 over r = modrm>>3&7) and the modrm/decoder defaults whose match domain is "
    "exhaustive (modrm.c:328/461 = upstream modrm.rs:31/129, where upstream's arm is "
    "`unreachable_unchecked()` -- UB in RELEASE, so the port's check is strictly safer); "
    "(d) 0x62/0xF1 and the emitter's own default arms, the port's D2 policy; (a) the "
    "device-layer states (dma.c:48, pci.c:108, virtio.c:985, string.c:341, "
    "uop_cache.c's decoder arms).")
put("i >= 0 && i < 3", "KEEP", "(a)",
    "pit.c's channel index, produced by the port's own 3-channel registration loop.")
put("((uint32_t)ret & 0xFFFFFF00u) == 0", "KEEP", "(c)",
    "arith.c's saturate_* helpers; upstream arith.rs:1137+ has the identical "
    "dbg_assert! and the same callers (the SSE pack instructions' own clamp).")
put("cpu->fpu_stack_ptr < 8", "KEEP", "(b)",
    "used as an index into the 8-entry x87 register file; upstream fpu.rs's dbg_assert!.")
put("cpu->prefixes == 0", "KEEP", "(a)",
    "the dispatch's own accumulator invariant, kept at the same places upstream does "
    "(cpu.rs's interpreted loop); a leak here is a port bug the assert exists to catch.")
put("queue->avail_addr != 0", "KEEP", "(a)",
    "virtqueue state installed by the device layer's own queue setup.")
put("space != NULL", "KEEP", "(a)", "pci_register_device's API precondition (host caller).")
put("!in_mapped_range(offset)", "KEEP", "(b)",
    "the blob path copies through raw `mem8 + offset` for `length` bytes; this is the "
    "host bound on that copy (memory.c:72-73, and the device-side twin).")
put("!in_mapped_range(offset + length - 1)", "KEEP", "(b)", "as above: the range's last byte.")
put("!in_mapped_range(phys_addr)", "KEEP", "(b)",
    "THE 15TH SITE (cpu.c:100, get_phys_eip) and its twin (cpu.c:113, interpreted_run). "
    "KEPT, and this is the unit's central measurement: dropping cpu.c:100 does NOT match "
    "the release reference, it moves the stop one frame later (cpu.c:113) and deleting "
    "BOTH gives rc=139 SIGSEGV on the corpus's own jump image, while the reference on "
    "that image runs 1e6 instructions of wasm-fabricated zeros past guest RAM and never "
    "halts (results/perf-fix-2-cpu100.{sh,txt}).  The site is the port's substitute for "
    "a mechanism the reference HAS in release (the wasm memory access is bounds-checked, "
    "and it traps -- MEASURED, 'RuntimeError: memory access out of bounds'), not a "
    "release-compiled-out debug invariant whose condition the reference ignores.")
put("cond", "KEEP", "(a)", "the DBG_ASSERT definition itself (cpu_state.h:38/41) -- not a site.")
put("connector != NULL", "KEEP", "(a)", "bus_register/bus_unregister's API precondition.")
put("device != NULL", "KEEP", "(a)", "io_register_read/write's API precondition.")
put("(high & 0xFFF) == 0", "KEEP", "(c)",
    "paging.c's virt_boundary_read16/write16 contract (the high page's offset is 0 by "
    "construction); upstream cpu.rs has the same dbg_assert! in the same helpers.")
put("((high - 3) & 0xFFF) == (low & 0xFFF)", "KEEP", "(c)",
    "the 32-bit boundary helpers' contract; upstream cpu.rs has the same dbg_assert!.")
put("i < 16", "KEEP", "(c)",
    "pic's IRQ index: the line number arrives masked (the guest's PIC register bytes are "
    "& 7/& 0xFF) and the loops are bounded; upstream pic.rs has the same dbg_assert!.")
put("i >= 0 && i < 8", "KEEP", "(b)",
    "an index into the 8-entry x87 register file (fpu.c:69/76); upstream fpu.rs.")
put("index < 128", "KEEP", "(c)",
    "the CMOS index is MASKED at the port write -- rtc.c:65 `cmos_index = value & 0x7F` "
    "(rtc.js:77-81) -- so the guest cannot make it >= 128; the check also keeps the "
    "128-byte cmos_data array a bounded read.")
put("(low & 0xFFF) == 0xFFF", "KEEP", "(c)",
    "paging.c's 16-bit boundary helper is only called from the branch that tested it.")
put("(low & 0xFFF) >= 0xFFD", "KEEP", "(c)",
    "as above for the 32-bit helper (`(phys_addr_ & 0xFFF) >= 0xFFD` in paging.h's macro).")
put("port >= 0 && port < 0x10000", "KEEP", "(a)", "io_register_*'s API precondition.")
put("(cpu->prefixes & PREFIX_MASK_REP) == 0", "KEEP", "(c)",
    "instr_F2/instr_F3's own guard (upstream instructions.rs:2159+ has the identical "
    "dbg_assert!).  MEASURED NOT REACHABLE: `F2 F2 90` on the current tree runs green "
    "(rc=0, same terminal state as the reference) because run_prefix_instruction clears "
    "the accumulator before re-dispatching, so the second F2 sees a zero REP mask.")
put("cpu->protected_mode", "KEEP", "(c)",
    "instr16_9C/32_9C: the guarded branch is `FLAG_VM && getiopl() < 3`, i.e. VM86, which "
    "IS protected mode; upstream instructions.rs has the same dbg_assert!.")
put("queue->used_addr != 0", "KEEP", "(a)", "virtqueue state installed by its own setup.")
put("reg == ECX || reg == ESI || reg == EDI", "KEEP", "(c)",
    "misc_instr.c's get/set_reg_asize are called only by the string ops, which pass those "
    "three; upstream cpu.rs has the same dbg_assert!.")
put("(ret & 0xFFFF0000u) == 0", "KEEP", "(c)",
    "arith.c's saturate_sd_to_sw/uw *_handle_input helpers; upstream arith.rs.")
put("size < 0x1000", "KEEP", "(c)",
    "readable_or_pagefault/writable_or_pagefault_cpl are called with constant operand "
    "sizes (1/2/4/8/10/16); upstream cpu.rs has the same dbg_assert!.")
put("size > 0", "KEEP", "(c)", "as above (the same two call sites).")
put("src_addr < cpu->memory_size", "KEEP", "(b)",
    "memory.c's memcpy_no_mmap_or_dirty_check/memcpy_into_svga_lfb write through raw "
    "`mem8 + addr`; upstream memory.rs has the same dbg_assert!.")
put("value >= 0 && value < 0x10000", "KEEP", "(c)",
    "paging.h's SAFE_READ_WRITE16 macro, whose `value_` operand is built from 8/16-bit "
    "reads; the second site (io.c:417) is the device read bridge's return width, which "
    "the port's own handlers supply.")
put("x >= 0 && x < 0x100", "KEEP", "(c)", "arith.c add8/cmp8; the callers pass `read_reg8`.")
put("x >= 0 && x < 0x10000", "KEEP", "(c)", "arith.c add16/cmp16; the callers pass `read_reg16`.")
put("y >= 0 && y < 0x100", "KEEP", "(c)", "as the x case above.")
put("y >= 0 && y < 0x10000", "KEEP", "(c)", "as the x case above.")
put("((uint32_t)v & 0xFFFF0000u) == 0", "KEEP", "(c)",
    "arith.c's saturate_sw_to_sb; upstream arith.rs:1137-1138 has the identical "
    "dbg_assert!.  The value is the caller's own already-clamped computation.")
put("(uint32_t)cpu->last_op1 <= (uint32_t)m", "KEEP", "(c)",
    "flags.c getcf; upstream misc_instr.rs:10-11 has the identical dbg_assert! and the "
    "values are stored by the port's own masked arithmetic ops.")
put("(uint32_t)cpu->last_result <= (uint32_t)m", "KEEP", "(c)", "as above (:11).")
put("cpu->cpl == 0", "KEEP", "(c)",
    "a privileged-register write path; upstream cpu.rs has the same dbg_assert!.")
put("getiopl() == 3", "KEEP", "(c)",
    "cpu.rs's iopl gate; the guard above it is what makes it true.")
put("index >= 0 && index < 8", "KEEP", "(b)", "an x87/integer register index; upstream cpu.rs.")
put("seg <= 5 && seg >= 0", "KEEP", "(c)", "a segment register index; upstream cpu.rs.")
put("segment >= 0 && segment < 8", "KEEP", "(c)", "as above; upstream cpu.rs.")
put("segment != CS && segment != SS", "KEEP", "(c)",
    "upstream cpu.rs's dbg_assert! in iret/far_return/get_seg -- the call paths are the "
    "ones that exclude CS and SS by construction.")
put("addr % 8 == 0", "KEEP", "(c)", "memory.c's 64-bit access path; upstream memory.rs.")
put("dst_addr < cpu->memory_size", "KEEP", "(b)",
    "memcpy_into_svga_lfb copies through raw `mem8`; upstream memory.rs.")
put("eax < 0x10000", "KEEP", "(c)",
    "instructions_0f.c's CPUID leaf 0x15/0x16 registers are the port's OWN constants "
    "(TSC_RATE and the core-clock arithmetic), not guest input.")
put("ebx < 0x10000", "KEEP", "(c)", "as the eax case above.")
put("ecx < 0x10000", "KEEP", "(c)", "as the eax case above.")
put("ecx > 0", "KEEP", "(c)", "as the eax case above (a constant times TSC_RATE).")
put("length % 8 == 0", "KEEP", "(c)", "memory.c's 128-bit path; upstream memory.rs.")
put("modrm_byte < 192", "KEEP", "(a)",
    "modrm.c's mod/reg/rm routing; upstream modrm.rs has the same dbg_assert!.")
put("prefix != SEG_PREFIX_ZERO", "KEEP", "(c)",
    "segment_prefix()'s asserting form is called only by the string ops; SEG_PREFIX_ZERO "
    "is set by lea (instructions.c:1040/1060) to make modrm_resolve skip the base, and "
    "the non-asserting get_seg_prefix is what those paths use.")
put("valid_tlb_entries_count < VALID_TLB_ENTRY_MAX", "KEEP", "(a)",
    "the port's TLB array bound; upstream cpu.rs.  Guest input cannot size the TLB.")
put("value >= 0 && value < 0x100", "KEEP", "(c)", "an 8-bit read's value; upstream cpu.rs.")
put("value_ >= 0 && value_ < 0x100", "KEEP", "(c)",
    "paging.h's SAFE_READ_WRITE8 macro: `value_` is built from an 8-bit read.")
put("value_ >= 0 && value_ < 0x10000", "KEEP", "(c)",
    "paging.h's SAFE_READ_WRITE16 macro: `value_` is built from a 16-bit read.")
put("(high ^ ((uint32_t)page << 12)) & 0xFFF) == 0", "KEEP", "(c)",
    "invlpg's own address arithmetic; upstream cpu.rs has the same dbg_assert!.")
put("i <= count", "KEEP", "(a)", "string.c's rep-loop counter; upstream string.rs.")
put("in_svga_lfb(dst_addr)", "KEEP", "(c)",
    "memory.c's LFB copy: the caller tested it (the branch structure makes it true); "
    "upstream memory.rs.")
put("irq_mask != 0", "KEEP", "(a)", "pic.c's own IRR bookkeeping; upstream pic.rs.")
put("irq_mask == 1 << irq_number", "KEEP", "(a)", "as above.")

# ------------------------------------------------- the port's own (non-class) guards
put("i < 16", "KEEP", "(c)", "see the class's row above (identical condition).")
put("!pic0_get_irq(&pic.master, &ignored)", "KEEP", "(a)",
    "pic_acknowledge_irq: the ISR/IRR relation the function just established.")
put("!pic0_get_irq(&pic.slave, &ignored)", "KEEP", "(a)", "as above (the slave's handshake).")
put("(addr & (MMAP_BLOCK_SIZE - 1)) == 0", "KEEP", "(a)",
    "io.c's mmap write shim: an alignment contract on the port's own MMAP layer.")
put("aligned_addr < MMAP_BLOCK_COUNT", "KEEP", "(a)", "as above (the block index bound).")
put("size != 0 && (size & (MMAP_BLOCK_SIZE - 1)) == 0", "KEEP", "(a)",
    "as above (the registered range).")
put("(address & 1) == 0", "KEEP", "(a)",
    "pci_write16's routing: the io layer dispatches the width, so the config-space offset "
    "is aligned by construction.")
put("(address & 3) == 0", "KEEP", "(a)", "as above for pci_write32.")
put("!(addr >= 0x30 && addr < 0x34)", "KEEP", "(a)",
    "pci_write16's own out-of-band register guard (the caller routes those separately).")
put("!((addr >= 0x10 && addr < 0x2C) || (addr >= 0x30 && addr < 0x34))", "KEEP", "(a)",
    "pci_write8's out-of-band register guard, as above.")
put("(address & 1) == 0", "KEEP", "(a)", "duplicate of the pci_write16 row.")
put("bar_entry_pool_used + (int32_t)bar->size <= PCI_BAR_ENTRY_POOL", "KEEP", "(a)",
    "pci_register_device: the port's own BAR pool accounting (host caller).")
put("(bar->size & (bar->size - 1)) == 0", "KEEP", "(a)",
    "pci.c's BAR size must be a power of two; a property of the DEVICE's descriptor.")
put("device->pci_id < PCI_DEVICE_COUNT", "KEEP", "(a)", "pci_register_device's host precondition.")
put("device->pci_space != NULL", "KEEP", "(a)", "as above.")
put("device->pci_bars_count == 0 || device->pci_bars != NULL", "KEEP", "(a)", "as above.")
put("device->pci_space_length >= 64 && device->pci_space_length <= PCI_SPACE_DWORDS", "KEEP", "(a)",
    "as above (the config space's size class).")
put("space != NULL", "KEEP", "(a)", "duplicate of the pci row.")
put("type == 1", "KEEP", "(a)", "pci_register_device's BAR type field (host descriptor).")
put("port + (int32_t)j < IO_PORT_COUNT", "KEEP", "(a)",
    "pci_set_io_bars: the port's own IO-port table bound (host descriptor drives it).")
put("empty_entry.device == NULL || empty_entry.read8 != NULL || empty_entry.write8 != NULL", "KEEP", "(a)",
    "pci_set_io_bars: the io table's occupancy invariant.")
put("arm != NULL", "KEEP", "(a)", "the port's own device pointer, set by its setup.")
put("connector->listener_count < BUS_MAX_LISTENERS", "KEEP", "(a)", "bus.c's own array bound.")
put("unmask_listener_count < DMA_MAX_UNMASK_LISTENERS", "KEEP", "(a)", "dma.c's own array bound.")
put("w8 != NULL || w16 != NULL || w32 != NULL", "KEEP", "(a)", "io_register_write's API precondition.")
put("r8 != NULL || r16 != NULL || r32 != NULL", "KEEP", "(a)", "io_register_read's API precondition.")
put("mem8 == NULL", "KEEP", "(a)", "allocate_memory's one-shot contract (host caller).")
put("count_until_page_end > 0", "KEEP", "(c)",
    "string.c's page-window helper: its caller only calls it while count remains.")
put("(cr3 & 0xFFF) == 0", "KEEP", "(c)",
    "set_cr3: the low 12 bits are architecturally reserved; upstream cpu.rs has the same "
    "dbg_assert! and the port masks/ignores them the same way.")
put("(dst_addr >> 12) == ((dst_addr + count - 1) >> 12)", "KEEP", "(c)",
    "memory.c's LFB copy: the caller guarantees one page; upstream memory.rs.")
put("(new_mxcsr & ~MXCSR_MASK) == 0", "KEEP", "(c)",
    "misc_instr.c set_mxcsr; upstream cpu.rs's dbg_assert! says 'checked by caller' and "
    "the port's caller (instructions_0f.c:1425) triggers #GP on reserved bits first.")
put("(offset & 0x3) == 0", "KEEP", "(a)", "virtio's own capability-offset alignment.")
put("(phys >> 12) == ((phys + blk.phys_len - 1u) >> 12)", "KEEP", "(a)",
    "the uop decoder's one-page-per-block framing (M1/M2's own decoder).")
put("n <= (int)UOP_MAX_BLOCK_UOPS", "KEEP", "(a)", "as above (the block cap).")
put("(size & (size - 1)) == 0", "KEEP", "(e)",
    "virtqueue_set_size: the guest CAN write a non-power-of-two queue size through the "
    "virtio config bar.  Whether the JS reference proceeds (it has no dbg_assert in the "
    "release bundle) is NOT established here; flagged for the reviewer, reachability is "
    "the open question, not the port's memory model.")
put("size <= queue->size_supported", "KEEP", "(e)", "as above (the same guest-written field).")
put("size != 0 && (size & (MMAP_BLOCK_SIZE - 1)) == 0", "KEEP", "(a)", "the mmap shim, as above.")
put("(cap->port & (bar_size - 1)) == 0", "KEEP", "(a)", "virtio's own capability arithmetic.")
put("cap->bar >= 0 && cap->bar < 6", "KEEP", "(a)", "as above (the device's own descriptor).")
put("cap_next + cap_len <= 256", "KEEP", "(a)", "as above (the config space it builds).")
put("cap_next <= 256", "KEEP", "(a)", "as above.")
put("f < 128", "KEEP", "(a)", "virtio_init's own feature-bit table size.")
put("has_version_1", "KEEP", "(a)", "virtio_init's own version declaration.")
put("effective < (uint32_t)VIRTIO_MAX_QUEUES", "KEEP", "(a)", "virtio_init's own queue table.")
put("options->handler_count <= VIRTIO_MAX_QUEUES", "KEEP", "(a)", "the device descriptor's own field.")
put("options->handler_count == 1", "KEEP", "(a)", "as above.")
put("options->notification.handlers[effective] != NULL", "KEEP", "(a)", "as above.")
put("options->notification.handlers[i] == NULL || offsets[i]", "KEEP", "(a)", "as above.")
put("queue->num_staged_replies < queue->size", "KEEP", "(a)", "the virtio reply ring's own bound.")
put("virtqueue_has_request(queue)", "KEEP", "(a)", "the virtqueue API's precondition.")
put("virtqueue_is_configured(queue)", "KEEP", "(a)", "as above.")
put("x > 0", "KEEP", "(a)", "virtio_int_log2's argument is the queue size, a power of two.")
put("high < 10", "KEEP", "(c)",
    "rtc.c's BCD unpack of the port's OWN cmos bytes (the port writes them BCD-encoded).")
put("low < 10", "KEEP", "(c)", "as above.")
put("n < 0x100", "KEEP", "(c)", "as above (the byte itself).")
put("i < SIZE", "KEEP", "(a)", "kernel.c's diagnostic buffer loop (host loader path).")
put("reload != 0", "KEEP", "(c)",
    "pit.c: the reload value is installed by the port's own channel setup; the guest "
    "writes it as two bytes, and the DIV-by-zero case is the port's own guard.")
put("t > acpi->timer_last_value", "KEEP", "(c)",
    "the ACPI timer is derived from microtick (cpu->instruction_counter/1000), which is "
    "monotonic; the guest's reads cannot make it go backwards.")
put("tsc_last_extra < tsc_resolution", "KEEP", "(c)",
    "cpu.rs:306-308 gates this on #[cfg(debug_assertions)]; the port keeps it live, and "
    "it is provably unreachable because the branch above clamps `extra` to "
    "`< tsc_resolution - 1` before returning.")
put("cpu->instruction_counter != initial_instruction_counter || uop_pending != 0", "KEEP", "(a)",
    "M2's documented relaxation of upstream's progress assert (cpu.c:192); with the "
    "cache off the second conjunct is dead and it is upstream's assert exactly.")
put("!in_mapped_range((uint32_t)(cpu->eip_phys ^ eip))", "KEEP", "(b)",
    "modrm.c:121 (read_imm8) -- upstream cpu.rs:2590 carries the IDENTICAL dbg_assert! "
    "and the line below it is the raw `mem8.offset(...)` fetch.  Same reading as "
    "cpu.c:100: the reference's read is bounds-checked by the wasm VM in RELEASE (it "
    "traps), the port's flat pointer is not, so the check stands in for a release "
    "mechanism, and this is the 337M-calls-per-benchmark path (results/perf-1-inlining).")
put("device->pci_space_length >= 64 && device->pci_space_length <= PCI_SPACE_DWORDS * 4",
    "KEEP", "(a)",
    "pci_register_device: the DEVICE descriptor's config-space size class (host caller).")
put("data != NULL", "KEEP", "(a)", "the port's own device pointer.")
put("size <= queue->size_supported", "KEEP", "(e)", "see the virtqueue_set_size row.")
put("virtqueue_is_configured(queue)", "KEEP", "(a)", "duplicate of the virtqueue row.")


def main():
    paths = []
    for dirpath, _d, files in os.walk(os.path.join(ROOT, "src")):
        for fn in sorted(files):
            if fn.endswith((".c", ".h")):
                paths.append(os.path.join(dirpath, fn))
    sites = sw.gather_c_sites(paths)
    groups = {}
    for s in sites:
        groups.setdefault(s["key"], []).append(s)

    out = os.path.join(HERE, "perf-fix-2-sweep-verdicts.tsv")
    missing = []
    with open(out, "w", encoding="utf-8") as fh:
        fh.write("# condition\tverdict\treason\twhy\n")
        fh.write("# (generated by results/perf-fix-2-verdicts.py; the sweep joins on column 1)\n")
        for cond in sorted({g[0]["cond"] for g in groups.values()}):
            key = sw.normalise(cond)
            if key not in V:
                missing.append(cond)
                continue
            verdict, code, why = V[key]
            fh.write("%s\t%s %s\t%s\n" % (cond.replace("\t", " "), verdict, code,
                                          " ".join(why.split())))
    # the sweep keys the lookup on the CONDITION TEXT, so write it under that key too:
    # rewrite the sweep's loader to read column 1 as text (see perf-fix-2-sweep.py).
    print("wrote %s (%d rows)" % (out, sum(1 for _ in open(out)) - 2))
    if missing:
        print("UNVERDICTED groups (%d):" % len(missing))
        for c in missing:
            print("   %s" % c[:100])
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
