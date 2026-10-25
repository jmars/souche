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

THE ADDRESS DOMAIN (results/x86-64-kallsyms-guards.md)

A name is only meaningful inside the image's own symbol domain: from the ELF's lowest text VMA
(.text/.init.text/.exit.text) to the highest address the table holds.  Outside it the tool prints a
NAMED REFUSAL, never a name -- because the nearest-entry lookup always HAS an answer, and a
plausible wrong name is the worst thing a naming tool can print (MEASURED before this guard: the
direct map 0xffff888000000000 -> __per_cpu_end, the module region 0xffffffffc0123000 ->
__init_scratch_end+0x3b723000, the percpu offset 0x1000 -> cpu_debug_store).  The domain is computed
from the image itself, and a percpu OFFSET is not an address at all (it needs a per-cpu base), so
it is refused too.

The offset convention is CHOSEN BY EVIDENCE, not by order: base-relative is probed first, and a mode
is accepted only if (i) its first decoded address at or above the text floor falls INSIDE the ELF's
text VMA range and (ii) the decoded table is non-decreasing.  If no mode validates the tool exits
non-zero instead of naming anything.  The banner states the chosen mode and that evidence.

USAGE

    python3 tools/kallsyms.py <image> <address> [<address> ...]
    python3 tools/kallsyms.py <image> --verify

    python3 tools/kallsyms.py /tmp/vmlinux.bin 0xffffffff81c00c02 0xffffffff81a3d3aa
    python3 tools/kallsyms.py /tmp/vmlinux.bin --verify

Addresses are LINK addresses (the image's own base -- 0xffffffff81000000 for the pinned 6.1.0-50
amd64 image).  Output is one line per address, `<address> -> <symbol>+<offset>`; a banner on stderr
gives the table geometry, the validated address domain and the offset convention the parse settled
on, so a reader can tell whether the parse is the one they think.

`--verify` checks anchors the image's own content pins down independently of the parse (the ELF
section VMAs and their boundary symbols, __stack_chk_fail at the address .text calls thousands
of times, the #PF entry), checks that the three out-of-domain addresses above are REFUSED, and exits non-zero if
any anchor fails.

