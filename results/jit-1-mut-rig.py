#!/usr/bin/env python3
"""jit-1-mut-rig.py -- UNIT JIT-1 (M1)'s ABLE-TO-FAIL EVIDENCE (the brief's G1 second half).

WHAT IT PROVES.  `make selftest`'s new suite (src/cpu/jit_codegen_selftest.c) is only worth its
green if it can go RED.  Each arm below changes ONE thing in the emitted block -- or in the
encoder -- and REQUIRES the suite to fail; the restoration arm then requires it to pass again
on the unmutated sources.  The asymmetry (fail before, pass after) is the proof.

WHY THESE ARMS.  Each corresponds to a specific pin in the suite, i.e. to a contract the plan's
M2 breadth relies on:
  recipe-direction / spill / reload   -> the state differential (test_recipe)
  abort check                        -> the SMC pin (test_smc_abort)
  materialisation-fault check        -> the fault pin (test_fault_early_return)
  prefixes store                     -> the prefix pin (test_prefix_and_previous_ip)
  flush arithmetic                   -> the staircase pin (test_staircase)
  previous_ip store                  -> the previous_ip assertion
  page-cross mask                    -> the exit-kind tally (test_cross_return)
  REX.R in the encoder               -> the byte pins (test_encoder_pins)

A control arm runs the unmutated tree first, so a suite that fails for an environmental reason
(broken build, missing kernel image) is caught before any arm is credited.

Usage: python3 results/jit-1-mut-rig.py          (rebuilds ONE binary per arm: ~15 s each)
"""

import hashlib
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

FILES = ["src/cpu/jit_x64.c", "src/cpu/jit_codegen.c"]
BIN = "build/cpu_jit_codegen_selftest"
TARGET = "build/cpu_jit_codegen_selftest"
OUT = "results/jit-1/mut-rig.txt"

# (name, file, old, new) -- the replacement MUST match exactly once (the rig asserts that, so a
# drifted source is a rig failure and not a silenced arm).
ARMS = [
    ("recipe direction swapped", "src/cpu/jit_codegen.c",
     "jx64_mov_r32_r32(e, jitcg_greg[u->modrm & 7], jitcg_greg[u->g]);",
     "jx64_mov_r32_r32(e, jitcg_greg[u->g], jitcg_greg[u->modrm & 7]);"),

    ("spill spills only one register", "src/cpu/jit_codegen.c",
     """static void jitcg_emit_spill(jx64_t *e)
{
\tfor (int i = 0; i < 8; i++) {""",
     """static void jitcg_emit_spill(jx64_t *e)
{
\tfor (int i = 0; i < 1; i++) {"""),

    ("reload reloads only one register", "src/cpu/jit_codegen.c",
     """static void jitcg_emit_reload(jx64_t *e)
{
\tfor (int i = 0; i < 8; i++) {""",
     """static void jitcg_emit_reload(jx64_t *e)
{
\tfor (int i = 0; i < 1; i++) {"""),

    # JX_CC_L after `cmp byte [r8], 0` is never true for a flag byte of 0/1 (SF = OF = 0), so
    # the check becomes a no-op.  NOTE the first version of this arm used JX_CC_NO, which is
    # JNO (0x81) -- taken whenever OF == 0, i.e. ALWAYS -- and that made the compiled block
    # abort at every uop and uop_run's re-decode path LIVELOCK (MEASURED: 25 minutes at 100%
    # CPU, no output).  The livelock itself is a finding, recorded in results/jit-1-m1.md.
    ("SMC abort check never taken", "src/cpu/jit_codegen.c",
     "\tjx64_jcc_label(e, JX_CC_NE, l_kind_abort);",
     "\tjx64_jcc_label(e, JX_CC_L, l_kind_abort);"),

    ("materialisation-fault check dropped", "src/cpu/jit_codegen.c",
     "\t\tjx64_jcc_label(e, JX_CC_NE, l_noran);",
     "\t\t/* jx64_jcc_label(e, JX_CC_NE, l_noran); */"),

    ("prefixes store dropped", "src/cpu/jit_codegen.c",
     "\tif (u->pfx != 0) jx64_mov_m8_imm8(e, JX_RBP, JC_OFF(prefixes), u->pfx);",
     "\tif (0 && u->pfx != 0) jx64_mov_m8_imm8(e, JX_RBP, JC_OFF(prefixes), u->pfx);"),

    ("flush drops the counter add", "src/cpu/jit_codegen.c",
     "\tjx64_add_r32_r32(e, JC_TMP, JC_PENDING);",
     "\t/* jx64_add_r32_r32(e, JC_TMP, JC_PENDING); */"),

    ("previous_ip store dropped", "src/cpu/jit_codegen.c",
     "\tjx64_mov_m32_imm32(e, JX_RBP, JC_OFF(previous_ip), u->eip);",
     "\t/* jx64_mov_m32_imm32(e, JX_RBP, JC_OFF(previous_ip), u->eip); */"),

    ("page-cross mask broken", "src/cpu/jit_codegen.c",
     "\tjx64_test_r32_imm32(e, JC_TMP, 0xFFFFF000u);",
     "\tjx64_test_r32_imm32(e, JC_TMP, 0x00000000u);"),

    ("encoder takes REX.R from the wrong bit", "src/cpu/jit_x64.c",
     "\tint rr = (r >> 3) & 1, xx = (x >> 3) & 1, bb = (b >> 3) & 1;",
     "\tint rr = (r >> 5) & 1, xx = (x >> 3) & 1, bb = (b >> 3) & 1;"),
]


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def build_and_run():
    """Returns (build_rc, run_rc, first_failure_line)."""
    b = subprocess.run(["make", TARGET], capture_output=True, text=True)
    if b.returncode != 0:
        return b.returncode, None, (b.stdout + b.stderr).strip().splitlines()[-1:]
    # THROUGH A SHELL: build/*_selftest is a cosmocc APE, whose kernel exec gets ENOEXEC and
    # whose re-exec fallback lives in a shell -- a direct spawn fails with "Exec format error"
    # (the same reason the Makefile ships $(BUILD)/v86c-nasm as a shell wrapper).
    try:
        r = subprocess.run(["/bin/sh", "-c", "./" + BIN], capture_output=True, text=True,
                           timeout=60)
    except subprocess.TimeoutExpired:
        # A MUTANT THAT HANGS IS NOT A PASS.  (Named as such rather than folded into "caught":
        # a hang is a different failure mode -- executing stale code can livelock a guest that
        # patches its own page, because every re-decode recompiles -- and the reader should see
        # which arm it was.)
        return 0, "HANG", ["the suite did not terminate within 60 s"]
    first = [l for l in (r.stdout + r.stderr).splitlines() if l.startswith("FAIL")]
    return 0, r.returncode, first[:1]


