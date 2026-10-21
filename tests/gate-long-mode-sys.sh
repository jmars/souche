#!/bin/sh
# tests/gate-long-mode-sys.sh -- S11's gate: THE SYSTEM LAYER, ONE MUTATION PROBE PER RULE.
#
# Specification: results/x86-64-plan.md ss6's S11 row ("the full 4-level walk with 1 GiB +
# canonicality + NX, ... gated by the synthetic image with a mutation probe per rule") and ss2.2
# (the per-item table).  The instrument is the corpus stub: its page tables are BUILT AT RUNTIME
# by the image's own code, so a probe patches the STUB'S IMMEDIATES (tests/patch-image.py, which
# requires each pattern to occur EXACTLY once) and the run's outcome is the verdict.
#
# A RULE WITH NO PROBE IS A NAMED GAP, NOT A PROMISE -- results/x86-64-s11.md ss5 lists them.
set -e
cd "$(dirname "$0")/.."
PYTHON=python3
PORT64="build/v86c-nasm64"
WORK=build/nasm64
MUT=$WORK/sys
POISON=$WORK/mov-imm64.img
PASS=0; FAIL=0
step() { local name="$1"; shift; if "$@" >/tmp/sys.log 2>&1; then PASS=$((PASS+1)); echo "ok   $name"; else FAIL=$((FAIL+1)); echo "FAIL $name"; sed -n '1,20p' /tmp/sys.log; fi; }
must_fail() { local name="$1"; shift; if "$@" >/tmp/sys.log 2>&1; then FAIL=$((FAIL+1)); echo "FAIL $name (it PASSED and must not)"; else PASS=$((PASS+1)); echo "ok   $name (correctly refused)"; fi; }
patch() { mkdir -p $MUT; cp $POISON "$MUT/$1.img"; $PYTHON tests/patch-image.py "$MUT/$1.img" "$2" "$3" >/dev/null || exit 1; }

echo "== R1. THE 1 GiB PAGE: PDPTE.PS composes a 1 GiB identity map (S5 stopped by name here)"
# the stub's PDPT[0] install: `mov dword ptr [0x105000], 0x106003` -> `... 0x83` (PS, frame 0)
patch pdpt-1gib c7050050100003601000 c7050050100083000000
step "1gib-identity-runs" sh -c \
	"$PORT64 $MUT/pdpt-1gib.img 20000000 --fixture64 --long-mode on > $MUT/1g.out 2>&1; \
	 grep -q 'BEGIN JSON' $MUT/1g.out && ! grep -q '(signal SIG' $MUT/1g.out"
# ... and the unmutated image still runs (the probe is not measuring a broken machine)
step "baseline-runs" sh -c "$PORT64 $POISON 20000000 --fixture64 --long-mode on | grep -q 'BEGIN JSON'"

echo "== R2. CANONICALITY: a non-canonical linear address is #GP, not an ALIAS (S5's defect)"
# the body's first 10 bytes become `mov rax, [0x0000800000000000]` (48 A1 + the 8-byte offset) --
# a non-canonical address whose low 48 bits are ZERO, i.e. the page the identity map makes MAPPED:
# an aliasing walk translates it successfully, the canonical check faults.
# `mov rax, 0x0000800000000000; mov rax, [rax]` -- the address is in a REGISTER because a moffs64
# above 4 GiB hits the port's own moffs BOUND before the walk (MEASURED: the first version of this
# probe was green against the canonicality mutant for that reason).
patch noncanonical 31c031c931d231db31ed31f631ff 48b80000000000800000488b0090
step "noncanonical-faults" sh -c \
	"$PORT64 $MUT/noncanonical.img 20000000 --fixture64 --long-mode on 2>&1 | grep -q '(signal SIG'"
# ABLE-TO-FAIL: the same 10-byte shape with a CANONICAL address (0x100000, low 48 bits) must run
patch canonical 31c031c931d231db31ed31f631ff 48b80000100000000000488b0090
step "canonical-runs" sh -c \
	"$PORT64 $MUT/canonical.img 20000000 --fixture64 --long-mode on > $MUT/can.out 2>&1; \
	 grep -q 'BEGIN JSON' $MUT/can.out && ! grep -q '(signal SIG' $MUT/can.out"

echo "== R3. THE FS-BASE MSR: wrmsr(0xC0000100) moves the base the address generator reads"
# the prologue's first 14 bytes become `mov ecx,0xC0000100; mov eax,0x12345678; xor edx,edx; wrmsr`
patch fs-base 31c031c931d231db31ed31f631ff4531 b9000100c0b87856341231d20f309090
step "fs-base-written" sh -c \
	"$PORT64 $MUT/fs-base.img 20000000 --fixture64 --long-mode on 2>/dev/null \
	 | grep -q '\"base\": \"0x12345678\"'"

echo
echo "[+] gate-long-mode-sys: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
