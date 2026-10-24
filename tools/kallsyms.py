#!/usr/bin/env python3
"""kallsyms.py -- name a kernel address from a STRIPPED vmlinux, using the image's own tables.

WHY THIS EXISTS (results/x86-64-nostorm.md ss4): the pinned Debian amd64 kernel's boot/System.map
is a decoy ("The real System.map is in the linux-image-<version>-dbg package"), so the repeated
frames of the amd64 guest's stack descent could not be named from the package.  The image itself
carries kallsyms (CONFIG_KALLSYMS + CONFIG_KALLSYMS_BASE_RELATIVE) inside .rodata, and that is a
cheaper instrument than the -dbg ddeb: no download, no second pin, and it reads the SAME image the
guest runs.

The tables are located STRUCTURALLY, from the ELF section headers and from each other -- never from
hard-coded file offsets, so a different image cannot silently produce another image's names.  The
layout this parser relies on, and what pins each table down:

    kallsyms_offsets[]      int32 x num_syms        at names - 16 - 4*num_syms
    kallsyms_relative_base  u64                     at names - 16
    kallsyms_num_syms       u64                     at names - 8
    kallsyms_names          compressed names        the candidate walk (below)
    kallsyms_markers        u32 x ceil(num/256)     the u32 run at align4(names end) whose k-th
                                                    entry IS the byte offset of symbol 256k
    (an optional table -- this image carries a 3-byte-per-symbol seq table here)
    kallsyms_token_table    256 NUL-terminated strings
    kallsyms_token_index    u16 x 256, each token's offset in that table

The token index is found first: 256 distinct u16 values below 0x2000 whose strings chain into a
table that ends just before them.  The names start is then the one candidate position whose
num_syms/relative_base header is plausible AND whose first names decompress to printable symbol
names.  The markers are the CHECK on the walk: every 256th name's byte offset must equal the
marker, so a walk that is off by even one byte is rejected.  Nothing is accepted on faith: the
numbers printed on stderr are the geometry this parse settled on.

USAGE

    python3 tools/kallsyms.py <image> <address> [<address> ...]

    python3 tools/kallsyms.py /tmp/vmlinux.bin 0xffffffff81c00c02 0xffffffff81a3d3aa

Addresses are LINK addresses (the image's own base -- 0xffffffff81000000 for the pinned 6.1.0-50
amd64 image).  Output is one line per address, `<address> -> <symbol>+<offset>`; a banner on stderr
gives the table geometry and the offset convention the parse settled on, so a reader can tell
whether the parse is the one they think.

No third-party imports: this must run on a bare host.
"""

import bisect
import struct
import sys

KERNEL_TEXT_LO = 0xFFFFFFFF80000000
KERNEL_TEXT_HI = 0xFFFFFFFFC0000000
TOKEN_INDEX_MAX = 0x2000      # no token table is 8 KiB of strings
NAMES_SEARCH = 4 << 20        # how far back from the token table kallsyms_names may start


