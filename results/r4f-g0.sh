#!/bin/sh
# results/r4f-g0.sh -- the G0 asymmetry rig (unit R4F, gates G1 and G2).
#
# G0 is a TEST THAT COULD NOT FAIL: tests/nasm_run.py's MMX-register branch was
# unreachable (a signed 32-bit fixture field compared against the 16-bit constant
# 0xAAAA), so the corpus never compared mm0..mm7 at all (results/m1b2-mmx-sse.md
# finding 2).  This rig measures the repair from both sides:
#
#   G2 REACHABILITY -- results/r4f-branch-count.py evaluates BOTH predicates (upstream's
#      run.js:423 one and the repaired one) on the tag word of every fixture the gate
#      reads, so "the branch is reachable" is a count over the real corpus, not a claim.
#
#   G1 ASYMMETRY -- one MMX mutation (punpcklbw's two operands exchanged in the MMX body
#      `instr_0F60`, the same arm M1b-2's rig used as M5) applied ALONE and restored
#      byte-identically: with the repaired oracle the corpus FAILS and NAMES the mm
#      fields; by construction the repaired branch must have evaluated to report that.
#
# Usage:  sh results/r4f-g0.sh
# Reads: tests/nasm_run.py, src/cpu/instructions_0f.c, the fixtures in build/nasm
# Writes: build/r4f-probe/*

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

WORK=build/r4f-probe
SRC=src/cpu/instructions_0f.c
SAVE=$WORK/instructions_0f.pristine.c

say() { printf '%s\n' "$*"; }
hr() { say "-------------------------------------------------------------------"; }

mkdir -p "$WORK" || exit 1

# --- G2: reachability -----------------------------------------------------------------
hr
say "G2 -- WHICH BRANCH EACH CORPUS TEST TAKES (results/r4f-branch-count.py)"
say "   (the fixtures are the ones the gate reads: build/nasm/*.fixture)"
if ! python3 results/r4f-branch-count.py > "$WORK/branch-count.txt" 2>&1; then
	say "FATAL: the instrument failed; is build/nasm populated (make nasm-fixtures)?"
	cat "$WORK/branch-count.txt"
	exit 1
fi
tail -6 "$WORK/branch-count.txt"
say "   full table: $WORK/branch-count.txt"

# --- G1: the mutation -----------------------------------------------------------------
hr
say "G1 -- THE ASYMMETRY (one MMX mutation, applied alone, then restored)"
cp "$SRC" "$SAVE" || exit 1
pristine_sha=$(sha256sum "$SRC" | cut -d' ' -f1)
say "pristine $SRC sha256=$pristine_sha"

python3 - <<'PY' || exit 1
path = "src/cpu/instructions_0f.c"
s = open(path).read()
anchor = "static void instr_0F60(int32_t source, int32_t r)"
i = s.index(anchor)
head, tail = s[:i], s[i:]
old = "\t\tresult.u8[2 * i + 0] = destination.u8[i];\n\t\tresult.u8[2 * i + 1] = src.u8[i];"
new = "\t\tresult.u8[2 * i + 0] = src.u8[i];\n\t\tresult.u8[2 * i + 1] = destination.u8[i];"
assert tail.count(old) == 1, "the MMX punpcklbw body did not match exactly once"
open(path, "w").write(head + tail.replace(old, new, 1))
print("MUTATION APPLIED (instr_0F60, the MMX punpcklbw body):")
print("  -  result.u8[2*i+0] = destination.u8[i];  result.u8[2*i+1] = src.u8[i];")
print("  +  result.u8[2*i+0] = src.u8[i];          result.u8[2*i+1] = destination.u8[i];")
PY

say ""
say "MUTATED: make nasm (all other files pristine; the oracle is the repaired one)"
make nasm > "$WORK/m5-mutated.txt" 2>&1
rc=$?
say "  rc=$rc"
grep -E '^\[\+\] Passed|^\[-\] Failed|^\[-\] ' "$WORK/m5-mutated.txt" | head -8
say "  the failing test's reported field(s) -- the mm branch is what can name them:"
grep -E 'mm[0-9]\.int32|Actual:|Expected:' "$WORK/m5-mutated.txt" | head -6 | sed 's/^/    /'

say ""
say "RESTORED (byte-identical?)"
cp "$SAVE" "$SRC"
restored_sha=$(sha256sum "$SRC" | cut -d' ' -f1)
say "  sha256=$restored_sha"
[ "$restored_sha" = "$pristine_sha" ] && say "  byte-identical: YES" || { say "  byte-identical: NO -- STOP"; exit 1; }

say ""
say "RESTORED: make nasm"
make nasm > "$WORK/pristine.txt" 2>&1
rc=$?
say "  rc=$rc"
grep -E '^\[\+\] Passed' "$WORK/pristine.txt" | sed 's/^/  /'
say ""
say "raw: $WORK/{branch-count,m5-mutated,pristine}.txt"
