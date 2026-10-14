#!/bin/sh
# tests/gate-m2-5-guest.sh -- SLICE 3's gates: THE SHARED IMAGE (HANDOFF=v86guest).
#
# THE ARTIFACT SET: slice 1's i386 kernel + fx-demo's SHELL-FREE initramfs (the
# nobb rebuild: NO busybox, NO fx-core payload -- fx-init's disk path is native
# Zig now, so the guest needs no shell to bring the disk up) + a 64 MiB ATA disk
# image, booted on souche's native CLI (build/v86c) AND on the pinned JS
# reference oracle/run-boot.mjs -- one image, two legs.
#
# WHY THE BUSYBOX IS GONE FROM THE IMAGE (the user's decision, unit nobb, and
# why it was right): fx-init's disk path used to exec /usr/bin/busybox for
# mkfs.ext2/rm/cp/mv/sync (init.zig run_bb), fx-demo's image builder shipped NO
# busybox at all, and no PREBUILT i386 busybox that can be pinned carries the
# mkfs.ext2 applet -- so a busybox was SOURCE-BUILT (kbuild/build-busybox.sh,
# now deleted) with CONFIG_MKFS_EXT2=y and installed with -B.  MEASURED on the
# pinned build (tests/images/busybox-i386-v86guest, kept as provenance): its
# applet set carried sh, ash AND hush -- the image was carrying three shells
# while the project's containment claim said "no sh, no bash, no busybox".  The
# fix is at the root: the operations are NATIVE in fx-init, formatting is a
# BUILD condition (the host mkfs's the FILE image; only MOUNT needs privilege),
# and a BLANK disk now gets a LOUD REFUSAL instead of an in-guest mkfs.  The
# fxsh CONSOLE is deferred the same way: it comes back by installing fxsh
# THROUGH THE SYSTEM DHALL CONFIG as a package (so it gets provides/src_of and
# the oracle can attribute it) -- not by re-adding a host-bindir blob.
#
# THE GATE IS AN ASYMMETRY, on ONE machine, ONE kernel, ONE initramfs:
#
#   ARM A  a PRE-FORMATTED 64 MiB disk   -> mount (no mkfs), NATIVE seed,
#          boot-ok.  THE ONLY SHAPE THAT MOUNTS.
#   ARM B  a BLANK (all-zeros) disk      -> the LOUD PRE-FORMAT REFUSAL: the
#          guest NAMES the device and the build-time contract ("is not a
#          formatted filesystem ... must be pre-formatted at build time"),
#          formats NOTHING, and the boot still reaches its verdict from the
#          ramfs store (fx-init's ensure_disk_store is non-fatal by design).
#          This arm USED to prove IN-GUEST FORMATTING -- the busybox decision's
#          whole point, and the one slice 2 had to record as blocked; that
#          capability was REMOVED BY DECISION, so the arm now asserts the NEW
#          contract with the same structure and the same asymmetry (fx-init's
#          own G5 arm, tests/qemu_disk_probe.sh, is the reference assertion).
#   LEAN   the SAME machine + SAME initrd + fx-demo's pinned LEAN kernel
#          (slice 1's control, no PCI/ATA) -> the ramfs line the browser lane
#          prints today: "fx-init: disk store: no /dev/vda - using ramfs store".
#
# GATE 3b is the two-leg oracle: ARM A on build/v86c and on the pinned JS
# reference, same files, same machine, serials compared after normalization.
#
# THE WRITE->READ-BACK PROOF, and its LIMIT (MEASURED, and it is surprising):
# BOTH legs hold the disk as an IN-MEMORY BUFFER -- the C port read_file()s it
# (src/main.c:903) and the reference passes {buffer: ...} (oracle/run-boot.mjs
# hda, v86 cpu.js:1221-1224) -- and NEITHER flushes it back to the host file.
# MEASURED: tests/images/disk-fmt-64m.img is byte-identical (sha256) after a run
# in which the guest mounted it, seeded ~30 MB into it and synced.  So the write
# path is observable only IN-GUEST, and the gate says exactly that:
#   * ARM A's "disk store mounted (current vN)" line is READ BACK FROM THE DISK
#     (store_current_of(DISK_STORE) after the seed copy), so it proves a
#     write->read-back through the emulated ATA device within the run -- ARM A
#     remains the write->read-back proof;
#   * ARM B is now the NEGATIVE arm: the blank disk is refused BY NAME, no mkfs
#     line ever prints, and the boot still reaches its verdict from the ramfs
#     store (the same shape fx-init's G5 arm asserts);
#   * persistence ACROSS BOOTS is not assertable here and is not claimed.
# The disk's volatility is RECORDED per arm (sha before/after), not asserted: a
# later port adding write-back must not fail this gate.
#
# CONCURRENCY (stated because this gate shares a directory with another lane):
# build/v86c is rebuilt by whoever is editing src/, and its sha256 is printed and
# kept in the evidence.  The gate never runs make; set V86C= to pin a frozen
# binary.  Every disk the emulator is handed is a COPY under $WORK -- a run can
# never mutate tests/images/.
#
# usage: sh tests/gate-m2-5-guest.sh [pin|c|js|armA|armB|lean|diff|all]
#   pin    the sha256 of every input + the archive re-pin check (fast, no boot)
#   c      ARM A, C leg only (the PORT's leg; ~75 s)
#   js     ARM A, reference leg only (the pinned v86: ~20 min -- MEASURED)
#   armA   the pre-formatted disk, BOTH legs (gate 3a + 3b's inputs)
#   armB   the blank disk, the LOUD PRE-FORMAT REFUSAL arm (C leg; see LIMIT_BLANK)
#   lean   the failure/asymmetry arm (the pinned lean kernel, C leg)
#   diff   gate 3b's normalized leg diff (needs armA)
#   all    pin + armA + armB + lean + diff
#
# Raw evidence lands in results/m2-5-guest-*.txt; the write-up is
# results/m2-5-baliveau-guest.md.

