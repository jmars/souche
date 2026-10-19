#!/bin/sh
# tests/gate-long-mode.sh -- S5's gate: THE FIRST LONG-MODE VERTICAL SLICE IS REAL.
#
# Specification: results/x86-64-plan.md ss6's S5 row, ss2.1.7b (RED before green), ss3.6 (the
# decoder's placement), ss5.1 (the physical bound), ss5.2 (the virtual space); results/
# x86-64-s5.md is the record these numbers are measured into.
#
# WHAT THIS GATE PROVES, and each item is a step whose EXIT CODE is the verdict:
#
#   A. THE OFF SWITCH IS THE 32-BIT MACHINE.  With `--long-mode` absent (the default) every
#      corpus image still stops at S2's refusal, BY NAME, with the S4 census numbers
#      (green=0 red_unimplemented=57) -- so the flag OFF is the machine S4's gate measured,
#      and long mode cannot be entered by accident.
#   B. THE CAPABILITY IS THE ENGINE'S, AND AN ENGINE WITHOUT IT REFUSES BY NAME.  `--long-mode
#      on` declares CPU_MODE_LONG on the INTERPRETER and selects it when no engine was NAMED;
#      naming `uopjit` instead is a refusal that names the backend rather than a 32-bit decoder
#      running 64-bit bytes (plan R1: the silent desynchronisation channel).
#   C. THE DECODER'S OWN REFUSAL EXISTS.  `cpu_uop_selftest`'s S5 arm puts EFER.LMA in the
#      state by hand and requires the named stop -- the third layer, which the shipped
#      dispatcher cannot reach today (which is exactly why it needs its own arm).
#   D. THE 0x2NN OVERLAY'S CENSUS IS A MEASUREMENT.  The emitter prints how many encodings it
#      routed to `instr64_*` bodies and how many of those call sites are NAMED STOPS; both
#      numbers are asserted here, so a slice that lands arms has to EDIT them.
#   E. FAMILY 1 IS GREEN AGAINST THE HOST -- 12/12, THE ACTUAL COUNT (the plan's ">= 25" is
#      reconciled in results/x86-64-s5.md ss2: the corpus's family 1 IS 12 cases and the gate
#      uses the real number rather than inflating the corpus), asserted as SUMMARY COUNTS and
#      not as "a stop exists" (S4 measured a permissive verdict returning rc 0 on 55/55
#      mismatches).  The WHOLE corpus is additionally asserted to have ZERO mismatches: every
#      case is either green or a NAMED stop naming the encoding or the slice that owns it.
#   F. THE WALK IS LOAD-BEARING: five mutations of the image's page tables each change the
#      outcome -- PML4[0] not present, the PD's PT base zeroed, PDPTE.PS (1 GiB), PDE.PS
#      (2 MiB), and a PTE frame at or above 4 GiB.  The last three must produce the NAMED
#      stops that say whose slice they are.  A walk that ignored a level would pass the
#      unmutated run and notice none of them.
#   G. THE PHYSICAL BOUND STOPS BY NAME AND NAMES THE FRAME (plan ss5.1), and the unmutated
#      image produces neither that stop nor a fault.
#   H. THE MODE REACHED IS LONG, AND THE 64-BIT ENGINE DIFFERENTIAL IS VACUOUS: the STATE tail
#      says EFER.LMA, and the only engine that can produce that state is the interpreter -- so
#      there is no second engine to differ against until S13 gives the uop cache and the JIT
#      their long-mode arms.  STATED HERE as a step rather than implied in prose.
#
# EVERY STEP IS AN EXIT CODE.  Nothing below reads a step's tail to decide a verdict.
set -e
cd "$(dirname "$0")/.."

PYTHON=python3
PORT64="build/v86c-nasm64"
ACTUAL_OFF="$PORT64 {img} {limit} --fixture64"
ACTUAL_ON="$PORT64 {img} {limit} --fixture64 --long-mode on"
WORK=build/nasm64
POISON=$WORK/mov-imm64.img
LOG=/tmp/gate-long-mode-step.log
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

