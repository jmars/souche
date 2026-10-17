#!/bin/sh
# tests/gate-vga-2.sh -- VGA-U2's gate (HANDOFF=v86c-vga).
#
# WHAT U2 CLAIMS, AND WHAT THIS SCRIPT THEREFORE ASSERTS.  U2 lands mode 13h and the
# graphical document: the sequencer (0x3C4/0x3C5) and graphics-controller (0x3CE/0x3CF)
# register subsets mode 13h needs, the graphical_mode flip through the attribute controller,
# the shift modes 0x40/0x60, vga_replot + vga_redraw (with the reference's dest-buffer clamp
# quirk), the image_data-equivalent dest buffer, set_size_graphical/update_vga_size, and the
# 0xA0000 window branching on graphical_mode.  The image (tests/vga-mode13.asm) self-programs
# 320x200x256, paints a 16-colour ramp plus several pixel-column/row EDGE cells, and reads
# back through read modes 0 and 1 before halting.
#
#   D1  THE TWO-LEG DOCUMENT.  The same image at the same limit, booted by this port and by
#       the pinned reference through our recorder (oracle/run-vga.mjs), produces
#       BYTE-IDENTICAL documents -- the 320x410 RGBA dest buffer, the header's mode/dims and
#       the instruction count all agree.  This is the lane's raison d'etre.
#   D2  THE PAYLOAD IS NON-TRIVIAL.  The differential can be green on a blank framebuffer;
#       this asserts the document actually carries mode="graphical", the 320x200 screen, a
#       MANY-colour palette (>= 12 distinct RGBA values) and the EXACT edge pixels -- so a
#       blank or single-colour screen, a transposed colour lane, or a wrong pixel stride
#       would fail HERE, not silently pass.
#   D3  THE DOCUMENT IS A FUNCTION OF THE MACHINE, not of the run: each leg twice, cmp.
#   D4  THE GUEST'S OWN VERDICT.  The image reads four edge pixels (read mode 0) and a
#       colour-compare run (read mode 1) back and halts only on a match; both legs producing
#       a document already proves the halt, and this re-states it as the gate's precondition.
#
# EXIT CODES ARE THE VERDICT, never the tail of stdout.  Usage:
#     sh tests/gate-vga-2.sh          (the whole gate)

set -u
cd "$(dirname "$0")/.."

V86C=${V86C:-build/v86c}
IMAGE=${IMAGE:-tests/vga-mode13.img}
LIMIT=${LIMIT:-2000000}
WORK=${WORK:-/tmp/gate-vga-2}

fails=0
pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; fails=$((fails + 1)); }

require() {
    [ -x "$V86C" ] || { echo "gate-vga-2: no $V86C (run make)" >&2; exit 2; }
    [ -f "$IMAGE" ] || { echo "gate-vga-2: no $IMAGE (run make tests/vga-mode13.img)" >&2; exit 2; }
    command -v node >/dev/null || { echo "gate-vga-2: no node" >&2; exit 2; }
    [ -f oracle/run-vga.mjs ] || { echo "gate-vga-2: no oracle/run-vga.mjs" >&2; exit 2; }
}

# The payload inspection: a python one-liner that reads a graphical document and reports,
# space-separated:
#   <mode> <screen_width> <screen_height> <virtual_width> <virtual_height> <distinct_colours>
#   <px00_r> <px00_g> <px00_b> <px3190_r> <px3190_g> <px3190_b> <px01_r> <px01_g> <px01_b>
#   <px160100_r> <px160100_g> <px160100_b> <px319199_r> <px319199_g> <px319199_b>
# The gate asserts on these rather than on the bytes' own eyes, so a blank framebuffer, a
# single-colour screen, a transposed colour lane, or a wrong stride is a FAIL, not a reading.
inspect() {
    python3 - "$1" <<'PYEOF'
import json, sys
data = open(sys.argv[1], "rb").read()
nl = data.index(b"\n")
header = json.loads(data[nl + 1 : data.index(b"\n", nl + 1)])
payload = data[data.index(b"\n", nl + 1) + 1 :]
vw, vh = header["virtual_width"], header["virtual_height"]
sw, sh = header["screen_width"], header["screen_height"]
def px(x, y):
    off = (y * vw + x) * 4
    return (payload[off], payload[off + 1], payload[off + 2])
distinct = set(px(x, y) for y in range(sh) for x in range(sw))
e = [px(0, 0), px(sw - 1, 0), px(0, 1), px(160, 100), px(sw - 1, sh - 1)]
vals = []
for t in e:
    vals += [t[0], t[1], t[2]]
print(header["mode"], sw, sh, vw, vh, len(distinct), *vals)
PYEOF
}