set -u
cd "$(dirname "$0")/.."

# Every input path is overridable ONLY so a probe can be run against a
# different artifact (the falsification of a pin, e.g. an image built without
# -B): overriding any of them makes the sha256 pin FAIL, which is the point.
KERNEL="${KERNEL:-tests/images/vmlinuz-i386-v86guest}"
LEAN="${LEAN:-tests/images/vmlinuz-i386-lean-control}"
# THE BOOTED IMAGE: fx-demo's shell-free rebuild (no busybox, no fx-core, the
# native disk path).  This is a RE-PIN (post-nobb): the gate used to boot
# guest-initrd-fxdisk.xz, the busybox+fx-core image whose in-guest mkfs ARM B
# asserted -- that contract is gone, and booting the superseded artifact would
# be a gate passing on bytes nothing ships any more.
INITRD="${INITRD:-tests/images/guest-initrd-shellfree.xz}"
# the SUPERSEDED artifact, FROZEN (souche is not a git repo -- this file is the
# only copy of the old contract's bytes; the pin below keeps it reproducible
# and the busybox-file pins keep ITS provenance attributable).
FROZEN_INITRD="${FROZEN_INITRD:-tests/images/guest-initrd-fxdisk.xz}"
BUSYBOX="${BUSYBOX:-tests/images/busybox-i386-v86guest}"
DISK_FMT="${DISK_FMT:-tests/images/disk-fmt-64m.img}"
FW="${FW:-build/minibios-80m.bin}"
V86C="${V86C:-build/v86c}"
WORK="${WORK:-/tmp/gate-m2-5-guest}"
R=results

MEM=80
# The guest never halts itself (PID1 serves the console forever), so a run ends
# at its INSTRUCTION LIMIT: the limit must clear boot-ok with margin.
# THE INSTRUCTION LIMIT IS A uint32 ON *BOTH* MACHINES, AND ABOVE 2**32-1 IT IS
# SILENTLY UNENFORCEABLE -- MEASURED on each:
#   * the C port rejects the text outright ("instruction limit must be a positive
#     integer, got: 6000000000"), and at exactly 4294967295 it does not stop
#     either (a 600 s run, no stop line, killed by the timeout);
#   * the reference does NOT reject it and never stops: a 6e9 run booted normally
#     (its live console reaches boot-ok) and was still running at 600 s / 1 h 46 m;
#     v86 stores the count in a Uint32Array, so `count >= limit` is unreachable.
# So EVERY limit here is below 2**32-1.  ARM A therefore has ~4.2e9 to work in
# and reaches boot-ok v3 inside 2.9e9.  ARM B used to need the maximum too --
# MEASURED, in-guest mkfs.ext2 under the interpreter fit only inside 4.2e9 --
# but the refusal path formats NOTHING, so the blank-disk boot now runs in ARM
# A's envelope; LIMIT_BLANK stays as the arm's own bound so its wall-clock
# contract does not silently move under it.
LIMIT_C="${LIMIT_C:-4000000000}"
LIMIT_JS="${LIMIT_JS:-4000000000}"
# ARM B's bound.  See the uint32 note above for why it cannot go past 2**32-1.
LIMIT_BLANK="${LIMIT_BLANK:-4200000000}"

# ── pins (every one re-verified before use) ─────────────────────────────────
SHA_KERNEL=f0278a8f2893eb93a8c857f3f2d5a589a5b9aa6a45033171bc5411fc10f658cd
SHA_LEAN=25be0d5358d9445712f6d04e59a7be9133acbd3f69ad0720a69e43d5ba844b3d
# the BOOTED image: fx-demo's shell-free rebuild (829216 bytes; copied from
# fx-demo/web/initrd.xz at the re-pin, 2026-10-14)
SHA_INITRD=df1da2e31ff9fdd66b2d97af68fbd35cc16999e0ee17d53759126d4cf7178665
# the SUPERSEDED image (busybox + fx-core + in-guest mkfs): frozen, not booted
SHA_INITRD_FROZEN=accc806d937260ed5abffafe0530efc701ebaf0383cb22cfc25b2680bf8e4d02
SHA_BUSYBOX=fce38d5b7a492b86d719a77a07b189661132981dac1ef98c8435c087118586e0
SHA_DISK_FMT=a2f0f6daac00c518727d7bf2981c2f6208dfe93e4973b06ffb2dc6c65ca0460c
# 64 MiB of zeros: the BLANK arm's input, and the reason its provenance needs no
# artifact -- truncate(1) reproduces it byte for byte.
SHA_DISK_BLANK=3b6a07d0d404fab4e23b6d34bc6696a6a312dd92821332385e5af7c01c421351
DISK_SIZE=64M

