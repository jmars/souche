#!/usr/bin/env python3
"""results/r4f-cites-fix.py -- N1: the citation corrections, re-derived from ./v86.

R4's N1 found that src/cpu/instructions_0f.c's `instructions_0f.rs:N-M` citations are
systematically stale -- cpuid was cited at 1064-1228 where `instr_0FA2` is at 3256-3419, so a
reader following a citation lands on unrelated code.  73 of them were wrong; this script
holds the correction as DATA (which upstream functions each citation heads) and DERIVES the
line range from the pinned clone at ./v86 (first `pub unsafe fn` line .. the function's
closing brace).  No line number is typed in: re-derive any of them by hand and you get the
same numbers.

  python3 results/r4f-cites-fix.py --list                    # line, cited, derived, subjects
  python3 results/r4f-cites-fix.py --apply [path]            # default src/cpu/instructions_0f.c
  python3 results/r4f-cites-fix.py --verify-prefix <file>    # pre-fix file -> byte-diff

--verify-prefix is the asymmetry: it applies the table to the PRE-FIX file
(results/r4f-instructions_0f-prefix.c.gz, the file R4 reviewed) in a scratch copy and
requires the result to be byte-identical to the corrected src/cpu/instructions_0f.c.

Line numbers are stable across the correction: every edit replaces a number range ON A
COMMENT LINE and touches no other line.
"""

import gzip
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUST = ROOT / "v86" / "src" / "rust" / "cpu" / "instructions_0f.rs"
# the tail covers the pre-fix `2693?-...` uncertainty marker too
CITE_RE = re.compile(r"(instructions_0f\.rs:)(\d+)(?:-(\d+))?(?:\?)?(?:-?\.\.\.)?")
FN_RE = re.compile(r"pub unsafe fn (\w+)")