class Image:
    """An ELF64 little-endian image's sections, by name."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:4] != b"\x7fELF":
            raise SystemExit("%s: not an ELF image" % path)
        if d[4] != 2 or d[5] != 1:
            raise SystemExit("%s: not ELFCLASS64/little-endian" % path)
        shoff, = struct.unpack_from("<Q", d, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3A)
        sh = d[shoff:shoff + shentsize * shnum]
        strtab_off, strtab_size = self._raw(sh, shentsize, shstrndx)[4:6]
        strtab = d[strtab_off:strtab_off + strtab_size]
        self.sections = []
        for i in range(shnum):
            name_off, typ, flags, addr, off, size = self._raw(sh, shentsize, i)
            end = strtab.index(b"\0", name_off)
            self.sections.append({"name": strtab[name_off:end].decode(), "type": typ,
                                  "addr": addr, "off": off, "size": size})

    @staticmethod
    def _raw(sh, shentsize, i):
        o = i * shentsize
        return struct.unpack_from("<IIQQQQ", sh, o)

    def by_name(self, name):
        for s in self.sections:
            if s["name"] == name:
                return s
        return None


def tokens(img, token_table, token_index):
    """The 256 expanded tokens, once: decompressing ~94k names through a bytes.index per token is
    the difference between a 3-second parse and a 30-second one."""
    out = []
    for b in range(256):
        p = token_table + token_index[b]
        out.append(img.data[p:img.data.index(b"\0", p)])
    return out


# a kallsyms token is a substring of a symbol name: letters, digits, '_', '.', '$'
NAME_ALPHABET = frozenset(b"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.$")


def _plausible_token(buf, t, vals, i):
    """A token is a NON-EMPTY substring of a symbol name.  Without this check the chain test alone
    admits every byte-shift of a table of equal-length strings (1785 candidates on the pinned image,
    each costing a full names scan); with it, one."""
    p = t + vals[i]
    end = t + vals[i + 1] - 1 if i < 255 else buf.index(b"\0", p)
    return end > p and all(c in NAME_ALPHABET for c in buf[p:end])


def iter_token_tables(img, ro):
    """Yield candidate (token_table, token_index) pairs: 256 distinct u16 below 0x2000 whose strings
    chain exactly into a table that ends just before them (the last string may be followed by zero
    padding).  A table where every token is a single byte is a byte-index table, not a token table,
    so the candidates are tried against the rest of the pipeline -- the names walk and the markers
    are what actually decide."""
    base, size = ro["off"], ro["size"]
    buf = img.data
    for q in range(base + 512, base + size - 512, 2):
        if buf[q] or buf[q + 1]:
            continue                      # vals[0] is 0 for this image: cheap, decisive filter
        vals = struct.unpack_from("<256H", buf, q)
        if len(set(vals)) != 256 or max(vals) > TOKEN_INDEX_MAX:
            continue
        for t in range(q - 8192, q - 256):
            if t < base or q - t <= 512:
                continue      # a table of 256 ONE-byte tokens is a byte index, not a token table
            if not all(buf[t + vals[i + 1] - 1] == 0 for i in range(255)):
                continue
            if any(buf[t + vals[i]:t + vals[i + 1]].count(b"\0") != 1 for i in range(255)):
                continue
            if buf.find(b"\0", t + vals[255], q) < 0:
                continue
            if not all(_plausible_token(buf, t, vals, i) for i in range(256)):
                continue
            yield t, q


def _name_at(img, toks, pos):
    """Decompress the name at `pos`: length byte(s), then that many token indices.  The first
    expanded byte is the symbol TYPE and is skipped, exactly as the kernel's own
    kallsyms_expand_symbol does."""
    buf = img.data
    ln = buf[pos]
    pos += 1
    if ln & 0x80:
        ln = (ln & 0x7F) | (buf[pos] << 7)
        pos += 1
    if ln == 0 or ln > 256:
        return None, pos
    name = b"".join(toks[buf[pos + k]] for k in range(ln))
    return name[1:], pos + ln


def find_names(img, ro, token_table, toks):
    """kallsyms_names: the candidate position whose header (relative_base, num_syms) is plausible
    AND whose first names decompress to printable symbol names.  The candidate set is tiny, so the
    filter is a u64 range test before any decompression."""
    buf = img.data
    base = ro["off"]
    lo = max(base, token_table - NAMES_SEARCH)
    # The header's high halves are the cheap filter and bytes.find is the fast scan: relative_base
    # is a 0xffffffff8... kernel address (so its high four bytes are 0xff) and num_syms' high four
    # bytes are zero.  A scan of four million positions with struct.unpack_from is 30 seconds; the
    # same filter as a byte search is a fraction of one.
    p = buf.find(b"\xff\xff\xff\xff", lo + 12, token_table - 4)
    while p >= 0:
        s = p + 12
        p = buf.find(b"\xff\xff\xff\xff", s - 11, token_table - 4)
        if s % 4 or buf[s - 4:s] != b"\0\0\0\0":
            continue
        rel_base = struct.unpack_from("<Q", buf, s - 16)[0]
        if not (KERNEL_TEXT_LO <= rel_base < KERNEL_TEXT_HI):
            continue
        num = struct.unpack_from("<Q", buf, s - 8)[0]
        if not (10000 <= num <= 3000000):
            continue
        if s - 16 - 4 * num < base:
            continue
        ok, pos, n = True, s, 0
        while n < 24:
            name, pos = _name_at(img, toks, pos)
            if name is None or not (2 <= len(name) <= 100) or not all(32 < c < 127 for c in name):
                ok = False
                break
            n += 1
        if ok:
            return s, num, rel_base
    raise SystemExit("kallsyms_names not found: no candidate header decompresses to names")


def walk_names(img, toks, names_off, num):
    """Every symbol's byte offset in kallsyms_names, and the end of the stream."""
    pos = names_off
    offs = []
    for _ in range(num):
        offs.append(pos - names_off)
        _, pos = _name_at(img, toks, pos)
    return offs, pos


def find_markers(img, names_end, num, offs, token_table):
    """The marker array at align4(names end): u32 x ceil(num/256), whose k-th entry must BE the
    byte offset of symbol 256k.  This is the check that makes the walk above verifiable -- an
    off-by-one in the length decoding cannot satisfy it."""
    buf = img.data
    n = (num + 255) // 256
    for pad in range(4):
        ms = names_end + pad
        if any(buf[names_end:ms]) or ms + 4 * n > token_table:
            continue
        vals = struct.unpack_from("<%dI" % n, buf, ms)
        if all(vals[k] == offs[256 * k] for k in range(n)):
            return ms, n, ms + 4 * n
    raise SystemExit("kallsyms_markers not found: no u32 run at the names end matches the walk")


