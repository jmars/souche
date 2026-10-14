#!/bin/sh
# results/perf-fix-guard.sh -- ITEM 1's asymmetry rig (unit PERF-FIX, PERF-1R's SHOULD-FIX).
#
# PERF-1R asked for the heuristic unity guard to be replaced by an EXACT one: an
# `nm --defined-only` global-set parity between the per-TU objects and build/unity.o.  It
# claimed that check "catches a synthetic shadowing the regex cannot".  This rig tests
# that claim and the delivered guards side by side, and it is the evidence behind
# results/perf-fix.md ITEM 1:
#
#   (1) THE COMPILER'S OWN VERDICT on in-unit collisions (the review's table, re-measured
#       here with the shipped compiler, because gen/unity_c.py's corrected header now
#       states it as fact).
#   (2) THE SILENT CLASS, END TO END: cpua.c's `static rebind` + cpub.c's `extern`
#       prototype + hwdef.c's outside definition.  Per-TU cb() = 1001, unity cb() = 2,
#       no diagnostic.  This is the reviewer's own synthetic (their /tmp/unityprobe).
#   (3) THE OLD GUARD (gen/unity_c.py before this unit: results/perf-fix-unity_c-prefix.py,
#       sha256 checked against the REVIEW's recorded sha) EMITS that unit.
#   (4) THE REVIEW'S CHECK AS FORMULATED (`--parity-only`: defined-global sets equal in
#       both directions) ALSO PASSES IT -- so the review's stated fix does not close the
#       class it was proposed for.  MEASURED, not argued.
#   (5) THE DELIVERED CHECK (the parity PLUS the outside-reference preservation) FAILS it,
#       naming `rebind`.
#   (6) ON THE REAL TREE the delivered check is GREEN (counts printed), and a deliberate
#       cross-set shadowing INJECTED INTO THE REAL UNITY SOURCE LIST -- a `static
#       io_port_read8` shadowing src/hw/io.c's global -- compiles SILENTLY under the
#       shipped flags and is caught by it.
#
# Usage:  sh results/perf-fix-guard.sh
# Reads: gen/unity_c.py, gen/unity_symbols.py, results/perf-fix-unity_c-prefix.py, the
#        Makefile (for the source lists), results/perf-1/guard-asymmetry.sh (unchanged)
# Writes: build/pf-guard/*

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

COSMOCC=${COSMOCC:-$HOME/.local/cosmo/bin/cosmocc}
WORK=build/pf-guard
PREFIX_PY=results/perf-fix-unity_c-prefix.py
PREFIX_SHA=52346016517cdeb296e0900aca6e95b0dd9cd4be2cbed9579f86987abcb5cdce
fail=0

say() { printf '%s\n' "$*"; }
hr() { say "-------------------------------------------------------------------"; }
verdict() { if [ "$1" = 0 ]; then printf 'rc=0 EMITTED'; else printf 'rc=%s REFUSED' "$1"; fi; }

mkdir -p "$WORK" || exit 1

# --- 0. the pre-fix guard is the reviewed artifact -------------------------------------
got=$(sha256sum "$PREFIX_PY" | cut -d' ' -f1)
if [ "$got" != "$PREFIX_SHA" ]; then
	say "FATAL: $PREFIX_PY is not the reviewed pre-fix generator"
	say "  want $PREFIX_SHA"
	say "  got  $got"
	exit 1
fi
say "pre-fix generator sha256 == the sha the review recorded for gen/unity_c.py:"
say "  $got"

# --- 1. the compiler's verdict on in-unit collisions (the review's table) --------------
hr
say "1. IN-UNIT COLLISIONS, shipped compiler ($("$COSMOCC" --version 2>&1 | head -1))"
inunit() { # <name> <fileA-body> <fileB-body>
	name=$1
	printf '%s\n' "$2" > "$WORK/ia.c"
	printf '%s\n' "$3" > "$WORK/ib.c"
	printf '#include "ia.c"\n#include "ib.c"\n' > "$WORK/iu.c"
	if out=$("$COSMOCC" -std=gnu11 -O2 -c -o "$WORK/iu.o" "$WORK/iu.c" 2>&1); then
		say "  $name: rc=0 -- SILENT (a diagnostics-quality case only)"
	else
		first=$(printf '%s\n' "$out" | grep -m1 'error:')
		say "  $name: rc=1 -- ${first:-error}"
	fi
}
inunit "static fn + static fn (same signature)" \
	'static int f(int x) { return x + 1; } int a(void) { return f(1); }' \
	'static int f(int x) { return x + 2; } int b(void) { return f(2); }'
