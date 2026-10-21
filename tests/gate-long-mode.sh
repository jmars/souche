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
	--expect counts --expect-counts "green=0 red_unimplemented=63 mismatch=0 error=0" --quiet
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
# MEASURED at S7: 136 overlay entries, 96 routed arms, 38 unimplemented call sites.  The 38 are
# S8's group (mul/imul/div/idiv in 0xF7, the shifts), S9's (xadd/cmpxchg/bt/bsf), S10's (the
# SSE/movd/cvt 0F encodings) and the 0x81 imm32 ALU group S6 left uninstrumented; S5 measured
# 102/61/23 and S6 117/76/29 -- each slice's move is in its own report.  The 23 are
# the default-64 named stops (ret/call/jmp/pop r-m/iret) that S6-S11 replace; a slice that
# lands one EDITS THIS LINE, which is the record of what it landed.
step "overlay-census" sh -c \
	"node gen/interpreter_c.js 2>&1 | grep -q 'long mode: 146 overlay entries, 106 routed to instr64 bodies, 34 of those call sites unimplemented'"
must_fail "overlay-census-able-to-fail" sh -c \
	"node gen/interpreter_c.js 2>&1 | grep -q 'long mode: 0 overlay entries'"

echo "== E. FAMILY 1 GREEN, 12/12 -- the ACTUAL count; and the whole corpus has ZERO mismatches"
step "family-1-green" $PYTHON tests/nasm64_run.py --stage run --actual "$ACTUAL_ON" \
	--filter '^mov-' --expect counts \
	--expect-counts "green=12 red_unimplemented=0 mismatch=0 error=0" --quiet
step "whole-corpus-no-mismatch" $PYTHON tests/nasm64_run.py --stage run --actual "$ACTUAL_ON" \
	--expect counts --expect-counts "green=60 red_unimplemented=3 mismatch=0 error=0" --quiet
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
# (4b) the PD base is repointed ABOVE guest RAM (MEMORY_SIZE = 0x200000, src/main.c:68): a
# page-table read must be BOUNDED by memory_size, not handed to read64s -- the second HOST
# out-of-bounds read this slice found (results/x86-64-s5.md ss6.6).  The patched instruction is
# the stub's `mov dword ptr [0x106000], 0x107003` (PD[0] -> PT0); the mutation sets the base to
# 0x200000, so the PT read for the body's page computes 0x200480 and the walk must STOP BY NAME
# instead of reading the host's heap.
patch pt-base-past-ram c7050060100003701000 c7050060100003002000
step "walk-table-read-past-ram" sh -c \
	"$PORT64 $MUT/pt-base-past-ram.img 20000000 --fixture64 --long-mode on 2>&1 | grep -q 'page-table read at physical'"

# (5) the PT fill loop writes a NON-ZERO high dword -> every frame is at 4 GiB: the BOUND.
patch pte-above-4gib c7470400000000 c7470401000000
step "walk-above-4gib-named-stop" sh -c \
	"$PORT64 $MUT/pte-above-4gib.img 20000000 --fixture64 --long-mode on 2>&1 | grep -q \"above the port's 32-bit physical model\""

echo "== F2. ROUND 2: the 64-bit-invalid and S6-owned guards BITE (they were silent before)"
# (i) a BARE 0xEA in the body.  In a 64-bit CS the SDM says #UD; before round 2 it dispatched
#     to `case 0x1EA` -- the REAL far jump -- because dispatch_os_index only answers 2 for 0xEA
#     when a REX.W precedes it.  The patch replaces the prologue's `xor eax,eax; xor ecx,ecx`
#     (31 c0 31 c9) with `ea 90 31 c9`: the same length, and the 0xEA is the first byte executed.
patch bare-far-jump 31c031c9 ea9031c9
#     THE RIP IS PART OF THE ASSERTION, and it is what makes the arm able to fail: a #UD raised
#     ON the 0xEA byte leaves rip = body_start (0x90000), while the OLD behaviour (the real far
#     jump) leaves the machine somewhere else entirely and would ALSO eventually die with some
#     signal.  Asserting only "(signal SIGILL)" was MEASURED to pass against the mutant, i.e. it
#     was an arm that could not fail.
step "bare-0xEA-is-UD-in-64bit" sh -c \
	"$PORT64 $MUT/bare-far-jump.img 20000000 --fixture64 --long-mode on 2>&1 > $MUT/ea.out; \
	 grep -q '(signal SIGILL)' $MUT/ea.out && \
	 grep -q '\"instruction_pointer\": \"0x90000\"' $MUT/ea.out"
