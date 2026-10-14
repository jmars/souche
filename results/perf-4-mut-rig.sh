#!/bin/bash
# perf-4-mut-rig.sh -- PERF-4/M2's G2: every M2 gate, shown ABLE TO FAIL.
#
# The discipline is results/perf-3-mut-rig.sh's (and the project's): ONE source line per arm,
# applied to a COPY-tracked original, the gate rebuilt from the mutated source (never a stale
# object), the arm's verdict recorded from the EXIT CODE, and the tree restored afterwards.
# Arm 0 is the control: the unmutated tree must be GREEN, or the rig is measuring itself.
#
# Usage: bash results/perf-4-mut-rig.sh [--out FILE]
set -u
cd "$(dirname "$0")/.." || exit 2
OUT=results/perf-4/mut-rig.txt
[ "${1:-}" = "--out" ] && OUT="$2"
mkdir -p "$(dirname "$OUT")"

SELFTEST=build/cpu_uop_exec_selftest
WORK=build/mutrig4
mkdir -p "$WORK"

fails=0
caught=0
total=0

# mutate <file> <old> <new> [<old2> <new2> ...] -- every pair must occur exactly once
mutate() {
    python3 - "$@" <<'PY'
import sys
p = sys.argv[1]
pairs = sys.argv[2:]
if len(pairs) % 2 != 0:
    sys.exit(4)
s = open(p).read()
for k in range(0, len(pairs), 2):
    old, new = pairs[k], pairs[k + 1]
    if s.count(old) != 1:
        print(f"MUTATE: {old!r} occurs {s.count(old)} times in {p}", file=sys.stderr)
        sys.exit(3)
    s = s.replace(old, new, 1)
open(p, 'w').write(s)
PY
}

run_arm() {
    local name="$1" file="$2" target="$3" cmd="$4"
    shift 4
    total=$((total+1))
    cp "$file" "$WORK/$(basename "$file").orig"
    if ! mutate "$file" "$@"; then
        echo "ARM $name: COULD NOT APPLY THE MUTATION" | tee -a "$OUT"
        cp "$WORK/$(basename "$file").orig" "$file"
        return
    fi
    rm -f "$target"
    make "$target" build/v86c-nasm > "$WORK/build.$name.log" 2>&1
    local buildrc=$?
    if [ $buildrc -ne 0 ]; then
        echo "ARM $name: BUILD FAILED (see $WORK/build.$name.log)" | tee -a "$OUT"
        cp "$WORK/$(basename "$file").orig" "$file"
        fails=$((fails+1))
        return
    fi
    bash -c "$cmd" > "$WORK/run.$name.log" 2>&1
    local rc=$?
    if [ $rc -ne 0 ]; then
        caught=$((caught+1))
        echo "ARM $name: CAUGHT (rc=$rc) :: $(grep -m1 -E 'FAIL|Failed|no state' "$WORK/run.$name.log" | cut -c1-150)" | tee -a "$OUT"
    else
        echo "ARM $name: NOT CAUGHT (rc=0) -- THE RIG FOUND A HOLE" | tee -a "$OUT"
        fails=$((fails+1))
    fi
    cp "$WORK/$(basename "$file").orig" "$file"
    rm -f "$target"
    make "$target" > /dev/null 2>&1
}

echo "== PERF-4/M2 mutation rig $(date -u +%Y-%m-%dT%H:%M:%SZ)" | tee "$OUT"
echo "== the gates it drives: $SELFTEST (the M2 suite) and the nasm differential" | tee -a "$OUT"

# ---- control ------------------------------------------------------------------
echo "ARM 0 (control): the unmutated tree" | tee -a "$OUT"
rm -f "$SELFTEST"; make "$SELFTEST" > /dev/null 2>&1
./"$SELFTEST" > "$WORK/run.control.log" 2>&1
rc=$?
if [ $rc -eq 0 ]; then
    echo "ARM 0: GREEN (rc=0) -- the rig's baseline" | tee -a "$OUT"
else
    echo "ARM 0: RED (rc=$rc) WITHOUT A MUTATION -- the rig is invalid" | tee -a "$OUT"
    fails=$((fails+1))
fi

# ---- 1. the invalidation hook, disabled wholesale ------------------------------
run_arm "invalidation-off" src/cpu/uop_cache.c build/cpu_uop_exec_selftest ./build/cpu_uop_exec_selftest \
    '	if (page >= uop_page_count) return;' \
    '	if (1 || page >= uop_page_count) return;'

# ---- 2. the SMC abort alone (the page is unlinked, the RUNNING block is not aborted) ----
run_arm "smc-abort-off" src/cpu/uop_cache.c build/cpu_uop_exec_selftest ./build/cpu_uop_exec_selftest \
    '		uop_abort_pending = true;' \
    '		uop_abort_pending = false;'

# ---- 3. the staircase: no mid-block flush (jit_block_boundary only) ------------
run_arm "no-mid-block-flush" src/cpu/uop_cache.c build/cpu_uop_exec_selftest ./build/cpu_uop_exec_selftest \
    '			if (ran && ((u->flags & UOP_FLAG_FLUSH) || jit_block_boundary)) {' \
    '			if (ran && (jit_block_boundary)) {'

# ---- 4. the sti block-end return drops its pending count (the pre-fix defect) ----
run_arm "sti-pending-dropped" src/cpu/uop_cache.c build/cpu_uop_exec_selftest ./build/cpu_uop_exec_selftest \
    '				uop_stats.block_ends++;' \
    '				uop_stats.block_ends++; uop_pending = 0;'

# ---- 5. the state byte dropped from the lookup's key COMPARISON ----------------
# TWO LINES, and that is the MINIMAL mutation that makes the cache actually reuse a
# wrong-state block: the state byte feeds the lookup's KEY in two places -- the hash position
# (uop_hash_pos) and the full-key comparison on the probe -- and dropping EITHER ONE alone is
# masked by the other (a dropped comparison still probes from a different slot; a dropped hash
# term still fails the comparison).  MEASURED both ways.
run_arm "state-key-ignored" src/cpu/uop_cache.c build/cpu_uop_exec_selftest ./build/cpu_uop_exec_selftest \
    '	uint32_t h = phys * 0x9E3779B1u + virt * 0x85EBCA6Bu + ((uint32_t)state + 1u) * 0xC2B2AE35u;' \
    '	uint32_t h = phys * 0x9E3779B1u + virt * 0x85EBCA6Bu + ((uint32_t)(state & 0u) + 1u) * 0xC2B2AE35u;' \
    '			if (b->phys_start == phys && b->virt_start == virt && b->state == state) {' \
    '			if (b->phys_start == phys && b->virt_start == virt && (b->state == state || b->state != state)) {'

# ---- 6. the body-entry pointer for a custom_modrm_resolve uop (the pop/lea defect),
#        driven through the NASM differential instead of the M2 suite --------------
run_arm "fetch-internal-ip" src/cpu/uop_cache.c build/v86c \
    'python3 tests/nasm_run.py --stage run --actual "build/v86c-nasm {img} {limit} --fixture" --skip-list tests/nasm_skip_c.txt --filter "lea-nop|push|leave16"' \
    '					? (uint32_t)u->pfx_len + (uint32_t)u->opcode_bytes + 1u' \
    '					? (uint32_t)u->pfx_len + (uint32_t)u->opcode_bytes'

echo "== arms $total, caught $caught, holes $fails" | tee -a "$OUT"
[ "$fails" -eq 0 ] || exit 1
