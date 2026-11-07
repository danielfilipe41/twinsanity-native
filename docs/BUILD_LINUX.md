# Building on Linux

The platform this port is developed on: OpenGL and Vulkan both run the game. The shared requirements (32-bit, Python,
submodules, disc data) are in [BUILDING.md](BUILDING.md).

## SDL3

The game links SDL3 in statically, built from the submodule: `configure.py` builds it on the first configure (through
`tools/build_sdl_static.sh`, which can also be run by hand) into `build/sdl3-static/install`. That is why the binary needs no
SDL3 package to run -- the 32-bit SDL3 many 64-bit distributions do not carry included. The version is the repository's
rather than the distribution's, and the C++ runtime and libgcc are linked in too (`-static-libstdc++ -static-libgcc`). What SDL links inside (X11, Wayland, the audio servers) it loads from the desktop at run time, so those are
the only shared libraries left beside libc. `SDL_STATIC=0` links the system's shared SDL3 instead (a 32-bit package,
`SDL_PREFIX` for a custom install).

## bgfx, bx and bimg

32-bit static libraries, built once:

```sh
cd third_party/bgfx
make config=release32 -C .build/projects/gmake-linux-gcc
cd ../..
```

That is Linux's renderer set: OpenGL and Vulkan (bgfx's desktop default also carries its WebGPU loader). They land in
`third_party/bgfx/.build/linux32_gcc/bin`.

bgfx compiles its GL renderer for one profile at a time — defining both `BGFX_CONFIG_RENDERER_OPENGL` and
`BGFX_CONFIG_RENDERER_OPENGLES` is a compile error — so an OpenGL ES platform (Android) takes:

```sh
make config=release32 -C .build/projects/gmake-linux-gcc -B bgfx \
  CPPFLAGS="-DBGFX_CONFIG_RENDERER_OPENGLES=1 -DBGFX_CONFIG_RENDERER_VULKAN=1"
python3 tools/build_shaders.py --platform android
```

`-B` is needed because the make project does not track the defines, and the game has to be relinked after. An ES build
needs the EGL and GLES development headers and libraries.

## The game

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
PLATFORM=sdl3 .venv/bin/python configure.py
ninja game
```

`build/host/sdl3/game-with-data` is the result, and needs no SDL3 at run time (only the desktop's X11 and GL or Vulkan).
The game reads its files relative to the working directory, so run it
from `disc/` -- or point it at your own disc image, which needs no extraction (see
[Saves and settings](../README.md#saves-and-settings)):

```sh
cd disc
../build/host/sdl3/game-with-data
TWINSANITY_RENDERER=vulkan ../build/host/sdl3/game-with-data
../build/host/sdl3/game-with-data --disc "../Crash Twinsanity (Europe).iso"
../build/host/sdl3/game-with-data --disc "../Crash Twinsanity (Europe).iso" --verify-disc   # which dump it is
```

Renderers and shader blobs are [here](BUILDING.md#renderers).

## Compiling without a platform layer

`PLATFORM=null .venv/bin/python configure.py` builds the same game C++ against the null platform with the system
compiler, and `ninja compile` checks that every TU compiles. How the decompilation and its tools work is in
[DEVELOPMENT.md](DEVELOPMENT.md).
