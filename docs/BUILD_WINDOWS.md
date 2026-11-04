# Building for Windows

`PLATFORM=sdl3 TARGET=windows` compiles the same sdl3 platform layer and game code with a 32-bit MinGW compiler into
`build/win32/game-with-data.exe` (a console program, since the port logs to stdout). It builds from Linux, and under Wine
it starts, loads its data, draws through bgfx's OpenGL renderer and runs at the disc's 50 Hz logic rate; it has not been
run on Windows itself yet, and Wine's `WineDbg`/desktop still show its share of fixmes. The shared requirements are in
[BUILDING.md](BUILDING.md).

## What it needs

- mingw-w64 for i686: MSYS2's `mingw-w64-i686-gcc` on Windows, `g++-mingw-w64-i686` on Debian/Ubuntu, or on Arch/CachyOS
  `sudo pacman -S mingw-w64-gcc` (it carries both i686 and x86_64; `$CXX` overrides the compiler).
- SDL3 built for MinGW. On Arch/CachyOS the AUR package lands it where the build looks for it: `yay -S mingw-w64-sdl3`
  (it installs to `/usr/i686-w64-mingw32`, found automatically). Otherwise build SDL3 from the tree into
  `third_party/SDL/install`, or point `$SDL_PREFIX` at an install. The SDL3 tree carries the cross toolchain file:

  ```sh
  cmake -S third_party/SDL -B build/sdl-mingw32 -GNinja \
    -DCMAKE_TOOLCHAIN_FILE=third_party/SDL/build-scripts/cmake-toolchain-mingw64-i686.cmake \
    -DCMAKE_INSTALL_PREFIX=third_party/SDL/install
  cmake --build build/sdl-mingw32 --target install
  ```

- bgfx, bx and bimg for MinGW: generate its mingw project with GENie
  (`MINGW=/usr ../bx/tools/bin/linux/genie --gcc=mingw-gcc gmake`) and
  `make -R -C .build/projects/gmake-mingw-gcc config=release32 bgfx bimg bimg_decode bx`. The libraries land in
  `.build/win32_mingw-gcc/bin`, which is where the build looks, or point `$BGFX_PREFIX` at them. bgfx dlopens
  `vulkan-1.dll` itself, so the Vulkan renderer needs no import library to link.
- Shaders: `python3 tools/build_shaders.py --platform windows` generates the same GLSL 330 and SPIR-V blobs Linux checks
  in. Direct3D's DXBC blobs are the one thing a Linux shaderc cannot make (see [Direct3D](#direct3d)).

```sh
PLATFORM=sdl3 TARGET=windows python3 configure.py
ninja game                                      # build/win32/game-with-data.exe
cd disc && wine ../build/win32/game-with-data.exe
```

## The Windows-specific parts of the port

- `tools/host_data.py --coff` writes the retail data as COFF assembly. `.type` and `.size` are ELF directives COFF
  rejects, so each defined data label gets an underscore alias (`.globl _X` + `.set _X, X`) instead, and the references
  to `_end` are zeroed (PE's linker keeps that symbol in a section it discards).
- `tools/pe_link.py` wraps the link. i686 PE spells a C symbol with a leading underscore, while `RETAIL()` gives the
  retail symbols their literal spelling through an asm label, so a definition and its reference can end up on opposite
  sides of the underscore. The wrapper nm-scans the objects and passes `-Wl,--defsym=a=b` wherever only one spelling is
  defined. It skips an alias when the object defining its target also references it: that is a forwarder, and aliasing
  the CRT's name to it would close the loop onto itself.
- `src/platform/null/pe_libc.cpp` defines the unadorned `memcpy`, `memmove`, `memset`, `sprintf`, `strcat`, `strcmp`,
  `strcpy`, `strlen` and `strncpy` that `include/retail/libc.h` references, each calling the CRT's underscored function.
  Without the skip above, those calls became jumps to the forwarders themselves, which hung the process in the CRT's
  argument copying before `main` ran at all.
- `RETAIL()` also carries `__attribute__((cdecl))` on 32-bit MinGW. MinGW's member ABI is thiscall (`this` in ECX, the
  callee popping its arguments) where every retail function takes all of its arguments on the stack, and the retail
  vtables hold plain functions and member functions alike, dispatched through `CallVirtual`'s one cast. The attribute
  gives the retail functions the convention they are called with; pointers to member functions are the one call MinGW
  can't express that way (it always passes their `this` in ECX), so the code doesn't make any.
- Everything that can hold a `Matrix4x4`/`Vector4` has to be 16-byte aligned. Those types are `alignas(16)` (the VU reads
  them as one 128-bit load) and the compiler copies them with SSE's aligned moves, so a copy through a misaligned pointer
  faults. The i386 CRT's `malloc`/`calloc` hand back 8-byte aligned memory where the POSIX hosts' give 16, so the three
  allocators that give out such memory ask for the alignment on Windows: `src/platform/null/pe_alloc.cpp` replaces the
  global `operator new`/`delete` with `_aligned_malloc(16)` (the link takes its definitions before libstdc++'s, so each
  allocation and its matching free use the same allocator), `Platform::Memory::AllocatePool` aligns the game's heap pool,
  which the retail heap then carves in 16-byte units, and `Platform::Graphics::HostMemory` (in `host_graphics.h`) is the
  aligned `malloc`/`calloc`/`free` the host's resources and their arrays go through. Leave any of them out and roughly half
  of the matrix copies fault, in whichever renderer function happens to touch one first.
