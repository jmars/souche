#!/bin/sh
# tests/gate-vga-1.sh -- VGA-U1's gate (HANDOFF=v86c-vga).
#
# WHAT U1 CLAIMS, AND WHAT THIS SCRIPT THEREFORE ASSERTS.  U1 lands the text-mode decode:
# the CRTC (start address, offset, line compare, cursor), the attribute-controller subset,
# the DAC (with the reference's 6-bit expand + byte-lane rotation), dac_map/dac_mask/
# vga256_palette, vga_memory_write_text_mode, text_mode_redraw, put_char's flags+colours,
# set_size_text, and the 0xA0000 window's real handlers.  The image (tests/vga-text.asm)
# self-programs an 8x4 text mode and DRAWS CELLS WITH bg != fg AND DIFFERENT ATTRIBUTES --
# the lane U0's review found UNTESTED (a VGA-blind image has every cell bg=fg=0, so the
# colour lane could be transposed without the differential noticing) -- plus a non-zero
# cursor and a visible cursor scanline, and one BLINKING cell (flags lane).
#
#   D1  THE TWO-LEG DOCUMENT.  The same image at the same limit, booted by this port and by
#       the pinned reference through our recorder (oracle/run-vga.mjs), produces
#       BYTE-IDENTICAL documents -- the grid, the cursor, the scanline, the palette and the
#       instruction count all agree.  This is the lane's raison d'etre.
#   D2  THE PAYLOAD IS NON-TRIVIAL.  The differential can be green on a happy-path image;
#       this asserts the document actually carries real chars, at least one cell with
#       bg != fg, a non-zero cursor, a visible scanline, and one blinking cell -- so a
#       colour-lane or flags-lane transposition would fail HERE, not silently pass.
#   D3  THE DOCUMENT IS A FUNCTION OF THE MACHINE, not of the run: each leg twice, cmp.
#   D4  THE GUEST'S OWN VERDICT.  The image reads two cells back and halts only if the
#       window stored them; both legs producing a document already proves the halt, and this
#       re-states it as the gate's precondition.
#
# EXIT CODES ARE THE VERDICT, never the tail of stdout.  Usage:
#     sh tests/gate-vga-1.sh          (the whole gate)

set -u
cd "$(dirname "$0")/.."

V86C=${V86C:-build/v86c}
IMAGE=${IMAGE:-tests/vga-text.img}
LIMIT=${LIMIT:-1000000}
WORK=${WORK:-/tmp/gate-vga-1}

fails=0
pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; fails=$((fails + 1)); }

require() {
    [ -x "$V86C" ] || { echo "gate-vga-1: no $V86C (run make)" >&2; exit 2; }
    [ -f "$IMAGE" ] || { echo "gate-vga-1: no $IMAGE (run make tests/vga-text.img)" >&2; exit 2; }
    command -v node >/dev/null || { echo "gate-vga-1: no node" >&2; exit 2; }
    [ -f oracle/run-vga.mjs ] || { echo "gate-vga-1: no oracle/run-vga.mjs" >&2; exit 2; }
}

# The payload inspection: a python one-liner that reads a document and reports, space-separated:
#   <cols> <rows> <cursor_row> <cursor_col> <scan_start> <scan_end> <scan_visible> <nchars>
#   <bgneqfg> <blink_cells>
# The gate asserts on these rather than on the bytes' own eyes, so a colour/flags lane
# transposition is a FAIL, not a reading.
inspect() {
    python3 - "$1" <<'PYEOF'
import json, sys
data = open(sys.argv[1], "rb").read()
nl = data.index(b"\n")
header = json.loads(data[nl + 1 : data.index(b"\n", nl + 1)])
payload = data[data.index(b"\n", nl + 1) + 1 :]
cols, rows = header["cols"], header["rows"]
nchars = bgneqfg = blinks = 0
for row in range(rows):
    for col in range(cols):
        off = (row * cols + col) * 10
        chr_ = payload[off]; flags = payload[off + 1]
        bg = int.from_bytes(payload[off + 2 : off + 6], "little")
        fg = int.from_bytes(payload[off + 6 : off + 10], "little")
        if 32 <= chr_ < 127: nchars += 1
        if bg != fg: bgneqfg += 1
        if flags & 0x01: blinks += 1
cs = header["cursor_scanline"]
print(cols, rows, header["cursor_row"], header["cursor_col"],
      cs["start"], cs["end"], "1" if cs["visible"] else "0",
      nchars, bgneqfg, blinks)
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
        pass "the two documents are BYTE-IDENTICAL (the U1 differential)"
    else
        fail "the two documents DIFFER"
        cmp "$WORK/c.vga" "$WORK/js.vga" 2>&1 | sed 's/^/    /' | head -4
        return
    fi

    echo "### D2: the payload is non-trivial (a green cmp on a happy-path image proves nothing)"
    _m=$(inspect "$WORK/c.vga")
    set -- $_m
    _cols=$1 _rows=$2 _crow=$3 _ccol=$4 _sstart=$5 _send=$6 _svis=$7 _nchars=$8 _bgneqfg=$9 _blinks=${10}
    [ "$_cols" = 8 ] && [ "$_rows" = 4 ] \
        && pass "the grid is the image's own 8x4" \
        || fail "the grid is ${_cols}x${_rows}, not 8x4"
    [ "$_ccol" != 0 ] && [ "$_crow" = 0 ] \
        && pass "the cursor is non-zero (row $_crow, col $_ccol)" \
        || fail "the cursor is not the image's non-zero one (row $_crow, col $_ccol)"
    [ "$_sstart" = 0 ] && [ "$_send" = 3 ] && [ "$_svis" = 1 ] \
        && pass "the cursor scanline is 0..3, visible" \
        || fail "the cursor scanline is $_sstart..$_send, visible=$_svis"
    [ "$_nchars" -ge 8 ] \
        && pass "the payload carries $_nchars real chars" \
        || fail "the payload carries $_nchars real chars (a happy-path zero grid?)"
    [ "$_bgneqfg" -ge 8 ] \
        && pass "$_bgneqfg cells have bg != fg (the colour lane is exercised)" \
        || fail "only $_bgneqfg cells have bg != fg (the colour lane is NOT exercised)"
    [ "$_blinks" -ge 1 ] \
        && pass "$_blinks cell blinks (the flags lane is exercised)" \
        || fail "no cell blinks (the flags lane is NOT exercised)"

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
        pass "both legs halted after the image's own window read-back"
    else
        fail "a leg produced no document magic (did the guest fail its read-back?)"
    fi
}

mkdir -p "$WORK"
require
doc

echo
echo "### vga-1 gate: $fails FAILING assertion(s)"
[ "$fails" = 0 ]
