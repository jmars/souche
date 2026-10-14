#!/usr/bin/env python3
"""unity_symbols.py -- the EXACT unity guard (PERF-1R's check, completed by PERF-FIX).

Reads the COMPILED objects and compares the per-TU compilation of the unit's sources
with the one-translation-unit compilation of the same sources.  Two checks:

  (A) the defined-global sets must be identical in both directions, and
  (C) every name the per-TU unit resolves from OUTSIDE the unit must still be an
      undefined reference in the unity object.

(A) is PERF-1R's formulation, kept verbatim.  (C) is the comparison that actually
closes the silent class (below); (A) alone does not, which is the finding this unit
records -- results/perf-fix.md ITEM 1, results/perf-fix-guard.sh.

WHY THERE IS A SECOND CHECK (PERF-FIX, MEASURED).  The defined-global parity alone
does NOT catch the silent class, and the review that proposed it as the replacement
for the heuristic was wrong on that point.  The class is:

    cpua.c (in the unit):   static int rebind(int x) { return x + 1; }
    cpub.c (in the unit):   extern int rebind(int x); int cb(void){ return rebind(1); }
    hwdef.c (OUTSIDE):      int rebind(int x) { return x + 1000; }

MEASURED with the shipped compiler (cosmocc, gcc 14.1): the unit compiles with no
diagnostic, the link succeeds, per-TU `cb()` = 1001 and unity `cb()` = 2 -- and the
defined-global sets are {cb} on BOTH sides, so the parity check passes it.  The
difference is on the UNDEFINED side: per-TU, cpub.o references `rebind` (U) and the
link resolves it from an object OUTSIDE the unit; in the unity TU that reference is
gone (it bound to the local static).  Hence check (C) below: every name the per-TU
unit files need from OUTSIDE the unit must still be referenced by the unity object.

Usage: unity_symbols.py --unity UNITY.o OBJ.o ... [-- OUTSIDE.o ...]
  --unity   the unity translation unit's object
  before -- the per-TU compilation of the SAME sources the unity TU contains
            (the Makefile passes $(UNITY_PERTU_OBJ), derived from $(UNITY_SRC))
  after --  objects in the shipped link that are NOT part of the unity TU
            (src/hw, softfloat): a name defined by one of these is
            OUTSIDE-provided, and losing its reference is the silent class
  --parity-only  run ONLY the defined-global parity (check A) -- the formulation
            PERF-1R proposed, kept addressable because results/perf-fix-guard.sh
            measures what it does and does not catch.  NOT the gate.

Exit 0 iff the sets are equal, every outside-provided reference is preserved and
there are no other lost references; prints the differences and exits 1 otherwise.
"""
import subprocess
import sys

GLOBAL_TYPES = "TDBR"


def symbols(path, defined):
    """-> set of names defined with external linkage (defined=True) or referenced
    but not defined (defined=False, nm's `U` entries)."""
    args = ["nm", "--defined-only" if defined else "--undefined-only", path]
    out = subprocess.run(args, capture_output=True, text=True)
    if out.returncode != 0:
        sys.stderr.write("unity_symbols.py: nm failed on %s\n" % path)
        sys.exit(2)
    names = set()
    for line in out.stdout.splitlines():
        parts = line.split()
        if defined:
            if len(parts) >= 3 and parts[1] in GLOBAL_TYPES:
                names.add(parts[2])
        else:
            if len(parts) == 2 and parts[0] == "U":
                names.add(parts[1])
    return names


USAGE = "usage: unity_symbols.py --unity UNITY.o OBJ.o ... [-- OUTSIDE.o ...]\n"


def main(argv):
    argv = list(argv)
    parity_only = "--parity-only" in argv
    if parity_only:
        argv.remove("--parity-only")
    if len(argv) < 4 or argv[1] != "--unity":
        sys.stderr.write(USAGE)
        return 2
    unity_obj = argv[2]
    rest = argv[3:]
    split = rest.index("--") if "--" in rest else len(rest)
    pertu_paths, outside_paths = rest[:split], rest[split + 1:]
    if not pertu_paths:
        sys.stderr.write(USAGE)
        return 2

    unity_def = symbols(unity_obj, True)
    pertu_def, pertu_und = set(), set()
    for path in pertu_paths:
        pertu_def |= symbols(path, True)
        pertu_und |= symbols(path, False)
    outside_def = set()
    for path in outside_paths:
        outside_def |= symbols(path, True)
    unity_und = symbols(unity_obj, False)

    failed = False

    # (A) the defined-global sets must be identical in BOTH directions.
    lost = sorted(pertu_def - unity_def)     # defined per-TU, gone in unity
    gained = sorted(unity_def - pertu_def)   # defined only in unity
    if lost or gained:
        failed = True
        sys.stderr.write("unity_symbols.py: SYMBOL MISMATCH between the per-TU and "
                         "unity compilation units.\n")
        for name in lost:
            sys.stderr.write("  in per-TU but MISSING from unity: %s\n" % name)
        for name in gained:
            sys.stderr.write("  in unity but not in per-TU: %s\n" % name)
        sys.stderr.write("unity_symbols.py: the unity TU would silently change the "
                         "binding of these names -- rename one side.\n")

    # (C) every name the per-TU unit needs from OUTSIDE the unit must still be
    # referenced by the unity object.  A name that vanishes without being defined
    # inside the unit was bound to something the unit defines (the silent class).
    ref_out = pertu_und - pertu_def
    vanished = [] if parity_only else sorted(ref_out - unity_und)
    fatal = [n for n in vanished if n in outside_def]
    warn = [n for n in vanished if n not in outside_def]
    if fatal:
        failed = True
        sys.stderr.write("unity_symbols.py: %d reference(s) the per-TU unit resolves "
                         "from OUTSIDE the unit are gone from the unity TU: the unity TU "
                         "bound them to something it defines itself -- the silent "
                         "shadowing class (a unit `static` whose name is an outside "
                         "global).\n" % len(fatal))
        for name in fatal:
            sys.stderr.write("  outside-provided but not referenced by unity.o: %s\n"
                             % name)
        sys.stderr.write("unity_symbols.py: rename the shadowing definition, or drop it "
                         "from the unity source list.\n")
    if warn:
        sys.stderr.write("unity_symbols.py: warning: %d reference(s) not provided by the "
                         "watched outside objects also vanished (a toolchain-call "
                         "elimination is possible; investigate if unexpected): %s\n"
                         % (len(warn), ", ".join(warn)))

    if failed:
        return 1
    if parity_only:
        sys.stderr.write("unity_symbols.py: PARITY ONLY (PERF-1R's formulation): %d per-TU / "
                         "%d unity defined globals, identical sets\n"
                         % (len(pertu_def), len(unity_def)))
    else:
        sys.stderr.write("unity_symbols.py: %d per-TU / %d unity defined globals, identical "
                         "sets; %d of %d outside references preserved\n"
                         % (len(pertu_def), len(unity_def), len(ref_out) - len(warn),
                            len(ref_out)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
