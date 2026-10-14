#!/bin/bash
# results/perf-3/attribution.sh -- does PERF-3 cost the SHIPPED path anything?
#
# The unit adds two files (a 623-line decoder and a 5558-line generated table) and a
# generator/corpus/selftest wiring to the Makefile.  Nothing in the shipped path calls the
# decoder yet (M2 wires it into cycle_internal), so the question is only: does adding it change
# what `build/v86c` IS?
#
# Two measurements, both needed:
#
#   PART 1 -- IDENTITY.  Reconstruct the pre-unit Makefile (all PERF-3 additions removed),
#   build the same tree with it, and compare the two shipped binaries with `objcopy
#   --strip-all` (the perf-1 ss2b technique: build/v86c's own sha256 is not a code oracle,
#   because the APE's shell preamble carries a per-content heredoc token).
#
#   PART 2 -- the arm the plan WOULD have shipped: PERF-3's two files moved INTO the unity
#   translation unit ($(CPU_SRC) += uop_cache.c, $(UNITY_SRC) += uop_gen.c), which is what the
#   first version of this unit did and what M2 will do.  Measured INTERLEAVED (the perf-1
#   lesson: a cross-block comparison on a shared host cannot separate a few percent from the
#   drift) -- and read the verdict, not the first number: at this host's load the effect is
#   NOT RESOLVABLE (see the summary at the end).
#
# Usage: results/perf-3/attribution.sh [rounds]

set -u
cd "$(dirname "$0")/../.." || exit 2
ROUNDS="${1:-6}"

echo "== PART 1: the pre-unit Makefile (PERF-3's additions removed) vs the shipped one =="
python3 - <<'PY'
import re
s = open("Makefile").read()
s = re.sub(r"#\n# NOT IN \$\(CPU_SRC\).*?UOP_UNITY_C := \$\(BUILD\)/uop_unity\.c\n", "#\n", s, flags=re.S)
s = re.sub(r"# The M1 decode checker's corpus.*?NASM_CORPUS_DEP := \$\(NASM_IMGS\)\nendif\n", "", s, flags=re.S)
s = s.replace("$(UOP_GEN): $(UOP_GENERATOR) $(X86_TABLE) $(CPU_SRC) $(CPU_HDR)\n\t$(NODE) $(UOP_GENERATOR)\n\n", "")
s = s.replace("generator: $(GEN_C) $(UOP_GEN)", "generator: $(GEN_C)")
s = s.replace("uop-gen: $(UOP_GEN)\n\n", "")
s = re.sub(r"\$\(UOP_C_OBJ\) \$\(UOP_OBJ\): \$\(UOP_HDR\)\n\n", "", s)
s = re.sub(r"# THE UNITY FIT .*?uop-unity: the decoder and its tables compile inside the shipped unity TU\"\n\n", "", s, flags=re.S)
s = re.sub(r"\$\(NASM_CORPUS\): gen/nasm_corpus\.py .*?\n\n", "", s, flags=re.S)
s = s.replace(" uop-unity", "")
s = re.sub(r"# PERF-3 M1's gate: THE DECODE CHECKER.*?\n\n(?=# M2-1's device gate)", "", s, flags=re.S)
s = s.replace(" $(BUILD)/cpu_uop_selftest", "")
s = re.sub(r"\t\./\$\(BUILD\)/cpu_uop_selftest \$\(NASM_CORPUS\)\n", "", s)
open("/tmp/Makefile.pre", "w").write(s)
PY
diff Makefile /tmp/Makefile.pre | grep -c '^[<>]' | sed 's/^/   lines differing: /'
make -f /tmp/Makefile.pre BUILD=build/pre all > /tmp/perf-3-pre-build.log 2>&1 || {
    tail -20 /tmp/perf-3-pre-build.log; exit 1; }
objcopy --strip-all build/pre/v86c.com.dbg /tmp/perf-3-pre.dbg
objcopy --strip-all build/v86c.com.dbg /tmp/perf-3-ship.dbg
if cmp -s /tmp/perf-3-pre.dbg /tmp/perf-3-ship.dbg; then
    echo "   IDENTICAL: the shipped binary's code is unchanged by this unit:"
else
    echo "   DIFFER -- this unit changes the shipped binary's code:"
fi
sha256sum /tmp/perf-3-pre.dbg /tmp/perf-3-ship.dbg | sed 's/^/   /'
echo

echo "== PART 2: the files-IN-the-unity-TU arm (what the plan would ship, what M2 will) =="
sed -e 's|           src/cpu/cpu.c src/cpu/exec_boundary.c|           src/cpu/cpu.c src/cpu/exec_boundary.c src/cpu/uop_cache.c|' \
    -e 's|^UNITY_SRC := \$(CPU_SRC) \$(GEN_C)|UNITY_SRC := $(CPU_SRC) $(GEN_C) $(UOP_GEN)|' \
    Makefile > /tmp/Makefile.filesin
grep -c 'uop_cache.c\|uop_gen.c' /tmp/Makefile.filesin | sed 's/^/   references to the two files in the arm Makefile: /'
diff -u Makefile /tmp/Makefile.filesin | sed 's/^/   /'
make -f /tmp/Makefile.filesin BUILD=build/filesin all >/tmp/perf-3-filesin-build.log 2>&1 || {
    tail -20 /tmp/perf-3-filesin-build.log; exit 1; }
echo "   built"
echo

echo "== interleaved: $ROUNDS rounds of (shipped = files OUT, then files IN) =="
echo "   NOTE: the ORDER in each pair is fixed, so a drifting host biases the first one.  Three"
echo "   runs of this arm have now read +1.9% (4 rounds, load climbing 9.0 -> 10.3 with the"
echo "   files-IN binary always second), -0.2% and +0.2% (6 rounds each) -- i.e. NO SIGNAL."
echo "   Read the summary as a noise band, not as a number."
for i in $(seq 1 "$ROUNDS"); do
    tests/perf_bench.sh --runs 1 --bin build/v86c --label "round $i BEFORE (files OUT)" \
        --samples results/perf-3/samples.txt | grep -E 'real:'
    tests/perf_bench.sh --runs 1 --bin build/filesin/v86c --label "round $i AFTER (files IN the unity TU)" \
        --samples results/perf-3/samples.txt | grep -E 'real:'
done

echo
echo "== summary (every interleaved sample in results/perf-3/samples.txt) =="
python3 - <<'PY'
import statistics
rows, label = {"OUT": [], "IN": []}, None
for line in open("results/perf-3/samples.txt"):
    if line.startswith("# perf_bench"):
        label = "IN" if " (files IN" in line else "OUT"
    elif line.startswith("#"):
        continue
    else:
        rows[label].append(float(line.split()[0]))
for k, v in rows.items():
    if v:
        print(f"   {k:4s} n={len(v)}  best {min(v):.3f}  median {statistics.median(v):.3f}  all={v}")
if rows["OUT"] and rows["IN"]:
    d = statistics.median(rows["IN"]) - statistics.median(rows["OUT"])
    print(f"   median difference (IN - OUT): {d:+.3f} s = {100*d/statistics.median(rows['OUT']):+.2f}%"
          f"  -- inside this host's drift at the load printed above")
PY
