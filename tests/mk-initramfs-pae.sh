#!/bin/sh
# mk-initramfs-pae.sh -- PAE-L3's initramfs (tests/images/guest-initramfs-pae.img).
#
# THE SMALLEST USERSPACE that can answer the acceptance question for a real PAE boot:
# an init that cats /proc/cpuinfo's flags line -- the guest's OWN evidence that the CPU
# advertises `pae` (and `nx`) -- and then powers off, so the run ends at a deterministic
# halt instead of the instruction limit.  It prints no host-derived value.
#
# THE TREE IS THE TREE'S OWN WORKING REBOOT IMAGE (tests/images/guest-initramfs-reboot.img,
# extracted) with ./init REPLACED -- not a from-scratch busybox archive, for the reason
# mk-initramfs-acpi.sh records (two from-scratch attempts were MEASURED dead on arrival
# on this exact kernel+machine).  The reboot image's tree is the one this guest
# demonstrably runs, so the delta here is exactly one file: ./init.
#
# WHY THE INIT READS /proc/cpuinfo AND NOTHING ELSE DECISIVE: "NX (Execute Disable)
# protection: active" in the kernel's own boot log is the DECISIVE PAE evidence (that
# string exists only in the CONFIG_X86_PAE=y branch of arch/x86/kernel/setup.c; the
# non-PAE branch prints its inverse "cannot be enabled: non-PAE kernel!").  The cpuinfo
# flags line is the SECOND, guest-readable signal -- the flags the kernel derived from
# CPUID -- and reaching it is also the proof userspace ran.  The gate asserts both.
set -e
cd "$(dirname "$0")/.."
OUT="$(pwd)/tests/images/guest-initramfs-pae.img"
SRC=tests/images/guest-initramfs-reboot.img
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
zcat "$SRC" | cpio -idm --quiet -D "$TMP"
cat > "$TMP/init" <<'EOF'
#!/bin/sh
# PAE-L3's init: the reboot image's own prologue (this archive IS that image, minus its
# disk-work block) plus the smallest userspace that answers the acceptance question.
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mount -t proc none /proc 2>/dev/null
mount -t sysfs none /sys 2>/dev/null
mount -t devtmpfs none /dev 2>/dev/null
echo "PAE-INIT: begin"
echo "PAE-INIT: uname = $(uname -r)"
echo "PAE-INIT: cpuinfo flags: $(grep -m1 '^flags' /proc/cpuinfo)"
flags=$(grep -m1 '^flags' /proc/cpuinfo)
case " $flags " in
    *" pae "*) echo "PAE-INIT: cpuinfo advertises pae" ;;
    *)         echo "PAE-INIT: cpuinfo does NOT advertise pae" ;;
esac
case " $flags " in
    *" nx "*) echo "PAE-INIT: cpuinfo advertises nx" ;;
    *)        echo "PAE-INIT: cpuinfo does NOT advertise nx" ;;
esac
echo "PAE-INIT: end"
poweroff -f
exec sleep 3600
EOF
chmod +x "$TMP/init"
# DETERMINISM: the archive must be byte-reproducible so the gate's sha256 pin is stable.
# THREE leaks are closed, and each is MEASURED necessary:
#   * mtime  -- `touch -h -d @0` zeroes every entry's mtime before archiving.  `-h` is
#     load-bearing: the tree carries a dangling symlink (sbin_modprobe -> ../bin/kmod) and
#     plain `touch` dereferences it and fails.  (GNU cpio 2.15's `--reproducible` zeroes
#     ino/dev/uid/gid but NOT mtime -- the orchestrator measured the mtime fields leaking
#     into two builds, so this step is what actually closes that one.)
#   * inode/device/uid/gid -- `cpio --reproducible` zeroes them (a fresh mktemp -d assigns
#     different inodes each build, which the plain form would archive).
#   * the walk order and the gzip header -- `LC_ALL=C sort` and `gzip -9n`.
( cd "$TMP" && find . -exec touch -h -d @0 {} + && find . | LC_ALL=C sort \
    | cpio -o --quiet -H newc --reproducible | gzip -9n ) > "$OUT"
echo "built $OUT"
