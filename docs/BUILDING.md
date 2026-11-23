# Building

Each platform has its own document; this one holds what they all need.

- [BUILD_LINUX.md](BUILD_LINUX.md) — the platform the port is developed on, OpenGL and Vulkan.
- [BUILD_WINDOWS.md](BUILD_WINDOWS.md) — 32-bit MinGW, cross-built from Linux and run under Wine.
- [BUILD_WEB.md](BUILD_WEB.md) — wasm32; compiles, does not link yet.
- [BUILD_PS2.md](BUILD_PS2.md) — the original decompilation build.

## Common requirements

- A 32-bit x86 build, since the retail data uses 32-bit pointers and fixed addresses.
- CMake, for SDL3. `configure.py` builds a 32-bit static SDL3 from `third_party/SDL` (through `tools/build_sdl_static.sh`)
  and links it in, so the binary needs no SDL3 package at run time. `SDL_STATIC=0` links the system's shared SDL3 instead,
  and `SDL_PREFIX` names another install either way.
- Python 3.10 or later for the build scripts, and the submodules.

```sh
git clone https://github.com/Smartkin/twinsanity-reversed.git
cd twinsanity-reversed
git submodule update --init
```

The game's files are [Getting the game](../README.md#getting-the-game): the extracted `disc/` folder for the PC builds,
`SLES_525.68` for the PS2 build.

## A build without the retail data (`DATA=runtime`)

The `game-with-data` builds carry the retail program's tables, strings and vtables in the executable: `tools/host_data.py`
turns the split's data asm into host objects and the link resolves every pointer in them. `DATA=runtime` is a second way to
build the same game, with none of that data in it: at the first start it reads `SLES_525.68` out of the disc it is playing
(the folder, or the image named with `--disc`), copies its data sections into an image whose symbols are the game's, and
points every pointer the data holds at this port's own function or at the image itself (`src/platform/null/retaildata.cpp`).

```sh
DATA=runtime PLATFORM=sdl3 python3 configure.py            # Linux host
DATA=runtime PLATFORM=sdl3 TARGET=windows python3 configure.py   # Windows (cross-build too)
ninja game
```

The disc's executable is checked against the European release's SHA-1 before any of it is used: another release's program
has its data at other addresses and is refused rather than half read. The addresses it needs are `tools/retail_symbols.h`,
generated from `symbol_addrs.txt` by `tools/retail_map.py` and checked in; `python3 tools/retail_map.py --check` compares
that map with the pointer words the split itself made. Nothing of the game's data is in such an executable, so it can be
handed to anyone who owns the disc, and it plays from their copy (the disc's files are needed at every start either way).

The disc has to be there at each start, so the mode suits letting someone play from their own copy, not a build that runs
without it. Every other build is unchanged.

`--verify-data` does the load and says what it read and relocated, without starting the game:

```sh
build/host/sdl3/game-with-data --disc "Crash Twinsanity (Europe).iso" --verify-data
```

`--dump-data` writes the loaded image out word by word, and `tools/retail_parity.py` compares it with the split's data
objects (which a normal build leaves in `build/host/data`): every word the same, and every pointer the same symbol plus the
same offset.

```sh
build/host/sdl3/game-with-data --disc disc --dump-data build/host/retail-image.txt
python3 tools/retail_parity.py build/host/sdl3/game-with-data build/host/retail-image.txt
```

`tools/check_runtime_data.py` runs the whole check: the split's objects against the disc's bytes, then a runtime build's
load against those objects. It builds both ways, leaves the tree on the ordinary build, and exits nonzero on any
difference.

```sh
python3 tools/check_runtime_data.py            # disc/
python3 tools/check_runtime_data.py "my.iso"   # a disc image
```

The same check runs in CI: `.github/workflows/runtime-data.yml` on the changes that touch the loader or the split, on a
self-hosted runner that has the disc (the retail files are not in the repository, so a hosted runner has neither half of
the comparison). It reads the disc's path from the repository variable `TWINSANITY_DISC`, or from the `disc` input when the
workflow is started by hand, and any difference fails the job.

## Renderers

A build carries only the renderers its platform uses. `TWINSANITY_RENDERER` names one of them; a named renderer the build
or the driver cannot start fails the launch rather than quietly rendering another way. With no name (or `auto`) the platform
walks its order below and starts the first renderer the machine takes, so a machine without a Vulkan driver still runs on
Direct3D, and one with neither on OpenGL. Vulkan leads because it is the fastest of them by a wide margin: the same scene
measured about four times OpenGL's frame rate on the Windows test machine.

| Platform | Renderers, best first |
|---|---|
| Linux | `vulkan` (default), `opengl` |
| Windows | `vulkan` (default), `direct3d12`, `direct3d11`, `opengl` (see [the Direct3D blobs](BUILD_WINDOWS.md#direct3d)) |
| Android | `opengles` / `gles` (default; its GL), `vulkan` |
| macOS | `metal` (no Metal shaders yet) |
| Web | `webgl` / `opengles` (WebGL, GLSL ES 300), `webgpu` (WGSL) |

Android and macOS have no platform layer here yet: their rows are the renderers and shader backends the tables already hold.

## Antialiasing

The settings file's `antialias` line picks how the finished frame is cleaned up. The game's own options screen has a row for
it, on OPTIONS then GRAPHIC OPTIONS, next to the rest of the picture's settings (the resolution, the window's mode, vsync, the
picture's shape and the resolution scale); the line is what that row writes, so it can be set by hand too (see
[Saves and settings](../README.md#saves-and-settings)):

| `antialias` | What draws |
|---|---|
| `off` (default) | nothing: the pictures the render checks compare against were drawn this way |
| `fxaa` | one post pass over the composited frame, no look-up tables: the cheapest, and the softest |
| `smaa` | three post passes with SMAA's area and search tables: sharper than FXAA, and it keeps subpixel detail |

Both are post passes on the composited picture, so they cover everything the frame shows: the 3D geometry, the sprites and
the menus' text. FXAA blends the whole span it finds an edge in, which is why it is the cheap one and the blurry one; SMAA
finds each span's ends, so it anti-aliases the edge and leaves the rest of the picture alone.

Hardware multisampling is not offered. It was built and measured: bgfx's multisample render targets
(`BGFX_TEXTURE_RT_MSAA_X*`) on the scene target, 2x and 4x, resolving on every framebuffer switch the frame makes. The scene
target is left and re-entered several times a frame (the sky, the shadow volumes, the screen effects and the fog each take
the frame elsewhere and back), bgfx resolves colour and depth on every one of those switches, and on this port's GL driver
that dropped a frame from over a hundred a second to under thirty -- with D24S8 and D32F depth alike, so the resolve itself
is the cost, not the sample count. The two post passes cost one fullscreen pass (FXAA) or three (SMAA) at the window's
resolution and nothing else.

SMAA's two look-up tables are checked in as generated headers (`shaders/smaa_area_tex.bin.h`,
`shaders/smaa_search_tex.bin.h`), like the shader blobs: `python3 tools/smaa/build_smaa_textures.py` regenerates them
(it needs Pillow) and `--check` verifies them.

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
library: the browser provides WebGPU, so a `WEBGPU=1` web build runs that renderer against bgfx built for wasm with it
enabled (`third_party/bgfx/src/config.h`'s Emscripten line; see [BUILD_WEB.md](BUILD_WEB.md)). The default web build is
WebGL, because WebGPU's link needs Asyncify, which costs size and speed. Metal and Direct3D need their own platforms.