# line (in the pre-fix file) -> the upstream functions that citation heads.
# 73 entries: 57 whose cited start was more than 20 lines from the code it heads (the whole
# of the pre-1900 block plus 16 one-opcode-slip cases in the MMX/SSE half), 10 whose START
# was fine but whose END pointed past its subject, and 7 that results/r4f-cites.py caught as
# starting inside the FOLLOWING instruction (incl. the `2693?-...` uncertainty marker,
# resolved: instr_F30F6F is at 2685).
SUBJECTS = {
    287: ["instr16_0F01_7_reg", "instr32_0F01_7_reg", "instr16_0F01_7_mem", "instr32_0F01_7_mem"],
    320: ["instr_0F06", "instr_0F07"],
    332: ["instr_0F08"],
    335: ["instr_0F09", "instr_0F0A"],
    347: ["instr_0F0B", "instr_0F0C"],
    352: ["instr_0F0D", "instr_0F0E", "instr_0F0F"],
    431: ["instr_0F20"],
    458: ["instr_0F21"],
    479: ["instr_0F22"],
    525: ["instr_0F23", "instr_0F24", "instr_0F25", "instr_0F26", "instr_0F27"],
    560: ["instr_0F33"],
    564: ["instr_0F37"],
    577: ["instr16_0F40_mem", "instr16_0F40_reg", "instr32_0F40_mem", "instr32_0F40_reg", "instr16_0F41_mem", "instr16_0F41_reg", "instr32_0F41_mem", "instr32_0F41_reg", "instr16_0F42_mem", "instr16_0F42_reg", "instr32_0F42_mem", "instr32_0F42_reg", "instr16_0F43_mem", "instr16_0F43_reg", "instr32_0F43_mem", "instr32_0F43_reg", "instr16_0F44_mem", "instr16_0F44_reg", "instr32_0F44_mem", "instr32_0F44_reg", "instr16_0F45_mem", "instr16_0F45_reg", "instr32_0F45_mem", "instr32_0F45_reg", "instr16_0F46_mem", "instr16_0F46_reg", "instr32_0F46_mem", "instr32_0F46_reg", "instr16_0F47_mem", "instr16_0F47_reg", "instr32_0F47_mem", "instr32_0F47_reg", "instr16_0F48_mem", "instr16_0F48_reg", "instr32_0F48_mem", "instr32_0F48_reg", "instr16_0F49_mem", "instr16_0F49_reg", "instr32_0F49_mem", "instr32_0F49_reg", "instr16_0F4A_mem", "instr16_0F4A_reg", "instr32_0F4A_mem", "instr32_0F4A_reg", "instr16_0F4B_mem", "instr16_0F4B_reg", "instr32_0F4B_mem", "instr32_0F4B_reg", "instr16_0F4C_mem", "instr16_0F4C_reg", "instr32_0F4C_mem", "instr32_0F4C_reg", "instr16_0F4D_mem", "instr16_0F4D_reg", "instr32_0F4D_mem", "instr32_0F4D_reg", "instr16_0F4E_mem", "instr16_0F4E_reg", "instr32_0F4E_mem", "instr32_0F4E_reg", "instr16_0F4F_mem", "instr16_0F4F_reg", "instr32_0F4F_mem", "instr32_0F4F_reg"],
    585: ["instr16_0F40_mem", "instr16_0F40_reg", "instr32_0F40_mem", "instr32_0F40_reg", "instr16_0F41_mem", "instr16_0F41_reg", "instr32_0F41_mem", "instr32_0F41_reg", "instr16_0F42_mem", "instr16_0F42_reg", "instr32_0F42_mem", "instr32_0F42_reg", "instr16_0F43_mem", "instr16_0F43_reg", "instr32_0F43_mem", "instr32_0F43_reg", "instr16_0F44_mem", "instr16_0F44_reg", "instr32_0F44_mem", "instr32_0F44_reg", "instr16_0F45_mem", "instr16_0F45_reg", "instr32_0F45_mem", "instr32_0F45_reg", "instr16_0F46_mem", "instr16_0F46_reg", "instr32_0F46_mem", "instr32_0F46_reg", "instr16_0F47_mem", "instr16_0F47_reg", "instr32_0F47_mem", "instr32_0F47_reg", "instr16_0F48_mem", "instr16_0F48_reg", "instr32_0F48_mem", "instr32_0F48_reg", "instr16_0F49_mem", "instr16_0F49_reg", "instr32_0F49_mem", "instr32_0F49_reg", "instr16_0F4A_mem", "instr16_0F4A_reg", "instr32_0F4A_mem", "instr32_0F4A_reg", "instr16_0F4B_mem", "instr16_0F4B_reg", "instr32_0F4B_mem", "instr32_0F4B_reg", "instr16_0F4C_mem", "instr16_0F4C_reg", "instr32_0F4C_mem", "instr32_0F4C_reg", "instr16_0F4D_mem", "instr16_0F4D_reg", "instr32_0F4D_mem", "instr32_0F4D_reg", "instr16_0F4E_mem", "instr16_0F4E_reg", "instr32_0F4E_mem", "instr32_0F4E_reg", "instr16_0F4F_mem", "instr16_0F4F_reg", "instr32_0F4F_mem", "instr32_0F4F_reg"],
    850: ["instr_0F78", "instr_0F79", "instr_0F7A", "instr_0F7B", "instr_0F7C", "instr_0F7D"],
    860: ["instr_660F7C", "instr_660F7C_reg", "instr_660F7C_mem", "instr_F20F7C", "instr_F20F7C_reg", "instr_F20F7C_mem", "instr_660F7D", "instr_660F7D_reg", "instr_660F7D_mem", "instr_F20F7D", "instr_F20F7D_reg", "instr_F20F7D_mem"],
    946: ["instr16_0F80", "instr32_0F80", "instr16_0F81", "instr32_0F81", "instr16_0F82", "instr32_0F82", "instr16_0F83", "instr32_0F83", "instr16_0F84", "instr32_0F84", "instr16_0F85", "instr32_0F85", "instr16_0F86", "instr32_0F86", "instr16_0F87", "instr32_0F87", "instr16_0F88", "instr32_0F88", "instr16_0F89", "instr32_0F89", "instr16_0F8A", "instr32_0F8A", "instr16_0F8B", "instr32_0F8B", "instr16_0F8C", "instr32_0F8C", "instr16_0F8D", "instr32_0F8D", "instr16_0F8E", "instr32_0F8E", "instr16_0F8F", "instr32_0F8F"],
    1003: ["instr_0F90_reg", "instr_0F90_mem", "instr_0F91_reg", "instr_0F91_mem", "instr_0F92_reg", "instr_0F92_mem", "instr_0F93_reg", "instr_0F93_mem", "instr_0F94_reg", "instr_0F94_mem", "instr_0F95_reg", "instr_0F95_mem", "instr_0F96_reg", "instr_0F96_mem", "instr_0F97_reg", "instr_0F97_mem", "instr_0F98_reg", "instr_0F98_mem", "instr_0F99_reg", "instr_0F99_mem", "instr_0F9A_reg", "instr_0F9A_mem", "instr_0F9B_reg", "instr_0F9B_mem", "instr_0F9C_reg", "instr_0F9C_mem", "instr_0F9D_reg", "instr_0F9D_mem", "instr_0F9E_reg", "instr_0F9E_mem", "instr_0F9F_reg", "instr_0F9F_mem"],
    1063: ["instr16_0FA0", "instr32_0FA0", "instr16_0FA1", "instr32_0FA1"],
    1107: ["instr_0FA2"],
    1260: ["instr16_0FA3_reg", "instr16_0FA3_mem", "instr32_0FA3_reg", "instr32_0FA3_mem"],
    1266: ["instr16_0FA4_mem", "instr16_0FA4_reg", "instr32_0FA4_mem", "instr32_0FA4_reg", "instr16_0FA5_mem", "instr16_0FA5_reg", "instr32_0FA5_mem", "instr32_0FA5_reg"],
    1302: ["instr_0FA6", "instr_0FA7"],
    1306: ["instr16_0FA8", "instr32_0FA8", "instr16_0FA9", "instr32_0FA9"],
    1342: ["instr_0FAA"],
    1345: ["instr16_0FAB_reg", "instr16_0FAB_mem", "instr32_0FAB_reg", "instr32_0FAB_mem"],
    1357: ["instr16_0FAC_mem", "instr16_0FAC_reg", "instr32_0FAC_mem", "instr32_0FAC_reg", "instr16_0FAD_mem", "instr16_0FAD_reg", "instr32_0FAD_mem", "instr32_0FAD_reg"],
    1392: ["instr_0FAE_0_reg", "instr_0FAE_0_mem", "instr_0FAE_1_reg", "instr_0FAE_1_mem", "instr_0FAE_2_reg", "instr_0FAE_2_mem", "instr_0FAE_3_reg", "instr_0FAE_3_mem", "instr_0FAE_4_reg", "instr_0FAE_4_mem", "instr_0FAE_5_reg", "instr_0FAE_5_mem", "instr_0FAE_6_reg", "instr_0FAE_6_mem", "instr_0FAE_7_reg", "instr_0FAE_7_mem"],
    1412: ["instr_0FAE_2_reg", "instr_0FAE_2_mem"],
    1432: ["instr_0FAE_3_reg", "instr_0FAE_3_mem", "instr_0FAE_4_reg", "instr_0FAE_4_mem", "instr_0FAE_5_reg", "instr_0FAE_5_mem", "instr_0FAE_6_reg", "instr_0FAE_6_mem", "instr_0FAE_7_reg", "instr_0FAE_7_mem"],
    1490: ["instr16_0FAF_mem", "instr16_0FAF_reg", "instr32_0FAF_mem", "instr32_0FAF_reg"],
    1510: ["instr_0FB0_reg", "instr_0FB0_mem", "instr16_0FB1_reg", "instr16_0FB1_mem", "instr32_0FB1_reg", "instr32_0FB1_mem"],
    1519: ["instr16_0FB2_reg", "instr16_0FB2_mem", "instr32_0FB2_reg", "instr32_0FB2_mem"],
    1535: ["instr16_0FB3_reg", "instr16_0FB3_mem", "instr32_0FB3_reg", "instr32_0FB3_mem"],
    1621: ["instr_0FB9"],
    1625: ["instr16_0FBA_4_reg", "instr16_0FBA_4_mem", "instr16_0FBA_5_reg", "instr16_0FBA_5_mem", "instr16_0FBA_6_reg", "instr16_0FBA_6_mem", "instr16_0FBA_7_reg", "instr16_0FBA_7_mem", "instr32_0FBA_4_reg", "instr32_0FBA_4_mem", "instr32_0FBA_5_reg", "instr32_0FBA_5_mem", "instr32_0FBA_6_reg", "instr32_0FBA_6_mem", "instr32_0FBA_7_reg", "instr32_0FBA_7_mem"],
    1648: ["instr16_0FBB_reg", "instr16_0FBB_mem", "instr32_0FBB_reg", "instr32_0FBB_mem"],
    1660: ["instr16_0FBC_mem", "instr16_0FBC_reg", "instr32_0FBC_mem", "instr32_0FBC_reg", "instr16_0FBD_mem", "instr16_0FBD_reg", "instr32_0FBD_mem", "instr32_0FBD_reg"],
    1691: ["instr_0FC0_mem", "instr_0FC0_reg", "instr16_0FC1_mem", "instr16_0FC1_reg", "instr32_0FC1_mem", "instr32_0FC1_reg"],
    1701: ["instr16_0FC7_1_reg", "instr32_0FC7_1_reg", "instr16_0FC7_1_mem", "instr32_0FC7_1_mem", "instr16_0FC7_6_mem", "instr32_0FC7_6_mem"],
    1754: ["instr_0FC8", "instr_0FC9", "instr_0FCA", "instr_0FCB", "instr_0FCC", "instr_0FCD", "instr_0FCE", "instr_0FCF"],
    1765: ["instr_0FFF"],
    2006: ["instr_660F12_reg", "instr_660F12_mem"],
    2036: ["instr_F30F12", "instr_F30F12_reg", "instr_F30F12_mem"],
    3306: ["instr_0F61", "instr_0F61_reg", "instr_0F61_mem", "instr_660F61", "instr_660F61_reg", "instr_660F61_mem"],
    3346: ["instr_0F62", "instr_0F62_reg", "instr_0F62_mem", "instr_660F62", "instr_660F62_reg", "instr_660F62_mem"],
    3377: ["instr_0F63", "instr_0F63_reg", "instr_0F63_mem", "instr_660F63", "instr_660F63_reg", "instr_660F63_mem"],
    3417: ["instr_0F64", "instr_0F64_reg", "instr_0F64_mem", "instr_660F64", "instr_660F64_reg", "instr_660F64_mem"],
    3455: ["instr_0F65", "instr_0F65_reg", "instr_0F65_mem", "instr_660F65", "instr_660F65_reg", "instr_660F65_mem"],
    3493: ["instr_0F66", "instr_0F66_reg", "instr_0F66_mem", "instr_660F66", "instr_660F66_reg", "instr_660F66_mem"],
    3530: ["instr_0F67", "instr_0F67_reg", "instr_0F67_mem", "instr_660F67", "instr_660F67_reg", "instr_660F67_mem"],
    3570: ["instr_0F68", "instr_0F68_reg", "instr_0F68_mem", "instr_660F68", "instr_660F68_reg", "instr_660F68_mem"],
    3610: ["instr_0F69", "instr_0F69_reg", "instr_0F69_mem", "instr_660F69", "instr_660F69_reg", "instr_660F69_mem"],
    3649: ["instr_0F6A", "instr_0F6A_reg", "instr_0F6A_mem", "instr_660F6A", "instr_660F6A_reg", "instr_660F6A_mem"],
    3680: ["instr_0F6B", "instr_0F6B_reg", "instr_0F6B_mem", "instr_660F6B", "instr_660F6B_reg", "instr_660F6B_mem"],
    3820: ["instr_660F6F", "instr_660F6F_reg", "instr_660F6F_mem"],
    3829: ["instr_F30F6F", "instr_F30F6F_reg", "instr_F30F6F_mem"],
    4124: ["instr_0F74", "instr_0F74_reg", "instr_0F74_mem", "instr_660F74", "instr_660F74_reg", "instr_660F74_mem"],
    4160: ["instr_0F75", "instr_0F75_reg", "instr_0F75_mem", "instr_660F75", "instr_660F75_reg", "instr_660F75_mem"],
    4197: ["instr_0F76", "instr_0F76_reg", "instr_0F76_mem", "instr_660F76", "instr_660F76_reg", "instr_660F76_mem"],
    4260: ["instr_660F7E_reg", "instr_660F7E_mem"],
    4272: ["instr_F30F7E_mem", "instr_F30F7E_reg"],
    4501: ["instr_0FD0", "instr_0FD1", "instr_0FD1_reg", "instr_0FD1_mem", "instr_660FD1", "instr_660FD1_reg", "instr_660FD1_mem", "instr_0FD2", "instr_0FD2_reg", "instr_0FD2_mem", "instr_660FD2", "instr_660FD2_reg", "instr_660FD2_mem", "instr_0FD3", "instr_0FD3_reg", "instr_0FD3_mem", "instr_660FD3", "instr_660FD3_reg", "instr_660FD3_mem", "instr_0FD4", "instr_0FD4_reg", "instr_0FD4_mem", "instr_660FD4", "instr_660FD4_reg", "instr_660FD4_mem", "instr_0FD5", "instr_0FD5_reg", "instr_0FD5_mem", "instr_660FD5", "instr_660FD5_reg", "instr_660FD5_mem", "instr_0FD6_mem", "instr_0FD6_reg", "instr_660FD6_mem", "instr_660FD6_reg", "instr_F20FD6_mem", "instr_F20FD6_reg", "instr_F30FD6_mem", "instr_F30FD6_reg", "instr_0FD7_mem", "instr_0FD7_reg", "instr_660FD7_mem", "instr_660FD7_reg", "instr_0FD8", "instr_0FD8_reg", "instr_0FD8_mem", "instr_660FD8", "instr_660FD8_reg", "instr_660FD8_mem", "instr_0FD9", "instr_0FD9_reg", "instr_0FD9_mem", "instr_660FD9", "instr_660FD9_reg", "instr_660FD9_mem", "instr_0FDA", "instr_0FDA_reg", "instr_0FDA_mem", "instr_660FDA", "instr_660FDA_reg", "instr_660FDA_mem", "instr_0FDB", "instr_0FDB_reg", "instr_0FDB_mem", "instr_660FDB", "instr_660FDB_reg", "instr_660FDB_mem", "instr_0FDC", "instr_0FDC_reg", "instr_0FDC_mem", "instr_660FDC", "instr_660FDC_reg", "instr_660FDC_mem", "instr_0FDD", "instr_0FDD_reg", "instr_0FDD_mem", "instr_660FDD", "instr_660FDD_reg", "instr_660FDD_mem", "instr_0FDE", "instr_0FDE_reg", "instr_0FDE_mem", "instr_660FDE", "instr_660FDE_reg", "instr_660FDE_mem", "instr_0FDF", "instr_0FDF_reg", "instr_0FDF_mem", "instr_660FDF", "instr_660FDF_reg", "instr_660FDF_mem", "instr_0FE0", "instr_0FE0_reg", "instr_0FE0_mem", "instr_660FE0", "instr_660FE0_reg", "instr_660FE0_mem", "instr_0FE1", "instr_0FE1_reg", "instr_0FE1_mem", "instr_660FE1", "instr_660FE1_reg", "instr_660FE1_mem", "instr_0FE2", "instr_0FE2_reg", "instr_0FE2_mem", "instr_660FE2", "instr_660FE2_reg", "instr_660FE2_mem", "instr_0FE3", "instr_0FE3_reg", "instr_0FE3_mem", "instr_660FE3", "instr_660FE3_reg", "instr_660FE3_mem", "instr_0FE4", "instr_0FE4_reg", "instr_0FE4_mem", "instr_660FE4", "instr_660FE4_reg", "instr_660FE4_mem", "instr_0FE5", "instr_0FE5_reg", "instr_0FE5_mem", "instr_660FE5", "instr_660FE5_reg", "instr_660FE5_mem", "instr_0FE6_mem", "instr_0FE6_reg", "instr_660FE6", "instr_660FE6_mem", "instr_660FE6_reg", "instr_F20FE6", "instr_F20FE6_mem", "instr_F20FE6_reg", "instr_F30FE6", "instr_F30FE6_mem", "instr_F30FE6_reg", "instr_0FE7_mem", "instr_0FE7_reg", "instr_660FE7_reg", "instr_660FE7_mem"],
    5537: ["instr_0FF0"],
    5558: ["instr_0FF2", "instr_0FF2_reg", "instr_0FF2_mem", "instr_660FF2", "instr_660FF2_reg", "instr_660FF2_mem"],
    5576: ["instr_0FF3", "instr_0FF3_reg", "instr_0FF3_mem", "instr_660FF3", "instr_660FF3_reg", "instr_660FF3_mem"],
    5594: ["instr_0FF4", "instr_0FF4_reg", "instr_0FF4_mem", "instr_660FF4", "instr_660FF4_reg", "instr_660FF4_mem"],
    5624: ["instr_0FF5", "instr_0FF5_reg", "instr_0FF5_mem", "instr_660FF5", "instr_660FF5_reg", "instr_660FF5_mem"],
    5670: ["instr_0FF6", "instr_0FF6_reg", "instr_0FF6_mem", "instr_660FF6", "instr_660FF6_reg", "instr_660FF6_mem"],
    5842: ["instr_0FFA", "instr_0FFA_reg", "instr_0FFA_mem", "instr_660FFA", "instr_660FFA_reg", "instr_660FFA_mem"],
    5878: ["instr_0FFB", "instr_0FFB_reg", "instr_0FFB_mem", "instr_660FFB", "instr_660FFB_reg", "instr_660FFB_mem"],
    5906: ["instr_0FFC", "instr_0FFC_reg", "instr_0FFC_mem", "instr_660FFC", "instr_660FFC_reg", "instr_660FFC_mem"],
    5942: ["instr_0FFD", "instr_0FFD_reg", "instr_0FFD_mem", "instr_660FFD", "instr_660FFD_reg", "instr_660FFD_mem"],
}


