#!/usr/bin/env python3
"""Links the Windows build, bridging PE's leading underscore.

i686 PE spells a C symbol with a leading underscore, but the retail symbols here are named through asm labels
(RETAIL(name) is asm("name")) and the generated retail data keeps those names verbatim, so a definition and its
reference can end up on opposite sides of the underscore. Where only one spelling is defined, the other is
defined too, as a linker --defsym alias of it. Wraps the link command it is given:

    python3 tools/pe_link.py --nm i686-w64-mingw32-nm -- i686-w64-mingw32-g++ -o out.exe a.o b.o -l...

An alias is skipped when the object that defines its target also references the alias: that object is a forwarder
(src/platform/null/pe_libc.cpp defines asm("strlen") as a call to the CRT's _strlen), and its underscored reference
means the CRT library, not the forwarder it lives in. Aliasing it there links the forwarder to itself, which is a
jump to the same address: the executable starts and hangs before main.
"""
import subprocess
import sys


def nm_names(nm, objects, undefined):
    result = subprocess.run([nm, "-A"] + (["-u"] if undefined else ["--defined-only"]) + objects,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    # -A prefixes each line with the object file, one "path: 00000000 T name" (or "path:          U name") per symbol
    names = {}
    for line in result.stdout.splitlines():
        object_path, separator, rest = line.partition(":")
        if not separator:
            continue
        parts = rest.split()
        if not parts or (undefined and parts[0] != "U"):
            continue
        name = parts[-1]
        # Section and COFF pseudo symbols (".text$foo", ".data") have no counterpart on the other side
        if name.startswith("."):
            continue
        names.setdefault(object_path, set()).add(name)
    return names


def flatten(per_object):
    return set().union(*per_object.values()) if per_object else set()


def forwards_to_itself(defined_name, alias, defines, references):
    """The object defining defined_name references alias: an alias to it would close the loop onto itself."""
    return any(defined_name in names and alias in references.get(object_path, ())
               for object_path, names in defines.items())


def main(argv):
    if "--" not in argv:
        raise SystemExit("pe_link: expected --nm <nm> -- <link command>")
    split = argv.index("--")
    options = argv[:split]
    command = argv[split + 1:]
    if "--nm" not in options or not command:
        raise SystemExit("pe_link: expected --nm <nm> -- <link command>")
    nm = options[options.index("--nm") + 1]
    objects = [argument for argument in command if argument.endswith(".o")]

    defines = nm_names(nm, objects, undefined=False)
    references = nm_names(nm, objects, undefined=True)
    defined = flatten(defines)
    undefined = flatten(references)
    aliases = []
    for name in sorted(undefined):
        if name in defined:
            continue
        if name.startswith("_") and name[1:] in defined:
            target = name[1:]
        elif not name.startswith("_") and "_" + name in defined:
            target = "_" + name
        else:
            continue
        if forwards_to_itself(target, name, defines, references):
            continue
        aliases.append(f"--defsym={name}={target}")

    return subprocess.run(command + [f"-Wl,{alias}" for alias in aliases]).returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
