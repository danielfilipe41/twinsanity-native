# Crash Twinsanity: decompilation and native port

This project is a multi-system port of Crash Twinsanity (PAL, SLES-525.68), built on top of
[Smartkin's decompilation of the game](https://github.com/Smartkin/twinsanity-reversed). The PlayStation 2 backend comes
from that decomp: the game code, the PS2 platform layer and the matching build. This repository adds a native Linux/PC
backend and is where the port work happens now.

The PC build uses SDL3 for the window, input and audio, and bgfx for rendering. The game code is the same C++ as the PS2
build (minor some tweaks and things necessary for some features); it reaches the hardware through the platform layer, so the renderer, audio, files and pads can be replaced one system at a time. The 32-bit build keeps the retail pointer layouts and absolute addresses.

Nothing from the game is included in the repository. You need your own copy of the PAL disc.

## Status

The Linux port is nearly done: the game is playable start to finish and 100% of the gems can be collected. Windows
cross-builds from Linux and runs under Wine; every other platform has its backend only.

Known issues:
- Only Linux and, under Wine, Windows run the game (see [Building](docs/BUILDING.md)).

Future features:
- Multiple save pages
- Cutscene skip
- Support for other ISO regions (NTSC)
- Mod support
- PC port settings, including pop-in distance / LOD
- WebGPU support (blocked: bgfx needs Dawn, and Dawn has no 32-bit build)
- HD textures
- Other consoles button glyphs

## Getting the game

You need a PAL copy of Crash Twinsanity (SLES-525.68). Extracting the disc gives you `CRASH6`, `FMV` and `SLES_525.68`.

For the PC build, extract the whole ISO into `disc/` in the repository. The game reads its files by their paths on the disc,
so it runs from that folder (or from wherever the folder is, see below).

```sh
mkdir -p disc
bsdtar -xf "Crash Twinsanity (Europe).iso" -C disc
```

`7z x` or mounting the image also works. `bsdtar` is libarchive's tar, which reads disc images; GNU tar does not.

The PS2 build only needs `SLES_525.68` from the disc root. Put it in the repository and check it:

```sh
bsdtar -xf "Crash Twinsanity (Europe).iso" SLES_525.68
sha1sum SLES_525.68    # d41b8d53f733f930cf03d8ea944bec24429fe57c
```

Started somewhere else, the PC build asks for the folder once, with a picker titled "Point to the extracted ISO folder":
the answer is kept beside the saves (in `gamedata.txt`), so the next run opens straight from it, and a folder that no longer
has the files asks again. `TWINSANITY_DATA` names the folder for a run without asking -- also the way a host whose picker
hands over something that isn't a path (Android's content URIs) is given its files.

For the PS2 build to run in PCSX2, point `disc_image` (or `TWINSANITY_ISO`) at the ISO. The PC build does not read the ISO
directly; it reads the extracted folder.

## Building

| Platform | Document | State |
|---|---|---|
| Linux | [docs/BUILD_LINUX.md](docs/BUILD_LINUX.md) | builds and runs; OpenGL and Vulkan |
| Windows | [docs/BUILD_WINDOWS.md](docs/BUILD_WINDOWS.md) | cross-builds from Linux, runs under Wine |
| Web (wasm32) | [docs/BUILD_WEB.md](docs/BUILD_WEB.md) | compiles TU by TU, does not link yet |
| PS2 | [docs/BUILD_PS2.md](docs/BUILD_PS2.md) | the original decompilation build |

Every build needs a 32-bit x86 toolchain (the retail data uses 32-bit pointers and fixed addresses), a 32-bit SDL3, Python
3.10 or later and the submodules; [docs/BUILDING.md](docs/BUILDING.md) holds those, the renderers each platform takes, and
how the shader blobs are made. Linux, start to finish:

```sh
git submodule update --init
cd third_party/bgfx && make config=release32 -C .build/projects/gmake-linux-gcc && cd ../..
python3 -m venv .venv && .venv/bin/python -m pip install -r requirements.txt
PLATFORM=sdl3 .venv/bin/python configure.py && ninja game
cd disc && ../build/host/sdl3/game-with-data
```

## Controls

Keyboard and SDL3 gamepads both work. A pad maps directly onto the game's controller.

| Game | Keyboard | Gamepad |
|---|---|---|
| D-pad | Arrow keys | D-pad |
| Left stick | `W` `A` `S` `D` | Left stick |
| Right stick | `I` `J` `K` `L` | Right stick |
| Cross | `Space` | South |
| Circle | `K` | East |
| Square | `J` | West |
| Triangle | `L` | North |
| L1 / R1 | `Q` / `E` | Left / right shoulder |
| L2 / R2 | `1` / `3` | Left / right trigger |
| Select / Start | `Backspace` / `Enter` | Back / Start |
| L3 / R3 | none | Left / right stick click |

