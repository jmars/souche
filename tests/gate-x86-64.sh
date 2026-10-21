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
LIMIT=20000000
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
step "kernel-page-tables-in-ram" sh -c \
	"grep -q 'cr3=0x4f76000' $BOOTLOG"
step "kernel-serial-output" sh -c "grep -q 'Probing EDD' $SERIAL"

echo "== D. THE HONEST STOP, ASSERTED BY NAME (the plan's falsifier, recorded)"
# THE FINDING, and the reason this gate is not green-and-done: the kernel stops at the 64-bit
# interrupt frame.  The assertion is that it stops THERE -- a stop EARLIER is a regression, and a
# silent hang is a defect; when the frame lands this step is REPLACED by the userspace assertion.
step "stop-is-the-64bit-interrupt-frame" sh -c \
	"grep -q 'call_interrupt_vector' $BOOTLOG"

echo
echo "[+] gate-x86-64: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
