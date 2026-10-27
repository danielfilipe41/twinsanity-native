# Crash Twinsanity: decompilation and native port

This project is a multi-system port of Crash Twinsanity (PAL, SLES-525.68), built on top of
[Smartkin's decompilation of the game](https://github.com/Smartkin/twinsanity-reversed). The
PlayStation 2 backend comes from that decomp: the game code, the PS2 platform layer and the
matching build. This repository adds a native Linux/PC backend and is where the port work happens
now. Other platforms are planned, but there is only the PS2 and SDL3 code today.

The PC build uses SDL3 for the window, input and audio, and bgfx for rendering. The game code is
the same C++ as the PS2 build; it reaches the hardware through the platform layer, so the renderer,
audio, files and pads can be replaced one system at a time. The 32-bit build keeps the retail
pointer layouts and absolute addresses.

This is an alpha. It boots, plays and saves, but it is not finished, and the known issues below are
real. Nothing from the game is included in the repository. You need your own copy of the PAL disc.

## Status

The PC port is the current focus. Working: boot, menus, levels, characters, cutscenes, movies,
audio, saves and memory cards, keyboard and gamepad input. Rough edges: camera behaviour, audio
mixing, some draw ordering and pop-in.

Known issues:

- The camera can sit inside characters, jump between positions, get stuck, pass through walls.
- Cortex bee chase is bugged, watch this video on how to go through it even with it bugged: https://youtu.be/O_tHFcklyX8
- Pop-in is noticeable; a distance setting is planned.
- Ambient sounds play during the chase sequence when they probably should not.

## Getting the game

You need a PAL copy of Crash Twinsanity (SLES-525.68). Extracting the disc gives you `CRASH6`,
`FMV` and `SLES_525.68`.

For the PC build, extract the whole ISO into `disc/` in the repository. The game reads its files
relative to the working directory, so it must be run from that folder.

```sh
mkdir -p disc
bsdtar -xf "Crash Twinsanity (Europe).iso" -C disc
```

`7z x` or mounting the image also works. `bsdtar` is libarchive's tar, which reads disc images;
GNU tar does not.

The PS2 build only needs `SLES_525.68` from the disc root. Put it in the repository and check it:

```sh
bsdtar -xf "Crash Twinsanity (Europe).iso" SLES_525.68
sha1sum SLES_525.68    # d41b8d53f733f930cf03d8ea944bec24429fe57c
```

For the PS2 build to run in PCSX2, point `disc_image` (or `TWINSANITY_ISO`) at the ISO. The PC
build does not read the ISO directly; it reads the extracted `disc/` folder.

## Building the PC port (Linux)

Requirements:

- A 32-bit x86 userspace, since the retail data uses 32-bit pointers and fixed addresses.
- A 32-bit SDL3. Set `SDL_PREFIX` if it is not in the system paths.
- Python 3.10 or later for the build scripts.
- Git, and the submodules.

The PC port is currently built and tested on Linux only.

Clone and initialize the submodules:

```sh
git clone https://github.com/Smartkin/twinsanity-reversed.git
cd twinsanity-reversed
git submodule update --init
```

Build bgfx, bx and bimg once. They are 32-bit static libraries:

```sh
cd third_party/bgfx
make config=release32 -C .build/projects/gmake-linux-gcc
cd ../..
```

The libraries land in `third_party/bgfx/.build/linux32_gcc/bin`. `tools/build_shaders.py`
regenerates the checked-in shader blobs after a `.sc` edit; `--check` verifies them.

Configure and build:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
PLATFORM=sdl3 .venv/bin/python configure.py
ninja game
```

The result is `build/host/sdl3/game-with-data`. Run it with `disc/` as the working directory:

```sh
cd disc
../build/host/sdl3/game-with-data
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

`J`, `K` and `L` are both face buttons and the right stick's left, down and right. The right
stick's up is `I`.

## Saves and settings

Memory cards are directories under `$XDG_DATA_HOME/twinsanity`, or `~/.local/share/twinsanity`
when it is unset (`/tmp/twinsanity` without a home). There is one per port and slot:
`card0_0`, `card0_1`, `card1_0`, `card1_1`. Each holds the game's saves as they would be on a
card.

The game's options are written to `settings.txt` next to the saves. Each line is one option:
`group <n> <left> <right>` for the four volume groups (0 effects, 1 second effects, 2 music,
3 movie sound), `stereo <n>` and `widescreen <n>`. The game's own save wins where they overlap.

A crash writes `crash.log` in the working directory and to stderr: the signal, registers, and the
faulting frame with the frames below it.

## Building the PS2 version

This is the original decompilation build. It needs the PS2SDK toolchain from
[ps2dev](https://github.com/ps2dev/ps2dev) (GCC 15.2) and the split of the retail executable.
Download `ps2dev-ubuntu-latest.tar.gz` from ps2dev's latest release and extract it into
`/usr/local` (or put its folder in `PS2DEV` or `local.json`). Then:

```sh
sudo tar -xzf ps2dev-ubuntu-latest.tar.gz -C /usr/local
.venv/bin/python tools/split.py
.venv/bin/python tools/build.py
```

The build is `build/SLES_525.68.elf`. `tools/build.py --matching` builds the asm alone and checks
that it reproduces the retail load image byte for byte.

`tools/play.py` boots the build in PCSX2 with the ISO from `local.json` or `$TWINSANITY_ISO`.

On Windows, extract `ps2dev-windows-latest.tar.gz` into `C:\` so the toolchain is at `C:\ps2dev`,
and run the toolchain DLL fix once after installing it (and again after replacing it):

```powershell
tar -xzf "$HOME\Downloads\ps2dev-windows-latest.tar.gz" -C C:\
py -3 -m venv .venv
.venv\Scripts\python.exe -m pip install -r requirements.txt
.venv\Scripts\python.exe tools\toolchain_dlls.py
.venv\Scripts\python.exe tools\split.py
.venv\Scripts\python.exe tools\build.py
```

How the decomp and its tools work is in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

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
| `docs/DEVELOPMENT.md` | How the decomp and its tools work |
| `docs/RETAIL_BUGS.md` | Retail bugs that the C++ keeps |
| `splat.yaml`, `symbol_addrs.txt` | Split settings and the executable's symbols |

`asm/`, `assets/`, `disc/` and `build/` are produced locally and are not committed.

## Configuration

`local.json` keeps machine-specific paths and is not committed. Copy `local.example.json` to
start one. Environment variables override it.

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
