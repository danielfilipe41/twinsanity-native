#pragma once

#include "common.h"

// The PS2 disc's own file system, read straight out of a disc image: ISO9660 (ECMA-119) level 1, 2048-byte logical sectors,
// the primary volume descriptor at sector 16, the root directory record at 156 in it, and names recorded uppercase with a ";1"
// version suffix ("CRASH6\SYS\IOPRP300.IMG;1"). A raw dump (.bin, 2352 bytes per sector: 12-byte sync, 3-byte address, mode
// byte, then the 2048 user bytes) is read through the same interface by remapping every logical read, so callers see one
// 2048-byte-per-sector volume whichever file they were given.
//
// A disc image is what a player has before extracting anything, so the port reads one directly when it is pointed at one: the
// reader is here, beside the file system it stands in for (files.cpp), and Platform::GameData decides whether a path names a
// folder of extracted files or an image.
namespace Platform::Iso9660
{
// Opens the image and parses its volume: false, with the reason on stderr, when the file isn't an ISO9660 volume
bool Open(const char* imagePath);
void Close();
bool IsOpen();

// The image bytes that hold the file the game's path names ("Crash6\\SYS\\Ioprp300.img", either slash, any case, with or
// without "cdrom0:" and ";1"). False when the volume has no such file, or when it names a directory
bool Find(const char* gamePath, u64* offset, u32* size);

// Bytes at a position in the image's logical space (offset is a byte, so a file at LBA 300000 reads at 300000 * 2048).
// Positioned, so two threads reading do not split each other's read; returns the bytes read, negative on failure
s32 ReadAt(u64 offset, void* buffer, s32 size);

// The volume's logical size in bytes: sectors * 2048, not the file's size when the file is raw
u64 Bytes();
// The volume identifier the image records ("CRASH6" on the PAL disc)
const char* VolumeId();

// Whether a path names a file that is a disc image: a regular file whose sector 16 carries the ISO9660 descriptor, or whose
// first sector carries a raw dump's sync pattern. Cheap enough to call on whatever the player picked
bool LooksLikeImage(const char* path);
}
