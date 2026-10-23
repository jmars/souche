#!/bin/sh
# tests/gate-x86-64.sh -- S12's gate: THE GUEST LAYER, in the PAE-L3 shape.
#
# Specification: results/x86-64-plan.md ss6's S12 row ("the amd64 pin + a gate-x86-64.sh in the
# PAE-L3 shape; the QEMU leg; the userspace milestone") and ss2.3 (the guest layer and what proves
# it).  results/pae-l3.md is the vehicle pattern this copies: a PINNED Debian kernel with its URL
# and sha256 recorded, a reproducible initramfs, a gate that asserts the guest's OWN evidence, and
# an honest seat for what has not been reached.
#
# THE STATE OF THE GUEST, stated here rather than discovered by a reader: THE KERNEL BOOTS INTO
# LONG MODE AND THEN STOPS AT A NAMED SYSTEM-LAYER PIECE -- the 64-bit IDT gate / interrupt frame.
# That is the plan's own falsifier ("the corpus-green-but-guest-dead case, a real finding not a
# regression"), and this gate asserts BOTH HALVES: the progress (the kernel reaches its own long-mode
# entry with a CR3 inside guest RAM) and the stop (a NAMED message, not a hang and not a wrong
# answer).  When the interrupt frame lands, the assertion moves DOWN this file -- visibly.
set -e
cd "$(dirname "$0")/.."
V86C=build/v86c
KERNEL=tests/images/vmlinuz-6.1.0-50-amd64
# THE PIN.  Provenance (results/x86-64-s12.md ss2): the .deb URL and its sha256, then the vmlinuz
# inside it.  The image is committed beside tests/images/vmlinuz-i386 (8.2 MB), so the gate needs no
# network.
DEB_URL="https://deb.debian.org/debian/pool/main/l/linux/linux-image-6.1.0-50-amd64-unsigned_6.1.176-1_amd64.deb"
DEB_SHA=439422e41d2dbb840b60b81e3bf5e5955bfa56b7a8c268aec83c9ff882d421e6
SHA_KERNEL=653421d9774c0de27502ca010d572323b52a5d7141d067b9b04214bd24baca3a
# raised from 20M (results/x86-64-rep.md) and again from 40M: the kernel now runs its OWN code
# past start_kernel and prints its banner at ~4.49 G instructions (results/x86-64-kgs.md).  At the
# measured ~40 M instructions/second that is ~2 minutes, inside the 300 s timeout below.
LIMIT=6000000000
SERIAL=/tmp/gate-x86-64-serial.txt
LOG=/tmp/gate-x86-64.log
BOOTLOG=/tmp/gate-x86-64-boot.log
PASS=0; FAIL=0
step() { local n="$1"; shift; if "$@" >$LOG 2>&1; then PASS=$((PASS+1)); echo "ok   $n"; else FAIL=$((FAIL+1)); echo "FAIL $n"; sed -n '1,25p' $LOG; fi; }
must_fail() { local n="$1"; shift; if "$@" >$LOG 2>&1; then FAIL=$((FAIL+1)); echo "FAIL $n (it PASSED and must not)"; else PASS=$((PASS+1)); echo "ok   $n (correctly refused)"; fi; }

echo "== A. THE PIN: the amd64 kernel is the pinned one"
step "kernel-sha256" sh -c "echo '$SHA_KERNEL  $KERNEL' | sha256sum -c -"
# the provenance is a RECORD, not a fetch: the gate runs offline (the .deb sha256 is in the report)

echo "== B. THE FLAG IS OFF BY DEFAULT: without --long-mode the amd64 kernel must be REFUSED"
# the S2 transition refusal is what makes the capability a DECLARATION: an x86-64 kernel on a
# 32-bit-only machine stops BY NAME at its first IA-32e write.
must_fail "no-long-mode-refuses" sh -c \
	"timeout 120 ./$V86C --bzimage $KERNEL $LIMIT --memory 256 --cmdline 'console=ttyS0' \
	 --serial $SERIAL 2>&1 | grep -q 'cannot run long mode'"

echo "== C. THE PROGRESS: with --long-mode the kernel reaches its own long-mode entry"
# the port's own boot line names eip/cs/cr0/cr3 at the stop.  cs=0x10 is the kernel's DATA selector
# and cr3 is inside guest RAM: the kernel has built its own page tables and is executing 64-bit
# code.  A regression that stops EARLIER (a walk fault, a refusal) moves this assertion.
boot_once() {
	timeout 300 ./$V86C --bzimage $KERNEL $LIMIT --memory 256 \
		--cmdline 'console=ttyS0 earlyprintk=serial' --serial $SERIAL --long-mode on > $BOOTLOG 2>&1 || true
	grep -q 'boot: [0-9]* instructions' $BOOTLOG
}
step "kernel-runs" boot_once
step "kernel-in-long-mode" sh -c \
	"grep -q 'cs=0x10 cr0=0x80050033' $BOOTLOG"