# The store version baked into THIS initrd (the build printed "activated ... as
# version 3" and the guest reads the same number back off the disk; a mismatch
# is a FAIL, so a rebuilt image with a different version fails loudly rather
# than passing on a stale assertion).
V_IMAGE=3

fails=0
pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; fails=$((fails + 1)); }
die()  { echo "gate-m2-5-guest: FATAL: $*" >&2; exit 2; }

# The guest console is a TTY: CRLF on every line (MEASURED).  Every assertion
# reads a CR-stripped transcript (gate-m2-5.sh precedent).
want()     { if tr -d '\r' < "$1" | grep -qE "$2"; then pass "$3"; else fail "$3 (no /$2/ in $1)"; fi; }
want_not() { if tr -d '\r' < "$1" | grep -qE "$2"; then fail "$3 (found /$2/ in $1)"; else pass "$3"; fi; }
assert_eq() { if [ "$1" = "$2" ]; then pass "$3"; else fail "$3 (got '$1' want '$2')"; fi; }

# rdinit= must name the store the ARCHIVE carries, so it is PARSED out of the
# archive, never remembered (BUILD.md §4's re-pin check: exactly one fx-init dir
# in the image, and the same path behind /bin/init).
#
# A GREP IS NOT ENOUGH HERE, and that is MEASURED, not caution: `grep -a
# usr/bin/busybox` on an image built WITHOUT -B matches too, because fx-init's
# own binary embeds the string "/usr/bin/busybox" (run_bb's execv path) -- the
# count was 1 for the image with no busybox in it and 2 with it.  So the cpio
# newc headers are WALKED (both concatenated segments) and the busybox is
# identified by its NAME AND THE SHA256 OF ITS CONTENT, and the store dirs by
# their ENTRY NAMES.  Prints, one fact per line:
#   store_dirs N / store_dir P / init_link T / busybox_sha S / busybox_size N
#   / fxsh_sha S / fxsh_size N / fxcore_entries N
archive_probe() {
    python3 - "$INITRD" <<'PYARCH'
import hashlib, lzma, sys

data = lzma.open(sys.argv[1], "rb").read()
off = 0
store_dirs = {}
init_link = set()
busybox = []
fxsh = []
fxcore = 0
while True:
    i = data.find(b"070701", off)
    if i < 0 or i + 110 > len(data):
        break
    off = i + 6
    hdr = data[i:i + 110]
    try:
        fs = [int(hdr[6 + 8 * k:6 + 8 * (k + 1)], 16) for k in range(13)]
    except ValueError:
        continue
    size, namesize = fs[6], fs[11]
    if namesize == 0 or namesize > 256 or size > 64 << 20:
        continue
    name = data[i + 110:i + 110 + namesize - 1]
    body = (i + 110 + namesize + 3) & ~3
    if not name:
        continue
    nm = name.decode("utf-8", "replace")
    base = nm.lstrip("./")
    if "-fx-init" in nm:
        prefix = nm.lstrip("/").split("-fx-init")[0]
        store_dirs[prefix + "-fx-init"] = True
    if base in ("bin/init",):
        init_link.add(data[body:body + size].decode("utf-8", "replace"))
    if base == "usr/bin/busybox":
        busybox.append((size, hashlib.sha256(data[body:body + size]).hexdigest()))
    if base.split("/")[-1] == "fxsh":
        fxsh.append((size, hashlib.sha256(data[body:body + size]).hexdigest()))
    if base.startswith("usr/fx-core/"):
        fxcore += 1

print("store_dirs", len(store_dirs))
for p in sorted(store_dirs):
    print("store_dir /" + p)
for t in sorted(init_link):
    print("init_link", t)
if busybox:
    for size, sha in busybox:
        print("busybox_sha", sha)
        print("busybox_size", size)
else:
    print("busybox_sha ABSENT")
    print("busybox_size 0")
if fxsh:
    for size, sha in fxsh:
        print("fxsh_sha", sha)
        print("fxsh_size", size)
else:
    print("fxsh_sha ABSENT")
    print("fxsh_size 0")
print("fxcore_entries", fxcore)
PYARCH
}

store_path() { archive_probe | sed -n 's|^store_dir ||p'; }
CMDNAME=""