echo "== A. --long-mode OFF is still the 32-bit machine (S2's refusal, by name, S4's counts)"
step "off-is-32bit" $PYTHON tests/nasm64_run.py --stage run --actual "$ACTUAL_OFF" \
	--expect counts --expect-counts "green=0 red_unimplemented=57 mismatch=0 error=0" --quiet
# a count assertion alone would also pass on 57 of anything, so the census's own line is
# checked: the refusal must NAME the reason
step "off-refusal-names-the-reason" sh -c \
	"$PORT64 $POISON 20000000 --fixture64 2>&1 | grep -q \"EFER.LME asks for IA-32e, and cpu backend 'uopjit' cannot run long mode\""

echo "== B. the capability is the engine's: a NAMED engine without it refuses by name"
step "uopjit-refuses-long-mode" sh -c \
	"$PORT64 $POISON 20000000 --fixture64 --long-mode on --cpu-engine uopjit 2>&1 | grep -q \"cannot run long mode\""
# the able-to-fail arm: the same probe on the interpreter (which CAN run it) finds no refusal
must_fail "uopjit-refusal-is-not-universal" sh -c \
	"$PORT64 $POISON 20000000 --fixture64 --long-mode on 2>&1 | grep -q 'cannot run long mode'"

echo "== C. the uop DECODER's own long-mode refusal (its selftest arm)"
step "uop-decoder-refusal" sh -c \
	"./build/cpu_uop_selftest 2>&1 | grep -q 'PASS S5: the uop decoder refuses long mode'"

echo "== D. the 0x2NN overlay's census (routed arms / named stops) AS NUMBERS"
# MEASURED at S5: 79 overlay entries, 61 routed arms, 23 unimplemented call sites.  The 23 are
# the default-64 named stops (ret/call/jmp/pop r-m/iret) that S6-S11 replace; a slice that
# lands one EDITS THIS LINE, which is the record of what it landed.
step "overlay-census" sh -c \
	"node gen/interpreter_c.js 2>&1 | grep -q 'long mode: 79 overlay entries, 61 routed to instr64 bodies, 23 of those call sites unimplemented'"
must_fail "overlay-census-able-to-fail" sh -c \
	"node gen/interpreter_c.js 2>&1 | grep -q 'long mode: 0 overlay entries'"

echo "== E. FAMILY 1 GREEN, 12/12 -- the ACTUAL count; and the whole corpus has ZERO mismatches"
step "family-1-green" $PYTHON tests/nasm64_run.py --stage run --actual "$ACTUAL_ON" \
	--filter '^mov-' --expect counts \
	--expect-counts "green=12 red_unimplemented=0 mismatch=0 error=0" --quiet
step "whole-corpus-no-mismatch" $PYTHON tests/nasm64_run.py --stage run --actual "$ACTUAL_ON" \
	--expect counts --expect-counts "green=17 red_unimplemented=40 mismatch=0 error=0" --quiet
must_fail "whole-corpus-counts-able-to-fail" $PYTHON tests/nasm64_run.py --stage run \
	--actual "$ACTUAL_ON" --expect counts --expect-counts "green=57 mismatch=0" --quiet

echo "== F. THE WALK IS LOAD-BEARING: five page-table mutations must change the outcome"
# THE MUTATIONS PATCH THE STUB'S OWN IMMEDIATES, because the page tables are BUILT AT RUNTIME by
# the stub -- there is no table in the image file to patch.  Each pattern is one whole
# instruction, and a patch is applied only if the pattern occurs EXACTLY ONCE: that is itself an
# assertion (a corpus stub that changed shape fails here rather than silently mutating nothing).
MUT=$WORK/mut
mkdir -p $MUT
patch() { # patch <name> <old-hex> <new-hex>
	cp $POISON "$MUT/$1.img"
	$PYTHON tests/patch-image.py "$MUT/$1.img" "$2" "$3" || exit 1
}
# (0) the baseline: the unmutated image runs to a document
step "walk-baseline-runs" sh -c \
	"$PORT64 $POISON 20000000 --fixture64 --long-mode on | grep -q 'BEGIN JSON'"