def load_spans():
    """name -> (first line, last body line) for every `pub unsafe fn` in the clone."""
    rust = RUST.read_text().splitlines()
    spans = {}
    for ln, line in enumerate(rust, 1):
        m = FN_RE.match(line)
        if not m:
            continue
        end = ln
        if not (line.rstrip().endswith("}") and line.count("{") == line.count("}")):
            for j in range(ln + 1, len(rust) + 1):
                if rust[j - 1] == "}":
                    end = j
                    break
        spans[m.group(1)] = (ln, end)
    return spans


def derived(spans, names):
    lo = min(spans[n][0] for n in names)
    hi = max(spans[n][1] for n in names)
    return (lo, hi) if lo != hi else (lo, lo)


def apply(path, spans, report):
    lines = path.read_text().splitlines(keepends=True)
    changed = skipped = 0
    for ln, names in sorted(SUBJECTS.items()):
        lo, hi = derived(spans, names)
        new = ("%d-%d" % (lo, hi)) if lo != hi else ("%d" % lo)
        text = lines[ln - 1]
        m = CITE_RE.search(text)
        if not m:
            print("FATAL: line %d no longer carries a citation: %s" % (ln, text.strip()))
            return None
        old = m.group(2) + ("-%s" % m.group(3) if m.group(3) else "")
        if old == new:
            skipped += 1
            continue
        lines[ln - 1] = CITE_RE.sub(lambda mm: mm.group(1) + new, text, count=1)
        changed += 1
        if report:
            print("%5d  %-11s -> %-11s  %s" % (ln, old, new, ", ".join(names[:3]) +
                                               (" +%d more" % (len(names) - 3) if len(names) > 3 else "")))
    return "".join(lines), changed, skipped


