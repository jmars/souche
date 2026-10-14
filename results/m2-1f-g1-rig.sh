#!/bin/sh
# results/m2-1f-g1-rig.sh -- M2-1F's G1 ASYMMETRY RIG (the D-F1 policy decision).
#
# The claim being tested: "an unclaimed port now answers as the pinned reference does instead
# of stopping the machine".  A test can only prove that if it FAILS against the pre-fix
# artifact and PASSES against the tree.  results/m2-1f-io-prefix.c IS that artifact: the
# tree's src/hw/io.c with the six dispatch bodies restored to the pre-D-F1 stop form (M1c's
# policy) and the stop message restored to M2-1's wording.
#
# Usage: sh results/m2-1f-g1-rig.sh        (from the project root; transcript on stdout)
#
# The exit status of the SELFTEST is the gate, not the tail of its output: the pre-fix side
# must exit NON-ZERO (the port-0x80 write stops the machine) and the restored side ZERO.

set -u
cd "$(dirname "$0")/.." || exit 2

side() {
    label="$1"
    echo "--- $label ---"
    make build/hw_selftest >/tmp/m2-1f-g1-build.log 2>&1
    echo "build RC=$?"
    ./build/hw_selftest >/tmp/m2-1f-g1-run.log 2>&1
    rc=$?
    grep -E 'PORT-0x80|hw_selftest:' /tmp/m2-1f-g1-run.log
    echo "FAIL lines: $(grep -c '^FAIL src/hw' /tmp/m2-1f-g1-run.log)"
    echo "selftest exit status (THE GATE): $rc"
    echo
}

echo "=== M2-1F G1 -- the port-0x80 probe, BEFORE and AFTER (D-F1) ==="
echo
cp src/hw/io.c /tmp/m2-1f-io-tree.c

cp results/m2-1f-io-prefix.c src/hw/io.c
side "BEFORE: results/m2-1f-io-prefix.c copied over src/hw/io.c"

cp /tmp/m2-1f-io-tree.c src/hw/io.c
side "AFTER: the tree's src/hw/io.c restored"