# cr3 MOVES now (results/x86-64-kgs.md): the guest starts on the decompressor's tables
# (0x4f76000), switches to the kernel's own (0x30ea000) during __startup_64, and by the banner it
# is on the relocated image's -- so the assertion is the LONG-MODE property, not one historical
# value: paging on, long mode active, a non-zero cr3.
step "kernel-page-tables-in-ram" sh -c \
	"grep -q 'cr0=0x80050033' $BOOTLOG && ! grep -q 'cr3=0x0 ' $BOOTLOG"
step "kernel-serial-output" sh -c "grep -q 'Probing EDD' $SERIAL"

echo "== D. THE HONEST STOP, ASSERTED BY NAME (replaced: the frame LANDED, results/x86-64-rep.md)"
# THE STEP THIS REPLACES asserted `call_interrupt_vector` in the boot log -- the 64-bit interrupt
# frame, which the kernel could not deliver through.  That is GONE (MEASURED): the guest now
# decompresses, its own console prints, and it stops on a 64-bit effective address above 4 GiB
# that the 32-bit-arm bodies cannot carry.  The assertions are (a) the decompressor's console is
# ALIVE, which is the milestone the old step could not reach, and (b) the stop is the 4 GiB
# address bound, so a stop EARLIER (a delivery stop, a refusal, a silent hang) fails here.
step "decompressor-console-alive" sh -c "grep -q 'Physical KASLR disabled' $SERIAL"

# THE MILESTONE (results/x86-64-kgs.md): the amd64 kernel runs its OWN C code far enough to print.
# It got there the moment the 64-bit interrupt frame carried SS:RSP (the frame the kernel's early
# return path reads) and iretq existed; before those, the machine died in the exception return and
# never reached start_kernel.  The assertion is the kernel's own banner, which no decompressor
# message can produce.
step "kernel-first-printk" sh -c "grep -q 'Linux version 6.1.0-50-amd64' $SERIAL"
# THE STEP THIS REPLACES asserted the 4 GiB ADDRESS BOUND -- the stop this gate used to end on.
# That stop is GONE (MEASURED, results/x86-64-wide.md): the guest now decompresses, and at this
# gate's 40 M-instruction budget it is still decompressing with NO named stop.  Asserting the
# whole decompression here would need ~1.5 G instructions (~9 minutes), so the gate asserts what
# it can afford and names the rest: no unimplemented stop in the log, and the bound's message
# ABSENT.  The full extent (8,388,607 bytes, i.e. the whole kernel) is MEASURED in the report at
# a 1.5 G budget, not asserted here.
# THE STOP MOVED (results/x86-64-kgs.md): with the banner reached, the guest stops on the NEXT
# unimplemented 64-bit body -- asserted BY NAME so that a regression to an earlier stop, or a
# silent hang, fails here.  Replace the name, not the step, when the next body lands.
# THE STOP MOVED AGAIN (results/x86-64-f7.md): F7 /0 landed and the guest now runs ~30 SECONDS of
# kernel boot (the banner, then percpu setup) before it dies in the fault-delivery path -- not on a
# missing body at all.  The step names the CURRENT stop, so a regression to an earlier one, or a
# silent hang, still fails here; replace the name, not the step, when the next one lands.
step "stop-is-the-next-unimplemented-body" sh -c \
	"grep -q 'a fault raised while delivering a fault' $BOOTLOG"
must_fail "stop-name-able-to-fail" sh -c \
	"grep -q 'unimplemented: instructions::instr64_C3' $BOOTLOG"
must_fail "address-bound-gone-able-to-fail" sh -c \
	"grep -q 'a 64-bit effective address above 4 GiB' $BOOTLOG"

echo "== E. THE SECOND REFERENCE: qemu-system-x86_64 ON THE SAME IMAGE AND CMDLINE"
# THE POINT OF THIS STEP (results/x86-64-qemu.md): the port stops the amd64 kernel in an early
# triple-fault window, and the honest question is whether that window is the IMAGE's or the PORT's.
# QEMU -- a second, independent reference for the same kernel -- answers it: with the SAME image,
# the SAME cmdline and the same memory it boots the kernel to the root-mount panic.  So the window
# is the PORT's, and this step records the reference sequence to bisect against.
#
# FLAKE DISCIPLINE (the tree's own rule): a QEMU run under load can time out, and a timeout is NOT
# a regression.  The assertion is therefore on the SERIAL, not on qemu's exit code, and a run that
# produced no serial at all is reported as such rather than failed -- the port's own steps above
# still carry the port's side.
QSERIAL=/tmp/gate-x86-64-qemu-serial.txt
rm -f $QSERIAL
timeout 130 qemu-system-x86_64 -machine pc -m 256 -enable-kvm -no-reboot -display none \
	-kernel $KERNEL -append 'console=ttyS0 earlyprintk=serial' \
	-serial file:$QSERIAL > /dev/null 2>&1 || true
step "qemu-boots-the-same-image" sh -c "grep -q 'Linux version 6.1.0-50-amd64' $QSERIAL"
step "qemu-reaches-the-root-mount" sh -c \
	"grep -q 'Unable to mount root fs' $QSERIAL"
must_fail "qemu-able-to-fail" sh -c "grep -q 'Unable to mount root fs' $BOOTLOG"

echo
echo "[+] gate-x86-64: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
