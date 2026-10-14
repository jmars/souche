#!/usr/bin/env python3
"""tests/model.py -- independent model of the M0 test program's final state.

Mirrors tests/test.asm statement by statement in Python, computing the final
general-purpose registers, EFLAGS, the 4 KiB hashed memory window and the
number of executed instructions. `make oracle` diffs the oracle transcript
against this model; a mismatch in either direction is a gate failure.

HONEST SCOPE (see README.md): the model shares the *constants* with
tests/test.asm (a mistranscribed constant would cancel out of this check) but
implements the x86 semantics independently, so it does test the oracle
transcript against the program's arithmetic, flags, memory effects and
instruction count. It is not a proof of x86 semantics.

usage: model.py [--out results/expected.json] [--loop-n 100]
"""

import argparse
import hashlib
import json

MASK32 = 0xFFFFFFFF

WINDOW_ADDRESS = 0x101000
WINDOW_BYTES = 4096
STACK_TOP = 0x102000
LOAD_ADDRESS = 0x100000
ENTRY_ADDRESS = 0x100020
HALT_ADDRESS = 0x00100119  # MEASURED: objdump -d build/test.elf, the `hlt`


def p32(value):
    return value & MASK32


def parity(value):
    return bin(value & 0xFF).count("1") % 2 == 0


class Machine:
    def __init__(self, loop_n):
        self.n = 0
        self.regs = dict.fromkeys(["eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"], 0)
        self.cf = self.pf = self.af = self.zf = self.sf = self.of = 0
        self.df = 0
        self.memory = bytearray(WINDOW_BYTES)
        self.loop_n = loop_n

    # -- instruction accounting -------------------------------------------
    def step(self):
        self.n += 1

    def gpr(self, name):
        return self.regs[name]

    def setg(self, name, value, size):
        value = p32(value)
        if size == 32:
            self.regs[name] = value
        elif size == 16:
            self.regs[name] = p32((self.regs[name] & 0xFFFF0000) | (value & 0xFFFF))
        elif size == 8:
            self.regs[name] = p32((self.regs[name] & 0xFFFFFF00) | (value & 0xFF))
        else:
            raise ValueError(size)

    # -- memory ------------------------------------------------------------
    def store(self, address, value, size):
        offset = address - WINDOW_ADDRESS
        assert 0 <= offset < WINDOW_BYTES, hex(address)
        self.memory[offset:offset + size] = (value & ((1 << (size * 8)) - 1)).to_bytes(size, "little")

    def load(self, address, size):
        offset = address - WINDOW_ADDRESS
        assert 0 <= offset < WINDOW_BYTES, hex(address)
        return int.from_bytes(self.memory[offset:offset + size], "little")

    # -- flag helpers ------------------------------------------------------
    def add_flags(self, a, b, result, size):
        mask = (1 << size) - 1
        sign = 1 << (size - 1)
        a &= mask
        b &= mask
        self.cf = 1 if result > mask else 0
        self.pf = 1 if parity(result) else 0
        self.af = 1 if ((a ^ b ^ result) & 0x10) else 0
        self.zf = 1 if (result & mask) == 0 else 0
        self.sf = 1 if (result & mask) & sign else 0
        self.of = 1 if ((~(a ^ b) & (a ^ result)) & sign) else 0
        return result & mask

    def sub_flags(self, a, b, size):
        mask = (1 << size) - 1
        result = (a - b) & mask
        self.cf = 1 if (a & mask) < (b & mask) else 0
        self.pf = 1 if parity(result) else 0
        self.af = 1 if ((a ^ b ^ result) & 0x10) else 0
        self.zf = 1 if result == 0 else 0
        self.sf = 1 if result & (1 << (size - 1)) else 0
        self.of = 1 if (((a & mask) ^ (b & mask)) & (a & mask) ^ result) & (1 << (size - 1)) else 0
        return result

    def logic_flags(self, result, size):
        # v86 clears CF and OF for logical ops (and/or/xor/test), AF is undefined
        mask = (1 << size) - 1
        self.cf = 0
        self.of = 0
        self.af = 0
        self.pf = 1 if parity(result) else 0
        self.zf = 1 if (result & mask) == 0 else 0
        self.sf = 1 if (result & mask) & (1 << (size - 1)) else 0
        return result & mask

    def shift_flags(self, result, cf, size):
        mask = (1 << size) - 1
        self.cf = cf & 1
        self.pf = 1 if parity(result) else 0
        self.zf = 1 if (result & mask) == 0 else 0
        self.sf = 1 if (result & mask) & (1 << (size - 1)) else 0
        self.of = (self.cf ^ (self.sf)) & 1  # shl/of rule for shifts by 1
        return result & mask