# (1) PML4[0].P = 0: the walk finds no PDPT -> #PF, and NO document.
patch pml4-not-present c7050040100003501000 c7050040100000000000
step "walk-pml4-absent-faults" sh -c \
	"$PORT64 $MUT/pml4-not-present.img 20000000 --fixture64 --long-mode on | grep -q '(signal SIG'"
# (2) PD[0] is repointed at the tail of the identity map (0x109000), so the body's page
# translates ABOVE guest RAM: the fetch bound NAMES it.  (This mutation is also the one that
# found a HOST out-of-bounds read: the port named the fetch and then indexed mem8 with the
# address anyway -- a SIGSEGV of the host with no output.  cpu.c's entry note has the fix.)
patch pt-base-repointed c7050060100003701000 c7050060100003901000
step "walk-pt-level-matters" sh -c \
	"$PORT64 $MUT/pt-base-repointed.img 20000000 --fixture64 --long-mode on 2>&1 | grep -q 'past the end of guest RAM'"
# (3) PDPTE.PS = 1 -> a 1 GiB page: a NAMED stop naming S11.
patch pdpt-1gib c7050050100003601000 c7050050100083601000
step "walk-1gib-named-stop" sh -c \
	"$PORT64 $MUT/pdpt-1gib.img 20000000 --fixture64 --long-mode on 2>&1 | grep -q 'a 1 GiB page (PDPTE.PS) in long mode is S11'"
# (4) PDE.PS = 1 -> a 2 MiB page: a NAMED stop naming S11.
patch pde-2mib c7050060100003701000 c7050060100083701000
step "walk-2mib-named-stop" sh -c \
	"$PORT64 $MUT/pde-2mib.img 20000000 --fixture64 --long-mode on 2>&1 | grep -q 'a 2 MiB page (PDE.PS) in long mode is S11'"
# (5) the PT fill loop writes a NON-ZERO high dword -> every frame is at 4 GiB: the BOUND.
patch pte-above-4gib c7470400000000 c7470401000000
step "walk-above-4gib-named-stop" sh -c \
	"$PORT64 $MUT/pte-above-4gib.img 20000000 --fixture64 --long-mode on 2>&1 | grep -q \"above the port's 32-bit physical model\""

echo "== G. the bound NAMES THE FRAME, and the unmutated image produces neither stop nor fault"
step "above-4gib-names-the-frame" sh -c \
	"$PORT64 $MUT/pte-above-4gib.img 20000000 --fixture64 --long-mode on 2>&1 | grep -q 'the page frame 0x'"
must_fail "baseline-has-no-bound-stop" sh -c \
	"$PORT64 $POISON 20000000 --fixture64 --long-mode on 2>&1 | grep -q \"above the port's 32-bit physical model\""
must_fail "baseline-does-not-fault" sh -c \
	"$PORT64 $POISON 20000000 --fixture64 --long-mode on 2>&1 | grep -q '(signal SIG'"

echo "== H. the mode reached IS long, and the 64-bit engine differential is VACUOUS"
step "state-tail-says-long" sh -c \
	"$PORT64 $POISON 20000000 --fixture64 --long-mode on 2>/dev/null | grep -q '\"mode\": \"long\"'"
step "state-tail-efer-lma" sh -c \
	"$PORT64 $POISON 20000000 --fixture64 --long-mode on 2>/dev/null | $PYTHON -c 'import re,sys
t = sys.stdin.read()
m = re.search(r\"\\\"efer\\\": \\\"0x([0-9a-f]+)\\\"\", t)
sys.exit(0 if m and (int(m.group(1), 16) & 0x400) else 1)'"
# THE DIFFERENTIAL IS VACUOUS, and this is the mechanical form of the claim: the OTHER two
# engines cannot produce a long-mode state at all, so there is no second document to compare.
step "differential-vacuous-uop" sh -c \
	"$PORT64 $POISON 20000000 --fixture64 --long-mode on --cpu-engine uop 2>&1 | grep -q \"cpu backend 'uop' cannot run long mode\""
step "differential-vacuous-jit" sh -c \
	"$PORT64 $POISON 20000000 --fixture64 --long-mode on --cpu-engine uopjit 2>&1 | grep -q \"cpu backend 'uopjit' cannot run long mode\""

echo
echo "[+] gate-long-mode: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