#     ... and the OTHER half of the same rule: 0xEA is LEGAL in compatibility mode, which is what
#     every green case already proves (the stub's own far jump enters the body through it).
step "0xEA-is-legal-in-compat" sh -c \
	"$PORT64 $POISON 20000000 --fixture64 --long-mode on | grep -q 'BEGIN JSON'"
# (iii) 0x63: S7 IMPLEMENTED MOVSXD, so the S5-round-2 arm that required the named stop is
#      INVERTED -- the same mutant (`63 90 31 c9`: movsxd r10 with REX-less encoding... note the
#      bare 0x63 is now movsxd r/m32->r32) must EXECUTE, and the able-to-fail form is the stop
#      text that used to be required.
# ROUND 2 (blocker 2): the BARE 0x63 (no REX.W) is MOVSXD at 32 bits on hardware -- NOT ARPL,
# which does not exist in 64-bit mode.  S7's first version deleted S5 round 2's guard with the
# stop64 entry it replaced, so the narrow form executed ARPL; the `narrow64` guard makes it a
# NAMED STOP, and this arm asserts the stop rather than a document.
patch bare-0x63-narrow 31c031c9 639031c9
step "bare-0x63-narrow-stops" sh -c \
	"$PORT64 $MUT/bare-0x63-narrow.img 20000000 --fixture64 --long-mode on 2>&1 | grep -q '0x63 without REX.W is MOVSXD'"
# ... and the REX.W form is the real body: `48 63 d0` is movsxd rdx, eax, which must EXECUTE
patch rexw-0x63 31c031c9 4863d090
step "rexw-0x63-executes" sh -c \
	"$PORT64 $MUT/rexw-0x63.img 20000000 --fixture64 --long-mode on | grep -q 'BEGIN JSON'"
must_fail "rexw-0x63-is-not-the-narrow-stop" sh -c \
	"$PORT64 $MUT/rexw-0x63.img 20000000 --fixture64 --long-mode on 2>&1 | grep -q '0x63 without REX.W'"
# ROUND 2 (should-fix 3): a 0x66-prefixed default-64 encoding.  Hardware HONOURS `66 50` as a
# two-byte push, but this port's 16-bit stack path runs through adjust_stack_reg, i.e.
# write_reg32(ESP, ...), which would ZERO RSP's high half in long mode -- so the narrow form is
# a NAMED STOP (refusing a legal instruction is visible; truncating RSP is not).  The mutant
# turns the prologue's `push 0` (6a 00) into `66 50` (push ax), same length.
patch narrow-66-push 6a00 6650
step "66-prefixed-default64-stops" sh -c \
	"$PORT64 $MUT/narrow-66-push.img 20000000 --fixture64 --long-mode on 2>&1 | grep -q '16-bit operand form of a default-64 encoding'"
# ROUND 2 (blocker 1): the sixteen jcc rel8 bodies must be CALLED, not stopped.  The generated
# file is the artifact to assert on (the review's point: a regex over hand-written SOURCE is
# what broke the availability scan in the first place), and the behavioural proof is
# ctrl-jcc-rexw, whose REX.W jccs take the 0x27x arms.
step "jcc-arms-call-their-bodies" sh -c \
	"test \$(grep -c 'instr64_7[0-9A-F](imm)' src/cpu/interpreter_gen.c) -eq 16"
step "ctrl-jcc-rexw" $PYTHON tests/nasm64_run.py --stage run --actual "$ACTUAL_ON" \
	--filter 'jcc-rexw' --expect counts \
	--expect-counts "green=1 red_unimplemented=0 mismatch=0 error=0" --quiet
