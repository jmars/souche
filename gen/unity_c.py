#!/usr/bin/env python3
"""unity_c.py -- emit the UNITY translation unit (HANDOFF=v86c-perf, unit PERF-1).

WHY THIS EXISTS.  cosmocc has no LTO -- `-flto` fails with "LTO support has not been
enabled in this configuration" -- and gcc does not inline across separately compiled
translation units.  This port is PER-OPERAND-CALL BOUND (measured: read_imm8 337.3M
calls, read_reg32 212.6M, get_phys_eip 200.1M, write_reg32 119.0M, resolve_modrm32
83.1M for 200,095,214 guest instructions), and every one of those is a cross-TU call.
Compiling the whole core as ONE translation unit gives the compiler intra-TU inlining
WITHOUT LTO -- most of what -flto would have given -- and MEASURED 6.367 s -> 4.970 s
on the boot benchmark (results/perf-1-inlining.md).

WHAT IT EMITS: one `#include` per source file, in the order given on the command line.
Nothing else -- no macro games, no renames.  The unit is a PURE concatenation, so a
semantic change in the unity build is a compiler bug or an ODR-style collision, and
both are what the gate suite (byte-identical transcript, 118/118 nasm, the boot) is
there to catch.

THE ONE HAZARD IT GUARDS: file-scope `static` definitions live in one namespace once
every TU is one.  CORRECTED BY MEASUREMENT (PERF-FIX; the first version of this file
claimed the opposite and was wrong): a collision between two definitions INSIDE the
unit is NEVER silent.  MEASURED with the shipped compiler (cosmocc, gcc 14.1, real
files): `static fn` + `static fn`, `static fn` + global fn with an IDENTICAL
signature, and `static obj` + global obj in either order are all hard ERRORS
(redefinition / "static declaration follows non-static").  The behaviour is not
type-dependent, so this script's in-set refusal is a DIAGNOSTICS-QUALITY check (a
clearer message than gcc's), not the safety net.

The collision that IS silent needs a definition OUTSIDE the unit (`src/hw`,
`src/main.c`, softfloat) plus a prototype visible to another unit file:

    cpua.c (in the unit):  static int rebind(int x) { return x + 1; }
    cpub.c (in the unit):  extern int rebind(int x); int cb(void){ return rebind(1); }
    hwdef.c (OUTSIDE):     int rebind(int x) { return x + 1000; }

MEASURED end to end: no diagnostic, the link succeeds, per-TU `cb()` = 1001 (it binds
to the outside global) and unity `cb()` = 2 (it binds to the static) -- a silent
behaviour change with a working per-TU baseline.  This scan does not cover that class
by itself.  Two things close it:

  * `--watch FILE ...` (below): refuse a unit whose file-scope static name is also
    DEFINED or DECLARED with external linkage in any watched file (the other unit
    files of the link: src/hw, src/main.c, softfloat, and the headers).  The scan is a
    heuristic over the same shapes as the unit scan -- it over-refuses, never
    under-refuses.
  * gen/unity_symbols.py, which reads the COMPILED objects and is the exact check.
    Both are wired into `make selftest` (target `unity-symbols`).

Two collisions were found this way while this unit was being built and are renamed at
their definitions (ps2.c's js_to_int32 vs pit.c's; kernel.c's write_blob vs
memory.c's) -- both in-set, i.e. loud under the corrected model.
"""

import re
import sys

# A definition at file scope starts in column 0.  `static` ones are the hazard; the
# others are collected so a static can be compared against them.
STATIC_DEF = re.compile(
    r"^static\s+(?:inline\s+|const\s+|volatile\s+|constexpr\s+)*"
    r"(?:unsigned\s+|signed\s+|struct\s+\w+\s+|union\s+\w+\s+|enum\s+\w+\s+)*"
    r"[\w\s\*]*?\b(\w+)\s*[\(\[=;]")
GLOBAL_DEF = re.compile(
    r"^(?!static\b|typedef\b|extern\b|struct\b|union\b|enum\b|#|/\*|\s|})"
    r"[\w]+[\w\s\*]*?\b(\w+)\s*\(")
# PERF-FIX: the same two shapes, applied to the files OUTSIDE the unit.  Prototypes
# count too -- a header declaration is enough to give a later unit file an external
# reference to the name.
GLOBAL_DECL = re.compile(
    r"^(?!static\b|typedef\b|extern\b|struct\b|union\b|enum\b|#|/\*|\s|})"
    r"[\w]+[\w\s\*]*?\b(\w+)\s*\(")
