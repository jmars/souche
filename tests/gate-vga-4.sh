#!/bin/sh
# tests/gate-vga-4.sh -- VGA-U4's gate (HANDOFF=v86c-vga).
#
# WHAT U4 CLAIMS, AND WHAT THIS SCRIPT THEREFORE ASSERTS.  U4 lands the Bochs VBE dispi
# interface (0x1CE/0x1CF, 16-bit ports), the 0xE0000000 linear framebuffer, bank switching
# through the 0xA0000 window, and the REAL dirty bitmap behind the bpp-32 LFB->RGBA fill
# (the bpp 8/15/16/24 arms are the hw_selftest case 9f probes).  The differential image
# (tests/vga-lfb.img) self-programs 320x200x32 through dispi, paints the LFB, switches a
# bank through the window, and reads everything back in-guest before halting.
#
# THE HARDEST REQUIREMENT IS OUTPUT INVARIANCE: the dirty bitmap exists to let a later unit
# skip work, and the reference's own redraw is a full recompute -- if making the bitmap real
# changes ANY document byte against the reference, D1 catches it here.  The bitmap is
# invisible by construction (the dest buffer is zeroed at allocation, svga_mark_dirty runs
# in the same block, so the single stop-time fill is a full recompute), and this gate proves
# it: D1 must stay byte-identical.
#
#   D1  THE TWO-LEG DOCUMENT: same image, same limit, this port vs the pinned reference
#       (oracle/run-vga.mjs) -- byte-identical, header AND payload.
#   D2  THE PAYLOAD IS NON-TRIVIAL: mode/dims/distinct-colour count and the five named
#       pixels (each the reference's own 32bpp BGRX -> RGBA shuffle), so a blank/single-
#       colour framebuffer or a wrong byte-lane/stride FAILS here, not silently passes.
#   D3  THE DOCUMENT IS A FUNCTION OF THE MACHINE: each leg twice, cmp.
#   D4  THE GUEST'S OWN VERDICT: the image reads its LFB pixels back in-guest and halts
#       only on a match; both legs producing a document already proves the halt, re-stated
#       here.
#
# EXIT CODES ARE THE VERDICT.  Usage:
#     sh tests/gate-vga-4.sh          (the whole gate)

set -u
cd "$(dirname "$0")/.."

V86C=${V86C:-build/v86c}
LIMIT=${LIMIT:-2000000}
WORK=${WORK:-/tmp/gate-vga-4}

fails=0
pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; fails=$((fails + 1)); }

require() {
    [ -x "$V86C" ] || { echo "gate-vga-4: no $V86C (run make)" >&2; exit 2; }
    command -v node >/dev/null || { echo "gate-vga-4: no node" >&2; exit 2; }
    [ -f oracle/run-vga.mjs ] || { echo "gate-vga-4: no oracle/run-vga.mjs" >&2; exit 2; }
    [ -f tests/vga-lfb.img ] || { echo "gate-vga-4: no tests/vga-lfb.img (run make tests/vga-lfb.img)" >&2; exit 2; }
}

# d2(doc) -- the whole D2 non-triviality check in python: the header's mode/dims/svga
# fields, the distinct-colour count, and the five named pixels.
d2() {
    python3 - "$1" <<'PYEOF'
import json, sys
doc = sys.argv[1]
data = open(doc, "rb").read()
nl = data.index(b"\n")
header = json.loads(data[nl + 1 : data.index(b"\n", nl + 1)])
payload = data[data.index(b"\n", nl + 1) + 1 :]
vw, vh = header["virtual_width"], header["virtual_height"]
sw, sh = header["screen_width"], header["screen_height"]
def px(x, y):
    off = (y * vw + x) * 4
    return (payload[off], payload[off + 1], payload[off + 2])
distinct = len(set(px(x, y) for y in range(sh) for x in range(sw)))
print("MODE", header["mode"])
print("SVGA", header["svga_enabled"], "BPP", header["bpp"])
print("DIMS", f"{sw}x{sh}", f"{vw}x{vh}")
print("DISTINCT", distinct)
for name, x, y, want in [
    ("(0,0)", 0, 0, "255,0,0"),
    ("(319,0)", 319, 0, "0,255,0"),
    ("(0,1)", 0, 1, "0,0,255"),
    ("(160,100)", 160, 100, "255,255,255"),
    ("(319,199)", 319, 199, "255,255,0"),
]:
    got = ",".join(str(v) for v in px(x, y))
    print("PIX", name, got, "EXPECT", want)
PYEOF
}