pin_check() {
    echo "### PINS (the inputs this gate is a claim about)"
    for pair in "$KERNEL $SHA_KERNEL" "$LEAN $SHA_LEAN" "$INITRD $SHA_INITRD" \
                "$BUSYBOX $SHA_BUSYBOX" "$DISK_FMT $SHA_DISK_FMT"; do
        set -- $pair
        [ -f "$1" ] || die "missing input: $1"
        got=$(sha256sum "$1" | cut -d' ' -f1)
        if [ "$got" = "$2" ]; then pass "$1 sha256 $got"; else fail "$1 sha256 $got != $2"; fi
    done
    # the SUPERSEDED artifact: pinned, frozen, and NOT booted by this gate any
    # more (it is the busybox+fx-core image whose in-guest mkfs ARM B used to
    # assert).  souche is not a git repo -- this pin is the old contract's only
    # reproducibility.
    if [ -f "$FROZEN_INITRD" ]; then
        got=$(sha256sum "$FROZEN_INITRD" | cut -d' ' -f1)
        if [ "$got" = "$SHA_INITRD_FROZEN" ]; then pass "$FROZEN_INITRD (SUPERSEDED, frozen, not booted) sha256 $got"; else fail "$FROZEN_INITRD sha256 $got != $SHA_INITRD_FROZEN"; fi
    else
        fail "the frozen superseded image is gone: $FROZEN_INITRD (the old contract loses its artifact)"
    fi
    got=$(truncate -s $DISK_SIZE "$WORK/blank-probe.img" 2>/dev/null && sha256sum "$WORK/blank-probe.img" | cut -d' ' -f1)
    if [ "$got" = "$SHA_DISK_BLANK" ]; then pass "a blank $DISK_SIZE image has sha256 $got"; else fail "blank image sha256 $got != $SHA_DISK_BLANK"; fi
    [ -x "$V86C" ] || die "no $V86C (make; set V86C= to pin a frozen binary)"
    pass "$V86C present, sha256 $(sha256sum "$V86C" | cut -c1-64)"
    [ -f "$FW" ] || die "no $FW (make minibios-mem)"
    pass "$FW present, sha256 $(sha256sum "$FW" | cut -c1-64)"

    echo "### THE ARCHIVE RE-PIN CHECK (the cmdline names the store the image carries)"
    ARCHIVE=$(archive_probe)
    RD_LINES=$(store_path)
    # -w, not -l: the parsed list is one line with no trailing newline
    assert_eq "$(printf '%s' "$RD_LINES" | wc -w)" "1" "exactly ONE fx-init store dir in the image"
    assert_eq "$RD_LINES" "$STORE_DIR" "the store dir is the pinned one ($STORE_DIR)"
    # SHELL-FREE BY DECISION: the image must carry NO busybox, NO fxsh, and NO
    # fx-core payload.  (The pinned BUSYBOX *file* below is the OLD image's
    # artifact, kept as provenance -- it is not in this image any more.)
    assert_eq "$(printf '%s' "$ARCHIVE" | sed -n 's/^busybox_sha //p')" "ABSENT" \
        "the image carries NO busybox entry (shell-free by decision; fx-demo's harness/check-shell-free.mjs is the walking gate)"
    assert_eq "$(printf '%s' "$ARCHIVE" | sed -n 's/^busybox_size //p')" "0" \
        "and therefore no busybox size"
    assert_eq "$(printf '%s' "$ARCHIVE" | sed -n 's/^fxsh_sha //p')" "ABSENT" \
        "the image carries NO fxsh (the terminal is DEFERRED: it returns by installing fxsh THROUGH THE SYSTEM DHALL CONFIG as a package -- provides/src_of, oracle-attributable)"
    assert_eq "$(printf '%s' "$ARCHIVE" | sed -n 's/^fxcore_entries //p')" "0" \
        "NO usr/fx-core payload (the un-provenanced host-bindir blob is out)"
    # /bin/init, the path the GUEST resolves, points into the same store dir
    assert_eq "$(printf '%s' "$ARCHIVE" | sed -n 's|^init_link ||p')" "$STORE_DIR/fx-init" \
        "/bin/init -> the store dir the cmdline names"
    # the console row in the arms branches on the SAME fact, decided here
    HAVE_FXSH=0
    [ "$(printf '%s' "$ARCHIVE" | sed -n 's/^fxsh_sha //p')" != "ABSENT" ] && HAVE_FXSH=1

    echo "### THE PINNED BUSYBOX FILE (the OLD image's artifact, frozen as provenance --"
    echo "###  the image above no longer carries it; these rows keep ITS provenance honest)"
    case "$(file -b "$BUSYBOX")" in
        *"ELF 32-bit"*i386*"statically linked"*) pass "static elf_i386: $(file -b "$BUSYBOX")" ;;
        *) fail "$BUSYBOX is not a static i386 ELF: $(file -b "$BUSYBOX")" ;;
    esac
    BB_LIST="$("$BUSYBOX" --list)"
    for applet in mkfs.ext2 rm cp mv sync; do
        if printf '%s\n' "$BB_LIST" | grep -qx "$applet"; then
            pass "applet $applet present"
        else
            fail "applet $applet MISSING from $BUSYBOX"
        fi
    done

    echo "### THE DISKS"
    assert_eq "$(stat -c %s "$DISK_FMT")" "67108864" "the formatted disk is 64 MiB"
    if python3 - "$DISK_FMT" <<'EOF'
