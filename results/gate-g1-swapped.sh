#!/bin/bash
# jit-fix-1-gates.sh -- UNIT JIT-FIX-1 (HANDOFF=v86c-jit): the blocker fix + the review's four
# should-fixes, gated.  Run as:  bash results/jit-fix-1-gates.sh      (about 5 minutes)
#
#   G1  THE PROBE (the blocker, both helpers): probe.asm (the orchestrator's, read path) and
#       probe2.asm (this unit's: read AND write, with the corruption designed to LAND) --
#       --jit off vs --jit on, on the fixed tree.  Both sources are in results/jit-fix-1/ so
#       the evidence reproduces from the tree.
#   G2  EVERY NEW GATE IS SHOWN ABLE TO FAIL, by reverting the thing it guards (the project's
#       rule: a regression probe must fail before and pass after):
#         G2a the blocker           -- both emitter tokens back to the shipped `true`
#         G2b the span leak         -- the five jit_page_recycle calls removed
#         G2c the jcc array pins    -- jitcg_jcc8_fns[6] and [7] (BE/A) swapped
#         G2d the encoder pins      -- jx64_not_rm32's digit 2 -> 3
#         G2e the TLB_READONLY fold -- the write mask's TLB_READONLY term deleted
#       Each arm is restored from a saved copy and the restore is verified with cmp.
#   G3  every existing gate: make, make selftest (all suites, counts), make nasm (default,
#       --uop-cache off, --jit on), make transcript-diff + the --jit on leg against the JS
#       reference, the 1.5 G boot's final state + serial sha, the aarch64 compile.
#   G4  tests/perf_bench.sh --runs 7, --jit off and --jit on (the ~1.8x claim, re-measured).
set -u
cd "$(dirname "$0")/.." || exit 2
OUT=results/jit-fix-2/gate-g1
mkdir -p "$OUT" "$OUT/aarch64"

# JIT-FIX-1R NICE-TO-HAVE-2: the rows were loud but the EXIT CODE did not aggregate them, so a
# "VARIANT MISSING (rc=2)" row could sit next to a script exit 0 -- the same false-pass shape this
# unit fixed one level down.  `note` now tracks the worst rc and the script exits it.
WORST_RC=0
note() {
    printf '%-56s rc=%s\n' "$1" "$2" | tee -a "$OUT/gates-summary.txt"
    case "$2" in
        ''|*[!0-9]*) ;;
        *) [ "$2" -gt "$WORST_RC" ] && WORST_RC=$2 ;;
    esac
    return 0   # MUST return 0: the script's arms are `A && note OK || note BAD`
               # chains, and a non-zero return here makes BOTH branches fire.
}

: > "$OUT/gates-summary.txt"
echo "== JIT-FIX-1 GATES: $(date -u +%Y-%m-%dT%H:%M:%SZ) load=$(cut -d' ' -f1-3 /proc/loadavg)" \
    | tee -a "$OUT/gates-summary.txt"

# the tree as this unit leaves it -- every revert below restores from these copies
cp src/cpu/jit_codegen.c "$OUT/jit_codegen.c.fixed"
cp src/cpu/jit_mem.c "$OUT/jit_mem.c.fixed"
cp src/cpu/jit_x64.c "$OUT/jit_x64.c.fixed"

# ---- G1: the probes, against the fixed tree --------------------------------------------
for p in probe probe2; do
    as --32 -o "$OUT/$p.o" "$OUT/$p.asm" \
        && ld -m elf_i386 -T tests/test.ld -o "$OUT/$p.elf" "$OUT/$p.o" \
        && objcopy -O binary "$OUT/$p.elf" "$OUT/$p.img"
    note "assemble $p.img" $?
    ./build/v86c "$OUT/$p.img" 100000 --jit off > "$OUT/$p.jitoff.txt" 2>/dev/null
    ./build/v86c "$OUT/$p.img" 100000 --jit on  > "$OUT/$p.jiton.txt"  2>/dev/null
    if diff -u "$OUT/$p.jitoff.txt" "$OUT/$p.jiton.txt" > "$OUT/$p.diff.txt" 2>&1; then
        note "G1 $p: --jit on == --jit off (fixed tree)" 0
    else
        note "G1 $p: --jit on == --jit off (fixed tree)" 1
    fi
done
grep -h '"eax"\|"ecx"\|"edx"\|"esi"\|"edi"\|"ebx"' "$OUT"/probe2.jitoff.txt | tr -d ' ' \
    | tee -a "$OUT/gates-summary.txt"

