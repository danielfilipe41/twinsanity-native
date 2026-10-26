// Background reads: synchronous reads over the host file system (the disc folder when run from it).
// Read performs the read at once, so nothing is ever in flight: IsReading is false and Wait returns at once.
#include "platform/stream.h"

#include "platform/files.h"

#include <string>
#include <vector>

namespace
{
// An open disc file by number: its host descriptor and its size (FileSize reports it until the next opens)
struct HostFile
{
    bool open = false;
    s32 descriptor = -1;
    u32 size = 0;
};

std::vector<HostFile> g_Files;
u32 g_LastSize = 0;

std::string HostPath(const char* path)
{
    std::string converted(path);
    for (char& letter : converted)
    {
        if (letter == '\\')
        {
            letter = '/';
        }
    }
    // The game names disc paths from the disc's root (Crash6\...); running from the disc folder finds them as-is
    return converted;
}
}

s32 Platform::Stream::Initialise(s32 channels, void* workMemory)
{
    (void)channels;
    (void)workMemory;
    return 0;
}

s32 Platform::Stream::Update()
{
    return 0;
}

Platform::Stream::State Platform::Stream::GetState()
{
    return State::Running;
}

void Platform::Stream::AttachBuffer(s32 channel, u32 size, u32 soundAddress, u32 soundSize)
{
    (void)channel;
    (void)size;
    (void)soundAddress;
    (void)soundSize;
}

void Platform::Stream::DetachBuffer(s32 channel)
{
    (void)channel;
}

u32 Platform::Stream::FreeBufferMemory()
{
    return 0;
}

s32 Platform::Stream::OpenFile(const char* path)
{
    s32 descriptor = Platform::Files::Open(HostPath(path).c_str(), Platform::Files::OpenRead);
    if (descriptor < 0)
    {
        return NoFile;
    }

    s32 size = Platform::Files::Seek(descriptor, 0, Platform::Files::SeekEnd);
    Platform::Files::Seek(descriptor, 0, Platform::Files::SeekSet);
    HostFile file;
    file.open = true;
    file.descriptor = descriptor;
    file.size = size < 0 ? 0 : static_cast<u32>(size);
    g_LastSize = file.size;
    g_Files.push_back(file);
    return static_cast<s32>(g_Files.size()) - 1;
}

u32 Platform::Stream::FileSize()
{
    return g_LastSize;
}

void Platform::Stream::CloseFile(s32 file)
{
    if (file < 0 || static_cast<u32>(file) >= g_Files.size() || !g_Files[file].open)
    {
        return;
    }

    Platform::Files::Close(g_Files[file].descriptor);
    g_Files[file].open = false;
}

void Platform::Stream::Read(s32 channel, s32 file, u32 offset, u32 size, void* destination)
{
    (void)channel;
    if (file < 0 || static_cast<u32>(file) >= g_Files.size() || !g_Files[file].open)
    {
        return;
    }

    Platform::Files::Seek(g_Files[file].descriptor, static_cast<s32>(offset), Platform::Files::SeekSet);
    Platform::Files::Read(g_Files[file].descriptor, destination, static_cast<s32>(size));
}

void Platform::Stream::ReadSoundBank(s32 channel, u32 bank, s32 file, u32 offset, u32 size)
{
    // The null platform has no sound processor memory: reads into it are dropped (music streams silently)
    (void)channel;
    (void)bank;
    (void)file;
    (void)offset;
    (void)size;
}

bool Platform::Stream::IsReading(s32 channel)
{
    (void)channel;
    return false;
}

s32 Platform::Stream::Wait(s32 channel)
{
    (void)channel;
    return 0;
}