import sys
d = open(sys.argv[1], 'rb').read(2048)
sys.exit(0 if int.from_bytes(d[1080:1082], 'little') == 0xef53 else 1)
EOF
    then pass "the formatted disk carries an ext2/ext4 superblock (0xef53 at 1024+56)"; else fail "no ext2 magic in $DISK_FMT"; fi
    # and it is NOT blank, which is the other half of ARM A's provenance
    if [ "$(head -c 2048 "$DISK_FMT" | sha256sum | cut -d' ' -f1)" = "$(head -c 2048 /dev/zero | sha256sum | cut -d' ' -f1)" ]; then
        fail "the 'formatted' disk's first 2 KiB is all zeros"
    else
        pass "the formatted disk is not all zeros (formatted by the pinned busybox's own mkfs.ext2)"
    fi
}

# ── the arms ────────────────────────────────────────────────────────────────
# Every disk handed to an emulator is a COPY under $WORK: a run must never
# mutate tests/images/, and both legs of ARM A must start from the same bytes.
disk_copy() { # disk_copy SRC NAME -> echoes the path
    cp "$1" "$WORK/$2" || die "cannot copy $1"
    echo "$WORK/$2"
}

leg_c() { # leg_c NAME DISK LIMIT
    _name=$1; _disk=$2; _limit=$3
    echo "\$ $V86C --bzimage $KERNEL $_limit --cmdline '$CMD' --hda $_disk \\"
    echo "      --initrd $INITRD --bios $FW --memory $MEM --serial $R/m2-5-guest-$_name-serial.txt"
    _before=$(sha256sum "$_disk" | cut -d' ' -f1)
    timeout 5400 "$V86C" --bzimage "$KERNEL" "$_limit" --cmdline "$CMD" --hda "$_disk" \
        --initrd "$INITRD" --bios "$FW" --memory "$MEM" \
        --virtio-console "$R/m2-5-guest-$_name-virtio.txt" \
        --serial "$R/m2-5-guest-$_name-serial.txt" 2> "$R/m2-5-guest-$_name-stderr.txt"
    echo "port rc=$? (a run ends at its instruction LIMIT: PID1 never halts)"
    grep -E "instructions|hda .* sha256|virtio console" "$R/m2-5-guest-$_name-stderr.txt" | sed 's/^/    /'
    _after=$(sha256sum "$_disk" | cut -d' ' -f1)
    echo "  disk sha256 before=$_before after=$_after"
    if [ "$_before" = "$_after" ]; then
        echo "  NOTE (MEASURED, both legs): the emulator held the disk in memory and did not flush it"
        echo "       back to this file -- so write->read-back is observable IN-GUEST only (see the header)."
    fi
}

leg_js() { # leg_js NAME DISK LIMIT
    _name=$1; _disk=$2; _limit=$3
    echo "\$ node oracle/run-boot.mjs $KERNEL $FW $_limit '$CMD' --hda $_disk \\"
    echo "      --initrd $INITRD --memory $MEM --virtio-console $R/m2-5-guest-$_name-virtio.txt"
    _before=$(sha256sum "$_disk" | cut -d' ' -f1)
    # TIMEOUT_JS is generous because the reference leg is the slow one (~50x the
    # port): MEASURED, 3.6e9 instructions take ~80 s here, but a 6e9 limit never
    # finishes at all (see the uint32 note above), and run-boot.mjs writes the
    # serial to stdout only at the END -- a killed run leaves an EMPTY transcript,
    # which is exactly what an over-limit run looks like.  Set V86C_BOOT_TRACE=1
    # (or 2, which adds a sample every 10M instructions) to watch it live on
    # stderr; that is how the shot above was diagnosed.
    timeout "${TIMEOUT_JS:-14400}" node oracle/run-boot.mjs "$KERNEL" "$FW" "$_limit" "$CMD" --hda "$_disk" \
        --initrd "$INITRD" --memory "$MEM" \
        --virtio-console "$R/m2-5-guest-$_name-virtio.txt" \
        > "$R/m2-5-guest-$_name-serial.txt" 2> "$R/m2-5-guest-$_name-stderr.txt"
    echo "reference rc=$?"
    grep -E "instructions|hda .* sha256|stop " "$R/m2-5-guest-$_name-stderr.txt" | tail -3 | sed 's/^/    /'
    _after=$(sha256sum "$_disk" | cut -d' ' -f1)
    echo "  disk sha256 before=$_before after=$_after"
    [ "$_before" = "$_after" ] && echo "  NOTE: in-memory disk, not flushed (see the header)."
}

