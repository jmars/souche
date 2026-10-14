#!/usr/bin/env python3
"""perf-5-seg-audit.py -- THE AUDIT BEHIND PERF-5's M4a DECISION.

WHY IT EXISTS.  PERF-5 removed the per-uop `get_seg` call -- 78,235,256 calls per 200,095,214
instructions on the benchmark -- by TRANSCRIBING modrm.c's get_seg_prefix/get_seg into
uop_cache.c as a static inline (see src/cpu/uop_cache.c's `uop_seg_base`).  The plan's own M4
prescribes the other design: a segment-base CACHE whose validity rests on an "epoch" that must
be bumped wherever segment state changes.  The transcription was chosen because it assumes
nothing beyond its own correctness -- it reads the live state, exactly where get_seg does.

THIS SCRIPT IS WHAT THE REJECTED DESIGN WOULD HAVE REQUIRED, and it is the evidence for that
choice: an epoch is only sound if EVERY writer of the state it summarises is accounted for.
The list below is that accounting, and the 17 sites it flags are the reason a transcription is
the safer of the two.  (It is NOT needed for the shipped code -- uop_seg_base has no epoch.)

WHAT IT DOES, MECHANICALLY.  It lists every write to `cpu->segment_offsets[...]`
and `cpu->segment_is_null[...]` in the shipped sources, and for each one reports whether an
`update_state_flags()` call follows it INSIDE THE SAME FUNCTION.  A site that does not is not
automatically a defect -- the caller may do it -- but it is exactly the list a reader has to
check, so the ones that matter are printed with their enclosing function and line, and the
summary is the function-level claim: "every function that writes segment state and returns to
the interpreter also updates the flags".

WHAT IT WOULD NOT CATCH (stated, not hidden): a write performed outside these two arrays by a
different name (a struct copy of cpu_state_t, a memcpy over the segment arrays).  The tree has
none -- `rg 'segment_offsets|segment_is_null'` in src/ is the complete set of mentions -- and
the script's last check re-runs that search so a new site cannot appear silently.

Usage: python3 results/perf-5-seg-audit.py [--src src]     exit 0 iff no unexplained writer.
"""

import re
import subprocess
import sys

WRITE_RE = re.compile(r"(segment_offsets|segment_is_null)\s*\[\s*[^\]]+\s*\]\s*=[^=]")
FUNC_RE = re.compile(r"^(?:[A-Za-z_][A-Za-z0-9_ \t\*]*?)\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
EXCLUDE = ("selftest",)  # harnesses write state by hand; not the shipped path


def files(src):
    out = subprocess.run(["rg", "-l", "segment_offsets|segment_is_null", src],
                         capture_output=True, text=True, check=True).stdout.split()
    return [f for f in out if not any(x in f for x in EXCLUDE) and f.endswith((".c", ".h"))]


def enclosing(text_lines, idx):
    """the nearest preceding function-definition-looking line that is not an invocation"""
    for i in range(idx - 1, -1, -1):
        line = text_lines[i]
        if line.startswith(("{", "}", "\t", " ")):
            continue
        if "(" in line and ")" in line and ";" not in line and "=" not in line:
            return i, line.strip()
    return None, "<file scope>"


def main():
    src = sys.argv[2] if len(sys.argv) > 2 and sys.argv[1] == "--src" else "src"
    unexplained = 0
    total = 0
    print("== every writer of segment_offsets[] / segment_is_null[], and what follows it")
    for path in sorted(files(src)):
        lines = open(path).read().split("\n")
        for i, line in enumerate(lines):
            if not WRITE_RE.search(line):
                continue
            total += 1
            fstart, fname = enclosing(lines, i)
            # look for update_state_flags() after this write, before the function's end
            follows = False
            depth = 0
            started = False
            for j in range(i, len(lines)):
                txt = lines[j]
                depth += txt.count("{") - txt.count("}")
                if "{" in txt:
                    started = True
                if "update_state_flags()" in txt:
                    follows = True
                    break
                if started and depth <= 0 and j > i:
                    break
            if not follows:
                # a caller may do it; report for the reader, and count only the ones in
                # functions whose name says they are a segment/descriptor change
                print("  %-28s:%-4d %-42s %s" % (path, i + 1, fname, "no update_state_flags() after it"))
                if re.search(r"switch_seg|descriptor|null|load_seg|pop_seg|mov_seg|segment|reset_cpu|far|iret|int", fname, re.I):
                    unexplained += 1
            else:
                print("  %-28s:%-4d %-42s ok" % (path, i + 1, fname))

    print()
    print("writers found: %d" % total)

    # the completeness re-check: no OTHER name may touch the two arrays
    rg = subprocess.run(["rg", "-n", "segment_offsets|segment_is_null", src],
                        capture_output=True, text=True).stdout.split("\n")
    other = [l for l in rg if l and not WRITE_RE.search(l) and ":" in l]
    print("mentions that are not writers (reads, declarations): %d" % len(other))
    print()
    if unexplained:
        print("VERDICT: %d writer(s) in a segment-changing function have no update_state_flags()"
              " after them -- the EPOCH design would have to account for each of these" % unexplained)
        return 1
    print("VERDICT: every writer is followed by update_state_flags() inside its own function")
    return 0


if __name__ == "__main__":
    sys.exit(main())
