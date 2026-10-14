#!/bin/bash
# revert-attribution.sh -- PERF-1's ATTRIBUTION ARM (gate G4).
#
# The unit ships TWO changes at once: $(PERF_CFLAGS) (the optimizer flags cosmocc's
# wrapper forces off -- a Makefile-only change) and the UNITY translation unit (a
# structural change).  A before/after alone cannot say which did what, so this arm
# REMOVES exactly the second one -- the same sources, the same flags, linked per-TU as
# the tree linked before PERF-1 -- and measures it.
#
# The transformation is a sed over a COPY of the shipped Makefile (the shipped one is
# not edited; the diff is printed below as the evidence of what "revert" means):
#   * the v86c prerequisite and link argument $(UNITY_OBJ) -> $(CPU_OBJ) (+ $(GEN_OBJ)
#     back on the command line, where it was before),
#   * $(CPU_OBJ) re-defined (the shipped Makefile dropped it with the per-TU build).
#
# Usage: results/perf-1/revert-attribution.sh [runs]
set -u
cd "$(dirname "$0")/../.." || exit 2
RUNS="${1:-7}"

sed -e '/^CPU_CFLAGS := /a CPU_OBJ := $(patsubst src/cpu/%.c,$(BUILD)/cpu/%.o,$(CPU_SRC))' \
    -e 's|^\$(BUILD)/v86c: src/main.c \$(UNITY_OBJ)|$(BUILD)/v86c: src/main.c $(CPU_OBJ)|' \
    -e 's|\$(MINIBIOS_EMBED) \$(UNITY_OBJ) \$(HW_OBJ)|$(MINIBIOS_EMBED) $(CPU_OBJ) $(HW_OBJ) $(GEN_OBJ)|' \
    Makefile > /tmp/Makefile.pertu

echo "== the revert, as a diff against the shipped Makefile =="
diff -u Makefile /tmp/Makefile.pertu
echo "== building the per-TU arm (build/revert) =="
make -f /tmp/Makefile.pertu BUILD=build/revert all || exit 1
echo "== benchmarking it =="
exec tests/perf_bench.sh --runs "$RUNS" --bin build/revert/v86c \
     --label "ATTRIBUTION: per-TU link, same $(PERF_CFLAGS)" \
     --samples results/perf-1/samples.txt
