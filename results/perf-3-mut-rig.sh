#!/bin/bash
# results/perf-3-mut-rig.sh -- G3: the ASYMMETRY rig for the M1 decode checker.
#
# Each arm is ONE mutation of the DECODER (src/cpu/uop_cache.c, or the emitter's output) --
# never of the checker -- applied to the real file, with the checker REBUILT FROM SCRATCH and
# run, and the file restored afterwards.  An arm proves nothing unless it fails; the output
# records, per arm, the exit status, the first FAIL line, and sha256 of the checker binary
# (which is what shows the rebuild really happened -- a same-second, same-length edit that a
# stale object answered for would show the SAME hash).
#
#     results/perf-3-mut-rig.sh            # all arms
#     results/perf-3-mut-rig.sh <n>        # one arm, by number
#
# The unmutated control is arm 0: the shipped checker is green before the first mutation, so a
# red arm is the mutation's doing and not the rig's.

set -u

cd "$(dirname "$0")/.." || exit 2
DECODER=src/cpu/uop_cache.c
CHECKER=build/cpu_uop_selftest
CORPUS=build/nasm_corpus.bin
BAK=/tmp/perf-3-decoder.bak

only="${1:-}"

# exact-string replacement with a REQUIRED single match: a mutation that does not apply is a
# silent no-op otherwise, which is the one error that would make this rig lie
apply() {
    python3 - "$1" "$2" "$3" <<'PY'
import sys
path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(path).read()
n = s.count(old)
if n != 1:
    sys.exit("mutation does not apply exactly once (found %d)" % n)
open(path, "w").write(s.replace(old, new))
PY
}

run_arm() {
    local num="$1" name="$2"; shift 2
    if [ -n "$only" ] && [ "$only" != "$num" ]; then return 0; fi

    cp "$DECODER" "$BAK"
    if [ "$num" != 0 ]; then
        if ! apply "$DECODER" "$1" "$2"; then
            echo "ARM $num  $name: MUTATION DID NOT APPLY"
            cp "$BAK" "$DECODER"
            return 1
        fi
    fi

    # from scratch: the binary is REMOVED, so a stale object cannot answer for the mutation
    rm -f "$CHECKER"
    if ! make -s "$CHECKER" >/tmp/perf-3-mut-build.log 2>&1; then
        echo "ARM $num  $name: build FAILED (see /tmp/perf-3-mut-build.log)"
        cp "$BAK" "$DECODER"
        return 1
    fi
    local sha; sha=$(sha256sum "$CHECKER" | cut -c1-12)
    "./$CHECKER" "$CORPUS" >/tmp/perf-3-mut-run.log 2>&1
    local rc=$?
    local verdict first
    if [ $rc -eq 0 ]; then
        verdict="NOT CAUGHT (checker still green)"
    else
        verdict="caught (rc=$rc)"
    fi
    first=$(grep -m1 -E '^FAIL|assert|Aborted' /tmp/perf-3-mut-run.log | cut -c1-150)
    printf 'ARM %-2s %-52s %s  binary=%s\n' "$num" "$name" "$verdict" "$sha"
    if [ -n "$first" ]; then printf '        first: %s\n' "$first"; fi

    cp "$BAK" "$DECODER"
    rm -f "$CHECKER"
    make -s "$CHECKER" >/dev/null 2>&1
}

run_arm 0 "control: the shipped decoder, no mutation"

run_arm 1 "dec_read16 consumes ONE extra byte (imm16 widths)" \
    '	int32_t high = dec_read8();
	return low | (high << 8);' \
    '	int32_t high = dec_read8();
	return low | (high << 8) | (dec_read8() << 16);'

run_arm 2 "0x66 is not a prefix byte any more" \
    '	case 0x66: /* instr_66 */
		*pfx |= PREFIX_MASK_OPSIZE;
		return true;' \
    '	case 0x99: /* instr_66 */
		*pfx |= PREFIX_MASK_OPSIZE;
		return true;'

run_arm 3 "the prefix accumulator leaks across instructions (the bug the checker found)" \
    '		uint8_t pfx = 0;
		uint32_t inst_start = eip;' \
    '		uint8_t pfx = n ? dec_pfx : 0;
		uint32_t inst_start = eip;'

run_arm 4 "a sib base of EBP with a displacement uses DS instead of SS" \
    '			u->base = EBP;
			u->seg = SS;
		}
		else {              /* without one: the base IS a disp32, and the segment is DS */' \
    '			u->base = EBP;
			u->seg = DS;
		}
		else {              /* without one: the base IS a disp32, and the segment is DS */'

run_arm 5 "sib index field 4 (no index) becomes an index" \
    '	if (m != 4) {           /* index 4 is "no index" (modrm.rs: m == 4 -> offset 0) */' \
    '	if (m != 5) {           /* index 4 is "no index" (modrm.rs: m == 4 -> offset 0) */'

run_arm 6 "a block_end instruction no longer ends the block" \
    '		if (u->flags & UOP_FLAG_BLOCK_END) {' \
    '		if (0) {'

run_arm 7 "a fixed_g MEMORY form gets the REGISTER form's shape" \
    '		if (fixed_g) return has_imm ? UOP_SHAPE_MEMF1 : UOP_SHAPE_MEMF0;' \
    '		if (fixed_g) return has_imm ? UOP_SHAPE_REGF1 : UOP_SHAPE_REGF0;'

run_arm 8 "the decode window is two pages wide (crossing instructions get decoded)" \
    '	dec_end_virt = (virt_start | 0xFFFu) + 1;' \
    '	dec_end_virt = (virt_start | 0xFFFu) + 0x1001;'

echo "done (the decoder is restored to the shipped bytes: $(sha256sum "$DECODER" | cut -c1-12))"
