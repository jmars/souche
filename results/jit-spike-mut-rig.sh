#!/bin/bash
# jit-spike-mut-rig.sh -- JIT-SPIKE's G3: the STALE-BLOCK PROBE, shown ABLE TO FAIL.
#
# The discipline is results/perf-4-mut-rig.sh's (and the project's): ONE source line per arm,
# applied to a COPY-tracked original, the target rebuilt from the mutated source (never a stale
# object), the arm's verdict taken from the EXIT CODE, and the tree restored afterwards.  Arm 0
# is the control: the unmutated tree must be GREEN, or the rig is measuring itself.
#
# WHAT EACH ARM REMOVES, and why it is the arm that matters:
#  1. the forward call in uop_invalidate_page -- the JIT riding the ONE invalidation mechanism.
#     Without it nothing reaches jit_code_invalidate_page and the stale block stays reachable.
#  2. the drop in jit_block_drop -- the hook fires, the registry is unlinked, but the block
#     stays LIVE and reachable through the hash (the state a dropped invalidation would leave).
#  3. the guest-page registry link in jit_cache_bind -- THE BUG THIS UNIT ACTUALLY SHIPPED AND
#     FOUND (the block stays live in the hash and no write can reach it).  This is the arm that
#     says the probe is testing the LINK, not just a flag.
#  4. the shared dirty-page bit in jit_cache_bind -- the write sites' inlined test then misses
#     the page, which is the one way the JIT could be invisible to the PERF-FIX-3 hooks.
#  5. the DEFERRED recycle of the executing block's page -- the page is flipped to RW under the
#     block that is running on it.
#
# Usage: bash results/jit-spike-mut-rig.sh [--out FILE]
set -u
cd "$(dirname "$0")/.." || exit 2
OUT=results/jit-spike/mut-rig.txt
[ "${1:-}" = "--out" ] && OUT="$2"
mkdir -p "$(dirname "$OUT")"

TARGET=build/cpu_jit_selftest
WORK=build/mutrig-jit
mkdir -p "$WORK"

fails=0
caught=0
total=0

# mutate <file> <old> <new> ... -- every pair must occur exactly once
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

echo "=== jit-spike mut-rig: the stale-block probe, shown able to fail ===" | tee "$OUT"

# arm 0: the control
total=$((total+1))
rm -f "$TARGET"
make "$TARGET" > "$WORK/build.control.log" 2>&1
if ./"$TARGET" > "$WORK/run.control.txt" 2>&1; then
    echo "ARM 0 (control, unmutated): PASS (exit 0) -- the probe is GREEN before any mutation" | tee -a "$OUT"
else
    echo "ARM 0 (control, unmutated): ** FAILED ** -- the rig is measuring itself" | tee -a "$OUT"
    fails=$((fails+1))
fi

run_arm() {
    local name="$1" file="$2"
    shift 2
    total=$((total+1))
    cp "$file" "$WORK/$(basename "$file").orig"
    if ! mutate "$file" "$@"; then
        echo "ARM $name: COULD NOT APPLY THE MUTATION" | tee -a "$OUT"
        cp "$WORK/$(basename "$file").orig" "$file"
        fails=$((fails+1))
        return
    fi
    rm -f "$TARGET"
    make "$TARGET" > "$WORK/build.$name.log" 2>&1
    local buildrc=$?
    local verdict
    if [ $buildrc -ne 0 ]; then
        verdict="BUILD FAILED (see $WORK/build.$name.log) -- NOT a valid arm"
        fails=$((fails+1))
    else
        ./"$TARGET" > "$WORK/run.$name.txt" 2>&1
        local rc=$?
        if [ $rc -ne 0 ]; then
            verdict="CAUGHT (exit $rc) -- the probe FAILED on the mutated tree"
            caught=$((caught+1))
        else
            verdict="** NOT CAUGHT (exit 0) ** -- the probe passed on the mutated tree"
            fails=$((fails+1))
        fi
    fi
    echo "ARM $name: $verdict" | tee -a "$OUT"
    grep -m3 -E '^(FAIL|jit-selftest:)' "$WORK/run.$name.txt" 2>/dev/null | sed 's/^/    /' | tee -a "$OUT"
    cp "$WORK/$(basename "$file").orig" "$file"
}

run_arm 1 src/cpu/uop_cache.c \
    '	jit_code_invalidate_page(phys_addr);' \
    '	/* MUTATED: the forward call is gone */'

run_arm 2 src/cpu/jit_mem.c \
    '	if (b->hash_slot < JIT_HASH_SLOTS) jit_hash[b->hash_slot] = JIT_HASH_DEAD;' \
    '	/* MUTATED: the hash entry is left pointing at the block */' \
    '	b->live = 0;
	b->next_page = 0;' \
    '	b->live = 1; /* MUTATED: the block stays live */
	b->next_page = 0;'

run_arm 3 src/cpu/jit_mem.c \
    '		b->next_page = jit_page_head[gpage];
		jit_page_head[gpage] = (uint16_t)(id + 1u);' \
    '		/* MUTATED: the block is not linked into the guest-page registry */'

run_arm 4 src/cpu/jit_mem.c \
    '	if (gpage < uop_page_code_len) uop_page_code[gpage] = 1;' \
    '	/* MUTATED: the shared dirty-page bit is not set */'

run_arm 5 src/cpu/jit_mem.c \
    '		jit_pages[idx].state = JP_DEFER;
		jit_stats.deferred_frees++;
		return;' \
    '		/* MUTATED: the executing page is flipped under the running block */'

# THE RESTORATION ARM: every mutated file is a copy of an original and has been put back, so a
# from-scratch rebuild must be GREEN again.  Without this the rig leaves a MUTATED binary behind
# (MEASURED: the first version did, and the tree's own gate then read 1 failure).
total=$((total+1))
rm -f "$TARGET"
make "$TARGET" > "$WORK/build.restored.log" 2>&1
if ./"$TARGET" > "$WORK/run.restored.txt" 2>&1; then
    echo "ARM R (restored tree, rebuilt): PASS (exit 0) -- no mutation left behind" | tee -a "$OUT"
else
    echo "ARM R (restored tree, rebuilt): ** FAILED ** -- a mutation was left in the tree" | tee -a "$OUT"
    fails=$((fails+1))
fi

echo "" | tee -a "$OUT"
echo "arms: $((total-2)) mutated + 1 control + 1 restoration; CAUGHT $caught/$((total-2)); failures $fails" | tee -a "$OUT"
[ $fails -eq 0 ]
