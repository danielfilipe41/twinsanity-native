// Files: the host's file system under the working directory.
#include "platform/files.h"

#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace
{
// The game names paths with either slash in any case; the host takes slashes as they are. The disc's files are
// uppercase (the PS2's ISO9660 reads case-insensitively) while the game asks lowercase: fall back to a
// case-insensitive directory scan when the exact path misses
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
    return converted;
}

bool SameLower(char a, char b)
{
    if (a >= 'A' && a <= 'Z')
    {
        a = static_cast<char>(a - 'A' + 'a');
    }
    if (b >= 'A' && b <= 'Z')
    {
        b = static_cast<char>(b - 'A' + 'a');
    }
    return a == b;
}

bool MatchesLower(const std::string& candidate, const std::string& wanted)
{
    if (candidate.size() != wanted.size())
    {
        return false;
    }
    for (u32 i = 0; i < candidate.size(); i++)
    {
        if (!SameLower(candidate[i], wanted[i]))
        {
            return false;
        }
    }
    return true;
}

// Opens path case-insensitively: walks each component, scanning its directory for a case-insensitive match
s32 OpenInsensitive(const std::string& path, s32 flags)
{
    std::string resolved;
    u32 start = 0;
    while (start <= path.size())
    {
        u32 slash = static_cast<u32>(path.find('/', start));
        bool last = slash == static_cast<u32>(std::string::npos);
        std::string want = last ? path.substr(start) : path.substr(start, slash - start);
        std::string directory = resolved.empty() ? "." : resolved;
        DIR* dir = ::opendir(directory.c_str());
        if (dir == nullptr)
        {
            return -1;
        }
        bool matched = false;
        while (dirent* entry = ::readdir(dir))
        {
            if (MatchesLower(entry->d_name, want))
            {
                resolved = directory == "." ? entry->d_name : directory + "/" + entry->d_name;
                matched = true;
                break;
            }
        }
        ::closedir(dir);
        if (!matched)
        {
            return -1;
        }
        if (last)
        {
            break;
        }
        start = slash + 1;
    }
    return ::open(resolved.c_str(), flags, 0666);
}

s32 HostFlags(s32 flags)
{
    s32 access = flags & Platform::Files::OpenReadWrite;
    s32 host = (access == Platform::Files::OpenRead)
        ? O_RDONLY
        : ((access == Platform::Files::OpenWrite) ? O_WRONLY : O_RDWR);
    if ((flags & Platform::Files::OpenAppend) != 0)
    {
        host |= O_APPEND;
    }
    if ((flags & Platform::Files::OpenCreate) != 0)
    {
        host |= O_CREAT;
    }
    if ((flags & Platform::Files::OpenTruncate) != 0)
    {
        host |= O_TRUNC;
    }
    return host;
}

s32 HostWhence(Platform::Files::Whence whence)
{
    switch (whence)
    {
        case Platform::Files::SeekSet:
            return SEEK_SET;
        case Platform::Files::SeekCurrent:
            return SEEK_CUR;
        case Platform::Files::SeekEnd:
            return SEEK_END;
    }
    return SEEK_SET;
}
}

void Platform::Files::Reset()
{
}

s32 Platform::Files::Open(const char* path, s32 flags)
{
    std::string host = HostPath(path);
    s32 hostFlags = HostFlags(flags);
    s32 file = ::open(host.c_str(), hostFlags, 0666);
    if (file < 0 && (flags & Platform::Files::OpenCreate) == 0)
    {
        file = OpenInsensitive(host, hostFlags);
    }
    return file;
}

s32 Platform::Files::Close(s32 file)
{
    return ::close(file);
}

s32 Platform::Files::Read(s32 file, void* buffer, s32 size)
{
    return static_cast<s32>(::read(file, buffer, static_cast<size_t>(size)));
}

s32 Platform::Files::Write(s32 file, const void* buffer, s32 size)
{
    return static_cast<s32>(::write(file, buffer, static_cast<size_t>(size)));
}

s32 Platform::Files::Seek(s32 file, s32 offset, Whence whence)
{
    return static_cast<s32>(::lseek(file, offset, HostWhence(whence)));
}
