#!/bin/sh
# gate-reboot-1.sh -- REBOOT-1's gate (results/reboot-1.md, G1 + G2 + the runaway
# guard).  Run `make minibios-mem` first only if a --bios/--memory gate is added;
# this script needs nothing beyond `make` + `make reboot-images`' dependencies.
#
# G1: the two-boot image (tests/reboot2x.asm, both trigger variants) completes TWO
#     boots in the transcript mode, under all three execution configs (JIT on, JIT
#     off, uop cache off), with byte-identical transcripts across the configs and
#     the GOLD verdict 0x600D600D in the final EBX (--fixture prints the same).
# G2: the properties, checked against an INDEPENDENT model of the whole hashed 4 KiB
#     window (tests/reboot2x.asm's header documents every word): memory survived,
#     the CPU entered boot 2 at the multiboot entry state (ECX zeroed by the reset,
#     CS=0x08, CR0=1), the PS/2 and the PIT came back at their RESET values, and
#     two runs are byte-identical (the m2-5 determinism property, on a rebooting
#     guest).  Nothing but the modelled words may differ from zero -- an interrupt
#     delivered into the window's stack, say, would show up here.
# -- the runaway guard: tests/reboot-loop.img reboots forever and the driver must
#     NAME it (MAX_REBOOTS) rather than spin in it.
# -- REBOOT-FIX-1's two asymmetry images (results/reboot-fix-1.md), same checks
#     per arm as G1 (three configs, GOLD, byte-identity, the window model):
#     fwpage -- the trigger EXECUTED FROM PAGE 0xFF000 (the reset vector's page).
#       MEASURED: the trigger port write is a FLUSH+BLOCK_END uop, so the batch
#       ends at the trigger and this runs GOLD; it TRIPS (verdict 0xFACD0007 +
#       a broken window hash -- the marker at 0x101028) if a trigger ever stops
#       ending its reference batch, which is what would route the compiled TAIL
#       arm through the overwritten reset vector (the review's SF1 corner).
#     geom  -- the triggering batch's count equals the cumulative count at its
#       cycle's entry (17 = 17, PADS=13).  Pre-fix this ABORTED on
#       cycle_internal's DBG_ASSERT (rc 134, all three configs, MEASURED); the
#       reboot-tolerance conjunct makes the run land GOLD.
#
# THE KERNEL ARM (REBOOT_KERNEL_ARM=1, ~2 min): a real i386 Linux guest reboots
# ITSELF -- its own sysrq emergency restart takes the keyboard-controller path
# (out 0xFE to port 0x64, the PS/2 trigger site) -- and boot 2 reads the marker
# boot 1 wrote to /dev/sda sector 8 back out of the IDE device's buffer: the disk
# survived and was NOT re-seeded, and the host's disk.img file is never written.
# Needs tests/images/guest-initramfs-reboot.img (committed) and, for --memory 80,
# `make minibios-mem` (build/minibios-80m.bin).
set -e
cd "$(dirname "$0")/.."

V86C=build/v86c
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
PASS=0
FAIL=0

check() { # check <name> <cmd...>
	local name="$1"; shift
	if "$@" >/dev/null 2>&1; then
		PASS=$((PASS+1))
	else
		FAIL=$((FAIL+1))
		echo "FAIL: $name"
	fi
}

GOLD='0x600d600d'
for variant in pci ps2; do
	case $variant in
	pci) trigid=1 ;;
	ps2) trigid=2 ;;
	esac
	img=tests/reboot-$variant.img

	# the three execution configs; the transcripts must be byte-identical
	i=0
	for flags in "" "--jit off" "--uop-cache off"; do
		i=$((i+1))
		# shellcheck disable=SC2086
		check "$variant config '$flags' runs to a transcript" \
			sh -c "./$V86C $img 2000000 $flags > $TMP/$variant-$i.tx 2>$TMP/$variant-$i.err"
		# the fixture's EBX is the same verdict, from a second validation path
		# shellcheck disable=SC2086
		verdict=$(./$V86C $img 2000000 $flags --fixture 2>/dev/null | sed -n 6p | tr -d ' ,')
		# line 6 is reg32[3]: const.js's register order is EAX,ECX,EDX,EBX
		if [ "$verdict" = "1611489293" ]; then # 0x600D600D decimal
			PASS=$((PASS+1))
		else
			FAIL=$((FAIL+1)); echo "FAIL: $variant config '$flags' verdict is $verdict (want 1611489293 = GOLD)"
		fi
	done
	check "$variant: the three configs' transcripts are byte-identical" \
		cmp -s "$TMP/$variant-1.tx" "$TMP/$variant-2.tx"
	check "$variant: cache-off matches the shipped default" \
		cmp -s "$TMP/$variant-1.tx" "$TMP/$variant-3.tx"

	# G2's determinism: a second run of the same command
	# shellcheck disable=SC2086
	./$V86C $img 2000000 > "$TMP/$variant-rerun.tx" 2>/dev/null
	check "$variant: two runs are byte-identical (determinism on a rebooting guest)" \
		cmp -s "$TMP/$variant-1.tx" "$TMP/$variant-rerun.tx"

	# the window model: the ENTIRE hashed window is accounted for
	check "$variant: the hashed window matches the model exactly" \
		python3 tests/reboot_window_model.py "$trigid" "$TMP/$variant-1.tx"
