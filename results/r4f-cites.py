#!/usr/bin/env python3
"""results/r4f-cites.py -- the N1 invariant checker (unit R4F).

Every `instructions_0f.rs:N-M` citation in src/cpu/instructions_0f.c must point at the
upstream code the block beneath it ports.  Before R4F that was false in 66 places -- dozens
of them by more than 2000 lines (cpuid cited 1064-1228 where instr_0FA2 is at 3256-3419),
i.e. a reader following the citation landed on unrelated code.

This script re-derives the truth from ./v86 (READ-ONLY) and reports, for every citation,
where the block beneath it really is.  Its convention, stated so the numbers can be audited:

  * a citation's BLOCK is the `instr*` definitions between it and the next citation;
  * the block's true span is [first function's `pub unsafe fn` line, last function's closing
    brace] -- upstream's fn bodies, not its `#[no_mangle]`/comment preamble;
  * a citation PASSES if its start is inside that span, or at most PREAMBLE lines before it,
    which is the file's (correct) habit of citing a shared helper or a `#[no_mangle]` line
    that sits just above the ported block (e.g. lmsw cites the `lmsw` helper at 282-288,
    whose ported callers are at 291-309).  Anything further away is reported as a FAILURE.

    python3 results/r4f-cites.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUST = ROOT / "v86" / "src" / "rust" / "cpu" / "instructions_0f.rs"
C = ROOT / "src" / "cpu" / "instructions_0f.c"
PREAMBLE = 12

rust = RUST.read_text().splitlines()
span = {}
for ln, line in enumerate(rust, 1):
    m = re.match(r"pub unsafe fn (\w+)", line)
    if m:
        end = ln
        if not (line.rstrip().endswith("}") and line.count("{") == line.count("}")):
            for j in range(ln + 1, len(rust) + 1):
                if rust[j - 1] == "}":
                    end = j
                    break
        span[m.group(1)] = (ln, end)

clines = C.read_text().splitlines()
defs = []
for ln, line in enumerate(clines, 1):
    m = re.match(r"(?:static )?void (instr\w+)\(", line)
    if m:
        if m.group(1) not in span:
            print("error: %s has no upstream `pub unsafe fn` to cite" % m.group(1),
                  file=sys.stderr)
            sys.exit(2)
        defs.append((ln, m.group(1)))
cites = []
for ln, line in enumerate(clines, 1):
    m = re.search(r"instructions_0f\.rs:(\d+)(?:-(\d+))?", line)
    if m:
        cites.append({"line": ln, "a": int(m.group(1)),
                      "b": int(m.group(2)) if m.group(2) else None, "text": line})

failures = []
exact = preamble = helper = 0
worst = (0, None)
for i, c in enumerate(cites):
    endln = cites[i + 1]["line"] if i + 1 < len(cites) else len(clines) + 1
    block = [n for dl, n in defs if c["line"] < dl < endln]
    if not block:                      # a group/file-level remark, or a header with no defs
        continue
    sp = [span[n] for n in block]
    lo, hi = min(s for s, _ in sp), max(e for _, e in sp)
    d = c["a"] - lo
    if lo <= c["a"] <= hi:
        exact += 1
    elif 0 < lo - c["a"] <= PREAMBLE:
        preamble += 1
        if d < worst[0]:
            worst = (d, c)
    else:
        helper += 1
        failures.append((c, (lo, hi), d))

print("citations with a block beneath them: %d" % (exact + preamble + helper))
print("  start inside the block's upstream span        : %d" % exact)
print("  start in the %d-line preamble above it (helper): %d" % (PREAMBLE, preamble))
print("  MISPOINTED (>%d lines away)                   : %d" % (PREAMBLE, helper))
if worst[1]:
    print("  the largest preamble case: line %d, cited %d, block starts %d (d=%d)"
          % (worst[1]["line"], worst[1]["a"], worst[1]["a"] - worst[0], worst[0]))
for c, (lo, hi), d in failures:
    print("  FAIL line %d: cited %d-%s, block is %d-%d (d=%+d)  %s"
          % (c["line"], c["a"], c["b"], lo, hi, d, c["text"].strip()[:60]))
sys.exit(1 if failures else 0)