def run(loop_n):
    m = Machine(loop_n)

    # ---- prologue ----
    m.step()                      # cld
    m.df = 0
    m.step()                      # mov esp, 0x102000
    m.setg("esp", STACK_TOP, 32)
    for name in ["eax", "ebx", "ecx", "edx", "esi", "edi", "ebp"]:
        m.step()                  # xor r32, r32
        m.logic_flags(0, 32)
        m.regs[name] = 0

    # ---- 1. counted loop: eax = sum(1..loop_n) ----
    m.step()                      # mov ecx, loop_n
    m.setg("ecx", loop_n, 32)
    for _ in range(loop_n):
        m.step()                  # add eax, ecx
        m.setg("eax", m.add_flags(m.gpr("eax"), m.gpr("ecx"), m.gpr("eax") + m.gpr("ecx"), 32), 32)
        m.step()                  # dec ecx
        m.setg("ecx", m.sub_flags(m.gpr("ecx"), 1, 32), 32)
        m.step()                  # jnz
    m.step()                      # mov [0x101000], eax
    m.store(0x101000, m.gpr("eax"), 4)

    # ---- 2. ALU chain on ebx ----
    m.step()                      # mov ebx, 0xFFFF0000
    m.setg("ebx", 0xFFFF0000, 32)
    m.step()                      # sub ebx, 1
    m.setg("ebx", m.sub_flags(m.gpr("ebx"), 1, 32), 32)
    m.step()                      # add ebx, 2
    m.setg("ebx", m.add_flags(m.gpr("ebx"), 2, m.gpr("ebx") + 2, 32), 32)
    m.step()                      # and ebx, 0x0FF00FF0
    m.setg("ebx", m.logic_flags(m.gpr("ebx") & 0x0FF00FF0, 32), 32)
    m.step()                      # or ebx, 0x0F
    m.setg("ebx", m.logic_flags(m.gpr("ebx") | 0x0F, 32), 32)
    m.step()                      # xor ebx, 0x00FF00FF
    m.setg("ebx", m.logic_flags(m.gpr("ebx") ^ 0x00FF00FF, 32), 32)
    m.step()                      # not ebx
    m.setg("ebx", (~m.gpr("ebx")) & MASK32, 32)
    m.step()                      # neg ebx
    m.setg("ebx", m.sub_flags(0, m.gpr("ebx"), 32), 32)
    m.step()                      # mov [0x101008], ebx
    m.store(0x101008, m.gpr("ebx"), 4)

    # ---- 3. shifts and rotates on ecx ----
    m.step()                      # mov ecx, 0x80000001
    m.setg("ecx", 0x80000001, 32)
    m.step()                      # shl ecx, 1
    value = m.gpr("ecx")
    m.setg("ecx", m.shift_flags(value << 1, value >> 31, 32), 32)
    m.step()                      # ror ecx, 3
    value = m.gpr("ecx")
    result = (value >> 3) | ((value << (32 - 3)) & MASK32)
    m.setg("ecx", m.shift_flags(result, (value >> 2) & 1, 32), 32)
    m.step()                      # sar ecx, 5
    value = m.gpr("ecx")
    signed = value - (1 << 32) if value >> 31 else value
    result = (signed >> 5) & MASK32
    m.setg("ecx", m.shift_flags(result, (value >> 4) & 1, 32), 32)
    m.step()                      # shr ecx, 4
    value = m.gpr("ecx")
    m.setg("ecx", m.shift_flags(value >> 4, (value >> 3) & 1, 32), 32)
    m.step()                      # rol ecx, 8
    value = m.gpr("ecx")
    result = ((value << 8) | (value >> 24)) & MASK32
    m.setg("ecx", m.shift_flags(result, value >> 31, 32), 32)
    m.step()                      # mov [0x101004], ecx
    m.store(0x101004, m.gpr("ecx"), 4)

    # ---- 4. 16/8-bit operand sizes and a segment override on edi ----
    m.step()                      # mov word [0x101010], 0x1234
    m.store(0x101010, 0x1234, 2)
    m.step()                      # mov ax, word es:[0x101010]
    m.setg("eax", m.load(0x101010, 2), 16)
    m.step()                      # add ax, 0x0FED
    m.setg("eax", m.add_flags(m.gpr("eax") & 0xFFFF, 0x0FED, (m.gpr("eax") & 0xFFFF) + 0x0FED, 16), 16)
    m.step()                      # mov byte [0x101012], 0xA5
    m.store(0x101012, 0xA5, 1)
    m.step()                      # mov dl, byte [0x101012]
    m.setg("edx", m.load(0x101012, 1), 8)
    m.step()                      # movzx edi, dl
    m.setg("edi", m.gpr("edx") & 0xFF, 32)
    m.step()                      # movsx edi, ax
    ax = m.gpr("eax") & 0xFFFF
    m.setg("edi", ax - 0x10000 if ax & 0x8000 else ax, 32)
    m.step()                      # and edi, 0x000FFFFF
    m.setg("edi", m.logic_flags(m.gpr("edi") & 0x000FFFFF, 32), 32)
    m.step()                      # lea edi, [edi + 4 * edi + 8]
    m.setg("edi", m.gpr("edi") + 4 * m.gpr("edi") + 8, 32)
    m.step()                      # mov [0x10100C], edi
    m.store(0x10100C, m.gpr("edi"), 4)

    # ---- 5. LCG table fill into the hashed window ----
    m.step()                      # mov esi, 1
    m.setg("esi", 1, 32)
    m.step()                      # xor edi, edi
    m.setg("edi", m.logic_flags(0, 32), 32)
    m.step()                      # mov ecx, 16
    m.setg("ecx", 16, 32)
    for _ in range(16):
        m.step()                  # imul esi, esi, 1103515245
        product = m.gpr("esi") * 1103515245
        m.setg("esi", product, 32)
        m.cf = 1 if product > MASK32 else 0
        m.of = 0
        m.step()                  # add esi, 12345
        m.setg("esi", m.add_flags(m.gpr("esi"), 12345, m.gpr("esi") + 12345, 32), 32)
        m.step()                  # mov [edi * 4 + 0x101040], esi
        m.store(0x101040 + 4 * m.gpr("edi"), m.gpr("esi"), 4)
        m.step()                  # inc edi
        m.setg("edi", m.add_flags(m.gpr("edi"), 1, m.gpr("edi") + 1, 32), 32)
        m.step()                  # cmp edi, ecx
        m.sub_flags(m.gpr("edi"), m.gpr("ecx"), 32)
        m.step()                  # jb

    # ---- 6. checksum loop over the table ----
    m.step()                      # xor edx, edx
    m.setg("edx", m.logic_flags(0, 32), 32)
    m.step()                      # xor edi, edi
    m.setg("edi", m.logic_flags(0, 32), 32)
    m.step()                      # mov ecx, 16
    m.setg("ecx", 16, 32)
    for _ in range(16):
        m.step()                  # mov eax, [edi * 4 + 0x101040]
        m.setg("eax", m.load(0x101040 + 4 * m.gpr("edi"), 4), 32)
        m.step()                  # add edx, eax
        m.setg("edx", m.add_flags(m.gpr("edx"), m.gpr("eax"), m.gpr("edx") + m.gpr("eax"), 32), 32)
        m.step()                  # xor edx, eax
        m.setg("edx", m.logic_flags(m.gpr("edx") ^ m.gpr("eax"), 32), 32)
        m.step()                  # rol edx, 1
        value = m.gpr("edx")
        result = ((value << 1) | (value >> 31)) & MASK32
        m.setg("edx", m.shift_flags(result, value >> 31, 32), 32)
        m.step()                  # inc edi
        m.setg("edi", m.add_flags(m.gpr("edi"), 1, m.gpr("edi") + 1, 32), 32)
        m.step()                  # cmp edi, ecx
        m.sub_flags(m.gpr("edi"), m.gpr("ecx"), 32)
        m.step()                  # jb
    m.step()                      # mov [0x101014], edx
    m.store(0x101014, m.gpr("edx"), 4)

    # ---- 7. push/pop and the stack-top snapshot ----
    m.step()                      # push 0x10203040
    m.setg("esp", m.gpr("esp") - 4, 32)
    m.store(m.gpr("esp"), 0x10203040, 4)
    m.step()                      # push edx
    m.setg("esp", m.gpr("esp") - 4, 32)
    m.store(m.gpr("esp"), m.gpr("edx"), 4)
    m.step()                      # mov [0x101018], esp
    m.store(0x101018, m.gpr("esp"), 4)
    m.step()                      # pop ebp
    m.setg("ebp", m.load(m.gpr("esp"), 4), 32)
    m.setg("esp", m.gpr("esp") + 4, 32)
    m.step()                      # pop eax
    m.setg("eax", m.load(m.gpr("esp"), 4), 32)
    m.setg("esp", m.gpr("esp") + 4, 32)
    m.step()                      # mov ebp, esp
    m.setg("ebp", m.gpr("esp"), 32)
    m.step()                      # mov [0x10101C], ebp
    m.store(0x10101C, m.gpr("ebp"), 4)

    # ---- 8. final flags, then halt ----
    m.step()                      # cmp ebp, 0x102000
    m.sub_flags(m.gpr("ebp"), STACK_TOP, 32)
    m.step()                      # hlt

    eflags = 0x2  # FLAGS_DEFAULT (bit 1 set)
    eflags |= m.cf << 0
    eflags |= m.pf << 2
    eflags |= m.af << 4
    eflags |= m.zf << 6
    eflags |= m.sf << 7
    eflags |= m.of << 11

    return m, eflags


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=None)
    parser.add_argument("--loop-n", type=int, default=100)
    args = parser.parse_args()

    m, eflags = run(args.loop_n)
    window = bytes(m.memory)

    expected = {
        "cpu": {
            "eip": "0x%08x" % HALT_ADDRESS,
            "eflags": "0x%08x" % eflags,
            "gpr": {name: "0x%08x" % m.regs[name]
                    for name in ["eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"]},
        },
        "instruction_count": m.n,
        "memory_window": {
            "address": "0x%08x" % WINDOW_ADDRESS,
            "bytes": WINDOW_BYTES,
            "sha256": hashlib.sha256(window).hexdigest(),
        },
    }

    text = json.dumps(expected, indent=2, sort_keys=True)
    if args.out:
        with open(args.out, "w") as handle:
            handle.write(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
