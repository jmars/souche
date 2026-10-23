# x86-64: the port DOES execute `__startup_64` — and the fixup lands. The watch was blind.

**Status: MEASUREMENT + ONE FIX (an instrument, not the machine). The question this unit was
given — "does the port execute `__startup_64` at all?" — is answered YES, with the entry, the
registers, the path through the routine and the fixup's own stores all MEASURED. The premise the
last three units rested on ("the kernel never writes `early_top_pgt`") is **void**: `V86C_WATCH`
could not see a long-mode store at all, because every 64-bit store body calls
`safe_write*_addr()`, which never called the hook.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The answer (MEASURED, one run per claim)

The handover is `head_64.S`'s `jmp *%rax` into the decompressed kernel at **0x1000000**, and the
entry's own code names the rest of the chain. Read from the image (`vmlinux.bin`, `.text` at file
0x200000 = phys 0x1000000, so phys = file + 0xE00000):

    1000000  lea rsp,[rip+0x1a03f51]      ; = 0x2a03f58 = __end_init_task - FRAME_SIZE
    1000007  lea rdi,[rip-0xe]            ; = 0x1000000 = _text  (physbase, the runtime address)
    100000e  mov ecx,0xc0000101           ; MSR_GS_BASE
    1000013  mov rax,[0x2bfb2b8]          ; initial_gs (link-time)
    100001a  mov rdx,0xffffffff81000000
    1000021  sub rax,rdx                  ; rax -= _text
    1000024  add rax,rdi                  ; ... + physbase  -> the fixed-up initial_gs
    100002e  wrmsr
    1000031  call 0x10007e0               ; startup_64_setup_env (lgdt of early_gdt_descr)
    1000041  lretq                        ; CS = 0x10
    1000043  mov rdi,rsi                  ; rdi = boot_params
    1000047  call 0x1001a20               ; sme_enable(bp)      (cpuid 0x80000000/0x8000001f)
    100004d  call 0x1000160               ; verify_cpu
    1000052  lea rdi,[rip-0x59]           ; = 0x1000000 = physbase
    100005a  call 0x1000340               ; __startup_64(physaddr, bp)
    1000060  add rax,0x30ea000            ; += (early_top_pgt - _text)   [relocated by the stub]
    10000a4  add rax,[0x2a1a010]          ; += (-delta), the stub's relocation undone
    10000b5  mov cr3,rax

**READ** (`arch/x86/kernel/head_64.S` and `head64.c`, v6.1, fetched from the source rather than
recalled): `leaq _text(%rip), %rdi` is physbase; `call __startup_64` takes
`(unsigned long physaddr, struct boot_params *bp)`; the routine ends with
`*fixup_long(&phys_base, physaddr) += load_delta - sme_get_me_mask();` and
`return sme_postprocess_startup(bp, pmd);`. The entry matches that source instruction for
instruction, so the identity of every call target above is **READ from the source + matched
against the bytes**, not inferred from an address alone.