`J`, `K` and `L` are both face buttons and the right stick's left, down and right. The right stick's up is `I`.

## Saves and settings

Memory cards are directories under the platform's per-user data directory: `%LOCALAPPDATA%\twinsanity` on Windows,
`$XDG_DATA_HOME/twinsanity` (`~/.local/share/twinsanity` when it is unset, `/tmp/twinsanity` without a home) elsewhere.
There is one per port and slot: `card0_0`, `card0_1`, `card1_0`, `card1_1`. Each holds the game's saves as they would be on
a card.

The folder the game's files were found in is kept in `gamedata.txt` next to the saves (asked for when it stops having them);
`settings.txt` beside it holds the game's options, one per line: `group <n> <left> <right>` for
the four volume groups (0 effects, 1 second effects, 2 music, 3 movie sound), `stereo <n>` and `widescreen <n>`. The game's
own save wins where they overlap.

`render <n>` is the picture's rate, the frames the port presents a second: 50 when the file has none, and raised to 50 if
set lower. The game's logic always steps at the disc's field rate, whatever this says; the frames between two steps draw
the state eased between them, so the picture runs one step (20 ms) behind the logic and the parts that step in one go
(particles, decals' ageing, material shader animation) still move at 50. The extra frames cost CPU in proportion, since the
scene is drawn again for each.

```sh
echo 'render 120' >> "${XDG_DATA_HOME:-$HOME/.local/share}/twinsanity/settings.txt"
```

`antialias <mode>` cleans up the finished picture: `off` when the file has none, `fxaa` (one post pass, the cheap one) or
`smaa` (three passes, sharper and it leaves the picture's detail alone). Both work on the composited frame, so they smooth
the 3D geometry, the sprites and the menus' text alike; hardware multisampling is not one of the choices, and
[the antialiasing section](docs/BUILDING.md#antialiasing) says what it cost here.

```sh
echo 'antialias smaa' >> "${XDG_DATA_HOME:-$HOME/.local/share}/twinsanity/settings.txt"
```

A crash writes `crash.log` in the working directory and to stderr: the signal, registers, and the faulting frame with the
frames below it. On Windows it is the unhandled exception's code and fault address with the stack's module offsets
(resolve one with `addr2line -f -C -e game-with-data.exe <offset>`).

## Repository layout

| Path | Contents |
|---|---|
| `src/game/` | The game's C++ |
| `src/platform/ps2/` | PS2 backend, from the original decompilation |
| `src/platform/sdl3/` | SDL3/bgfx backend for the PC port |
| `src/platform/null/` | Stubs and CPU maths used by the host builds |
| `src/abi.cpp` | Calls between the retail calling convention and the C++ |
| `include/` | Game types, platform interfaces and asm macros |
| `tools/` | Split, build, checks and PCSX2 runners |
| `docs/BUILDING.md` | What every build needs, the renderers and the shader blobs |
| `docs/BUILD_LINUX.md`, `docs/BUILD_WINDOWS.md`, `docs/BUILD_WEB.md`, `docs/BUILD_PS2.md` | One per platform |
| `docs/DEVELOPMENT.md` | How the decomp and its tools work |
| `docs/AUDIO.md` | The sound: the module, the sound processor's arithmetic and how it is checked |
| `docs/RETAIL_BUGS.md` | Retail bugs that the C++ keeps |
| `splat.yaml`, `symbol_addrs.txt` | Split settings and the executable's symbols |

`asm/`, `assets/`, `disc/` and `build/` are produced locally and are not committed.

## Configuration

`local.json` keeps machine-specific paths and is not committed. Copy `local.example.json` to start one. Environment
variables override it.

| Key | Variable | Meaning | Default |
|---|---|---|---|
| `ps2dev` | `PS2DEV` | Toolchain folder | `/usr/local/ps2dev`, `C:/ps2dev` on Windows |
| `ps2sdk` | `PS2SDK` | PS2SDK folder | `ps2sdk` inside the toolchain |
| `disc_image` | `TWINSANITY_ISO` | PAL disc image for PCSX2 | none |
| `pcsx2` | `PCSX2` | PCSX2 executable, or `flatpak` for the Flatpak | auto-detected |

Use forward slashes in Windows paths (`C:/Games/Twinsanity.iso`) or double the backslashes.

## Credits

The decompilation this port is built on is [Smartkin/twinsanity-reversed](https://github.com/Smartkin/twinsanity-reversed).
Crash Twinsanity is property of Activision. This repository contains no game code or data.
