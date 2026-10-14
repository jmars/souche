#!/usr/bin/env python3
"""Differential oracle for the v86 nasm corpus (v86/tests/nasm/*.asm).

For each test file the pipeline is:

    v86/tests/nasm/<t>.asm
      -> tests/nasm2gas.py          (nasm -> GNU as, .intel_syntax noprefix)
      -> as --32                    (assemble)
      -> ld -m elf_i386 <geometry>  (link at upstream's addresses)
      -> build/nasm/<t>.img         (a bootable ELF, load_multiboot's ELF branch)
      -> build/nasm/mkfixture       (ptrace ground truth, gdb-extract-def format)
      -> the ACTUAL implementation  (default: the JS reference, oracle/nasm_state.mjs)
      -> compare with run.js's own logic (v86/tests/nasm/run.js:384-510)

This replaces upstream's nasm -> gdb -> v86 chain on a host without nasm or gdb.
The comparison is a transcription of upstream's, field for field and constant for
constant (reg32, the memory window, `eflags & MASK_ARITH`, the fpu/mm branch keyed
on FPU_TAG_ALL_INVALID, the FPU status word under FPU_STATUS_MASK, the
7-significant-digit float tolerance, the exception kind, and the exception eip),
so a failure here means the same thing it means upstream -- with ONE DELIBERATE
REPAIR (unit R4F, the G0 item, commented at the comparison and measured by
results/r4f-probe.sh): upstream's `fpu_tag !== FPU_TAG_ALL_INVALID` compares a
32-bit SIGNED fixture field against that 16-bit constant and is therefore
unreachable, which left the MMX registers compared by nothing; the test here masks
the fixture's high half first, so the mm branch is taken exactly when the LOW 16
bits ARE FPU_TAG_ALL_INVALID.
Everything else is upstream's logic, unaltered.

The ACTUAL side defaults to the JS reference -- run.js's own emulator -- which
validates the fixtures and the comparison before any C exists:

    python3 tests/nasm_run.py --stage run

Later units point --actual at the C port ("<cmd> {img} {limit}") and shrink
tests/nasm_skip.txt as instructions land.

Stages: translate (as+ld), fixtures (mkfixture), run (compare), all = the three.

Read-only: v86/ is never written to.
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent

CORPUS_DIR = ROOT / "v86" / "tests" / "nasm"
ORACLE = ROOT / "oracle" / "nasm_state.mjs"
TRANSLATOR = HERE / "nasm2gas.py"
WORKDIR = ROOT / "build" / "nasm"
SKIP_LIST = HERE / "nasm_skip.txt"

# Upstream's link geometry (v86/tests/nasm/create_tests.js:127-130), plus one
# measured deviation: -z noseparate-code.
#
# With ld's modern default (-z separate-code) a test that has a .data section
# (63 of the 123: everything with `section .data`) gets .data in its own
# page-aligned PT_LOAD at file offset 0x2000, and ld then places the orphan
# .multiboot section after it, at file offset 0x213c. v86 finds a multiboot image
# by scanning only the first MULTIBOOT_SEARCH_BYTES = 8192 bytes of the file
# (v86/src/cpu.js:1327-1338); finding neither magic nor the ELF branch there, its
# loader falls through and the guest never starts at all (MEASURED: eip stays at
# the reset vector 0xffff0 and the guest wanders the BIOS area). With
# -z noseparate-code every image -- .data or not -- puts .multiboot at file offset
# 0x111c-0x11c0, inside the window, with .text/.bss/entry/PADDRs unchanged.
LD_GEOMETRY = [
    "-m", "elf_i386",
    "-z", "noseparate-code",
    "--section-start=.bss=0x100000",
    "--section-start=.text=0x80000",
    "--section-start=.multiboot=0x20000",
    "-e", "_start",
]

# run.js:49-52 -- the comparison constants, verbatim.
MASK_ARITH = 1 | 1 << 2 | 1 << 4 | 1 << 6 | 1 << 7 | 1 << 11
FPU_TAG_ALL_INVALID = 0xAAAA
FPU_STATUS_MASK = 0xFFFF & ~(1 << 9 | 1 << 5 | 1 << 3 | 1 << 1)
FP_COMPARISON_SIGNIFICANT_DIGITS = 7

# run.js:44-47 and run.js:386-391 -- how gdb's unquoted infinity/NaN tokens are
# represented in the fixture text and translated back after parsing.
JSON_POS_INFINITY = "+INFINITY"
JSON_NEG_INFINITY = "-INFINITY"
JSON_POS_NAN = "+NAN"
JSON_NEG_NAN = "-NAN"
FLOAT_TRANSLATION = {
    JSON_POS_INFINITY: float("inf"),
    JSON_NEG_INFINITY: float("-inf"),
    JSON_POS_NAN: float("nan"),
    JSON_NEG_NAN: float("nan"),
}

JSON_RE = re.compile(r"---BEGIN JSON---([\s\[\]\.\+\w\":\-,]*)---END JSON---")


class FixtureError(Exception):
    pass


def format_value(v):
    """run.js:68-85."""
    if isinstance(v, (int, float)) and not isinstance(v, bool):
        if isinstance(v, int) or float(v).is_integer():
            return "0x%x" % (int(v) & 0xFFFFFFFF)
        return str(v)
    return str(v)


def extract_json(name, text):
    """run.js:89-135 -- signal marker -> x86 exception, then the JSON array."""
    exception = None
    if "(signal SIGFPE)" in text:
        exception = "DE"
    if "(signal SIGILL)" in text:
        exception = "UD"
    if "(signal SIGSEGV)" in text:
        exception = "GP"
    if "(signal SIGBUS)" in text:
        exception = "PF"
    if exception is None and "Program received signal" in text:
        raise FixtureError("killed during execution (no known signal marker): %s" % name)

    text = re.sub(r"-inf\b", json.dumps(JSON_NEG_INFINITY), text)
    text = re.sub(r"\binf\b", json.dumps(JSON_POS_INFINITY), text)
    text = re.sub(r"-nan\b", json.dumps(JSON_NEG_NAN), text)
    text = re.sub(r"\bnan\b", json.dumps(JSON_POS_NAN), text)

    m = JSON_RE.search(text)
    if not m:
        raise FixtureError("no JSON in fixture text: %s" % name)
    try:
        array = json.loads(m.group(1))
    except json.JSONDecodeError as e:
        raise FixtureError("unparseable JSON in %s: %s" % (name, e))
    return {"array": array, "exception": exception}


def float_equal(x, y):
    """run.js:54-66."""
    if x == float("inf") and y == float("inf"):
        return True
    if x == float("-inf") and y == float("-inf"):
        return True
    if x != x and y != y:  # NaN == NaN, sign ignored (run.js:390)
        return True
    epsilon = 10.0 ** -FP_COMPARISON_SIGNIFICANT_DIGITS
    return abs(x - y) < epsilon


def compare(name, expected_text, actual_text):
    """run.js:362-511 -- `expected` is the native fixture, `actual` the port."""
    expected = extract_json(name, expected_text)
    actual = extract_json(name, actual_text)
    array = expected["array"]
    failures = []

    def fail(fname, exp, act):
        failures.append({
            "name": fname,
            "expected": format_value(exp),
            "actual": format_value(act),
        })

    offset = 0
    expected_reg32 = array[offset:offset + 8]
    offset += 8
    expected_eip = array[offset]
    offset += 1
    expected_fpu_regs = [FLOAT_TRANSLATION.get(x, x) for x in array[offset:offset + 8]]
    offset += 8
    expected_mmx_registers = array[offset:offset + 16]
    offset += 16
    expected_xmm_registers = array[offset:offset + 32]
    offset += 32
    expected_memory = array[offset:offset + 8192 // 4]
    offset += 8192 // 4
    expected_eflags = array[offset] & MASK_ARITH
    offset += 1
    fpu_tag = array[offset]
    offset += 1
    fpu_status = array[offset] & FPU_STATUS_MASK
    offset += 1

    if offset != len(array):
        raise FixtureError("bad fixture length in %s: %d vs %d" % (name, offset, len(array)))

    BSS = 0x100000
    evaluated_reg32, evaluated_eip, evaluated_fpu_regs, evaluated_mmxs, evaluated_xmms, \
        evaluated_memory, evaluated_eflags, evaluated_fpu_status = decode_actual(name, actual)

    if not expected["exception"]:
        for i in range(8):
            if evaluated_reg32[i] != expected_reg32[i]:
                fail("cpu.reg32[%d]" % i, expected_reg32[i], evaluated_reg32[i])

        # G0 REPAIR (unit R4F).  Upstream's test is `fpu_tag !== FPU_TAG_ALL_INVALID`
        # (run.js:423) and it is UNREACHABLE as written: the fixture's tag word is
        # printed SIGNED (`printf("    %d,\n", (int)ftag)`, tests/mkfixture.c:189 --
        # verbatim from upstream's own gdb-extract-def:138) and the value it prints is
        # 0xFFFF____ , where the low 16 bits are the real abridged tag word and the high
        # half is the kernel's convert_from_fxsr `| 0xffff0000` (mkfixture.c:138's
        # measured quirk).  For an all-empty tag word that is -21846 (0xFFFFAAAA), which
        # is never equal to the positive 16-bit constant 0xAAAA -- so the mm branch was
        # DEAD, every test took the st branch, and its own `!= "invalid"` guard then
        # skipped all eight registers (the fixture reports them all "invalid").  MEASURED
        # consequence: an MMX-register mutation passed the whole corpus
        # (results/m1b2-mmx-sse.md finding 2).
        #
        # The FIXTURE side is right -- the low half IS the tag word the reference is
        # given, and the high half is a documented property of the kernel's struct
        # conversion, measured on this host and produced upstream by gdb's own $ftag --
        # so the COMPARISON is what misreads the field: it compares a 32-bit signed value
        # against a 16-bit code.  Masking to the 16 bits the constant is defined over
        # (`FPU_TAG_ALL_INVALID` = eight 2-bit "invalid" slots) restores upstream's
        # intent: the mm branch is taken exactly when all eight x87 slots are empty.
        if (fpu_tag & 0xFFFF) != FPU_TAG_ALL_INVALID:
            for i in range(8):
                if expected_fpu_regs[i] != "invalid" and \
                        not float_equal(evaluated_fpu_regs[i], expected_fpu_regs[i]):
                    fail("st%d" % i, expected_fpu_regs[i], evaluated_fpu_regs[i])
            if fpu_status != evaluated_fpu_status:
                fail("fpu status word", fpu_status, evaluated_fpu_status)
        else:
            for i in range(16):
                if evaluated_mmxs[i] != expected_mmx_registers[i]:
                    fail("mm%d.int32[%d]" % (i >> 1, i & 1),
                         expected_mmx_registers[i], evaluated_mmxs[i])

        for i in range(32):
            if evaluated_xmms[i] != expected_xmm_registers[i]:
                fail("xmm%d.int32[%d] (cpu.reg_xmm[%d])" % (i >> 2, i & 3, i),
                     expected_xmm_registers[i], evaluated_xmms[i])

        for i in range(2048):
            if evaluated_memory[i] != expected_memory[i]:
                fail("mem[%X]" % (BSS + 4 * i), expected_memory[i], evaluated_memory[i])

        if evaluated_eflags != expected_eflags:
            fail("eflags", expected_eflags, evaluated_eflags)

    if expected["exception"]:
        seen_eip = evaluated_eip
        if seen_eip != expected_eip:
            fail("exception eip", expected_eip, seen_eip)

    if expected["exception"] != actual["exception"]:
        fail("Exception", expected["exception"], actual["exception"])

    return failures


def decode_actual(name, actual):
    """A fixture-shaped document from the port/reference -> run.js's evaluated state.

    run.js reads the state straight out of the emulator (run.js:375-380); a port
    cannot be linked into the runner, so the actual side is required to emit the
    same document the native harness does and it is decoded here. The decoding is
    the inverse of the layout run.js:393-403 slices apart.
    """
    a = actual["array"]
    if len(a) != 2116:
        raise FixtureError("bad actual fixture length in %s: %d" % (name, len(a)))
    reg32 = a[0:8]
    eip = a[8]
    fpu_regs = a[9:17]
    mmxs = a[17:33]
    xmms = a[33:65]
    memory = a[65:65 + 2048]
    eflags = a[65 + 2048] & MASK_ARITH
    fpu_status = a[65 + 2048 + 2] & FPU_STATUS_MASK
    # An "invalid" entry cannot be compared; run.js only skips it on the expected
    # side, so map it to NaN (float_equal(NaN, NaN) is true by run.js:59).
    fpu_regs = [x if isinstance(x, (int, float)) else float("nan") for x in fpu_regs]
    return reg32, eip, fpu_regs, mmxs, xmms, memory, eflags, fpu_status


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def corpus():
    return sorted(p.stem for p in CORPUS_DIR.glob("*.asm"))


def load_skip_list(path):
    """One `NAME<2+ spaces>reason` per line; a line starting with '#' is a comment
    ('#' inside a reason is literal -- the reasons name #DE/#UD/#GP)."""
    skipped = {}
    if not path.exists():
        return skipped
    for line in path.read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        name, _, reason = line.partition("  ")
        skipped[name.strip()] = reason.strip() or "(no reason given)"
    return skipped


def loop_address(img):
    out = run(["nm", str(img)])
    if out.returncode != 0:
        raise FixtureError("nm failed on %s: %s" % (img, out.stderr.strip()))
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == "loop":
            return int(parts[0], 16)
    raise FixtureError("no `loop` symbol in %s" % img)


def translate(name, workdir):
    """nasm2gas.py -> as --32 -> ld. The translated source is kept for inspection."""
    workdir.mkdir(parents=True, exist_ok=True)   # build/ is shared with other units (make clean)
    src = CORPUS_DIR / ("%s.asm" % name)
    s = workdir / ("%s.s" % name)
    o = workdir / ("%s.o" % name)
    img = workdir / ("%s.img" % name)
    with open(s, "w") as fh:
        rc = subprocess.run([sys.executable, str(TRANSLATOR), str(src)],
                            stdout=fh, stderr=subprocess.PIPE, text=True)
    if rc.returncode != 0:
        raise FixtureError("translate failed: %s" % rc.stderr.strip())
    # -mx86-used-note=no: gas would otherwise emit a .note.gnu.property section,
    # which ld places in its own page-aligned R-only PT_LOAD past the 8192-byte
    # window for 4 tests (psllw/psrlw/psllq/psrlq) and pushes .multiboot out with
    # it. nasm never emits that note, so this also brings the image closer to
    # upstream's product.
    rc = run(["as", "--32", "-mx86-used-note=no", "-o", str(o), str(s)])
    if rc.returncode != 0:
        raise FixtureError("assemble failed: %s" % rc.stderr.strip())
    rc = run(["ld"] + LD_GEOMETRY + ["-o", str(img), str(o)])
    if rc.returncode != 0:
        raise FixtureError("link failed: %s" % rc.stderr.strip())
    return img


def make_fixture(name, workdir, harness, fixture_dir):
    fixture_dir.mkdir(parents=True, exist_ok=True)
    img = workdir / ("%s.img" % name)
    if not img.exists():
        raise FixtureError("no image for %s; run the translate stage first" % name)
    out = run([str(harness), str(img), hex(loop_address(img))])
    if out.returncode != 0:
        raise FixtureError("mkfixture failed on %s: %s" % (name, out.stderr.strip()))
    path = fixture_dir / ("%s.fixture" % name)
    path.write_text(out.stdout)
    return path, out.stdout


def actual_state(name, img, actual, limit):
    if actual == "js":
        out = run(["node", str(ORACLE), str(img), str(limit)])
        if out.returncode not in (0, 3):
            raise FixtureError("reference leg failed on %s: %s" % (name, out.stderr.strip()))
        return out.stdout, out.returncode == 3
    cmd = [a.replace("{img}", str(img)).replace("{limit}", str(limit))
           for a in actual.split()]
    out = run(cmd)
    if out.returncode != 0:
        raise FixtureError("actual leg failed on %s: %s" % (name, out.stderr.strip()))
    return out.stdout, False


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stage", choices=("translate", "fixtures", "run", "all"), default="run")
    ap.add_argument("--actual", default="js",
                    help="'js' for the reference (default) or a command with "
                         "{img}/{limit}, e.g. 'build/v86c-nasm {img} {limit}'")
    ap.add_argument("--filter", default=None, help="regex over test names")
    ap.add_argument("--skip-list", default=str(SKIP_LIST))
    ap.add_argument("--workdir", default=str(WORKDIR))
    ap.add_argument("--fixture-dir", default=None,
                    help="generate/read native fixtures here (default: workdir)")
    ap.add_argument("--reuse-fixtures", action="store_true",
                    help="read the native fixture from --fixture-dir instead of regenerating "
                         "it; the negative control uses this to feed a perturbed fixture")
    ap.add_argument("--limit", type=int, default=1000000, help="instruction limit for the actual leg")
    ap.add_argument("--quiet", action="store_true", help="only print failures and the summary")
    args = ap.parse_args()

    workdir = Path(args.workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    fixture_dir = Path(args.fixture_dir) if args.fixture_dir else workdir
    fixture_dir.mkdir(parents=True, exist_ok=True)
    harness = ROOT / "build" / "mkfixture"

    skipped = load_skip_list(Path(args.skip_list))
    names = corpus()
    if args.filter:
        rx = re.compile(args.filter)
        names = [n for n in names if rx.search(n)]
    if not names:
        print("no tests selected", file=sys.stderr)
        return 2

    if args.stage in ("fixtures", "run", "all") and not harness.exists():
        print("error: %s not built (make nasm should build it)" % harness, file=sys.stderr)
        return 2

    translated, fixtured = 0, 0
    failed_to_build, results, skipped_names = [], [], []
    for name in names:
        if name in skipped and args.stage in ("fixtures", "run", "all"):
            skipped_names.append((name, skipped[name]))
        try:
            if args.stage in ("translate", "all"):
                translate(name, workdir)
                translated += 1
            if args.stage in ("fixtures", "run", "all"):
                if args.stage in ("run", "all") and not (workdir / ("%s.img" % name)).exists():
                    translate(name, workdir)
                    translated += 1
                if args.stage == "run" and args.reuse_fixtures:
                    fixture_path = fixture_dir / ("%s.fixture" % name)
                    if not fixture_path.exists():
                        raise FixtureError("--reuse-fixtures: no %s" % fixture_path)
                    fixture_text = fixture_path.read_text()
                else:
                    fixture_path, fixture_text = make_fixture(name, workdir, harness, fixture_dir)
                    fixtured += 1
            if args.stage in ("run", "all"):
                if name in skipped:
                    continue
                actual_text, no_halt = actual_state(
                    name, workdir / ("%s.img" % name), args.actual, args.limit)
                if not args.quiet:
                    print("Testing %s.img" % name)
                failures = compare(name, fixture_text, actual_text)
                if no_halt:
                    failures.append({"name": "guest did not halt within %d instructions" % args.limit,
                                     "expected": "hlt", "actual": "still running"})
                results.append((name, failures))
        except FixtureError as e:
            failed_to_build.append((name, str(e)))

    if args.stage == "translate":
        print("\n[+] Translated + assembled + linked %d/%d file(s)" % (translated, len(names)))
    if args.stage in ("fixtures", "all"):
        print("\n[+] Generated %d/%d native fixture(s)" % (fixtured, len(names)))
        signals = {}
        for name in names:
            p = fixture_dir / ("%s.fixture" % name)
            if p.exists():
                text = p.read_text()
                for sig in ("SIGFPE", "SIGILL", "SIGSEGV", "SIGBUS"):
                    if "(signal %s)" % sig in text:
                        signals[sig] = signals.get(sig, 0) + 1
                        break
                else:
                    signals["breakpoint"] = signals.get("breakpoint", 0) + 1
        print("[+] Fixture mix: " + ", ".join("%s=%d" % kv for kv in sorted(signals.items())))

    if failed_to_build:
        print("\n[!] %d test(s) could not run against the actual leg:" % len(failed_to_build))
        for name, err in failed_to_build:
            print("\n[-] %s:" % name)
            print("\t" + err)

    if args.stage in ("run", "all"):
        failed = [(n, f) for n, f in results if f]
        print("\n[+] Passed %d/%d tests." % (len(results) - len(failed), len(results)))
        print("[!] %d of %d selected test(s) produced no state from the actual leg, "
              "%d were skipped (%s)."
              % (len(failed_to_build), len(names), len(skipped_names), args.skip_list))
        if skipped_names:
            print("[~] Skipped %d test(s) (%s):" % (len(skipped_names), args.skip_list))
            for name, reason in skipped_names:
                print("\t%s: %s" % (name, reason))
        if failed:
            print("[-] Failed %d test(s)." % len(failed))
            for name, failures in failed:
                print("\n[-] %s.img:" % name)
                for f in failures:
                    print("\n\t" + f["name"])
                    print("\tActual:   " + f["actual"])
                    print("\tExpected: " + f["expected"])
        if failed or failed_to_build:
            return 1
    elif failed_to_build:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
