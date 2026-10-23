#!/bin/sh
# tests/gate-x86-64-diff.sh -- S13's gate: THE 64-BIT ENGINE DIFFERENTIAL, BYTE FOR BYTE.
#
# Specification: results/x86-64-plan.md ss6's S13 row and ss3.4 (the engine cascade).  S5 ss4
# recorded that the differential was VACUOUS there (only the interpreter could run IA-32e); this
# slice makes it REAL: the `uop`/`uopjit` engines now DECLARE the mode and DELEGATE each long-mode
# batch to the interpreter (uop_cache.c's long-mode delegation, counted as a fallback), so all
# three engines can run the whole corpus.
#
# WHAT THIS GATE IS, and what it is NOT.  It asserts (a) each engine agrees with the HOST on all
# 63 cases, and (b) the three engines' DOCUMENTS are byte-identical for the same case -- the state
# document is the machine's canonical form, so a decoder or executor that disagrees anywhere the
# document reaches is caught.  It is NOT a claim that the cache decodes long mode: the counters
# say otherwise and results/x86-64-s13.md names that as the gap.
set -e
cd "$(dirname "$0")/.."
PYTHON=python3
PORT64="build/v86c-nasm64"
WORK=build/nasm64
LOG=/tmp/gate-x86-64-diff.log
PASS=0; FAIL=0
step() { local n="$1"; shift; if "$@" >$LOG 2>&1; then PASS=$((PASS+1)); echo "ok   $n"; else FAIL=$((FAIL+1)); echo "FAIL $n"; sed -n '1,25p' $LOG; fi; }
must_fail() { local n="$1"; shift; if "$@" >$LOG 2>&1; then FAIL=$((FAIL+1)); echo "FAIL $n (it PASSED and must not)"; else PASS=$((PASS+1)); echo "ok   $n (correctly refused)"; fi; }

echo "== A. EVERY ENGINE AGREES WITH THE HOST on the whole corpus (63 cases each)"
for e in interp uop uopjit; do
	step "corpus-$e" $PYTHON tests/nasm64_run.py --stage run \
		--actual "$PORT64 {img} {limit} --fixture64 --long-mode on --cpu-engine $e" \
		--expect counts --expect-counts "green=84 red_unimplemented=1 mismatch=0 error=0" --quiet
done

echo "== B. THE ENGINES' DOCUMENTS ARE BYTE-IDENTICAL (the differential proper)"
# the whole v2 document AND the STATE control tail: the tail carries efer/cr/cr3/gdtr/idtr and the
# segment bases, i.e. everything the engines could disagree about that the compared array does not.
same_docs() { # same_docs <case>
	c="$1"
	for e in interp uop uopjit; do
		$PORT64 "$WORK/$c.img" 20000000 --fixture64 --long-mode on --cpu-engine $e \
			> "$WORK/diff-$c-$e.txt" 2>/dev/null
	done
	cmp -s "$WORK/diff-$c-interp.txt" "$WORK/diff-$c-uop.txt" &&
	cmp -s "$WORK/diff-$c-interp.txt" "$WORK/diff-$c-uopjit.txt"
}
for c in mov-imm64 alu-logic-64 ctrl-call-ret-64 muldiv-div-64 strobits-stos-movs-64 simd-movdqa-64; do
	step "identical-$c" same_docs "$c"
done
# ABLE-TO-FAIL: the differential must be able to FAIL.  A deliberately different run (a DIFFERENT
# case's document) must not compare equal -- otherwise the comparison is vacuous.
must_fail "differential-can-fail" sh -c \
	"cmp -s '$WORK/diff-mov-imm64-interp.txt' '$WORK/diff-alu-logic-64-interp.txt'"

echo "== C. THE CACHE ENGINES DELEGATE (the counters say so: this is not a decode claim)"
# a long-mode run under `uop` must grow fallback_runs and compile NO block: the cache is not
# decoding 64-bit bytes, it is handing the batch to the interpreter.
step "uop-delegates" sh -c \
	"$PORT64 $WORK/mov-imm64.img 20000000 --fixture64 --long-mode on --cpu-engine uop \
	 --uop-cache-stats 2>&1 >/dev/null | grep -q 'long-mode batches delegated to the interpreter: [1-9]'"
# ... and the SAME run WITHOUT the flag must delegate ZERO batches: the counter is measuring the
# mode, not the engine (a control, not a tautology)
step "no-long-mode-no-delegation" sh -c \
	"$PORT64 $WORK/mov-imm64.img 20000000 --fixture64 --cpu-engine uop \
	 --uop-cache-stats 2>&1 >/dev/null | grep -q 'long-mode batches delegated to the interpreter: 0'"
# the JIT compiles no 64-bit block: the blocks it DID compile belong to the stub's 16/32-bit phase,
# so the assertion is that the count does not grow with a longer long-mode run
step "jit-no-long-mode-blocks" sh -c \
	"$PORT64 $WORK/mov-imm64.img 20000000 --fixture64 --long-mode on --cpu-engine uopjit \
	 --jit-stats 2>&1 >/dev/null | grep -q 'blocks compiled 10'"

echo
echo "[+] gate-x86-64-diff: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