def main():
    pristine = {f: open(f).read() for f in FILES}
    before = {f: sha(f) for f in FILES}
    lines = []

    def emit(s):
        print(s)
        lines.append(s)
        open(OUT, "w").write("\n".join(lines) + "\n")

    emit("# JIT-1 (M1) mutation rig -- every arm must make `%s` FAIL" % BIN)
    emit("#   sources: %s" % ", ".join("%s %s" % (f.split("/")[-1], before[f][:16]) for f in FILES))
    emit("")

    # ---- the control: the unmutated tree must PASS ----
    brc, rrc, fail = build_and_run()
    ok = brc == 0 and rrc == 0
    emit("control (no mutation)                        build rc=%s run rc=%s  %s"
         % (brc, rrc, "PASS as required" if ok else "UNEXPECTED %s" % fail))
    if not ok:
        emit("the control arm did not pass: no mutation arm can be credited")
        return 2

    caught = 0
    for name, path, old, new in ARMS:
        assert old in pristine[path], "rig: the arm text is not in the pristine source: %r" % old
        n = pristine[path].count(old)
        if n != 1:
            emit("%-44s RIG ERROR: the pattern matches %d times" % (name, n))
            open(path, "w").write(pristine[path])
            return 2
        open(path, "w").write(pristine[path].replace(old, new))
        brc, rrc, fail = build_and_run()
        b_last = fail[0] if fail else ""
        if brc != 0:
            verdict = "NOT CAUGHT (the mutant did not even build: %s)" % (
                (b_last or "?")[:90])
        elif rrc == "HANG":
            verdict = "CAUGHT (hung: the suite never terminated)"
            caught += 1
        elif rrc != 0:
            verdict = "CAUGHT (%s)" % (fail[0].strip() if fail else "suite failed")
            caught += 1
        else:
            verdict = "NOT CAUGHT -- THE SUITE PASSED ON A MUTANT"
        emit("%-44s run rc=%-3s %s" % (name, rrc, verdict))
        open(path, "w").write(pristine[path])

    # ---- the restoration arm: the pristine tree must pass again ----
    brc, rrc, fail = build_and_run()
    restored = all(sha(f) == before[f] for f in FILES)
    emit("")
    emit("restoration: sources byte-identical=%s, build rc=%s run rc=%s %s"
         % (restored, brc, rrc, "PASS as required" if (restored and rrc == 0) else "UNEXPECTED"))
    emit("")
    emit("VERDICT: %d/%d mutation arms CAUGHT; control and restoration PASS" % (caught, len(ARMS)))
    if caught != len(ARMS) or not restored or rrc != 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
