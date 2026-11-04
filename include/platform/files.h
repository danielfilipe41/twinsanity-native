#pragma once

#include "common.h"

// The game's files, by their paths on the disc ("Crash6\\Crash.bh", either slash, any case), and descriptors
namespace Platform::Files
{
enum OpenFlags : s32
{
    OpenRead = 0x1,
    OpenWrite = 0x2,
    OpenReadWrite = 0x3,
    OpenAppend = 0x100,
    OpenCreate = 0x200,
    OpenTruncate = 0x400,
};

enum Whence : s32
{
    SeekSet = 0,
    SeekCurrent = 1,
    SeekEnd = 2,
};

// The directory the game's own paths are read from: empty for the working directory (a run started in the disc's folder),
// or the folder the launch found the files in (Platform::GameData::Locate). Set once, before the game opens anything
void SetRoot(const char* path);
// Whether the disc being read is the European release's build, which ran at 50 fields a second: the North American and
// Japanese builds ran at 60. The game's own flag for it (g_Pal) is a word of the retail data, which this port has from the
// European build, so the disc's serial at its root is what says which it is
bool DiscIsPal();
// Forgets what it knew of the file system (open again after the I/O processor restarted)
void Reset();
// Returns the descriptor, negative on failure
s32 Open(const char* path, s32 flags);
s32 Close(s32 file);
// Return the bytes read or written, negative on failure
s32 Read(s32 file, void* buffer, s32 size);
s32 Write(s32 file, const void* buffer, s32 size);
// Returns the new position
s32 Seek(s32 file, s32 offset, Whence whence);
// Reads size bytes at offset without moving the descriptor's read position. The stream channels read in the background,
// two of them possibly from one file, so a read that moved the shared position would race: this leaves it alone. Returns
// the bytes read, negative on failure
s32 ReadAt(s32 file, u32 offset, void* buffer, s32 size);
}