# ---- G2: the new gates, each ABLE TO FAIL ----------------------------------------------
python3 - "$OUT" <<'PY'
import re, sys
out = sys.argv[1]
def read(p): return open(p).read()
def write(p, s): open(p, 'w').write(s)

# G2a the blocker: the two emitter tokens back to the shipped (dead-mask) form
jc = read(out + '/jit_codegen.c.fixed')
a = 'jx64_alu_rm_imm(e, 4 /* and */, jc_reg(dst), 0xFF, false);'
b = 'jx64_alu_rm_imm(e, 4, jc_reg(tmp), 0xFF, false);'
assert jc.count(a) == 1 and jc.count(b) == 1
reverted = jc.replace(a, a.replace('0xFF, false);', '0xFF, true);')) \
             .replace(b, b.replace('0xFF, false);', '0xFF, true);'))
write(out + '/jit_codegen.c.reverted', reverted)
# the two single-site arms of the same measurement
read_only = jc.replace(b, b.replace('false);', 'true);'))   # the reader fixed, this dead
write_only = jc.replace(a, a.replace('false);', 'true);'))  # this fixed, the reader dead
write(out + '/jit_codegen.c.read-only-fixed', read_only)
write(out + '/jit_codegen.c.write-only-fixed', write_only)
# the three MUST be distinct files, or an arm silently tests another arm's tree (MEASURED: the
# first version of this script dropped the writer replacement and 'reverted' == 'write-only')
assert len({reverted, read_only, write_only}) == 3

# G2b the span leak: remove every recycle this unit added to the refusal paths
jm = read(out + '/jit_mem.c.fixed')
pat = re.compile(r'^\t\tjit_page_recycle\(idx\);.*\n', re.M)
assert len(pat.findall(jm)) == 5, len(pat.findall(jm))
write(out + '/jit_mem.c.leaky', pat.sub('', jm))

# G2c the jcc array pins: swap the rel8 BE and A slots (positional, so it de-inlines one and
# mis-inlines the other -- invisible before this unit's jbe rel8 cases)
s = jc.replace('(void (*)(void))instr32_76, (void (*)(void))instr32_77, (void (*)(void))instr32_78,',
               '(void (*)(void))instr32_77, (void (*)(void))instr32_76, (void (*)(void))instr32_78,')
assert s != jc
write(out + '/jit_codegen.c.jcc-swapped', s)

# G2d the encoder pins: a digit change in an emitter NO call site reaches
jx = read(out + '/jit_x64.c.fixed')
o = '\tput(e, 0xF7);\n\temit_rm(e, 2, o);\n}'
assert jx.count(o) == 1
write(out + '/jit_x64.c.mutated', jx.replace(o, '\tput(e, 0xF7);\n\temit_rm(e, 3, o);\n}'))

# G2e the TLB_READONLY fold: the write's mask term deleted
t = '\t           (write ? TLB_READONLY : 0);'
assert jc.count(t) == 1
write(out + '/jit_codegen.c.no-readonly-fold', jc.replace(t, '\t           0;'))

print("G2 variants written")
PY

g2_arm() { # <name> <variant> <target> <fixed-copy> <suite> <want>   want = pass|fail
    local name="$1" variant="$2" target="$3" fixed="$4" suite="$5" want="$6"
    # THE ANTI-FALSE-PASS GUARD, and it is not decoration: this function's first version
    # scored a MISSING variant file (a `cp` that failed, and therefore a suite that ran the
    # FIXED tree) as a pass -- "rc=0" printed next to "want rc!=0".  A harness that cannot
    # tell "the arm did not run" from "the arm passed" is the failure mode this project
    # names; so: the variant must exist AND differ from the fixed tree first.
    if [ ! -f "$variant" ]; then
        note "G2 $name: VARIANT MISSING -- the arm did NOT run" 2
        return
    fi
    if cmp -s "$variant" "$fixed"; then
        note "G2 $name: VARIANT IDENTICAL to the fixed tree -- nothing was tested" 3
        return
    fi
    cp "$variant" "$target"
    make "$suite" > "$OUT/gates-G2-$name.txt" 2>&1
    local rc=$?
    if [ "$want" = fail ]; then
        [ "$rc" -ne 0 ] && note "G2 $name: FAILS as it must (rc=$rc)" 0 \
                        || note "G2 $name: PASSED -- THE GATE HAS NO TEETH" 1
    else
        [ "$rc" -eq 0 ] && note "G2 $name: green as measured (rc=0)" 0 \
                        || note "G2 $name: UNEXPECTED FAILURE (rc=$rc)" 1
    fi
    grep -E '^FAIL|checks, [0-9]+ failures' "$OUT/gates-G2-$name.txt" | head -4 \
        | tee -a "$OUT/gates-summary.txt"
    cp "$fixed" "$target"
    cmp -s "$fixed" "$target" && note "G2 $name: restored (cmp vs the saved copy)" 0 \
        || note "G2 $name: RESTORE FAILED" 1
}

