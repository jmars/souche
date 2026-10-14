#!/bin/sh
# results/perf-fix-2-cpu100.sh -- is src/cpu/cpu.c:100's assert DROPPABLE? (PERF-FIX-2, ITEM A)
#
# PERF-FIX reported cpu.c:100 (`DBG_ASSERT(!in_mapped_range(phys_addr))` in get_phys_eip,
# upstream cpu.rs:3295) as a 15th site "of the same class" as its 14 and left it to its
# owner.  The class's remedy (PERF-FIX's 14, R3F's N1, R4F's N3, and this unit's three
# instructions.c sites) is to DROP the check AT ITS SITE, because the release reference
# simply executes on with the same value.  This rig tests whether that remedy applies
# here, by measuring what the port DOES at that site rather than what upstream's text
# says about it.
#
# It builds one more leg from the SAME tree with ONLY
#   `DBG_ASSERT(!in_mapped_range(phys_addr));`  (get_phys_eip's, cpu.c:100)
# deleted -- a one-line change of the file the sweep is about -- and runs the nasm
# corpus's own `jump` image on it, which is the guest that reaches the site (MEASURED:
# the tree's binary stops at cpu.c:100 on that image; the reference keeps executing at
# eip 0x42b001, past the 2 MiB of guest RAM).
#
#   before = the tree as it ships                    -> the stop, rc=134 at cpu.c:100
#   dropped = the same tree, that one line deleted   -> WHAT HAPPENS INSTEAD
#   node oracle/nasm_state.mjs jump.img              -> the reference's own answer
#
# Usage:  sh results/perf-fix-2-cpu100.sh
# Writes: build/pf2-cpu100/*

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

COSMOCC=${COSMOCC:-$HOME/.local/cosmo/bin/cosmocc}
WORK=build/pf2-cpu100
LIMIT=100000
IMG=build/nasm/jump.img

say() { printf '%s\n' "$*"; }
hr() { say "-------------------------------------------------------------------"; }

mkdir -p "$WORK" || exit 1

if [ ! -f "$IMG" ]; then
	say "FATAL: $IMG does not exist (run 'make nasm' once to build the corpus images)"
	exit 1
fi

db=$(make -s -p -n 2>/dev/null)
val() { printf '%s\n' "$db" | sed -n "s/^$1 :\?= *//p" | head -1; }
CPU_CFLAGS=$(val CPU_CFLAGS)
CFLAGS=$(val CFLAGS)
PERF_CFLAGS=$(val PERF_CFLAGS)
UNITY_SRC=$(val UNITY_SRC)
HW_OBJ=$(val HW_OBJ)
SF_OBJ=$(val SF_OBJ)
MINIBIOS_EMBED=$(val MINIBIOS_EMBED)

# --- the one-line mutation -------------------------------------------------------------
python3 - "$WORK/cpu.c" "$WORK/cpu.both.c" <<'PY' || exit 1
import sys
src = open('src/cpu/cpu.c').read()
needle = "\tDBG_ASSERT(!in_mapped_range(phys_addr));\n"
n = src.count(needle)
if n != 2:
    sys.stderr.write("results/perf-fix-2-cpu100.sh: expected 2 occurrences of %r, found %d\n"
                     % (needle, n))
    sys.exit(2)
# the FIRST one is get_phys_eip's (cpu.c:100).  Deleting it ONLY: the second
# (interpreted_run's) stays, so the two legs differ by exactly the site under test.
mut = src.replace(needle, "", 1)
assert mut.count(needle) == 1
open(sys.argv[1], 'w').write(mut)
# and a THIRD leg with BOTH guards gone: that is the leg that actually reaches the raw
# mem8 read past the allocation, which is the measurement that decides the verdict.
open(sys.argv[2], 'w').write(src.replace(needle, ""))
sys.stderr.write("mutations written: get_phys_eip's only; and both\n")
PY
say "cpu.c mutation: $(diff src/cpu/cpu.c "$WORK/cpu.c" | grep -c '^[<>]') line(s) differ (get_phys_eip's site only)"
say "cpu.c mutation: $(diff src/cpu/cpu.c "$WORK/cpu.both.c" | grep -c '^[<>]') line(s) differ (both guards)"

# --- the two unity objects --------------------------------------------------------------
make -s $HW_OBJ $SF_OBJ $MINIBIOS_EMBED || exit 1
python3 gen/unity_c.py -o "$WORK/after.c" $UNITY_SRC || exit 1
sed "s|#include \"src/cpu/cpu.c\"|#include \"$(pwd)/$WORK/cpu.c\"|" "$WORK/after.c" > "$WORK/dropped.c"
sed "s|#include \"src/cpu/cpu.c\"|#include \"$(pwd)/$WORK/cpu.both.c\"|" "$WORK/after.c" > "$WORK/both.c"
if [ "$(grep -c "$WORK/cpu.c" "$WORK/dropped.c")" -ne 1 ] || [ "$(grep -c "$WORK/cpu.both.c" "$WORK/both.c")" -ne 1 ]; then
	say "FATAL: a substitution did not hit exactly one include"
	exit 1
