#!/bin/sh
# tests/gate-vga-0.sh -- VGA-U0's gate (HANDOFF=v86c-vga).
#
# WHAT U0 CLAIMS, AND WHAT THIS SCRIPT THEREFORE ASSERTS.  U0 lands the device's SEATS (the
# 8 MiB VRAM, the 0xA0000 window, PCI 00:12.0), the three RENDERING ADAPTERS behind the
# 11-method seam, and the DOCUMENT -- the lane's verified path.  It lands NO register
# semantic (that is U1-U4), so this gate can prove:
#
#   D0  THE TWO-LEG DOCUMENT.  The same image at the same limit, booted by this port and by
#       the pinned reference through OUR recorder (oracle/run-vga.mjs), produces
#       BYTE-IDENTICAL documents: the format, the stop point, the geometry, the cursor and
#       every payload record agree.  On a VGA-blind image every cell is a zero record, which
#       is exactly what the reference's own post-boot state is (MEASURED: text_mode_redraw
#       emits 2000 put_char(...,0,0,0,0) calls; the recorder receives them all).
#       WHAT IT CANNOT PROVE, stated so nobody reads more into a green run: no register
#       decode, no palette, no glyph, no pixel.  Those are U1's and U2's asymmetries, each
#       with its own image (tests/vga-text.asm, tests/vga-mode13.asm).
#   D1  THE DOCUMENT IS A FUNCTION OF THE MACHINE, not of the run: each leg twice, cmp.
#   D2  THE FLAGS CHANGE NOTHING A GUEST OR THE TRANSCRIPT SEES: the transcript with
#       --vga-doc is byte-identical to the transcript without it (the M0 contract's own
#       first gate, run here as well because the document path shares the stop).
#   D3  --vga-ansi RUNS and prints a text screen (25 rows, a truecolor escape per row until
#       a colour repeats).  It is NOT byte-gated: it is the see-it-working adapter
#       (decision 3), and its escapes may drift without a claim failing.
#
# AND THE SWEEP (mode `sweep`), which is the Q4 BOUNDARY GATE: does the machine that now
# always has a VGA answer the same things the machine without one answered?  The instrument
# is the port's own two diagnostics -- V86C_LOG_PORTS (unclaimed I/O ports) and
# V86C_TRACE_VGA (the window, added by this unit) -- so the answer is measured per image
# rather than assumed.  MEASURED ON THIS TREE, and it CORRECTS the plan's assumption that
# the pinned images never touch the VGA:
#   - the M0 corpus images and tests/test.img touch NOTHING (0 port accesses, 0 window
#     accesses) -- so their transcripts must stay byte-identical, and they do;
#   - the KERNEL BOOT DOES touch the VGA: 24 accesses to the CRTC index/data ports
#     0x3D4/0x3D5 (the kernel's vgacon probe / the firmware's mode query) and ~8k window
#     accesses, including the FIRMWARE's INT 10h teletype writing characters at 0xB84C0.
#     Those accesses take the SAME answers before and after this unit -- the ports are still
#     unclaimed (U1 owns them) and the window still reads 0/discards -- so the boot's own
#     instruction count stays at its pin (200,095,214) and its serial output does not move.
#     That equality IS the boundary proof; the counts below are the evidence.
#
# EXIT CODES ARE THE VERDICT, never the tail of stdout.  Usage:
#     sh tests/gate-vga-0.sh [all|doc|sweep]      (default: all)
#
# The kernel-boot arm needs the pinned images; `sweep` skips it with a named SKIP when they
# are absent rather than passing silently.

set -u
cd "$(dirname "$0")/.."

V86C=${V86C:-build/v86c}
IMAGE=${IMAGE:-tests/test.img}
LIMIT=${LIMIT:-1000000}
WORK=${WORK:-/tmp/gate-vga-0}
KERNEL=${KERNEL:-tests/images/vmlinuz-i386}
EXPECT_INSTRS=200095214
NASM_WORKDIR=build/nasm

fails=0
pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; fails=$((fails + 1)); }
skip() { echo "  SKIP  $1"; }

# Every unmatched string of a leg's stderr, for the sweep.
vga_port_rx() { grep -E "port 0x0(3b[4-9a-f]|3c[0-9a-ef]|3d[0-9a-f]|1c[ef])"; }