g2_arm blocker            "$OUT/jit_codegen.c.reverted"      src/cpu/jit_codegen.c \
       "$OUT/jit_codegen.c.fixed"       jit-codegen-selftest fail
# THE FOUR-STATE MEASUREMENT of the two helpers, as two arms.  MEASURED: fixing the READER
# alone leaves the tree green (the writer's dead mask then has nothing left to leak), while
# fixing the WRITER alone still leaves 12 read cases failing -- which is why the reader is the
# live site and the writer is fixed defensively (results/jit-fix-1.md ss2).
g2_arm read-only-fixed    "$OUT/jit_codegen.c.read-only-fixed"  src/cpu/jit_codegen.c \
       "$OUT/jit_codegen.c.fixed"       jit-codegen-selftest pass
g2_arm write-only-fixed   "$OUT/jit_codegen.c.write-only-fixed" src/cpu/jit_codegen.c \
       "$OUT/jit_codegen.c.fixed"       jit-codegen-selftest fail
g2_arm leaky              "$OUT/jit_mem.c.leaky"             src/cpu/jit_mem.c \
       "$OUT/jit_mem.c.fixed"           jit-selftest           fail
g2_arm jcc-swapped        "$OUT/jit_codegen.c.jcc-swapped"    src/cpu/jit_codegen.c \
       "$OUT/jit_codegen.c.fixed"       jit-codegen-selftest fail
g2_arm encoder-mutated    "$OUT/jit_x64.c.mutated"            src/cpu/jit_x64.c \
       "$OUT/jit_x64.c.fixed"           jit-codegen-selftest fail
g2_arm no-readonly-fold   "$OUT/jit_codegen.c.no-readonly-fold" src/cpu/jit_codegen.c \
       "$OUT/jit_codegen.c.fixed"       jit-codegen-selftest fail

# the control: the restored tree is green again
make jit-codegen-selftest > "$OUT/gates-G2-restored.txt" 2>&1
note "G2 restored tree: jit-codegen-selftest green" $?
grep -E 'checks, [0-9]+ failures' "$OUT/gates-G2-restored.txt" | tee -a "$OUT/gates-summary.txt"
make jit-selftest > "$OUT/gates-G2-restored-jitselftest.txt" 2>&1
note "G2 restored tree: jit-selftest green" $?
grep -E 'checks, [0-9]+ failures' "$OUT/gates-G2-restored-jitselftest.txt" \
    | tee -a "$OUT/gates-summary.txt"

# ---- G3: make / selftest --------------------------------------------------------------
make > "$OUT/gates-make.txt" 2>&1; note "make" $?
sha256sum build/v86c | tee -a "$OUT/gates-summary.txt"
make selftest > "$OUT/gates-selftest.txt" 2>&1; note "make selftest" $?
grep -E "_selftest: [0-9]+ checks|all suites" "$OUT/gates-selftest.txt" | tail -10 \
    | tee -a "$OUT/gates-summary.txt"

