#!/bin/sh
# tests/gate-vga-3.sh -- VGA-U3's gate (HANDOFF=v86c-vga).
#
# WHAT U3 CLAIMS, AND WHAT THIS SCRIPT THEREFORE ASSERTS.  U3 proves the planar 16-colour
# path -- read modes 0/1, write modes 0-3 with rotate/expand/setreset/logical/bitmask,
# chain-2 odd/even, shift modes 0x00/0x20, the latches, odd/even host read, the colorset arm,
# and the row-substitution arm (crtc_mode & 3 != 3) -- with four self-programming images and
# the hw_selftest case 9e probes.  The four images mix read modes 0/1 with write modes 0-3
# and the two shift modes, so a sequence-dependent divergence (R1) cannot hide:
#
#   tests/vga-planar12.img  mode 0x12 (640x480x16, chain-2, shift 0x00): all four write modes,
#                           rotate 1, logical XOR, setreset+enable, bitmask, the latches, read
#                           modes 0 (planes 0-3) + 1, odd/even host read, and -- LAST -- the
#                           colorset arm (attribute_mode 0x80 + color_select 0x05), which
#                           remaps the whole redraw so source colour c -> DAC[0x50+c].
#   tests/vga-planar4.img   packed shift 0x20 (CGA 4-colour interleave) through chain-4.
#   tests/vga-planar0d.img  mode 0x0D (320x200x16, WORD mode: crtc_mode bit 6 clear -> the
#                           addr-to-pixel shift is 2, a different geometry arithmetic).
#   tests/vga-planarsub.img the row-substitution arm (crtc_mode & 3 == 0) across three rows.
#
# For each image:
#   D1  THE TWO-LEG DOCUMENT: same image, same limit, this port vs the pinned reference
#       (oracle/run-vga.mjs) -- byte-identical, header AND payload.
#   D2  THE PAYLOAD IS NON-TRIVIAL: mode/dims/distinct-colour count and specific named pixels
#       (each hand-computed from vga.js's arithmetic), so a blank/single-colour framebuffer or
#       a wrong write-mode/shift/substitution decode FAILS here, not silently passes.
#   D3  THE DOCUMENT IS A FUNCTION OF THE MACHINE: each leg twice, cmp.
#   D4  THE GUEST'S OWN VERDICT: the image reads its plane bytes back in-guest and halts only
#       on a match; both legs producing a document already proves the halt, re-stated here.
#
# EXIT CODES ARE THE VERDICT.  Usage:
#     sh tests/gate-vga-3.sh          (the whole gate, all four images)

set -u
cd "$(dirname "$0")/.."

V86C=${V86C:-build/v86c}
LIMIT=${LIMIT:-2000000}
WORK=${WORK:-/tmp/gate-vga-3}

fails=0
pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; fails=$((fails + 1)); }

require() {
    [ -x "$V86C" ] || { echo "gate-vga-3: no $V86C (run make)" >&2; exit 2; }
    command -v node >/dev/null || { echo "gate-vga-3: no node" >&2; exit 2; }
    [ -f oracle/run-vga.mjs ] || { echo "gate-vga-3: no oracle/run-vga.mjs" >&2; exit 2; }
    for i in planar12 planar4 planar0d planarsub; do
        [ -f "tests/vga-$i.img" ] || { echo "gate-vga-3: no tests/vga-$i.img (run make tests/vga-$i.img)" >&2; exit 2; }
    done
}

# d2(doc, specs, expected...) -- the whole D2 non-triviality check in python, so the shell is
# not doing positional-parameter gymnastics.  specs is "x:y,x:y,...", each expected is "r,g,b".
# Prints "MODE <mode>" / "DIMS <sw>x<sh> <vw>x<vh>" / "DISTINCT <n>" / "PIX <x:y> <r>,<g>,<b>".
d2() {
    python3 - "$@" <<'PYEOF'
import json, sys
doc, specs = sys.argv[1], sys.argv[2].split(",")
expected = [tuple(int(v) for v in e.split(",")) for e in sys.argv[3:]]
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
print("DIMS", f"{sw}x{sh}", f"{vw}x{vh}")
print("DISTINCT", distinct)
for s, e in zip(specs, expected):
    x, y = (int(v) for v in s.split(":"))
    print("PIX", s, f"{px(x,y)[0]},{px(x,y)[1]},{px(x,y)[2]}", "EXPECT", f"{e[0]},{e[1]},{e[2]}")
PYEOF
}

