#!/bin/sh
# tests/gate-nasm64.sh -- S4's gate: THE HOST-CORPUS INFRASTRUCTURE IS TRUSTWORTHY.
#
# This gate is NEGATIVE ON PURPOSE (results/x86-64-plan.md ss6 S4, ss2.1.7).  The machine
# cannot execute a 64-bit image yet (S5 is the first slice that can), so a corpus that
# passed today would have tested nothing.  What S4 has to prove is that the instrument
# WOULD catch a disagreement, that its own parts are alive, and that its VERDICT cannot be
# fooled -- including by progress.
#
#   A. THE DOCUMENT SHAPE (`--stage shape`): `--fixture` and `--fixture64` are one
#      printer; one 32-bit image run both ways must give 2116/2124 elements, the same eight
#      registers, R8-R15 zero (the seat), the same rip, and an identical
#      x87/MMX/XMM/memory/flags block.  Without this the v2 printer is a seat only S5
#      could sit in.
#   B. THE HARNESS'S OWN CONTROL (`--stage control`): the hand-written
#      tests/nasm64_control.asm, whose 8 KiB window, sixteen GPRs, flag word, x87 value,
#      tag word and status word are re-derived in PYTHON from the source's constants.
#   B2. THE WITNESS RULE'S OWN CONTROL (`gen/nasm64_gen.py --witness-selftest`): the
#      battery that made the rule sound in round 2 -- the measured false accepts (a wholly
#      32-bit body), the zero-padded literal, the decimal at or above 2^32, and the
#      positive classes.
#   C1. THE COMPARISON CAN PASS: with the native harness as the actual leg, all 57 cases
#      must AGREE.  A comparison that cannot pass cannot be said to fail for the right
#      reason.
#   C2-C4. THE COMPARISON BITES: one 64-bit word of the first GPR, one 32-bit word of the
#      compared memory window, and the rip are each perturbed in every fixture, and every
#      comparable case must MISMATCH NAMING THAT FIELD.  (The fault cases are excluded
#      from C2/C3 because the comparison compares the exception kind and the faulting rip
#      only for them -- run.js:497-505 -- and are C4's subject.)
#   D. THE RED CENSUS, AS COUNTS: every corpus case run against the CURRENT binary must
#      stop with a named `v86c: unimplemented: <symbol>`; the SUMMARY NUMBERS are asserted
#      (green=0 red_unimplemented=61 mismatch=0 error=0 -- 58 since S6 added the
#      alu-pop-rsp case), NOT a shape.  Round 2, fix 6:
#      asserting "the stop exists" cannot tell progress from catastrophe (MEASURED: an
#      `--expect any`-style permissive verdict returns rc 0 with mismatch=55/55).
#   D2. THE COUNT ASSERTION'S ABLE-TO-FAIL ARM: the same step with DELIBERATELY WRONG
#      counts must FAIL.  An assertion that cannot fail is not an assertion.
#   D3. THE NOT-RED ARM (round 2's BLOCKER, B1): a bare `--stage run` against a leg that
#      PRODUCES DOCUMENTS must return a SUMMARY and green=61.  Before the fix this raised
#      UnboundLocalError at the first case that stopped being red -- which is exactly the
#      form step D has -- so the harness would have been dead on arrival for S5.
#
# S5 UPDATE (results/x86-64-s5.md), and this gate's numbers DELIBERATELY DID NOT MOVE.  The
# port can now execute these images (`--long-mode on`), but that flag is DEFAULT OFF and this
# gate runs the shipped default -- so step D's census is still green=0 red_unimplemented=57,
# word for word what S4 measured.  That is the byte-identical-32-bit-machine requirement
# holding, not a missing edit.  The SAME corpus with the flag ON is measured by
# tests/gate-long-mode.sh: family 1 green 12/12 (the ACTUAL count; the plan's ">= 25" is
# reconciled in the report), the whole corpus green=17 red_unimplemented=40 mismatch=0 error=0.
#
# EVERY STEP'S EXIT CODE IS THE GATE (the tree's rule).  A step's tail is not read.
set -e
cd "$(dirname "$0")/.."

PYTHON=python3
ACTUAL_PORT="build/v86c-nasm64 {img} {limit} --fixture64"
ACTUAL_NATIVE="build/mkfixture64 {img} {body} {loop}"
LOG=/tmp/gate-nasm64-step.log
PASS=0
FAIL=0

step() { # step <name> <cmd...>
	local name="$1"; shift
	if "$@" >$LOG 2>&1; then
		PASS=$((PASS+1))
		echo "ok   $name"
	else
		FAIL=$((FAIL+1))
		echo "FAIL $name"
		sed -n '1,40p' $LOG
	fi
}

must_fail() { # must_fail <name> <cmd...> -- the able-to-fail arm of an assertion
	local name="$1"; shift
	if "$@" >$LOG 2>&1; then
		FAIL=$((FAIL+1))
		echo "FAIL $name (it PASSED, and it must not: the assertion cannot fail)"
	else
		PASS=$((PASS+1))
		echo "ok   $name (correctly refused)"
	fi
}

echo "== A. the v2 document's shape (--fixture vs --fixture64, one 32-bit image)"
step "shape" $PYTHON tests/nasm64_run.py --stage shape

