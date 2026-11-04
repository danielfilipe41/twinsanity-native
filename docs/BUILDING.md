# Building

Each platform has its own document; this one holds what they all need.

- [BUILD_LINUX.md](BUILD_LINUX.md) — the platform the port is developed on, OpenGL and Vulkan.
- [BUILD_WINDOWS.md](BUILD_WINDOWS.md) — 32-bit MinGW, cross-built from Linux and run under Wine.
- [BUILD_WEB.md](BUILD_WEB.md) — wasm32; compiles, does not link yet.
- [BUILD_PS2.md](BUILD_PS2.md) — the original decompilation build.

## Common requirements

- A 32-bit x86 build, since the retail data uses 32-bit pointers and fixed addresses.
- A 32-bit SDL3. Set `SDL_PREFIX` if it is not in the system paths.
- Python 3.10 or later for the build scripts, and the submodules.

```sh
git clone https://github.com/Smartkin/twinsanity-reversed.git
cd twinsanity-reversed
git submodule update --init
```

The game's files are [Getting the game](../README.md#getting-the-game): the extracted `disc/` folder for the PC builds,
`SLES_525.68` for the PS2 build.

## Renderers

A build carries only the renderers its platform uses. `TWINSANITY_RENDERER` names one of them, or `auto` (the first the
machine supports, and the only one that falls back to another); a named renderer the build or the driver cannot start
fails the launch rather than quietly rendering another way.

| Platform | Renderers |
|---|---|
| Linux | `opengl` (default), `vulkan` |
| Windows | `opengl` (default), `vulkan`, `direct3d11`/`direct3d12` (see [the Direct3D blobs](BUILD_WINDOWS.md#direct3d)) |
| Android | `opengles` / `gles` (default; its GL), `vulkan` |
| macOS | `metal` (no Metal shaders yet) |
| Web | `webgl` / `opengles` (WebGL, GLSL ES 300), `webgpu` (WGSL) |

Android and macOS have no platform layer here yet: their rows are the renderers and shader backends the tables already hold.

## Shader blobs

The shaders are bgfx shaderc sources (`src/platform/sdl3/graphics/shaders/*.sc`) compiled into the C headers the platform
includes. `tools/build_shaders.py --platform <platform>` regenerates the checked-in blobs and `--check` verifies them:
desktop builds take GLSL 330 for OpenGL, Android and the web's WebGL take GLSL ES 300, the web's WebGPU takes WGSL, and
every native platform takes SPIR-V for Vulkan. A web build carries both of its sets, so `TWINSANITY_RENDERER=webgl` and
`=webgpu` both work in one build. `host_shaders.cpp` picks one by `bgfx::getRendererType()`; adding a renderer means
adding its profile to that platform's backends in `tools/build_shaders.py` and its column there.

shaderc comes from the bgfx submodule (`third_party/bgfx/.build/<config>/bin/shadercRelease`); `make tools` in
`third_party/bgfx` builds it when missing.

## WebGPU on the 32-bit desktop builds

WebGPU is **blocked** there. bgfx's WebGPU renderer is written against Dawn's `webgpu.h` and loads `libwebgpu_dawn.so` at
run time, and there is no 32-bit Dawn: Dawn builds for x86-64 and arm64 only (x86 is listed untested upstream), while this
port is 32-bit because the retail data uses 32-bit pointers. A 32-bit process cannot load a 64-bit library, so
`TWINSANITY_RENDERER=webgpu` fails with `bgfx::init failed` until a 32-bit Dawn exists. On the web there is no such
library: the browser provides WebGPU, so the web build's `webgpu` renderer is only waiting on the wasm build itself
(see [BUILD_WEB.md](BUILD_WEB.md)). Metal and Direct3D need their own platforms.
