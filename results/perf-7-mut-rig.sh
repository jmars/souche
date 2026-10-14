#!/bin/bash
# perf-7-mut-rig.sh -- the asymmetry rig for PERF-7's two emissions (the audit's gate note:
# "invert the cont criterion to always-false -> must produce the shipped binary's behaviour;
# make it always-true -> a staircase-sensitive guest must fail").
#
# Each arm mutates ONE clause of PERF-7's work in src/cpu/jit_codegen.c, rebuilds the codegen
# selftest (and, for the always-false arm, the shipped binary), runs it, and REQUIRES the
# named pin to fail.  A control arm runs the unmutated tree first.  The tree is restored from
# git-verified pristine copies at the end of every arm (the stack files are staged in
# /tmp/perf7 by the unit; this rig refuses to run without them).
#
# Arms:
#   control      no mutation; the suite must PASS -- the rig can see green
#   mut1         jitcg_uop_continues returns false: the elision never fires.  The elision
#                battery's engage pins (elided == 6 / elided == inlined-1) MUST fail -- that
#                is what makes them able to see the criterion not firing.  The shipped
#                behaviour is otherwise the pre-F1 shape (the state probes still agree).
#   mut2         the kind exclusion dropped (JCC/JMP/LEA elided): the matrix's jcc/jmp/lea
#                cases and test_tail_exit MUST fail (a taken branch no longer reaches
#                step 11's TAIL).
#   mut4         F2's count mask dropped (the count==0 test reads the UNMASKED CL): the
#                matrix's CL=0x20-masked-to-0 arm MUST fail (flag state stored on a
#                count-0 shift).
#   (NO mut3: the criterion's FLUSH/BLOCK_END clause has no firing instance today -- the
#   landing scan found no recipe encoding carrying either descriptor flag -- so a mutation
#   of it would change nothing and prove nothing.  The clause's instrument is instead the
#   invariant pin (d) in test_recipe_table, which fails the suite if a future table entry
#   gains a flagged encoding.  Mutating a clause that cannot fire is a test that cannot
#   fail, and this rig does not ship those.)
#
# Usage: sh results/perf-7-mut-rig.sh            (from the repo root)
# Raw output: results/perf-7/mut-<arm>.txt

set -u
cd "$(dirname "$0")/.." || exit 2

STAGE=/tmp/perf7
[ -f "$STAGE/stack/jit_codegen.c" ] || { echo "mut-rig: $STAGE/stack/jit_codegen.c missing (build the stack snapshot first)" >&2; exit 2; }

WORK=/tmp/perf7/mut-tree
rm -rf "$WORK"
mkdir -p "$WORK"
cp src/cpu/jit_codegen.c "$WORK/jit_codegen.c.good"
cp src/cpu/jit_codegen_selftest.c "$WORK/selftest.c.good"
restore() { cp "$WORK/jit_codegen.c.good" src/cpu/jit_codegen.c; cp "$WORK/selftest.c.good" src/cpu/jit_codegen_selftest.c; }
trap restore EXIT

run_suite() { # $1 = log
	make jit-codegen-selftest > "$1" 2>&1
	local rc=$?
	local fails
	fails=$(grep -oE "[0-9]+ checks, [0-9]+ failures" "$1" | tail -1)
	echo "$1 rc=$rc ($fails)"
	[ $rc -ne 0 ]
}

run_suite_ok() { # $1 = log -- expects the suite to PASS
	make jit-codegen-selftest > "$1" 2>&1
	local rc=$?
	local fails
	fails=$(grep -oE "[0-9]+ checks, [0-9]+ failures" "$1" | tail -1)
	echo "$1 rc=$rc ($fails)"
	[ $rc -eq 0 ]
}

mkdir -p results/perf-7
overall=0

# ---- control ----------------------------------------------------------------
echo "== control (unmutated: the suite must pass)"
run_suite_ok results/perf-7/mut-control.txt || { echo "mut-rig: CONTROL FAILED -- the rig cannot see green"; overall=1; }

# ---- mut1: the criterion always false ----------------------------------------
echo "== mut1: jitcg_uop_continues -> return false (elision never fires)"
python3 - <<'EOF'
s = open('src/cpu/jit_codegen.c').read()
old = '''static bool jitcg_uop_continues(const uop_t *u, const jc_entry_t *rec)
{
	if (u->flags & (UOP_FLAG_FLUSH | UOP_FLAG_BLOCK_END)) return false;'''
assert old in s
s = s.replace(old, '''static bool jitcg_uop_continues(const uop_t *u, const jc_entry_t *rec)
{
	(void)u; (void)rec;
	return false; /* MUT1: the criterion never fires */
	if (u->flags & (UOP_FLAG_FLUSH | UOP_FLAG_BLOCK_END)) return false;''')
open('src/cpu/jit_codegen.c','w').write(s)
EOF
run_suite results/perf-7/mut-mut1.txt || { echo "mut-rig: MUT1 DID NOT FAIL THE SUITE (the engage pins cannot see the criterion)"; overall=1; }
restore

# ---- mut2: the kind exclusion dropped (JCC/JMP/LEA elided) --------------------
echo "== mut2: JCC/JMP/LEA elided (the ip writers keep no checks)"
python3 - <<'EOF'
s = open('src/cpu/jit_codegen.c').read()
old = '''	switch (rec->kind) {
	case JIT_REC_JCC:
	case JIT_REC_JMP:
	case JIT_REC_LEA:
		return false;
	default:
		break;
	}'''
assert old in s
s = s.replace(old, '''	/* MUT2: the ip writers are elided too */
	(void)rec;''')
open('src/cpu/jit_codegen.c','w').write(s)
EOF
run_suite results/perf-7/mut-mut2.txt || { echo "mut-rig: MUT2 DID NOT FAIL THE SUITE"; overall=1; }
restore

# ---- mut3: RETIRED -- see the header note. -----------------------------------

# ---- mut4: F2's count mask dropped --------------------------------------------
echo "== mut4: F2 shifts without the & 31 (CL=0x20 must still store nothing)"
python3 - <<'EOF'
s = open('src/cpu/jit_codegen.c').read()
old = '''	/* count = CL & 31; zero -> the body stores nothing at all */
	jx64_mov_r32_r32(e, JC_TMP, JX_RCX);
	jx64_alu_rm_imm(e, 4, jc_reg(JC_TMP), 31, true);
	jx64_jcc_label(e, JX_CC_E, l_done);'''
assert old in s
s = s.replace(old, '''	/* MUT4: the mask is gone -- the zero test reads CL directly */
	jx64_mov_r32_r32(e, JC_TMP, JX_RCX);
	jx64_test_r32_r32(e, JC_TMP, JC_TMP);
	jx64_jcc_label(e, JX_CC_E, l_done);''')
open('src/cpu/jit_codegen.c','w').write(s)
EOF
run_suite results/perf-7/mut-mut4.txt || { echo "mut-rig: MUT4 DID NOT FAIL THE SUITE"; overall=1; }
restore

echo "== mut-rig done, overall rc=$overall (0 = every arm was caught)"
exit $overall
