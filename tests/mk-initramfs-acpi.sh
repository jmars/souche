#!/bin/sh
# mk-initramfs-acpi.sh -- the ACPI-2 G2 initramfs (tests/images/guest-initramfs-acpi.img).
#
# THE SMALLEST USERSPACE that can answer the acceptance question: an init that cats
# /proc/interrupts (the IO-APIC-vs-XT-PIC labels) and greps the kernel's own ACPI/APIC
# lines out of dmesg, then sleeps.  It prints no host-derived value (no date, no epoch
# line): the image doubles as the --acpi determinism pair.
#
# THE TREE IS THE TREE'S OWN WORKING REBOOT IMAGE (tests/images/guest-initramfs-reboot.img,
# extracted) with ./init REPLACED -- not a from-scratch busybox archive.  Two from-scratch
# attempts were MEASURED dead on arrival on this exact kernel+machine, both before the
# script's first line produced a byte: (a) busybox bare at /busybox with a '#!/busybox sh'
# shebang -> "Attempted to kill init! exitcode=0x00000200"; (b) the /bin/busybox + symlink
# layout WITHOUT the reboot image's /lib and device dirs -> the same instant exit.  The
# reboot image's tree is the one this guest demonstrably runs (REBOOT_KERNEL_ARM boots it),
# so the delta here is exactly one file: ./init.
set -e
cd "$(dirname "$0")/.."
OUT="$(pwd)/tests/images/guest-initramfs-acpi.img"
SRC=tests/images/guest-initramfs-reboot.img
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
zcat "$SRC" | cpio -idm --quiet -D "$TMP"
cat > "$TMP/init" <<'EOF'
#!/bin/sh
# The ACPI-2 G2 init: the reboot image's own prologue (this archive IS that image,
# minus its disk-work block) plus the smallest userspace that can answer the
# acceptance question: /proc/interrupts and the kernel's own ACPI/APIC dmesg lines.
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mount -t proc none /proc 2>/dev/null
mount -t sysfs none /sys 2>/dev/null
mount -t devtmpfs none /dev 2>/dev/null
# The evidence goes to hvc0 FIRST: under --acpi the serial TTY write path freezes the
# kernel (results/acpi-2.md, finding F1) and a write to ttyS0 is therefore attempted
# only AFTER everything hvc0 carries is out.
if [ -c /dev/hvc0 ]; then
    exec >/dev/hvc0 2>&1
fi
echo "ACPI-INIT: begin"
echo "ACPI-INIT: /proc/interrupts:"
cat /proc/interrupts
echo "ACPI-INIT: dmesg, the acpi and apic lines:"
dmesg | grep -Ei 'ACPI|APIC|IO-?APIC|PIC|MADT|NR_IRQS'
echo "ACPI-INIT: end"
echo "ACPI-INIT: ttyS0 probe begins (may freeze, finding F1)"
echo "ACPI-INIT: ttyS0 probe line" > /dev/ttyS0
exec sleep 3600
EOF
chmod +x "$TMP/init"
(cd "$TMP" && find . | cpio -o -H newc --quiet | gzip > "$OUT")
gzip -t "$OUT"
echo "mk-initramfs-acpi: $OUT written ($(stat -c %s "$OUT") bytes)"
