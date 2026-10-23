# x86-64: the stop was the #PF **delivery**, not the fault — a 32-bit mask on a 64-bit table base

**Status: ONE FIX, MEASURED BY ASYMMETRY, PLUS THE NAMED REMAINING DEFECT. The instruction at
`0xFFFFFFFF8AE77246` is `copy_bootdata`'s `rep movsd`. The fault it takes is the **source** read of
the direct map; the reason the machine *stopped* is that every #PF delivery read the IDT at a
**truncated 32-bit base** — `lgdt`/`lidt` applied the operand-size mask to the 64-bit base long
mode had just loaded. With that fixed the delivery works, the kernel handles the #PF and runs 74,218
instructions further, and then halts on the remaining defect: nothing in the port's run ever maps
the direct map (`PML4[273]`).**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The fault, and what kind of fault it is (MEASURED)

`V86C_DUMP_ON_STOP=1 V86C_CHAIN=0xFFFFFFFF8AE77246`:

    state rip=0xffffffff8ae77246 cr2=0x8b1100e0 cr3=0x30ea000 cr4=0xa0 efer=0xd01
          idtr=0x8b110000/4095 gdtr=0x8ae4d000/127 cpl=0
    regs  rax=0 rbx=0xffff888000084790 rcx=0x400 rsi=0xffff888000084790
          rdi=0xffffffff8b117600 rbp=0xa0 rsp=0xffffffff8a803f30
    chain va=0xffffffff8ae77246 idx=511/510/87/119
      PML4E  @0x30eaff8 = 0x2a15067     (present -> level3_kernel_pgt)
      PDPTE @0x2a15ff0 = 0x2a16063     (present -> level2_kernel_pgt)
      PDE   @0x2a162b8 = 0x30001e3     (present, PS -> 2 MiB page at phys 0x3000000)

The ring's last boundary is `0xffffffff8ae77243` = `48 89 de f3 a5`, so the stop instruction is the
`rep movsd` at `0xffffffff8ae77246`. Disassembled from the image (the ELF's own program headers
give `file 0x2477000 -> phys 0x3077000 -> link 0xffffffff83077000`):

    ffffffff8307722e  push %rbx ; mov %rdi,%rbx ; call …   ← copy_bootdata(char *real_mode_data)
    ffffffff83077237  mov $0xffffffff83317600,%rdi          ← &boot_params (link)
    ffffffff8307723e  mov $0x400,%ecx                       ← sizeof(struct boot_params) = 0x1000
    ffffffff83077243  mov %rbx,%rsi
    ffffffff83077246  rep movsl (%rsi),(%rdi)

and the caller (`0xffffffff83077544`) is `x86_64_start_kernel`'s `copy_bootdata(__va(real_mode_data))`
with `add [0xffffffff82413d60], %rdi` — that global is `page_offset_base`, which the image
initializes to `0xffff888000000000` (`READ`, and `READ` from `head64.c`). So the port's
`rsi = 0xffff888000084790` is `__va(0x84790)` and `rdi = 0xffffffff8b117600` is the *relocated*
`&boot_params` — the image's immediate `0xffffffff83317600` plus the kernel's virtual slide
`0x7E00000` ✓ (slide cross-checked against the program headers, and against the page tables:
VA `0xffffffff8ae00000` → phys `0x3000000` = image offset `0x2000000`).

**So the fetch is right, the code is right, and the page tables that map the code are right.**
`V86C_WATCH` with the fixed hook names the accesses in order:

    WATCH F 0xffff888000084790 read  rip=0xffffffff8ae77246   ← THE FIRST FAULT (the direct map)
    WATCH F 0x8b1100e0        read  rip=0xffffffff8ae77246   ×4   (the IDT gate, i.e. delivery)

