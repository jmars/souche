# preserved from /tmp/flagref (the M1a-1 reference cross-check; run with cwd=/tmp/flagref)
import subprocess, sys

CASES = [
 (0,0x0f,0x01,8,0),(0,0xff,0x01,8,0),(0,0x7fffffff,0x1,32,0),(0,0xffffffff,0x1,32,0),
 (0,0x7fff,0x1,16,0),(0,0xffff,0x1,16,0),
 (2,0,0x1,32,0),(2,0x5,0x5,32,0),(2,0x80,0x1,8,0),(2,0x8000,0x1,16,0),
 (3,0x12345678,0x12345678,32,0),(3,0x1,0x2,32,0),
 (1,0x7f,0x0,8,1),(1,0xffffffff,0x0,32,1),
 (4,0,0,32,1),(4,0x100,0x1,32,0),
 (5,0x7fffffff,0x0,32,1),(5,0x7f,0x0,8,0),(5,0xffffffff,0x0,32,0),
 (6,0x80,0x0,8,1),(6,0x1,0x0,32,1),
 (7,0x1,0x0,32,0),(7,0x80000000,0x0,32,0),(7,0x0,0x0,8,0),
]
MNEM = {0:'add',1:'adc',2:'sub',3:'cmp',4:'sbb',5:'inc',6:'dec',7:'neg'}
REGS = {32:('eax','ebx','dword ptr'),16:('ax','bx','dword ptr'),8:('al','bl','dword ptr')}

L = []
L.append('\t.intel_syntax noprefix')
L.append('\t.section .mbheader, "a", @progbits')
L.append('\t.align 4')
L.append('\t.long\t0x1BADB002')
L.append('\t.long\t0x00010000')
L.append('\t.long\t-(0x1BADB002 + 0x00010000)')
L.append('\t.long\t0x00100000')
L.append('\t.long\t0x00100000')
L.append('\t.long\t0')
L.append('\t.long\t0x00101000')
L.append('\t.long\t0x00100020')
L.append('\t.section .text')
L.append('\t.code32')
L.append('\t.globl _start')
L.append('_start:')
L.append('\tcld')
L.append('\tmov\tesp, 0x102000')
for i,(op,a,b,size,cf) in enumerate(CASES):
    d,s,_ = REGS[size]
    mn = MNEM[op]
    L.append('/* case %d: %s%d a=0x%x b=0x%x cf_in=%d */' % (i, mn, size, a, b, cf))
    if size == 8:
        L.append('\tmov\t%s, 0x%x' % (d, a & 0xff))
        L.append('\tmov\t%s, 0x%x' % (s, b & 0xff))
    elif size == 16:
        L.append('\tmov\t%s, 0x%x' % (d, a & 0xffff))
        L.append('\tmov\t%s, 0x%x' % (s, b & 0xffff))
    else:
        L.append('\tmov\t%s, 0x%x' % (d, a & 0xffffffff))
        L.append('\tmov\t%s, 0x%x' % (s, b & 0xffffffff))
    if cf: L.append('\tstc')
    else:  L.append('\tclc')
    if op in (5,6,7):   # inc/dec/neg take one operand
        L.append('\t%s\t%s' % (mn, d))
    else:
        L.append('\t%s\t%s, %s' % (mn, d, s))
    L.append('\tpushf')
    L.append('\tpop\tdword ptr [0x%x]' % (0x101000 + 4*i))
L.append('\thlt')
open('flagcases.s','w').write('\n'.join(L) + '\n')

subprocess.run(['as','--32','-o','flagcases.o','flagcases.s'], check=True)
subprocess.run(['ld','-m','elf_i386','-T','/home/jaye/futaie/souche/tests/test.ld','-o','flagcases.elf','flagcases.o'], check=True)
subprocess.run(['objcopy','-O','binary','flagcases.elf','flagcases.img'], check=True)
print("image written: flagcases.img, %d cases" % len(CASES))
