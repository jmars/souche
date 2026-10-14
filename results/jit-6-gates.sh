#!/bin/bash
# jit-6-gates.sh -- UNIT JIT-6 (HANDOFF=v86c-jit): BLOCK CHAINING, gated.
# Run as:  bash results/jit-6-gates.sh      (the mutation arms ~2 min, the system gates ~10)
#
#   G1  EVERY NEW GATE IS SHOWN ABLE TO FAIL, by reverting the thing it guards (the project's
#       rule: a regression probe must fail before and pass after).  Each arm is restored from
#       a saved copy and the restore is verified with cmp.
#         G1a the chain never engages  -- the stub's indirect jmp replaced by the plain TAIL
#             fall-back: test_chain's ENGAGE pin must fire (and nothing else), while the
#             GUEST gates stay green -- the chain is an optimisation, not a semantics.
#         G1b the pending count is not carried -- the stub reloads it as 0: the staircase
#             (instruction_counter at the flush) must mismatch in test_chain.
#         G1c the RCX/RDX reloads swapped -- the two link-surviving registers must swap in
#             the stores (test_chain holds them non-zero across every link on purpose).
#         G1d the helper drops the state conjunct TOGETHER WITH the virt conjunct -- the
#             walk for S1 then takes the DECOY the test forced into A's probe slot (same
#             phys, other virt+state) and assertion 1 fires.  The state conjunct ALONE is
#             deliberately NOT armed because it is PROVABLY unobservable: the hash mixes the
#             state into the slot number, so a wrong-state entry is never in the queried
#             probe chain at all, and a decoy that passes the virt+phys compares would have
#             to BE A's key.  The compare is collision protection; the pair is the class the
#             decoy gates.
#         G1e the helper drops the fetch-cache gate -- the probe then computes the GARBAGE
#             translation (eip_phys ^ uncovered-virt), whose key the test has bound a block
#             at, and the miss arm fires.
#       NOT armed: dropping the phys conjunct ALONE.  With the state conjunct intact the
#       decoy is skipped on state, and no layout puts a same-(virt,state) different-phys
#       block in front of the probe; that class is the system gates' (nasm, the boots), and
#       the test's comment documents it beside the cpl3-fold deferral.
#   G2  every existing gate on the FIXED tree: make; make selftest (all suites, counts);
#       make nasm (default, --uop-cache off, --jit on); make transcript-diff + both legs vs
#       the JS reference; the 1.5 G boot's final state + serial byte-identity; make
#       minibios-mem; sh tests/gate-reboot-1.sh; aarch64.
#   G3  the bench is tests/perf-5-ab.sh (interleaved, identity-asserted) -- recorded in
#       results/jit-6-headroom.md, not re-run here.
set -u
cd "$(dirname "$0")/.." || exit 2
OUT=results/jit-6
mkdir -p "$OUT"

WORST_RC=0
note() {
    printf '%-64s rc=%s\n' "$1" "$2" | tee -a "$OUT/gates-summary.txt"
    case "$2" in
        ''|*[!0-9]*) ;;
        *) [ "$2" -gt "$WORST_RC" ] && WORST_RC=$2 ;;
    esac
    return 0
}

: > "$OUT/gates-summary.txt"
echo "== JIT-6 GATES: $(date -u +%Y-%m-%dT%H:%M:%SZ) load=$(cut -d' ' -f1-3 /proc/loadavg)" \
    | tee -a "$OUT/gates-summary.txt"

# the tree as this unit leaves it -- every arm below restores from these copies
cp src/cpu/jit_codegen.c "$OUT/jit_codegen.c.fixed"
cp src/cpu/jit_mem.c "$OUT/jit_mem.c.fixed"

expect_fail() { # $1 label, $2 actual rc -- rc!=0 is the PASS here
    if [ "$2" != 0 ]; then note "$1 -> FAILS as it must" 0
    else note "$1 -> PASSED (NO TEETH)" 1; fi
}

arm_reset() { # $1 file
    cp "$OUT/$1.fixed" "src/cpu/$1"
}

