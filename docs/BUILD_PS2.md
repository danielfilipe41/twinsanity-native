# Building the PS2 version

The original decompilation build, which needs the PS2SDK toolchain from [ps2dev](https://github.com/ps2dev/ps2dev)
(GCC 15.2) and the split of the retail executable (`SLES_525.68`, see [Getting the game](../README.md#getting-the-game)).

## Linux

Download `ps2dev-ubuntu-latest.tar.gz` from ps2dev's latest release and extract it into `/usr/local` (or put its folder in
`PS2DEV` or `local.json`). Then:

```sh
sudo tar -xzf ps2dev-ubuntu-latest.tar.gz -C /usr/local
.venv/bin/python tools/split.py
.venv/bin/python tools/build.py
```

The build is `build/SLES_525.68.elf`. `tools/build.py --matching` builds the asm alone and checks that it reproduces the
retail load image byte for byte.

`tools/play.py` boots the build in PCSX2 with the ISO from `local.json` or `$TWINSANITY_ISO`.

## Windows

Extract `ps2dev-windows-latest.tar.gz` into `C:\` so the toolchain is at `C:\ps2dev`, and run the toolchain DLL fix once
after installing it (and again after replacing it):

```powershell
tar -xzf "$HOME\Downloads\ps2dev-windows-latest.tar.gz" -C C:\
py -3 -m venv .venv
.venv\Scripts\python.exe -m pip install -r requirements.txt
.venv\Scripts\python.exe tools\toolchain_dlls.py
.venv\Scripts\python.exe tools\split.py
.venv\Scripts\python.exe tools\build.py
```

## How it works

The decompilation, the split and the matching build are described in [DEVELOPMENT.md](DEVELOPMENT.md); the retail bugs the
C++ keeps are in [RETAIL_BUGS.md](RETAIL_BUGS.md).
