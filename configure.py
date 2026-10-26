#!/usr/bin/env python3
"""Writes build.ninja and build/compile_commands.json. The retail executable is split into asm first (tools/split.py), then
`ninja` assembles it, compiles src/ and links build/SLES_525.68.elf with PS2SDK in place of the Sony SDK functions ps2sdk.txt
lists.

    python configure.py [--matching]

--matching links the asm alone (no src/, no PS2SDK), for `ninja check`, which compares the load image with the retail one.
PLATFORM (an environment variable, ps2 by default) picks the platform layer's side in src/platform/. Where the toolchain is comes
from tools/local_config.py ($PS2DEV, local.json, else /usr/local/ps2dev or C:/ps2dev). The commands run the tools with the
Python running this script, so the same works on Linux and Windows (where ninja runs commands without a shell).
"""
import json
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "tools"))
import local_config
import toolchain_dlls

WINDOWS = os.name == "nt"
PS2SDK = local_config.ps2sdk()

# The asm keeps the game's own calling convention (the EABI with 64 bit registers, float arguments from $f12 on), but it's
# assembled as n32, the ABI of the C++ and PS2SDK's libraries, since the linker takes one ABI. splat names registers the n32 way
ASFLAGS = ["-EL", "-march=r5900", "-mabi=n32", "-msingle-float", "-G0", "-no-pad-sections", "-I", "include", "-I", "."]
# C++ is PS2SDK's n32: EABI's stack is only kept 8 byte aligned by GCC, and the retail code (lq/sq) needs 16. The two agree on
# integer and pointer arguments; calls mixing ints and floats go through thunks (see docs/DEVELOPMENT.md). The retail code
# expects $f20-$f31 kept across calls, n32 only the even ones: GCC 15 accepts -fcall-saved-$f21... and still uses the odd
# ones without saving them (RigidBody::Step clobbered StepPhysicsWorld's $f21), so the C++ doesn't use them at all. C++26 for
# asm statements made of constant expressions (abi.h's thunks). The retail code's divisions don't trap on 0
CALL_SAVED_FPRS = [f"-ffixed-$f{n}" for n in range(21, 32, 2)]
CXXFLAGS = (["-march=r5900", "-mabi=n32", "-msingle-float", "-mno-abicalls", "-G0", "-O2", "-std=gnu++26", "-D_EE",
             "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics", "-fno-asynchronous-unwind-tables", "-fno-common",
             "-fno-strict-aliasing", "-ffunction-sections", "-mno-check-zero-division", "-ffp-contract=off",
             "-fno-delete-null-pointer-checks", "-Wall", "-Wno-invalid-offsetof"] + CALL_SAVED_FPRS +
            ["-Iinclude", f"-I{PS2SDK}/ee/include", f"-I{PS2SDK}/common/include"])

PLATFORM = os.environ.get("PLATFORM", "ps2")

# Host builds (PLATFORM=null/sdl3): 32-bit x86 keeps the retail 32-bit-pointer layouts, so CHECK_SIZE/OFFSET hold.
# -malign-double matches the MIPS ABI's 8-byte doubles; SSE2 avoids x87 extended precision. No -mno-check-zero-division:
# x86 idiv traps on zero by hardware (retail didn't); game code guards its divisors in practice. -no-pie: the retail data
# holds absolute addresses (jump tables, baked pointers) that only make sense at a fixed load base
HOST = PLATFORM in ("null", "sdl3")
HOST_CXXFLAGS = (["-m32", "-malign-double", "-msse2", "-mfpmath=sse", "-O2", "-g", "-std=gnu++26", "-fno-pie",
                  "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics", "-fno-asynchronous-unwind-tables",
                  "-fno-common", "-fno-strict-aliasing", "-ffunction-sections",
                  "-ffp-contract=off", "-fno-delete-null-pointer-checks", "-Wall", "-Wno-invalid-offsetof",
                  # A crash under gdb wants a real stack: the host build keeps frame pointers and unwind info
                  "-fasynchronous-unwind-tables", "-fno-omit-frame-pointer",
                  "-Iinclude", "-Iinclude/host"])


def quote(argument):
    """An argument of a ninja command: POSIX ninja runs commands through /bin/sh, Windows' straight through CreateProcess"""
    if WINDOWS:
        return f'"{argument}"' if re.search(r'[\s"]', argument) else argument

    return argument if re.fullmatch(r"[\w@%+=:,./-]+", argument) else "'" + argument.replace("'", "'\\''") + "'"


