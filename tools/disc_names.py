#!/usr/bin/env python3
"""Says how a disc image's naming differs from the European disc's, and what the port needs to be told for it to play.

The port plays any of the game's releases by serving each disc's archive index under the game's own names (the European
release's): the platform's table in src/platform/null/files.cpp has one row per build, keyed by the serial at the disc's root.
This reads a disc's ISO9660 volume and its CRASH6/CRASH.BH index and prints what that row would have to say:

    python3 tools/disc_names.py "<your disc>.iso" [--reference path/to/CRASH.BH]

The reference is the European disc's archive index, `disc/CRASH6/CRASH.BH` when the working directory has one (the repo's own
layout). Nothing is written: this only reads, and it needs no game files of its own beyond the disc you point it at.

A disc whose serial is the European one (SLES_525.68) needs no row at all; one whose archive names match the reference's needs
none either. The names that differ are what the row carries: a bare name (the language: it is a folder of that language's
pictures and the stem of its texts) and a whole path (the font).
"""
import argparse
import struct
import sys
from pathlib import Path

SECTOR = 2048

# The European release's names, as the game's C++ and its data ask for them: the row's left-hand side
GAME_NAMES = {
    "language": "English",
    "font": r"StartUp\Fonts\Crash_Euro",
}


def le32(data, at):
    return struct.unpack_from("<I", data, at)[0]


class Volume:
    """An ISO9660 image's directories and files (2048-byte sectors; a raw 2352-byte .bin remapped on the way)."""

    def __init__(self, path):
        self.file = open(path, "rb")
        with open(path, "rb") as probe:
            head = probe.read(16)
        self.stride, self.user_offset = SECTOR, 0
        if len(head) == 16 and head[0] == 0 and head[1:11] == b"\xff" * 10 and head[11] == 0:
            # A raw dump: every logical sector carries a 12-byte sync, a 3-byte address and a mode byte first
            self.stride, self.user_offset = 2352, 24 if head[15] == 2 else 16
        descriptor = self._logical(16 * SECTOR, SECTOR)
        if descriptor[0] != 1 or descriptor[1:6] != b"CD001":
            raise SystemExit(f"{path}: no ISO9660 volume descriptor at sector 16")
        self.root = (le32(descriptor, 158), le32(descriptor, 166))
        self.volume_id = descriptor[40:72].decode("latin-1").rstrip()
        self.serial = self._serial()

    def _logical(self, offset, size):
        out = bytearray()
        while len(out) < size:
            logical = offset + len(out)
            sector, inside = divmod(logical, SECTOR)
            take = min(size - len(out), SECTOR - inside)
            self.file.seek(sector * self.stride + self.user_offset + inside)
            block = self.file.read(take)
            if not block:
                break
            out += block
        return bytes(out)

    def records(self, lba, size):
        data = self._logical(lba * SECTOR, size)
        pos, out = 0, []
        while pos < size:
            length = data[pos]
            if length == 0:
                pos = (pos // SECTOR + 1) * SECTOR
                continue
            name = data[pos + 33:pos + 33 + data[pos + 32]].decode("latin-1")
            out.append((name.split(";")[0], le32(data, pos + 2), le32(data, pos + 10), bool(data[pos + 25] & 2)))
            pos += length
        return out

    def find(self, path):
        lba, size = self.root
        for part in [p for p in path.replace("\\", "/").split("/") if p]:
            for name, entry_lba, entry_size, _ in self.records(lba, size):
                if name.upper() == part.upper():
                    lba, size = entry_lba, entry_size
                    break
            else:
                return None
        return lba, size

    def read(self, path):
        found = self.find(path)
        if found is None:
            return None
        return self._logical(found[0] * SECTOR, found[1])

    def listing(self, path=""):
        found = self.find(path) if path else (self.root[0], self.root[1])
        return [] if found is None else [(name, size) for name, _, size, _ in self.records(*found)]

    def _serial(self):
        for name, _ in self.listing():
            if name.upper().startswith(("SLES_", "SLUS_", "SLPM_", "SLED_", "SLPS_")):
                return name
        return None


def archive_names(index):
    """A CRASH.BH's entry names: a header word, then {length, name, offset, size} an entry."""
    names = []
    if index is None or len(index) < 4:
        return names
    count = le32(index, 0)
    at = 4
    for _ in range(count):
        if at + 4 > len(index):
            break
        length = le32(index, at)
        if length == 0 or length > 1024 or at + 12 + length > len(index):
            break
        names.append(index[at + 4:at + 4 + length].decode("latin-1"))
        at += 12 + length
    return names


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", help="the disc image (.iso, or a raw .bin)")
    parser.add_argument("--reference", help="the European disc's CRASH.BH (default: disc/CRASH6/CRASH.BH here)")
    arguments = parser.parse_args()

    reference_path = arguments.reference or str(Path("disc/CRASH6/CRASH.BH"))
    try:
        reference = archive_names(Path(reference_path).read_bytes())
    except OSError:
        raise SystemExit(f"no reference archive at {reference_path}: pass --reference with your European disc's CRASH.BH")
    if not reference:
        raise SystemExit(f"{reference_path} does not read as an archive index")

    volume = Volume(arguments.image)
    index = volume.read("CRASH6/CRASH.BH")
    if index is None:
        raise SystemExit(f"{arguments.image}: no CRASH6/CRASH.BH in it, so it is not this game's disc")
    names = archive_names(index)

    print(f"{arguments.image}")
    print(f"  volume \"{volume.volume_id}\", serial {volume.serial or 'none found'}, {len(names)} archive entries")
    print("\n  files at its root and in CRASH6:")
    for name, size in volume.listing():
        print(f"    {name:20s} {size}")
    for name, size in volume.listing("CRASH6"):
        print(f"    CRASH6/{name:20s} {size}")

    have = set(names)
    only_disc = [name for name in names if name not in set(reference)]
    only_reference = [name for name in reference if name not in have]
    print(f"\n  {len(only_reference)} of the European disc's names are not in this one; {len(only_disc)} of this one's are new:")
    for name in only_reference:
        print(f"    - {name}")
    for name in only_disc:
        print(f"    + {name}")

    if not only_disc and not only_reference:
        print("\n  Its naming is the European disc's: it needs no row of its own.")
        return

    # What the row carries: the language (as a bare name) and any font (a whole path), by what the two sides have in common
    def common(names_a, names_b, under):
        prefixed_a = [n for n in names_a if n.lower().startswith(under.lower())]
        prefixed_b = [n for n in names_b if n.lower().startswith(under.lower())]
        return prefixed_a, prefixed_b

    print("\n  what src/platform/null/files.cpp needs for this build (a row keyed by its serial):")
    language_old, language_new = common(only_reference, only_disc, "Language\\Code\\")
    if language_old and language_new:
        old = language_old[0].rsplit("\\", 1)[-1].split(".")[0]
        new = language_new[0].rsplit("\\", 1)[-1].split(".")[0]
        print(f'    {{"{old}", "{new}"}},   // the language')

    def by_kind(names_list, marker):
        return [n for n in names_list if marker.lower() in n.lower()]

    for marker in ("Fonts\\",):
        old = by_kind(only_reference, "Fonts\\")
        new = by_kind(only_disc, "Fonts\\")
        if old and new:
            old_path = old[0].rsplit(".", 1)[0]
            new_path = new[0].rsplit(".", 1)[0]
            print(f'    {{"{old_path}", "{new_path}"}},   // the font (whole paths, extension kept either way)')
    print(f"\n    and the build's serial, which DiscBuild() reads: {volume.serial}")


if __name__ == "__main__":
    main()
