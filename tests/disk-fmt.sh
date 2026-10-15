# tests/disk-fmt.sh -- MATERIALISE the 64 MiB test disk on demand (HANDOFF=v86c-repo-weight).
#
# Sourced (never executed) by the m2-5 gates right after their DISK_FMT default:
# tests/images/disk-fmt-64m.img (67,108,864 bytes) is no longer tracked in git --
# it crossed GitHub's 50 MB blob warning -- so the tracked artifact is its xz -9
# (tests/images/disk-fmt-64m.img.xz, 10,240 bytes) and a FRESH CLONE materialises
# the raw file here, on first gate use.
#
# Contract:
#   * only the DEFAULT path is ever written (a caller-supplied DISK_FMT override
#     is never materialised into -- the gate's own missing-input check handles it);
#   * an existing file is never clobbered;
#   * the decompressed bytes are verified against the pin BEFORE they are put in
#     place: written to a temp file, sha256-checked, then renamed -- so a bad or
#     truncated .xz leaves no half-written image behind, and this file exits
#     nonzero (it is sourced at top level, so `exit` fails the gate itself).
#
# The pin is the raw image's sha256, the same value the slice-3 gate pins
# independently (SHA_DISK_FMT in tests/gate-m2-5-guest.sh): the two cannot drift
# without one of them failing loudly.

DISK_FMT_DEFAULT=tests/images/disk-fmt-64m.img
DISK_FMT_XZ="$DISK_FMT_DEFAULT.xz"
SHA_DISK_FMT_DEFAULT=a2f0f6daac00c518727d7bf2981c2f6208dfe93e4973b06ffb2dc6c65ca0460c

disk_fmt_die() {
    echo "disk-fmt: FATAL: $*" >&2
    exit 2
}

DISK_FMT="${DISK_FMT:-$DISK_FMT_DEFAULT}"
if [ "$DISK_FMT" = "$DISK_FMT_DEFAULT" ] && [ ! -f "$DISK_FMT" ]; then
    [ -f "$DISK_FMT_XZ" ] \
        || disk_fmt_die "no $DISK_FMT and no $DISK_FMT_XZ to materialise it from"
    _dfmt_tmp="$DISK_FMT.tmp.$$"
    if ! xz -dc "$DISK_FMT_XZ" > "$_dfmt_tmp"; then
        rm -f "$_dfmt_tmp"
        disk_fmt_die "xz -dc $DISK_FMT_XZ failed"
    fi
    _dfmt_sha=$(sha256sum "$_dfmt_tmp" | cut -d' ' -f1)
    if [ "$_dfmt_sha" != "$SHA_DISK_FMT_DEFAULT" ]; then
        rm -f "$_dfmt_tmp"
        disk_fmt_die "$DISK_FMT_XZ decompressed to sha256 ${_dfmt_sha:-<none>}, expected $SHA_DISK_FMT_DEFAULT -- nothing put in place"
    fi
    mv "$_dfmt_tmp" "$DISK_FMT" \
        || { rm -f "$_dfmt_tmp"; disk_fmt_die "cannot put $DISK_FMT in place"; }
    echo "disk-fmt: materialised $DISK_FMT from $DISK_FMT_XZ (sha256 verified: $_dfmt_sha)"
fi
