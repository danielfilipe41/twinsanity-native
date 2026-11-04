#!/usr/bin/env python3
"""Compiles the host renderer's bgfx shaders (src/platform/sdl3/graphics/shaders/*.sc) into the C headers the
platform includes (shaders/<name>.bin.h and shaders/<name>_spv.bin.h).

Each shader is compiled for the backends the target platform's renderers take, which is not every backend the engine
knows: a platform carries the renderers it uses, so Linux and Windows take GLSL 330 for OpenGL (the checked-in
shaders/<name>.bin.h, array "<name>_glsl") and SPIR-V for Vulkan (shaders/<name>_spv.bin.h, array "<name>_spv"),
while Android and the web's WebGL take GLSL ES 300 (shaders/<name>_gles.bin.h, array "<name>_gles"), the web's
WebGPU takes WGSL (shaders/<name>_wgsl.bin.h, array "<name>_wgsl"), and the native platforms take the same SPIR-V.
host_shaders.cpp picks one by bgfx::getRendererType(). This is the checked-in blobs' generator: run it after editing a
.sc or a .def.sc.

    python tools/build_shaders.py                     # regenerate this platform's blobs (linux)
    python tools/build_shaders.py --platform android  # regenerate Android's
    python tools/build_shaders.py --check             # verify them (no writes)

shaderc comes from the bgfx submodule's tools build (third_party/bgfx/.build/<config>/bin/shadercRelease); GLSL is
the default profile and SPIR-V is -p spirv. `make tools` in third_party/bgfx builds it when missing. Direct3D's DXBC is
the one backend a Linux shaderc has none of (its HLSL side loads d3dcompiler_47.dll at run time), so those blobs come from
a shaderc built for Windows, run through Wine when the host is Linux (`--d3d`).
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

# A shader source and the .def.sc its varyings/attributes come from. These define the attributes and varyings the
# draw code fills in (mesh_builder's layouts, particle_billboard's shapes); every backend compiles the same source.
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
# (header name suffix, bin2c array suffix, shaderc profile) per platform, alongside the bgfx renderers that platform
# builds (README's build section): GLSL 330 is the desktop OpenGL bgfx asks for, SPIR-V is what its Vulkan renderer
# takes, and GLSL ES 300 is what Android's GL ES renderer takes. Adding a renderer to a platform means adding its
# profile here and its column in host_shaders.cpp.
BACKENDS = {
    "linux": (("", "glsl", "330", "linux"), ("_spv", "spv", "spirv", "linux")),
    "windows": (("", "glsl", "330", "linux"), ("_spv", "spv", "spirv", "linux")),  # Direct3D's DXBC joins with --d3d
    "android": (("_gles", "gles", "300_es", "linux"), ("_spv", "spv", "spirv", "linux")),
    "macos": (("_metal", "metal", "metal", "linux"),),
    # The web offers either: WebGL is bgfx's GL ES renderer (GLSL ES 300) and WebGPU takes WGSL, and both blob sets are
    # generated so a web build can pick either at run time
    "web": (("_gles", "gles", "300_es", "linux"), ("_wgsl", "wgsl", "wgsl", "linux")),
}
# The platforms with blobs generated and checked in. Windows shares the desktop GLSL 330 and SPIR-V blobs with Linux
# (same files); only its Direct3D blobs need a Windows shaderc (--d3d)
BUILT_PLATFORMS = ("linux", "windows", "android", "web")
# Direct3D's blobs are the one backend a Linux shaderc cannot make: its HLSL side loads the D3D compiler at run time
# (d3dcompiler_47.dll), so these come from a shaderc built for Windows, through Wine here. Direct3D 11 and 12 both take
# DXBC, which is all bgfx's shaderc makes (it has no DXIL/DXC path), so one blob set serves both renderers.
D3D_BACKENDS = (("_dxbc", "dxbc", "s_5_0", "windows"),)


def FindShaderc() -> list:
    for config in ("linux64_gcc", "linux32_gcc", "osx64_clang", "windows_vs2022"):
        candidate = BGFX / ".build" / config / "bin" / "shadercRelease"
        if candidate.is_file():
            return [str(candidate)]
    for name in ("shaderc", "shadercRelease"):
        found = shutil.which(name)
        if found:
            return [found]
    raise SystemExit(
        "shaderc not found: build it with `make tools` in third_party/bgfx (its .build/<config>/bin/shadercRelease)"
    )


def FindWindowsShaderc():
    """The Windows shaderc, which is the one with an HLSL backend: it loads d3dcompiler_47.dll at run time, so on a Linux
    host it runs through Wine, which ships that DLL."""
    for config in ("win32_mingw-gcc", "windows_vs2022", "windows_vs2019"):
        for name in ("shadercRelease.exe", "shadercRelease"):
            candidate = BGFX / ".build" / config / "bin" / name
            if not candidate.is_file():
                continue
            if os.name == "nt":
                return [str(candidate)]
            wine = shutil.which("wine")
            if wine is None:
                raise SystemExit(f"{candidate} is a Windows program and wine is not on PATH: install wine, or run this "
                                 "on Windows")
            return [wine, str(candidate)]
    return None


def Build(shaderc: list, stem: str, suffix: str, arraySuffix: str, profile: str, shadercPlatform: str,
          check: bool) -> bool:
    source = SHADERS / f"{stem}.sc"
    varying = SHADERS / SHADER_OF[stem]
    target = SHADERS / f"{stem}{suffix}.bin.h"
    shaderType = next(t for prefix, t in TYPE_OF.items() if stem.startswith(prefix))
    array = f"{stem}_{arraySuffix}"

    output = target if not check else Path("/tmp") / f"{stem}{suffix}.bin.h"
    command = [
        *shaderc, "-f", str(source), "-o", str(output), "--type", shaderType, "--platform", shadercPlatform,
        "-p", profile, "--bin2c", array, "--varyingdef", str(varying),
        "-i", str(BGFX / "src"), "-i", str(SHADERS),
    ]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0 or not output.is_file():
        print(f"{array}: failed\n{result.stdout}\n{result.stderr}")
        return False

    if check:
        same = output.read_bytes() == target.read_bytes()
        print(f"{array}: {'ok' if same else 'STALE (regenerate)'}")
        output.unlink(missing_ok=True)
        return same

    print(f"{array}: wrote {target.name}")
    return True


def main() -> int:
    arguments = sys.argv[1:]
    check = "--check" in arguments
    platform = "linux"
    d3d = "--d3d" in arguments
    if "--platform" in arguments:
        platform = arguments[arguments.index("--platform") + 1]
    if platform not in BACKENDS:
        raise SystemExit(f"--platform {platform} has no shader backends (one of {', '.join(BACKENDS)})")
    if platform not in BUILT_PLATFORMS:
        raise SystemExit(f"--platform {platform} has no blobs yet: the port has no layer for it "
                         f"(built today: {', '.join(BUILT_PLATFORMS)})")
    if d3d and platform != "windows":
        raise SystemExit("--d3d is Windows' Direct3D blobs: --platform windows --d3d")
    shaderc = FindShaderc()
    windowsShaderc = None
    if d3d:
        windowsShaderc = FindWindowsShaderc()
        if windowsShaderc is None:
            raise SystemExit(
                "--d3d needs a shaderc built for Windows, which carries the HLSL backend: build one with "
                "`MINGW=/usr ../bx/tools/bin/linux/genie --gcc=mingw-gcc --with-tools gmake` and "
                "`make -R -C .build/projects/gmake-mingw-gcc config=release32 shaderc` in third_party/bgfx"
            )
    ok = True
    backends = BACKENDS[platform] + (D3D_BACKENDS if d3d else ())
    for stem in SHADER_OF:
        for suffix, arraySuffix, profile, shadercPlatform in backends:
            runner = windowsShaderc if shadercPlatform == "windows" else shaderc
            ok = Build(runner, stem, suffix, arraySuffix, profile, shadercPlatform, check) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
