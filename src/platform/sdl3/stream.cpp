#include "platform/stream.h"
#include "platform/files.h"

#include "audio/soundmodule.h"
#include "audio/audio.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// The sdl3 host's side of Platform::Stream: reads from the disc files on the host filesystem through Platform::Files
// (the null platform's own stream.cpp returns 0 from FreeBufferMemory and drops sound banks, which stops music playing).
namespace Platform::Stream
{
namespace
{
struct FileInfo
{
    s32 fd;
    u32 size;
    // Kept so a bank read can say which disc file its offset belongs to: a sound looked at offline at the wrong offset is a
    // sound that reads as noise and says nothing about the sound
    std::string path;
};

std::vector<FileInfo> g_OpenFiles;
s32 g_NextFile = 0;
// The size of the file opened last: FileSize is its size until the next file is opened (platform/stream.h)
u32 g_LastFileSize = 0;

s32 GetFreeSlot()
{
    for (size_t i = 0; i < g_OpenFiles.size(); i++)
    {
        if (g_OpenFiles[i].fd < 0)
        {
            return static_cast<s32>(i);
        }
    }
    g_OpenFiles.push_back({-1, 0, {}});
    return static_cast<s32>(g_OpenFiles.size() - 1);
}
}

s32 Initialise(s32 channels, void* workMemory)
{
    (void)channels;
    (void)workMemory;
    g_OpenFiles.clear();
    g_NextFile = 0;
    g_LastFileSize = 0;
    return 0;
}

s32 Update()
{
    return 0;
}

State GetState()
{
    return State::Running;
}

void AttachBuffer(s32 channel, u32 size, u32 soundAddress, u32 soundSize)
{
    (void)size;
    // A channel that plays music out of the sound processor's memory is given the ring the player reserved there: the module
    // streams the track's blocks through it (the game's own file stream, whose buffer this is, reads nothing itself)
    if (soundAddress != 0 && soundSize != 0)
    {
        SdlAudio::LockAudio();
        SdlAudio::Spu2::Module().SetChannelBuffer(channel, soundAddress, soundSize);
        SdlAudio::UnlockAudio();
    }
}

void DetachBuffer(s32 channel)
{
    (void)channel;
}

u32 FreeBufferMemory()
{
    return 0x1000000; // large amount
}

s32 OpenFile(const char* path)
{
    if (path == nullptr)
    {
        return NoFile;
    }
    // The disc path straight to the host's files (Platform::Files matches the case)
    s32 fd = Platform::Files::Open(path, Platform::Files::OpenRead);
    if (fd < 0)
    {
        g_LastFileSize = 0;
        return NoFile;
    }

    // The file's size, the read position left where it was
    s32 old = Platform::Files::Seek(fd, 0, Platform::Files::SeekCurrent);
    s32 end = Platform::Files::Seek(fd, 0, Platform::Files::SeekEnd);
    Platform::Files::Seek(fd, old, Platform::Files::SeekSet);
    if (end < 0)
    {
        end = 0;
    }

    g_LastFileSize = static_cast<u32>(end);
    s32 slot = GetFreeSlot();
    g_OpenFiles[slot] = {fd, g_LastFileSize, path};
    return slot;
}

u32 FileSize()
{
    return g_LastFileSize;
}

void CloseFile(s32 file)
{
    if (file >= 0 && file < static_cast<s32>(g_OpenFiles.size()))
    {
        if (g_OpenFiles[file].fd >= 0)
        {
            Platform::Files::Close(g_OpenFiles[file].fd);
            g_OpenFiles[file].fd = -1;
            g_OpenFiles[file].size = 0;
        }
    }
}

void Read(s32 channel, s32 file, u32 offset, u32 size, void* destination)
{
    (void)channel;
    if (file < 0 || file >= static_cast<s32>(g_OpenFiles.size())) return;
    if (g_OpenFiles[file].fd < 0) return;
    s32 fd = g_OpenFiles[file].fd;
    Platform::Files::Seek(fd, static_cast<s32>(offset), Platform::Files::SeekSet);
    Platform::Files::Read(fd, destination, static_cast<s32>(size));
}

void ReadSoundBank(s32 channel, u32 bank, s32 file, u32 offset, u32 size)
{
    (void)channel;
    if (file < 0 || file >= static_cast<s32>(g_OpenFiles.size()) || g_OpenFiles[file].fd < 0 || size == 0)
    {
        return;
    }

    // Retail loads the bank's samples straight into the sound processor's memory (ps2/stream.cpp's LoadToSpu, the module
    // writing at g_MsSoundBankAddress and MsSetBankAddress giving them their sound's ID). The host reads them and hands them
    // to the module, which puts them in its own memory and registers them under the sound's ID.
    if (std::getenv("TS_AUDIOLOG") != nullptr || std::getenv("TS_SOUNDLOG") != nullptr)
    {
        // Where on the disc the sound is, so its own bytes can be looked at offline. The path is here because the offset is
        // only meaningful against the right file: the port's file ids are not the disc's order
        std::fprintf(stderr, "audio: bank %u from file %d (%s) at %u, %u bytes\n", bank, file, g_OpenFiles[file].path.c_str(),
                     offset, size);
    }

    std::vector<u8> samples(size);
    Read(channel, file, offset, size, samples.data());
    SdlAudio::LockAudio();
    SdlAudio::Spu2::Module().LoadSoundBank(bank, samples.data(), size);
    SdlAudio::UnlockAudio();
}

bool IsReading(s32 channel)
{
    (void)channel;
    return false; // reads complete synchronously
}

s32 Wait(s32 channel)
{
    (void)channel;
    return 0;
}
}