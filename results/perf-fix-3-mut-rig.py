#!/usr/bin/env python3
"""perf-fix-3-mut-rig.py -- PERF-FIX-3's ASYMMETRY RIG: every new gate, shown ABLE TO FAIL.

THE DISCIPLINE (AGENTS.md, the project's rigs): a fix is proved by ASYMMETRY -- the arm must
FAIL on the pre-fix tree and PASS on the fixed one -- one mutation per arm, applied to a
copy-tracked original, the gate rebuilt from the mutated source (never a stale object), the
verdict taken from the EXIT CODE, and the tree restored afterwards.  Arm 0 is the control:
the unmutated tree must be GREEN or the rig is measuring itself.

The arms are the INVERSE of each fix in this unit, each named by the gate it must trip:

  1. paging.h   -- remove the SAFE_READ_WRITE8/16/32 invalidation hooks (the unhooked tree of
                   BLOCKER-1): the RMW SMC arm must fail, and so must the extended
                   reconciliation check (results/perf-fix-3-recon-check.py) -- that pair is
                   the whole point: the OLD gate was green on this tree.
  2. memory.c   -- remove the write16 straddle hook: straddle-w16 must fail.
  3. memory.c   -- remove the write32 straddle hook: straddle-w32 must fail.
  4. uop_cache.c-- the REVIEW's SHOULD-FIX-2a (materialise BEFORE the guard): the guard-order
                   pin must fail, i.e. the prescribed reorder is DETECTABLY wrong (it delivers
                   #GP where the reference delivers #NM).
  5. uop_cache.c-- `return true` on the materialise fault (SHOULD-FIX-2b's pre-fix state): the
                   faulted-flush pin must fail.

Usage: python3 results/perf-fix-3-mut-rig.py [--out FILE]
"""
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
OUT = "results/perf-fix-3/mut-rig.txt"
if len(sys.argv) == 3 and sys.argv[1] == "--out":
    OUT = sys.argv[2]
os.makedirs(os.path.dirname(OUT), exist_ok=True)

SELFTEST = "build/cpu_uop_exec_selftest"
RECON = "results/perf-4-recon.sh"
log_lines = []


def emit(line):
    print(line)
    log_lines.append(line)


def run(cmd):
    p = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def mutate(path, spec):
    """apply a ("text", [(old,new)]) or ("lines", callable(lines)->lines) mutation"""
    kind, fn = spec
    with open(path) as f:
        text = f.read()
    if kind == "text":
        for old, new in fn:
            n = text.count(old)
            if n != 1:
                return "%r occurs %d times in %s" % (old[:60], n, path)
            text = text.replace(old, new, 1)
    else:
        text = "\n".join(fn(text.split("\n")))
    with open(path, "w") as f:
        f.write(text)
    return None


# ----------------------------------------------------------------------------------
# the arms: (name, file, mutation, the gate output line that must appear)
# ----------------------------------------------------------------------------------
def strip_hook(lines):
    out = []
    for line in lines:
        if re.match(r"^\s*uop_invalidate_page_fast\(phys_addr_\);.*\\\s*$", line):
            out.append(line.replace("uop_invalidate_page_fast(phys_addr_);",
                                    "(void)phys_addr_; /* RIG: hook removed */", 1))
        else:
            out.append(line)
    return out


def move_guard_after_materialise(lines):
    guard = ("\tif (u->ts_guard != UOP_TS_NONE) {\n"
             "\t\tbool ok = u->ts_guard == UOP_TS_MMX ? task_switch_test_mmx() : task_switch_test();\n"
             "\t\tif (!ok) return false;\n"
             "\t}\n\n")
    text = "\n".join(lines)
    assert text.count(guard) == 1, "the guard block is not unique"
    text = text.replace(guard, "", 1)
    assert text.count("\tswitch (u->shape) {") == 1, "the shape switch is not unique"
    text = text.replace("\tswitch (u->shape) {", guard + "\tswitch (u->shape) {", 1)
    return text.split("\n")


