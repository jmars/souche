# x86-64: `__startup_64`'s fixup never runs — not even its high-half entry is written

**Status: MEASUREMENT ONLY. The question the bisect unit asked is answered for the *what*: the kernel
never writes `early_top_pgt` at all — neither the low entry the bisect unit watched nor the HIGH entry
the kernel's own code needs. The *why* is narrowed to one named question and one instrument.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The test, and why it is the right one (READ + MEASURED)

`results/x86-64-bisect.md` watched **`0x30EA000`** — `early_top_pgt[0]`, the LOW half — and found only
the decompressor's writes. **That is not by itself damning:** a kernel running at
`0xFFFFFFFF9C27…` lives in the **HIGH** half, which is `early_top_pgt[511]` (`0x30EA000 + 511*8 =
0x30EAFF8`). If the fixup had run, *that* entry would carry a present, kernel-owned PDPT pointer, and
the low entry would be filled by the identity-map copy.

So the same instrument on the high entry:

    $ V86C_WATCH=0x30eaff8:8 V86C_RING=8 V86C_DUMP_ON_STOP=1 …
    v86c dbg: WATCH V 0x30eaff8 width=1 value=0x18 rip=0x4f399c0
    v86c dbg: WATCH V 0x30eaff9 width=1 value=0x00 rip=0x4f399c0
    v86c dbg: WATCH V 0x30eaffa width=1 value=0x00 rip=0x4f399c0
    v86c dbg: WATCH V 0x30eaffb width=1 value=0x00 rip=0x4f399c0
    v86c dbg: WATCH V 0x30eaffc width=1 value=0x05 rip=0x4f399c0
    v86c dbg: WATCH V 0x30eaffd width=1 value=0x00 rip=0x4f399c0
    v86c dbg: WATCH V 0x30eaffe width=1 value=0x10 rip=0x4f399c0
    v86c dbg: WATCH V 0x30eafff width=1 value=0x00 rip=0x4f399c0

**Every byte of the high entry is written by `rip=0x4F399C0` — the decompressor's output flush — and by
nothing else.** The bytes are image data (`0x0010000500000018`), whose low bit is **0**: the entry is
**not present**. So:

* **the kernel never writes `early_top_pgt` at all** — not the low entry, not the high one;
* therefore **`__startup_64`'s page-table fixup does not run** (it is the only writer of those entries
  in the kernel's early path);
* and the kernel is nevertheless executing at `0xFFFFFFFF9C27…`, an address whose PML4 entry is *not
  present* — so those fetches are being served by the **64-bit TLB**, i.e. by translations cached
  before the tables were replaced. That is the same stale-TLB shape `results/x86-64-firstfault.md`
  recorded when a hand walk and the port's walk disagreed for the stack page.

## 2. What this rules out, and what it leaves

**Ruled out (each was a candidate the brief listed):**

* **(b) a store whose address the port computes wrongly** — there is no store at all, so no address was
  computed;
* **(c) a store whose width or segment is wrong** — likewise;
* **a mis-sized or mis-addressed write to the watched entries** — the watch is byte-granular and would
  have shown *any* writer.

**What is left is (a) or (d):** the port either **never reaches `__startup_64`** (the control flow
diverges earlier — the ring's `ret`-thunk entries are the kernel's C code, so it reaches *C*, but not
necessarily the fixup), or it reaches the routine and **branches past the fixup** on a flag or a compare
the port gets wrong (the S8-rotate / `jz` class).

## 3. The next measurement, named

**Does the port execute `__startup_64` at all?** The routine's entry is reachable from the handover
target the port logs; its first instructions are a `push`-heavy prologue and its fixup begins with a
`lea` of `early_top_pgt`. The instrument is the toolkit's ring, widened to keep more boundaries, plus a
`V86C_WATCH` on the **`early_top_pgt` page as a 4 KiB window** with a *kernel-rip filter* (the watch
already prints the rip, so the filter is a `grep -v 0x4f3` on its output): a single run then answers
"did any kernel instruction write anywhere in that page", and if none did, the ring's eip sequence
around the stop names the branch the port took instead.

## 4. Verification (MEASURED; **no functional change**)

    make selftest                          rc 0 — all suites green
    tests/gate-x86-64.sh                   rc 0 — 12 passed, 0 failed
    tests/gate-m2-5-determinism.sh         rc 0 — 0 FAILING, both runs 4,000,096,757

`git status` shows nothing under `src/`, `gen/` or `tests/`. No pin moved. The rest of the battery was
not re-run because nothing functional changed — the QEMU unit's full battery on the same tree is the
record for it.

## 5. Files touched

| file | change |
|---|---|
| — | **none.** One port run with `V86C_WATCH` on `early_top_pgt[511]`. |
