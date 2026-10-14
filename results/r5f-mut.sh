#!/bin/sh
# R5F's mutation rig: THREE single-variable mutations of the fixed tree, each applied to a
# SHADOW copy of src/cpu (never the live tree -- src/hw/ is another unit's lane while this
# runs).  Two artifacts are rebuilt against each shadow: the probe (results/r5f-probe.sh)
# and the SHIPPED selftest (make selftest's m1c binary, same flags and sources).
#
#   mutB  "fix reverted": delete the post-dispatch clear from cpu.c.
#         MEASURED: the probe's `same` scenario aborts at cpu.c:145 (B1's exact failure) and
#         the selftest's new group aborts the same way, while `diff` and `fetch` still pass
#         -- which is what makes that ONE added line the cause.
#   mutA  "cycle-entry fetch check deleted" from cpu.c.
#         PREDICTED "fetch aborts at cpu.c:90"; MEASURED otherwise, and the measurement is
#         the better evidence: in_mapped_range(0) is FALSE (memory.h:93 -- addr 0 is neither
#         in the VGA window nor >= memory_size), so nothing asserts.  Instead the guest was
#         left at the HANDLER by the delivery and the run INTERPRETED PHYSICAL ADDRESS 0
#         from there: 3 instructions counted, none of them the guest's, the handler's marker
#         never set -- a SILENT wrong execution, which is what the flag's abort prevents.
#         The selftest catches it too (5 failures in the new group, rc 1), and `same` passes.
#   mutC  S2's asymmetry: ONE of the four dropped dbg_assert!s put back (the EFER one, the
#         first in the flow) in instructions_0f.c.  MEASURED: the selftest aborts on guest
#         bytes, which is S2's exact failure.
set -u
SHADOW=/tmp/r5f-mut
rm -rf "$SHADOW"; mkdir -p "$SHADOW"
cp -r src/cpu "$SHADOW/"
COSMOCC=${COSMOCC:-/home/jaye/.local/cosmo/bin/cosmocc}
CPU_SRC="cpu/cpu_state.c cpu/flags.c cpu/memory.c cpu/paging.c cpu/arith.c cpu/modrm.c \
cpu/misc_instr.c cpu/string.c cpu/instructions.c cpu/instructions_0f.c cpu/sse_instr.c \
cpu/softfloat.c cpu/fpu.c cpu/exceptions.c cpu/pic.c cpu/cpu.c cpu/exec_boundary.c"
HW_SRC="src/hw/io.c src/hw/bus.c src/hw/pit.c src/hw/rtc.c src/hw/dma.c src/hw/ps2.c \
src/hw/acpi.c src/hw/pci.c src/hw/machine.c"
SHA_C=$(sha256sum src/cpu/cpu.c | cut -d' ' -f1)
SHA_0F=$(sha256sum src/cpu/instructions_0f.c | cut -d' ' -f1)

# every mutation starts from the LIVE sources, so each is single-variable
reset_shadow() { cp src/cpu/*.c src/cpu/*.h "$SHADOW/cpu/"; }

mutB() {
python3 - "$SHADOW/cpu/cpu.c" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
a = "\t\tcpu->pagefault = false;\n\t\tDBG_ASSERT(cpu->prefixes == 0);\n\n\t\t/* the port"
b = "\t\tDBG_ASSERT(cpu->prefixes == 0);\n\n\t\t/* the port"
assert s.count(a) == 1, "post-dispatch clear: count=%d" % s.count(a)
open(p, "w").write(s.replace(a, b))
EOF
}

mutA() {
python3 - "$SHADOW/cpu/cpu.c" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
a = "\tuint32_t phys_addr = get_phys_eip();\n\tif (cpu->pagefault) return;\n"
b = "\tuint32_t phys_addr = get_phys_eip();\n"
assert s.count(a) == 1, "cycle-entry check: count=%d" % s.count(a)
open(p, "w").write(s.replace(a, b))
EOF
}

mutC() {
python3 - "$SHADOW/cpu/instructions_0f.c" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
a = ("\t\tuint64_t value = ((uint64_t)(uint32_t)high << 32) | (uint64_t)(uint32_t)low;\n"
     "\t\t/* upstream: dbg_assert!(value & !EFER_NXE == 0")
b = ("\t\tuint64_t value = ((uint64_t)(uint32_t)high << 32) | (uint64_t)(uint32_t)low;\n"
     "\t\tDBG_ASSERT((value & ~EFER_NXE) == 0);\n"
     "\t\t/* upstream: dbg_assert!(value & !EFER_NXE == 0")
assert s.count(a) == 1, "EFER site: count=%d" % s.count(a)
open(p, "w").write(s.replace(a, b))
EOF
}

run_probe() {
    sh results/r5f-probe.sh "$SHADOW" 2>&1 | grep -v '^== building'
}

# the SHIPPED gate, rebuilt against the shadow with `make selftest`'s own rule
run_selftest_shadow() {
    SRCS=""
    for f in $CPU_SRC; do SRCS="$SRCS $SHADOW/$f"; done
    $COSMOCC -std=gnu11 -O2 -Wall -Wextra -Werror -Wl,--gc-sections \
        -o /tmp/r5f-mut-selftest "$SHADOW/cpu/m1c_selftest.c" $SRCS $HW_SRC \
        build/cpu/interpreter_gen.o build/sf/softfloat.o || { echo "BUILD FAILED"; return 1; }
    out=$(/tmp/r5f-mut-selftest 2>&1); rc=$?
    echo "$out" | grep -vE "^(PASS|test_)"
    echo "rc=$rc (a nonzero rc or an abort above is the mutation being caught)"
}

for name in mutB mutA mutC; do
    reset_shadow
    $name || { echo "$name: MUTATION FAILED TO APPLY"; exit 1; }
    echo "=== $name ==="
    echo "shadow cpu.c sha: $(sha256sum "$SHADOW/cpu/cpu.c" | cut -d' ' -f1)  live: $SHA_C"
    echo "shadow 0F.c  sha: $(sha256sum "$SHADOW/cpu/instructions_0f.c" | cut -d' ' -f1)  live: $SHA_0F"
    run_probe
    echo "--- the shipped m1c selftest against this shadow ---"
    run_selftest_shadow
    echo
done

echo "=== the live tree (never mutated) ==="
echo "live cpu.c sha: $(sha256sum src/cpu/cpu.c | cut -d' ' -f1)  (want $SHA_C)"
echo "live 0F.c  sha: $(sha256sum src/cpu/instructions_0f.c | cut -d' ' -f1)  (want $SHA_0F)"
rm -rf "$SHADOW"
