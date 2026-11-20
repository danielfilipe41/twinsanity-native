# Crash Twinsanity: decompilation and native port

This project is a multi-system port of Crash Twinsanity (PAL, SLES-525.68), built on top of
[Smartkin's decompilation of the game](https://github.com/Smartkin/twinsanity-reversed). The PlayStation 2 backend comes
from that decomp: the game code, the PS2 platform layer and the matching build. This repository adds a native Linux/PC
backend and is where the port work happens now.

The PC build uses SDL3 for the window, input and audio, and bgfx for rendering. The game code is the same C++ as the PS2
build (minor some tweaks and things necessary for some features); it reaches the hardware through the platform layer, so the renderer, audio, files and pads can be replaced one system at a time. The 32-bit build keeps the retail pointer layouts and absolute addresses.

Nothing from the game is included in the repository. You need your own copy of the disc: any of its four releases plays (see
[Other regions](#other-regions)), and the European one is the only one that can be built with.

**Play in your browser:** <https://danielfilipe41.github.io/twinsanity-native/> — the page takes your own disc (the ISO, or
dragged in as a file) and boots the game in the tab. Nothing is uploaded and the site serves no game data; your saves and
mods are kept in the browser's own storage.

## Status

Linux runs perfectly, Windows cross-builds from Linux and runs under Wine, and the web build plays in a browser; every
other platform has its backend only.

Known issues:
- Sometimes camera gets stuck on places still not sure if this is retail behaviour or not
- Pop in still kinda noticeable idk how to fix
- Sometimes audio plays for a bit then stops then goes again
- Rarely cutscene skip's are so fast that the game doesnt have time to load the terrain (if you can reproduce it please open up an issue about it)
- ON LINUX IT ONLY RUNS IF YOU HAVE LIBGL for 32 bit okay so if you dont have it if you have integrated graphics and amd/intel gpus download lib32-mesa if you use nvidia download the package for that google it idk

Future features:
- Android build
- Other consoles button glyphs

QOL Changes:
- Pc port settings
- Rebindable controls, on the keyboard and the pad, from `keybinds.txt` or the options' remap controls page
- A stick dead zone, in the game options (off to 20%)
- Multiple save pages: the save screens' pages are memory cards, stepped with L1/L2/R1/R2 (four cards, retail saves untouched)
- Exit game option (also on pause screen the old quit game that sent u back to the title screen now closes the game and is called close game)
- Added option to start a new game from inside the save game option on the pause menu
- Removed the delay on the Save file check screens
- Fast loading screens (on an nvme it only takes 62ms)
- Cutscene skip, in the game options and on by default. The scenes' own hint is on screen while they play. Off is retail's behaviour. Every watched scene can be skipped: a cutscene file skips outright, and a scripted scene runs out under a black fade at the `cutscene_speed` rate, so its own scripts still reach its own end. The `skip` hint is shown for every scripted scene, and a skipped scene is silent (its sounds are mixed at nothing, the music plays on) with no pad rumble or camera shake until it ends. A scene that waits on something the rate can't reach (the player, a sound) gets its picture back after 45 s rather than racing on

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

## Building without the retail data (DATA=runtime)

The `game-with-data` build carries the retail program's tables, strings and vtables in the executable: they come out of
`SLES_525.68` through the split (`tools/split.py`, `tools/host_data.py`). A build can carry none of them instead:

```sh
DATA=runtime PLATFORM=sdl3 python3 configure.py
ninja game
```

It links the game's code alone. At the first start it reads `SLES_525.68` out of the disc it is playing (the folder, or the
image named with `--disc`) and fills the image the code reads its tables from, pointing every pointer the data holds at this
port's own function for it, or at the same place in the image for a pointer to data. The disc's executable is checked against
the European release's SHA-1 before any of it is used: another release's program has its data at other addresses, and is
refused rather than half read. Nothing of the game's data is in such an executable, so it can be handed to anyone who owns
the disc, and it plays from their copy. The disc itself is needed at every start, as it is for the data files either way.

`--verify-data` does the load and says what it read and relocated, without starting the game:

```sh
build/host/sdl3/game-with-data --disc "Crash Twinsanity (Europe).iso" --verify-data
```

The addresses and names it needs are `tools/retail_symbols.h`, generated from `symbol_addrs.txt`; `tools/retail_map.py
--check` compares that map with the pointer words the split itself made, and `tools/retail_map.py` writes it again after a
split that changes the symbols. The build and its checks are the Linux host's (`TARGET=linux`, `PLATFORM=sdl3`); every other
build, the PS2 one and the data-bearing host one, is unchanged.

## Mods

The game plays the community's mods without a launcher: one folder holds a folder per mod, and `mods.txt` in it lists the ones
that are on, one per line, in the order they apply (`#` and `;` start a comment; a line of `*` or `all` turns on every mod the
folder has). Nothing is written to the disc.

The folder is chosen in this order: `--mods-dir`, `TWINSANITY_MODS`, the `mods_dir` line of `settings.txt` (one line, the whole
path, set by hand or by a launcher), `mods/` beside the disc's files, `./mods`, then the host's own writable folder — `mods/`
under the platform's preferences directory, made when it is not there. That last one is what a phone or a page uses, where the
disc's own folder cannot be written; `mods.txt` and unpacked zips live in whichever folder was chosen, so the mods page can save
whenever that folder is writable. A folder that exists but cannot be written still has its mods read: the log says its
`mods.txt` cannot change and zips cannot unpack there.

Four shapes are read, decided by what a folder holds, never by its name:

- **A Mod Crate** (Crate Mod Loader's, what the community ships): `ModCrateInfo.txt`, `layer0/` (the disc's files) and
  `layer1/` (entries of `CRASH6/CRASH.BD` by their index path).
- **A TwinsanityEvolved mod folder**: `mod.json` with a `disc/` tree laid out like the disc. A file of that tree may come as
  the ranges a mod changes of it instead of whole -- TwinsanityEvolved's `<disc path>.ranges` recipe naming the slices of the
  disc's own file (`base <offset> <length>`), of the mod's payload (`file <path> <offset> <length>`) and the zeroes between
  them -- and the port composes the file as the game reads it, so a mod that changes a few bytes of a 900 MB `CRASH.BD` costs
  only those bytes.
- **A folder of the game's own built files** (TT Lab's output, or loose files): `CRASH6/` and `FMV/` are the disc's files, and
  `Startup/`, `Levels/`, `Language/` and `Extras/` are the archive's entries. A bare file the tools write (`Default.rm2`,
  `bossarea.sm2`, a text bank) is placed by its name when the archive has exactly one entry of that name.
