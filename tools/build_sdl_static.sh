#!/usr/bin/env bash
# Builds the 32-bit static SDL3 the Linux host build links in (configure.py's SDL_STATIC default). The source is the
# third_party/SDL submodule, so the version is the repository's rather than whatever the distribution has, and nothing has
# to be installed to run the game afterwards: SDL's own backends (X11, Wayland, the audio servers) are dlopen'd from the
# desktop at run time either way, and those are libraries a desktop already has.
#
#   tools/build_sdl_static.sh            # once per checkout, after git submodule update --init
#
# configure.py finds the result in build/sdl3-static/install; SDL_PREFIX points it at another install, SDL_STATIC=0 links
# the system's shared SDL3 instead.
set -euo pipefail

cd "$(dirname "$0")/.."

if [ ! -f third_party/SDL/CMakeLists.txt ]; then
    echo "build_sdl_static: third_party/SDL is empty: git submodule update --init" >&2
    exit 1
fi

# SDL_SHARED=OFF keeps the build to libSDL3.a alone; the tests and examples are not needed. -m32 because the port is 32-bit
# (the retail data's pointer layouts and fixed addresses), so the library has to be the 32-bit one too
cmake -S third_party/SDL -B build/sdl3-static \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON \
    -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_TESTS=OFF \
    -DCMAKE_C_FLAGS=-m32 \
    -DCMAKE_INSTALL_PREFIX=build/sdl3-static/install
cmake --build build/sdl3-static
cmake --install build/sdl3-static

echo "build_sdl_static: $(ls -l build/sdl3-static/install/lib/libSDL3.a | awk '{print $5}') bytes in build/sdl3-static/install"