echo "### vga-lfb: the two-leg document, tests/vga-lfb.img at $LIMIT instructions"
require
mkdir -p "$WORK"

if ! "$V86C" tests/vga-lfb.img "$LIMIT" --vga-doc "$WORK/lfb-c.vga" > "$WORK/lfb-c.transcript" 2> "$WORK/lfb-c.err"; then
    fail "vga-lfb: the C leg did not halt-and-document (see $WORK/lfb-c.err)"
else
    if ! node oracle/run-vga.mjs tests/vga-lfb.img "$LIMIT" "$WORK/lfb-js.vga" 2> "$WORK/lfb-js.err"; then
        fail "vga-lfb: the reference leg did not halt-and-document (see $WORK/lfb-js.err)"
    else
        echo "    C leg:  $(wc -c < "$WORK/lfb-c.vga") bytes, $(sha256sum "$WORK/lfb-c.vga" | cut -d' ' -f1)"
        echo "    JS leg: $(wc -c < "$WORK/lfb-js.vga") bytes, $(sha256sum "$WORK/lfb-js.vga" | cut -d' ' -f1)"
        if cmp -s "$WORK/lfb-c.vga" "$WORK/lfb-js.vga"; then
            pass "vga-lfb D1: the two documents are BYTE-IDENTICAL"
        else
            fail "vga-lfb D1: the two documents DIFFER"
            cmp "$WORK/lfb-c.vga" "$WORK/lfb-js.vga" 2>&1 | sed 's/^/    /' | head -4
        fi
    fi
fi

echo "### vga-lfb D2: the payload is non-trivial"
d2 "$WORK/lfb-c.vga" > "$WORK/lfb-d2.out" 2>/dev/null || { fail "vga-lfb D2: could not read the document"; }
while read -r kind rest; do
    case "$kind" in
    MODE) [ "$rest" = graphical ] && pass "  the document is a GRAPHICAL one (mode=$rest)" \
          || fail "  the document's mode is $rest, not graphical" ;;
    SVGA)  set -- $rest
           [ "$1" = True ] && [ "$2" = BPP ] && [ "$3" = 32 ] \
               && pass "  svga_enabled=true at bpp 32" \
               || fail "  svga_enabled/bpp = $1/$3, not True/32" ;;
    DIMS) set -- $rest
          [ "$1" = "320x200" ] && [ "$2" = "320x200" ] \
              && pass "  the screen is 320x200 (virtual 320x200)" \
              || fail "  the screen is $1 (virtual $2), not 320x200" ;;
    DISTINCT) [ "$rest" -ge 6 ] \
              && pass "  the payload carries $rest distinct colours" \
              || fail "  only $rest distinct colours (a blank/single-colour screen?)" ;;
    PIX)    set -- $rest
            # $1 = "x:y", $2 = got, $3 = "EXPECT", $4 = expected
            [ "$2" = "$4" ] \
                && pass "  pixel $1 = $2" \
                || fail "  pixel $1 = $2, not $4" ;;
    esac
done < "$WORK/lfb-d2.out"

echo "### vga-lfb D3: each leg twice -- the document is a function of the machine"
"$V86C" tests/vga-lfb.img "$LIMIT" --vga-doc "$WORK/lfb-c2.vga" > /dev/null 2>&1
node oracle/run-vga.mjs tests/vga-lfb.img "$LIMIT" "$WORK/lfb-js2.vga" 2>/dev/null
cmp -s "$WORK/lfb-c.vga" "$WORK/lfb-c2.vga" \
    && pass "  the C leg's two documents are BYTE-IDENTICAL" \
    || fail "  the C leg's two documents differ"
cmp -s "$WORK/lfb-js.vga" "$WORK/lfb-js2.vga" \
    && pass "  the reference leg's two documents are BYTE-IDENTICAL" \
    || fail "  the reference leg's two documents differ"

echo "### vga-lfb D4: the guest's own read-back verdict"
if grep -q 'v86c-vga-doc/1' "$WORK/lfb-c.vga" && grep -q 'v86c-vga-doc/1' "$WORK/lfb-js.vga"; then
    pass "  both legs halted after the image's own read-back"
else
    fail "  a leg produced no document magic (did the guest fail its read-back?)"
fi

echo
echo "### vga-4 gate: $fails FAILING assertion(s)"
[ "$fails" = 0 ]
