# x86-64: `tools/kallsyms.py` now refuses an address it cannot place — and `watchphys` §1 is retracted

Unit: two small instruments-of-record fixes from `souche-instruments-review` (finding **M5** and minor
**m1**) plus the retraction that finding **M1** obliges. Repo `~/futaie/souche`, baseline HEAD
`cf4ada0`. `v86/` untouched, nothing committed, nothing pushed. No guest path, no solver/CPU
behaviour touched: the unit is `tools/kallsyms.py`, two docs, and one report.

Tags: **MEASURED** = a command run in this unit with its output quoted; **READ** = read off the code
or the tree's record with file:line; **INTERPRETATION** = an inference over the two.

---

## 0. Bottom line, and which finding was which

| finding | verdict |
|---|---|
| **M5** — confident wrong names outside kernel text | **a real defect, now fixed.** The tool prints a NAMED REFUSAL for the direct-map/vmalloc range, the module region, the percpu offset namespace, non-canonical values and anything above the image's highest symbol. |
| **m1** — "absolute-percpu probed before base-relative, both only sortedness checks" | **the probe ORDER was a real (latent) defect; the reachability claim was a docstring overclaim.** MEASURED: on the pinned image base-relative is rejected by sortedness at symbol 384, so the wrong arm could not win here — m1 was not reachable on this image, exactly as the review said. What was overclaimed was the docstring's "a different image cannot silently produce another image's names": the robustness rested on the image's own convention, not on a check. The discriminator is now enforced (text-VMA landing + non-decreasing), so the claim is true rather than aspirational. |

Nothing in the PARSE changed: the pad discriminator, the token-table walk, the markers check and the
name decompression are byte-for-byte what the reviewer verified sound on this image.

---

## 1. The address domain, computed from the image (MEASURED)

`text_range()` takes the lowest and highest VMA of `.text`/`.init.text`/`.exit.text` from the ELF
section headers — the image measuring itself. `symbol_domain()` then takes the table's first symbol at
or above that floor, and the table's highest symbol:

```
.text      0xffffffff81000000 .. 0xffffffff81e01d32
.init.text 0xffffffff83077000 .. 0xffffffff830e57f8
.exit.text 0xffffffff83301658 .. 0xffffffff83302382
=> text VMA range 0xffffffff81000000..0xffffffff83302382
=> symbol domain 0xffffffff81000000..0xffffffff84a00000  (94101 symbols)
=> percpu offsets 0x0..0x35000 (382 symbols below the floor, ending at __per_cpu_end)
```

An address outside the domain gets a NAMED REFUSAL and exit code 1, never a name — because the
nearest-entry lookup always HAS an answer, and a plausible wrong name is the worst output a naming
tool can produce. MEASURED on `/tmp/vmlinux.bin`, the three addresses the reviewer measured, plus two
more classes:

```
$ python3 tools/kallsyms.py /tmp/vmlinux.bin 0xffff888000000000 0xffffffffc0123000 0x1000 \
      0x50000000 0x800000000000 0xffffffff84a00001 0xffffffff81c00c02
0xffff888000000000 -> <not kernel text: the direct-map/vmalloc/vmemmap range (0xffff800000000000..0xffffffff81000000) -- an address there is a runtime mapping, not an image symbol; no symbol name is meaningful here>
0xffffffffc0123000 -> <not kernel text: the module region (0xffffffffc0000000..0xffffffffff000000) -- symbols there are runtime-loaded, not in this image>
0x1000 -> <not an address: the percpu offset namespace (this image's per-cpu symbols occupy 0x0..0x35000) -- a per-cpu symbol's offset, meaningless without that CPU's per-cpu base>
0x50000000 -> <not kernel text: below the image's text floor 0xffffffff81000000 and outside the percpu offsets (0x0..0x35000) -- a low-half or unmapped value, not an image symbol>
0x800000000000 -> <not a canonical address: bits 63:48 are neither all 0 nor all 1, so this is not a virtual address at all>
0xffffffff84a00001 -> <not kernel text: above the highest symbol in this image (0xffffffff84a00000)>
0xffffffff81c00c02 -> asm_exc_page_fault+0x22        (rc 0 for this line alone; the run's rc is 1)
```

Classes distinguished: non-canonical; the module region (by the kernel's `MODULES_VADDR`/`MODULES_END`
convention); above the image's highest symbol; inside the domain (named); the direct-map/vmalloc/
vmemmap half below the text floor; the percpu offset namespace, whose ceiling is read from the table
itself (0x35000 here) rather than guessed; and any other below-floor low-half value.

The percpu **offset** case is a deliberate refusal, and it is why the old "below the first kallsyms
entry" guard was dead code: the percpu zero-offset entries sit at index 0 (MEASURED: symbols 0-4 are
`0x0, 0x0, 0x1000, 0x2000, 0x6000`), so nothing was ever below the first entry. A percpu offset is not
an address — it needs that CPU's per-cpu base — so `0x1000` is now refused as what it is.

---