# ---- G1a: the chain never engages (the abort test inverted: the link is never attempted) --
python3 - <<'EOF'
import pathlib
p = pathlib.Path("src/cpu/jit_codegen.c"); s = p.read_text()
old = """	jx64_cmp_m8_imm8(e, JC_ABORT_REG, 0, 0);
	jx64_jcc_label(e, JX_CC_NE, l_kind_tail);"""
new = """	jx64_cmp_m8_imm8(e, JC_ABORT_REG, 0, 0);
	jx64_jcc_label(e, JX_CC_E, l_kind_tail); /* G1a: the link NEVER attempted */"""
assert old in s; p.write_text(s.replace(old, new))
EOF
make jit-codegen-selftest > "$OUT/g1a-codegen.txt" 2>&1
expect_fail "G1a chain never attempted: the engage pin fires" $?
grep -E "FAIL chain" "$OUT/g1a-codegen.txt" | head -2 | tee -a "$OUT/gates-summary.txt"
arm_reset jit_codegen.c

# ---- G1b: the pending count is not carried across the link ------------------------------
python3 - <<'EOF'
import pathlib
p = pathlib.Path("src/cpu/jit_codegen.c"); s = p.read_text()
old = "	jx64_mov_r32_m32(e, JC_PENDING, JX_RSP, JC_FRAME_SPILL_R9);"
new = "	jx64_xor_r32_r32(e, JC_PENDING, JC_PENDING); /* G1b: the count LOST */"
assert old in s; p.write_text(s.replace(old, new))
EOF
make jit-codegen-selftest > "$OUT/g1b-codegen.txt" 2>&1
expect_fail "G1b pending dropped at the link: state mismatch" $?
grep -E "FAIL|mismatch" "$OUT/g1b-codegen.txt" | head -2 | tee -a "$OUT/gates-summary.txt"
arm_reset jit_codegen.c

# ---- G1c: the RCX/RDX reloads swapped ----------------------------------------------------
python3 - <<'EOF'
import pathlib
p = pathlib.Path("src/cpu/jit_codegen.c"); s = p.read_text()
old = """	jx64_mov_r32_m32(e, JX_RCX, JX_RSP, JC_FRAME_SPILL_RCX);
	jx64_mov_r32_m32(e, JX_RDX, JX_RSP, JC_FRAME_SPILL_RDX);"""
new = """	jx64_mov_r32_m32(e, JX_RCX, JX_RSP, JC_FRAME_SPILL_RDX); /* G1c: swapped */
	jx64_mov_r32_m32(e, JX_RDX, JX_RSP, JC_FRAME_SPILL_RCX); /* G1c: swapped */"""
assert old in s; p.write_text(s.replace(old, new))
EOF
make jit-codegen-selftest > "$OUT/g1c-codegen.txt" 2>&1
expect_fail "G1c RCX/RDX reloads swapped: state mismatch" $?
grep -E "FAIL|reg32" "$OUT/g1c-codegen.txt" | head -3 | tee -a "$OUT/gates-summary.txt"
arm_reset jit_codegen.c

# ---- G1d: the helper drops the state conjunct --------------------------------------------
python3 - <<'EOF'
import pathlib
p = pathlib.Path("src/cpu/jit_mem.c"); s = p.read_text()
old = """			if (b->live && b->phys_start == phys && b->virt_start == virt
			    && b->state == state) {
				uop_stats.hits++;"""
new = """			if (b->live && b->phys_start == phys) { /* G1d: virt+state dropped */
				uop_stats.hits++;"""
assert old in s; p.write_text(s.replace(old, new))
EOF
make jit-selftest > "$OUT/g1d-jitselftest.txt" 2>&1
expect_fail "G1d key drops virt+state: the decoy is entered, test_chain_key fires" $?
grep -E "FAIL" "$OUT/g1d-jitselftest.txt" | head -2 | tee -a "$OUT/gates-summary.txt"
arm_reset jit_mem.c