ARMS = [
    ("0 CONTROL: the unmutated tree", None, None, None),
    ("1 BLOCKER-1 inverse: the SAFE_READ_WRITE* hooks removed (paging.h)",
     "src/cpu/paging.h", ("lines", strip_hook),
     ("smc-rmw:", "ram32(0x3000) == 0x2A")),
    ("2 SHOULD-FIX-1 inverse: the write16 straddle hook removed (memory.c)",
     "src/cpu/memory.c",
     ("text", [("uop_invalidate_page_fast(addr + 1u)", "(void)(addr + 1u) /* RIG */")]),
     ("straddle-w16:", "ram32(0x3000) == 0x11")),
    ("3 SHOULD-FIX-1 inverse: the write32 straddle hook removed (memory.c)",
     "src/cpu/memory.c",
     ("text", [("uop_invalidate_page_fast(addr + 3u)", "(void)(addr + 3u) /* RIG */")]),
     ("straddle-w32:", "ram32(0x3000) == 0x11")),
    ("4 SHOULD-FIX-2a, THE REVIEW'S PRESCRIPTION (materialise before the guard)",
     "src/cpu/uop_cache.c", ("lines", move_guard_after_materialise),
     ("guard-order:", "on == 7")),
    ("5 SHOULD-FIX-2b inverse: `return true` on a materialise fault (uop_cache.c)",
     "src/cpu/uop_cache.c",
     ("text", [("if (cpu->pagefault) return false;",
                "if (cpu->pagefault) return true; /* RIG */")]),
     ("faulted-flush:", "flushes_on == 4")),
]

emit("== PERF-FIX-3 asymmetry rig %s" % time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
emit("== gates: %s (the M2 suite with the four new pins) and %s" % (SELFTEST, RECON))

caught = 0
holes = 0
for name, path, how, expect in ARMS:
    emit("")
    emit("ARM %s" % name)
    backup = path + ".rigbak" if path else None
    if path:
        shutil.copy(path, backup)
        err = mutate(path, how)
        if err:
            emit("   COULD NOT APPLY THE MUTATION: %s" % err)
            shutil.copy(backup, path)
            holes += 1
            continue
        rc, out = run("make %s 2>&1 | tail -2" % SELFTEST)
        if not os.path.exists(SELFTEST):
            emit("   BUILD FAILED:\n%s" % out)
            shutil.copy(backup, path)
            holes += 1
            continue
    rc, out = run("./%s" % SELFTEST)
    failures = re.search(r"(\d+) failures", out)
    fails = int(failures.group(1)) if failures else -1
    recon_rc, recon_out = run("bash %s --out /tmp/rig-recon.txt" % RECON)
    if path is None:
        verdict = "GREEN" if (rc == 0 and recon_rc == 0) else "NOT GREEN (the rig is measuring itself)"
        emit("   %s: selftest rc %d (%d failures), recon rc %d" % (verdict, rc, fails, recon_rc))
        if rc != 0 or recon_rc != 0:
            holes += 1
    else:
        banner, assertion = expect
        caught_here = (rc != 0) and (banner in out) and (assertion in out)
        recon_note = ""
        if name.startswith("1 "):
            recon_note = ", recon rc %d (must be NONZERO: the extended gate closes the class)" % recon_rc
            caught_here = caught_here and recon_rc != 0
        emit("   %s: selftest rc %d (%d failures); the pin '%s' and the assertion '%s'%s"
             % ("CAUGHT" if caught_here else "NOT CAUGHT", rc, fails, banner, assertion,
                recon_note))
        for line in out.split("\n"):
            if line.startswith("FAIL ") or line.strip().startswith(banner):
                emit("      %s" % line.strip())
        if caught_here:
            caught += 1
        else:
            holes += 1
        shutil.copy(backup, path)
        os.remove(backup)
        run("make %s > /dev/null 2>&1" % SELFTEST)

emit("")
emit("== ARMS: %d, CAUGHT: %d, HOLES: %d (a hole is an arm the gates could not see)"
     % (len(ARMS) - 1, caught, holes))
with open(OUT, "w") as f:
    f.write("\n".join(log_lines) + "\n")
run("bash %s --out results/perf-fix-3/reconciliation.txt" % RECON)  # leave the GOOD artifact
emit("== wrote %s (and re-ran the reconciliation on the restored tree)" % OUT)
sys.exit(1 if holes else 0)
