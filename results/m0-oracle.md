# M0 — scaffold + the reference oracle (findings)

**Unit:** `HANDOFF=v86c`, unit M0. **Deliverable:** the project scaffold and the reference
ORACLE for the v86-to-C port — *not* the emulator.

**Bottom line (MEASURED):** the oracle works. The published reference implementation (npm
`v86` 0.5.471) boots the flat test image, produces a canonical machine-state transcript, two
consecutive runs are byte-identical, and the transcript matches an independent model of the
test program on all 8 GPRs, EFLAGS, EIP, the executed-instruction count and the sha256 of the
4 KiB RAM window. The APE builds and runs. All four gates pass; **no gate was skipped and no
deliverable is a stub**.

---

## 1. Environment (MEASURED)

| Tool | Version |
| --- | --- |
| node | v26.8.1 |
| npm | 11.19.0 |
| reference implementation | npm package `v86`, version **`0.5.471`** (package version string `0.5.471+g94faf3d`), pinned in `oracle/package.json` + `oracle/package-lock.json`, integrity `sha512-2TuNIpJU6sYYKnq/xR6F9Epi59DVBYpmkcV1LzLgNwHWxs/uFPB4szeYdsbhn6P8kV5YOlC6+7xSHEOQ2UiYdQ==` |
| python3 | 3.13.14 |
| GNU as / ld / objcopy | binutils 2.45.0.20251103 |
| cosmocc | GCC 14.1.0 (`/home/jaye/.local/cosmo/bin/cosmocc`, extracted from `/tmp/cosmocc-dl/cosmocc.zip` with python3's `zipfile` — the host has no `unzip`) |

The npm build **is** drivable from node — MEASURED, not assumed: the fallback the brief
allowed (importing `v86/src/main.js` directly) was not needed. Two node-specific facts had to
be handled, both measured:

* the release bundle resolves its wasm through `fetch()` of a relative path, which does not
  work in node; `V86Options.wasm_fn` (`v86.d.ts`) is the node-safe entry point;
* a node `Buffer` is a view into a shared pool, so `readFileSync(...).buffer` for a 283-byte
  image is **not** the image — an exact `ArrayBuffer` slice is required. (The reference's own
  `v86/tests/nasm/run.js:261` passes `.buffer` directly and gets away with it only because
  those images exceed the pool size.)

## 2. What was built

```
README.md                  the project statement, scope, NON-GOALS and the ORACLE CONTRACT
Makefile                   `make` (cosmocc APE) / `make oracle` / `make determinism`
src/main.c                 placeholder: compiles, prints the banner, keeps the argv surface
gen/README.md              placeholder for the C code generator
oracle/run.mjs             the reference harness (node), canonical transcript to stdout
oracle/package.json        pins v86 0.5.471
oracle/package-lock.json   the lockfile
tests/test.asm             the flat 386-level test program (GNU as, intel syntax)
tests/test.ld              links it flat at 0x100000
tests/test.img             the built image (283 bytes)
tests/model.py             independent model of the program's final state (the gate)
tests/check_transcript.py  diffs the transcript against the model
results/transcript.txt     the canonical transcript (the deliverable artifact)
results/transcript.run1.txt, results/transcript.run2.txt   the determinism pair
results/expected.json      the model's expected state
results/gates.txt, results/gates-g4.txt   raw gate output
```

Nothing under `./v86` was modified (MEASURED: `git -C v86 status --porcelain` is empty and the
clone is still at its original commit; it is read-only input). The tree is not a git repository and was left that way.

## 3. The gates — raw evidence

```text
=== G1: make oracle  (from scratch: no build/) ===
python3 tests/model.py --out results/expected.json > /dev/null
node oracle/run.mjs tests/test.img 1000000 > results/transcript.txt
python3 tests/check_transcript.py results/expected.json results/transcript.txt
check_transcript: transcript matches tests/model.py (8 GPRs, EFLAGS, EIP, 561 instructions, 4096-byte window sha256)
G1 RC=0

=== G2: make determinism ===
node oracle/run.mjs tests/test.img 1000000 > results/transcript.run1.txt
node oracle/run.mjs tests/test.img 1000000 > results/transcript.run2.txt
diff -u results/transcript.run1.txt results/transcript.run2.txt
determinism: two consecutive oracle runs are byte-identical
G2 RC=0

=== G4: make (cosmocc APE, from scratch) ===
/home/jaye/.local/cosmo/bin/cosmocc -std=gnu11 -O2 -Wall -Wextra -o build/v86c src/main.c
G4 make RC=0
-rwxr-xr-x. 1 jaye jaye 599140 Oct 11 21:31 build/v86c
0000000   M   Z   q   F   p   D   =   '  \n  \n  \0 020  \0 370  \0  \0
od: write error: Broken pipe
v86c M0 -- v86-to-C port (scaffold; the CPU core is not implemented yet)
scope: x86 PC emulator, interpreter only, 386-level instructions
non-goals: JIT, wasm, multicore, 64-bit mode, sound, NE2K, floppy, modem, 9p
usage: ./build/v86c <image> <instruction-limit>
       (no arguments prints this banner)
G4 run RC=0

=== expected vs transcript (model) ===
{
  "cpu": {
    "eflags": "0x00000046",
    "eip": "0x00100119",
    "gpr": {
      "eax": "0x10203040",
      "ebp": "0x00102000",
      "ebx": "0x0f0f00f1",
      "ecx": "0x00000010",
      "edi": "0x00000010",
      "edx": "0x00000000",
      "esi": "0xc487eb71",
      "esp": "0x00102000"
    }
  },
  "instruction_count": 561,
  "memory_window": {
    "address": "0x00101000",
    "bytes": 4096,
    "sha256": "9b4639eca15ccfca5c91cb48bc6ae3fc1dd2826fc86c88ee290eaeb16fc4d6fb"
  }
}

=== host tools ===
v26.8.1
cosmocc (GCC) 14.1.0
Python 3.13.14
GNU assembler (GNU Binutils; openSUSE Tumbleweed) 2.45.0.20251103-4
```

## 4. G3 — non-triviality: the transcript reflects the computation

Raw transcript (`results/transcript.txt`, exactly as written by `oracle/run.mjs`):

```json
{
  "cpu": {
    "control_registers": {
      "cr0": "0x00000001",
      "cr2": "0x00000000",
      "cr3": "0x00000000"
    },
    "eflags": "0x00000046",
    "eip": "0x00100119",
    "gpr": {
      "eax": "0x10203040",
      "ebp": "0x00102000",
      "ebx": "0x0f0f00f1",
      "ecx": "0x00000010",
      "edi": "0x00000010",
      "edx": "0x00000000",
      "esi": "0xc487eb71",
      "esp": "0x00102000"
    },
    "segments": {
      "cs": {
        "base": "0x00000000",
        "limit": "0xffffffff",
        "selector": "0x0008"
      },
      "ds": {
        "base": "0x00000000",
        "limit": "0xffffffff",
        "selector": "0x0010"
      },
      "es": {
        "base": "0x00000000",
        "limit": "0xffffffff",
        "selector": "0x0010"
      },
      "fs": {
        "base": "0x00000000",
        "limit": "0xffffffff",
        "selector": "0x0010"
      },
      "gs": {
        "base": "0x00000000",
        "limit": "0xffffffff",
        "selector": "0x0010"
      },
      "ss": {
        "base": "0x00000000",
        "limit": "0xffffffff",
        "selector": "0x0010"
      }
    },
    "x87": {
      "control_word": "0x037f",
      "data": [
        "0x000000000x00000000",
        "0x000000000x00000000",
        "0x000000000x00000000",
        "0x000000000x00000000",
        "0x000000000x00000000",
        "0x000000000x00000000",
        "0x000000000x00000000",
        "0x000000000x00000000"
      ],
      "stack_empty": "0xff",
      "stack_ptr": "0x0000",
      "status_word": "0x0000",
      "tag_word": "0xffff"
    }
  },
  "image": {
    "bytes": 283,
    "entry_address": "0x00100020",
    "load_address": "0x00100000",
    "sha256": "1a597ac159554271a380493a19aa52ca0b65481cdb6c979c407684c9c96e737a"
  },
  "instruction_count": 561,
  "instruction_limit": 1000000,
  "memory_window": {
    "address": "0x00101000",
    "bytes": 4096,
    "sha256": "9b4639eca15ccfca5c91cb48bc6ae3fc1dd2826fc86c88ee290eaeb16fc4d6fb"
  }
}
```

And the independent model (`tests/model.py` → `results/expected.json`), which `make oracle`
requires the transcript to match leaf-for-leaf:

```json
{
  "cpu": {
    "eflags": "0x00000046",
    "eip": "0x00100119",
    "gpr": {
      "eax": "0x10203040",
      "ebp": "0x00102000",
      "ebx": "0x0f0f00f1",
      "ecx": "0x00000010",
      "edi": "0x00000010",
      "edx": "0x00000000",
      "esi": "0xc487eb71",
      "esp": "0x00102000"
    }
  },
  "instruction_count": 561,
  "memory_window": {
    "address": "0x00101000",
    "bytes": 4096,
    "sha256": "9b4639eca15ccfca5c91cb48bc6ae3fc1dd2826fc86c88ee290eaeb16fc4d6fb"
  }
}
```

### The program's computation, stated, and what the transcript shows

The image is `tests/test.asm`; the table is the final state the *program* must leave, derived
from the source, next to the transcript's value. All rows MEASURED (transcript) and checked
against the model.

| # | What the program does | Expected | Transcript |
| --- | --- | --- | --- |
| 1 | counted loop `eax = sum(1..100)` | `0x000013ba` (5050) — stored to `[0x101000]`, then `eax` is reused in step 6 | eax final `0x10203040` (step 7 popped it); `[0x101000]` inside the window digest |
| 2 | ALU chain on `ebx`: `sub`, `add`, `and`, `or`, `xor`, `not`, `neg` from `0xFFFF0000` | `0x0f0f00f1` | `"ebx": "0x0f0f00f1"` ✓ |
| 3 | `shl 1`, `ror 3`, `sar 5`, `shr 4`, `rol 8` from `0x80000001` | `0x20000000`, `CF=1` on the first shift | stored to `[0x101004]` and inside the digest; `ecx` is then reused as a loop counter (`0x10`) |
| 4 | 16-bit `mov`/`add` + `es:` override + `movzx`/`movsx` + `lea edi, [edi+4*edi+8]` | `ax = 0x2221`, `edi = 5*0x2221+8 = 0xaaa d`, `dl = 0xa5` | stored to `[0x10100c]`, `[0x101010]`, `[0x101012]` (all inside the digest) |
| 5 | LCG table fill `esi = esi*1103515245 + 12345` ×16 into `[0x101040 .. 0x10107f]` | last value `0xc487eb71` | `"esi": "0xc487eb71"` ✓, and the 16 dwords are inside the window digest |
| 6 | checksum loop over the table: `edx = rol(edx xor (edx+eax), 1)` ×16 | `0` — the two operators cancel over this table | `"edx": "0x00000000"` ✓ and `[0x101014] = 0` inside the digest |
| 7 | `push`/`push`/`pop`/`pop`, snapshot `esp` | `ebp = esp = 0x00102000`, the pushed dwords remain at `[0x101ff8..0x101fff]`, inside the window | `"ebp"/"esp": "0x00102000"` ✓, stack residue inside the digest |
| 8 | final `cmp ebp, 0x102000` then `hlt` | `ZF=1, PF=1, CF=OF=SF=AF=0`, IF=0, DF=0 → `0x00000046` | `"eflags": "0x00000046"` ✓ |
| 9 | `hlt` at `0x100119` (objdump) | EIP `0x00100119` | `"eip": "0x00100119"` ✓ |
| 10 | executed instruction count, as modelled by `tests/model.py` | `561` = prologue 9 + sum section 302 + ALU chain 9 + shifts 7 + 16/8-bit 10 + LCG 99 + checksum 116 + push/pop 7 + `cmp`/`hlt` 2 | `"instruction_count": 561` ✓ — **exact**, and it is why the whole program fits in one `main_loop` chunk |
| 11 | the 4 KiB window `[0x101000, 0x102000)` | mostly zeros, but 8 result dwords + 16 LCG dwords + 8 stack bytes are nonzero | sha256 `9b4639eca15ccfca5c91cb48bc6ae3fc1dd2826fc86c88ee290eaeb16fc4d6fb` ✓ |

**An all-zero transcript would fail gate 3** (the model's window digest is not the digest of
zeros, and seven of the eight GPRs are nonzero), and a transcript whose window were not the
program's would fail the digest comparison. The measured digest is a function of the loop trip
counts, so it cannot be produced by a decoder that merely walks the bytes.

### Honest scope (MEASURED)

`tests/model.py` shares the program's constants with `tests/test.asm` — a constant
mistranscribed into both would cancel out — but implements the x86 semantics independently. It
therefore tests the transcript against the program's arithmetic, flags, memory effects and
instruction count; it is **not** a proof of x86 semantics. It is a gate, not an oracle of
oracles.

## 5. Divergences from the brief (explicit, each with its reason and its evidence)

1. **Boot path: not BIOS + `hda`.**
   *Brief:* fixed BIOS, `hda` pointing at the image.
   *Why it cannot work:* the test image is a flat program linked at `0x100000`, not a bootable
   disk — no `0x55AA` MBR signature, no real-mode loader. MEASURED: the reference refuses
   multiboot when a BIOS is present (`v86/src/cpu.js:1291-1304`), and its own CPU tests
   therefore run with no BIOS (`v86/tests/nasm/run.js:261`).
   *What was done instead:* the image is loaded by **the reference's own multiboot loader**
   with the address table (flags bit 16) — the machine bring-up is the reference's code, and
   the flat-image equivalence the C side must reproduce is stated in README.md §4 (a verbatim
   copy to `0x100000` plus a listed fixed state).
2. **Instruction count: `HLT` + a hard limit, not an exact instruction count.**
   *Why:* MEASURED — `CPU.prototype.main_loop` executes in chunks of `LOOP_COUNTER = 100003`
   (`v86/src/rust/cpu/cpu.rs:293`) and returns only at a chunk boundary; the wasm exposes no
   single-step entry point. An exact stop is not expressible through the reference's public
   API.
   *What was done instead:* `run.mjs <image> <limit>` stops at the guest's `HLT`, bounded by
   the limit, and reports the count actually reached. For M0's image the whole program fits in
   one chunk (561 instructions), so chunking does not bias the transcript; the count is exact
   and equals the model's.
3. **Geometry of the hashed window: `[0x101000, 0x102000)`.** The brief said "linked at
   `0x100000`, mirroring BSS=0x100000 / STACK_TOP=0x102000". In the reference those two are
   not the same region: the code is linked at `0x80000` and BSS is at `0x100000`
   (`v86/tests/nasm/create_tests.js:131`), so the reference hashes data+stack, not code. With
   the code at `0x100000` as the brief requires, the window is shifted by `0x1000` — the size
   of the code — and `STACK_TOP` stays `0x102000`. The window is the image's own `.bss` base,
   as the brief asked ("choose the window from the image's own linkage address").
4. **"no VGA": not achievable without patching the reference.** `VGAScreen` is constructed
   unconditionally (`v86/src/cpu.js:1193`). It is inert here: no listeners, no output, no
   screen read, and its memory is outside the hashed window.

## 6. Findings for the later units (durable)

1. **The reference's public API has no fixed-instruction-count stop.** Any C-side gate that
   says "run N instructions" must be reconciled with the reference, which can only stop at a
   chunk boundary or at `HLT`. The M0 contract sidesteps this by making the image halt; if a
   later unit needs a run that does not halt, the contract needs a re-think *then*, not after
   the C core exists.
2. **The transcript must be produced after the guest halts, not while it runs.** `get_eflags()`
   materializes v86's lazy flag cache (`v86/src/rust/cpu/cpu.rs:1911`); reading `flags[0]`
   directly would report the stale cached word. The oracle uses `get_eflags()`, and the C side
   has no such cache to get wrong — a difference worth remembering when a mismatch is
   debugged.
3. **`dbg_assert` is compiled out of the release build.** The npm build is the *release*
   wasm (`v86.wasm`, 2.0 MB); `v86-debug.wasm` (12.6 MB) and `libv86-debug.mjs` ship alongside
   it. Release is the right reference for the port (no assertion side effects), but it means
   the reference will **not** complain about conditions the source asserts — e.g. the
   multiboot flag mask check. Do not read release silence as a satisfied invariant.
4. **Host tool gaps that shaped the build:** no `unzip` (python3 `zipfile` used), no `nasm`
   (GNU as `.intel_syntax noprefix` used, as the brief allowed), no `java`/wasm target (the
   reference is taken from npm prebuilt, as the brief directed). `file` reports the APE as a
   `DOS/MBR boot sector` and `od` shows `MZqFpD` — that *is* the APE polyglot signature, not a
   build failure.

## 7. Not done (deliberately, this unit)

* the emulator: `src/main.c` is a placeholder and `gen/` is empty — M0 is the scaffold and the
  oracle;
* no JIT, wasm, multicore, 64-bit mode, sound, NE2K, floppy, modem or 9p work (NON-GOALS in
  the README);
* no paging and no interrupts in the M0 test program — the image runs in flat protected mode
  with `IF=0`, so the transcript does not yet cover page-walk or interrupt-delivery state;
* no git commit, per the brief.