# (ii) S6 REPLACED THE ALU STOPS WITH REAL BODIES, so the S5 arm that required `48 31 c0` to
#      stop by name is now INVERTED: the same mutant must EXECUTE (a document, no stop).  Its
#      able-to-fail form is the stop text that used to be required -- if a body ever regresses
#      to `INSTR64_ALU_IS_S6`, this pair goes red.
patch rexw-xor 31c031c9 4831c090
step "rexw-xor-executes" sh -c \
	"$PORT64 $MUT/rexw-xor.img 20000000 --fixture64 --long-mode on | grep -q 'BEGIN JSON'"
must_fail "rexw-xor-no-longer-stops" sh -c \
	"$PORT64 $MUT/rexw-xor.img 20000000 --fixture64 --long-mode on 2>&1 | grep -q 'at 64 bits is S6'"

echo "== F3. S6: THE ALU FAMILY AT 64 BITS, WITH ITS FLAGS (family 2, case by case)"
# 9 of the 10 cases are green against the HOST; alu-div-zero needs DIV, which is S8's, and is
# therefore RED BY NAME.  The counts are the assertion -- family 2's own census.
step "alu-family" $PYTHON tests/nasm64_run.py --stage run --actual "$ACTUAL_ON" \
	--filter '^alu-' --expect counts \
	--expect-counts "green=11 red_unimplemented=0 mismatch=0 error=0" --quiet
must_fail "alu-family-counts-able-to-fail" $PYTHON tests/nasm64_run.py --stage run \
	--actual "$ACTUAL_ON" --filter '^alu-' --expect counts \
	--expect-counts "green=12 mismatch=0" --quiet
# ROUND 2'S BLOCKER, AS ITS OWN STEP: alu-pop-rsp is the case that would have caught the
# 0x8F /0 pre-adjust direction (round 1 decremented; POP increments), and its asymmetry is
# MEASURED -- the old direction leaves [0x101DF0] = 0xAAAA... (mismatch), the fix stores at
# [0x101E00] and the case is green.
step "alu-pop-rsp-green" $PYTHON tests/nasm64_run.py --stage run --actual "$ACTUAL_ON" \
	--filter 'pop-rsp' --expect counts \
	--expect-counts "green=1 red_unimplemented=0 mismatch=0 error=0" --quiet

echo "== F4. S7: CONTROL FLOW AND STACK AT 64 BITS (family 3, 12/12 -- ALL GREEN)"
step "ctrl-family" $PYTHON tests/nasm64_run.py --stage run --actual "$ACTUAL_ON" \
	--filter '^ctrl-' --expect counts \
	--expect-counts "green=13 red_unimplemented=0 mismatch=0 error=0" --quiet
must_fail "ctrl-family-counts-able-to-fail" $PYTHON tests/nasm64_run.py --stage run \
	--actual "$ACTUAL_ON" --filter '^ctrl-' --expect counts \
	--expect-counts "green=12 mismatch=0" --quiet
# the call/ret pair is the slice's stack-discipline spine: an EIGHT-byte return address, and
# the far transfer actually returning.  Assert the case's own counts as a named step.
step "ctrl-call-ret" $PYTHON tests/nasm64_run.py --stage run --actual "$ACTUAL_ON" \
	--filter 'call-ret' --expect counts \
	--expect-counts "green=1 red_unimplemented=0 mismatch=0 error=0" --quiet

echo "== F5. S8: MUL/DIV/SHIFTS AT 64 BITS (family 4, 11/11 -- ALL GREEN)"
step "muldiv-family" $PYTHON tests/nasm64_run.py --stage run --actual "$ACTUAL_ON" \
	--filter '^muldiv-' --expect counts \
	--expect-counts "green=13 red_unimplemented=0 mismatch=0 error=0" --quiet
must_fail "muldiv-family-counts-able-to-fail" $PYTHON tests/nasm64_run.py --stage run \
	--actual "$ACTUAL_ON" --filter '^muldiv-' --expect counts \
	--expect-counts "green=12 mismatch=0" --quiet