# The assertions both legs of ARM A must satisfy (the plan's gate 3a list, plus
# the two lines that make the mount a MEASUREMENT rather than a printout).
assert_preformatted() { # assert_preformatted NAME DISK_SHA
    _s=$R/m2-5-guest-$1-serial.txt
    want "$_s" 'ata1\.00: ATA-' "libata identified the disk (ata1.00)"
    want "$_s" 'sd 0:0:0:0: \[sda\] 131072 512-byte logical blocks' "the 64 MiB ATA disk (131072 sectors)"
    want "$_s" "Run $STORE_DIR/fx-init as init process" "the kernel exec'd the store's fx-init (rdinit resolved)"
    want "$_s" 'fx-init: store from kernel command line fx.store=/fx/store' "fx-init read its store root from the cmdline"
    want "$_s" 'EXT4-fs \(sda\): mounted filesystem' "the guest MOUNTED the ATA disk (the kernel's own line)"
    want "$_s" "fx-init: disk store mounted \(current v$V_IMAGE\)" "fx-init's disk-store verdict, read back from the disk"
    want "$_s" 'fx-init: pivoted to tmpfs root' "the pivot ran (the boot chain continued past the disk)"
    # the console service: CONDITIONAL on the image, never silent.  With fxsh in
    # the image this asserts the prompt exactly as before; without it (the
    # shell-free image -- fxsh is DEFERRED), it is a NAMED skip, not a fail and
    # not a deletion: the day fxsh comes back THROUGH THE SYSTEM DHALL CONFIG
    # (as a package -- provides/src_of, oracle-attributable), the row asserts
    # again with no edit here.
    if [ "${HAVE_FXSH:-0}" = 1 ]; then
        want "$_s" 'fx> ' "the console service came up (dhake materialized /bin, silently)"
    else
        echo "  SKIP  (terminal deferred) the console prompt -- the image carries no fxsh;"
        echo "        restore it by installing fxsh THROUGH THE SYSTEM DHALL CONFIG as a package"
        echo "        (provides/src_of, oracle-attributable), rebuild, and this row asserts again."
    fi
    want "$_s" "fx-init: boot-ok v$V_IMAGE" "THE BOOT VERDICT"
    want_not "$_s" 'fx-init: disk store: no /dev/vda' "the ramfs fallback did NOT fire"
    want_not "$_s" 'mkfs\.ext2 FAILED' "no mkfs failure"
    want_not "$_s" 'current v0' "the disk store was not silently re-seeded"
    want_not "$_s" 'Kernel panic' "no kernel panic"
    # the same lines seen a second time, through the virtio console (the shared
    # kernel's other device: `console=hvc0 console=ttyS0` registers both, and
    # hvc0 receives the kernel log from the point it is registered -- which is
    # AFTER the "legacy console [hvc0] enabled" line itself, so that line is NOT
    # in this stream (MEASURED).  fx-init's control channel scans /dev/vport*p*
    # and is not wired to hvc0 -- a separate seam, named not opened here.)
    _v=$R/m2-5-guest-$1-virtio.txt
    want "$_v" 'sd 0:0:0:0: \[sda\]' "the sda line came out of the virtio console"
    want "$_v" 'EXT4-fs \(sda\): mounted filesystem' "the mount line came out of the virtio console"
    # the inputs really were the pinned bytes (both legs print the disk sha)
    want "$R/m2-5-guest-$1-stderr.txt" "hda .*sha256 $2" "the leg's disk input is the pinned artifact"
}

gate_armA_c() {
    echo "### GATE 3a, ARM A: a PRE-FORMATTED 64 MiB disk (C leg, build/v86c)"
    _c=$(disk_copy "$DISK_FMT" armA-c-disk.img)
    leg_c armA-c "$_c" "$LIMIT_C"
    echo "--- assertions (C leg)"
    assert_preformatted armA-c "$SHA_DISK_FMT"
}

gate_armA_js() {
    echo "### GATE 3a, ARM A, REFERENCE LEG: the pinned v86 (oracle/run-boot.mjs)"
    _j=$(disk_copy "$DISK_FMT" armA-js-disk.img)
    leg_js armA-js "$_j" "$LIMIT_JS"
    echo "--- assertions (reference leg)"
    assert_preformatted armA-js "$SHA_DISK_FMT"
    echo "--- the reference's stop line"
    tail -4 "$R/m2-5-guest-armA-js-stderr.txt" | sed 's/^/    /'
}

gate_armA() { gate_armA_c; gate_armA_js; }

