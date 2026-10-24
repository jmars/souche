# x86-64: the descriptor-address path is 64 bits — the GDT base is no longer projected, and the guest boots to 74 s

**Status: THE FIX + AN ASYMMETRIC ARM + THE GUEST RUN + THE FULL BATTERY (all green, no pin moved).**
Claims tagged MEASURED / READ / INTERPRETATION.

## 1. What changed (READ)

`src/cpu/exceptions.c`, `lookup_segment_selector` — the two casts `results/x86-64-e7e8.md` named:

| before | after |
|---|---|
| `uint32_t table_offset = (uint32_t)cpu->gdtr_offset;` (and the LDTR branch likewise) | `uint64_t table_offset = cpu->gdtr_offset;` (LDTR: `cpu->segment_offsets[LDTR]`) |
| `int32_t address = (int32_t)offset + (int32_t)table_offset;` | `int64_t address = (int64_t)offset + (int64_t)table_offset;` |
| out-parameter `int32_t *descriptor_address` | `int64_t *descriptor_address` — **12 call-site locals followed** (8 in `exceptions.c`, 4 in `misc_instr.c`) plus the recording definition in `cpu_selftest.c:1680` |
| `int32_t address = (int32_t)translate_address_system_write(descriptor_address + 5);` then `write8((uint32_t)address, …)` (both accessed-bit writes) | `uint32_t address = translate_address_system_write(descriptor_address + 5);` then `write8(address, …)` |
| the IDT-gate path's `int32_t descriptor_address = (int32_t)translate_address_system_read(…)` with two `read64s((uint32_t)descriptor_address)` | `uint32_t descriptor_address = translate_address_system_read(…)` with plain `read64s(descriptor_address)` |

**THE SINGLE PROJECTION BACK TO THE MODEL'S 32-BIT PHYSICAL ADDRESS IS
`translate_address_system_read()`/`_write()`'s `uint32_t` RETURN** — stated in the code comment at the
site, the same shape as S3's named projection: the *linear* address is 64 bits, the *physical* address
the port's memory model can carry is 32, and that one function is where the narrowing happens.

## 2. The arm (MEASURED: fails before, passes after)

`src/cpu/m1c_selftest.c`'s `test_long_mode_descriptor_address_is_64bit`: the fixture builds 4-level
paging (`PML4[0] → PDPT → PD` identity 2 MiB, as the 64-bit gate test does) **plus** `PDPT[4] → PD → PT[0]`
mapping the 4 GiB page to the frame at 1 MiB, puts an available 386-TSS descriptor at
`gdt_base + 0x40 = 0x100000040` and a **null** descriptor at `0x40` (the address the truncation used),
then asserts `addr == 0x100000040` and `desc.raw == tss_raw`.

**4 KiB pages, not a 2 MiB one, deliberately**: the fixture has 2 MiB of RAM, so a 2 MiB frame could only
be frame 0 — the very address the truncation reads — which would make the arm blind to the defect.

    with `(uint32_t)cpu->gdtr_offset` restored:  FAIL addr == 0x100000040 (got 0x40)
                                                 FAIL desc.raw == tss_raw (got 0x0, want 0x890010000067)
                                                 FAIL seg_descriptor_system_type(desc) == 9 (got 0x0)
    with the fix:                                PASS

## 3. The guest after the fix (MEASURED, 256 MiB, `--long-mode on`, 6G budget)

    4590876590 instructions  (was 4520768763)  — and the serial now reaches 74 s of boot:
    [   74.035257] Calibrating delay loop (skipped), value calculated using timer frequency.. 2000.00 BogoMIPS (lpj=4000000)
    [   74.037591] Last level iTLB entries: 4KB 64, 2MB 64, 4MB 64
    [   74.037591] Last level dTLB entries: 4KB 64, 2MB 0, 4MB 64, 1GB 0
    [   74.037591] Spectre V1 : Mitigation: usercopy/swapgs barriers and __user pointer sanitization

`cpu_init`'s `ltr` now succeeds; the kernel is well past `setup_per_cpu_areas` and into `start_kernel`'s
mitigation setup.

**The stop is NOT stable, so `tests/gate-x86-64.sh`'s stop step was NOT touched.** Two runs, same binary,
differ: the KASLR run ends in a delivery fault at `eip=0x897e42c6` after 4590876590 instructions, while
the `nokaslr` run runs the whole 6,000,000,000-instruction budget (`unimplemented=0`, `eip=0x81a4f6e5`)
and prints the same last serial line. A stop that depends on the KASLR slide is a different defect from
the one this unit fixed, and pinning it now would pin a coincidence.

## 4. Gates (all by EXIT CODE; **no pin moved**)

    make ✓ · selftest ✓ (all suites green) · nasm 118/118 ✓ · transcript BYTE-IDENTICAL ✓
    determinism (two oracle runs byte-identical) ✓ · bench 200,095,214 ✓
    gate-m2-5-determinism 4,000,096,757 both runs ✓ · gate-m2-5-guest 0 FAILING ✓
    gate-x86-64 14/0 ✓ (the banner AND its stop step) · gate-long-mode 57/0 ✓
    gate-long-mode-sys 5/0 ✓ · gate-x86-64-diff 13/0 ✓ · gate-nasm64 13/0 ✓
    gate-reboot-1 39/0 ✓ · gate-vga-0..4 0 FAILING ✓ · gate-pae-1 11/0 ✓ · gate-pae-l3 0 FAILING ✓

The 32-bit behaviour is byte-identical (transcript + the 118-image corpus + the whole 32-bit gate set),
as it must be: the widening only changes values that were already within 32 bits in those modes.

## The ONE next measurement

**Symbolicate the KASLR-only stop `eip=0x897e42c6`** (subtract that run's slide, as this lane did for
`0xffffffff88e7e8d4`) **and read its operands at the break — but first establish which of the two runs is
the true state**, since `nokaslr` does not stop at all: run the same binary twice at the same settings and
compare the instruction counts before treating `0x897e42c6` as the next defect.
