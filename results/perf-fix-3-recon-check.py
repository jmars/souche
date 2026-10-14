#!/usr/bin/env python3
"""perf-fix-3-recon-check.py -- PERF-FIX-3's G2: the invalidation gate with teeth.

THE PROBLEM IT CLOSES (results/perf-2r-review.md, BLOCKER-1): the M2 reconciliation was a
PRINTOUT of two lists plus a human's judgement that they matched, and it silently missed a
whole writer family -- src/cpu/paging.h's SAFE_READ_WRITE8/16/32 macros, 165 call sites of
read-modify-write instructions.  The writer was a MACRO, so the `write*()` call regex never
saw it.  A gate that only covers the shapes its author thought of is not a gate.

WHAT THIS DOES: every line in src/ that writes guest RAM -- a `*_no_mmap_or_dirty_check`
call, a direct `mem8[..] =`, or a memcpy/memset/memmove into mem8 -- is either

  * EXEMPT, with a printed reason (a writer's own definition, a header declaration, the
    LOADER-TIME class: code that runs before the guest's first instruction, when the cache's
    tables are empty and the hook is a no-op either way), or
  * required to have a `uop_invalidate_*` hook within the 8 preceding lines,

and the process EXITS NONZERO if any hit is neither.  Run it as `... <srcdir>`; it prints a
verdict line per hit and a summary.

WHAT IT CANNOT SEE (stated, not hidden): a writer reached only through a function pointer, or
a pointer alias into guest RAM stored through after the call site.  Both are measured empty in
this tree (see results/perf-4-recon.sh section E) and both need a human inventory, which is
why section E exists.
"""
import os
import re
import sys

WRITER_CALL = re.compile(
    r"\b(write(?:8|16|32|64|128)_no_mmap_or_dirty_check|mem(?:set|cpy)_no_mmap_or_dirty_check)\s*\(")
# PERF-3R NICE-TO-HAVE-1: the simple `=` arm missed every COMPOUND-assignment store on guest
# RAM (`mem8[i] &= v`, `|=`, `+=`, `-=`, `^=`, `<<=`, `>>=`, `++`, `--`), which IS a write but
# is not in the list.  MEASURED: with the old pattern a synthetic `mem8[i] &= v` in a
# non-exempt file passed the gate silently (rc 0); with this alternation the same tree is
# reported and the gate exits 1, while the real tree stays clean (rc 0).
WRITER_DIRECT = re.compile(
    r"mem8\s*\[[^\]]*\]\s*(?:=(?!=)|\+=|-=|\|=|&=|\^=|<<=|>>=|\+\+|--)"
    r"|mem(?:set|cpy|move)\s*\(\s*mem8")
# the VGA LFB is a SEPARATE allocation (memory.h:80), not guest RAM, and uop_block_decode
# refuses a block there -- it must not be reconciled against the guest-RAM hook table.
VGA_LFB = re.compile(r"vga_mem8\s*\[")
HOOK = re.compile(r"uop_invalidate_(?:page|range)")
DEFINITION = re.compile(
    r"^\s*(?:static\s+)?(?:inline\s+)?void\s+\w*no_mmap_or_dirty_check\s*\(")
