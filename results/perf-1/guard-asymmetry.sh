#!/bin/bash
# guard-asymmetry.sh -- a test for the UNITY BUILD's collision guard (PERF-1).
#
# A guard that has never refused anything is not known to work, and the collision it
# exists for is the kind that produces NO ERROR: two file-scope `static` definitions
# with the same name and the SAME signature bind silently once both files are one
# translation unit (the compiler only catches the type-mismatch half).
#
# The guard fired for real twice while this unit was being built: `js_to_int32` is
# defined `static` in BOTH src/hw/ps2.c and src/hw/pit.c, and src/hw/kernel.c's `static
# write_blob` collides with src/cpu/memory.c's exported `write_blob` (different
# signature -- that one the compiler also caught).  The shipped build is CPU-only, so
# those two files are not in one unit there; the cases below are therefore SYNTHETIC
# (one fixture per collision SHAPE the guard must handle), so this test does not break
# when unrelated files are renamed, plus the real shipping set, which must emit.
#
# The asymmetry is the point: refuse-on-collision beside emit-on-the-shipped-set.
set -u
cd "$(dirname "$0")/../.." || exit 2
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

printf 'static int shared_helper(double x) { return (int)x; }\nint a(void) { return shared_helper(1.0); }\n' > "$TMP/a.c"
printf 'static int shared_helper(double x) { return (int)x; }\nint b(void) { return shared_helper(2.0); }\n' > "$TMP/b.c"
printf 'int exported(void) { return 1; }\n'                                              > "$TMP/g.c"
printf 'static int exported(void) { return 2; }\nint c(void) { return exported(); }\n'    > "$TMP/stat.c"

refuse() { # name, then the source list
    local name="$1"; shift
    if python3 gen/unity_c.py -o "$TMP/out.c" "$@" >"$TMP/log" 2>&1; then
        echo "FAIL  $name: the guard EMITTED a unit it must refuse"
        fail=1
    else
        echo "PASS  $name: refused ($(sed -n '2p' "$TMP/log"))"
    fi
}
emit() { # name, then the source list
    local name="$1"; shift
    if python3 gen/unity_c.py -o "$TMP/out.c" "$@" >"$TMP/log" 2>&1; then
        echo "PASS  $name: emitted"
    else
        echo "FAIL  $name: refused a unit it must emit"; cat "$TMP/log"; fail=1
    fi
}

refuse "static vs static, same name, same signature"          "$TMP/a.c" "$TMP/b.c"
refuse "a file-local static shadowing another unit's export"  "$TMP/g.c" "$TMP/stat.c"
emit   "the same file alone"                                  "$TMP/a.c"
emit   "the shipped unity set (cpu core, one unit)"           $(make -s unity-src)

exit $fail
