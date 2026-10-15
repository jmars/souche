#!/bin/sh
# Reproduce JIT-FIX-2's headline measurement on the CURRENT tree: is tests/perf_bench.sh
# blind to a swap of two jcc recipe-array slots, or does it catch them?
set -u
cd ~/futaie/souche || exit 2
SRC=src/cpu/jit_codegen.c
KEEP=/tmp/jitcg.fixed.c
OUT=/tmp/jitcg-repro
mkdir -p "$OUT"

cp "$SRC" "$KEEP" || exit 2
echo "== baseline sha of $SRC: $(sha256sum "$SRC" | cut -c1-16)"

python3 - <<'EOF'
import re
src = open("/tmp/jitcg.fixed.c").read()
PAIRS = [
    ('jcc8-swap-bbe',  'instr32_72',  'instr32_76'),
    ('jcc8-swap-aea',  'instr32_73',  'instr32_77'),
    ('jcc32-swap-bbe', 'instr32_0F82', 'instr32_0F86'),
    ('jcc32-swap-aea', 'instr32_0F83', 'instr32_0F87'),
    ('jcc8-swap-b0',   'instr32_70',  'instr32_72'),
    ('jcc8-swap-s6',   'instr32_78',  'instr32_76'),
]
def swap(text, a, b, tag):
    for s in (a, b):
        n = len(re.findall(r'\b%s\b' % s, text))
        assert n == 1, "%s occurs %d times, not 1" % (s, n)
    v = re.sub(r'\b%s\b' % a, '@@%s@@' % tag, text)
    v = re.sub(r'\b%s\b' % b, a, v)
    return v.replace('@@%s@@' % tag, b)
for name, a, b in PAIRS:
    v = swap(src, a, b, 'T')
    assert v != src, name
    open('/tmp/jitcg-repro/%s.c' % name, 'w').write(v)
    print("wrote", name)
EOF
[ $? -eq 0 ] || { echo "GENERATOR FAILED"; cp "$KEEP" "$SRC"; exit 2; }

run_one() { # $1 = label
    make >"$OUT/build.$1.log" 2>&1
    rc_build=$?
    sh tests/perf_bench.sh --runs 1 --label "$1" >"$OUT/bench.$1.txt" 2>&1
    rc_bench=$?
    printf '%s: build rc=%s  bench rc=%s  %s\n' "$1" "$rc_build" "$rc_bench" \
        "$(rg -o 'instructions, in_hlt=[0-9]+, exception=[-0-9]+, unimplemented=[0-9]+, eip=0x[0-9a-f]+' "$OUT/bench.$1.txt" | head -1)"
    printf '   first failing assertion: %s\n' "$(rg -m1 'perf_bench: (run [0-9]+ (executed|terminal|serial))' "$OUT/bench.$1.txt" | head -1)"
}

for v in jcc8-swap-bbe jcc8-swap-aea jcc32-swap-bbe jcc32-swap-aea jcc8-swap-b0 jcc8-swap-s6; do
    cp "$OUT/$v.c" "$SRC"
    run_one "$v"
    cp "$KEEP" "$SRC"
done

# baseline last, on the restored file
run_one baseline
if cmp -s "$KEEP" "$SRC"; then echo "restore check: source == the saved copy (cmp clean)"; else echo "RESTORE FAILED"; fi
echo "== baseline sha again: $(sha256sum "$SRC" | cut -c1-16)"