# ---- G1e: the helper drops the fetch-translation-cache gate ------------------------------
python3 - <<'EOF'
import pathlib
p = pathlib.Path("src/cpu/jit_mem.c"); s = p.read_text()
old = """	uint32_t virt = (uint32_t)cpu->instruction_pointer;
	if ((virt & ~0xFFFu) != (uint32_t)cpu->last_virt_eip) return NULL;"""
new = """	uint32_t virt = (uint32_t)cpu->instruction_pointer;"""
assert old in s; p.write_text(s.replace(old, new))
EOF
make jit-selftest > "$OUT/g1e-jitselftest.txt" 2>&1
expect_fail "G1e cache gate dropped: the miss arm fires" $?
grep -E "FAIL" "$OUT/g1e-jitselftest.txt" | head -2 | tee -a "$OUT/gates-summary.txt"
arm_reset jit_mem.c

# ---- every arm restored, byte for byte ----------------------------------------------------
ok=0
cmp -s src/cpu/jit_codegen.c "$OUT/jit_codegen.c.fixed" && cmp -s src/cpu/jit_mem.c "$OUT/jit_mem.c.fixed" && ok=1
note "arms restored (cmp vs the saved copies)" $((1 - ok))

# ---- G2: the system gates on the fixed tree -----------------------------------------------
make > "$OUT/g2-make.txt" 2>&1
note "make" $?
make selftest > "$OUT/g2-selftest.txt" 2>&1
note "make selftest" $?
grep -E "checks, [0-9]+ failures?|checks, 0 failures" "$OUT/g2-selftest.txt" | tail -7 | tee -a "$OUT/gates-summary.txt"
make minibios-mem > "$OUT/g2-minibios.txt" 2>&1
note "make minibios-mem" $?
make nasm > "$OUT/g2-nasm-default.txt" 2>&1
note "make nasm (default)" $?
for leg in "cacheoff:--uop-cache off" "jiton:--jit on"; do
    name=${leg%%:*}; flags=${leg#*:}
    python3 tests/nasm_run.py --stage run \
        --actual "build/v86c-nasm {img} {limit} --fixture $flags" \
        --skip-list tests/nasm_skip_c.txt > "$OUT/g2-nasm-$name.txt" 2>&1
    note "nasm with $flags" $?
    grep -E 'passed|failed|skipped' "$OUT/g2-nasm-$name.txt" | tail -2 | tee -a "$OUT/gates-summary.txt"
done
make transcript-diff > "$OUT/g2-transcript-default.txt" 2>&1
note "make transcript-diff (default)" $?
for leg in "jitoff:--jit off" "jiton:--jit on"; do
    name=${leg%%:*}; flags=${leg#*:}
    ./build/v86c tests/test.img 1000000 $flags > "$OUT/transcript-c-$name.txt" 2>/dev/null
    node oracle/run.mjs tests/test.img 1000000 > "$OUT/transcript-js-$name.txt" 2>/dev/null
    if cmp -s "$OUT/transcript-js-$name.txt" "$OUT/transcript-c-$name.txt"; then
        note "transcript-diff with $flags (vs the JS reference)" 0
    else
        note "transcript-diff with $flags (vs the JS reference)" 1
    fi
done
for mode in off onjit; do
    case "$mode" in
        off)   FLAG="" ;;
        onjit) FLAG="--jit on" ;;
    esac
    ./build/v86c --bzimage tests/images/vmlinuz-i386 1500000000 --cmdline "console=ttyS0" $FLAG \
        > "$OUT/boot-1.5g-$mode-serial.txt" 2> "$OUT/boot-1.5g-$mode-stderr.txt"
    note "1.5 G boot ($mode)" $?
    sed -n 's/^v86c boot: [0-9]* instructions, //p' "$OUT/boot-1.5g-$mode-stderr.txt" \
        | tee -a "$OUT/gates-summary.txt"
