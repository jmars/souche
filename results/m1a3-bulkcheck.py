#!/usr/bin/env python3
"""M1a-3 bulk cross-check: the SINGLE-LINE instruction functions against the upstream Rust.

Not a test of the port's semantics (the selftest is) -- a second reader for the ~500
one-liner transcriptions where a swapped operand or a wrong register index would be a
silent wrong result that a slice-based test may not reach.  Every "mismatch" it prints is
one of the KNOWN, stated divergences (SEX8/SEX16 for Rust's `x << n >> n`; the uint32_t
cast on mul/div; C's `~` for Rust's bitwise `!`; the `(void)` casts for unused
parameters) or a normaliser artifact.  Run: python3 results/m1a3-bulkcheck.py
"""
import re, sys

def strip_rf(s):
    # remove return_on_pagefault!(...) wrappers (balanced)
    out=[]; i=0
    while i < len(s):
        if s.startswith('return_on_pagefault!(', i):
            j = i + len('return_on_pagefault!(')
            depth = 1
            while j < len(s) and depth:
                if s[j] == '(': depth += 1
                elif s[j] == ')': depth -= 1
                j += 1
            out.append(strip_rf(s[i+len('return_on_pagefault!('):j-1]))
            i = j
        else:
            out.append(s[i]); i += 1
    return ''.join(out)

def norm_rust(body):
    b = body
    b = re.sub(r'return_on_pagefault!\((.*)\)', r'\1', b) if 'return_on_pagefault!' not in b else strip_rf(b)
    b = b.replace('&|x| ', '').replace('&|x|', '')
    b = re.sub(r'\bunsafe\b', '', b)
    b = b.replace('*flags_changed','FLAGSCHANGED').replace('*flags','FLAGS').replace('*prefixes','PREFIXES')
    b = b.replace('*instruction_pointer','IP').replace('*previous_ip','PREV').replace('*instruction_counter','ICOUNT')
    b = re.sub(r'\(\*([a-z_]+)\.offset\(([^)]+)\)\)', r'\1[\2]', b)
    b = re.sub(r'\*([a-z_]+)\.offset\(([^)]+)\)', r'\1[\2]', b)
    b = re.sub(r'\*([a-z_]+)\b', r'\1', b)
    b = re.sub(r'\bas (i16|i32|u8|u16|u32|usize)\b', '', b)
    # Rust's bitwise ! on the closure variable -> C's ~
    b = re.sub(r'!x\b', '~x', b)
    b = re.sub(r'\btrue\b','TRUE',b); b = re.sub(r'\bfalse\b','FALSE',b)
    b = re.sub(r'\s+','',b)
    b = re.sub(r'safe_read_write(8|16|32)\(([^,]+),([^)]*)\)', r'RMW\1(\2,\3)', b)
    return b.rstrip(';')

def norm_c(body):
    b = body
    b = re.sub(r'\b(int32_t|uint32_t|uint16_t|int16_t|int8_t|uint8_t)\b','',b)
    b = b.replace('(void)','')
    b = re.sub(r'\((?:int32_t|uint32_t|int16_t|uint16_t|uint8_t)\)','',b)
    b = b.replace('cpu->flags_changed','FLAGSCHANGED').replace('cpu->flags','FLAGS').replace('cpu->prefixes','PREFIXES')
    b = b.replace('cpu->instruction_pointer','IP').replace('cpu->previous_ip','PREV').replace('cpu->instruction_counter','ICOUNT')
    b = b.replace('cpu->','GLOBALS.').replace('GLOBALS.flags','FLAGS')
    b = re.sub(r'\s+','',b)
    b = re.sub(r'Safe_read_write([0-9]+)\(addr,([^;]*)\)','RMW\\1(addr,\\2)',b)
    b = b.replace('SAFE_READ_WRITE8','RMW8').replace('SAFE_READ_WRITE16','RMW16').replace('SAFE_READ_WRITE32','RMW32')
    b = b.replace('SEX8','sex8').replace('SEX16','sex16')
    b = re.sub(r'\bx_\b','x',b)
    b = b.rstrip(';').replace(';',';')
    b = b.rstrip(';')
    return b

def defs(text, pat):
    """yield (name, args, body) for single-line definitions"""
    for m in re.finditer(pat, text):
        yield m.group(1), m.group(2)

import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
up = open(os.path.join(ROOT, 'v86/src/rust/cpu/instructions.rs')).read()
c  = open(os.path.join(ROOT, 'src/cpu/instructions.c')).read()

# upstream: pub unsafe fn NAME(args) { body }   (body must not span a line break)
ur = {}
for m in re.finditer(r'pub unsafe fn (\w+)\(([^)]*)\) \{ ([^\n]*?) \}', up):
    ur[m.group(1)] = (m.group(2), m.group(3))

cr = {}
for m in re.finditer(r'^(?:void|int32_t|bool) (\w+)\(([^)]*)\) \{ ([^\n]*?) \}$', c, re.M):
    cr[m.group(1)] = (m.group(2), m.group(3))

skip = re.compile(r'^instr(16|32)?_D[89A-F]_\d')
mis = []
n = 0
for name,(a1,b1) in ur.items():
    if skip.match(name): continue
    if name not in cr: continue
    a2,b2 = cr[name]
    if 'return_on_pagefault' in b1: continue      # hoisted to a local in C -> not comparable
    r1, r2 = norm_rust(b1), norm_c(b2)
    n += 1
    if r1 != r2:
        mis.append((name, b1, b2, r1, r2))
print("single-line functions compared:", n, " mismatches:", len(mis))
for name,b1,b2,r1,r2 in mis:
    print(f"--- {name}\n    rust: {b1}\n    C   : {b2}\n    normR: {r1}\n    normC: {r2}")