def main():
    spans = load_spans()
    if "--list" in sys.argv:
        for ln, names in sorted(SUBJECTS.items()):
            lo, hi = derived(spans, names)
            print("%5d  derived %-11s  %s" % (ln, ("%d-%d" % (lo, hi)) if lo != hi else lo,
                                              ", ".join(names[:4]) +
                                              (" +%d more" % (len(names) - 4) if len(names) > 4 else "")))
        return 0
    if "--verify-prefix" in sys.argv:
        pre = Path(sys.argv[sys.argv.index("--verify-prefix") + 1])
        if pre.suffix == ".gz":                      # the stored pre-fix artifact
            import tempfile
            tmp = Path(tempfile.mkdtemp()) / "instructions_0f.prefix.c"
            tmp.write_bytes(gzip.open(pre, "rb").read())
            pre = tmp
        res = apply(pre, spans, False)
        if res is None:
            return 1
        out_text, changed, skipped = res
        corrected = (ROOT / "src" / "cpu" / "instructions_0f.c").read_text()
        same = out_text == corrected
        print("applied to %s: %d citation(s) rewritten, %d already correct" % (pre, changed, skipped))
        print("result is %s the corrected src/cpu/instructions_0f.c"
              % ("BYTE-IDENTICAL to" if same else "DIFFERENT FROM"))
        return 0 if same else 1
    path = Path(sys.argv[sys.argv.index("--apply") + 1]) if "--apply" in sys.argv \
        else ROOT / "src" / "cpu" / "instructions_0f.c"
    res = apply(path, spans, True)
    if res is None:
        return 1
    out_text, changed, skipped = res
    path.write_text(out_text)
    print("wrote %s: %d citation(s) rewritten, %d already correct (of %d)"
          % (path, changed, skipped, len(SUBJECTS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