# the 128-bit product is the slice's headline: mul-128's own counts, so a regression in the high
# half is a NAMED step rather than a corpus statistic
step "mul-128-high-half" $PYTHON tests/nasm64_run.py --stage run --actual "$ACTUAL_ON" \
	--filter 'mul-128' --expect counts \
	--expect-counts "green=1 red_unimplemented=0 mismatch=0 error=0" --quiet
# and the #DE case is the DIVIDE-ERROR path (alu-div-zero is S6's fault case; this asserts that
# the div-family's own error path is exercised at 64 bits)
step "div64-runs" $PYTHON tests/nasm64_run.py --stage run --actual "$ACTUAL_ON" \
	--filter 'muldiv-div-64' --expect counts \
	--expect-counts "green=1 red_unimplemented=0 mismatch=0 error=0" --quiet
# ROUND 2'S TWO NEW INSTRUMENTS, each with its mutant MEASURED: the rotate's "unaffected flags"
# rule (a clobbering rol64 turns it red) and IDIV's QUOTIENT-overflow #DE (deleting the range
# check turns it red).  A named step each, so a regression is not a corpus statistic.
step "rol-preserves-flags" $PYTHON tests/nasm64_run.py --stage run --actual "$ACTUAL_ON" \
	--filter 'rol-preserves' --expect counts \
	--expect-counts "green=1 red_unimplemented=0 mismatch=0 error=0" --quiet
step "idiv-overflow-DE" $PYTHON tests/nasm64_run.py --stage run --actual "$ACTUAL_ON" \
	--filter 'idiv-overflow' --expect counts \
	--expect-counts "green=1 red_unimplemented=0 mismatch=0 error=0" --quiet

# the ONE red case must be red for the named reason (div is S8's), not for a silent mismatch
# S8 landed DIV, so the S6-era arm that required a named stop on instr64_F7_6 is INVERTED: the
# fault case must now reach the #DE (a SIGFPE document), not a stop.
step "alu-div-zero-reaches-DE" sh -c \
	"$PORT64 build/nasm64/alu-div-zero.img 20000000 --fixture64 --long-mode on 2>&1 | grep -q '(signal SIGFPE)'" 

echo "== F6. S9: STRING / BIT / ATOMIC AT 64 BITS (family 5, 7/7)"
step "strobits-family" $PYTHON tests/nasm64_run.py --stage run --actual "$ACTUAL_ON" \
	--filter '^strobits-' --expect counts \
	--expect-counts "green=8 red_unimplemented=0 mismatch=0 error=0" --quiet
must_fail "strobits-family-counts-able-to-fail" $PYTHON tests/nasm64_run.py --stage run \
	--actual "$ACTUAL_ON" --filter '^strobits-' --expect counts \
	--expect-counts "green=7 mismatch=0" --quiet
# the 16-byte atomic and the string Q forms each get their own named step: they are the two
# places a 32-bit width would hide (a 4-byte stosq, an 8-byte cmpxchg16b)
step "cmpxchg16b-16-byte" $PYTHON tests/nasm64_run.py --stage run --actual "$ACTUAL_ON" \
	--filter 'cmpxchg16b' --expect counts \
	--expect-counts "green=1 red_unimplemented=0 mismatch=0 error=0" --quiet
# ROUND 2's two blockers, each a named step with its mutant MEASURED: the BT memory IMMEDIATE
# offset is masked to the operand width (unmasking turns it red), and a BARE string op must not
# touch RCX (a clobbering body turns it red).
step "bt-mem-imm-masked" $PYTHON tests/nasm64_run.py --stage run --actual "$ACTUAL_ON" \
	--filter 'bt-mem' --expect counts \
	--expect-counts "green=1 red_unimplemented=0 mismatch=0 error=0" --quiet
step "bare-string-preserves-rcx" $PYTHON tests/nasm64_run.py --stage run --actual "$ACTUAL_ON" \
	--filter 'bare-string' --expect counts \
	--expect-counts "green=1 red_unimplemented=0 mismatch=0 error=0" --quiet
step "rep-stosq-movsq" $PYTHON tests/nasm64_run.py --stage run --actual "$ACTUAL_ON" \
	--filter 'stos-movs' --expect counts \
	--expect-counts "green=1 red_unimplemented=0 mismatch=0 error=0" --quiet

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
