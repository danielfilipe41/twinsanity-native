#!/usr/bin/env python3
"""The symbol map a build without the split's asm needs.

A normal computer build bakes the retail data into the executable: tools/host_data.py turns the split's data asm into
host objects and the link resolves every pointer in the data to the host's own symbols. A build without the asm (DATA=
runtime) has no bytes to link, so it copies the data out of the player's own SLES_525.68 at the first start and points
the words that name a symbol at the port's own code (src/platform/null/retaildata.cpp). This script writes the symbol
list that both ends of that need: every symbol's retail address and name, sorted, so a word of the data can be looked
up by what it holds.

The list is the retail executable's own symbol addresses, not its bytes: the split's symbol_addrs.txt, checked in.
Generated once; the loader needs nothing but the player's file at run time.

    python3 tools/retail_map.py            # write tools/retail_symbols.h
    python3 tools/retail_map.py --check    # the split's pointer words against the map's, by the retail executable
    python3 tools/retail_map.py --emit DIR # write DIR's code-pointer table and the image's extents
"""
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
SYMBOLS = HERE / "symbol_addrs.txt"
EXECUTABLE = HERE / "disc" / "SLES_525.68"
HEADER = HERE / "tools" / "retail_symbols.h"

# The retail executable's data, by the section headers of the PAL 1.01 ELF: the lowest address the game's code reads as
# data is the VU microcode's. The image ends with the program's bss (splat.yaml's 0xD0DA0 at 0x30AD00).
IMAGE_BASE = 0x2D9D90
IMAGE_END = 0x3DBAA0
# The last byte of data the executable's file holds (the sdata section's end): the bss past it is zeroes, not bytes
DATA_END = 0x30A460
# The program's own end marker, which the split's data points at: the disc's ELF has it here, past the bss the split names
END_SYMBOL = 0x3DB200

TYPE_FUNC = "type:func"


