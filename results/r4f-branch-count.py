#!/usr/bin/env python3
"""results/r4f-branch-count.py -- G2 for unit R4F: WHICH fpu/mm comparison branch each
corpus test takes.

The claim under test is that tests/nasm_run.py's MMX-register branch is REACHABLE (and
was not, before R4F's G0 repair: `fpu_tag != FPU_TAG_ALL_INVALID` compared a signed
32-bit fixture field against a 16-bit constant -- see results/m1b2-mmx-sse.md finding 2).

The instrument reads the SAME fixtures the gate reads (build/nasm/<t>.fixture, written by
build/mkfixture) and evaluates BOTH predicates on the fixture's tag word -- the old one
verbatim from run.js:423 and the repaired one -- so the two are compared on the same
inputs rather than argued about.  It imports the runner itself (not a copy of its logic)
so there is one definition of FPU_TAG_ALL_INVALID.

    python3 results/r4f-branch-count.py            # needs `make nasm-fixtures` to have run
"""

import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("nasm_run", ROOT / "tests" / "nasm_run.py")
nasm_run = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nasm_run)

# run.js:393-403: reg32[8], eip, st[8], mm[16], xmm[32], mem[2048], eflags, fpu_tag, fpu_status
TAG_INDEX = 8 + 1 + 8 + 16 + 32 + 2048 + 1

fixture_dir = ROOT / "build" / "nasm"
fixtures = sorted(fixture_dir.glob("*.fixture"))
if not fixtures:
    print("error: no fixtures in %s -- run `make nasm-fixtures` first" % fixture_dir,
          file=sys.stderr)
    sys.exit(2)

old_mm = new_mm = 0
rows = []
for path in fixtures:
    name = path.stem
    try:
        expected = nasm_run.extract_json(name, path.read_text())
    except Exception as e:                       # noqa: BLE001 - report, do not hide
        rows.append((name, "unparseable: %s" % e, "", ""))
        continue
    tag = expected["array"][TAG_INDEX]
    old = "st" if tag != nasm_run.FPU_TAG_ALL_INVALID else "mm"
    new = "st" if (tag & 0xFFFF) != nasm_run.FPU_TAG_ALL_INVALID else "mm"
    old_mm += old == "mm"
    new_mm += new == "mm"
    rows.append((name, "0x%08X" % (tag & 0xFFFFFFFF), old, new))

width = max(len(r[0]) for r in rows)
print("%-*s  %-10s %-8s %s" % (width, "test", "tag word", "run.js", "repaired"))
for name, tag, old, new in rows:
    flag = "   <-- the branch the repair REACHES" if old != new else ""
    print("%-*s  %-10s %-8s %-8s%s" % (width, name, tag, old, new, flag))

print()
print("tests: %d" % len(rows))
print("tests taking the mm (MMX-register) branch, run.js:423 predicate : %d" % old_mm)
print("tests taking the mm (MMX-register) branch, repaired predicate   : %d" % new_mm)
print("tests whose branch CHANGED (unreachable -> reachable)            : %d"
      % (new_mm - old_mm))