gate_armB() {
    echo "### GATE 3a, ARM B: a BLANK disk -- the LOUD PRE-FORMAT REFUSAL (the NEW contract)"
    echo "### (this arm USED to prove IN-GUEST FORMATTING: the guest's busybox mkfs.ext2"
    echo "###  wrote a filesystem into the zeros and the kernel mounted it.  That capability"
    echo "###  was REMOVED BY DECISION -- the disk is PRE-FORMATTED at build time and fx-init"
    echo "###  REFUSES a blank one, by name.  Same structure, same asymmetry: what changed is"
    echo "###  what the arm asserts.  fx-init's own G5 arm (tests/qemu_disk_probe.sh) is the"
    echo "###  reference assertion.)"
    truncate -s $DISK_SIZE "$WORK/armB-c-disk.img" || die "cannot make the blank disk"
    _b="$WORK/armB-c-disk.img"
    assert_eq "$(sha256sum "$_b" | cut -d' ' -f1)" "$SHA_DISK_BLANK" "the ARM B input is $DISK_SIZE of zeros"
    leg_c armB-c "$_b" "$LIMIT_BLANK"
    _s=$R/m2-5-guest-armB-c-serial.txt
    echo "--- assertions (the refusal, by name; then the verdict from the ramfs store)"
    want "$_s" 'ata1\.00: ATA-' "libata identified the blank disk"
    want "$_s" 'sd 0:0:0:0: \[sda\] 131072 512-byte logical blocks' "the blank disk's geometry (131072 sectors)"
    want "$_s" "Run $STORE_DIR/fx-init as init process" "the store's fx-init ran"
    # the blank disk is NOT a filesystem: the mount fails, and that failure is
    # exactly what the refusal answers (this line used to trigger the mkfs)
    want "$_s" "EXT4-fs \(sda\): VFS: Can't find ext4 filesystem" "the blank disk rejected the mount attempt"
    # THE NEW CONTRACT, the two halves of the loud refusal (init.zig:2501):
    want "$_s" 'is not a formatted filesystem' "THE REFUSAL names the device"
    want "$_s" 'must be pre-formatted at build time' "THE REFUSAL names the pre-format contract (a BUILD condition, not a guest one)"
    # the refusal is NON-FATAL by design (ensure_disk_store): the boot continues
    # from the ramfs store and still reaches its verdict
    want "$_s" "fx-init: boot-ok v$V_IMAGE" "THE BOOT VERDICT still lands (the boot continues from the ramfs store)"
    want_not "$_s" "fx-init: disk store mounted" "the blank disk was NOT mounted (no store on it)"
    want_not "$_s" 'mkfs' "NOTHING was formatted in-guest (the removed capability stays removed)"
    want_not "$_s" 'inodes, 65536 blocks' "no busybox mkfs.ext2 output (the OLD contract's headline line, now its own negative)"
    want_not "$_s" 'fx-init: disk store: no /dev/vda' "not the silent no-disk fallback either: the device IS there and was REFUSED, not missed"
    want_not "$_s" 'Kernel panic' "no kernel panic"
    # The disk's content is volatile (in-memory buffer, see the header), and the
    # host-side sha cannot distinguish "untouched" from "not flushed" -- so the
    # no-format proof is IN-GUEST: no mkfs line, no mount line, and the refusal
    # naming the device, all above.
    echo "--- the arm's own stop line (the instruction count it needed)"
    grep -E "instructions" "$R/m2-5-guest-armB-c-stderr.txt" | sed 's/^/    /'
}

gate_lean() {
    echo "### THE ASYMMETRY: the SAME machine, SAME initrd, SAME disk, the PINNED LEAN KERNEL"
    echo "### (fx-demo's web/bzImage -- no PCI, no ATA: it cannot see a disk at all)"
    _c=$(disk_copy "$DISK_FMT" lean-c-disk.img)
    echo "\$ $V86C --bzimage $LEAN $LIMIT_C --cmdline '$CMD' --hda $_c --initrd $INITRD ..."
    timeout 5400 "$V86C" --bzimage "$LEAN" "$LIMIT_C" --cmdline "$CMD" --hda "$_c" \
        --initrd "$INITRD" --bios "$FW" --memory "$MEM" \
        --serial "$R/m2-5-guest-lean-c-serial.txt" 2> "$R/m2-5-guest-lean-c-stderr.txt"
    echo "port rc=$?"
    _s=$R/m2-5-guest-lean-c-serial.txt
    want_not "$_s" 'ata1\.00: ATA-' "no libata (no ATA driver in this kernel)"
    want_not "$_s" 'sd 0:0:0:0: \[sda\]' "no ATA disk"
    want_not "$_s" 'EXT4-fs \(sda\)' "no ext4 mount over an ATA disk"
    want "$_s" 'fx-init: disk store: no /dev/vda . using ramfs store' "THE PINNED PRE-FIX LINE (byte-for-byte the browser lane's)"
    want "$_s" 'fx-init: boot-ok v' "and the same image still boots (the negative is a measurement)"
    want_not "$_s" "fx-init: disk store mounted" "no disk store on the control kernel"
    # the same init ran: the kernel's own "Run ... as init process" line
    want "$_s" "Run $STORE_DIR/fx-init as init process" "the same initrd's fx-init ran (the control is not silence)"
}