doc() {
    echo "### D1: the two-leg document, $IMAGE at $LIMIT instructions"
    if ! "$V86C" "$IMAGE" "$LIMIT" --vga-doc "$WORK/c.vga" > "$WORK/c.transcript" 2> "$WORK/c.err"; then
        fail "the C leg did not halt-and-document (rc nonzero; see $WORK/c.err)"
        return
    fi
    echo "    C leg: $(wc -c < "$WORK/c.vga") bytes, $(sha256sum "$WORK/c.vga" | cut -d' ' -f1)"
    if ! node oracle/run-vga.mjs "$IMAGE" "$LIMIT" "$WORK/js.vga" 2> "$WORK/js.err"; then
        fail "the reference leg did not halt-and-document (see $WORK/js.err)"
        return
    fi
    echo "    JS leg: $(wc -c < "$WORK/js.vga") bytes, $(sha256sum "$WORK/js.vga" | cut -d' ' -f1)"
    if cmp -s "$WORK/c.vga" "$WORK/js.vga"; then
        pass "the two documents are BYTE-IDENTICAL (the U2 differential)"
    else
        fail "the two documents DIFFER"
        cmp "$WORK/c.vga" "$WORK/js.vga" 2>&1 | sed 's/^/    /' | head -4
        return
    fi

    echo "### D2: the payload is non-trivial (a green cmp on a blank framebuffer proves nothing)"
    _m=$(inspect "$WORK/c.vga")
    set -- $_m
    _mode=$1 _sw=$2 _sh=$3 _vw=$4 _vh=$5 _distinct=$6
    _p00=$7,$8,$9 _p3190=${10},${11},${12} _p01=${13},${14},${15}
    _p160100=${16},${17},${18} _p319199=${19},${20},${21}
    [ "$_mode" = graphical ] \
        && pass "the document is a GRAPHICAL one (mode=$_mode)" \
        || fail "the document's mode is $_mode, not graphical"
    [ "$_sw" = 320 ] && [ "$_sh" = 200 ] \
        && pass "the screen is the image's own 320x200" \
        || fail "the screen is ${_sw}x${_sh}, not 320x200"
    [ "$_vw" = 320 ] && [ "$_vh" = 410 ] \
        && pass "the virtual buffer is 320x410 (the reference's own geometry)" \
        || fail "the virtual buffer is ${_vw}x${_vh}, not 320x410"
    [ "$_distinct" -ge 12 ] \
        && pass "the payload carries $_distinct distinct colours (the palette lane is exercised)" \
        || fail "only $_distinct distinct colours (a blank or single-colour screen?)"
    # The exact edge pixels, RGBA through the reference's OWN 6-bit DAC expand
    # (0x1F -> 0x7F, 0x15 -> 0x57, 0x2A -> 0xA8), so a transposed lane or wrong stride fails.
    [ "$_p00" = 127,0,127 ] \
        && pass "pixel (0,0) = purple 127,0,127" \
        || fail "pixel (0,0) = $_p00, not purple 127,0,127"
    [ "$_p3190" = 0,127,127 ] \
        && pass "pixel (319,0) = teal 0,127,127 (the row-0 column edge)" \
        || fail "pixel (319,0) = $_p3190, not teal 0,127,127"
    [ "$_p01" = 255,127,0 ] \
        && pass "pixel (0,1) = orange 255,127,0 (the row edge)" \
        || fail "pixel (0,1) = $_p01, not orange 255,127,0"
    [ "$_p160100" = 87,87,87 ] \
        && pass "pixel (160,100) = dark gray 87,87,87" \
        || fail "pixel (160,100) = $_p160100, not dark gray 87,87,87"
    [ "$_p319199" = 168,168,168 ] \
        && pass "pixel (319,199) = light gray 168,168,168 (the last pixel)" \
        || fail "pixel (319,199) = $_p319199, not light gray 168,168,168"

    echo "### D3: each leg twice -- the document is a function of the machine"
    "$V86C" "$IMAGE" "$LIMIT" --vga-doc "$WORK/c2.vga" > /dev/null 2>&1
    node oracle/run-vga.mjs "$IMAGE" "$LIMIT" "$WORK/js2.vga" 2>/dev/null
    cmp -s "$WORK/c.vga" "$WORK/c2.vga" \
        && pass "the C leg's two documents are BYTE-IDENTICAL" \
        || fail "the C leg's two documents differ"
    cmp -s "$WORK/js.vga" "$WORK/js2.vga" \
        && pass "the reference leg's two documents are BYTE-IDENTICAL" \
        || fail "the reference leg's two documents differ"

    echo "### D4: the guest's own read-back verdict (both documents imply the halt)"
    if grep -q 'v86c-vga-doc/1' "$WORK/c.vga" && grep -q 'v86c-vga-doc/1' "$WORK/js.vga"; then
        pass "both legs halted after the image's own read-mode 0/1 read-back"
    else
        fail "a leg produced no document magic (did the guest fail its read-back?)"
    fi
}

mkdir -p "$WORK"
require
doc

echo
echo "### vga-2 gate: $fails FAILING assertion(s)"
[ "$fails" = 0 ]