- The link is static (`-static -static-libgcc -static-libstdc++ -Wl,-Bstatic` at the end), so the .exe needs no DLLs
  beside the system's: `objdump -p game-with-data.exe | grep "DLL Name"` shows only KERNEL32, USER32 and friends.
- Saves and `settings.txt` live under `%LOCALAPPDATA%\twinsanity` (see [Saves and settings](../README.md#saves-and-settings)).
- The crash log's Windows side is `SetUnhandledExceptionFilter` plus `RtlCaptureStackBackTrace`, writing the exception
  and the stack's module offsets to `crash.log` and stderr.

## Direct3D

`TWINSANITY_RENDERER=direct3d11` and `=direct3d12` run bgfx's two Direct3D renderers. Both take DXBC, and bgfx has no
DXIL/DXC path, so one blob set serves both.

DXBC is the one part of the port a Linux machine cannot make. It comes from `d3dcompiler_47.dll` (fxc's compiler, which
ships with Windows) or from DXC, and Wine's built-in `d3dcompiler_47` hands SM4/5 HLSL to vkd3d, whose compiler fails on
these shaders (`D3DCompile failed 0x80004005`, then a crash inside wined3d). So the blobs are generated on a Windows machine
(or over a real `d3dcompiler_47.dll` in the Wine prefix) and checked in:

```sh
python3 tools/build_shaders.py --platform windows --d3d
```

The rest of the plumbing is in place and waits on those files: `shaders/<name>_dxbc.bin.h`, array `<name>_dxbc`, which
`configure.py` notices (it defines `TWINSANITY_HAVE_DXBC` then), `host_shaders.cpp` hands to
`direct3d11`/`direct3d12`, and `bgfx_context.cpp` accepts those two names. A build without them carries OpenGL and Vulkan
and refuses the Direct3D names with a line saying why. Neither renderer has been run on real Windows yet.

### Building the shaderc that makes them

A shaderc built for Windows has the HLSL side (`d3dcompiler_47.dll` is loaded at run time). On a Linux host the generated
MinGW project needs three things fixed before it builds one:

```sh
cd third_party/bgfx
MINGW=/usr ../bx/tools/bin/linux/genie --gcc=mingw-gcc --with-tools gmake
# glslang's project takes its Unix OSDependent when GENie itself runs on Linux, and MinGW has no sys/resource.h
sed -i 's|OSDependent/Unix|OSDependent/Windows|g' .build/projects/gmake-mingw-gcc/glslang.make

# The tool itself is a host program, so it needs the host's aligned operator new (else its own movaps faults) and the
# shim: MinGW has no dxcapi headers for the DXIL half, and COM_NO_WINDOWS_H leaves the WSL stubs' IUnknown undefined
cd .. && i686-w64-mingw32-g++ -m32 -O2 -Iinclude -Iinclude/host -c src/platform/null/pe_alloc.cpp \
  -o build/win32/shaderc_alloc.o && cd third_party/bgfx

# The i686 driver links it: the x86_64 one has no 32-bit libraries on Arch, and shaderc's objects are -m32 either way
make -R -C .build/projects/gmake-mingw-gcc config=release32 shaderc CXX=i686-w64-mingw32-g++ \
  CXXFLAGS="-DSHADERC_CONFIG_HAS_DXC=0 -include $(pwd)/../../tools/shaderc_mingw_shim.h" \
  LDFLAGS="$(pwd)/../../build/win32/shaderc_alloc.o"
```

`tools/build_shaders.py` then finds `.build/win32_mingw-gcc/bin/shadercRelease.exe` and runs it through `wine` when the host
is Linux, which is where Wine's compiler gets in the way.

## MSVC

Not possible today, and the build system is the small part of it: the code is GCC-only. An MSVC build would stop on three
things the Linux and MinGW builds use on purpose:

- Inline assembly in 14 files: `include/abi.h`'s retail calling-convention thunks (GNU `asm` statements made of constant
  expressions), `include/include_asm.h`, and game files such as `rigidbody.cpp`, `collision.cpp` and `curves.cpp`.
- `__builtin_*` intrinsics: `__builtin_sqrtf` (259 uses) and `__builtin_fabsf` (236) carry the retail float arithmetic,
  with `__builtin_bit_cast`, `__builtin_fminf`/`fmaxf` and a few others beside them.
- `__attribute__`: `optimize` (16 uses), `weak`, `aligned`.

Making MSVC work means replacing those behind `_MSC_VER` -- a portability pass, not a compiler switch. `clang-cl` is the
smaller step if native Windows tooling is ever needed: it accepts the GNU inline asm and most of the builtins, though the
`__attribute__((optimize(...)))` sites still need to become pragmas or `__declspec` alternatives.
