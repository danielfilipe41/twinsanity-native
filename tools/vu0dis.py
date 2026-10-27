#!/usr/bin/env python3
"""vu0dis.py: the VU0 microcode the PS2 runs, read out of the disc's .vutext blob

The PS2's particle placement is not in the EE's code: it is a VU0 microprogram (the game's culling
set, loaded into VU0 at start-up) that the EE calls with vcallms.  The port's CPU stand-in for it
(src/platform/null/math.cpp's ParticleBlockView) is a translation of that microcode, so the
microcode is the source the port has to match, and this is the reader for it.

Where the code is: assets/vutext.textbin.bin is the linked .vutext section (the EA base 0x2D9D90).
The culling set's DMA chain starts at the file offset 0xB120 (the EA 0x2E4EB0) and the particle
program's code sits inside it.  A micro address M of the culling set is the file offset

    0xB230 + M

(so 0xA08 is the file offset 0xBC38 and 0xB90 is 0xBCC0), and one instruction is 32 bits.

How an instruction is laid out (the VU0's own format, which shares its opcode numbering with the
COP2 macro mode the PS2 SDK documents):

    bits  0- 5   the opcode
    bits  6-10   the destination register (fd)   -- the ALU and integer ops
    bits 11-15   the first source (fs)
    bits 16-20   the second source (ft)
    bits 21-24   the field mask (x, y, z, w) of the destination
    bits 25-26   zero
    bits 27-31   the flags (I, E, M, D, T)

The MOVE family is the exception: there the destination is ft and the source fs, and the field
mask says which components move.  0x8000033c is a MOVE with a zero mask (a no-op the compiler
leaves behind) and 0x000002ff is a NOP.

Usage: vu0dis.py [OFFSET [COUNT]]        (the file offset, 0xB120 by default; the count in words)
       vu0dis.py --entries              (the program's entry points)
"""
import struct
import sys
from pathlib import Path

BLOB = Path(__file__).resolve().parent.parent / "assets" / "vutext.textbin.bin"

# The file offset of the culling set's code, and the micro address it stands for
CODE_BASE = 0xB230

# The opcodes, as the COP2 macro mode numbers them (the microcode shares them)
OPS = {
    0x00: "ADDx", 0x01: "ADDy", 0x02: "ADDz", 0x03: "ADDw",
    0x04: "SUBx", 0x05: "SUBy", 0x06: "SUBz", 0x07: "SUBw",
    0x08: "MADDx", 0x09: "MADDy", 0x0A: "MADDz", 0x0B: "MADDw",
    0x0C: "MSUBx", 0x0D: "MSUBy", 0x0E: "MSUBz", 0x0F: "MSUBw",
    0x10: "MAXx", 0x11: "MAXy", 0x12: "MAXz", 0x13: "MAXw",
    0x14: "MINIx", 0x15: "MINIy", 0x16: "MINIz", 0x17: "MINIw",
    0x18: "MULx", 0x19: "MULy", 0x1A: "MULz", 0x1B: "MULw",
    0x1C: "MULq", 0x1D: "MAXi", 0x1E: "MULi", 0x1F: "MINIi",
    0x20: "ADDq", 0x21: "MADDq", 0x22: "ADDi", 0x23: "MADDi",
    0x24: "SUBq", 0x25: "MSUBq", 0x26: "SUBi", 0x27: "MSUBi",
    0x28: "ADD", 0x29: "MADD", 0x2A: "MUL", 0x2B: "MAX", 0x2C: "SUB", 0x2D: "MSUB",
    0x2E: "OPMSUB", 0x2F: "MINI",
    0x30: "IADD", 0x31: "ISUB", 0x32: "IADDI", 0x34: "IAND", 0x35: "IOR", 0x36: "ISUBI",
    0x38: "CALLMS", 0x39: "CALLMSR",
    0x3B: "CLIP", 0x3C: "MOVE", 0x3D: "MR32", 0x3E: "MTIR", 0x3F: "NOP",
    0x7C: "LQI", 0x7D: "SQI", 0x7E: "LQD", 0x7F: "SQD",
    0xBC: "DIV", 0xBD: "SQRT", 0xBE: "RSQRT", 0xBF: "WAITQ",
    0x13C: "ITOF0", 0x13D: "ITOF4", 0x13E: "ITOF12", 0x13F: "ITOF15",
    0x17C: "FTOI0", 0x17D: "FTOI4", 0x17E: "FTOI12", 0x17F: "FTOI15",
    0x1FD: "ABS", 0x2BC: "ADDA", 0x2BD: "MADDA", 0x2BE: "MULA", 0x2BF: "MULAW",
    0x2FC: "SUBA", 0x2FD: "MSUBA", 0x2FE: "OPMULA", 0x2FF: "NOP",
}

# The MOVE family: the destination is ft, the source fs
MOVES = ("MOVE", "MR32", "MTIR", "MFIR")

# The words the compiler leaves as the program's entry markers: an IADD with the E flag and the
# marker in the upper bits (0x100X00f0, the X counting the entry)
def is_entry(word):
    return (word & 0x3F) == 0x30 and (word & 0xFE000000) == 0x10000000


def decode(word):
    op = word & 0x3F
    fd = (word >> 6) & 0x1F
    fs = (word >> 11) & 0x1F
    ft = (word >> 16) & 0x1F
    mask = (word >> 21) & 0xF
    name = OPS.get(op, "?%02x" % op)
    if name in MOVES:
        if name in ("MTIR", "MFIR"):
            return "%-6s vi%-2d <- vf%d.x" % (name, ft, fs)
        return "%-6s vf%-2d <- vf%-2d  (mask %x)" % (name, ft, fs, mask)
    return "%-6s vf%-2d <- vf%-2d, vf%-2d  (mask %x)" % (name, fd, fs, ft, mask)