done

# the runaway guard
rc=0
./$V86C tests/reboot-loop.img 2000000 >"$TMP/loop.out" 2>"$TMP/loop.err" || rc=$?
if [ "$rc" -ne 0 ] && grep -q "guest rebooted more than 16 times" "$TMP/loop.err"; then
	PASS=$((PASS+1))
else
	FAIL=$((FAIL+1)); echo "FAIL: the runaway reboot image ended rc=$rc without the named MAX_REBOOTS die"
fi

# REBOOT-FIX-1's two asymmetry images (results/reboot-fix-1.md), per-arm checks
# as G1's: three configs to a transcript, GOLD in each, byte-identity across the
# configs, the whole-window model.
for arm in fwpage geom; do
	img=tests/reboot-$arm.img
	i=0
	for flags in "" "--jit off" "--uop-cache off"; do
		i=$((i+1))
		# shellcheck disable=SC2086
		check "$arm config '$flags' runs to a transcript" \
			sh -c "./$V86C $img 2000000 $flags > $TMP/$arm-$i.tx 2>$TMP/$arm-$i.err"
		# shellcheck disable=SC2086
		verdict=$(./$V86C $img 2000000 $flags --fixture 2>/dev/null | sed -n 6p | tr -d ' ,')
		if [ "$verdict" = "1611489293" ]; then # 0x600D600D decimal
			PASS=$((PASS+1))
		else
			FAIL=$((FAIL+1)); echo "FAIL: $arm config '$flags' verdict is $verdict (want 1611489293 = GOLD)"
		fi
	done
	check "$arm: the three configs' transcripts are byte-identical" \
		cmp -s "$TMP/$arm-1.tx" "$TMP/$arm-2.tx"
	check "$arm: cache-off matches the shipped default" \
		cmp -s "$TMP/$arm-1.tx" "$TMP/$arm-3.tx"
	check "$arm: the hashed window matches the model exactly" \
		python3 tests/reboot_window_model.py 2 "$TMP/$arm-1.tx"
done

echo "gate-reboot-1: $PASS passed, $FAIL failed"

# the kernel arm: a real guest reboots itself and its disk write survives
if [ "${REBOOT_KERNEL_ARM:-0}" = "1" ]; then
	log="$TMP/kernel-serial.log"
	if ./build/v86c --bzimage tests/images/vmlinuz-i386 4000000000 --cmdline "console=ttyS0" \
		--hda tests/images/disk.img --initrd tests/images/guest-initramfs-reboot.img \
		--memory 80 --bios build/minibios-80m.bin --serial "$log" >"$TMP/kernel.out" 2>"$TMP/kernel.err"; then
		if grep -aq "DISK PERSIST OK" "$log" && grep -aq "BOOT1: marker written" "$log"; then
			echo "gate-reboot-1 kernel arm: PASS (two boots, the disk write survived, rc 0)"
		else
			echo "gate-reboot-1 kernel arm: FAIL (rc 0 but the persist lines are missing)"
			FAIL=$((FAIL+1))
		fi
	else
		echo "gate-reboot-1 kernel arm: FAIL (rc $?)"
		FAIL=$((FAIL+1))
	fi
	echo "gate-reboot-1 (with kernel arm): $PASS passed, $FAIL failed"
fi

[ "$FAIL" -eq 0 ]