echo "== B. the native harness's own control (hand-written body + an independent model)"
step "control" $PYTHON tests/nasm64_run.py --stage control

echo "== B2. the witness rule's own battery (the round-2 false accepts must be refused)"
step "witness-selftest" $PYTHON gen/nasm64_gen.py --witness-selftest

echo "== C1. the comparison CAN pass: native leg against native fixture, 57 cases"
step "baseline-all-green" $PYTHON tests/nasm64_run.py --stage all --actual "$ACTUAL_NATIVE" \
	--expect all-green --quiet

echo "== C2. one perturbed register word per fixture: every case must mismatch, naming it"
$PYTHON tests/nasm64_run.py --stage fixtures --perturb-field reg \
	--perturb-dir build/nasm64/perturb-reg --quiet >/tmp/gate-nasm64-fixtures.log 2>&1 \
	|| { echo "FAIL perturb-reg fixtures"; exit 1; }
step "perturb-reg" $PYTHON tests/nasm64_run.py --stage run --reuse-fixtures \
	--fixture-dir build/nasm64/perturb-reg --exclude '(div-zero|ud2|idiv-overflow)' \
	--actual "$ACTUAL_NATIVE" --expect all-failing --expect-field 'reg64[0]' --quiet

echo "== C3. one perturbed comparison-window word per fixture: every case must mismatch"
$PYTHON tests/nasm64_run.py --stage fixtures --perturb-field mem \
	--perturb-dir build/nasm64/perturb-mem --quiet >/tmp/gate-nasm64-fixtures.log 2>&1 \
	|| { echo "FAIL perturb-mem fixtures"; exit 1; }
step "perturb-mem" $PYTHON tests/nasm64_run.py --stage run --reuse-fixtures \
	--fixture-dir build/nasm64/perturb-mem --exclude '(div-zero|ud2|idiv-overflow)' \
	--actual "$ACTUAL_NATIVE" --expect all-failing --expect-field 'mem[100000]' --quiet

echo "== C4. one perturbed rip in the FAULT cases (the only cases whose rip is compared)"
$PYTHON tests/nasm64_run.py --stage fixtures --perturb-field rip \
	--perturb-dir build/nasm64/perturb-rip --quiet >/tmp/gate-nasm64-fixtures.log 2>&1 \
	|| { echo "FAIL perturb-rip fixtures"; exit 1; }
step "perturb-rip" $PYTHON tests/nasm64_run.py --stage run --reuse-fixtures \
	--fixture-dir build/nasm64/perturb-rip --filter '(div-zero|ud2|idiv-overflow)' \
	--actual "$ACTUAL_NATIVE" --expect all-failing --expect-field 'exception rip' --quiet

echo "== C5. the EMPTIED-STACK arm (round 2, fix 2): a stack popped empty must read all-invalid"
# simd-fild-fistp64 ends with `fistp`, so the x87 stack is EMPTY while its slots still hold
# deterministic residue.  The document must carry the all-invalid sentinel -- which is what
# makes the comparison's MMX branch FIRE -- and every st slot must say "invalid".  Round 1
# reported 0x0AAA with two registers "live" and compared this case's mm words with nothing.
stack_arm() { # stack_arm <expected-tag-low>
	$PYTHON - "$1" <<'PYEOF'
import json, re, sys
want = int(sys.argv[1], 16)
text = open("build/nasm64/simd-fild-fistp64.fixture").read()
m = re.search(r"---BEGIN JSON---(.*)---END JSON---", text, re.S)
array = json.loads(re.sub(r"\b(inf|-inf|nan)\b", "0", m.group(1)))
tag = array[2122] & 0xFFFF
st = array[17:25]
ok = tag == want
if ok and want == 0xAAAA:
    ok = all(x == "invalid" for x in st)
print("tag_low=0x%04X st=%s" % (tag, st))
sys.exit(0 if ok else 1)
PYEOF
}
step "emptied-stack-arm" stack_arm 0xAAAA
must_fail "emptied-stack-arm-negative" stack_arm 0x0AAA

echo "== D. THE RED CENSUS AS COUNTS: every case against the CURRENT binary"
step "red-census-counts" $PYTHON tests/nasm64_run.py --stage run --actual "$ACTUAL_PORT" \
	--expect counts --expect-counts "green=0 red_unimplemented=61 mismatch=0 error=0" --quiet
if grep -q '^\[+\] SUMMARY' $LOG; then
	PASS=$((PASS+1))
	echo "ok   red-census printed a SUMMARY"
	grep -E '^\[+\] SUMMARY|unimplemented:' $LOG || true
else
	FAIL=$((FAIL+1))
	echo "FAIL red-census printed no SUMMARY line"
fi

echo "== D2. the count assertion's ABLE-TO-FAIL arm (wrong counts must be refused)"
must_fail "red-census-counts-arm" $PYTHON tests/nasm64_run.py --stage run \
	--actual "$ACTUAL_PORT" --expect counts --expect-counts "green=57 mismatch=0" --quiet

echo "== D3. the NOT-RED arm: a bare --stage run against a document-producing leg"
step "not-red-arm" $PYTHON tests/nasm64_run.py --stage run --actual "$ACTUAL_NATIVE" \
	--expect counts --expect-counts "green=61 mismatch=0 error=0" --quiet

echo
echo "[+] gate-nasm64: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
