#!/usr/bin/env python3
"""nasm2gas -- translate a v86/tests/nasm/*.asm test to GNU as (.intel_syntax noprefix).

There is no nasm on this host, so v86's own corpus is translated instead of
re-assembled. The NASM surface actually used by the 123 checked-in tests, measured
over the corpus, is:

    %include (inlined, relative to the including file)
    %macro/%endmacro (three single-argument macros, expanded in place)
    db/dw/dd/dq, including float literals, nasm hex-floats (0x80000001.0) and the
    named nasm float constants (__QNaN__, __SNaN__, __Infinity__, __-Infinity__)
    resb/resw/resd/resq, section, global, align, equ, trailing-h hex
    size-prefixed memory operands (dword [x]), tword -> tbyte
    bare no-operand fdiv/fdivr/fmul, st1..st7 operands, `push word <imm>`

Gas-intel traps handled here (each one was a measured failure):
  * `mov reg, symbol` is a MEMORY operand in gas, but NASM means its address:
    the corpus only writes it as `mov reg, label`, so it becomes `OFFSET label`.
  * a register name must not look like a trailing-h hex literal: `ah`/`bh`/`ch`/
    `dh` are excluded from the trailing-h rule by a lookahead.
  * `push word 0x..` does not exist in gas: it becomes `pushw 0x..`.
  * `tword` (NASM) is `tbyte` (gas).

usage: nasm2gas.py FILE            (to stdout)
       nasm2gas.py -o OUT.s FILE
"""

import argparse
import pathlib
import re
import sys

SIZES = {"resb": 1, "resw": 2, "resd": 4, "resq": 8}

# NASM named floating-point constants -> raw 32-bit patterns.
FLOAT_CONSTANTS = [
    ("__QNaN__", "0x7FC00000"), ("__NaN__", "0x7FC00000"),
    ("__SNaN__", "0x7FA00000"), ("__Infinity__", "0x7F800000"),
    ("__QInfinity__", "0x7F800000"), ("__-Infinity__", "0xFF800000"),
]

# register names that a naive trailing-h hex rule would corrupt
REGS_NO_HEX = r"(?!ah\b|bh\b|ch\b|dh\b)"
HEX_H = re.compile(REGS_NO_HEX + r"\b([0-9a-fA-F][0-9a-fA-F_]*)h\b")

# the macros the corpus expands by hand; anything else is refused loudly
MACROS = {
    "moveflags":
        ".macro moveflags reg\n pushf\n and dword ptr [esp], 0x45\n pop eax\n"
        " movd \\reg, eax\n.endm\n",
    "setRoundingBits":
        ".macro setRoundingBits bits\n stmxcsr dword ptr [mxcsr]\n mov eax, dword ptr [mxcsr]\n"
        " and ax, 0x9F80\n or ah, \\bits\n mov dword ptr [mxcsr], eax\n"
        " ldmxcsr dword ptr [mxcsr]\n.endm\n",
}

# every register name that may not be treated as a symbol in `mov reg, X`
REG_NAMES = re.compile(
    r"^(?:e?[abcd]x|esp|ebp|esi|edi|[abcd][hl]|[abcd]x|sp|bp|si|di|ss|cs|ds|es|fs|gs"
    r"|cr[0-8]|dr[0-8]|st[0-7]\(?:\d\)?|mm[0-7]|xmm[0-7])$", re.I)


def is_float_literal(token):
    token = token.strip()
    return bool(re.match(r"^[-+]?[0-9]", token)) and "." in token and \
        not re.match(r"^[-+]?0[xX]", token)


