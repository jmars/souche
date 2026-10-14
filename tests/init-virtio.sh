#!/bin/sh
# The M2-4 virtio gate's guest initramfs init.
#
# WHAT IT IS FOR: it loads the Alpine kernel's OWN virtio drivers (virtio-pci and the
# multi-port virtio-console, which this netboot kernel ships as MODULES -- MEASURED: the
# 16.3 MiB decompressed kernel contains no virtio_pci and no virtio_console string), then
# reports what the KERNEL makes of the device: its own dmesg lines, the console port
# discovery, /dev/hvc*, and -- the point of the gate -- everything written ON the virtio
# console, which the host leg captures as "virtio-console0-output-bytes".
#
# WHY IT NEVER SLEEPS: the harness stops the machine at the first HLT (M0's stop condition,
# kept), and the virtio-pci probe runs from the driver's own probe path; a guest that goes
# idle before that happens observes nothing.  Every wait below is a BUSY wait in userspace,
# which is also what a real machine does while its drivers probe.
#
# TWO CONSOLES, DELIBERATELY: the cmdline is `console=hvc0 console=ttyS0`, so the kernel's
# log goes to BOTH -- to ttyS0 (which the harness captures as the comparable transcript for
# the reference leg) and to hvc0 (which the harness captures as the virtio console's own
# bytes).  The hvc0 stream is therefore the kernel's own messages arriving THROUGH the
# virtio console driver, not a userspace echo of them.
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mount -t proc none /proc 2>/dev/null
mount -t sysfs none /sys 2>/dev/null
mount -t devtmpfs none /dev 2>/dev/null

# Every report line goes to BOTH consoles: stdout is /dev/console (ttyS0 here, since it is
# the cmdline's LAST console) and /dev/hvc0 is written explicitly whenever it exists.  This
# is why the same text appears in the serial transcript and in the virtio console's bytes.
HVC=""
say() {
    echo "$@"
    [ -n "$HVC" ] && echo "$@" > /dev/hvc0
}

# The device as the firmware left it, before any driver loads: the kernel's own PCI table.
if [ -r /proc/bus/pci/devices ]; then
    say "GUEST-INIT: /proc/bus/pci/devices:"; cat /proc/bus/pci/devices
fi

say "GUEST-INIT: start, uname = $(uname -r)"
# busybox's modprobe, which is what the M2-3 gate's init uses: it reads modules.dep and
# loads the dependencies itself.  (/bin/kmod is included for parity with that image but its
# musl build here wants libz/libcrypto/liblzma/libzstd, which are NOT in this initramfs --
# MEASURED: it fails with "Error relocating /bin/kmod: gzread: symbol not found".)
/bin/busybox modprobe virtio_console &
/bin/busybox modprobe virtio_pci &
/bin/busybox modprobe virtio_ring &
/bin/busybox modprobe virtio &

i=0
while [ ! -e /dev/hvc0 ] && [ $i -lt 800 ]; do
    dd if=/dev/zero of=/dev/null bs=4096 count=1024 2>/dev/null
    i=$((i+1))
done
[ -e /dev/hvc0 ] && HVC=1
say "GUEST-INIT: waited $i round(s) for /dev/hvc0"

say "GUEST-INIT: /dev/hvc*:"; ls /dev/hvc* 2>/dev/null
say "GUEST-INIT: /sys/bus/virtio/devices:"; ls /sys/bus/virtio/devices 2>/dev/null
say "GUEST-INIT: virtio0's ids and its driver:"
for f in vendor device; do
    say "  virtio0/$f = $(cat /sys/bus/virtio/devices/virtio0/$f 2>/dev/null)"
done
say "  virtio0 (ls): $(ls /sys/bus/virtio/devices/virtio0/ 2>/dev/null | tr '\n' ' ')"
say "  /sys/bus/virtio/drivers: $(ls /sys/bus/virtio/drivers 2>/dev/null | tr '\n' ' ')"
say "GUEST-INIT: /proc/modules (virtio):"; grep virtio /proc/modules
say "GUEST-INIT: /proc/interrupts:"; cat /proc/interrupts
say "GUEST-INIT: /proc/devices (hvc/virtio):"; grep -E "hvc|virtio" /proc/devices

# The kernel's own probe output: -s because busybox dmesg's default buffer is 16 KiB while
# this kernel's log buffer is larger -- WITHOUT it the read returns the EARLIEST 16 KiB and
# the driver's probe lines are not in it (MEASURED in the first run of this gate).
say "GUEST-INIT: dmesg (virtio/hvc/console), the KERNEL's own lines:"
dmesg -s 131072 | grep -E "virtio|hvc|console|Console" | tail -30

# THE GATE'S OWN LINE, written THROUGH the virtio console by userspace: everything above
# went to the kernel's console(s), and this goes to the user-space side of the hvc port.
if [ -e /dev/hvc0 ]; then
    echo "GUEST-INIT: written to /dev/hvc0 by the guest (userspace write through the device)" > /dev/hvc0
    dmesg -s 131072 | grep -E "virtio|hvc|console|Console|pci 0000" >> /dev/hvc0 2>/dev/null

    # THE INBOUND DIRECTION, live: the guest announces that its /dev/hvc0 is open and the
    # driver has posted receive buffers, the HOST answers with --virtio-console-in's bytes
    # (the handshake is needed because upstream's input path DROPS a byte that arrives with
    # no posted receive buffer), and the guest echoes what it read back out through the same
    # console.  Both directions of the same device are then exercised in one boot.
    # SIXTEEN bytes, not thirty-two, and that bound is MEASURED: the driver posts exactly as
    # many receive buffers as the queue holds (fill_queue adds inbufs until add_inbuf fails,
    # and the queue size is 16), and upstream's input path DROPS a byte that arrives with no
    # posted buffer (virtio_console.js:213-215's `//TODO: Buffer`).  A first run of this gate
    # offered 32 and the guest received 16 (chain bytes written rose by exactly 16 while 32
    # were sent); the read then blocked forever and the machine halted.  So the host offers
    # what the queue can take, and the guest echoes it back through the same console.
    # The 16 bytes END WITH A NEWLINE because /dev/hvc0 is a canonical-mode tty: a read
    # returns a completed LINE, so a payload without one blocks forever (MEASURED: the first
    # 16-byte payload, which had no newline within the 16 that landed, blocked the machine).
    # THE READ SIDE IS OPENED FIRST, and that is a MEASURED requirement: the host answers the
    # sentinel immediately (inside the guest's own OUT instruction, via the notification), so
    # the bytes arrive while the ONE-SHOT write fd is still the tty's only open -- and the
    # close of that fd discards the queued input before the read can see it (the first three
    # runs of this gate blocked here).  With fd 3 held open for reading, the data is delivered
    # to a tty that stays open, and the line read completes.
    exec 3< /dev/hvc0
    echo "GUEST-INIT: waiting for host input" > /dev/hvc0
    # a LINE read, not a byte count: /dev/hvc0 is a canonical-mode tty, so a line read
    # completes exactly when the newline arrives (MEASURED: `head -c N` blocks forever when
    # the payload's newline is not among the N bytes that made it into the queue).
    read IN <&3 2>/dev/null
    exec 3<&-
    say "GUEST-INIT: read back from /dev/hvc0: [$IN]"
    echo "GUEST-INIT: read back from /dev/hvc0: [$IN]" > /dev/hvc0
fi

say "GUEST-INIT: end"

# never sleep: keep the guest executing until the harness's instruction limit stops it
exec dd if=/dev/zero of=/dev/null bs=4096 count=100000000 2>/dev/null