def command(arguments):
    return " ".join(quote(argument).replace("$", "$$") for argument in arguments)


def toolchain_library_dirs():
    """Where the toolchain's newlib and libgcc are: the link runs ld, which isn't told by gcc's driver"""
    dirs = []
    for name in ("libc_nano.a", "libgcc.a"):
        try:
            result = subprocess.run([local_config.ee_tool("gcc"), f"-print-file-name={name}"], capture_output=True, text=True,
                                    timeout=60)
        except (OSError, subprocess.TimeoutExpired):
            continue

        path = Path(result.stdout.strip())
        if path.is_absolute() and path.exists():
            dirs.append(Path(os.path.normpath(path.parent)).as_posix())

    return dirs


def toolchain_includes():
    """Where the toolchain's g++ looks for its own headers (the C++ library's, newlib's), for the editors' code models: clangd
    (with another target) and VS Code's C/C++ extension don't ask a cross compiler for them"""
    try:
        result = subprocess.run([local_config.ee_tool("g++"), "-E", "-v", "-x", "c++", os.devnull], capture_output=True,
                                text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return []

    lines = result.stderr.splitlines()
    if "#include <...> search starts here:" not in lines:
        return []

    start = lines.index("#include <...> search starts here:") + 1
    end = lines.index("End of search list.", start)
    return [Path(os.path.normpath(line.strip())).as_posix() for line in lines[start:end]]


def sdl_flags():
    """SDL3 compile and link flags for a 32-bit host build: $SDL_PREFIX (headers + 32-bit library) when set, else the
    system's SDL3."""
    prefix = os.environ.get("SDL_PREFIX", "/usr")
    return [f"-I{prefix}/include/SDL3"], [f"-L{prefix}/lib32" if (Path(prefix) / "lib32").is_dir() else f"-L{prefix}/lib",
                                          "-lSDL3"]


def bgfx_flags():
    """bgfx+bx+bimg compile and link flags for a 32-bit host build. The libraries are built once from third_party/
    (bgfx's GENie flow: genie --gcc=linux-gcc gmake, then make config=release32 in third_party/bgfx/.build/projects/
    gmake-linux-gcc; the toolchain script already passes -m32 for the x32 platform) into .build/linux32_gcc/bin;
    SDL3 headers come from third_party/SDL, the link from the system's 32-bit SDL3."""
    bgfx_lib = HERE / "third_party" / "bgfx" / ".build" / "linux32_gcc" / "bin"
    cflags = ["-DBX_CONFIG_DEBUG=0",
              "-Ithird_party/bgfx/include", "-Ithird_party/bx/include", "-Ithird_party/bimg/include",
              "-Ithird_party/SDL/include"]
    libs = [f"-L{bgfx_lib.as_posix()}", "-lbgfxRelease", "-lbimgRelease", "-lbimg_decodeRelease", "-lbxRelease",
            "-lX11", "-lGL", "-lpthread", "-ldl", "-lrt"]
    return cflags, libs


def main_host():
    """Host build (PLATFORM=null/sdl3): compiles the game's C++ and the platform side for 32-bit x86 with the system
    compiler. No asm split, no PS2SDK, no retail link script: `ninja compile` checks every TU compiles. The game links
    against the host platform layer once the data converter supplies the retail data as host objects."""
    cxx = command(["g++"])
    # The sdl3 platform's entry includes SDL3 and links it; null needs neither. The bgfx context's headers and the
    # prebuilt bgfx+bx+bimg static libraries join both compiles and links on sdl3, so bgfx_context.cpp builds with the
    # game and the standalone test links the same libraries
    sdl_cflags, sdl_libs = sdl_flags() if PLATFORM == "sdl3" else ([], [])
    bgfx_cflags, bgfx_libs = bgfx_flags() if PLATFORM == "sdl3" else ([], [])
    lines = [
        f"# host platform: {PLATFORM}",
        "ninja_required_version = 1.10",
        f"cxx = {cxx}",
        f"cxxflags = {command(HOST_CXXFLAGS + sdl_cflags + bgfx_cflags)}",
        f"ldflags = {command(['-m32', '-no-pie', '-rdynamic'] + sdl_libs + bgfx_libs)}",
        "",
        "rule cxx",
        "  command = $cxx $cxxflags -MMD -MF $out.d -c -o $out $in",
        "  depfile = $out.d",
        "  deps = gcc",
        "  description = CXX $in",
        "rule link",
        "  command = $cxx -o $out $in $ldflags",
        "  description = LINK $out",
        "rule cxxlink",
        "  command = $cxx $cxxflags $ldflags -o $out $in",
        "  description = CXXLINK $in",
        "rule configure",
        "  command = python3 configure.py",
        "  generator = 1",
        "",
    ]
    src = sorted(path.relative_to(HERE).as_posix() for path in (HERE / "src").rglob("*.cpp")
                 if (path.relative_to(HERE / "src").parts[:1] != ("platform",)
                     or path.relative_to(HERE / "src").parts[1] == PLATFORM)
                 # The standalone tests have their own mains; they build as build/host/bgfx_test,
                 # build/host/draw2d_test and build/host/screenfx_test, not into the game
                 and path.name not in ("bgfx_test.cpp", "draw2d_test.cpp", "screenfx_test.cpp", "shadows_test.cpp",
                                       "particles_test.cpp"))
    # The sdl3 platform starts on the null platform's stubs (its own entry opens the window): every null file compiles
    # too, except the ones sdl3 replaces with its own — same-named files anywhere under sdl3/, plus sound.cpp whose
    # Sound::InitialiseRemote lives in sdl3/audio/audio.cpp next to the mixer
    if PLATFORM == "sdl3":
        sdl3_names = {path.name for path in (HERE / "src" / "platform" / "sdl3").rglob("*.cpp")} | {"sound.cpp"}
        src += sorted(path.relative_to(HERE).as_posix() for path in (HERE / "src" / "platform" / "null").rglob("*.cpp")
                      if path.name not in sdl3_names)
    src_objects = []
    compile_commands = []
    for path in src:
        obj = f"build/host/{PLATFORM}/{path}.o"
        src_objects.append(obj)
        lines.append(f"build {obj}: cxx {path}")
        compile_commands.append({"directory": HERE.as_posix(), "file": path, "output": obj,
                                 "arguments": ["g++"] + HOST_CXXFLAGS + sdl_cflags + bgfx_cflags + ["-c", "-o", obj, path]})
    data_objects = []
    if (HERE / "asm" / "data").is_dir():
        lines += [
            "rule hostdata",
            "  command = python3 tools/host_data.py --file $in $out.host.s && "
            "$cxx -m32 -c $out.host.s -o $out",
            "  description = HOSTDATA $in",
        ]
        data_objects = []
        for path in sorted((HERE / "asm" / "data").rglob("*.s")):
            obj = f"build/host/data/{path.stem}.o"
            data_objects.append(obj)
            lines.append(f"build {obj}: hostdata {path.relative_to(HERE).as_posix()} | tools/host_data.py")
        lines += [
            f"build build/host/{PLATFORM}/game-with-data: link {' '.join(src_objects + data_objects)}",
            "build game: phony " + f"build/host/{PLATFORM}/game-with-data",
        ]
    lines += [
        f"build build/host/{PLATFORM}/game: link {' '.join(src_objects)}",
        f"build compile: phony {' '.join(src_objects)}",
        "build build/host/vtable_test: cxxlink tools/host_vtable_test.cpp",
        "build vtable-test: phony build/host/vtable_test",
        f"build build.ninja: configure | configure.py",
        f"default compile",
        "",
    ]
    if PLATFORM == "sdl3":
        # The standalone bgfx test: its own SDL main plus the platform's bgfx context, linked against the prebuilt
        # bgfx+bx+bimg static libraries (bgfx_libs rides in ldflags); reachable via `ninja bgfx-test`
        bgfx_context_obj = "build/host/sdl3/src/platform/sdl3/graphics/bgfx_context.cpp.o"
        # draws_host.cpp now composes the light directions into each model's space (VuTransposeRotation /
        # VuRotateVector), so the host VU maths rides along in every test that links it
        vumath_obj = ("build/host/sdl3/src/platform/null/vumath.cpp.o "
                      "build/host/sdl3/src/platform/null/math.cpp.o")
        lines += [
            "build build/host/sdl3/src/platform/sdl3/graphics/bgfx_test.cpp.o: cxx "
            "src/platform/sdl3/graphics/bgfx_test.cpp",
            f"build build/host/bgfx_test: link build/host/sdl3/src/platform/sdl3/graphics/bgfx_test.cpp.o "
            f"{bgfx_context_obj}",
            "build bgfx-test: phony build/host/bgfx_test",
            # The draw2d headless test: the 2D draws and the material system through the same context and the
            # sdl3 graphics layer (which carries the null stubs for everything else); `ninja draw2d-test`
            "build build/host/sdl3/src/platform/sdl3/graphics/draw2d_test.cpp.o: cxx "
            "src/platform/sdl3/graphics/draw2d_test.cpp",
            f"build build/host/draw2d_test: link build/host/sdl3/src/platform/sdl3/graphics/draw2d_test.cpp.o "
            f"{bgfx_context_obj} {vumath_obj} "
            "build/host/sdl3/src/platform/sdl3/graphics/draws_host.cpp.o "
            "build/host/sdl3/src/platform/sdl3/graphics/mesh_builder.cpp.o "
            "build/host/sdl3/src/platform/sdl3/graphics/draw2d.cpp.o "
            "build/host/sdl3/src/platform/sdl3/graphics/material_system.cpp.o "
            "build/host/sdl3/src/platform/sdl3/graphics/texture_decoder.cpp.o",
            "build draw2d-test: phony build/host/draw2d_test",
            # The screenfx headless test: the depth-as-palette filter through the same context (the R32F depth
            # pre-pass, the palette indexed by depth, the checksum over the readback); `ninja screenfx-test`
            "build build/host/sdl3/src/platform/sdl3/graphics/screenfx_test.cpp.o: cxx "
            "src/platform/sdl3/graphics/screenfx_test.cpp",
            f"build build/host/screenfx_test: link build/host/sdl3/src/platform/sdl3/graphics/screenfx_test.cpp.o "
            # graphics.cpp's frustum/particle-page readers reach the game's own symbols (G_RendRel, the tables,
            # TextureFromStream), so the proof links the whole game (minus the platform entry, whose main this proof
            # replaces) the way texture-material-proof does, rather than a curated subset
            + " ".join(obj for obj in src_objects + data_objects if not obj.endswith("entry.cpp.o")),
            "build screenfx-test: phony build/host/screenfx_test",
            # The draws headless test: one rigid + one skinned mesh from beach.rm2 through the game-facing draws on
            # bgfx's Noop renderer (mesh_builder parse -> buffer cache -> uniforms -> submit); `ninja draws-test`
            "build build/host/tools/draws_proof.o: cxx tools/draws_proof.cpp",
            f"build build/host/draws_proof: link build/host/tools/draws_proof.o "
            # Same as the other proofs: graphics.cpp reaches the game's symbols, so the whole game links (minus the
            # platform entry this proof's main replaces)
            + " ".join(obj for obj in src_objects + data_objects if not obj.endswith("entry.cpp.o")),
            "build draws-test: phony build/host/draws_proof",
            # The shadows headless test: the half-res offscreen target, the synthetic volume mesh through the
            # rigid-mesh path and the reverse-subtract resolve, with the frame read back and checksummed before
            # and after (nothing in the run reaches graphics.cpp's stubs, so it isn't linked); `ninja shadows-test`
            "build build/host/sdl3/src/platform/sdl3/graphics/shadows_test.cpp.o: cxx "
            "src/platform/sdl3/graphics/shadows_test.cpp",
            f"build build/host/shadows_test: link build/host/sdl3/src/platform/sdl3/graphics/shadows_test.cpp.o "
            f"{bgfx_context_obj} {vumath_obj} "
            "build/host/sdl3/src/platform/sdl3/graphics/shadows.cpp.o "
            "build/host/sdl3/src/platform/sdl3/graphics/draws_host.cpp.o "
            "build/host/sdl3/src/platform/sdl3/graphics/mesh_builder.cpp.o "
            "build/host/sdl3/src/platform/sdl3/graphics/draw2d.cpp.o "
            "build/host/sdl3/src/platform/sdl3/graphics/material_system.cpp.o "
            "build/host/sdl3/src/platform/sdl3/graphics/texture_decoder.cpp.o",
            "build shadows-test: phony build/host/shadows_test",
            # The particles headless test: a chain of particle blocks, a system's render table, an aged decal block
            # and a frame copy through the host's DrawParticleBlock/DrawDecals/DrawDistortionBlock, printing each
            # one's vertices and checksum; `ninja particles-test`
            "build build/host/sdl3/src/platform/sdl3/graphics/particles_test.cpp.o: cxx "
            "src/platform/sdl3/graphics/particles_test.cpp",
            f"build build/host/particles_test: link build/host/sdl3/src/platform/sdl3/graphics/particles_test.cpp.o "
            # Same as screenfx-test: graphics.cpp reaches the game's symbols, so the whole game links (minus the
            # platform entry this proof's main replaces)
            + " ".join(obj for obj in src_objects + data_objects if not obj.endswith("entry.cpp.o")),
            "build particles-test: phony build/host/particles_test",
            # The material/texture linkage proof: a real chunk's textures and materials through the game's own
            # ReadTexture/ReadMaterial and the SubmitQuad bind path, headless on bgfx's Noop renderer. Links the whole
            # game (minus the platform entry, whose main the proof replaces) so the graphics tables and the resources'
            # streams are the real ones; `ninja texture-material-proof`
            "build build/host/tools/texture_material_proof.o: cxx tools/texture_material_proof.cpp",
            f"build build/host/texture_material_proof: link "
            + " ".join(obj for obj in src_objects if not obj.endswith("entry.cpp.o"))
            + " " + " ".join(data_objects) + " build/host/tools/texture_material_proof.o",
            "build texture-material-proof: phony build/host/texture_material_proof",
            # The particle pages' picture proof: the default chunk's particle section's three page records and the
            # texture each page's material samples (through the game's own g_MaterialTable.Acquire and ReadMaterial),
            # written out as a PPM so the page's art can be looked at; `ninja page-proof`
            "build build/host/tools/pageproof.o: cxx tools/pageproof.cpp",
            f"build build/host/page_proof: link "
            + " ".join(obj for obj in src_objects if not obj.endswith("entry.cpp.o"))
            + " " + " ".join(data_objects) + " build/host/tools/pageproof.o",
            "build page-proof: phony build/host/page_proof",
            "",
        ]
        compile_commands.append({"directory": HERE.as_posix(), "file": "src/platform/sdl3/graphics/bgfx_test.cpp",
                                 "output": "build/host/sdl3/src/platform/sdl3/graphics/bgfx_test.cpp.o",
                                 "arguments": ["g++"] + HOST_CXXFLAGS + sdl_cflags + bgfx_cflags +
                                 ["-c", "-o", "build/host/sdl3/src/platform/sdl3/graphics/bgfx_test.cpp.o",
                                  "src/platform/sdl3/graphics/bgfx_test.cpp"]})
        # The audio mixer headless proof: the SPU mixer driven with real disc data (a bank sound's voice and a music
        # track's region), its output measured (RMS, peak, non-silence, clipping, frame-boundary jumps) and printed;
        # `ninja audio-mixer-proof` then run build/host/audio_mixer_proof from the repo root
        lines += [
            "build build/host/tools/audio_mixer_proof.o: cxx tools/audio_mixer_proof.cpp",
            f"build build/host/audio_mixer_proof: link build/host/tools/audio_mixer_proof.o "
            "build/host/sdl3/src/platform/sdl3/audio/spu_mixer.cpp.o "
            "build/host/sdl3/src/platform/sdl3/audio/adpcm.cpp.o "
            "build/host/sdl3/src/platform/sdl3/settings.cpp.o",
            "build audio-mixer-proof: phony build/host/audio_mixer_proof",
            "",
        ]
        compile_commands.append({"directory": HERE.as_posix(), "file": "tools/audio_mixer_proof.cpp",
                                 "output": "build/host/tools/audio_mixer_proof.o",
                                 "arguments": ["g++"] + HOST_CXXFLAGS + sdl_cflags + bgfx_cflags +
                                 ["-c", "-o", "build/host/tools/audio_mixer_proof.o", "tools/audio_mixer_proof.cpp"]})
        # The saves proof: drives the host Platform::Saves (src/platform/null/saves.cpp, the card as a directory of the
        # host's filesystem) through the game's save sequence (Initialise/Format/CreateSave/Write/Read/FindFile/MeasureSave),
        # the error paths and a fresh-process re-read, printing PASS/FAIL a step; `ninja saves-proof` then run
        # build/host/saves_proof from the repo root (the card is $XDG_DATA_HOME/twinsanity)
        lines += [
            "build build/host/tools/saves_proof.o: cxx tools/saves_proof.cpp",
            f"build build/host/saves_proof: link build/host/tools/saves_proof.o "
            f"build/host/{PLATFORM}/src/platform/null/saves.cpp.o",
            "build saves-proof: phony build/host/saves_proof",
            "",
        ]
        compile_commands.append({"directory": HERE.as_posix(), "file": "tools/saves_proof.cpp",
                                 "output": "build/host/tools/saves_proof.o",
                                 "arguments": ["g++"] + HOST_CXXFLAGS + sdl_cflags + bgfx_cflags +
                                 ["-c", "-o", "build/host/tools/saves_proof.o", "tools/saves_proof.cpp"]})
    (HERE / "build.ninja").write_text("\n".join(lines), newline="\n")


def main():
    if HOST:
        main_host()
        return

    toolchain_dlls.check()
    matching = "--matching" in sys.argv[1:]
    asm = sorted((path.relative_to(HERE).as_posix() for path in (HERE / "asm").rglob("*.s")))
    # The platform layer's side for the platform built for (src/platform/<platform>/), and none of the others
    src = sorted(path.relative_to(HERE).as_posix() for path in (HERE / "src").rglob("*.cpp")
                 if path.relative_to(HERE / "src").parts[:1] != ("platform",)
                 or path.relative_to(HERE / "src").parts[1] == PLATFORM)
    if matching:
        src = []
    if not asm:
        raise SystemExit("There's no asm yet: run tools/split.py first")

    python = command([sys.executable, "-X", "utf8"])
    libs = "" if matching else " ".join("-l" + lib for lib in LIBRARIES)
    lines = [
        f"# host: {os.name}",
        "ninja_required_version = 1.10",
        f"as = {command([local_config.ee_tool('as')])}",
        f"cxx = {command([local_config.ee_tool('g++')])}",
        f"ld = {command([local_config.ee_tool('ld')])}",
        f"nm = {command([local_config.ee_tool('nm')])}",
        f"python = {python}",
        f"asflags = {command(ASFLAGS)}",
        f"libdir = {command(['-L' + directory for directory in [PS2SDK + '/ee/lib'] + toolchain_library_dirs()])}",
        f"libs = {libs}",
        f"cxxflags = {command(CXXFLAGS)}",
        "",
        "rule as",
        "  command = $as $asflags -o $out $in",
        "  description = AS $in",
        "rule cxx",
        "  command = $cxx $cxxflags -MMD -MF $out.d -c -o $out $in",
        "  depfile = $out.d",
        "  deps = gcc",
        "  description = CXX $in",
        "rule ldscript",
        f"  command = $python tools/make_ld.py $out --nm $nm {'--matching ' if matching else ''}$in",
        "  description = LDSCRIPT $out",
        "rule link",
        "  command = $ld -m elf32lr5900n32 -T build/link.ld -Map build/SLES_525.68.map -o $out $libdir --start-group $libs --end-group",
        "  description = LINK $out",
        "rule check",
        "  command = $python tools/compare.py $in SLES_525.68 build/SLES_525.68.map --touch $out",
        "  description = CHECK $in",
        "rule configure",
        f"  command = $python configure.py{' --matching' if matching else ''}",
        "  generator = 1",
        "",
    ]
    asm_objects = []
    for path in asm:
        obj = f"build/{path}.o"
        asm_objects.append(obj)
        lines.append(f"build {obj}: as {path} | include/macro.inc")

    src_objects = []
    compile_commands = []
    system_includes = [argument for path in toolchain_includes() for argument in ("-isystem", path)] if src else []
    for path in src:
        obj = f"build/{path}.o"
        src_objects.append(obj)
        lines.append(f"build {obj}: cxx {path}")
        compile_commands.append({"directory": HERE.as_posix(), "file": path, "output": obj,
                                 "arguments": [local_config.ee_tool("g++")] + CXXFLAGS + system_includes +
                                              ["-c", "-o", obj, path]})

    lines += [
        f"build build/link.ld: ldscript {' '.join(src_objects)} | tools/make_ld.py ps2sdk.txt retired.txt fragments.txt build/splat.yaml build/undefined_syms_auto.txt",
        f"build build/SLES_525.68.elf: link | build/link.ld {' '.join(asm_objects)} {' '.join(src_objects)}",
        "build build/check.ok: check build/SLES_525.68.elf",
        "build check: phony build/check.ok",
        "build build.ninja: configure | configure.py tools/local_config.py",
        "default build/SLES_525.68.elf",
        "",
    ]
    (HERE / "build.ninja").write_text("\n".join(lines), newline="\n")
    # For the editors' code models (VS Code's C/C++ extension, clangd): the C++ as it's compiled. A matching configuration keeps
    # the last one, which has every file
    if src:
        (HERE / "build").mkdir(exist_ok=True)
        (HERE / "build" / "compile_commands.json").write_text(json.dumps(compile_commands, indent=1), newline="\n")

    print(f"build.ninja: {len(asm)} asm files, {len(src)} C++ files{', matching' if matching else ''}")


main()