No third-party imports: this must run on a bare host.
"""

import bisect
import struct
import sys

KERNEL_TEXT_LO = 0xFFFFFFFF80000000
KERNEL_TEXT_HI = 0xFFFFFFFFC0000000
TOKEN_INDEX_MAX = 0x2000      # no token table is 8 KiB of strings
NAMES_SEARCH = 4 << 20        # how far back from the token table kallsyms_names may start

TEXT_SECTIONS = (".text", ".init.text", ".exit.text")
# the kernel's own layout conventions (arch/x86/include/asm/pgtable_64_types.h); these are NOT the
# image's symbol domain -- they only say WHICH refusal an out-of-domain address earns
MODULES_VADDR = 0xFFFFFFFFC0000000
MODULES_END = 0xFFFFFFFFFF000000
HIGH_HALF = 0xFFFF800000000000   # everything at or above here is the kernel's runtime half

# the anchors --verify asserts, with the image-side evidence for each (see verify())
STACK_CHK_FAIL = 0xFFFFFFFF81A3D470
PAGE_FAULT_ENTRY = 0xFFFFFFFF81C00C02


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
        offsets, base, mode, addrs, evidence = address_table(img, ro, names, num)
        if base != rel_base:
            continue
        return {
            "token_table": token_table, "token_index": token_index, "tokens": toks,
            "token_index_off": token_index_off,
            "names": names, "names_end": names_end, "num_syms": num, "relative_base": rel_base,
            "offsets": offsets, "addresses": addrs, "mode": mode, "mode_evidence": evidence,
            "markers": markers, "n_markers": n_markers,
            "marker_values": struct.unpack_from("<%dI" % n_markers, img.data, markers),
            "markers_end": markers_end, "gap": token_table - markers_end,
        }
    raise SystemExit("kallsyms tables not found in .rodata (%d token-table candidate(s) rejected "
                     "by the names walk or the markers)" % tried)


def text_range(img):
    """The ELF's own text VMA range: the lowest and highest of .text/.init.text/.exit.text.  This is
    the image measuring itself -- it is what a decoded symbol address has to land in, and the low
    end is the floor of the symbol domain."""
    lo = hi = None
    found = []
    for name in TEXT_SECTIONS:
        s = img.by_name(name)
        if s is None or not s["size"]:
            continue
        found.append(name)
        lo = s["addr"] if lo is None else min(lo, s["addr"])
        hi = s["addr"] + s["size"] if hi is None else max(hi, s["addr"] + s["size"])
    if lo is None:
        raise SystemExit("no .text/.init.text/.exit.text section in the image: there is no text VMA "
                         "range to validate a decode against, so no name can be trusted")
    return lo, hi, found


def _validates(addrs, text_lo, text_hi):
    """The two conditions a decoded offset table must meet before its names may be printed.

    (i) The first decoded address at or above the text floor must fall INSIDE the ELF's text VMA
        range.  This is the discriminator the sortedness check alone is not: on a base-relative-only
        image the ABSOLUTE_PERCPU formula decodes the raw offsets, and raw offsets can be sorted --
        so the percpu arm could win and return an offset as if it were an address (review m1).
        Percpu entries legitimately sit at index 0, so the test is the first address at or above the
        floor, not addrs[0].
    (ii) The decoded table is non-decreasing (kallsyms is sorted by address)."""
    for i in range(len(addrs) - 1):
        if addrs[i] > addrs[i + 1]:
            return False, ("not non-decreasing at symbol %d (0x%x > 0x%x)"
                           % (i, addrs[i], addrs[i + 1])), None
    k = bisect.bisect_left(addrs, text_lo)
    if k >= len(addrs):
        return False, "no decoded address reaches the text floor 0x%x" % text_lo, None
    if not (text_lo <= addrs[k] < text_hi):
        return False, ("first address at/above the text floor is 0x%x, outside the text VMA range "
                       "0x%x..0x%x" % (addrs[k], text_lo, text_hi)), None
    n = sum(1 for a in addrs if text_lo <= a < text_hi)
    return True, "", ("first text symbol 0x%x (symbol %d), %d of %d symbols between the text floor "
                      "and the text sections' end 0x%x, last 0x%x"
                      % (addrs[k], k, n, len(addrs), text_hi, addrs[-1]))


def address_table(img, ro, names, num):
    """kallsyms_offsets + the convention it uses.  The array is int32 x num_syms and ends at
    kallsyms_relative_base (names - 16), but the base is PTR-aligned and the array is only 4-aligned,
    so up to 4 bytes of zero padding can sit between them -- and getting that wrong shifts EVERY
    symbol by one (MEASURED on the pinned image: pad 0 puts __stack_chk_fail's name on
    get_cpu_entry_area's address).  The discriminator is that kallsyms is sorted by address, so the
    right alignment is the one whose addresses are non-decreasing end to end; a one-symbol shift
    appends the padding's zero as a final "address" and breaks it.

    The CONVENTION is decided by _validates, base-relative probed FIRST (it is the modern kernel's
    default: CONFIG_KALLSYMS_BASE_RELATIVE without CONFIG_KALLSYMS_ABSOLUTE_PERCPU).  The pinned
    image is the other case -- it enables ABSOLUTE_PERCPU, where positive offsets are absolute and
    negative ones are relative to the base -- and it is accepted on the same evidence, not because
    it is tried second."""
    buf = img.data
    rbpos = names - 16
    base = struct.unpack_from("<Q", buf, rbpos)[0]
    text_lo, text_hi, _ = text_range(img)
    rejected = []
    for pad in range(0, 8):
        off = rbpos - pad - 4 * num
        if off < ro["off"] or any(buf[rbpos - pad:rbpos]):
            continue
        raw = struct.unpack_from("<%di" % num, buf, off)
        for mode, addrs in (("base-relative", [base + (o & 0xFFFFFFFF) for o in raw]),
                            ("absolute-percpu", [(o if o >= 0 else base - 1 - o) for o in raw])):
            ok, why, evidence = _validates(addrs, text_lo, text_hi)
            if ok:
                return off, base, mode, list(addrs), evidence
            rejected.append("%s, pad=%d: %s" % (mode, pad, why))
    raise SystemExit("kallsyms_offsets: no offset convention validates against the image's text VMA "
                     "range 0x%x..0x%x, so NO name can be trusted -- refusing to print any:\n  %s"
                     % (text_lo, text_hi, "\n  ".join(rejected[:16])))


def symbol_domain(tables, text_lo):
    """The range in which a name from this table MEANS something: from the image's text floor to the
    highest address the table holds (that covers .rodata/.data/.bss symbols above .text, which is
    where the section-boundary anchors live).  Percpu symbols below the floor are deliberately
    outside it: a percpu offset is not an address.  Their ceiling is returned too, from the table
    itself -- it is what says whether a below-floor value is a percpu offset or not."""
    a = tables["addresses"]
    k = bisect.bisect_left(a, text_lo)
    if k >= len(a):
        raise SystemExit("no symbol at or above the image's text floor 0x%x: this table names "
                         "nothing in the image" % text_lo)
    return a[k], a[-1], (a[k - 1] if k else -1)


def refusal(target, text_lo, text_hi, sym_lo, sym_hi, percpu_hi):
    """None if `target` is inside the symbol domain, else a NAMED refusal.  The nearest-entry lookup
    always has an answer, so the only defence against a plausible wrong name is to say which range
    the address belongs to instead."""
    if (target >> 47) not in (0, 0x1FFFF):
        return ("not a canonical address: bits 63:48 are neither all 0 nor all 1, so this is not a "
                "virtual address at all")
    if MODULES_VADDR <= target < MODULES_END:
        return ("not kernel text: the module region (0x%x..0x%x) -- symbols there are runtime-loaded, "
                "not in this image" % (MODULES_VADDR, MODULES_END))
    if target > sym_hi:
        return ("not kernel text: above the highest symbol in this image (0x%x)" % sym_hi)
    if target >= sym_lo:
        return None
    if target >= HIGH_HALF:
        return ("not kernel text: the direct-map/vmalloc/vmemmap range (0x%x..0x%x) -- an address "
                "there is a runtime mapping, not an image symbol; no symbol name is meaningful here"
                % (HIGH_HALF, text_lo))
    if 0 <= target <= percpu_hi:
        return ("not an address: the percpu offset namespace (this image's per-cpu symbols occupy "
                "0x0..0x%x) -- a per-cpu symbol's offset, meaningless without that CPU's per-cpu base"
                % percpu_hi)
    return ("not kernel text: below the image's text floor 0x%x and outside the percpu offsets "
            "(0x0..0x%x) -- a low-half or unmapped value, not an image symbol"
            % (text_lo, percpu_hi))


def name_address(img, tables, target, text_lo, text_hi, sym_lo, sym_hi, percpu_hi):
    """One output line for one address, and whether it is a NAME (True) or a refusal (False)."""
    why = refusal(target, text_lo, text_hi, sym_lo, sym_hi, percpu_hi)
    if why is not None:
        return "0x%x -> <%s>" % (target, why), False
    addrs = tables["addresses"]
    i = bisect.bisect_right(addrs, target) - 1
    if i < 0:                                   # unreachable inside the domain; loud if it is not
        return "0x%x -> <no symbol: below the first kallsyms entry 0x%x>" % (target, addrs[0]), False
    name, _ = _name_at(img, tables["tokens"], tables["names"] + _offset_of(img, tables, i))
    return "0x%x -> %s+0x%x" % (target, name.decode("latin-1"), target - addrs[i]), True


def symbols_at(img, tables, target):
    """Every (name, offset) the table holds at exactly `target` -- several symbols can share an
    address (MEASURED: .bss's VMA holds empty_zero_page, __bss_start and __nosave_end)."""
    a = tables["addresses"]
    out = []
    for i in range(bisect.bisect_left(a, target), bisect.bisect_right(a, target)):
        name, _ = _name_at(img, tables["tokens"], tables["names"] + _offset_of(img, tables, i))
        out.append((name.decode("latin-1"), target - a[i]))
    return out


def call_sites(img, target):
    """How many `call rel32` in .text target `target`, and the first one.  This is the image's own
    code read WITHOUT kallsyms: it is the independent half of the __stack_chk_fail anchor (the
    address came from the callers, results/x86-64-rsplow.md ss4)."""
    s = img.by_name(".text")
    if s is None:
        return 0, None
    lo, b = s["addr"], img.data[s["off"]:s["off"] + s["size"]]
    n, first, i = 0, None, b.find(b"\xe8")
    while 0 <= i <= len(b) - 5:
        rel, = struct.unpack_from("<i", b, i + 1)
        if lo + i + 5 + rel == target:
            n += 1
            if first is None:
                first = lo + i
        i = b.find(b"\xe8", i + 1)
    return n, first


# the out-of-domain addresses --verify requires to be REFUSED (the three the reviewer MEASURED as
# confidently wrong names before this guard)
REFUSAL_PROBES = (0xFFFF888000000000,   # the direct map / page_offset_base
                  0xFFFFFFFFC0123000,   # the module region
                  0x1000)               # a percpu offset


def verify(img, tables, text_lo, text_hi, sym_lo, sym_hi, percpu_hi):
    """Anchors the image pins down INDEPENDENTLY of the parse, plus the refusals.  Non-zero exit if
    any anchor fails: a tool that cannot prove itself must not be quoted."""
    checks = []

    def check(label, ok, detail):
        checks.append(ok)
        print("%s %s: %s" % ("verify: PASS" if ok else "verify: FAIL", label, detail))

    for sec, want in ((".text", "_text"), (".rodata", "__start_rodata"), (".bss", "__bss_start")):
        s = img.by_name(sec)
        if s is None:
            check("%s boundary symbol" % sec, False, "no %s section in the image" % sec)
            continue
        got = [n for n, o in symbols_at(img, tables, s["addr"])]
        check("%s boundary symbol" % sec, want in got,
              "the ELF puts %s at 0x%x; the table names %s there" % (sec, s["addr"], ", ".join(got)))

    n, first = call_sites(img, STACK_CHK_FAIL)
    got = [x for x, o in symbols_at(img, tables, STACK_CHK_FAIL)]
    check("__stack_chk_fail", n >= 1000 and "__stack_chk_fail" in got,
          "0x%x is the target of %d call rel32 in .text (first 0x%x); the table names %s"
          % (STACK_CHK_FAIL, n, first or 0, ", ".join(got) or "nothing"))

    a = tables["addresses"]
    i = bisect.bisect_right(a, PAGE_FAULT_ENTRY) - 1
    got = _name_at(img, tables["tokens"], tables["names"] + _offset_of(img, tables, i))[0].decode("latin-1")
    check("#PF entry", "page_fault" in got and PAGE_FAULT_ENTRY - a[i] < 0x100,
          "0x%x names %s+0x%x (the image's #PF entry code, results/x86-64-rsplow.md ss4)"
          % (PAGE_FAULT_ENTRY, got, PAGE_FAULT_ENTRY - a[i]))

    for target in REFUSAL_PROBES:
        line, named = name_address(img, tables, target, text_lo, text_hi, sym_lo, sym_hi, percpu_hi)
        print("verify: %s" % line)
        check("refusal of 0x%x" % target, not named, "refused by name, not named")

    bad = checks.count(False)
    print("verify: %d/%d anchors pass%s" % (len(checks) - bad, len(checks),
                                            "" if not bad else " -- THE PARSE CANNOT BE TRUSTED"))
    return 0 if not bad else 1


def main(argv):
    if len(argv) < 3:
        sys.stderr.write("usage: %s <image> <address> [<address> ...] | <image> --verify\n" % argv[0])
        return 2
    img = Image(argv[1])
    tables = find_tables(img)
    text_lo, text_hi, text_sections = text_range(img)
    sym_lo, sym_hi, percpu_hi = symbol_domain(tables, text_lo)
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
    sys.stderr.write(
        "kallsyms: convention %s chosen on evidence -- %s; symbol domain 0x%x..0x%x (text VMA range "
        "0x%x..0x%x from %s, percpu offsets 0x0..0x%x); addresses outside the domain are refused by "
        "name\n"
        % (tables["mode"], tables["mode_evidence"], sym_lo, sym_hi, text_lo, text_hi,
           "+".join(text_sections), percpu_hi))
    if "--verify" in argv[2:]:
        return verify(img, tables, text_lo, text_hi, sym_lo, sym_hi, percpu_hi)
    rc = 0
    for a in argv[2:]:
        try:
            target = int(a, 0)
        except ValueError:
            sys.stderr.write("%s: not a number\n" % a)
            rc = 1
            continue
        line, named = name_address(img, tables, target, text_lo, text_hi, sym_lo, sym_hi, percpu_hi)
        print(line)
        if not named:
            rc = 1
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