OBJECT_DEF = re.compile(
    r"^(?!static\b|typedef\b|extern\b|struct\b|union\b|enum\b|#|/\*|\s|})"
    r"[\w]+[\w\s\*]*\b(\w+)\s*(?:=\s*[^;]+)?;")


def scan(path):
    """-> (statics, globals) defined at file scope in `path` (name -> line)."""
    statics, globals_ = {}, {}
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for number, line in enumerate(handle, 1):
            if line.startswith("static"):
                match = STATIC_DEF.match(line)
                if match:
                    statics.setdefault(match.group(1), number)
            else:
                match = GLOBAL_DEF.match(line)
                if match and not line.rstrip().endswith(";"):
                    globals_.setdefault(match.group(1), number)
    return statics, globals_


def scan_watch(path):
    """-> names DECLARED or DEFINED with external linkage in `path` (name -> line).

    `path` is OUTSIDE the unit (src/hw, src/main.c, softfloat, the headers), so a
    prototype counts as much as a definition: either one gives a unit file an
    external reference to the name."""
    names = {}
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for number, line in enumerate(handle, 1):
            for pattern in (GLOBAL_DECL, OBJECT_DEF):
                match = pattern.match(line)
                if match:
                    names.setdefault(match.group(1), number)
                    break
    return names


def main(argv):
    if len(argv) < 3:
        sys.stderr.write("usage: unity_c.py [--watch FILE ...] -o OUT.c SRC.c ...\n")
        return 2
    watch, rest = [], argv[1:]
    while rest and rest[0] == "--watch":
        rest.pop(0)
        while rest and rest[0] != "-o":
            watch.append(rest.pop(0))
    if len(rest) < 3 or rest[0] != "-o":
        sys.stderr.write("usage: unity_c.py [--watch FILE ...] -o OUT.c SRC.c ...\n")
        return 2
    out_path, sources = rest[1], rest[2:]

    defined = {}  # name -> [(path, kind, line)]
    for path in sources:
        statics, globals_ = scan(path)
        for name, line in statics.items():
            defined.setdefault(name, []).append((path, "static", line))
        for name, line in globals_.items():
            defined.setdefault(name, []).append((path, "global", line))

    bad = {name: hits for name, hits in defined.items()
           if len({hit[0] for hit in hits}) > 1 and any(hit[1] == "static" for hit in hits)}
    if bad:
        sys.stderr.write("unity_c.py: REFUSING to emit: a file-scope static name is "
                         "defined by more than one input file.\n")
        for name in sorted(bad):
            for path, kind, line in sorted(bad[name]):
                sys.stderr.write("unity_c.py:   %s (%s) %s:%d\n" % (name, kind, path, line))
        sys.stderr.write("unity_c.py: rename one of them at its definition -- in one "
                         "translation unit an agreeing signature would bind silently.\n")
        return 1

    # PERF-FIX: the class this scan exists for -- a unit static whose name is an
    # external-linkage DEFINITION or DECLARATION in a watched file OUTSIDE the unit.
    outside = {}  # name -> (path, line)
    for path in watch:
        for name, line in scan_watch(path).items():
            outside.setdefault(name, (path, line))
    shadowed = [(name, hits, outside[name])
                for name, hits in defined.items()
                if any(hit[1] == "static" for hit in hits) and name in outside]
    if shadowed:
        sys.stderr.write("unity_c.py: REFUSING to emit: a file-scope static name in the "
                         "unit is defined or declared OUTSIDE it (in one TU the calls "
                         "from the other unit files would silently bind to the static).\n")
        for name, hits, (path, line) in sorted(shadowed):
            for hpath, kind, hline in hits:
                sys.stderr.write("unity_c.py:   %s (%s) %s:%d\n" % (name, kind, hpath, hline))
            sys.stderr.write("unity_c.py:   %s (outside) %s:%d\n" % (name, path, line))
        sys.stderr.write("unity_c.py: rename the static, or drop the file from the "
                         "unity source list.\n")
        return 1

    with open(out_path, "w", encoding="utf-8") as out:
        out.write("/* GENERATED by gen/unity_c.py -- do not edit; edit the Makefile's\n"
                  " * source lists or the generator.  See the generator's header for WHY\n"
                  " * the core is compiled as one translation unit. */\n\n")
        for path in sources:
            out.write('#include "%s"\n' % path)
    sys.stderr.write("unity_c.py: %d sources -> %s\n" % (len(sources), out_path))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