# ---------------------------------------------------------------- the upper half
# A VU instruction is 64 bits: the upper half (bits 0-31 of the word pair, at the lower address) holds
# the integer and memory operations and the branches, the lower half the floating point ones. Only the
# lower half was decoded above, so a program's memory reads (LQ, the planes and the boxes) were invisible.
UPPER_OPS = {
    0x00: "LQ", 0x01: "SQ", 0x04: "ILW", 0x05: "ISW", 0x08: "IADDIU", 0x09: "ISUBIU",
    0x10: "FCEQ", 0x11: "FCSET", 0x12: "FCAND", 0x13: "FCOR", 0x14: "FSEQ", 0x15: "FSSET",
    0x16: "FSAND", 0x17: "FSOR", 0x18: "FMEQ", 0x19: "FMAND", 0x1A: "FMOR", 0x1C: "FCGET",
    0x20: "B", 0x21: "BAL", 0x24: "JR", 0x25: "JALR",
    0x28: "IBEQ", 0x29: "IBNE", 0x2C: "IBLTZ", 0x2D: "IBGTZ", 0x2E: "IBLEZ", 0x2F: "IBGEZ",
    0x30: "IADD", 0x31: "ISUB", 0x32: "IADDI", 0x34: "IAND", 0x35: "IOR", 0x36: "ISUBI", 0x37: "IANDI",
}
MASKS = "xyzw"


def decode_upper(word):
    op = word & 0x3F
    name = UPPER_OPS.get(op)
    if name is None:
        return None
    it = (word >> 16) & 0x1F
    is_ = (word >> 11) & 0x1F
    dest = (word >> 6) & 0x1F
    mask = (word >> 21) & 0xF
    flags = (word >> 27) & 0x1F
    if name in ("LQ", "SQ", "ILW", "ISW"):
        offset = (word & 0x7FF) * 8
        if offset >= 0x4000:
            offset -= 0x8000
        fields = "".join(MASKS[b] for b in range(4) if mask & (1 << b)) or "-"
        return "%-6s vf%-2d <- %s  (vi%02d %+#x)" % (name, dest, fields, is_, offset) if name == "LQ" else \
            "%-6s %s -> (vi%02d %+#x)  vf%d" % (name, fields, is_, offset, dest)
    if name in ("IADD", "ISUB", "IAND", "IOR"):
        return "%-6s vi%d <- vi%d, vi%d%s" % (name, dest, is_, it, "  E" if flags & 0x2 else "")
    if name in ("IADDI", "ISUBI", "IANDI"):
        immediate = (word >> 16) & 0x7FFF
        return "%-6s vi%d <- vi%s, %d" % (name, is_, it, immediate)
    if name == "IBEQ":
        return "IBEQ   vi%d, vi%d  -> %+#x" % (is_, it, (word & 0x7FF) * 8)
    if name == "IBNE":
        return "IBNE   vi%d, vi%d  -> %+#x" % (is_, it, (word & 0x7FF) * 8)
    if name in ("IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ"):
        return "%-6s vi%02d -> %+#x" % (name, is_, (word & 0x7FF) * 8)
    if name == "B":
        return "B      -> %+#x" % ((word & 0x7FF) * 8)
    if name == "BAL":
        return "BAL    vi%02d -> %+#x" % (is_, (word & 0x7FF) * 8)
    if name == "JR":
        return "JR     vi%02d" % is_
    if name == "JALR":
        return "JALR   vi%02d, vi%02d" % (is_, dest)
    if name in ("FCEQ", "FCAND", "FCOR", "FSEQ", "FSAND", "FSOR"):
        return "%-6s %+#x" % (name, (word >> 16) & 0xFFFFFF)
    return name


def main_pairs():
    args = [a for a in sys.argv[1:]]
    data = BLOB.read_bytes()
    offset = int(args[0], 0) if args else 0xB3F0
    count = int(args[1], 0) if len(args) > 1 else 40
    for index in range(count):
        off = offset + index * 8
        upper = struct.unpack_from("<I", data, off)[0]
        lower = struct.unpack_from("<I", data, off + 4)[0]
        micro = off - CODE_BASE
        up = decode_upper(upper)
        text = "%-46s | %s" % (up if up is not None else "?%02x" % (upper & 0x3F), decode(lower))
        print("0x%05x  micro %+#05x  %s" % (off, micro, text))


def main():
    args = [a for a in sys.argv[1:]]
    data = BLOB.read_bytes()
    if args and args[0] == "--entries":
        for off in range(0, len(data) - 4, 4):
            word = struct.unpack_from("<I", data, off)[0]
            if is_entry(word):
                print("%#07x  micro %#06x  %s" % (off, off - CODE_BASE, decode(word)))
        return
    offset = int(args[0], 0) if args else 0xBC00
    count = int(args[1], 0) if len(args) > 1 else 0x100
    for off in range(offset, offset + count * 4, 4):
        word = struct.unpack_from("<I", data, off)[0]
        if word == 0x8000033C:
            continue  # a MOVE with a zero mask: a no-op
        print("%#07x  micro %#06x  %010x  %s" % (off, off - CODE_BASE, word, decode(word)))


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--pairs":
        sys.argv = [sys.argv[0]] + sys.argv[2:]
        main_pairs()
    else:
        main()