# image-name, the D2 pixel specs ("x:y,..."), the EXPECTED screen size ("WxH"), then expected
# "r,g,b" values.  The expected values are the reference's OWN arithmetic (vga.js),
# hand-verified on the byte-identical document (see results/vga-u3.md).
check_image() {
    img=$1 specs=$2 dims=$3; shift 3
    echo "### $img: the two-leg document, tests/vga-$img.img at $LIMIT instructions"
    if ! "$V86C" "tests/vga-$img.img" "$LIMIT" --vga-doc "$WORK/$img-c.vga" > "$WORK/$img-c.transcript" 2> "$WORK/$img-c.err"; then
        fail "$img: the C leg did not halt-and-document (see $WORK/$img-c.err)"
        return
    fi
    if ! node oracle/run-vga.mjs "tests/vga-$img.img" "$LIMIT" "$WORK/$img-js.vga" 2> "$WORK/$img-js.err"; then
        fail "$img: the reference leg did not halt-and-document (see $WORK/$img-js.err)"
        return
    fi
    echo "    C leg:  $(wc -c < "$WORK/$img-c.vga") bytes, $(sha256sum "$WORK/$img-c.vga" | cut -d' ' -f1)"
    echo "    JS leg: $(wc -c < "$WORK/$img-js.vga") bytes, $(sha256sum "$WORK/$img-js.vga" | cut -d' ' -f1)"
    if cmp -s "$WORK/$img-c.vga" "$WORK/$img-js.vga"; then
        pass "$img D1: the two documents are BYTE-IDENTICAL"
    else
        fail "$img D1: the two documents DIFFER"
        cmp "$WORK/$img-c.vga" "$WORK/$img-js.vga" 2>&1 | sed 's/^/    /' | head -4
        return
    fi

    echo "### $img D2: the payload is non-trivial"
    d2 "$WORK/$img-c.vga" "$specs" "$@" > "$WORK/$img-d2.out"
    while read -r kind rest; do
        case "$kind" in
        MODE) [ "$rest" = graphical ] && pass "  the document is a GRAPHICAL one (mode=$rest)" \
              || fail "  the document's mode is $rest, not graphical" ;;
        DIMS) set -- $rest
              [ "$1" = "$dims" ] \
                  && pass "  the screen is $1 (virtual $2)" \
                  || fail "  the screen is $1 (virtual $2), not $dims" ;;
        DISTINCT) [ "$rest" -ge 3 ] \
              && pass "  the payload carries $rest distinct colours" \
              || fail "  only $rest distinct colours (a blank/single-colour screen?)" ;;
        PIX)    set -- $rest
              # $1 = "x:y", $2 = got, $3 = "EXPECT", $4 = expected
              [ "$2" = "$4" ] \
                  && pass "  pixel ($1) = $2" \
                  || fail "  pixel ($1) = $2, not $4" ;;
        esac
    done < "$WORK/$img-d2.out"

    echo "### $img D3: each leg twice -- the document is a function of the machine"
    "$V86C" "tests/vga-$img.img" "$LIMIT" --vga-doc "$WORK/$img-c2.vga" > /dev/null 2>&1
    node oracle/run-vga.mjs "tests/vga-$img.img" "$LIMIT" "$WORK/$img-js2.vga" 2>/dev/null
    cmp -s "$WORK/$img-c.vga" "$WORK/$img-c2.vga" \
        && pass "  the C leg's two documents are BYTE-IDENTICAL" \
        || fail "  the C leg's two documents differ"
    cmp -s "$WORK/$img-js.vga" "$WORK/$img-js2.vga" \
        && pass "  the reference leg's two documents are BYTE-IDENTICAL" \
        || fail "  the reference leg's two documents differ"

    echo "### $img D4: the guest's own read-back verdict"
    if grep -q 'v86c-vga-doc/1' "$WORK/$img-c.vga" && grep -q 'v86c-vga-doc/1' "$WORK/$img-js.vga"; then
        pass "  both legs halted after the image's own read-back"
    else
        fail "  a leg produced no document magic (did the guest fail its read-back?)"
    fi
}

mkdir -p "$WORK"
require

# planar12: mode 0x12, byte mode.  After the colorset remap (color_select 0x05), source
# colour c -> DAC[0x50+c]; DAC[0x51]=blue, 0x55=green, 0x5A=white, 0x5F=red.
check_image planar12 "0:0,4:0,16:0,68:0" "640x480" \
    "0,255,0" "255,0,0" "0,0,255" "255,255,255"

# planar4: packed shift 0x20.  byte0=0xE4/byte2=0x1B unpack to 3,6,9,12 then zeros.
check_image planar4 "0:0,1:0,2:0,3:0,4:0" "640x200" \
    "0,168,168" "168,87,0" "87,87,255" "255,87,87" "0,0,0"

# planar0d: mode 0x0D, word mode (addr-to-pixel shift 2).  Column B lands in pixels 8-15.
check_image planar0d "0:0,4:0,8:0" "320x200" \
    "168,0,168" "255,255,255" "0,0,168"

# planarsub: the row-substitution arm.  Row 0 -> pixels 0-7, row 1 (bit 13) -> 32-39, row 2
# (bit 14) -> 64-71, each reassembled by vga_replot's substitution.
check_image planarsub "0:0,4:0,32:0,64:0" "32x256" \
    "168,0,168" "255,255,255" "168,0,168" "87,255,87"

echo
echo "### vga-3 gate: $fails FAILING assertion(s)"
[ "$fails" = 0 ]
