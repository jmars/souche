#!/bin/sh
# results/r3f-inject.sh -- the STATE-MATCH half of G1 (unit R3F).
#
# G1 asks for two things: the fallthrough sequence must not abort after the fix (that is
# results/r3f-probe.sh's job), and it "must produce the same state as the reference for that
# input".  The second half cannot be met by the SHIPPED binary alone, and the reason is
# measured, not asserted: results/r3f-fallthrough.mjs counts 174 reachable fallthrough
# prefix combinations and ZERO of them have an unprefixed body the port has written yet --
# the shipped binary therefore STOPS BY NAME at the plain entry, exactly as the same
# encoding without the stray prefix does (it is the same arm's body).
#
# So this rig supplies the missing body as a STAND-IN and measures what happens: if the arm
# really executes the unprefixed entry, the run must continue and the transcript must match
# the reference's.  Nothing in the repo is touched: the emitter and the dispatcher are built
# in build/r3f-inject/ from a COPY of src/cpu, with the stand-in definitions added there.
#
# The stand-in is a NO-OP, and the rig does NOT claim a state match: MEASURED (results/
# r3f-gates.txt, G1), the reference's MOVQ is not state-neutral -- its MMX write moves the
# x87 stack-empty bit and tag word (0x00/0x5554 against the reset 0xff/0xffff).  So the
# no-op stand-in reproduces everything EXCEPT those x87 fields, and that residual is exactly
# the unported MOVQ semantics (M1b's).  What the rig establishes is the ARM: with a body
# present the fallthrough runs to completion instead of aborting or stopping.
#
# Usage: sh results/r3f-inject.sh    (after results/r3f-probe.sh has built the images)

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

COSMOCC=${COSMOCC:-$HOME/.local/cosmo/bin/cosmocc}
WORK=build/r3f-inject
LIMIT=1000

mkdir -p "$WORK/cpu" || exit 1

# --- 1. a scratch copy of the port's instruction sources + the stand-in bodies ----------
for f in src/cpu/*.c src/cpu/*.h; do cp "$f" "$WORK/cpu/"; done
rm -f "$WORK/cpu/cpu_selftest.c" "$WORK/cpu/interpreter_gen.c"
cat > "$WORK/cpu/r3f_standin.c" <<'EOF'
/* STAND-IN bodies (this scratch build only): MODE=1's plain entry for 0F 6F.  Both suffixes
 * must be defined or the generated mem/reg calls would not link. */
#include <stdint.h>
void instr_0F6F_reg(int32_t r1, int32_t r) { (void)r1; (void)r; }
void instr_0F6F_mem(int32_t addr, int32_t r) { (void)addr; (void)r; }
EOF

# --- 2. the emitter, pointed at the scratch copy ---------------------------------------
cat >> "$WORK/cpu/instructions_0f.h" <<'EOF'

/* STAND-IN declarations (this scratch build only), appended to a COPY of the header: the
 * generated dispatcher includes "instructions_0f.h", and the real one does not declare the
 * bodies this rig supplies. */
void instr_0F6F_mem(int32_t addr, int32_t r);
void instr_0F6F_reg(int32_t r1, int32_t r);
EOF

cp gen/interpreter_c.js "$WORK/emitter.js"
python3 - "$WORK" <<'PY'
import sys
w = sys.argv[1]
s = open("./gen/interpreter_c.js").read()
s = s.replace('import x86_table from "../v86/gen/x86_table.js";',
              'import x86_table from "/home/jaye/futaie/souche/v86/gen/x86_table.js";')
assert s.count('const CPU_DIR = path.join(ROOT, "src", "cpu");') == 1
s = s.replace('const CPU_DIR = path.join(ROOT, "src", "cpu");', f'const CPU_DIR = "{w}/cpu";')
assert s.count('const OUT_FILE = path.join(CPU_DIR, "interpreter_gen.c");') == 1
s = s.replace('const OUT_FILE = path.join(CPU_DIR, "interpreter_gen.c");',
              f'const OUT_FILE = "{w}/interpreter_gen.c";')
open(w + "/emitter.js", "w").write(s)
print("  emitter copy: 3 lines patched (import path, CPU_DIR, OUT_FILE); nothing else differs")
PY
node "$WORK/emitter.js" || exit 1

# --- 3. build the stand-in binary (the tree's own objects, only the dispatcher replaced) -
"$COSMOCC" -std=gnu11 -O2 -Wall -Wextra -Werror -I "$WORK/cpu" -I src -c -o "$WORK/interpreter_gen.o" "$WORK/interpreter_gen.c" || exit 1
"$COSMOCC" -std=gnu11 -O2 -Wall -Wextra -Werror -I "$WORK/cpu" -I src -c -o "$WORK/r3f_standin.o" "$WORK/cpu/r3f_standin.c" || exit 1
objs=$(ls build/cpu/*.o | grep -v 'interpreter_gen\.o$')
"$COSMOCC" -std=gnu11 -O2 -Wall -Wextra -I src -o "$WORK/v86c-standin" src/main.c $objs "$WORK/interpreter_gen.o" "$WORK/r3f_standin.o" || exit 1

# --- 4. the measurement ------------------------------------------------------------------
for mode in 1 2 0; do
	[ -f "build/r3f-probe/probe.$mode.img" ] || {
		echo "build/r3f-probe/probe.$mode.img missing -- run results/r3f-probe.sh first"
		exit 1
	}
done
echo "-------------------------------------------------------------------"
for mode in 1 2 0; do
	img=build/r3f-probe/probe.$mode.img
	sh -c "$WORK/v86c-standin $img $LIMIT" > "$WORK/out.$mode.txt" 2> "$WORK/err.$mode.txt"
	rc=$?
	echo "MODE=$mode  stand-in binary exit=$rc  transcript=$(wc -c < "$WORK/out.$mode.txt") bytes $(head -1 "$WORK/err.$mode.txt")"
	cp "$WORK/out.$mode.txt" "$WORK/out.$mode.saved"
	if [ "$rc" = 0 ]; then
		if diff -u "build/r3f-probe/js.$mode.txt" "$WORK/out.$mode.txt" > "$WORK/d.$mode"; then
			echo "          vs the JS reference: transcripts BYTE-IDENTICAL"
		else
			echo "          vs the JS reference: DIFFERS --"
			grep -E '^[+-][^+-]' "$WORK/d.$mode" | sed 's/^/            /'
		fi
	fi
done
echo "-------------------------------------------------------------------"
echo "the shipped binary (no stand-in) on the same images, for contrast:"
for mode in 1 2 0; do
	sh -c "./build/v86c build/r3f-probe/probe.$mode.img $LIMIT" > /dev/null 2> "$WORK/err.ship.$mode"
	printf '  MODE=%s exit=%s %s\n' "$mode" "$?" "$(head -1 "$WORK/err.ship.$mode")"
done
