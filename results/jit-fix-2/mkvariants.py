#!/usr/bin/env python3
"""JIT-FIX-2 variant generator: the four jcc array swaps that are INVISIBLE to the whole gate
before this unit's cases (positions 2<->6 and 3<->7 in the rel8 and rel32 arrays).

Both arrays are INDEXED BY THE CONDITION NIBBLE (jit_codegen.c's `e.aux = cc`), so a swap
substitutes one condition's recipe for another with no other trace: 2=jb<->6=jbe and
3=jae<->7=ja.  Each symbol occurs exactly ONCE in the file (asserted), so the swap is a
targeted textual replacement.

usage: mkvariants.py <jit_codegen.c.fixed> <outdir>
"""
import sys, re

src = open(sys.argv[1]).read()
out = sys.argv[2]

PAIRS = [
    ('jcc8-swap-bbe',  'instr32_72',  'instr32_76'),   # rel8  2=jb    <-> 6=jbe
    ('jcc8-swap-aea',  'instr32_73',  'instr32_77'),   # rel8  3=jae   <-> 7=ja
    ('jcc32-swap-bbe', 'instr32_0F82', 'instr32_0F86'),# rel32 2=jb    <-> 6=jbe
    ('jcc32-swap-aea', 'instr32_0F83', 'instr32_0F87'),# rel32 3=jae   <-> 7=ja
]

def swap(text, a, b, tag):
    for s in (a, b):
        n = len(re.findall(r'\b%s\b' % s, text))
        assert n == 1, "%s occurs %d times, not 1" % (s, n)
    # placeholders first, so the two replacements cannot cascade
    v = re.sub(r'\b%s\b' % a, '@@%s@@' % tag, text)
    v = re.sub(r'\b%s\b' % b, a, v)
    return v.replace('@@%s@@' % tag, b)

for name, a, b in PAIRS:
    v = swap(src, a, b, 'A')
    assert v != src
    open('%s/jit_codegen.c.%s' % (out, name), 'w').write(v)
    print("wrote jit_codegen.c.%s" % name)

# ALL FOUR swaps at once: the strongest form of the "invisible to the whole gate" claim -- if a
# leg sees ANY of them it will fail here, so this variant is the claim's hardest case.
allv = src
for i, (name, a, b) in enumerate(PAIRS):
    allv = swap(allv, a, b, 'ABCD'[i])
open('%s/jit_codegen.c.all-swap' % out, 'w').write(allv)
print("wrote jit_codegen.c.all-swap")

# ONE STEP OUTSIDE the brief's two pairs: swap a slot whose condition IS inlined (2 = jb) with one
# whose condition is NOT (0 = jo).  The unsupported encoding then LOOKS like it has a recipe and
# is compiled with jb's condition, while jb is de-inlined.  The matrix has no `jo` case at all, so
# if this mutant passes the suites the hole class is wider than "the CF=0,ZF=1 pairs".
extra = [('jcc8-swap-b0', 'instr32_70', 'instr32_72'),   # rel8 0=jo   <-> 2=jb
         ('jcc8-swap-s6', 'instr32_78', 'instr32_76')]   # rel8 8=js   <-> 6=jbe
for i, (name, a, b) in enumerate(extra):
    v = swap(src, a, b, 'X' + 'AB'[i])
    open('%s/jit_codegen.c.%s' % (out, name), 'w').write(v)
    print("wrote jit_codegen.c.%s" % name)
