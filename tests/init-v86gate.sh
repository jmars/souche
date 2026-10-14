#!/bin/sh
# tests/init-v86gate.sh -- the guest init for SLICE 1's gate: the v86 GUEST KERNEL
# (HANDOFF=v86guest; results/m2-5-baliveau-kernel.md).
#
# WHAT ONE BOOT OF THIS PROVES, on whatever machine it is handed:
#   * THE ATA/PCI PATH -- the kernel's OWN libata lines (ata_piix, a SCSI disk, sda1)
#     and m2-3's pinned read of the patterned disk: the md5 of /dev/sda's first 64
#     sectors, which is a pure function of tests/mkdisk.py's pattern.
#   * THE VIRTIO-CONSOLE PATH -- /dev/hvc0's existence, the kernel's own
#     "console [hvc0] enabled" and its virtio0 interrupt line, and 15 bytes the HOST
#     writes INTO the guest and the guest reads back THROUGH the device.
#
# ZERO MODULES: the kernel under test has every disk-path driver =y (the recipe asserts
# 0 '=m' lines), so this initramfs carries NO /lib/modules and no kmod -- an Alpine
# netboot userspace (busybox + musl, i386: the only userspace v86 can run) and this
# script.  m2-3/m2-4's Alpine-module initramfs is deliberately NOT reused: its modules
# are built for the Alpine 6.6 kernel and this kernel needs none.
#
# WHY EVERY WAIT IS A BUSY WAIT: the harness stops the machine at the first HLT (M0's
# stop condition, kept), and the drivers must be OBSERVED after they have probed --
# m2-4's policy, kept.
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mount -t proc none /proc 2>/dev/null
mount -t sysfs none /sys 2>/dev/null
mount -t devtmpfs none /dev 2>/dev/null

# stdout is /dev/console -- the cmdline's LAST console, ttyS0, which is the stream both
# machines are compared on.  /dev/hvc0 gets an explicit copy whenever it exists, so the
# same text is in the virtio console's byte stream too (m2-4's say()).
HVC=""
say() {
    echo "$@"
    [ -n "$HVC" ] && echo "$@" > /dev/hvc0 2>/dev/null
    return 0
}

say "GUEST-INIT: start, uname = $(uname -r)"
say "GUEST-INIT: cmdline = $(cat /proc/cmdline 2>/dev/null)"

# ---------------------------------------------------------------- the ATA/PCI path
say "GUEST-INIT: dmesg, the kernel's own ATA/PCI/SCSI lines:"
say "$(dmesg -s 131072 2>/dev/null | grep -E 'ata[0-9]+|libata|scsi [0-9]|sd [0-9]:|sda|pci 0000' | head -40)"

say "GUEST-INIT: /sys/block: $(ls /sys/block 2>/dev/null | tr '\n' ' ')"
say "GUEST-INIT: /dev/sda: $([ -b /dev/sda ] && echo present || echo absent)"
say "GUEST-INIT: /dev/sda1: $([ -b /dev/sda1 ] && echo present || echo absent)"
if [ -b /dev/sda ]; then
    say "GUEST-INIT: md5 of /dev/sda's first 64 sectors: $(dd if=/dev/sda bs=512 count=64 2>/dev/null | md5sum | cut -d' ' -f1)"
fi
say "GUEST-INIT: /proc/filesystems ext4 entries: $(grep -c ext4 /proc/filesystems 2>/dev/null)"

# ---------------------------------------------------------- the virtio-console path
# A CHEAP bounded spin: the device is created by the kernel's own driver during boot, so
# by the time userspace runs it is either already there or -- in the failure arm, whose
# kernel has no virtio at all -- it never comes.  No subprocess in the loop: this guest
# runs under an interpreter, where a fork+exec costs more than millions of iterations.
i=0
while [ ! -e /dev/hvc0 ] && [ $i -lt 5000 ]; do i=$((i+1)); done
[ -e /dev/hvc0 ] && HVC=1
say "GUEST-INIT: waited $i round(s) for /dev/hvc0"
say "GUEST-INIT: /dev/hvc*: $(ls /dev/hvc* 2>/dev/null | tr '\n' ' ')"
say "GUEST-INIT: /sys/bus/virtio/devices: $(ls /sys/bus/virtio/devices 2>/dev/null | tr '\n' ' ')"
say "GUEST-INIT: /proc/interrupts:"; cat /proc/interrupts 2>/dev/null
say "GUEST-INIT: /proc/devices hvc entries: $(grep -c hvc /proc/devices 2>/dev/null)"
say "GUEST-INIT: dmesg, the kernel's own virtio/console lines:"
say "$(dmesg -s 131072 2>/dev/null | grep -E 'virtio|hvc|console|Console' | tail -20)"

# ------------------------------------------------------- the host -> guest read-back
# The host leg answers the sentinel with 15 bytes (tests/v86gate-virtio-in.txt), and the
# guest echoes what it read.  The read fd is opened FIRST and held: m2-4 measured that
# closing the ONE-SHOT write fd first discards the queued input before the read sees it.
# -t bounds the wait so a machine whose host end is a plain file (gate 1b's qemu arm)
# still reaches "end" instead of blocking forever.
if [ -e /dev/hvc0 ]; then
    exec 3< /dev/hvc0
    echo "GUEST-INIT: waiting for host input" > /dev/hvc0
    read -t 30 IN <&3 2>/dev/null
    exec 3<&-
    say "GUEST-INIT: read back from /dev/hvc0: [$IN]"
fi

say "GUEST-INIT: end"

# STOP THE MACHINE, the way both harnesses' stop condition is defined (the first HLT):
# a clean deterministic end for BOTH legs at the same point, instead of burning to the
# instruction limit.  The loop below is the fallback for a userspace whose busybox has
# no halt applet or whose kernel cannot halt -- it never exits either (PID1 returning
# panics the kernel, which is noise in the transcript).
halt -f 2>/dev/null
while :; do dd if=/dev/zero of=/dev/null bs=4096 count=1024 2>/dev/null; done
