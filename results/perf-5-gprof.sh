#!/bin/bash
# perf-5-gprof.sh -- PERF-5's G4: THE PROFILE METHOD, as a script the next unit can re-run.
#
# WHAT IT DOES (the orchestrator's method, made reproducible):
#   1. copy the tree to a scratch directory (the shipped tree is never built with -pg);
#   2. patch src/main.c's HOST-FAULT handler -- the one edit host glibc needs (see below);
#   3. build the SHIPPED unity TU with the shipped flags PLUS -pg, with the HOST compiler
#      (`make CC=cc`, NOT cosmocc) -- MEASURED: cosmocc's APE never writes gmon.out, and the
#      host link additionally needs -lm (fmin at hw/machine.o) which the Makefile's link line
#      does not carry, so it rides in on CFLAGS (the link rule expands $(CFLAGS) too);
#   4. run the unit's benchmark workload (200,095,214 instructions of the kernel boot);
#   5. `gprof -b -p` (flat) and `-q` (call graph), keeping both raw.
#
# THE main.c PATCH, and why it is needed: boot_host_fault()'s #ifdef __x86_64__ block walks the
# signal context's register file.  cosmocc's ucontext names the registers (uc_mcontext.rsp...);
# glibc's is a gregset_t ARRAY, and the file's own __GLIBC__ branch indexes it -- but the block
# also declares `ucontext_t *uc = context;` and uses named members in the #else arm, which host
# glibc does not compile.  The patch replaces the whole diagnostic block with the one-line
# fprintf + _Exit.  It is DIAGNOSTIC-ONLY code (a host fault handler, never a guest path), which
# is why replacing it cannot affect the profile's meaning.
#
# READ THE OUTPUT WITH BOTH CAVEATS:
#   (A) SELF TIMES ARE NOT TIMES.  -pg inserts an mcount call at every function entry; the
#       orchestrator MEASURED the distortion at ~7x (interpreted_run's self time under -pg is
#       46% of a 1.88 s total for a workload that takes 3.2 s without instrumentation).  Use the
#       CALL COUNTS and the ORDERING.
#   (B) THE CALL COUNTS DESCRIBE THE -pg BUILD, NOT NECESSARILY THE SHIPPED ONE.  MEASURED,
#       and the reason PERF-5 re-checked every target against build/unity.o's relocations: the
#       -pg build attributes the executor loop and its callees differently from the -O2 build
#       (gprof lists `interpreted_run` with 238,547 calls and 46% self, while the shipped tree's
#       executor is uop_run.part.0/uop_exec, and `interpreted_run` there is only the rare
#       fallback).  A count is evidence that a call happens SOMEWHERE in the program; whether it
#       is on the shipped hot path is a separate question, and `objdump -r build/unity.o` is
#       what answers it (results/perf-5-tune.md ss2).
#
# Usage: results/perf-5-gprof.sh [--tree DIR] [--kernel PATH] [--limit N] [--keep]
#        (default scratch: /tmp/v86c-perf5-pg; default limit 200000000)

set -u
cd "$(dirname "$0")/.." || exit 2
SRC=$PWD
SCRATCH=/tmp/v86c-perf5-pg
KERNEL=$SRC/tests/images/vmlinuz-i386
LIMIT=200000000
KEEP=0
while [ $# -gt 0 ]; do
    case "$1" in
        --tree) SCRATCH=$2; shift 2 ;;
        --kernel) KERNEL=$2; shift 2 ;;
        --limit) LIMIT=$2; shift 2 ;;
        --keep) KEEP=1; shift 1 ;;
        *) echo "perf-5-gprof: unknown argument $1" >&2; exit 2 ;;
    esac
done

OUT="$SRC/results/perf-5"
mkdir -p "$OUT"
echo "== perf-5-gprof: scratch=$SCRATCH limit=$LIMIT"

rm -rf "$SCRATCH"; mkdir -p "$SCRATCH"
# the tree, minus the build products and the READ-ONLY upstream checkout (symlinked)
( cd "$SRC" && tar cf - --exclude=build --exclude=v86 --exclude=gmon.out . ) | ( cd "$SCRATCH" && tar xf - )
ln -s "$SRC/v86" "$SCRATCH/v86"

# (2) the host-glibc patch of the HOST-FAULT handler
python3 - "$SCRATCH/src/main.c" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
start = s.index("static void boot_host_fault(int signal, siginfo_t *info, void *context)")
end = s.index("static void boot_install_fault_handlers(void)")
body = """static void boot_host_fault(int signal, siginfo_t *info, void *context)
{
	/* G4 SCRATCH PATCH (results/perf-5-gprof.sh): host glibc cannot compile the port's
	 * named-member ucontext access.  Diagnostic only -- never a guest-visible path. */
	(void)context;
	fprintf(stderr, "v86c: HOST FAULT (%d) at %p instruction %u\\n", signal, info->si_addr,
	        (unsigned)cpu->instruction_counter);
	_Exit(128 + signal);
}

"""
open(p, "w").write(s[:start] + body + s[end:])
PY

# (3) the -pg build
( cd "$SCRATCH" && make CC=cc CFLAGS='-std=gnu11 -O2 -pg -Wall -Wextra -lm' build/v86c ) \
    > "$OUT/gprof-build.txt" 2>&1
rc=$?
echo "build rc=$rc (log: $OUT/gprof-build.txt)"
[ $rc = 0 ] || { tail -20 "$OUT/gprof-build.txt"; exit $rc; }

# (4) the run
( cd "$SCRATCH" && rm -f gmon.out && \
  ./build/v86c --bzimage "$KERNEL" "$LIMIT" --cmdline "console=ttyS0" ) \
    > "$OUT/gprof-run.txt" 2>&1
echo "run rc=$? (log: $OUT/gprof-run.txt)"
[ -s "$SCRATCH/gmon.out" ] || { echo "no gmon.out: the -pg runtime did not run (cosmocc?)"; exit 1; }

# (5) the parse
gprof -b -p "$SCRATCH/build/v86c" "$SCRATCH/gmon.out" > "$OUT/gprof-flat.txt" 2>&1
gprof -b -q "$SCRATCH/build/v86c" "$SCRATCH/gmon.out" > "$OUT/gprof-callgraph.txt" 2>&1
echo "== flat profile (top 25 by self time; read the CALL COUNTS, see the header)"
sed -n '1,32p' "$OUT/gprof-flat.txt"
echo "== the call counts this unit's two targets are ranked by:"
grep -E "uop_materialize_addr|get_seg|uop_exec|uop_run|interpreted_run" "$OUT/gprof-flat.txt" || true
[ "$KEEP" = 1 ] || echo "(scratch kept at $SCRATCH for inspection)"