## 2. The probe order and the discriminator (review m1)

**Decision: base-relative is probed FIRST** (it is the modern kernel's default:
`CONFIG_KALLSYMS_BASE_RELATIVE` without `CONFIG_KALLSYMS_ABSOLUTE_PERCPU`), and a mode is accepted
only on evidence — `_validates()` requires

1. **the first decoded address at or above the text floor to fall INSIDE the ELF's text VMA range**,
   and
2. **the decoded table to be non-decreasing**.

If no mode validates, the tool exits non-zero naming the reason and prints no names at all.

Why condition 1 is stated that way and not as "`addrs[0]` is in `.text`": on the correct decode of
THIS image `addrs[0]` is `0x0`, a percpu symbol. The first address at or above the floor is symbol
382 = `0xffffffff81000000` = `_text`, and that is the entry the test uses.

MEASURED, the two arms on the pinned image at the accepted pad (4 pad bytes between the int32 array
and `kallsyms_relative_base`):

```
base-relative    REJECTED: not non-decreasing at symbol 384 (0x10000000080ffffff > 0x10000000080ffff8f)
absolute-percpu  accepted: first text symbol 0xffffffff81000000 (symbol 382), 90296 of 94101 symbols
                 between the text floor and the text sections' end 0xffffffff83302382, last 0xffffffff84a00000
```

So m1 was **not reachable on this image** — the wrong arm cannot sort here, which is what the review
said. The claim in the docstring ("a different image cannot silently produce another image's names")
was still an overclaim, because the code's protection was the image's own convention, not a check.
Condition 1 has teeth where sortedness does not — MEASURED in-process on a **sorted** raw-offset decode
(what a sortedness-only probe accepts, and what a base-relative-only kernel would hand the percpu arm):

```
old check (sortedness only) accepts: True
new _validates accepts: False | reason: no decoded address reaches the text floor 0xffffffff81000000
new _validates on the real table: True | first text symbol 0xffffffff81000000 (symbol 382), ...
```

The banner now states the chosen convention AND that evidence, so a reader can tell whether the parse
is the one they think:

```
kallsyms: convention absolute-percpu chosen on evidence -- first text symbol 0xffffffff81000000
(symbol 382), 90296 of 94101 symbols between the text floor and the text sections' end
0xffffffff83302382, last 0xffffffff84a00000; symbol domain 0xffffffff81000000..0xffffffff84a00000
(text VMA range 0xffffffff81000000..0xffffffff83302382 from .text+.init.text+.exit.text, percpu
offsets 0x0..0x35000); addresses outside the domain are refused by name
```

---

## 3. `--verify`: anchors the image pins down itself (MEASURED)

**Exact invocation:**

```
python3 tools/kallsyms.py /tmp/vmlinux.bin --verify
```

Output (stdout; the two banner lines go to stderr), **rc 0**:

```
verify: PASS .text boundary symbol: the ELF puts .text at 0xffffffff81000000; the table names startup_64, _stext, _text there
verify: PASS .rodata boundary symbol: the ELF puts .rodata at 0xffffffff82000000; the table names __start_rodata there
verify: PASS .bss boundary symbol: the ELF puts .bss at 0xffffffff8330d000; the table names empty_zero_page, __bss_start, __nosave_end there
verify: PASS __stack_chk_fail: 0xffffffff81a3d470 is the target of 8914 call rel32 in .text (first 0xffffffff81000dd3); the table names __stack_chk_fail
verify: PASS #PF entry: 0xffffffff81c00c02 names asm_exc_page_fault+0x22 (the image's #PF entry code, results/x86-64-rsplow.md ss4)
verify: 0xffff888000000000 -> <not kernel text: the direct-map/vmalloc/vmemmap range (0xffff800000000000..0xffffffff81000000) -- ...>
verify: PASS refusal of 0xffff888000000000: refused by name, not named
verify: 0xffffffffc0123000 -> <not kernel text: the module region (0xffffffffc0000000..0xffffffffff000000) -- ...>
verify: PASS refusal of 0xffffffffc0123000: refused by name, not named
verify: 0x1000 -> <not an address: the percpu offset namespace (this image's per-cpu symbols occupy 0x0..0x35000) -- ...>
verify: PASS refusal of 0x1000: refused by name, not named
verify: 8/8 anchors pass
```

The anchors are independent of the parse:

- **the ELF's section VMAs and the boundary symbols at them** (`_text`, `__start_rodata`,
  `__bss_start` — the three anchors `docs/testing.md` §2.7 and `results/x86-64-rsplow.md` §4 already
  used; `.bss`'s VMA holds three symbols, so the check is membership, not equality);
- **`__stack_chk_fail` at `0xffffffff81a3d470`**, whose independence is on the caller side: the tool
  scans `.text` for `call rel32` targeting that address and finds **8914** sites (first at
  `0xffffffff81000dd3`) — the image's own code, read without kallsyms. A guard function called 8914
  times is the stack-protector fail routine, and the table must name it there;
