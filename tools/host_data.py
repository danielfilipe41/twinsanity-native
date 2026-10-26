#!/usr/bin/env python3
"""Converts the split's MIPS data asm (asm/data/*.s) into host-assemblable objects.

The retail data is symbolic MIPS assembly: pointers are symbols (spimdisasm names
them), so the same text assembles on x86 with directive translation and every
symbolic operand becoming a relocation the host linker resolves against the C++
functions' RETAIL() labels. That is what makes the retail vtables work: every
.word FUN_xxx in a vtable becomes a .4byte FUN_xxx relocation.

Build-time and local only: the repo ships no game data, and this changes nothing
about that. Run after tools/split.py:

    python3 tools/host_data.py [asm/data] [build/host/data]

Sections map as they come (.data stays .data, .rodata stays .rodata, ...).
.vutext's .incbin blobs pass through unchanged. Only data sections convert;
asm/text/ is retail code, replaced file by file with C++.

Directive map: .word→.4byte, .half/.short→.2byte, .dword/.xword→.8byte,
.byte/.float/.double/.ascii/.asciiz/.align/.space/.incbin unchanged. splat's
dlabel/enddlabel/nonmatching macros expand to plain .globl/.type/label so no
include/macro.inc is needed.
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
SRC = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "asm" / "data"
OUT = Path(sys.argv[2]) if len(sys.argv) > 2 else HERE / "build" / "host" / "data"

DIRECTIVES = {
    ".word": ".4byte",
    ".half": ".2byte",
    ".short": ".2byte",
    ".dword": ".8byte",
    ".xword": ".8byte",
}

# spimdisasm names bare constants as symbols (D_1000 for 0x1000, D_1C0 for 0x1C0, plain words in the sizes table): emit the
# constants so the host data holds the values, not relocations against fallback objects.
SHORT_CONSTANTS = {
    "D_1000": "0x1000",
    "D_1C0": "0x1C0",
}
SHORT_CONSTANT_RE = re.compile(r"\b(D_1000|D_1C0)\b")
LOCAL_LABEL_RE = re.compile(r"^\.L[0-9A-Fa-f]+$")

LABEL_MACRO = re.compile(r"^\s*(?:dlabel|glabel|jlabel|ehlabel|alabel)\s+(\S+)")
END_MACRO = re.compile(r"^\s*(?:enddlabel|endlabel)\s+(\S+)")
NONMATCHING = re.compile(r"^\s*nonmatching\s+\S+")

def convert_line(line):
    stripped = line.strip()
    if not stripped:
        return ""
    # splat's macro include: the converter expands dlabel itself, so the include is dropped
    if stripped.startswith('.include'):
        return None
    if stripped.startswith("#"):
        return "\t#" + stripped[1:]
    if stripped.startswith(";"):
        return None

    match = LABEL_MACRO.match(line)
    if match:
        name = match.group(1)
        return f"\t.globl {name}\n\t.type {name}, @object\n{name}:"

    match = END_MACRO.match(line)
    if match:
        name = match.group(1)
        return f"\t.size {name}, . - {name}"

    if NONMATCHING.match(line):
        return None

    # Bare section names are splat shorthand (GAS wants .section); .vudata has no host counterpart
    if stripped == ".vudata":
        return "\t.data"
    if stripped in (".data", ".rodata", ".sdata", ".sbss", ".bss", ".text"):
        return f"\t.section {stripped}"

    # spimdisasm annotates data lines with /* offset */ comments, often BEFORE the directive on the same line: the
    # code is what follows the last comment close. Over-wide .half values (4 ASCII chars in one halfword) split in two
    code = stripped.rsplit("*/", 1)[-1].strip()
    if not code:
        return ""
    parts = code.split(None, 1)
    rest = parts[1] if len(parts) > 1 else ""
    if parts[0] == ".asciiz":
        return f"\t.asciz {rest}".rstrip()
    if parts[0] == ".half":
        try:
            value = int(rest.split(",")[0].strip(), 0)
        except ValueError:
            value = None
        if value is not None and (value < 0 or value > 0xFFFF):
            unsigned = value & 0xFFFFFFFF
            return f"\t.2byte 0x{unsigned & 0xFFFF:X}, 0x{(unsigned >> 16) & 0xFFFF:X}"
        return f"\t.2byte {rest}".rstrip()
    # MIPS .align N is 2^N bytes; x86 GAS .align N is N bytes: .p2align keeps the power
    if parts[0] == ".align":
        return f"\t.p2align {rest}".rstrip()
    if parts[0] in DIRECTIVES:
        rest = SHORT_CONSTANT_RE.sub(lambda match: SHORT_CONSTANTS[match.group(1)], rest)
        # Cases of a replaced function's switch (BuildScriptCommand's ScriptCommandTable): the function is C++ on the host, so
        # its branch labels no longer exist. The retail link defines them as 0 (tools/make_ld.py); the data holds 0 too.
        operands = [operand.strip() for operand in rest.split(",")]
        if all(LOCAL_LABEL_RE.match(operand) is not None for operand in operands):
            rest = ", ".join(["0"] * len(operands))
        return f"\t{DIRECTIVES[parts[0]]} {rest}".rstrip()

    return code

VADDR_COMMENT = re.compile(r"/\*\s*[0-9A-Fa-f]+\s+([0-9A-Fa-f]+)")
VADDR_COMMENT_SHORT = re.compile(r"/\*\s*([0-9A-Fa-f]+)\s*\*/")


def label_was_aligned(lines, index):
    """Whether the retail label at lines[index] sat at a 16-aligned address: the first data line after it carries the
    retail vaddr in its /* comment */. movaps/movdqa into Vector4 arrays fault without it, and the host linker lays
    sections out at its own addresses."""
    for following in lines[index + 1:]:
        text = following.strip()
        if not text or text.startswith("nonmatching"):
            continue
        match = VADDR_COMMENT.search(following) or VADDR_COMMENT_SHORT.search(following)
        if match is None:
            return False
        return int(match.group(1), 16) % 16 == 0
    return False


def convert_file(src, dst):
    raws = src.read_text(encoding="utf-8", errors="replace").splitlines()
    out = []
    for index, raw in enumerate(raws):
        converted = convert_line(raw)
        if converted is None:
            continue
        if LABEL_MACRO.match(raw) is not None and label_was_aligned(raws, index):
            out.append("\t.p2align 4")
        out.append(converted)
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "--file":
        convert_file(Path(sys.argv[2]), Path(sys.argv[3]))
        return

    if not SRC.is_dir():
        raise SystemExit(f"{SRC} is no directory: run tools/split.py first")
    count = 0
    for path in sorted(SRC.rglob("*.s")):
        convert_file(path, OUT / (path.stem + ".host.s"))
        count += 1
    print(f"host_data: {count} files -> {OUT}")


main()