def read_symbols():
    symbols = []
    known = set()
    for line in SYMBOLS.read_text().splitlines():
        match = re.match(r"(\w+) = 0x([0-9A-Fa-f]+);(.*)", line)
        if match is None:
            continue
        address = int(match.group(2), 16)
        symbols.append((address, match.group(1), TYPE_FUNC in match.group(3)))
        known.add(address)

    # The labels the split found inside another function or without a name of their own (func_001D9590, D_0030A20C and
    # the like): they are not in symbol_addrs.txt, but the port's code names them, and the data points at some of them. The
    # data objects the baked build links define exactly the set, so their symbol tables fill the map in
    bases = {"data.data": 0x2E6F00, "rodata.rodata": 0x2EC400, "sdata.sdata": 0x309880, "vudata.data": 0x2EC348,
             "sbss.sbss": 0x30A480, "bss.bss": 0x30AD00}
    names = {name for _, name, _ in symbols}
    objects = HERE / "build" / "host" / "data"
    if objects.is_dir():
        for path in sorted(objects.glob("*.o")):
            if path.stem not in bases:
                continue
            data = path.read_bytes()
            shoff = struct.unpack_from("<I", data, 0x20)[0]
            shentsize = struct.unpack_from("<H", data, 0x2E)[0]
            shnum = struct.unpack_from("<H", data, 0x30)[0]
            tables = []
            for index in range(shnum):
                fields = struct.unpack_from("<IIIIIIIIII", data, shoff + index * shentsize)
                tables.append({"name_index": fields[0], "type": fields[1], "offset": fields[4], "size": fields[5]})
            for index in range(shnum):
                fields = struct.unpack_from("<IIIIIIIIII", data, shoff + index * shentsize)
                if fields[1] != 2:  # SHT_SYMTAB
                    continue
                strings = data[tables[fields[6]]["offset"]:tables[fields[6]]["offset"] + tables[fields[6]]["size"]]
                for entry in range(fields[5] // 16):
                    name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", data, fields[4] + entry * 16)
                    end = strings.find(b"\0", name)
                    name = strings[name:end].decode("ascii", "replace")
                    # A global definition, or a function the data points at that has no name of its own: an undefined
                    # reference there is a jump table's target, and its name carries the address
                    if (info >> 4) != 1 or not name or name in names:
                        continue
                    if shndx == 0:
                        match = re.match(r"^(?:FUN|func)_([0-9A-Fa-f]{8})$", name)
                        if match:
                            symbols.append((int(match.group(1), 16), name, True))
                            names.add(name)
                        continue
                    if (info & 0xF) not in (0, 1, 2):
                        continue
                    address = bases[path.stem] + value
                    if not IMAGE_BASE <= address < IMAGE_END + 0x1000:
                        continue
                    symbols.append((address, name, (info & 0xF) == 2))
                    names.add(name)
    symbols.sort()
    return symbols


# Where a data object's section lands in the retail image, by the section's own name (a `.data` section's base
# depends on which object it is in)
SECTION_BASES = {".rodata": 0x2EC400, ".sdata": 0x309880, ".sbss": 0x30A480, ".bss": 0x30AD00}


def section_base(stem, name):
    if name in SECTION_BASES:
        return SECTION_BASES[name]
    return 0x2EC348 if stem == "vudata.data" else 0x2E6F00


def read_relocations(symbols):
    """The data's pointer words that point into a function rather than at its start (a jump table's entry, a thunk): the
    split relocates them at the function's symbol plus an offset, so the loader needs the word's function to point it at
    the port's own. The word's address, and the symbol whose address it is an offset from."""
    bases = {"data.data": 0x2E6F00, "rodata.rodata": 0x2EC400, "sdata.sdata": 0x309880, "vudata.data": 0x2EC348}
    by_name = {name: address for address, name, _ in symbols}
    objects = HERE / "build" / "host" / "data"
    words = {}
    if not objects.is_dir():
        return []
    for path in sorted(objects.glob("*.o")):
        if path.stem not in bases:
            continue
        data = path.read_bytes()
        shoff = struct.unpack_from("<I", data, 0x20)[0]
        shentsize = struct.unpack_from("<H", data, 0x2E)[0]
        shnum = struct.unpack_from("<H", data, 0x30)[0]
        shstr = struct.unpack_from("<H", data, 0x32)[0]
        sections = []
        for index in range(shnum):
            fields = struct.unpack_from("<IIIIIIIIII", data, shoff + index * shentsize)
            sections.append({"name_index": fields[0], "type": fields[1], "offset": fields[4], "size": fields[5],
                             "info": fields[7]})
        names = data[sections[shstr]["offset"]:sections[shstr]["offset"] + sections[shstr]["size"]]

        def cstring(offset):
            end = names.find(b"\0", offset)
            return names[offset:end].decode("ascii", "replace")

        for section in sections:
            section["name"] = cstring(section["name_index"])
        by_name_section = {section["name"]: section for section in sections}
        table = by_name_section.get(".symtab")
        strings = by_name_section.get(".strtab")
        if table is None or strings is None:
            continue
        strings = data[strings["offset"]:strings["offset"] + strings["size"]]
        syms = []
        for index in range(table["size"] // 16):
            name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", data, table["offset"] + index * 16)
            end = strings.find(b"\0", name)
            syms.append(strings[name:end].decode("ascii", "replace"))
        for rel in sections:
            if rel["type"] != 9 or rel["info"] >= len(sections):
                continue
            base = section_base(path.stem, sections[rel["info"]]["name"])
            for entry in range(rel["size"] // 8):
                offset, info = struct.unpack_from("<II", data, rel["offset"] + entry * 8)
                if (info & 0xFF) != 1:  # R_386_32 only
                    continue
                name = syms[info >> 8]
                symbol = END_SYMBOL if name in ("_end", "end") else by_name.get(name)
                if symbol is None:
                    match = re.match(r"^(?:FUN|func)_([0-9A-Fa-f]{8})$", name)
                    symbol = int(match.group(1), 16) if match else None
                if symbol is not None:
                    words[base + offset] = symbol
    return sorted(words.items())


def write_header(symbols, relocations):
    lines = [
        "// Generated by tools/retail_map.py -- do not edit.",
        "//",
        "// The retail program's symbols, by the address each sits at: a word of the data that holds one of these is a",
        "// pointer, and a build without the split's asm (DATA=runtime) points it at the port's own code or at its own",
        "// image when it copies the data out of the player's SLES_525.68 (src/platform/null/retaildata.cpp).",
        "#pragma once",
        "",
        '#include "common.h"',
        "",
        "namespace RetailSymbols",
        "{",
        f"constexpr u32 ImageBase = 0x{IMAGE_BASE:08X}u;",
        f"constexpr u32 ImageEnd = 0x{IMAGE_END:08X}u;",
        "constexpr u32 ImageSize = ImageEnd - ImageBase;",
        "",
        "// A symbol of the retail program: where it is, what it is called, and whether it is code (below the data's start)",
        "struct Symbol",
        "{",
        "    u32 address;",
        "    const char* name;",
        "    bool code;",
        "};",
        "constexpr Symbol Symbols[] = {",
    ]
    for address, name, code in symbols:
        lines.append(f'    {{0x{address:08X}u, "{name}", {"true" if code else "false"}}},')
    lines += [
        "};",
        "constexpr u32 SymbolCount = sizeof(Symbols) / sizeof(Symbols[0]);",
        "",
        "// A pointer word the split made: its address, and the symbol whose address it holds (at an offset, or not)",
        "struct Relocation",
        "{",
        "    u32 word;",
        "    u32 symbol;",
        "};",
        "constexpr Relocation Relocations[] = {",
    ]
    for word, symbol in relocations:
        lines.append(f"    {{0x{word:08X}u, 0x{symbol:08X}u}},")
    lines += [
        "};",
        "constexpr u32 RelocationCount = sizeof(Relocations) / sizeof(Relocations[0]);",
        "",
        "// A code symbol's host address, for the pointer words that name it",
        "struct HostCode",
        "{",
        "    u32 address;",
        "    void* host;",
        "};",
        "extern const HostCode HostCodes[];",
        "extern const u32 HostCodeCount;",
        "}",
        "",
    ]
    HEADER.write_text("\n".join(lines), newline="\n")
    return len(lines)


def array(text, name):
    match = re.search(rf"\b{name}\[\] = \{{\n(.*?)\n\}};", text, re.S)
    if match is None:
        raise SystemExit(f"retail_symbols.h has no {name}: regenerate it")
    return match.group(1)


def emit(directory):
    """The code pointers the data may hold: a weak reference per code symbol, so a target the port does not define is a
    null the loader counts instead of a link error."""
    text = HEADER.read_text()
    base = int(re.search(r"constexpr u32 ImageBase = 0x([0-9A-F]+)u;", text).group(1), 16)
    end = int(re.search(r"constexpr u32 ImageEnd = 0x([0-9A-F]+)u;", text).group(1), 16)
    code = []
    data = []
    for address, name, is_code in re.findall(r'\{0x([0-9A-F]+)u, "([A-Za-z_][A-Za-z0-9_]*)", (true|false)\}', array(text, "Symbols")):
        if is_code == "true":
            code.append((int(address, 16), name))
        else:
            data.append((int(address, 16), name))

    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)

    lines = [
        "// Generated by tools/retail_map.py --emit, from tools/retail_symbols.h. Do not edit.",
        '#include "retail_symbols.h"',
        "",
    ]
    # Data labels in the text (the port's own variables under the retail names) that the data points at: the image has no
    # bytes for them, so their host definitions are taken by name too
    reloc_targets = {int(symbol, 16) for symbol in
                     re.findall(r"\{0x[0-9A-F]+u, 0x([0-9A-F]+)u\},", array(text, "Relocations"))}
    host_data = [(address, name) for address, name in data if address < base and address in reloc_targets]
    # By address: the loader finds a word's host symbol by binary search. The kind is part of the declaration because a
    # wasm link keeps functions and objects apart: a code symbol is a function, a data one the bytes it names
    hosts = sorted([(address, name, True) for address, name in code] +
                   [(address, name, False) for address, name in host_data])

    for index, (_, name, is_code) in enumerate(hosts):
        kind = f"void t_{index:04d}()" if is_code else f"u8 t_{index:04d}[]"
        lines.append(f'extern "C" {kind} asm("{name}") __attribute__((weak));')
    lines += ["", "namespace RetailSymbols", "{", "const HostCode HostCodes[] = {"]
    for index, (address, _, is_code) in enumerate(hosts):
        pointer = f"&t_{index:04d}" if is_code else f"t_{index:04d}"
        lines.append(f"    {{0x{address:08X}u, (void*){pointer}}},")
    lines += [
        "};",
        "const u32 HostCodeCount = sizeof(HostCodes) / sizeof(HostCodes[0]);",
        "}",
        "",
    ]
    (directory / "retail_code_table.cpp").write_text("\n".join(lines) + "\n", newline="\n")

    # The image and a symbol per data symbol at its offset: code that reads g_Foo reads the image's bytes there
    lines = [
        "// Generated by tools/retail_map.py --emit, from tools/retail_symbols.h. Do not edit.",
        "\t.section .bss",
        "\t.balign 64",
        "\t.globl __retail_image",
        "\t.globl __retail_image_end",
        "__retail_image:",
        f"\t.space 0x{end - base:X}",
        "__retail_image_end:",
        "",
    ]
    in_image = [(address, name) for address, name in data if base <= address < end]
    for address, name in in_image:
        lines.append(f"\t.globl {name}")
        lines.append(f"\t.set {name}, __retail_image + 0x{address - base:X}")
    (directory / "retail_data_symbols.s").write_text("\n".join(lines) + "\n", newline="\n")

    # The same symbols in the wasm assembler's dialect (PLATFORM=web): it has no .bss directive and wants every data
    # symbol's size, so the image is in .data with its size after its bytes, and a zero-size end label of its own. The
    # alias form is the same: `.set name, __retail_image + offset` resolves there too, and the loader reads the image
    # through it exactly as it does on x86
    lines = [
        "// Generated by tools/retail_map.py --emit, from tools/retail_symbols.h. Do not edit.",
        "\t.data",
        "\t.balign 64",
        "\t.globl __retail_image",
        "\t.globl __retail_image_end",
        "\t.type __retail_image,@object",
        "\t.type __retail_image_end,@object",
        "__retail_image:",
        f"\t.space 0x{end - base:X}",
        "__retail_image_end:",
        f"\t.size __retail_image, 0x{end - base:X}",
        "\t.size __retail_image_end, 0",
        "",
    ]
    for address, name in in_image:
        lines.append(f"\t.globl {name}")
        lines.append(f"\t.set {name}, __retail_image + 0x{address - base:X}")
    (directory / "retail_data_symbols_wasm.s").write_text("\n".join(lines) + "\n", newline="\n")

    extents = [
        "// Generated by tools/retail_map.py --emit, from tools/retail_symbols.h. Do not edit.",
        "#pragma once",
        "",
        '#include "common.h"',
        "",
        "// The image's own extent, as the linker places it (retail_data_symbols.s is its bytes)",
        "namespace RetailImage",
        "{",
        "struct Extent",
        "{",
        "    u8* base;",
        "    u32 size;",
        "};",
        "extern const Extent Image;",
        "}",
        "",
    ]
    (directory / "retail_image.h").write_text("\n".join(extents), newline="\n")
    print(f"emit: {len(code)} code symbols, {len(in_image)} data symbols in the image ({len(data) - len(in_image)} "
          f"below it, in text) -> {directory}")


def check(symbols, relocations):
    """The split's own pointer words against the map: every word of the retail executable that the split relocated has to
    be a word this map can find, or the loader would leave it pointing into the retail program's addresses."""
    if not EXECUTABLE.exists():
        print(f"check: no {EXECUTABLE}" if EXECUTABLE.name else "check: no executable")
        return 1
    if not (HERE / "build" / "host" / "data").is_dir():
        print("check: no build/host/data: build the baked host build first (this check needs the split's relocations)")
        return 1
    executable = EXECUTABLE.read_bytes()
    addresses = {address for address, _, _ in symbols}

    # Every pointer the split made, as an address in the image
    expected = set()
    for path in (HERE / "build" / "host" / "data").glob("*.o"):
        data = path.read_bytes()
        shoff = struct.unpack_from("<I", data, 0x20)[0]
        shentsize = struct.unpack_from("<H", data, 0x2E)[0]
        shnum = struct.unpack_from("<H", data, 0x30)[0]
        shstr = struct.unpack_from("<H", data, 0x32)[0]
        sections = []
        for index in range(shnum):
            fields = struct.unpack_from("<IIIIIIIIII", data, shoff + index * shentsize)
            sections.append({"name_index": fields[0], "type": fields[1], "offset": fields[4], "size": fields[5],
                             "info": fields[7]})
        names = data[sections[shstr]["offset"]:sections[shstr]["offset"] + sections[shstr]["size"]]

        def cstring(offset):
            end = names.find(b"\0", offset)
            return names[offset:end].decode("ascii", "replace")

        for section in sections:
            section["name"] = cstring(section["name_index"])
        by_name = {section["name"]: section for section in sections}
        table = by_name.get(".symtab")
        strings = data[by_name[".strtab"]["offset"]:by_name[".strtab"]["offset"] + by_name[".strtab"]["size"]]
        syms = []
        for index in range(table["size"] // 16):
            name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", data, table["offset"] + index * 16)
            end = strings.find(b"\0", name)
            syms.append({"name": strings[name:end].decode("ascii", "replace"), "value": value, "shndx": shndx})
        for rel in sections:
            if rel["type"] != 9 or rel["info"] >= len(sections):
                continue
            base = section_base(path.stem, sections[rel["info"]]["name"])
            for entry in range(rel["size"] // 8):
                offset, info = struct.unpack_from("<II", data, rel["offset"] + entry * 8)
                expected.add(base + offset)

    # Every word the map can find, by the value it holds: a symbol's address
    found = set()
    addends = 0
    for address in range(IMAGE_BASE, DATA_END - 3, 4):
        value = struct.unpack_from("<I", executable, 0x1000 + address - 0x100000)[0]
        if value in addresses:
            found.add(address)
        elif IMAGE_BASE <= value < IMAGE_END:
            # A pointer into the data at an address no symbol names (a table's own element, an auto label the split
            # knew): the loader moves every word in the data's range, so this is covered too
            found.add(address)
            addends += 1

    # An offset into a function: the loader has the table's word addresses and points them at the port's own function
    for word, symbol in relocations:
        value = struct.unpack_from("<I", executable, 0x1000 + word - 0x100000)[0]
        if value >= symbol:
            found.add(word)

    missing = sorted(expected - found)
    extra = sorted(found - expected)
    print(f"check: the split made {len(expected)} pointer words; the map finds {len(found)}; "
          f"{len(missing)} the map misses, {len(extra)} it adds")
    for address in missing[:10]:
        value = struct.unpack_from("<I", executable, 0x1000 + address - 0x100000)[0]
        print(f"check: missing 0x{address:08X} holds 0x{value:08X}")
    for address in extra[:10]:
        value = struct.unpack_from("<I", executable, 0x1000 + address - 0x100000)[0]
        print(f"check: extra 0x{address:08X} holds 0x{value:08X}")
    print(f"check: {addends} word(s) point inside the data without being a symbol's own address")
    return 0 if not missing else 1


def main():
    arguments = sys.argv[1:]
    if "--emit" in arguments:
        emit(arguments[arguments.index("--emit") + 1])
        return
    symbols = read_symbols()
    relocations = read_relocations(symbols)
    if "--check" in arguments:
        raise SystemExit(check(symbols, relocations))
    lines = write_header(symbols, relocations)
    code = sum(1 for _, _, is_code in symbols if is_code)
    print(f"{HEADER}: {len(symbols)} symbols ({code} code), {lines} lines")


if __name__ == "__main__":
    main()
