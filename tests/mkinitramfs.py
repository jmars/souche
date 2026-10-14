#!/usr/bin/env python3
"""Build a guest initramfs for the M2-4 virtio gate.

WHY THIS SCRIPT EXISTS: the M2-3 gate's initramfs (tests/images/guest-initramfs.img) was
built for that unit by a script that was not kept.  This unit needs a SECOND image -- the
Alpine netboot kernel has no virtio driver built in either (MEASURED: 3 'virtio' strings in
the 16.3 MiB decompressed kernel, none of them virtio_pci or virtio_console), so the kernel's
OWN virtio-pci + virtio-console modules have to come out of Alpine's modloop-lts
(6.6.134-0-lts, the module build of the very kernel tests/images/vmlinuz-i386 is) inside an
initramfs.  The M2-3 image is NOT touched: its sha256 is pinned in results/m2-3-ide.md and
G4 re-runs that gate.

    python3 tests/mkinitramfs.py <modloop-modules-dir> <init-script> <output.img> <mod.ko>...

The kernel modules come from the modloop's mounted tree (a squashfs; the M2-3 unit left one
mounted at /tmp/modloop-mnt).  The vmlinux modules' vermagic is checked implicitly: the
kernel refuses to load a module whose version magic does not match, and the gate prints the
kernel's own refusal if so.

The image is a gzip'd cpio (newc), which is what the kernel's initramfs unpacker reads and
what the M2-3 image is (1387445 bytes, `gzip compressed data`).
"""

import os
import shutil
import subprocess
import sys
import tempfile

# The Alpine userspace pieces the init script needs: a shell, the coreutils-applets the M2-3
# script uses, and kmod (busybox's own modprobe is NOT enough: it ignores modules.dep's
# dependency lines here, so the real kmod from the modloop-matching Alpine userspace is
# included, exactly as the M2-3 image does).
BUSYBOX_APPLETS = ["sh", "mount", "ls", "cat", "grep", "tail", "head", "dd", "md5sum",
                   "dmesg", "uname", "sleep", "echo", "cut", "tr", "find", "ln", "mkdir",
                   "basename", "wc", "stat"]


def main(argv):
    if len(argv) < 5:
        print(__doc__)
        return 2
    modloop_modules, init_script, output, modules = argv[1], argv[2], argv[3], argv[4:]

    # The Alpine userspace the init runs on.  The netboot initramfs-lts (fetched for the M2-3
    # unit) carries busybox, kmod and musl; this unit extracts it into ALPINE_ROOT.
    alpine = os.environ.get("ALPINE_ROOT", "/tmp/irf")
    root = tempfile.mkdtemp(prefix="initramfs-")
    try:
        for directory in ("bin", "sbin", "lib", "proc", "sys", "dev", "newroot", "tmp"):
            os.makedirs(os.path.join(root, directory), exist_ok=True)

        # the module tree: each module at its modloop-relative path, plus modules.dep and
        # modules.builtin so kmod can resolve the dependencies it is asked for.
        for module in modules:
            source = os.path.join(modloop_modules, module)
            if not os.path.isfile(source):
                print("mkinitramfs: no such module: " + source, file=sys.stderr)
                return 1
            target = os.path.join(root, "lib/modules/6.6.134-0-lts", module)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.copy(source, target)
        for name in ("modules.dep", "modules.builtin", "modules.alias", "modules.order"):
            source = os.path.join(modloop_modules, name)
            if os.path.isfile(source):
                shutil.copy(source, os.path.join(root, "lib/modules/6.6.134-0-lts", name))

        # the Alpine userspace the init runs on (musl + busybox + kmod)
        for name in ("bin/busybox", "bin/kmod", "lib/ld-musl-i386.so.1", "lib/libkmod.so.2.4.2"):
            source = os.path.join(alpine, name)
            if not os.path.isfile(source):
                print("mkinitramfs: missing Alpine file: " + source, file=sys.stderr)
                return 1
            shutil.copy(source, os.path.join(root, name))
        # versioned sonames, as the M2-3 image has them
        for link, target in (("lib/ld-musl-x86.so.1", "libc.musl-x86.so.1"),
                             ("lib/libc.musl-x86.so.1", "ld-musl-i386.so.1"),
                             ("lib/libkmod.so.2", "libkmod.so.2.4.2"),
                             ("sbin/insmod", "../bin/kmod"),
                             ("sbin/modprobe", "../bin/kmod"),
                             ("sbin/lsmod", "../bin/kmod")):
            path = os.path.join(root, link)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            if not os.path.lexists(path):
                os.symlink(target, path)
        for applet in BUSYBOX_APPLETS:
            path = os.path.join(root, "bin", applet)
            if not os.path.lexists(path):
                os.symlink("busybox", path)

        shutil.copy(init_script, os.path.join(root, "init"))
        os.chmod(os.path.join(root, "init"), 0o755)

        listing = subprocess.run(["find", ".", "-print0"], cwd=root, check=True,
                                 capture_output=True).stdout
        cpio = subprocess.run(["cpio", "--create", "--null", "--format=newc", "--quiet"],
                              cwd=root, input=listing, check=True, capture_output=True).stdout
        with open(output, "wb") as handle:
            handle.write(subprocess.run(["gzip", "-9", "-n", "-c"], input=cpio, check=True,
                                        capture_output=True).stdout)
    finally:
        shutil.rmtree(root)
    print("mkinitramfs: %s (%d bytes)" % (output, os.path.getsize(output)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