- **the #PF entry**: `0xffffffff81c00c02` must name a symbol containing `page_fault`, within 0x100
  bytes of the entry;
- **the three refusals**, which must be refusals.

**The anchors can fail — proved by mutation, not asserted** (mutants run from `/tmp` copies; the good
file untouched):

| mutant | effect | result |
|---|---|---|
| **A: every symbol shifted by one** (`addrs[1:] + [addrs[-1] + 0x1000]` — the failure mode the docstring names) | names land one symbol off | `FAIL .text boundary symbol … names __per_cpu_end, startup_64, _stext`; `FAIL __stack_chk_fail … names get_cpu_entry_area`; `FAIL #PF entry … names asm_exc_int3+0x22`; **4/8, rc 1** |
| **B: the domain guard removed** (`why = None`) | the pre-fix behaviour | `0xffffffffc0123000 -> __init_scratch_end+0x3b723000`, `0x1000 -> cpu_debug_store+0x0`, 3 refusal checks FAIL; **5/8, rc 1** |

Mutant B reproduces the reviewer's three MEASURED wrong names exactly, which is the asymmetry: the
guard is what stands between the tool and those lines.

---

## 4. The retraction (`results/x86-64-watchphys.md`)

The report's §1 headline — a `V86C_WATCH` on a **physical** range "saw **nothing** from any long-mode
store" — and `5178acd`'s "a watch set on a physical frame could never match one" are **refuted by the
tree's own record**, and the file now says so twice: a CORRECTION section at the top, and a short note
in place where §1's claim sits. The original text is untouched; nothing was reworded to look right.

What the correction records (READ/MEASURED):

- The physical write routers already carried the hooks **at that unit's own baseline** `592687f`:
  `src/cpu/memory.c:526/545/571/593` (plus `:687`), landed in `55ec3cb`. MEASURED with
  `git show 5178acd~1:src/cpu/memory.c | rg -n v86c_dbg_watch_p` → the same five lines.
- Every long-mode store reaches them (READ, `git show 5178acd~1:src/cpu/paging.c`): `safe_write8_addr`
  → `write8`, `safe_write16_addr` → `write16`, `safe_write32_addr` → `safe_write32_addr_local` →
  `write32`, `safe_write64_addr` → `write32` twice.
- And it was measured speaking from the long-mode guest on that baseline —
  `results/x86-64-producer.md:30-31`: `WATCH P 0x2413d60 width=4 value=0xffffffffc0000000
  rip=0xffffffff8ae9ef45`.
- What `5178acd` genuinely added, recorded honestly: **the linear address the physical hit came
  from** (`WATCH V <va> -> P <phys>` on one line), which is what makes a physical hit attributable —
  the pairing `results/x86-64-setcc.md` read a truncated-range store against (its decisive lines are
  the `WATCH F` line plus `cr2`, which the pairing made interpretable). Then the follow-up unit made
  one store print one line instead of 2-4.
- §2's straddle claim corrected per `results/x86-64-instrholes.md` §1: the qword straddle **did**
  reach the physical router (`safe_write32_addr_local` ends in `write32`, whose hook fires; MEASURED
  there, mutant C, `WATCH P 0x1000 width=4 …` for the second frame). What it did not carry was the
  **linear** address, and it printed two lines for one store.
- "The 32-bit path was fine" corrected, source checked before asserting: `jit_enabled` defaults
  **true** (`src/cpu/jit_mem.c:42`) and `jitcg_emit_mem_store` (`src/cpu/jit_codegen.c:404-421`)
  writes natively with no watch hook, so a TLB-hit RAM store in 32-bit mode carried no watch at all
  (~91% of the 32-bit bench). Long mode is unaffected: `src/cpu/uop_cache.c:1332-1336` sends every
  long-mode batch to `interpreted_run`.

`docs/testing.md` §2.7 was updated with the domain, the refusal forms, the convention-evidence
requirement and the exact `--verify` invocation.

---

## 5. Gates (exit codes, one pass, at the end)

| gate | rc | evidence |
|---|---|---|
| `python3 -c "import ast,sys; ast.parse(open('tools/kallsyms.py').read())"` | **0** | parses |
| `python3 tools/kallsyms.py /tmp/vmlinux.bin --verify` | **0** | 8/8 anchors, the three refusal lines shown (§3) |
| `make` | **0** | `[+] src/cpu/interpreter_gen.c … (unchanged)`, `uop_gen.c … (unchanged)` |
| `make selftest` | **0** | `selftest: all suites green`; `cpu_m1c_selftest: 438 checks, 0 failures`, hw 975/0, uop 7,934,363/0, uop-exec 102/0, jit 68/0, jit-codegen 3570/0 |

No guest run was made and no CPU/solver behaviour changed, as the unit required.

---

## 6. One sentence

**A naming tool that cannot place an address now says which range it is in instead of printing the
nearest symbol's name, proves itself on anchors the image pins down before anyone quotes it — and the
report that claimed the physical watch could not speak has been retracted in place, with what the fix
actually added recorded honestly.**