inunit "static fn + global fn (same signature)" \
	'static int g(int x) { return x + 1; } int c(void) { return g(1); }' \
	'int g(int x) { return x + 3; }'
inunit "static obj + global tentative obj" \
	'static int o; int d(void) { return o; }' \
	'int o;'
inunit "static obj + global obj with initializer" \
	'int p = 7;' \
	'static int p; int e(void) { return p; }'

# --- 2. the silent class, end to end ---------------------------------------------------
hr
say "2. THE SILENT CLASS (the review's synthetic; per-TU works, unity silently rebinds)"
printf 'static int rebind(int x) { return x + 1; }\nint ca(void) { return rebind(1); }\n' > "$WORK/cpua.c"
printf 'extern int rebind(int x);\nint cb(void) { return rebind(1); }\n'               > "$WORK/cpub.c"
printf 'int rebind(int x) { return x + 1000; }\n'                                       > "$WORK/hwdef.c"
printf '#include <stdio.h>\nextern int ca(void); extern int cb(void);\nint main(void){ printf("ca=%%d cb=%%d\\n", ca(), cb()); return 0; }\n' > "$WORK/drv.c"
# shellcheck disable=SC2086
"$COSMOCC" -O2 -c -o "$WORK/cpua.o" "$WORK/cpua.c" || exit 1
# shellcheck disable=SC2086
"$COSMOCC" -O2 -c -o "$WORK/cpub.o" "$WORK/cpub.c" || exit 1
# shellcheck disable=SC2086
"$COSMOCC" -O2 -c -o "$WORK/hwdef.o" "$WORK/hwdef.c" || exit 1
# shellcheck disable=SC2086
"$COSMOCC" -O2 -c -o "$WORK/drv.o" "$WORK/drv.c" || exit 1
"$COSMOCC" -o "$WORK/pertu" "$WORK/drv.o" "$WORK/cpua.o" "$WORK/cpub.o" "$WORK/hwdef.o" || exit 1
printf '#include "cpua.c"\n#include "cpub.c"\n' > "$WORK/unitysynth.c"
if "$COSMOCC" -O2 -I. -c -o "$WORK/unitysynth.o" "$WORK/unitysynth.c" 2>"$WORK/synth.log"; then
	say "  unity TU (cpua.c + cpub.c): rc=0, NO diagnostic from the compiler"
else
	say "  unity TU did NOT compile (the synthetic does not reproduce -- see $WORK/synth.log)"
	exit 1
fi
"$COSMOCC" -o "$WORK/unitybin" "$WORK/drv.o" "$WORK/unitysynth.o" "$WORK/hwdef.o" || exit 1
say "  per-TU link (cpua.o cpub.o hwdef.o): $("$WORK/pertu")   <- binds to the OUTSIDE global"
say "  unity   link (unitysynth.o hwdef.o): $("$WORK/unitybin")   <- silently bound to the static"

# --- 3. the OLD guard ------------------------------------------------------------------
hr
say "3. THE OLD GUARD (the pre-fix generator, no --watch) ON THAT UNIT"
python3 "$PREFIX_PY" -o "$WORK/old.c" "$WORK/cpua.c" "$WORK/cpub.c" > "$WORK/old.log" 2>&1
oldrc=$?
printf '  %s  <- ' "$(verdict $oldrc)"; tail -1 "$WORK/old.log"

# --- 4. the review's check AS FORMULATED ----------------------------------------------
hr
say "4. THE REVIEW'S CHECK AS FORMULATED (defined-global parity only, --parity-only)"
python3 gen/unity_symbols.py --parity-only --unity "$WORK/unitysynth.o" \
	"$WORK/cpua.o" "$WORK/cpub.o" -- "$WORK/hwdef.o"
r4=$?
printf '  %s  <- the parity alone does NOT see the rebind\n' "$(verdict $r4)"
if [ "$r4" = 0 ]; then
	say "  (each side's defined-global set is exactly {ca, cb}: the shadowing lives on the"
	say "   UNDEFINED side -- per-TU cpub.o references 'rebind', the unity TU does not)"
