#!/bin/sh
# gate-pae-1.sh -- PAE-1's Level-2 gate (results/pae-1.md, F3): the synthetic PAE
# guest (tests/pae-guest.asm) runs byte-identically under BOTH legs -- the C port
# and the pinned JS reference -- and across all three execution configs, and its own
# verdict (EBX via --fixture) is GOLD (0x600D600D).  The guest builds a PDPT/PD/PT,
# enables CR4.PAE+CR3+CR0.PG, executes a stub through a 4 KiB mapping, runs all of
# its post-paging code through a 2 MiB mapping, does 4 KiB + 2 MiB data round-trips,
# DISCRIMINATES the 2 MiB high composition (PD[2] -> phys 0x200000 has PDE bit 21 = 1
# against the access's bit 21 = 0, so a PSE high-mask clobbers the seeded canary at
# phys 0 and the verdict leaves GOLD), and takes an NX #PF (error code 0x11) through
# a minimal IDT.  A gate that cannot fail is not a gate: the three configs'
# byte-identity catches a PAE walk that behaves differently under the JIT/uop-cache
# fast paths.
#
# WHAT THIS GATE DOES NOT COVER (selftest-only, results/pae-1.md F2): the 4 KiB
# arm's PTE reserved mask (high-half PTE), the PDE reserved mask, the PDPTE
# not-present/reserved/#GP paths, and load_pdpte's #GP.  The pinned reference still
# asserts on reserved PDPTEs, so the guest cannot exercise those by design.
#
# Needs `make` (build/v86c) and `node` (oracle/run.mjs); the image is built by the
# tests/pae-guest.img make rule.  The kernel-arm conventions (REBOOT_KERNEL_ARM) do
# not apply here.
set -e
cd "$(dirname "$0")/.."

V86C=build/v86c
IMG=tests/pae-guest.img
LIMIT=2000000
GOLD=1611489293   # 0x600D600D
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

make "$IMG" >/dev/null 2>&1

# the three execution configs, each to a transcript
i=0
for flags in "" "--jit off" "--uop-cache off"; do
	i=$((i+1))
	# shellcheck disable=SC2086
	check "config '$flags' runs to a transcript" \
		sh -c "./$V86C $IMG $LIMIT $flags > $TMP/c-$i.tx 2>/dev/null"
	# shellcheck disable=SC2086
	verdict=$(./$V86C $IMG $LIMIT $flags --fixture 2>/dev/null | sed -n 6p | tr -d ' ,')
	if [ "$verdict" = "$GOLD" ]; then
		PASS=$((PASS+1))
	else
		FAIL=$((FAIL+1)); echo "FAIL: config '$flags' verdict is $verdict (want $GOLD)"
	fi
done

# the reference leg
check "the reference leg runs to a transcript" \
	sh -c "node oracle/run.mjs $IMG $LIMIT > $TMP/js.tx 2>/dev/null"

# byte-identity: the three C configs agree with each other and with the reference
check "default == --jit off"           cmp -s "$TMP/c-1.tx" "$TMP/c-2.tx"
check "default == --uop-cache off"     cmp -s "$TMP/c-1.tx" "$TMP/c-3.tx"
check "default == reference"           cmp -s "$TMP/c-1.tx" "$TMP/js.tx"

# determinism: a second run of the default config
./$V86C $IMG $LIMIT > "$TMP/c-rerun.tx" 2>/dev/null
check "two default runs are byte-identical" cmp -s "$TMP/c-1.tx" "$TMP/c-rerun.tx"

echo "gate-pae-1: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
