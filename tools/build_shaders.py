#!/usr/bin/env python3
"""Compiles the host renderer's bgfx shaders (src/platform/sdl3/graphics/shaders/*.sc) into the C headers the
platform includes (shaders/<name>.bin.h).

Each shader is compiled for GLSL (the renderer's BgfxInit picks RendererType::OpenGL), with its varying
definition; the output is shaderc's --bin2c array named "<name>_glsl" so the headers match the checked-in ones byte
for byte. This is the checked-in blobs' generator: run it after editing a .sc or a .def.sc.

    python tools/build_shaders.py            # regenerate every .bin.h
    python tools/build_shaders.py --check    # verify the checked-in blobs are current (no writes)

shaderc comes from the bgfx submodule's tools build (third_party/bgfx/.build/<config>/bin/shadercRelease); GLSL is
the default profile, so no -p is needed. `make tools` in third_party/bgfx builds it when missing.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SHADERS = ROOT / "src" / "platform" / "sdl3" / "graphics" / "shaders"
BGFX = ROOT / "third_party" / "bgfx"

# A shader source and the .def.sc its varyings/attributes come from. GLSL profile 330 (the desktop OpenGL bgfx
# asks for); the array name is "<stem>_glsl" like the checked-in headers.
SHADER_OF = {
    "vs_draw2d": "varying.def.sc",
    "fs_draw2d": "varying.def.sc",
    "vs_draw3d": "varying_draw3d.def.sc",
    "fs_draw3d": "varying_draw3d.def.sc",
    "fs_shadowvolume": "varying_draw3d.def.sc",
    "vs_draw3dskin": "varying_draw3d.def.sc",
    "vs_particle": "particle.def.sc",
    "fs_particle": "particle.def.sc",
    "vs_stq": "stq.def.sc",
    "fs_stq": "stq.def.sc",
    "vs_screenfx": "screenfx_varying.def.sc",
    "fs_screenfx": "screenfx_varying.def.sc",
    "vs_scenefog": "screenfx_varying.def.sc",
    "fs_scenefog": "screenfx_varying.def.sc",
    "vs_shadowresolve": "shadowresolve_varying.def.sc",
    "fs_shadowresolve": "shadowresolve_varying.def.sc",
    "vs_scenecomposite": "scenecomposite_varying.def.sc",
    "fs_scenecomposite": "scenecomposite_varying.def.sc",
}
TYPE_OF = {"vs_": "vertex", "fs_": "fragment"}
PROFILE = "330"


def FindShaderc() -> str:
    for config in ("linux64_gcc", "linux32_gcc", "osx64_clang", "windows_vs2022"):
        candidate = BGFX / ".build" / config / "bin" / "shadercRelease"
        if candidate.is_file():
            return str(candidate)
    for name in ("shaderc", "shadercRelease"):
        found = shutil.which(name)
        if found:
            return found
    raise SystemExit(
        "shaderc not found: build it with `make tools` in third_party/bgfx (its .build/<config>/bin/shadercRelease)"
    )


def Build(shaderc: str, stem: str, check: bool) -> bool:
    source = SHADERS / f"{stem}.sc"
    varying = SHADERS / SHADER_OF[stem]
    target = SHADERS / f"{stem}.bin.h"
    shaderType = next(t for prefix, t in TYPE_OF.items() if stem.startswith(prefix))
    array = f"{stem}_glsl"

    output = target if not check else Path("/tmp") / f"{stem}.bin.h"
    command = [
        shaderc, "-f", str(source), "-o", str(output), "--type", shaderType, "--platform", "linux",
        "-p", PROFILE, "--bin2c", array, "--varyingdef", str(varying),
        "-i", str(BGFX / "src"), "-i", str(SHADERS),
    ]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0 or not output.is_file():
        print(f"{stem}: failed\n{result.stdout}\n{result.stderr}")
        return False

    if check:
        same = output.read_bytes() == target.read_bytes()
        print(f"{stem}: {'ok' if same else 'STALE (regenerate)'}")
        output.unlink(missing_ok=True)
        return same

    print(f"{stem}: wrote {target.name}")
    return True


def main() -> int:
    check = "--check" in sys.argv[1:]
    shaderc = FindShaderc()
    ok = True
    for stem in SHADER_OF:
        ok = Build(shaderc, stem, check) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
