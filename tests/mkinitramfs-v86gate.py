#!/usr/bin/env python3
"""tests/mkinitramfs-v86gate.py -- the SLICE-1 v86 guest kernel gate's initramfs.

    python3 tests/mkinitramfs-v86gate.py <init-script> <output.img>

ZERO KERNEL MODULES, and that is the point of this builder existing at all: the kernel
under test carries every disk-path driver =y (kbuild/build-v86guest.sh asserts 0 '=m'
lines in the resolved config), so -- unlike tests/mkinitramfs.py -- there is NO
/lib/modules tree here, no modules.dep, and no kmod.  What the image carries is an
Alpine netboot userspace (busybox + musl, i386 -- the only userspace v86 can run) and
the init script.

The Alpine userspace comes from ALPINE_ROOT (default /tmp/irf), the Alpine netboot
initramfs tree the M2-3 and M2-4 units used; this script does not fetch it.  What makes
the image pinned is the OUTPUT: its sha256 is in tests/baliveau-kernel-pin.txt as
initramfs_sha256, so a rebuild that changes the bytes is loud (gzip -9 -n and cpio's
newc format are both deterministic for the same tree).

The two Alpine files and the two versioned sonames are copied exactly as
tests/mkinitramfs.py copies them (that image is sha256-pinned in results/m2-3-ide.md and
is NOT touched by this script).
"""

import os
import shutil
import subprocess
import sys
import tempfile

BUSYBOX_APPLETS = ["sh", "mount", "ls", "cat", "grep", "tail", "head", "dd", "md5sum",
                   "dmesg", "uname", "echo", "cut", "tr", "find", "ln", "mkdir",
                   "basename", "wc", "stat", "sleep", "sed", "halt", "poweroff"]


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    init_script, output = argv[1], argv[2]
    alpine = os.environ.get("ALPINE_ROOT", "/tmp/irf")
    root = tempfile.mkdtemp(prefix="initramfs-v86gate-")
    try:
        for directory in ("bin", "sbin", "lib", "proc", "sys", "dev", "tmp"):
            os.makedirs(os.path.join(root, directory), exist_ok=True)

        for name in ("bin/busybox", "lib/ld-musl-i386.so.1"):
            source = os.path.join(alpine, name)
            if not os.path.isfile(source):
                print("mkinitramfs-v86gate: missing Alpine file: " + source
                      + " (set ALPINE_ROOT)", file=sys.stderr)
                return 1
            shutil.copy(source, os.path.join(root, name))
        for link, target in (("lib/libc.musl-x86.so.1", "ld-musl-i386.so.1"),):
            path = os.path.join(root, link)
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
    print("mkinitramfs-v86gate: %s (%d bytes)" % (output, os.path.getsize(output)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