`V86C_PEEK 0x30ea888` = **0**: `PML4[273]`, the direct-map slot, is **ABSENT**. The destination is
fine — `V86C_PEEK 0x2a162c0` (PDE[88], `&boot_params`'s 2 MiB page) = `0x032001e3`, present.

## 2. The defect that made it a stop: the operand-size mask on a 64-bit base (MEASURED)

`idtr=0x8b110000` while the kernel's IDT is at `0xffffffff8b110000` — the *same* page range as
`gs=0xffffffff8ae42000`. `src/cpu/instructions_0f.c` read the eight-byte base correctly in long
mode and then threw the high half away:

    cpu->idtr_offset = (uint64_t)offset & (uint64_t)(uint32_t)mask;    /* mask = -1 for 32-bit */

`mask` is the **operand-size** mask (`0xFFFFFF` for a 16-bit operand size, `-1` for a 32-bit one);
in 64-bit mode the operand is a ten-byte descriptor with an **eight-byte base whatever the prefixes
say**, so the mask must not be applied. The same line existed in `lgdt`, and `sgdt`/`sidt` wrote
only four base bytes back.

**Fixed** in all four (`lgdt`, `lidt`, `sgdt`, `sidt`): the base is kept whole in long mode, and the
read-back writes all ten bytes.

**The asymmetry, same instrument, same guest** (MEASURED):

| | before | after |
|---|---|---|
| `idtr` | `0x8b110000/4095` | **`0xffffffff8b110000/4095`** |
| `gdtr` | `0x8ae4d000/127` | **`0xffffffff8ae4d000/127`** |
| outcome | `rc 1`, `unimplemented=1`, "a fault raised while delivering a fault: the delivery re-entered four times" | `rc 0`, `unimplemented=0`, `exception=-1`, **`in_hlt=1`** |
| instructions | 4,475,484,504 | **4,475,558,722** (+74,218) |

So the #PF is now *delivered*: the kernel's early #PF handler runs (the ring ends in
`0xffffffff8ae9c881  call [..] ; jmp $-8` — a panic loop — then `0xffffffff8984e8d9 hlt`), and the
machine halts cleanly instead of stopping on a delivery that could not read its own IDT.

**NAMED GAP:** the arm for this fix is not in `m1c_selftest.c`. Every long-mode `lgdt`/`lidt`
fixture there needs the IA-32e entry edge, and `set_cr0(…|CR0_PG)` refuses it in that harness
(`load_pdpte` validation, and the backend's long-mode refusal), so the fixture asserts in
`do_page_walk64` before the instruction under test runs. The proof above is integration-level, which
is the asymmetry that matters, but a unit arm is owed.

## 3. What is left, named: the direct map is never mapped (MEASURED)

`V86C_WATCH=0x30ea888:8,0x30ea880:8` over the whole run: **18 writes, every one of them the
decompressor's** (`0x4f399c0`). Nothing in the kernel ever writes `PML4[272]`/`PML4[273]`, so
`__va(boot_params)` is unmapped for the whole boot and `copy_bootdata` faults.

QEMU (same image, `-m 256`, same cmdline) shows **no early exception at all** and reaches the
banner, so hardware does have that mapping by then. The kernel's own code for building one on
demand is `__early_make_pgtable(address, pmd)` — `unsigned long physaddr = address - __PAGE_OFFSET;`
followed by the `early_dynamic_pgts` allocation and the `*pgd_p = …` store (`READ`, head64.c:335-370),
and it bails when `read_cr3_pa() != __pa_nodebug(early_top_pgt)`. **The next fork is that function's
callers**: the port's `cr3` is `early_top_pgt` throughout, so its guard passes, and either the port
never reaches the call or the call's own stores are the missing ones. `V86C_BREAK` on its entry
(link `0xffffffff830774xx`, i.e. `0xffffffff8ae774xx` in the port's slide) is the instrument, and
`V86C_WATCH` on the `early_dynamic_pgts` pages (`0x30ec000…`) is its confirmation.

**The calibrated target is NOT reached**: the kernel's first `printk` does not appear, so nothing was
added to `gate-x86-64.sh`. The port's serial still ends at the decompressor's KASLR lines.

## 4. Verification (MEASURED; **code changed**, so the full battery, by EXIT CODE)

    make                     rc 0
    make selftest            rc 0 — all suites green
    make nasm                rc 0
    make nasm64              rc 0
    make bench               200095214 (all runs)
    make transcript-diff     the C transcript is byte-identical to the reference's
    make determinism         two consecutive oracle runs byte-identical
    gate-m2-5-determinism    4,000,096,757 instructions both runs, serials byte-identical
    gate-x86-64              12 passed, 0 failed
    gate-long-mode           57 passed, 0 failed
    gate-long-mode-sys       5 passed, 0 failed
    gate-x86-64-diff         13 passed, 0 failed
    gate-nasm64              13 passed, 0 failed
    gate-reboot-1            39 passed, 0 failed
    gate-pae-1               11 passed, 0 failed
    gate-pae-l3              0 FAILING
    gate-vga-0..4            0 FAILING each

**No pin moved** — as it must not: the mask only changes behaviour in 64-bit mode, which no 32-bit
gate enters, and the 32-bit path still applies the mask (asserted by construction in the same
function).

## 5. Files touched

| file | change |
|---|---|
| `src/cpu/instructions_0f.c` | `lgdt`/`lidt` keep the full 64-bit base in long mode (the operand-size mask applies only outside it); `sgdt`/`sidt` write the ten-byte operand and reserve ten bytes |
| `src/cpu/debug.c` | the break dump now prints `idtr`/`gdtr`/`cr2` — the fields that named this defect |
| `results/x86-64-highpf.md` | this report |
| `TODO.md` | §8 |