# ---- G3: nasm, three legs -------------------------------------------------------------
make nasm > "$OUT/gates-nasm-default.txt" 2>&1; note "make nasm (default)" $?
grep -E 'passed|failed|skipped' "$OUT/gates-nasm-default.txt" | tail -2 | tee -a "$OUT/gates-summary.txt"
for leg in "cacheoff:--uop-cache off" "jiton:--jit on"; do
    name=${leg%%:*}; flags=${leg#*:}
    python3 tests/nasm_run.py --stage run \
        --actual "build/v86c-nasm {img} {limit} --fixture $flags" \
        --skip-list tests/nasm_skip_c.txt > "$OUT/gates-nasm-$name.txt" 2>&1
    note "nasm with $flags" $?
    grep -E 'passed|failed|skipped' "$OUT/gates-nasm-$name.txt" | tail -2 | tee -a "$OUT/gates-summary.txt"
done

# ---- G3: the transcript, and the --jit on leg against the JS reference -----------------
make transcript-diff > "$OUT/gates-transcript-default.txt" 2>&1
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

# ---- G3: the 1.5 G boot --------------------------------------------------------------
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
else
    echo "1.5 G DIFFER: off=\"$xa\" onjit=\"$xb\"" | tee -a "$OUT/gates-summary.txt"
fi
sha256sum "$OUT"/boot-1.5g-*-serial.txt | tee -a "$OUT/gates-summary.txt"
cmp -s "$OUT/boot-1.5g-off-serial.txt" "$OUT/boot-1.5g-onjit-serial.txt" \
    && echo "the --jit on 1.5 G serial is BYTE-IDENTICAL to the default leg" \
    | tee -a "$OUT/gates-summary.txt"
grep -hE '^jit: (pages alloc|blocks compiled|uops executed|recipe table)' \
    "$OUT/boot-1.5g-onjit-stderr.txt" | tee -a "$OUT/gates-summary.txt"
echo "-- the leaked-span line on the 1.5 G boot (must be a COUNTER, not a paging failure):" \
    | tee -a "$OUT/gates-summary.txt"
grep -c 'block table is full' "$OUT/boot-1.5g-onjit-stderr.txt" \
    | sed 's/^/  "block table is full" occurrences: /' | tee -a "$OUT/gates-summary.txt"

# ---- G3: aarch64 still builds ---------------------------------------------------------
AARCH64=/home/jaye/.local/cosmo/bin/aarch64-linux-cosmo-cc
: > "$OUT/gates-aarch64.txt"
for f in src/cpu/jit_codegen.c src/cpu/jit_codegen_selftest.c src/cpu/jit_mem.c \
         src/cpu/jit_x64.c src/cpu/uop_cache.c src/cpu/jit_selftest.c; do
    "$AARCH64" -std=gnu11 -O2 -Wall -Wextra -foptimize-sibling-calls \
        -finline-functions-called-once -fpatchable-function-entry=0 -Werror \
        -I . -I src -c -o "$OUT/aarch64/$(basename "$f").o" "$f" >> "$OUT/gates-aarch64.txt" 2>&1
    note "aarch64 compile $(basename "$f")" $?
done

# ---- G4: the benchmark, both ways -----------------------------------------------------
for m in off on; do
    printf '#!/bin/sh\nexec ./build/v86c "$@" --jit %s\n' "$m" > "$OUT/v86c-jit$m"
    chmod +x "$OUT/v86c-jit$m"
done
tests/perf_bench.sh --runs 7 --bin "$PWD/$OUT/v86c-jitoff" --label "JIT-FIX-1 --jit off" \
    --samples "$OUT/bench-jitoff.samples" > "$OUT/gates-bench-jitoff.txt" 2>&1
note "bench --jit off" $?
tests/perf_bench.sh --runs 7 --bin "$PWD/$OUT/v86c-jiton" --label "JIT-FIX-1 --jit on" \
    --samples "$OUT/bench-jiton.samples" > "$OUT/gates-bench-jiton.txt" 2>&1
note "bench --jit on" $?
python3 - "$OUT/bench-jitoff.samples" "$OUT/bench-jiton.samples" > "$OUT/gates-bench-ratio.txt" 2>&1 <<'PY'
import statistics, sys
def best(p):
    xs = [float(l.split()[0]) for l in open(p) if not l.startswith('#')]
    return min(xs), statistics.median(xs)
off, on = best(sys.argv[1]), best(sys.argv[2])
print("jit off  best %.3f  median %.3f s" % off)
print("jit on   best %.3f  median %.3f s" % on)
print("ratio (off/on, >1 means the JIT is faster): best %.3f  median %.3f"
      % (off[0] / on[0], off[1] / on[1]))
PY
cat "$OUT/gates-bench-ratio.txt" | tee -a "$OUT/gates-summary.txt"

echo "== done $(date -u +%Y-%m-%dT%H:%M:%SZ) load=$(cut -d' ' -f1-3 /proc/loadavg)" \
    | tee -a "$OUT/gates-summary.txt"
# JIT-FIX-1R NICE-TO-HAVE-2: exit the aggregate, never the last command's status.
echo "== WORST RC ACROSS ALL ROWS: $WORST_RC" | tee -a "$OUT/gates-summary.txt"
exit "$WORST_RC"