DECLARATION = re.compile(r"^\s*void\s+\w*no_mmap_or_dirty_check\s*\([^;]*\)\s*;")
# a function-header line at column 0 (used to name the enclosing function of a hit)
FUNC_HEAD = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \t*]*\([^;]*\)\s*$|^[A-Za-z_][A-Za-z0-9_ \t*]*\([^;]*$")

WINDOW = 8          # how close the hook must be to the write
LOADER_TXTS = ("main.c", "kernel.c")  # the two loader translation units
LOADER_FUNCS = ("zero_memory", "loader_write_blob")  # in-tree writers that run PRE-GUEST


def definition_bodies(lines):
    """indices of the lines INSIDE a *_no_mmap_or_dirty_check definition (brace-tracked):
    the body's own memcpy/memset IS the store the hook guards, so it needs no hook."""
    inside = set()
    i = 0
    while i < len(lines):
        if DEFINITION.match(lines[i]):
            depth = 0
            j = i
            while j < len(lines):
                depth += lines[j].count("{") - lines[j].count("}")
                if depth <= 0 and "{" in "".join(lines[i:j + 1]):
                    break
                j += 1
            inside.update(range(i, min(j + 1, len(lines))))
            i = j
        i += 1
    return inside


def loader_func_bodies(lines):
    """indices inside a function from LOADER_FUNCS (they run before the guest's first
    instruction, so the cache's tables are empty and the hook is a no-op either way)."""
    inside = set()
    for i, line in enumerate(lines):
        m = re.match(r"^[A-Za-z_][A-Za-z0-9_ \t*]*?\b(\w+)\s*\(", line)
        if not m or m.group(1) not in LOADER_FUNCS:
            continue
        depth = 0
        j = i
        while j < len(lines):
            depth += lines[j].count("{") - lines[j].count("}")
            if depth <= 0 and "{" in "".join(lines[i:j + 1]):
                break
            j += 1
        inside.update(range(i, min(j + 1, len(lines))))
    return inside


def enclosing_function(lines, idx):
    """the identifier of the function whose body contains lines[idx] (best effort, textual)"""
    i = idx
    while i >= 0:
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_ \t*]*?)\s*\([^;]*\)\s*$", lines[i].rstrip())
        if m:
            name = m.group(1).strip().split()[-1].lstrip("*")
            return name
        i -= 1
    return "?"


def main():
    if len(sys.argv) != 2:
        print("usage: perf-fix-3-recon-check.py <srcdir>")
        return 2
    root = sys.argv[1]
    hits = []
    for dirpath, _dirs, files in os.walk(root):
        for fn in sorted(files):
            if not fn.endswith((".c", ".h")):
                continue
            path = os.path.join(dirpath, fn)
            with open(path) as f:
                lines = f.read().split("\n")
            defs = definition_bodies(lines)
            loaders = loader_func_bodies(lines)
            for i, line in enumerate(lines):
                stripped = line.strip()
                if stripped.startswith("*") or stripped.startswith("/*"):
                    continue  # a comment that MENTIONS a store is not a store
                if not (WRITER_CALL.search(line) or WRITER_DIRECT.search(line)):
                    continue
                if VGA_LFB.search(line):
                    continue  # the VGA LFB, not guest RAM (printed as its own class below)
                hits.append((path, i, lines, defs, loaders))

    failed = 0
    exempt_counts = {}
    checked = 0
    for path, i, lines, defs, loaders in hits:
        line = lines[i]
        if DECLARATION.match(line):
            verdict = "EXEMPT-DECLARATION (a header prototype is not a write)"
        elif i in defs:
            verdict = "EXEMPT-DEFINITION (this line IS the store the hook guards)"
        elif os.path.basename(path).endswith("_selftest.c"):
            verdict = "EXEMPT-SELFTEST (builds code images by hand; the library default OFF)"
        elif any(path.endswith("/" + t) or path == t for t in LOADER_TXTS):
            verdict = "EXEMPT-LOADER-TIME (%s: a loader translation unit, pre-guest)" % \
                      enclosing_function(lines, i)
        elif i in loaders:
            verdict = "EXEMPT-LOADER-TIME (%s: runs before the guest's first instruction)" % \
                      enclosing_function(lines, i)
        elif any(HOOK.search(lines[j]) for j in range(max(0, i - WINDOW), i)):
            verdict = "OK (hooked within %d lines above)" % WINDOW
            checked += 1
        else:
            verdict = "FAIL (NO HOOK in the %d lines above)" % WINDOW
            failed += 1
        kind = verdict.split()[0]
        exempt_counts[kind] = exempt_counts.get(kind, 0) + 1
        print("   %-28s %s:%d: %s" % (kind, path, i + 1, line.strip()[:88]))
        print("   %-28s   -> %s" % ("", verdict))

    print("   checked %d hits: %d hooked, %d exempt-classified, %d FAIL" %
          (len(hits), checked, len(hits) - checked - failed, failed))
    for k in sorted(exempt_counts):
        print("   EXEMPT-CLASS %-24s %d" % (k, exempt_counts[k]))
    print("   VERDICT: %s" % ("THE RECONCILIATION IS EMPTY (every writer hooked or exempt)"
                              if failed == 0 else "%d UNHOOKED WRITER(S) -- STALE CODE RISK" % failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