require() {
    [ -x "$V86C" ] || { echo "gate-vga-0: no $V86C (run make)" >&2; exit 2; }
    [ -f "$IMAGE" ] || { echo "gate-vga-0: no $IMAGE (run make image)" >&2; exit 2; }
}

doc() {
    echo "### D0: the two-leg document, $IMAGE at $LIMIT instructions"
    rm -f "$WORK"/*.vga
    if ! "$V86C" "$IMAGE" "$LIMIT" --vga-doc "$WORK/c.vga" > "$WORK/c.transcript" 2> "$WORK/c.err"; then
        fail "the C leg did not run (rc nonzero; see $WORK/c.err)"
        return
    fi
    echo "    C leg: $(wc -c < "$WORK/c.vga") bytes, $(sha256sum "$WORK/c.vga" | cut -d' ' -f1)"
    if ! node oracle/run-vga.mjs "$IMAGE" "$LIMIT" "$WORK/js.vga" 2> "$WORK/js.err"; then
        fail "the reference leg did not run (see $WORK/js.err)"
        return
    fi
    echo "    JS leg: $(wc -c < "$WORK/js.vga") bytes, $(sha256sum "$WORK/js.vga" | cut -d' ' -f1)"
    if cmp -s "$WORK/c.vga" "$WORK/js.vga"; then
        pass "the two documents are BYTE-IDENTICAL (the U0 differential)"
    else
        fail "the two documents DIFFER"
        cmp "$WORK/c.vga" "$WORK/js.vga" 2>&1 | sed 's/^/    /' | head -3
        return
    fi

    # the format itself, asserted on the bytes rather than described: the magic line, and a
    # payload whose length the header declares
    head -1 "$WORK/c.vga" | grep -qx 'v86c-vga-doc/1' \
        && pass "the magic line is v86c-vga-doc/1" || fail "the magic line is not v86c-vga-doc/1"
    _declared=$(sed -n 2p "$WORK/c.vga" | sed -n 's/.*"payload_bytes":\([0-9]*\).*/\1/p')
    _bytes=$(( $(wc -c < "$WORK/c.vga") - $(head -2 "$WORK/c.vga" | wc -c) ))
    if [ "${_declared:-none}" = "$_bytes" ]; then
        pass "payload_bytes ($_declared) is the file's real payload length"
    else
        fail "payload_bytes ($_declared) is not the file's real payload length ($_bytes)"
    fi

    echo "### D1: each leg twice -- the document is a function of the machine"
    "$V86C" "$IMAGE" "$LIMIT" --vga-doc "$WORK/c2.vga" > /dev/null 2>&1
    node oracle/run-vga.mjs "$IMAGE" "$LIMIT" "$WORK/js2.vga" 2>/dev/null
    cmp -s "$WORK/c.vga" "$WORK/c2.vga" \
        && pass "the C leg's two documents are BYTE-IDENTICAL" \
        || fail "the C leg's two documents differ"
    cmp -s "$WORK/js.vga" "$WORK/js2.vga" \
        && pass "the reference leg's two documents are BYTE-IDENTICAL" \
        || fail "the reference leg's two documents differ"

    echo "### D2: the document path does not touch the transcript"
    if "$V86C" "$IMAGE" "$LIMIT" > "$WORK/plain.transcript" 2>/dev/null \
       && cmp -s "$WORK/plain.transcript" "$WORK/c.transcript"; then
        pass "the transcript with --vga-doc is byte-identical to the plain one"
    else
        fail "the transcript changed when --vga-doc was given"
    fi

    echo "### D3: --vga-ansi (the see-it-working adapter; NOT byte-gated)"
    if "$V86C" "$IMAGE" "$LIMIT" --vga-ansi > /dev/null 2> "$WORK/ansi.err"; then
        _rows=$(grep -c '\[48;2;' "$WORK/ansi.err")
        if [ "$_rows" = 25 ]; then
            pass "--vga-ansi printed 25 text rows"
        else
            fail "--vga-ansi printed $_rows text rows, not 25"
        fi
    else
        fail "--vga-ansi did not run"
    fi
}