def convert(src, out_lines, emit_syntax=True):
    """Append the gas form of `src` to out_lines. `src` may be a path or text."""
    if emit_syntax:
        out_lines.append(".intel_syntax noprefix\n")
    text = src if isinstance(src, str) and "\n" in src else pathlib.Path(src).read_text()
    base = pathlib.Path(src).parent if not (isinstance(src, str) and "\n" in src) else None
    expanding = None

    for raw in text.splitlines():
        if ";" in raw:
            code, _, comment = raw.partition(";")
            comment = "  #" + comment if comment.strip() else ""
        else:
            code, comment = raw, ""
        line = code.rstrip()
        stripped = line.strip()

        if stripped.startswith("%include"):
            include = re.search(r'"([^"]+)"', stripped).group(1)
            convert(str(base / include), out_lines, emit_syntax=False)
            continue
        if expanding:
            if stripped.startswith("%endmacro"):
                expanding = None
            continue
        if stripped.startswith("%macro"):
            expanding = stripped.split()[1]
            if expanding not in MACROS:
                raise SystemExit("unknown macro: " + expanding)
            out_lines.append(MACROS[expanding])
            continue

        # resb/resw/resd/resq -> .space, keeping an optional label
        m = re.match(r"^(\s*)(?:([a-z_][\w.]*)\s+)?(resb|resd|resw|resq)\s+(.+)$", line, re.I)
        if m and not re.search(r"\[", m.group(1)):
            size = SIZES[m.group(3).lower()]
            line = m.group(1) + (m.group(2) + ": " if m.group(2) else "") + \
                ".space %s * %d" % (m.group(4), size)

        line = re.sub(r"^(\s*)section\s+", r"\1.section ", line, flags=re.I)
        line = re.sub(r"^(\s*)global\s+", r"\1.global ", line, flags=re.I)
        line = re.sub(r"^(\s*)align\s+(\d+)", r"\1.balign \2", line, flags=re.I)
        line = re.sub(r"^(\s*)([a-z_][\w.]*)\s+equ\s+(.+)$", r"\1.equ \2, \3", line, flags=re.I)
        for name, value in FLOAT_CONSTANTS:
            line = line.replace(name, value)

        # float literals: `dd 1000.0` is .float, `dq 1000.0` is .double
        mf = re.match(r"^(\s*)d[qd]\s+([^;#]+)$", line, re.I)
        if mf and is_float_literal(mf.group(2).split()[0]):
            directive = ".double " if mf.group(0).lstrip()[:2].lower() == "dq" else ".float "
            line = mf.group(1) + directive + mf.group(2)
        line = re.sub(r"^(\s*)dq\s+", r"\1.quad ", line, flags=re.I)
        line = re.sub(r"^(\s*)dd\s+", r"\1.long ", line, flags=re.I)
        line = re.sub(r"^(\s*)dw\s+", r"\1.word ", line, flags=re.I)
        line = re.sub(r"^(\s*)db\s+", r"\1.byte ", line, flags=re.I)

        m = re.match(r"^(\s*)push\s+word\s+(0x[0-9a-f]+|\d+)$", line, re.I)
        if m:
            line = "%spushw %s" % (m.group(1), m.group(2))

        # NASM treats a bare label as its address; gas intel syntax treats it as a
        # memory operand.  Only `mov` with a bare label appears in the corpus.
        m = re.match(r"^(\s*)mov\s+([er]?[abcd]x|esp|ebp|esi|edi),\s*([a-z_][\w.]*)$", line, re.I)
        if m and not re.search(r"\[", line) and not REG_NAMES.match(m.group(3)):
            line = "%smov %s, OFFSET %s" % (m.group(1), m.group(2), m.group(3))

        # nasm hex-float literal (0x80000001.0) -> raw bit pattern
        line = re.sub(r"\b(0x[0-9a-fA-F]+)\.0\b", r"\1", line)
        # bare no-operand FPU ops -> explicit st, st(1) form (nasm semantics)
        for op in ("fdiv", "fdivr", "fmul"):
            if re.match(r"^\s*%s\s*$" % op, line, re.I):
                line = "%s st, st(1)" % op
        # x87 register operands: st1 -> st(1)
        line = re.sub(r"\bst([0-7])\b", r"st(\1)", line)
        # hex with a trailing h (after the comment split above)
        line = HEX_H.sub(lambda m: "0x" + m.group(1), line)
        # size-qualified memory operands: nasm `dword [x]` -> gas `dword ptr [x]`
        line = re.sub(r"\b(byte|word|dword|qword|tword|oword)\s+\[", r"\1 ptr [", line, flags=re.I)
        line = line.replace("tword ptr", "tbyte ptr")

        if line or comment:
            out_lines.append(line + comment + "\n")
    return out_lines


def main():
    ap = argparse.ArgumentParser(description="NASM -> GNU as (intel syntax) for v86/tests/nasm")
    ap.add_argument("source", help="the .asm file to translate")
    ap.add_argument("-o", "--output", help="write here instead of stdout")
    args = ap.parse_args()

    lines = convert(args.source, [], emit_syntax=True)
    text = "".join(lines)
    if args.output:
        pathlib.Path(args.output).write_text(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