fi
# shellcheck disable=SC2086
"$COSMOCC" $CPU_CFLAGS -iquote src/cpu -I . -I src -c -o "$WORK/after.o" "$WORK/after.c" || exit 1
# shellcheck disable=SC2086
"$COSMOCC" $CPU_CFLAGS -iquote src/cpu -I . -I src -c -o "$WORK/dropped.o" "$WORK/dropped.c" || exit 1
# shellcheck disable=SC2086
"$COSMOCC" $CFLAGS $PERF_CFLAGS -I src -Wl,--gc-sections -o "$WORK/v86c-after" \
	src/main.c $MINIBIOS_EMBED "$WORK/after.o" $HW_OBJ $SF_OBJ || exit 1
# shellcheck disable=SC2086
"$COSMOCC" $CFLAGS $PERF_CFLAGS -I src -Wl,--gc-sections -o "$WORK/v86c-dropped" \
	src/main.c $MINIBIOS_EMBED "$WORK/dropped.o" $HW_OBJ $SF_OBJ || exit 1
# shellcheck disable=SC2086
"$COSMOCC" $CPU_CFLAGS -iquote src/cpu -I . -I src -c -o "$WORK/both.o" "$WORK/both.c" || exit 1
# shellcheck disable=SC2086
"$COSMOCC" $CFLAGS $PERF_CFLAGS -I src -Wl,--gc-sections -o "$WORK/v86c-both" \
	src/main.c $MINIBIOS_EMBED "$WORK/both.o" $HW_OBJ $SF_OBJ || exit 1

hr
say "THE RUN  (the corpus's own jump image: the guest that reaches the site)"
say ""
printf '  the tree as it ships  : '
( ( "$WORK/v86c-after" "$IMG" "$LIMIT" --fixture >"$WORK/after.out" 2>"$WORK/after.err" ) 2>/dev/null )
printf 'rc=%-4s %s\n' "$?" "$(grep -m1 -E 'assert\(|v86c: ' "$WORK/after.err" | sed 's|.*/src/|src/|' | cut -c1-96)"
printf '  that line DELETED     : '
( ( "$WORK/v86c-dropped" "$IMG" "$LIMIT" --fixture >"$WORK/dropped.out" 2>"$WORK/dropped.err" ) 2>/dev/null )
rc=$?
printf 'rc=%-4s %s\n' "$rc" "$(grep -m1 -E 'assert\(|v86c: ' "$WORK/dropped.err" | sed 's|.*/src/|src/|' | cut -c1-96)"
[ -z "$(grep -m1 -E 'assert\(|v86c: ' "$WORK/dropped.err")" ] && say "                        (no stop message at all)"
say ""
printf '  BOTH guards DELETED   : '
( ( "$WORK/v86c-both" "$IMG" "$LIMIT" --fixture >"$WORK/both.out" 2>"$WORK/both.err" ) 2>/dev/null )
printf 'rc=%-4s %s\n' "$?" "$(grep -m1 -E 'assert\(|v86c: ' "$WORK/both.err" | sed 's|.*/src/|src/|' | cut -c1-96)"
[ -z "$(grep -m1 -E 'assert\(|v86c: ' "$WORK/both.err")" ] && say "                       (no stop message at all)"
say ""
say "  the dropped leg's first host signal, if any:"
sed -n '1,6p' "$WORK/dropped.err" | sed 's/^/    /'
say "  the both-deleted leg's first host signal, if any:"
sed -n '1,6p' "$WORK/both.err" | sed 's/^/    /'
say ""
say "  the port's own terminal state, when a leg survives to print one:"
for leg in dropped both; do
	printf '    %-8s ' "$leg"
	python3 - "$WORK/$leg.out" <<'PY'
import json, sys
try:
    a = json.load(open(sys.argv[1]))["array"]
except Exception:
    print("no fixture printed (the leg did not reach a state it could report)")
    sys.exit(0)
print("eip=0x%08x esp=0x%08x ebx=0x%08x" % (a[8] & 0xFFFFFFFF, a[4] & 0xFFFFFFFF, a[3] & 0xFFFFFFFF))
PY
done
hr
say "  THE REFERENCE's own answer on the same image (its release build has no such check):"
timeout 120 node oracle/nasm_state.mjs "$IMG" 1000000 >"$WORK/ref.out" 2>"$WORK/ref.err"; rrc=$?
say "    rc=$rrc $(head -1 "$WORK/ref.err")"
python3 - "$WORK/ref.out" <<'PY'
import json, sys
try:
    a = json.load(open(sys.argv[1]))["array"]
except Exception as e:
    print("    no fixture printed (%s)" % e)
    sys.exit(0)
print("    eip=0x%08x esp=0x%08x ebx=0x%08x" % (a[8] & 0xFFFFFFFF, a[4] & 0xFFFFFFFF, a[3] & 0xFFFFFFFF))
PY
hr
say "raw: $WORK/{after,dropped}.{out,err}, $WORK/ref.{out,err}"