done
xa=$(sed -n 's/^v86c boot: [0-9]* instructions, //p' "$OUT/boot-1.5g-off-stderr.txt")
xb=$(sed -n 's/^v86c boot: [0-9]* instructions, //p' "$OUT/boot-1.5g-onjit-stderr.txt")
if [ -n "$xa" ] && [ "$xa" = "$xb" ]; then
    echo "1.5 G final state IDENTICAL: $xa" | tee -a "$OUT/gates-summary.txt"
    note "1.5 G final state identical (off vs --jit on)" 0
else
    echo "1.5 G DIFFER: off=\"$xa\" onjit=\"$xb\"" | tee -a "$OUT/gates-summary.txt"
    note "1.5 G final state identical (off vs --jit on)" 1
fi
if cmp -s "$OUT/boot-1.5g-off-serial.txt" "$OUT/boot-1.5g-onjit-serial.txt"; then
    note "1.5 G serial byte-identical" 0
else
    note "1.5 G serial byte-identical" 1
fi
sha256sum "$OUT"/boot-1.5g-*-serial.txt | tee -a "$OUT/gates-summary.txt"
sh tests/gate-reboot-1.sh > "$OUT/g2-reboot-gate.txt" 2>&1
note "sh tests/gate-reboot-1.sh" $?
tail -2 "$OUT/g2-reboot-gate.txt" | tee -a "$OUT/gates-summary.txt"

mkdir -p "$OUT/aarch64"
# The toolchain's standalone aarch64-linux-cosmo-cc is a ONE-LINE TEXT FILE (no shebang): the
# shell executes it as a script and drops every argument ("no input files"), and it never sets
# the sysroot.  The invocation below is cosmocc's OWN driver line for the aarch64 half
# (bin/cosmocc's CPPFLAGS: -fno-pie -nostdinc -isystem $COSMO/include -include
# libc/integral/normalize.inc), aimed at the driver binary directly.  MEASURED: jit_mem/
# jit_x64/jit_codegen/uop_cache all rc=0 with it; the wrapper route fails on UNTOUCHED files.
AARCH64=/home/jaye/.local/cosmo/bin/aarch64-linux-cosmo-gcc
COSMO_INC=/home/jaye/.local/cosmo/include
for f in jit_mem jit_x64 jit_codegen uop_cache; do
    "$AARCH64" -fno-pie -nostdinc -isystem "$COSMO_INC" -include libc/integral/normalize.inc \
        -std=gnu11 -O2 -Wall -Wextra -foptimize-sibling-calls \
        -finline-functions-called-once -fpatchable-function-entry=0 -Werror \
        -I . -I src -c -o "$OUT/aarch64/$f.o" "src/cpu/$f.c" 2> "$OUT/aarch64/$f.err"
    note "aarch64 compile src/cpu/$f.c" $?
done

# ---- G4: the platform probe: the failure path is a TESTED one ------------------------------
# The probe runs at jit_mem_init on every --jit on start; V86C_JIT_PROBE_FAIL=1 forces its
# failure arm.  What must hold: the run REPORTS the failure (the caps line), disables the
# JIT, and the machine still runs the workload CORRECTLY -- the transcript against the JS
# reference stays byte-identical (the uop cache is the engine, as it was before the JIT).
V86C_JIT_PROBE_FAIL=1 ./build/v86c tests/test.img 1000000 --jit on \
    > "$OUT/probefail-c.txt" 2> "$OUT/probefail-stderr.txt"
note "probe-forced-fail run completes" $?
grep -q "PLATFORM PROBE FAILED" "$OUT/probefail-stderr.txt"
note "probe-fail is REPORTED (the caps line)" $?
grep -q "FORCED to fail" "$OUT/probefail-stderr.txt"
note "probe-fail arm names its hook" $?
node oracle/run.mjs tests/test.img 1000000 > "$OUT/probefail-js.txt" 2>/dev/null
if cmp -s "$OUT/probefail-js.txt" "$OUT/probefail-c.txt"; then
    note "probe-fail machine still correct (transcript == JS reference)" 0
else
    note "probe-fail machine still correct (transcript == JS reference)" 1
fi

echo "== WORST RC ACROSS ALL ROWS: $WORST_RC" | tee -a "$OUT/gates-summary.txt"
exit "$WORST_RC"
