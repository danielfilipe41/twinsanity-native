# Crash Twinsanity: decompilation and native port

This project is a multi-system port of Crash Twinsanity (PAL, SLES-525.68), built on top of
[Smartkin's decompilation of the game](https://github.com/Smartkin/twinsanity-reversed). The PlayStation 2 backend comes
from that decomp: the game code, the PS2 platform layer and the matching build. This repository adds a native Linux/PC
backend and is where the port work happens now.

The PC build uses SDL3 for the window, input and audio, and bgfx for rendering. The game code is the same C++ as the PS2
build (minor some tweaks and things necessary for some features); it reaches the hardware through the platform layer, so the renderer, audio, files and pads can be replaced one system at a time. The 32-bit build keeps the retail pointer layouts and absolute addresses.

Nothing from the game is included in the repository. You need your own copy of the disc: any of its four releases plays (see
[Other regions](#other-regions)), and the European one is the only one that can be built with.

## Status

The Linux port is nearly done: the game is playable start to finish and 100% of the gems can be collected. Windows
cross-builds from Linux and runs under Wine; every other platform has its backend only.

Known issues:
- Only Linux and, under Wine, Windows run the game (see [Building](docs/BUILDING.md)).
- Sometimes camera gets stuck on places still not sure if this is retail behaviour or not
- Build only supported with PAL SLES file
- Pop in still kinda noticeable idk how to fix
- Sometimes in game audio takes a bit to start

Future features:
- Multiple save pages
- Cutscene skip
- Mod support
- WebGPU support (blocked: bgfx needs Dawn, and Dawn has no 32-bit build)
- HD textures
- Other consoles button glyphs
- An in-game page for the sticks' dead zone and the triggers' threshold

QOL Changes:
- Pc port settings
- Rebindable controls, on the keyboard and the pad, from `keybinds.txt` or the options' controller page
- Exit game option (also on pause screen the old quit game that sent u back to the title screen now closes the game and is called close game)
- Added option to start a new game from inside the save game option on the pause menu
- Removed the delay on the Save file check screens
- Fast loading screens (on an nvme it only takes 62ms)

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

Two things give the PC build its files, and either will do: the extracted folder (the one with `SYSTEM.CNF` in it), or a disc
image of the same disc -- a plain `.iso`, or a raw 2352-byte `.bin`. Nothing has to be extracted for an image: the port reads
the volume's own file system (ISO9660, the PS2 disc's layout), read-only, straight out of the file.

Started somewhere else, it asks once, with a picker: choose the folder with the files in it, or the folder holding the image.
A folder with one image in it is taken as it is; a folder with several asks which one with a second, filtered (`.iso`/`.bin`)
dialog. The answer is kept beside the saves (in `gamedata.txt`), so the next run opens straight from it, and a path that no
longer has the files asks again.

`--disc <path>` answers with a folder or an image, and is kept like the picker's answer, so the next run needs no option:

```sh
cd disc && ../build/host/sdl3/game-with-data
../build/host/sdl3/game-with-data --disc "Crash Twinsanity (Europe).iso"
../build/host/sdl3/game-with-data --disc /path/to/extracted/
```

`TWINSANITY_DATA` does the same from the environment (a folder, an image, or a folder of images) -- also the way a host whose
picker hands over something that isn't a path (Android's content URIs) is given its files.

`--verify-disc` reads the whole image once and says which release it is, by the size, CRC-32 and SHA-1 redump.org publishes
for each of the disc's four dumps; a modified copy is reported as one (it plays as the original does):

```sh
../build/host/sdl3/game-with-data --disc "Crash Twinsanity (Europe).iso" --verify-disc
# verify-disc: 2558263296 bytes, CRC-32 9055a7bc, SHA-1 7cd787a8cb35d1bcde63ad742746d2222813145e
# verify-disc: the European (PAL) release (SLES-52568 v1.01), an original retail dump
```

For the PS2 build to run in PCSX2, point `disc_image` (or `TWINSANITY_ISO`) at the ISO.

### Other regions

The port's code is the European release's, and another region's **disc** plays with it: the game asks for files by the
European names, and the platform answers with the disc's own. Each other disc's whole archive was read against the European
one's, and exactly 23 of each one's names differ: its one language where the game says `English` (`American` on the North
American discs, `Japanese` on the Japanese one — its texts, its title pictures, its legal screen, its credits and its voice
bank), and its font where the game asks for `Startup\Fonts\Crash_Euro` (`Startup\Fonts\Crash`, `Startup\Fonts\Crash_Jpn`).
The archive index the game reads is served with its own names over the disc's entries, and a voice bank is found under the
disc's name; the start-up says so:

```
files: the disc's build files 23 of its archive's names differently; the game is given its own
```

A settings language the disc has no voices for would leave the game waiting on files that are not there, so it falls back to
English and says which it did (`settings: this disc has no French voices; playing in english`).

Four discs play, and `--verify-disc` names each of them:

| Disc | What it is |
|---|---|
| `SLES-52568` v1.01 | the European release (and any patched copy of it) |
| `SLUS-20909` v1.00 | the North American release |
| `SLUS-20909` v2.00 | the North American release, second pressing |
| `SLPM-65801` v1.00 | the Japanese release, its texts and voices Japanese |

A disc whose names are none of these is not silent about it: the port lists the game's files it cannot find
(`files: the disc has no <name>`), and `tools/disc_names.py` prints the row it would take to support it.

Each disc also plays at **its own field rate**: the release's build says which, 50 fields a second on the European disc and 60
on the North American and Japanese ones, and the port takes it from the disc's serial — the game's own PAL flag is a word of
the retail data, which the port carries the European build's value of. The rate decides the logic's step, the clock's frame,
the particle emitters' aging and the picture's framing, so a North American disc runs at the speed its console ran at.

**Only the European disc can be built with.** The North American and Japanese discs are *data* to this port: the game's own
code is the European program's, so their program's own behaviour does not come with them. The PS2 build is the one that needs
the program itself — `SLES_525.68` from the European disc, as above; the computer builds need no executable at all, they read
whichever disc they are pointed at.
What a build on another region's program would take is in [DEVELOPMENT.md](docs/DEVELOPMENT.md#another-regions-disc).

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

Keyboard and SDL3 gamepads both work. A pad maps onto the game's controller, and both it and the keyboard are reboundable in
the options' controller page or in `keybinds.txt` (below).

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

`keybinds.txt`, beside the saves, holds every binding: a key and a pad input for each of the game's inputs, so a pad can be
rearranged the way a keyboard can (SDL still maps the controller itself, and the system's remapping applies). The file is
written with the defaults the first time the game reads a pad. One line per binding, `<input> key|pad <binding>`:

```
cross key Space
cross pad south
left stick up key W
left stick up pad none
```

The inputs are `cross`, `circle`, `square`, `triangle`, `up`, `down`, `left`, `right`, `l1`, `r1`, `l2`, `r2`, `select`,
`start`, `l3`, `r3`, `left stick up`, `left stick down`, `left stick left`, `left stick right`, `right stick up`,
`right stick down`, `right stick right` and `right stick left`. A `key` is SDL's name for one (`space`, `return`,
`left shift`, `f1`, `a`, `1`, `keypad 5`); a `pad` input is `south`, `east`, `west`, `north`, `left shoulder`,
`right shoulder`, `left trigger`, `right trigger`, `back`, `start`, `left stick`, `right stick`, `dpad up`, `dpad down`,
`dpad left` or `dpad right`. `none` leaves that binding to the other kind: an input with no key is a pad's alone.

```sh
printf 'start key f1\ncross key none\n' >> "${XDG_DATA_HOME:-$HOME/.local/share}/twinsanity/keybinds.txt"
```

**OPTIONS → controller options** (in the main menu and in the pause menu) has a row per input, showing what it is on
(`cross: Space / south`). Select a row and it waits for the next key or button pressed, which becomes the row's binding and is
written straight to the file; the pad button pressed is the one bound, so the layout can be changed with a controller alone.
While a row waits the menus' own select, back and directions are held off, so the press being bound is not also a menu move;
the menus' back (circle by default) ends the wait instead, and `escape` or a pad's back button ends it without binding (those
two are left to the file, so binding them means editing a line).

A line for an input this build doesn't have is kept, and one whose key or pad input has no name leaves the binding as it was;
both say so on stderr (and in `crash.log`, which is stderr). The file is read when the game starts, so a change made by hand
lands at the next run.

The main menu and the pause menu have an `exit` row, past the options and past the pause menu's quitting: it ends the game from
the pad, so closing it needs no keyboard and no window manager. Whatever the options screens changed is written to
`settings.txt` on the way out.

## Saves and settings

Memory cards are directories under the platform's per-user data directory: `%LOCALAPPDATA%\twinsanity` on Windows,
`$XDG_DATA_HOME/twinsanity` (`~/.local/share/twinsanity` when it is unset, `/tmp/twinsanity` without a home) elsewhere.
There is one per port and slot: `card0_0`, `card0_1`, `card1_0`, `card1_1`. Each holds the game's saves as they would be on
a card.

The path the game's files were found at is kept in `gamedata.txt` next to the saves (asked for again when it stops having them);
`settings.txt` beside it holds the game's options, one per line: `group <n> <left> <right>` for
the four volume groups (0 effects, 1 second effects, 2 music, 3 movie sound), `stereo <n>` and `widescreen <n>`. The game's
own save wins where they overlap. `keybinds.txt` sits beside them too (the [controls](#controls)).

`render <n>` is the picture's rate, the frames the port presents a second: 50 when the file has none, raised to the logic's
rate if set lower, and `0` for unlimited (the picture presents as fast as it can be drawn, so with vsync on the display is what
paces it). The game's logic always steps at the disc's field rate, whatever this says; the frames between two steps draw
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

`window`, `resolution`, `vsync`, `aspect` and `scale` are the picture's presentation, and the game's own OPTIONS then
GRAPHIC OPTIONS screen has a row for each, applied as the row is stepped and written back here:

| line | Values | Default |
|---|---|---|
| `window` | `windowed`, `borderless`, `fullscreen` (a full screen is the display's own mode and size) | `windowed` |
| `resolution` | `1280x720`: the window's pixels (what a full screen returns to) | `960x540` |
| `vsync` | `on`, `off` | `off` |
| `aspect` | `stretch` (the picture fills the window), `fit` (its 16:9 kept, black bars around it) | `stretch` |
| `scale` | `0.5`, `0.75`, `1`, `1.5`, `2`, `3`, `4`: the scene drawn at that multiple of 960x540 and then stretched to the window, so a sharper or cheaper picture whatever the window's size | `1` |
| `draw_distance` | `1`, `1.5`, `2`, `3`, `4`: the pop-in distance. It multiplies the lens's 1500-unit reach (the projection and the cull), keeps a level of detail drawing its last mesh past the distance the level's data stopped it at, stretches a load wall's 2-unit reach, and makes the chunk loader keep more links of chunks loaded (retail's 2 links, up to 5). The cost is memory and what the extra geometry draws | `1` |

The file is merged when it is written: your own comments and lines this build does not know are kept as they are.

The port's own rows are translated with the rest: the labels are text of the port's own (in english, french, german, spanish and
italian, in whichever language the texts are read in, and in japanese on the Japanese disc), not literals, so they follow the
`language`/`text_language` lines like the disc's menus do. The values a row shows (a resolution, a multiplier, a rate) are numbers
and stay as they are.

`language <name>` is the language the game's voices, movies, title pictures and credits are in: `english` when the file has
none, or `french`, `german`, `spanish`, `italian` -- the five the disc carries. The options screen's game page has rows for it and
for the texts ("voices (restart)" and "texts (restart)"): the game puts its languages in place once, as it starts, so a change
lands at the next run.

`text_language <name>` puts the texts alone in another language, for discs whose text banks were moved into another slot (a
translation patch: the English slot holding Portuguese, say). Without it the texts are in the `language` above.

```sh
echo 'language french' >> "${XDG_DATA_HOME:-$HOME/.local/share}/twinsanity/settings.txt"
echo 'text_language english' >> "${XDG_DATA_HOME:-$HOME/.local/share}/twinsanity/settings.txt"   # French menus, English texts
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
Another pc port resource that was taken as inspiration is [menaman123/TwinsanityEvolved](https://github.com/menaman123/TwinsanityEvolved)
Crash Twinsanity is property of Activision. This repository contains no game code or data.