`V86C_BREAK` (the toolkit's execution watch, committed in 3b22f38) on the whole entry path:

| break | registers at the hit |
|---|---|
| `0x1000000` (entry) | `rax=0x1000000 rbx=0x4780000 rsi=0x84790 rsp=0x4f57000` |
| `0x1000060` (the cr3 addend) | `rax=0x0 rbp=0x1000000 rdi=0x2a03ee8 r10=0x30ed000` |
| `0x1000066` (after the add) | `rax=0xaeea000` |
| `0x10000ac` (after `+ (-delta)`) | `rax=0x30ea000` |
| `0x10000b5` (`mov cr3,rax`) | `rax=0x30ea000 rdi=0x30ea000` |

So the port executes the entry, `startup_64_setup_env`, `sme_enable`, `verify_cpu`,
`__startup_64`, `sme_postprocess_startup`, the CR3 formation and `mov cr3`. **(i) yes, (ii) the
registers above, and the next instructions are exactly the ones listed.**

## 2. `__startup_64`'s own stores DO execute, and they are CORRECT (MEASURED)

`V86C_BREAK` inside the routine, plus `V86C_PEEK` of the page-table pages at each hit:

    0x10004cd  (*next_pgt_ptr)++        rbx=0x30ea000 rbp=0x2a4c8f8 rax=0x2
    0x10007b7  pgd[i+0] = pud + flags   rbx=0x30ea000 rcx=0x0 rdx=0x30ec063
    0x1000613  pmd[idx] = pmd_entry     rsi=0x10000e3 rax=0x8
    PEEK 0x30ea000 after the pgd stores:  63 c0 0e 03 00 00 00 00  63 c0 0e 03 00 00 00 00

**`early_top_pgt[0] = early_top_pgt[1] = 0x30ec063`** — present, RW, pointing at
`early_dynamic_pgts[0]`. That is `__startup_64`'s identity-map switchover, written correctly.
`level2_kernel_pgt` (`0x2a16000`) is filled and invalidated, and the `phys_base` tail store lands
at `0x2a1a010`. **(iii) the fixup's stores execute and write the values the source says.**

The addresses, derived from the kernel's own `fixup_pointer` arithmetic and confirmed by the
break registers:

| symbol | vaddr | phys |
|---|---|---|
| `early_top_pgt` | `0xffffffff830ea000` | **0x30EA000** |
| `early_dynamic_pgts` | `0xffffffff830ec000` | 0x30EC000 |
| `level2_kernel_pgt` | `0xffffffff82a16000` | 0x2A16000 |
| `next_early_pgt` | `0xffffffff82a4c8f8` | 0x2A4C8F8 |
| the `-delta` global | `0xffffffff82a1a010` | 0x2A1A010 |

**So `cr3 = 0x30EA000` is CORRECT.** `__startup_64` returns 0 because `sme_postprocess_startup`
returns 0 when the SME mask is 0 — the port takes that exact early-return path (ring:
`0x1001570` → `cmp qword [0x2a4c8f8],0` → `je 0x10018f4` → `xor eax,eax` → `ret`) — and
`0 + (early_top_pgt - __START_KERNEL_map)` is `0x30ea000`. The decompressor relocated that
immediate by its own `delta = 0x7E00000` (the bytes in memory are `add rax,0x0aeea000`), and the
kernel's next instruction adds the `-delta` global back, so the net is the linked constant.

## 3. THE DEFECT THIS UNIT FOUND: the watch could not see a long-mode store

`V86C_WATCH` was hooked at the top of `safe_write8/16/32/64()`. But **every 64-bit store body
calls the `_addr` twins directly** (`src/cpu/instructions_64.c:104,168,194,…` and the generated
overlay), and the string path calls them too (`src/cpu/string.c:140-143`), and the bulk
`rep movs`/`stos` path writes through `memcpy_no_mmap_or_dirty_check`/`memset_…`
(`string.c:502,518`). The guest under investigation runs in **no other mode**, so the watch was
blind for every store that mattered.

The asymmetry, both MEASURED on this tree:

| run | instrument | result |
|---|---|---|
| before | `V86C_WATCH=0x30ea000:24576` (24 KiB: `early_top_pgt` + the dynamic tables) | 24,589 writes, **every one from the decompressor** (`0x4f399c0`, `0x4f3a168`, `0x4f39d3d`) — "the kernel never writes it" |
| after | `V86C_WATCH=0x30ea000:16` | 16 decompressor byte-writes **plus `rip=0x10007bb` and `rip=0x10007c0`** — the two `pgd` stores |

The same blindness hid the decompressor's `.data` segment copy and the `-delta` store at
`0x2a1a010` (the memory held `0xfffffffff8200000` while the watch reported no writer).

**This voids the central claim of `results/x86-64-startup64.md` and `results/x86-64-bisect.md`.**
Not because the measurement was careless but because *a control establishes attributability,
never exclusivity* — and the control's domain was the wrong store path.

## 4. The fix

`src/cpu/paging.c`: the hook now lives in the `_addr` twins (after each straddle split, so one
guest store is one line), and the hooks in `safe_write8/16/32/64()` were moved **below** their
`long_mode_64()` branch so nothing is logged twice. Cost when off: one load and one branch per
store, the same shape as before; the bench pin did not move (§6).

Able-to-fail arm: `test_debug_toolkit_range_overlap` (`src/cpu/m1c_selftest.c`) covers the range
seat's overlap arithmetic — a range that does not contain a watched byte must not count. The
per-store seat itself is proven at the integration level by the two runs in §3, which is the
asymmetry that matters: the same watch, the same guest, the same address, before and after.

**Discarded, and said plainly:** the working tree also carried an uncommitted, *never-green* arm
from the attempt that died (`test_debug_toolkit_sees_bulk_string_store`). Its fixture does not
produce a working long-mode store — MEASURED `cr2=0x20e0` with the store at `0x31558`, and the
same fault occurs with the new hook compiled out, so the fault is the fixture's and not this
unit's. I could not make it green inside this unit's budget, so it is not in the tree; the range
hook it was written for **is** in the tree (`debug.c`, `string.c`) and has its own green arm
above.

## 5. What the guest does now, against the calibrated target (MEASURED)

    $ ./build/v86c --bzimage tests/images/vmlinuz-6.1.0-50-amd64 6000000000 --memory 256 \
        --cmdline 'console=ttyS0 earlyprintk=serial' --serial /tmp/xr9.txt --long-mode on
    v86c boot: 4475484504 instructions, in_hlt=0, exception=-1, unimplemented=1,
               eip=0x8ae77246 cs=0x10 cr0=0x80050033 cr3=0x30ea000
    v86c: unimplemented: a fault raised while delivering a fault: the delivery re-entered four
          times (hardware would raise #DF; the port stops rather than recursing)

    serial tail: "Physical KASLR disabled: no suitable memory region!"

**The kernel's first `printk` is NOT reached.** The stop is the same one the previous units
recorded (same eip, same instruction count), and it is now known **not** to be a page-table
defect: the tables are right, `cr3` is right, `__startup_64` ran and returned correctly. The
faulting address is `0xFFFFFFFF8AE77246` (the print shows the low 32 bits), i.e. the kernel's own
high-half text — a #PF there is the next question, and the toolkit can now answer it *correctly*
for the first time.

## 6. Verification (MEASURED; **code changed**, so the full battery, every step by EXIT CODE**)

| step | result |
|---|---|
| `make` | rc 0 |
| `make selftest` | rc 0 — **all suites green** (incl. the new `test_debug_toolkit_range_overlap`) |
| `make nasm` | rc 0 |
| `make nasm64` | rc 0 |
| `gate-nasm64.sh` | 13 passed, 0 failed |
| `make bench` | **200,095,214** in all four runs |
| `make determinism` | two consecutive oracle runs byte-identical |
| `make transcript-diff` | the C transcript is **byte-identical** to the reference's |
| `gate-m2-5-determinism.sh` | 0 FAILING; **4,000,096,757** instructions in both runs, serials byte-identical |
| `gate-m2-5.sh` | 0 FAILING |
| `gate-x86-64.sh` | 12 passed, 0 failed |
| `gate-long-mode.sh` | 57 passed, 0 failed |
| `gate-long-mode-sys.sh` | 5 passed, 0 failed |
| `gate-x86-64-diff.sh` | 13 passed, 0 failed |
| `gate-reboot-1.sh` | 39 passed, 0 failed |
| `gate-pae-1.sh` | 11 passed, 0 failed |
| `gate-pae-l3.sh` | 0 FAILING |
| `gate-vga-0..4.sh` | 0 FAILING each |

**No pin moved.** One observation, not a pin: the gate runs regenerate `results/m2-5-*.txt` and
`results/pae-l3-*.txt`, and those regenerated serials carry four extra VGA PCI lines that the
committed copies lack. That is **pre-existing** — this unit's diff touches no machine code (a
debug hook, a selftest arm, and the position of an existing hook) — and the gate's own C-vs-JS
comparison passes on the regenerated output. The files were restored rather than committed, so
the churn is not in the tree.

## 7. Files touched

| file | change |
|---|---|
| `src/cpu/paging.c` | the watch hook moved into the four `safe_write*_addr()` twins; the four `safe_write*()` hooks moved below their long-mode branch |
| `src/cpu/m1c_selftest.c` | `test_debug_toolkit_range_overlap` (green; the range seat's overlap arithmetic) |
| `results/x86-64-reach2.md` | this report |
| `TODO.md` | §8 |