fi

# --- 5. the delivered check ------------------------------------------------------------
hr
say "5. THE DELIVERED CHECK (parity + outside-reference preservation) ON THE SAME UNIT"
python3 gen/unity_symbols.py --unity "$WORK/unitysynth.o" \
	"$WORK/cpua.o" "$WORK/cpub.o" -- "$WORK/hwdef.o"
newrc=$?
printf '  %s\n' "$(verdict $newrc)"
if [ "$oldrc" = 0 ] && [ "$r4" = 0 ] && [ "$newrc" != 0 ]; then
	say "  ASYMMETRY: the old guard and the review's parity check both pass it; the"
	say "             delivered check fails it."
else
	say "  ASYMMETRY NOT SHOWN (old=$oldrc parity=$r4 new=$newrc)"; fail=1
fi

# --- 6. the real tree: green, and a deliberate injection ------------------------------
hr
say "6a. THE REAL TREE (make unity-symbols: the shipped gate)"
make -s unity-symbols 2>&1 | tail -3
mrc=$?
printf '  %s\n' "$([ $mrc = 0 ] && echo "rc=0 GREEN" || echo "rc=$mrc FAILED")"
[ "$mrc" = 0 ] || fail=1

# The per-TU object list the gate itself uses, asked of make (one source of truth).
db=$(make -s -p -n 2>/dev/null)
PERTU=$(printf '%s\n' "$db" | sed -n 's/^UNITY_PERTU_OBJ :\?= *//p' | head -1)
HWOBJ=$(printf '%s\n' "$db" | sed -n 's/^HW_OBJ :\?= *//p' | head -1)
SFOBJ=$(printf '%s\n' "$db" | sed -n 's/^SF_OBJ :\?= *//p' | head -1)
WATCH=$(printf '%s\n' "$db" | sed -n 's/^UNITY_WATCH :\?= *//p' | head -1)
UNITY_SRC=$(printf '%s\n' "$db" | sed -n 's/^UNITY_SRC :\?= *//p' | head -1)

say ""
say "6b. A DELIBERATE CROSS-SET SHADOWING INJECTED INTO THE REAL UNITY SOURCE LIST"
say "    (a unit static named after src/hw/io.c's GLOBAL io_port_read8 -- the exact class,"
say "     in the exact shape the tree would take if a CPU file grew such a static)"
printf 'static int32_t io_port_read8(int32_t port) { return (int32_t)port; }\n' > "$WORK/shadow.c"
# shellcheck disable=SC2086
python3 gen/unity_c.py -o "$WORK/unity_shadow.c" "$WORK/shadow.c" $UNITY_SRC > "$WORK/gen_shadow.log" 2>&1
say "  old generator on the injected list: $(verdict $?)"
# shellcheck disable=SC2086
python3 gen/unity_c.py --watch $WATCH -o "$WORK/unity_shadow_watch.c" "$WORK/shadow.c" $UNITY_SRC \
	> "$WORK/gen_shadow_watch.log" 2>&1
say "  NEW generator with --watch:        $(verdict $?)  <- the heuristic gate refuses it"
sed -n '2,3p' "$WORK/gen_shadow_watch.log" | sed 's/^/    /'
# shellcheck disable=SC2086
if "$COSMOCC" $($db | sed -n 's/^CPU_CFLAGS :\?= *//p' | head -1) -I . -I src \
	-c -o "$WORK/unity_shadow.o" "$WORK/unity_shadow.c" 2>"$WORK/shadow_cc.log"; then
	say "  the injected unity TU COMPILES SILENTLY under the shipped flags (-Werror)"
else
	say "  the injected unity TU did NOT compile (then the injection is loud, not silent)"
	sed -n '1,3p' "$WORK/shadow_cc.log" | sed 's/^/    /'
	fail=1
fi
say "  the delivered check on it:"
# shellcheck disable=SC2086
python3 gen/unity_symbols.py --unity "$WORK/unity_shadow.o" $PERTU -- $HWOBJ $SFOBJ
shrc=$?
printf '  %s\n' "$(verdict $shrc)"
[ "$shrc" != 0 ] || fail=1

hr
say "result: $([ $fail = 0 ] && echo 'all guard asymmetries reproduced' || echo 'SOMETHING FAILED')"
exit $fail
