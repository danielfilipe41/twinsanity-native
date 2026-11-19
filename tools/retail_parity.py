#!/usr/bin/env python3
"""The runtime build's data image against the split's own data objects, word for word.

A `DATA=runtime` build carries none of the game's data and fills an image with the disc's at the first start
(src/platform/null/retaildata.cpp). A build from the split has the same data in the objects the split made, linked into the
executable. This compares the two: every word of the image has to be the object's word, and every word the split relocated
has to be the same symbol plus the same offset -- the port's own function for a code symbol, the image's same place for a
data one.

It needs a runtime build's dump and the baked objects (a normal host build makes them):

    build/host/sdl3/game-with-data --disc disc --dump-data build/host/retail-image.txt
    python3 tools/retail_parity.py build/host/sdl3/game-with-data build/host/retail-image.txt
"""
import struct
import subprocess
import sys
from pathlib import Path

import retail_map as retail

OBJECTS = ("data.data", "rodata.rodata", "sdata.sdata", "vudata.data", "sbss.sbss", "bss.bss")


def object_sections(path):
    data = path.read_bytes()
    shoff = struct.unpack_from("<I", data, 0x20)[0]
    shentsize = struct.unpack_from("<H", data, 0x2E)[0]
    shnum = struct.unpack_from("<H", data, 0x30)[0]
    shstr = struct.unpack_from("<H", data, 0x32)[0]
    sections = []
    for index in range(shnum):
        fields = struct.unpack_from("<IIIIIIIIII", data, shoff + index * shentsize)
        sections.append({"name_index": fields[0], "type": fields[1], "flags": fields[2], "offset": fields[4],
                         "size": fields[5]})
    names = data[sections[shstr]["offset"]:sections[shstr]["offset"] + sections[shstr]["size"]]
    for section in sections:
        end = names.find(b"\0", section["name_index"])
        section["name"] = names[section["name_index"]:end].decode("ascii", "replace")
    return data, sections


def read_dump(path):
    words = {}
    for line in Path(path).read_text().splitlines():
        if line:
            address, value = line.split()
            words[int(address, 16)] = int(value, 16)
    return words


def nm_addresses(binary):
    addresses = {}
    result = subprocess.run(["nm", binary], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            try:
                addresses.setdefault(parts[2], int(parts[0], 16))
            except ValueError:
                continue
    return addresses


def retail_check():
    """The split's objects against the disc's own bytes: every data word the split did not relocate has to be the disc's."""
    executable = retail.EXECUTABLE.read_bytes()
    relocations = dict(retail.read_relocations(retail.read_symbols()))
    checked = 0
    differ = []
    for stem in OBJECTS:
        path = retail.HERE / "build" / "host" / "data" / (stem + ".o")
        if not path.is_file():
            print(f"retail: no {path}: build the data-bearing host build first (it makes the split's objects)")
            return 1
        data, sections = object_sections(path)
        for section in sections:
            if section["name"] not in (".data", ".rodata", ".sdata") or section["size"] == 0:
                continue
            base = retail.section_base(stem, section["name"])
            for index in range(section["size"] // 4):
                address = base + index * 4
                if address in relocations or address >= retail.DATA_END:
                    continue
                literal = struct.unpack_from("<I", data, section["offset"] + index * 4)[0]
                actual = struct.unpack_from("<I", executable, 0x1000 + address - 0x100000)[0]
                checked += 1
                if literal != actual:
                    differ.append((address, literal, actual, stem, section["name"]))
    for address, literal, actual, stem, name in differ[:10]:
        print(f"retail: {stem}{name} 0x{address:08X}: object 0x{literal:08X}, disc 0x{actual:08X}")
    print(f"retail: {checked} data words, {len(differ)} differ from the disc's bytes")
    return 0 if not differ else 1


def main(argv):
    if "--retail" in argv:
        return retail_check()
    if len(argv) < 2:
        raise SystemExit("parity: expected <binary> <dump>")
    binary, dump_path = argv[0], argv[1]

    symbols = retail.read_symbols()
    relocations = dict(retail.read_relocations(symbols))
    by_address = {}
    for address, name, code in symbols:
        by_address.setdefault(address, []).append((name, code))
    hosts = nm_addresses(binary)

    image_host = hosts.get("__retail_image")
    if image_host is None:
        # The image's own base, from any data symbol the linker placed in it
        for address, name, code in symbols:
            if not code and address >= retail.IMAGE_BASE and name in hosts:
                image_host = hosts[name] - (address - retail.IMAGE_BASE)
                break
    if image_host is None:
        raise SystemExit("parity: the binary has no image: is it a DATA=runtime build?")

    def host_of(address):
        # A data symbol in the image is the image's own bytes; anything else is the port's symbol by name
        if address >= retail.IMAGE_BASE:
            return image_host + (address - retail.IMAGE_BASE)
        for name, code in sorted(by_address.get(address, ()), key=lambda pair: not pair[1]):
            if name in hosts:
                return hosts[name]
        return None

    dump = read_dump(dump_path)
    # A relocated word has to resolve exactly as the split's relocation does; the rest of the image is the disc's bytes,
    # which the split's own objects differ from where splat zeroed or shifted them (a baked build's data has that too)
    relocated = 0
    unnamed = 0
    differ_relocated = []
    drift = 0
    drift_examples = []
    for stem in OBJECTS:
        path = retail.HERE / "build" / "host" / "data" / (stem + ".o")
        if not path.is_file():
            print(f"parity: no {path}: build the data-bearing host build first (it makes the split's objects)")
            return 1
        data, sections = object_sections(path)
        for section in sections:
            if section["name"] not in (".data", ".rodata", ".sdata", ".sbss", ".bss") or section["size"] == 0:
                continue
            base = retail.section_base(stem, section["name"])
            no_bytes = section["type"] == 8
            for index in range(section["size"] // 4):
                address = base + index * 4
                literal = 0 if no_bytes else struct.unpack_from("<I", data, section["offset"] + index * 4)[0]
                actual = dump.get(address)
                if address in relocations:
                    host = host_of(relocations[address])
                    if host is None:
                        unnamed += 1
                        continue
                    relocated += 1
                    expected = (host + literal) & 0xFFFFFFFF
                    if actual != expected:
                        differ_relocated.append((address, expected, actual, stem, section["name"]))
                elif actual != literal:
                    drift += 1
                    if len(drift_examples) < 5:
                        drift_examples.append((address, literal, actual, stem, section["name"]))
    for address, expected, actual, stem, name in differ_relocated[:10]:
        actual_text = "missing" if actual is None else f"0x{actual:08X}"
        print(f"parity: {stem}{name} 0x{address:08X}: split's 0x{expected:08X}, image {actual_text}")
    for address, literal, actual, stem, name in drift_examples:
        actual_text = "missing" if actual is None else f"0x{actual:08X}"
        print(f"parity: split drift {stem}{name} 0x{address:08X}: object 0x{literal:08X}, image {actual_text}")
    print(f"parity: {relocated} relocated words, {len(differ_relocated)} differ; {drift} other words differ from the "
          f"split's objects (the split's own zeroing or shift: the image holds the disc's bytes); {unnamed} name no host "
          f"symbol")
    return 0 if not differ_relocated and unnamed == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