# ── gate 3b: the two legs, normalized ───────────────────────────────────────
gate_diff() {
    echo "### GATE 3b: the two legs' transcripts, normalized (m2-2's timestamp strip)"
    _c=$R/m2-5-guest-armA-c-serial.txt
    _j=$R/m2-5-guest-armA-js-serial.txt
    [ -s "$_c" ] && [ -s "$_j" ] || die "gate 3b needs armA's two serials (run armA first)"
    # m2-2's normalization: strip the "[    0.000000] " monotonic prefixes.  The
    # ext4 mount line carries a RANDOM uuid (mkfs generates it), so the uuid is
    # masked too: it is not a leg difference, it is a per-run value on one image.
    norm() {
        tr -d '\r' < "$1" | sed 's/^\[ *[0-9.]*\]//' \
            | sed 's/[0-9a-f]\{8\}-[0-9a-f]\{4\}-[0-9a-f]\{4\}-[0-9a-f]\{4\}-[0-9a-f]\{12\}/<uuid>/g'
    }
    norm "$_j" > "$R/m2-5-guest-armA-js.norm.txt"
    norm "$_c" > "$R/m2-5-guest-armA-c.norm.txt"
    diff "$R/m2-5-guest-armA-js.norm.txt" "$R/m2-5-guest-armA-c.norm.txt" \
        > "$R/m2-5-guest-leg-diff.txt"
    echo "the full leg diff: $(wc -l < "$R/m2-5-guest-leg-diff.txt") lines (kept as the measurement)"
    # THE CLAIM 3b MAKES: the DISK/STORE-RELEVANT lines are identical on both
    # legs.  Extracted by the same patterns on both sides, so a line that only
    # one leg has shows up as a difference here.
    devlines() {
        tr -d '\r' < "$1" | sed 's/^\[ *[0-9.]*\]//' \
            | sed 's/[0-9a-f]\{8\}-[0-9a-f]\{4\}-[0-9a-f]\{4\}-[0-9a-f]\{4\}-[0-9a-f]\{12\}/<uuid>/g' \
            | grep -E 'ata[0-9]|scsi [0-9]|sd 0:0|\[sda\]|EXT4-fs|fx-init: (store|disk|boot-ok|pivoted|boot start)|Run /fx/store|fx> ' \
            | sed 's/  */ /g'
    }
    devlines "$_j" > "$R/m2-5-guest-armA-js.dev.txt"
    devlines "$_c" > "$R/m2-5-guest-armA-c.dev.txt"
    # ORDERED diff: kept as the measurement, not as the assertion.  MEASURED, it
    # is 2 pairs of printk lines emitted in a different order (vendors' drivers
    # printk at bring-up from different contexts) -- an interleaving difference,
    # not a content one.  The ASSERTION is therefore over the MULTISET (sorted,
    # with counts), which cannot be satisfied by reordering and still catches a
    # line that only one leg has.
    sort "$R/m2-5-guest-armA-js.dev.txt" | uniq -c > "$R/m2-5-guest-armA-js.dev.multi.txt"
    sort "$R/m2-5-guest-armA-c.dev.txt"  | uniq -c > "$R/m2-5-guest-armA-c.dev.multi.txt"
    if diff -u "$R/m2-5-guest-armA-js.dev.txt" "$R/m2-5-guest-armA-c.dev.txt" \
            > "$R/m2-5-guest-armA-dev-diff.txt"; then
        pass "the device/store lines are IDENTICAL in order too ($(wc -l < "$R/m2-5-guest-armA-c.dev.txt") lines)"
    else
        echo "  NOTE  the ordered diff is $(wc -l < "$R/m2-5-guest-armA-dev-diff.txt") lines;"
        echo "        MEASURED: only printk ORDER, no line is missing on either leg (see the multiset check)"
    fi
    if diff -u "$R/m2-5-guest-armA-js.dev.multi.txt" "$R/m2-5-guest-armA-c.dev.multi.txt" \
            > "$R/m2-5-guest-armA-dev-multi-diff.txt"; then
        pass "the device/store lines are the same MULTISET on both legs ($(grep -c . "$R/m2-5-guest-armA-c.dev.txt") lines)"
    else
        fail "the device/store lines differ as a multiset (see $R/m2-5-guest-armA-dev-multi-diff.txt)"
        sed 's/^/    /' "$R/m2-5-guest-armA-dev-multi-diff.txt" | head -20
    fi
    echo "--- the device/store lines, both legs (C leg; the JS leg's are the same multiset)"
    sed 's/^/    /' "$R/m2-5-guest-armA-c.dev.txt"
    echo "--- and the ONE thing the ordered diff does show (no line is missing):"
    sed 's/^/    /' "$R/m2-5-guest-armA-dev-diff.txt"
}

# ── the store dir + the cmdline, resolved BEFORE anything runs ──────────────
[ -f "$INITRD" ] || die "no $INITRD (fx-demo's shell-free rebuild; the frozen guest-initrd-fxdisk.xz is the SUPERSEDED artifact, kept but not booted)"
STORE_DIR=$(store_path)
[ -n "$STORE_DIR" ] || die "no /fx/store/<hash>-fx-init in $INITRD"
# The kernel command line.  idle=poll is NOT optional (slice 1, re-measured on
# IDENTICAL cmdlines: tsc=unstable alone carries the C leg and NOT the reference
# leg, because both machines stop at an idle HLT mid-boot and this harness's M0
# stop condition ends the run there).  console=hvc0 FIRST so ttyS0 stays
# /dev/console (the LAST console= wins), which is where fx-init writes.
CMD="console=hvc0 console=ttyS0 tsc=unstable idle=poll rdinit=$STORE_DIR/fx-init fx.store=/fx/store"

mkdir -p "$WORK" "$R"

case "${1:-all}" in
    pin)   pin_check ;;
    c)     pin_check; gate_armA_c ;;
    js)    pin_check; gate_armA_js ;;
    armA)  pin_check; gate_armA ;;
    armB)  pin_check; gate_armB ;;
    lean)  pin_check; gate_lean ;;
    diff)  gate_diff ;;
    all)   pin_check; gate_armA; gate_armB; gate_lean; gate_diff ;;
    *)     echo "usage: sh tests/gate-m2-5-guest.sh [pin|armA|armB|lean|diff|all]"; exit 2 ;;
esac

echo
echo "### m2-5 slice-3 gate: $fails FAILING assertion(s)"
[ "$fails" = 0 ]