def find_tables(img):
    ro = img.by_name(".rodata")
    if ro is None:
        raise SystemExit("no .rodata section -- no kallsyms tables to find")
    tried = 0
    for token_table, token_index_off in iter_token_tables(img, ro):
        tried += 1
        token_index = struct.unpack_from("<256H", img.data, token_index_off)
        toks = tokens(img, token_table, token_index)
        try:
            names, num, rel_base = find_names(img, ro, token_table, toks)
            offs, names_end = walk_names(img, toks, names, num)
            markers, n_markers, markers_end = find_markers(img, names_end, num, offs, token_table)
        except SystemExit:
            continue
        offsets, base, mode, addrs = address_table(img, ro, names, num)
        if base != rel_base:
            continue
        return {
            "token_table": token_table, "token_index": token_index, "tokens": toks,
            "token_index_off": token_index_off,
            "names": names, "names_end": names_end, "num_syms": num, "relative_base": rel_base,
            "offsets": offsets, "addresses": addrs, "mode": mode,
            "markers": markers, "n_markers": n_markers,
            "marker_values": struct.unpack_from("<%dI" % n_markers, img.data, markers),
            "markers_end": markers_end, "gap": token_table - markers_end,
        }
    raise SystemExit("kallsyms tables not found in .rodata (%d token-table candidate(s) rejected "
                     "by the names walk or the markers)" % tried)


def address_table(img, ro, names, num):
    """kallsyms_offsets + the convention it uses.  The array is int32 x num_syms and ends at
    kallsyms_relative_base (names - 16), but the base is PTR-aligned and the array is only 4-aligned,
    so up to 4 bytes of zero padding can sit between them -- and getting that wrong shifts EVERY
    symbol by one (MEASURED on the pinned image: pad 0 puts __stack_chk_fail's name on
    get_cpu_entry_area's address).  The discriminator is that kallsyms is sorted by address, so the
    right alignment is the one whose addresses are non-decreasing end to end; a one-symbol shift
    appends the padding's zero as a final "address" and breaks it."""
    buf = img.data
    rbpos = names - 16
    base = struct.unpack_from("<Q", buf, rbpos)[0]
    for pad in range(0, 8):
        off = rbpos - pad - 4 * num
        if off < ro["off"] or any(buf[rbpos - pad:rbpos]):
            continue
        raw = struct.unpack_from("<%di" % num, buf, off)
        for mode, addrs in (("absolute-percpu", [(o if o >= 0 else base - 1 - o) for o in raw]),
                            ("base-relative", [base + (o & 0xFFFFFFFF) for o in raw])):
            if all(addrs[i] <= addrs[i + 1] for i in range(num - 1)):
                return off, base, mode, list(addrs)
    raise SystemExit("kallsyms_offsets not found: no alignment gives a sorted address sequence")


def main(argv):
    if len(argv) < 3:
        sys.stderr.write("usage: %s <image> <address> [<address> ...]\n" % argv[0])
        return 2
    img = Image(argv[1])
    tables = find_tables(img)
    addrs = tables["addresses"]
    gap = tables["gap"]
    seq = ("a 3-byte-per-symbol table (kallsyms_seqs_of_names)" if 3 * tables["num_syms"] <= gap
           <= 3 * tables["num_syms"] + 3 else "an unidentified table")
    sys.stderr.write(
        "kallsyms: %d symbols, relative_base=0x%x, offsets@0x%x (int32, %s) names@0x%x(+0x%x) "
        "markers@0x%x (%d) token_table@0x%x token_index@0x%x; %d byte(s) before the token table = "
        "%s\n"
        % (tables["num_syms"], tables["relative_base"], tables["offsets"], tables["mode"],
           tables["names"], tables["names_end"] - tables["names"], tables["markers"],
           tables["n_markers"], tables["token_table"], tables["token_index_off"], gap, seq))
    rc = 0
    for a in argv[2:]:
        try:
            target = int(a, 0)
        except ValueError:
            sys.stderr.write("%s: not a number\n" % a)
            rc = 1
            continue
        i = bisect.bisect_right(addrs, target) - 1
        if i < 0:
            print("0x%x -> <no symbol: below the first kallsyms entry 0x%x>" % (target, addrs[0]))
            rc = 1
            continue
        name, _ = _name_at(img, tables["tokens"], tables["names"] + _offset_of(img, tables, i))
        print("0x%x -> %s+0x%x" % (target, name.decode("latin-1"), target - addrs[i]))
    return rc


def _offset_of(img, tables, i):
    """The byte offset of symbol i in kallsyms_names, from the marker for its 256-block."""
    block, within = divmod(i, 256)
    pos = tables["names"] + tables["marker_values"][block]
    for _ in range(within):
        _, pos = _name_at(img, tables["tokens"], pos)
    return pos - tables["names"]


if __name__ == "__main__":
    sys.exit(main(sys.argv))
