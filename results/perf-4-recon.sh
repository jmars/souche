#!/bin/sh
# perf-4-recon.sh -- PERF-4/M2's G1: THE INVALIDATION RECONCILIATION, and PERF-FIX-3's G2:
# the same gate with its BLIND SPOT CLOSED.
#
# The gate is the plan's (results/perf-2-blockcache-plan.md ss4.1): "rg every mem8 /
# *_no_mmap_or_dirty_check writer against that table; THE GATE IS THE EMPTY DIFF BETWEEN THE
# TWO LISTS".  A missed write site is stale code and a silently wrong guest, and the corpus
# cannot see it (no nasm test self-modifies), which is why the SMC selftest exists separately.
#
# WHY THIS SCRIPT CHANGED (PERF-FIX-3): the M2 version of this gate was satisfied by the tree
# while the 165 read-modify-write call sites -- every `add/sub/xor/... [mem]` instruction, the
# SAFE_READ_WRITE8/16/32 family -- wrote guest RAM with NO invalidation and NO SMC abort.  The
# writer was a MACRO, and the gate's list B matched `write*()` CALLS; a macro invocation is
# not one.  So the gate is now:
#
#   * list B also scans src/cpu/paging.h and matches the SAFE_READ_WRITE* invocations
#     (the shape that beat it), and
#   * section C is a MECHANICAL CHECK: every writer hit in src/ must either be exempt for a
#     STATED reason (a writer's own definition, a header declaration, a loader-time writer) or
#     have a `uop_invalidate_*` hook within the preceding 8 lines.  The script EXITS NONZERO on
#     any unexempted hit -- it is a gate, not a printout.
#
# Usage: sh results/perf-4-recon.sh [--out FILE]
set -u
cd "$(dirname "$0")/.." || exit 2
OUT=results/perf-fix-3/reconciliation.txt
[ "${1:-}" = "--out" ] && OUT="$2"
mkdir -p "$(dirname "$OUT")"

{
echo "== A. THE HOOK SITES (every uop_invalidate_* call in the port) =="
rg -n 'uop_invalidate_(page|range)' src/ --glob '!*selftest*' | grep -v '^\s*$'

echo
echo "== B. THE GUEST-RAM WRITE SITES (the plan's other list) =="
echo "   scope: src/cpu/memory.c src/cpu/paging.c src/cpu/paging.h src/cpu/string.c"
echo "          src/main.c src/hw/*.c   [paging.h + the SAFE_READ_WRITE pattern ARE PERF-FIX-3:"
echo "          the macro family was the whole class the M2 scope could not see]"
rg -n 'mem8\[[^]]*\] *=|mem(set|cpy|move)\( *mem8|mem8 *\+.*, *(value|&)|write(8|16|32|64|128)_no_mmap_or_dirty_check\(|mem(set|cpy)_no_mmap_or_dirty_check\(|SAFE_READ_WRITE(8|16|32)\(' \
   src/cpu/memory.c src/cpu/paging.c src/cpu/paging.h src/cpu/string.c src/main.c src/hw/*.c | grep -v 'mem8 + addr, sizeof'

echo
echo "== C. THE CLASS CHECK (PERF-FIX-3/BLOCKER-1): every writer is hooked or exempt =="
python3 results/perf-fix-3-recon-check.py src
echo "CHECK_RC=$?"
echo
echo "== D. THE LOADER-TIME WRITERS (before the guest runs; hook inert by construction) =="
echo "   src/main.c      : the multiboot/ELF loader and the boot firmware/option-ROM copies"
echo "   src/hw/kernel.c : the bzImage loader's own static write_blob + the cmdline byte"
echo "   src/cpu/memory.c zero_memory (memset) -- called by main.c's loader and the selftests"
echo "   (the device DMA paths are NOT here: dma.c:240, ide.c:829 and virtio.c:1409 all call"
echo "    the CPU's write_blob, which is hooked -- see list A)"

echo
echo "== E. THE SHAPES SCANNED, AND THIS GATE'S BLIND SPOTS (the honest part) =="
cat <<'TXT'
   SCANNED, with the measurement that settles each:
   1. direct calls      -- the six *_no_mmap_or_dirty_check writers and every memcpy/memset
                           into mem8: section C checks each hit's hook window.
   2. FUNCTION-LIKE MACROS -- the shape that beat the previous gate.  paging.h's
                           SAFE_READ_WRITE8/16/32 are now in list B AND their bodies are
                           checked by C (a macro's arm is where the hook must sit).  The tree
                           has no other multi-line macro that writes guest RAM (MEASURED: rg
                           '#define' over src/cpu/*.h -- only these three contain a write).
   3. STATIC INLINE FUNCTIONS in headers -- MEASURED: none writes guest RAM (the only inline
                           in memory.h is in_mapped_range, a predicate).
   4. ALIASES            -- MEASURED: no `#define <name> write*`/`*_no_mmap*` alias exists
                           (rg '#define\s+\w+\s+(write|mem)' -> empty), and no writer is
                           reached through a function pointer that writes guest RAM: the
                           function-pointer tables are src/hw/io.c's io_mmap DEVICE hooks
                           (device registers, vga_mem8) and the CPU's memory_devices hooks;
                           the device paths that DO write guest RAM call the CPU's
                           write_blob/write16/write32 (hooked).
   5. POINTER ALIASES    -- MEASURED: the only pointer handed out into guest RAM is
                           src/hw/virtio.c:69 virtio_guest_read_blob, typed `const uint8_t *`
                           and used only as a memcpy SOURCE; no `uint8_t *p = ... mem8`
                           store-through and no `&mem8[` exists outside memory.c's own
                           definitions (rg, both empty).
   BLIND SPOTS -- what section C cannot see, stated so a reader can attack it:
   * A writer reached ONLY through a function pointer (a new device hook that stores into
     mem8 directly) is not in any writer list; the device inventory is manual (see 4).
   * A pointer alias into guest RAM written through after the fact (a `uint8_t *` parameter
     that is mem8 + n at the call site) is invisible to a textual scan.
   * `results/` probes and the selftest files (.c files excluded from C by name) write mem8
     by hand; they are not the shipped machine -- the library default OFF exists for exactly
     that reason (results/perf-4-uop-exec.md ss3).
TXT
} > "$OUT" 2>&1

rc=$(sed -n 's/^CHECK_RC=//p' "$OUT" | tail -1)
[ -z "$rc" ] && rc=2
echo "--- results/perf-4-recon.sh: wrote $OUT"
sed -n '/== C\./,/CHECK_RC=/p' "$OUT" | grep -E '^   (FAIL|EXEMPT-CLASS|OK:|checked)' | head -8
echo "   FAIL lines: $(grep -c '^   FAIL' "$OUT")"
echo "--- the class check's exit code: $rc  (0 = every writer hooked or exempt)"
exit "$rc"
