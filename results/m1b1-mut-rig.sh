#!/bin/sh
# G3 ASYMMETRY RIG (M1b-1).  Each arm is ONE single-line edit to src/cpu/instructions_0f.c.
# It must FAIL, and pass again once the line is restored.  Runs in a scratch tree so the
# concurrent unit's build is never touched.
set -u
F=src/cpu/instructions_0f.c
cp $F /tmp/m1b1-mut/0f.orig
run() {
    echo "----- $1 -----"
    if [ "$2" = selftest ]; then
        make selftest >/tmp/m1b1-mut/out.txt 2>&1; rc=$?
        echo "make selftest -> rc=$rc"
        grep -E "^FAIL|cpu_selftest:" /tmp/m1b1-mut/out.txt | head -8
    else
        make >/dev/null 2>&1; make v86c-nasm >/dev/null 2>&1
        make nasm >/tmp/m1b1-mut/out.txt 2>&1; rc=$?
        echo "make nasm -> rc=$rc"
        grep -E "Passed |Failed [0-9]+ test|^\[-\] [a-z]" /tmp/m1b1-mut/out.txt | head -8
    fi
    cp /tmp/m1b1-mut/0f.orig $F
}

echo "=== ARM 0: baseline (no mutation) ==="
run "baseline selftest" selftest
run "baseline nasm" nasm

echo
echo "=== M1: 0F 96 setbe -> setb (test_be() -> test_b()) ==="
sed -i 's/setcc_reg(test_be(), r)/setcc_reg(test_b(), r)/' $F
run "M1 (mutated)" nasm
run "M1 (restored)" nasm

echo
echo "=== M2: 0F B3's 16-bit memory form loses the sign extension ==="
sed -i 's/btr_mem(addr, SEX16(read_reg16(r))); }/btr_mem(addr, read_reg16(r)); }/' $F
grep -n "void instr16_0FB3_mem" $F
run "M2 (mutated)" selftest
run "M2 (restored)" selftest

echo
echo "=== M3: cmovne's memory form loses its page-fault guard ==="
python3 - <<'PY'
p='src/cpu/instructions_0f.c'
t=open(p).read()
old="""	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	cmovcc32(!test_z(), x, r);
}"""
new="""	int32_t x = safe_read32s(addr);
	if (0 && cpu->pagefault) return;
	cmovcc32(!test_z(), x, r);
}"""
assert t.count(old)==1
open(p,'w').write(t.replace(old,new))
PY
grep -n "0 && cpu->pagefault" $F | head -2
run "M3 (mutated)" selftest
run "M3 (restored)" selftest

echo
echo "=== M4: haddps writes the SOURCE pair first (the order is not commutative) ==="
sed -i 's/result.f32\[0\] = destination.f32\[0\] + destination.f32\[1\];/result.f32[0] = source.f32[0] + source.f32[1];/' $F
run "M4 (mutated)" selftest
run "M4 (restored)" selftest

echo
echo "=== ARM F: restored? ==="
diff /tmp/m1b1-mut/0f.orig $F && echo "src/cpu/instructions_0f.c restored byte-identical"
run "final selftest" selftest
