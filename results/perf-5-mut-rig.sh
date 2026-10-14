#!/bin/bash
# perf-5-mut-rig.sh -- PERF-5's ASYMMETRY RIG for the two M3/M4 changes.
#
# WHAT IT PROVES.  A gate that cannot fail is not a gate (results/perf-4-mut-rig.sh's own
# discipline, and the review's).  Each arm applies ONE inverse mutation to a change this unit
# landed, rebuilds the one binary that gates it, and requires the run to FAIL; the control arm
# (the unmutated tree) must PASS.  Verdicts come from the exit code, never from the output text.
#
# The mutations are applied to a COPY of the file and restored from the copy afterwards, so the
# tree is left byte-identical (checked at the end with sha256).
#
# Usage: results/perf-5-mut-rig.sh          (writes nothing outside /tmp; prints verdicts)

set -u
cd "$(dirname "$0")/.." || exit 2

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

UOP=src/cpu/uop_cache.c
EXEC=src/cpu/uop_exec_selftest.c
cp "$UOP" "$TMP/uop.orig"; cp "$EXEC" "$TMP/exec.orig"
SHA_UOP=$(sha256sum "$UOP" | cut -d' ' -f1)
SHA_EXEC=$(sha256sum "$EXEC" | cut -d' ' -f1)

restore() { cp "$TMP/uop.orig" "$UOP"; cp "$TMP/exec.orig" "$EXEC"; }

build_and_run() { # $1 = target, $2 = binary, $3 = extra args
    make "$1" >"$TMP/build.log" 2>&1 || { echo "BUILD-FAILED"; return 3; }
    ./"$2" ${3:-} >"$TMP/run.log" 2>&1
    return $?
}

report() { # $1 = arm name, $2 = want (PASS/FAIL), $3 = rc
    local got
    if [ "$3" = 0 ]; then got=PASS; else got=FAIL; fi
    if [ "$3" = 3 ]; then got=BUILD-FAILED; fi
    if [ "$got" = "$2" ]; then
        printf '%-46s %-14s rc=%-3s OK\n' "$1" "$got" "$3"
        caught=$((caught + 1))
    else
        printf '%-46s %-14s rc=%-3s HOLE (wanted %s)\n' "$1" "$got" "$3" "$2"
        holes=$((holes + 1))
    fi
}

caught=0; holes=0
echo "== PERF-5 mutation rig (one inverse mutation per arm)"
echo "-- control"

# 0: control -- the unmutated tree must PASS
restore
build_and_run build/cpu_uop_exec_selftest build/cpu_uop_exec_selftest
control_rc=$?
report "0 control: the tree as built" PASS $control_rc

# 1: the prefix branch of the inline segment lookup removed -- phase "non-flat" must move
#    (the FS/ES override slots would read through the DEFAULT segment)
cp "$TMP/uop.orig" "$UOP"
python3 - "$UOP" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = """	int32_t prefix = cpu->prefixes & PREFIX_MASK_SEGMENT;

	if (prefix != 0) {
		if (prefix == SEG_PREFIX_ZERO) {
			return 0;
		}
		seg = (uint8_t)(prefix - 1);
	}
"""
assert s.count(old) == 1, "arm 1 anchor not found"
open(p, "w").write(s.replace(old, ""))
PY
build_and_run build/cpu_uop_exec_selftest build/cpu_uop_exec_selftest
report "1 the inline lookup ignores the override prefix" FAIL $?
restore

# 2: the inline lookup's null-segment fault path removed -- the #GP (13) must not be delivered
cp "$TMP/uop.orig" "$UOP"
python3 - "$UOP" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = """	bool *is_null = cpu->segment_is_null;
	if (is_null[seg]) {
"""
assert s.count(old) == 1, "arm 2 anchor not found"
open(p, "w").write(s.replace(old, """	bool *is_null = cpu->segment_is_null;
	if (false && is_null[seg]) {
"""))
PY
build_and_run build/cpu_uop_exec_selftest build/cpu_uop_exec_selftest
report "2 the inline lookup never faults on a null segment" FAIL $?
restore

# 3: the inline lookup always uses the uop's default segment -- the SS slot in "non-flat"
#    must move (an EBP-relative form reads through DS instead of SS)
cp "$TMP/uop.orig" "$UOP"
python3 - "$UOP" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = """	int32_t seg_base = uop_seg_base(u->seg);
"""
assert s.count(old) == 1, "arm 3 anchor not found"
open(p, "w").write(s.replace(old, """	int32_t seg_base = uop_seg_base(DS);
"""))
PY
build_and_run build/cpu_uop_exec_selftest build/cpu_uop_exec_selftest
report "3 the lookup drops the uop's default segment" FAIL $?
restore

# 5: the per-uop statistics accumulator never published -- uop_stats.uops_executed stays low,
#    which the exec suite's own "the cache leg really ran" assertion reads
cp "$TMP/uop.orig" "$UOP"
python3 - "$UOP" <<'PY2'
import sys
p = sys.argv[1]; s = open(p).read()
old = "\t\tuop_stats.uops_executed += block_uops;\n"
assert s.count(old) == 1, "arm 5 anchor not found"
open(p, "w").write(s.replace(old, ""))
PY2
build_and_run build/cpu_uop_exec_selftest build/cpu_uop_exec_selftest
report "5 the per-uop stats accumulator is never published" FAIL $?
restore

# 6: body_ip loses its custom_modrm_resolve exception (the 0x8D/0x8F +1) -- the executor's own
#    advance test then reads a no-displacement lea as a control transfer.  Its gate is the nasm
#    corpus, exactly as PERF-4's rig arm 6 was: `make nasm` must fail on lea-nop|leave16|push.
cp "$TMP/uop.orig" "$UOP"
python3 - "$UOP" <<'PY2'
import sys
p = sys.argv[1]; s = open(p).read()
old = """	u->body_ip = (u->flags & UOP_FLAG_FETCH_INTERNAL)
	                 ? u->eip + (uint32_t)u->pfx_len + (uint32_t)u->opcode_bytes + 1u
	                 : u->eip + (uint32_t)u->len;"""
assert s.count(old) == 1, "arm 6 anchor not found"
open(p, "w").write(s.replace(old, "\tu->body_ip = u->eip + (uint32_t)u->len;"))
PY2
if make build/v86c >"$TMP/build.log" 2>&1 && make nasm >"$TMP/nasm.log" 2>&1; then
    nasm_rc=0
else
    nasm_rc=1
fi
report "6 body_ip drops the custom_modrm_resolve +1 (nasm)" FAIL $nasm_rc
restore
cp "$TMP/uop.orig" "$UOP"; make build/v86c >"$TMP/build.log" 2>&1

echo "-- restore + integrity"
make build/cpu_uop_exec_selftest >"$TMP/build.log" 2>&1
./build/cpu_uop_exec_selftest >"$TMP/run.log" 2>&1
report "7 restored tree (== control)" PASS $?
[ "$(sha256sum "$UOP" | cut -d' ' -f1)" = "$SHA_UOP" ] && echo "uop_cache.c restored byte-identical" || echo "uop_cache.c CHANGED"
[ "$(sha256sum "$EXEC" | cut -d' ' -f1)" = "$SHA_EXEC" ] && echo "uop_exec_selftest.c restored byte-identical" || echo "uop_exec_selftest.c CHANGED"

echo "arms=$(($caught + $holes)) caught=$caught holes=$holes"
[ "$holes" = 0 ] || exit 1
exit 0
