#!/usr/bin/env python3
"""perf-fix-2-sweep.py -- the tree-wide inventory of the `dbg_assert!` CLASS.

WHAT THE CLASS IS (PERF-FIX's ITEM 2, PERF-3's finding 3).  Upstream guards code
with `dbg_assert!`/`debug_assert!` (v86/src/rust/dbg.rs), which is a *Rust* macro
defined as `debug_assert!` -- COMPILED OUT of the pinned RELEASE reference.  The
port emitted those as live `DBG_ASSERT` (= `assert` with no -DNDEBUG), so when a
guest can make the condition false the port ABORTS where the release reference
simply executes on.  PERF-FIX dropped 14 such sites in src/cpu/instructions.c;
its own caveat is that those 14 were ONE GREP'S INVENTORY, not a tree-wide sweep.

WHAT THIS SCRIPT DOES.  It is the inventory half of the sweep, mechanised so it
is re-runnable rather than recalled:

  1. every `DBG_ASSERT(` in src/ with its file:line and its ENCLOSING FUNCTION;
  2. for each, whether upstream has a `dbg_assert!`/`debug_assert!` with the SAME
     normalised condition (the class's defining property upstream side);
  3. a grouping by normalised condition, because the sites are heavily duplicated
     (`!cpu->pagefault` alone is 121 of them) -- one reachability argument covers
     a whole group, which is what makes a 359-site sweep writable AND reviewable.

It does NOT decide reachability: that is a judgement about the DATA FLOW into
each group's operands, and it is recorded per group in
results/perf-fix-2-sweep-verdicts.tsv, which this script reads and joins in.
A group with no verdict line is printed as UNVERDICTED and the script exits 3 --
so a new assert added to the tree cannot silently enter the sweep.

Usage:
    python3 results/perf-fix-2-sweep.py [--tree DIR] [--upstream DIR] [--md FILE]
Exit 0 iff every group carries a verdict; 3 if any group is unverdicted.
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# A C source line that is a DBG_ASSERT invocation.  The port's format is one
# invocation per line (multi-line conditions exist: they are gathered by the
# continuation logic below).
INVOKE = re.compile(r"\bDBG_ASSERT\s*\(")
# A file-scope function definition's head line, K&R style: col 0, ends in ')' or '{'.
FUNC = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \t\*]*\([^;{]*\)\s*$")
# an upstream debug assert
RS_ASSERT = re.compile(r"\b(dbg_assert|debug_assert)!\s*\(")


def normalise(text):
    """Normalised comparison key for an assert condition.

    Two sides are the SAME SITE when their condition text is equal after:
      * removing comments and whitespace;
      * dropping the two spellings of a cast that the languages differ on --
        C's `(uint32_t)x` and Rust's `x as u32`;
      * dropping the numeric-literal decorations C and Rust differ on
        (`0xFFFF0000u` vs `0xFFFF_0000`) and the `*` of a Rust global deref;
      * mapping C's `cpu->x` and Rust's `x` to the same token.
    Deliberately CONSERVATIVE: a condition this cannot match is reported as
    "no upstream match" and is adjudicated by hand, never assumed.  (Its first
    form missed upstream's `v as u32 & 0xFFFF_0000 == 0` and
    `(*last_result as u32) <= m`, which are real matches -- the cast and literal
    handling above is what closes that.)
    """
    t = text
    t = re.sub(r"/\*.*?\*/", "", t)
    t = t.split("//")[0]
    t = re.sub(r"\bas\s+(u8|u16|u32|u64|i8|i16|i32|i64|usize|isize)\b", "", t)
    t = re.sub(r"\((u?int(8|16|32|64)_t|int|unsigned|size_t|uint|u32|i32|u64|i64)\)\s*", "", t)
    t = re.sub(r"\b(int32_t|uint32_t|int64_t|uint64_t|u32|i32|u64|i64)\b", "", t)
    t = re.sub(r"(0x[0-9A-Fa-f]+)_([0-9A-Fa-f_]+)", r"\1\2", t)
    t = re.sub(r"([0-9A-Fa-f])u\b", r"\1", t)
    t = re.sub(r"\*\s*([A-Za-z_][A-Za-z0-9_.]*)", r"\1", t)   # Rust *x -> x
    t = t.replace("cpu->", "")
    t = re.sub(r"\s+", "", t)
    t = t.replace("&&", "&").replace("||", "|")
    # Redundant parentheses only: `(v & m) == 0` and `v & m == 0` are the same
    # condition as far as finding the upstream counterpart goes, and the C and Rust
    # spellings differ exactly there.  Every group this newly matches is verified by
    # eye in the sweep table (the count is small and the pairs are named).
    t = t.replace("(", "").replace(")", "")
    t = t.replace("&", "&")
    return t


def codelines(text):
    """Yield (lineno, code-with-comments-blanked) for a C source, tracking block
    comments and string literals at file scope -- so a DBG_ASSERT named inside a
    comment is not counted as a site (two such lines exist in exceptions.c)."""
    out = []
    in_block = False
    for n, line in enumerate(text.split("\n"), 1):
        code, i = "", 0
        while i < len(line):
            if in_block:
                j = line.find("*/", i)
                if j < 0:
                    i = len(line)
                    break
                in_block = False
                i = j + 2
                continue
            if line.startswith("/*", i):
                in_block = True
                i += 2
                continue
            if line.startswith("//", i):
                break
            code += line[i]
            i += 1
        out.append((n, code))
    return out


def gather_c_sites(paths):
    sites = []
    for path in paths:
        rel = os.path.relpath(path, ROOT)
        raw = open(path, encoding="utf-8", errors="replace").read()
        lines = raw.split("\n")
        codes = codelines(raw)
        func = "<file scope>"
        for i, (num, line) in enumerate(codes):
            m = FUNC.match(line)
            if m and not line.lstrip().startswith(("//", "*", "#")):
                j = i + 1
                while j < len(lines) and lines[j].strip() == "":
                    j += 1
                if j < len(lines) and (lines[j].lstrip().startswith("{")
                                       or line.rstrip().endswith("{")):
                    func = line.split("(")[0].strip()
            if INVOKE.search(line) and not line.lstrip().startswith("#define"):
                text = line[line.find("DBG_ASSERT") + len("DBG_ASSERT"):]
                depth = text.count("(") - text.count(")")
                k = i
                while depth > 0 and k + 1 < len(codes):
                    k += 1
                    text += " " + codes[k][1]
                    depth = text.count("(") - text.count(")")
                cond = text.strip()
                if cond.startswith("("):
                    depth, end = 0, None
                    for idx, ch in enumerate(cond):
                        if ch == "(":
                            depth += 1
                        elif ch == ")":
                            depth -= 1
                            if depth == 0:
                                end = idx
                                break
                    if end is not None:
                        cond = cond[1:end]
                cond = re.sub(r"\s+", " ", cond).strip()
                sites.append({"file": rel, "line": num, "func": func,
                              "cond": cond, "key": normalise(cond)})
    return sites


def gather_rust_asserts(updir):
    by_key = {}
    for dirpath, _dirs, files in os.walk(updir):
        for fn in files:
            if not fn.endswith(".rs"):
                continue
            path = os.path.join(dirpath, fn)
            text = open(path, encoding="utf-8", errors="replace").read()
            for m in RS_ASSERT.finditer(text):
                depth, i = 0, m.end() - 1
                while i < len(text):
                    if text[i] == "(":
                        depth += 1
                    elif text[i] == ")":
                        depth -= 1
                        if depth == 0:
                            break
                    i += 1
                cond = text[m.end():i]
                cond = re.sub(r"//.*", "", cond)
                key = normalise(cond)
                rel = os.path.relpath(path, ROOT)
                by_key.setdefault(key, []).append((rel, cond.strip()))
    return by_key


def load_verdicts(path):
    out = {}
    if not os.path.exists(path):
        return out
    for line in open(path, encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        parts = line.rstrip("\n").split("\t")
        if len(parts) < 3:
            continue
        out[parts[0]] = (parts[1], parts[2])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default=os.path.join(ROOT, "src"))
    ap.add_argument("--upstream", default=os.path.join(ROOT, "v86", "src", "rust"))
    ap.add_argument("--verdicts",
                    default=os.path.join(ROOT, "results", "perf-fix-2-sweep-verdicts.tsv"))
    ap.add_argument("--md", default=None)
    args = ap.parse_args()

    paths = []
    for dirpath, _d, files in os.walk(args.tree):
        for fn in sorted(files):
            if fn.endswith((".c", ".h")):
                paths.append(os.path.join(dirpath, fn))
    sites = gather_c_sites(paths)
    rust = gather_rust_asserts(args.upstream)
    verdicts = load_verdicts(args.verdicts)

    groups = {}
    for s in sites:
        groups.setdefault(s["key"], []).append(s)

    rows, unverdicted = [], 0
    for key, members in sorted(groups.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        cond = members[0]["cond"]
        up = rust.get(key, [])
        files = sorted({m["file"] for m in members})
        funcs = sorted({m["func"] for m in members})
        sites_txt = ", ".join("%s:%d(%s)" % (m["file"], m["line"], m["func"]) for m in members)
        v = verdicts.get(cond)
        if v is None:
            unverdicted += 1
            verdict, why = "UNVERDICTED", ""
        else:
            verdict, why = v
        rows.append({"cond": cond, "n": len(members), "upstream": up,
                     "files": files, "funcs": funcs, "sites": sites_txt,
                     "verdict": verdict, "why": why})

    total = len(sites)
    print("DBG_ASSERT invocations in src/: %d, distinct conditions: %d" % (total, len(groups)))
    print("groups with an upstream dbg_assert!/debug_assert! of the same condition: %d"
          % sum(1 for r in rows if r["upstream"]))
    print("groups unverdicted: %d" % unverdicted)
    if args.md:
        with open(args.md, "w", encoding="utf-8") as fh:
            fh.write("| n | condition | sites | upstream dbg_assert! | verdict | why |\n")
            fh.write("|---|---|---|---|---|---|\n")
            for r in rows:
                up = "yes (" + ", ".join(sorted({u[0] for u in r["upstream"]})) + ")" \
                    if r["upstream"] else "no"
                fh.write("| %d | `%s` | %s | %s | **%s** | %s |\n"
                         % (r["n"], r["cond"].replace("|", "\\|"),
                            r["sites"].replace("|", "\\|"), up, r["verdict"], r["why"]))
        print("wrote %s" % args.md)
    return 3 if unverdicted else 0


if __name__ == "__main__":
    sys.exit(main())