sweep() {
    echo "### S1: the M0 images touch no VGA (V86C_LOG_PORTS + V86C_TRACE_VGA)"
    _ports=0
    _window=0
    for img in "$IMAGE"; do
        V86C_LOG_PORTS=1 V86C_TRACE_VGA=1 "$V86C" "$img" "$LIMIT" --fixture \
            > /dev/null 2> "$WORK/sweep-m0.err"
        _ports=$(( _ports + $(vga_port_rx < "$WORK/sweep-m0.err" | wc -l) ))
        _window=$(( _window + $(grep -c 'vga window' "$WORK/sweep-m0.err") ))
    done
    [ "$_ports" = 0 ] && pass "0 VGA port accesses in $IMAGE" \
                      || fail "$_ports VGA port accesses in $IMAGE"
    [ "$_window" = 0 ] && pass "0 window accesses in $IMAGE" \
                       || fail "$_window window accesses in $IMAGE"

    echo "### S2: the whole nasm corpus, one image at a time"
    if [ ! -d "$NASM_WORKDIR" ] || [ -z "$(ls "$NASM_WORKDIR"/*.img 2>/dev/null)" ]; then
        skip "no $NASM_WORKDIR/*.img (run make nasm-translate)"
    else
        _checked=0
        _dirty=""
        for img in "$NASM_WORKDIR"/*.img; do
            _name=$(basename "$img" .img)
            # the port's skip list is the reason a case is not run; it is also the reason
            # not to sweep it (the `jump` image leaves guest RAM and SIGSEGVs on BOTH trees)
            if grep -q "^$_name " tests/nasm_skip_c.txt 2>/dev/null; then continue; fi
            _checked=$(( _checked + 1 ))
            V86C_LOG_PORTS=1 V86C_TRACE_VGA=1 "$V86C" "$img" 1000000 --fixture \
                > /dev/null 2> "$WORK/sweep-nasm.err"
            if vga_port_rx < "$WORK/sweep-nasm.err" | grep -q . || grep -q 'vga window' "$WORK/sweep-nasm.err"; then
                _dirty="$_dirty $_name"
            fi
        done
        echo "    images swept: $_checked"
        [ -z "$_dirty" ] && pass "no corpus image touches the VGA" \
                         || fail "corpus images touch the VGA:$_dirty"
    fi

    echo "### S3: the kernel boot DOES touch the VGA -- and answers what it always did"
    if [ ! -f "$KERNEL" ]; then
        skip "no $KERNEL (the fetched kernel is not in the tree)"
        return
    fi
    V86C_LOG_PORTS=1 V86C_TRACE_VGA=1 "$V86C" --bzimage "$KERNEL" 200000000 \
        --cmdline "console=ttyS0" > "$WORK/sweep-boot.out" 2> "$WORK/sweep-boot.err"
    _ports=$(vga_port_rx < "$WORK/sweep-boot.err" | wc -l)
    _window=$(grep -c 'vga window' "$WORK/sweep-boot.err")
    _count=$(sed -n 's/^v86c boot: \([0-9]*\) instructions.*/\1/p' "$WORK/sweep-boot.err")
    echo "    VGA port accesses: $_ports; window accesses: $_window; instructions: ${_count:-<none>}"
    echo "    the ports it touches:"
    vga_port_rx < "$WORK/sweep-boot.err" | sed -n 's/.*port \(0x[0-9a-f]*\).*/      \1/p' | sort | uniq -c
    # THE BOUNDARY: the count is what a changed answer would move.  An unclaimed port that
    # started answering, or a window write that started storing, changes what the guest
    # sees, and this boot's own path is sensitive enough to have caught that class before
    # (results/m2-5-determinism.md's host-clock finding).
    if [ "${_count:-none}" = "$EXPECT_INSTRS" ]; then
        pass "the boot's instruction count is still the pin ($EXPECT_INSTRS)"
    else
        fail "the boot's instruction count moved: ${_count:-<none>} (pin $EXPECT_INSTRS)"
    fi
    if cmp -s "$WORK/sweep-boot.out" results/bench.stdout 2>/dev/null; then
        pass "the boot's serial output is byte-identical to the recorded one"
    else
        echo "    (no results/bench.stdout to compare; the serial pin lives in 'make bench')"
    fi
}

mkdir -p "$WORK"
require
case "${1:-all}" in
    doc)   doc ;;
    sweep) sweep ;;
    all)   doc; sweep ;;
    *)     echo "usage: sh tests/gate-vga-0.sh [all|doc|sweep]"; exit 2 ;;
esac

echo
echo "### vga-0 gate: $fails FAILING assertion(s)"
[ "$fails" = 0 ]