- **A patch of the disc image** -- a `.xdelta`/`.vcdiff`, `.ppf` or `.bps` file, loose in the mods folder or in a folder of
  its own, one per mod. The patch is laid over the disc image as it is read, so a patched disc is played without writing a
  patched copy anywhere. It has to be a plain ISO (a raw dump's sector offsets are its own and are not read), and only one
  patch mod is applied at a time: a second patch, or a patch chosen while the game plays a folder of files, is said in the
  log and left off. The patched image's saves go to its own `cards-mods/<patch>/` too.

A mod's disc files replace the disc's whole. Its archive entries go into a rebuilt `CRASH6/CRASH.BH` and are appended to a
composed `CRASH6/CRASH.BD`, so an entry may grow or shrink without the disc being touched. Mods are read at start-up, so a
change takes effect the next time the game starts. While any mod is on, the saves go to the memory cards of the mods that are
on, never the vanilla ones and never another lot's: `cards-mods/<mod>/` inside the game's data folder, or `<first>-<digest>`
when several are on (the digest covers the mods in the order they apply, so the same mods always find the same cards).

```sh
build/host/sdl3/game-with-data --mods-dir "path/to/mods" --mod DarkLair   # add one to what mods.txt lists, for this run
build/host/sdl3/game-with-data --no-mods                                  # the disc as it is
build/host/sdl3/game-with-data --mods-dir "path/to/mods" --verify-mods   # every file the mods change, byte for byte
build/host/sdl3/game-with-data --skip-intro                             # the title, with the two boot logos unplayed
```

In the game, **options → mods** shows a row per mod in the folder, on or off, in the order they apply: toggling a row writes
`mods.txt` at once, and since the mods are read as the game starts, the page says a restart applies the change.

A crate shipped as a `.zip` needs no unpacking by hand: the game unpacks it once into `mods/<zip name>/` and lists it like any
other mod (a marker says which zip was unpacked; a newer zip unpacks again). **Not applied**, and said in the log: a crate's
`ModCrateSettings.txt` and scripts (code changes of Crate Mod Loader's own), a modified executable's code (the port runs its
own), and TwinsanityEvolved language packs (this port plays its disc's own languages).

A patch is read in the three formats the community ships: xdelta3's VCDIFF, PPF 1.0/2.0/3.0 and BPS. A bit of it may still be
left to another tool, and the log says so rather than guessing: an xdelta3 patch made with secondary compression or with its
own code table, and a patch that writes more than 384 MB of new bytes. In those cases apply the patch to the image with
xdelta3 or Delta Patcher and play the patched image, which the port reads as any other disc.

The same chain runs on every host. On Android the preferences folder is the app's own writable storage and `mods_dir` can name
a folder the app can reach (its own files folder, or one a launcher prepared); on the web the preferences folder is mounted as
IDBFS (kept in the browser's storage) and flushed as the run goes, so the saves, the modded saves' own folders, the mods and
`mods.txt` survive a reload. The disc image itself is only in the page's memory, and is asked for again the next time.

## Building

| Platform | Document | State |
|---|---|---|
| Linux | [docs/BUILD_LINUX.md](docs/BUILD_LINUX.md) | builds and runs; OpenGL and Vulkan |
| Windows | [docs/BUILD_WINDOWS.md](docs/BUILD_WINDOWS.md) | cross-builds from Linux, runs under Wine |
| Web (wasm32) | [docs/BUILD_WEB.md](docs/BUILD_WEB.md) | builds and runs in the browser; published to GitHub Pages |
| PS2 | [docs/BUILD_PS2.md](docs/BUILD_PS2.md) | the original decompilation build |

Every build needs a 32-bit x86 toolchain (the retail data uses 32-bit pointers and fixed addresses), Python 3.10 or later
and the submodules; CMake as well for SDL3, which `configure.py` builds from the submodule and links in statically, so the
binary needs no SDL3 package.
[docs/BUILDING.md](docs/BUILDING.md) holds those, the renderers each platform takes, and how the shader blobs are made.
Linux, start to finish:

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

**OPTIONS → remap controls** (in the main menu and in the pause menu) has a row per input, showing what it is on
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

The save screens' pages are those cards, a page each: **L1**/**L2** step to the card before, **R1**/**R2** to the one after
(wrapping round), with the menus' page hints at the screen's edges, and the slots screen's title says which card it is
(`(card 2/4)`). The choice is kept in `settings.txt` as `save_card`, so the next run opens on it. A card holds the same four
save slots as it always did, in retail's own save directory, so nothing about a save changes: a card's folder can be copied to
another machine, or to another card, as it is. A card with no save on it yet has the save's folder made as it's opened and
shows its four empty slots — no "create a save?" question to answer before the slots can be seen, and the pages still step
either way.

The path the game's files were found at is kept in `gamedata.txt` next to the saves (asked for again when it stops having them);
`settings.txt` beside it holds the game's options, one per line: `group <n> <left> <right>` for
the four volume groups (0 effects, 1 second effects, 2 music, 3 movie sound), `stereo <n>` and `widescreen <n>`. The game's
own save wins where they overlap. `keybinds.txt` sits beside them too (the [controls](#controls)).

`mods_dir <path>` is the mods folder the game reads when it is not `mods/` beside the disc's files (see [Mods](#mods)): one
line, the whole path, written by hand or by a launcher. The mods are read once, as the game starts.

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
| `scale` | `0.5`, `0.75`, `1`, `1.5`, `2`, `3`, `4`: the scene drawn at that multiple of 960x540 and then stretched to the window, so a sharper or cheaper picture whatever the window's size. Every pass is drawn at the scene's size (the geometry, the sky's half-size buffer, the UI, the particles, the decals, the shadows, the fog), and the picture is resolved from it with the bilinear filter, so the extra pixels are filtered down rather than dropped | `1` |
| `draw_distance` | `1`, `1.5`, `2`, `3`, `4`: the pop-in distance. It multiplies the lens's 1500-unit reach (the projection and the cull), keeps a level of detail drawing its last mesh past the distance the level's data stopped it at, stretches a load wall's 2-unit reach, and makes the chunk loader keep more links of chunks loaded (retail's 2 links, up to 5). The cost is memory and what the extra geometry draws | `1` |
| `deadzone` | `0`, `5`, `10`, `15`, `20`: the sticks' dead zone, a percentage of an axis' half range. Inside it a stick reads as the middle, and outside it is moved out to the zone's edge and scaled over the rest of the range, so the centre holds still without the outer travel shrinking. GAME OPTIONS has the row | `0` |
| `save_card` | `0` to `3`: the card the save screens open on (their pages are the cards), written as the pages are stepped | `0` |
| `cutscene_skip` | `on`, `off`: whether cutscenes can be skipped (the game options' row). `on` skips a cutscene file on its first viewing and lets a scripted scene with a skip of its own end; `off` is retail's, a file only once it has been played | `on` |
| `screen_shake` | `on`, `off`: whether the camera's shake is drawn (the game options' row). `off` keeps the picture still; the pad's vibration is the game's own option | `on` |
| `skip_intro` | `on`, `off`: whether the two logos the game boots with are skipped (the host's `--skip-intro` does the same for one run). Every movie after them still plays: a new game's intro, a level's, a mod's | `off` |
| `cutscene_speed` | `1` to `1000`: how many seconds of a scripted cutscene a skip runs at a time while the scene runs out unseen. A step holds that much of the scene, so a low rate plays it near normal and a high one passes its triggers and physics between steps; the camera and the characters come out where the scene's own end leaves them at any rate | `10` |

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

## Frame rate and profiling

With `TWINSANITY_FRAME_LOG=1`, once a second in the log; with `TWINSANITY_FPS=1`, in the corner of the picture. Both are off by
default, so a normal run prints nothing:

```
59.9 fps  50 steps/s  cpu 1.32 ms  gpu 3.06 ms  412 draws
host frame 1830: fps 59.9 steps 50 cpu 1.32 ms gpu 3.06 ms draws 412 views 34 ...
```

* **fps** is the picture's rate: the frames the display actually took. The port presents at the settings file's `render` rate
  (`settings.txt`, 50 by default) and vsync paces it, so this is where a dropped frame shows up.
* **steps/s** is the game's own logic rate, which the disc fixes at 50. It is *separate* from the picture's rate, so more
  frames than steps is right; fewer steps than 50 means the game is falling behind the console's clock and every per-frame
  count in the game (animations, particles, movers) is running slow.
* **cpu** is the CPU time bgfx measured between two presents (the whole frame: game step, draw submission and the present),
  and **gpu** the card's time for the frame before.
* **draws** is the number of draw calls the frame submitted.

The log line and the corner text are both off by default: `TWINSANITY_FRAME_LOG=1` turns the line on, `TWINSANITY_FPS=1` the
corner text. Both read `0` as unset.

For *where* the time goes rather than how much of it there is, use an external profiler. `tools/profile.sh` wraps perf(1):

```
./tools/profile.sh                 # record a run, then print the hottest functions
./tools/profile.sh --stat          # counters only: cycles, cache and branch misses, context switches
./tools/profile.sh --flame         # also write build/profile.folded for flamegraph.pl
./tools/profile.sh --report build/profile.data
```

perf is a package of its own on Arch/CachyOS (`sudo pacman -S perf`), and the script says so when it is missing. Recording
needs `kernel.perf_event_paranoid` at 2 or lower (`sudo sysctl kernel.perf_event_paranoid=2`); the game needs no rebuild for
it, because the build keeps its symbols.

## Credits

The decompilation this port is built on is [Smartkin/twinsanity-reversed](https://github.com/Smartkin/twinsanity-reversed).
Another pc port resource that was taken as inspiration is [menaman123/TwinsanityEvolved](https://github.com/menaman123/TwinsanityEvolved)
Crash Twinsanity is property of Activision. This repository contains no game code or data.
