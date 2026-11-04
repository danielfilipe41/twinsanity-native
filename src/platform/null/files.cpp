// Files: the host's file system under the working directory, in standard C++ (std::filesystem for the paths, std::FILE for
// the handles) so every host platform uses the same file. The game names paths with either slash in any case; the disc's
// files are uppercase while the game asks lowercase, so a miss falls back to a case-insensitive walk of the components.
#include "platform/files.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace
{
// A handle's slot: its stream and the lock its operations take (the stream's read position is shared state; the stream
// channels read through ReadAt, which doesn't move it)
struct HostFile
{
    std::FILE* stream = nullptr;
    std::mutex mutex;
};

// The handles, by the numbers the game is given. A closed handle's slot is null and reused by the next Open
std::mutex g_TableMutex;
std::vector<std::unique_ptr<HostFile>> g_Files;

// The disc's folder when the game wasn't started in it (Platform::GameData::Locate), empty for the working directory
std::string g_Root;

std::string HostPath(const char* path)
{
    std::string converted(path != nullptr ? path : "");
    for (char& letter : converted)
    {
        if (letter == '\\')
        {
            letter = '/';
        }
    }

    // A relative path is one of the disc's own: resolved against the root when there is one. An absolute path (a leading
    // slash, or a drive letter) is the host's own and is left alone
    const bool absolute = !converted.empty() && (converted[0] == '/' || (converted.size() > 1 && converted[1] == ':'));
    if (!absolute && !g_Root.empty())
    {
        return g_Root + "/" + converted;
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

// The path with each component resolved case-insensitively, or empty when one has no match: a component is taken as it
// is when the file system has it (a case-insensitive one does, so the scan only runs where it's needed)
std::string ResolveInsensitive(const std::string& path)
{
    std::filesystem::path resolved;
    for (const std::filesystem::path& part : std::filesystem::path(path))
    {
        std::error_code error;
        std::filesystem::path candidate = resolved / part;
        if (std::filesystem::exists(candidate, error))
        {
            resolved = candidate;
            continue;
        }

        std::filesystem::path directory = resolved.empty() ? std::filesystem::path(".") : resolved;
        bool matched = false;
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory, error))
        {
            if (MatchesLower(entry.path().filename().string(), part.string()))
            {
                resolved = entry.path();
                matched = true;
                break;
            }
        }
        if (!matched)
        {
            return {};
        }
    }
    return resolved.string();
}

bool Exists(const std::string& path)
{
    std::error_code error;
    return std::filesystem::exists(path, error);
}

const char* HostMode(s32 flags)
{
    const bool read = (flags & Platform::Files::OpenRead) != 0;
    const bool write = (flags & Platform::Files::OpenWrite) != 0;
    const bool create = (flags & Platform::Files::OpenCreate) != 0;
    const bool truncate = (flags & Platform::Files::OpenTruncate) != 0;
    const bool append = (flags & Platform::Files::OpenAppend) != 0;
    if (append)
    {
        return read ? "a+b" : "ab";
    }
    if (read && write)
    {
        return create ? "w+b" : "r+b";
    }
    if (write)
    {
        return (create || truncate) ? "wb" : "r+b";
    }
    return "rb";
}

int HostWhence(Platform::Files::Whence whence)
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

// The handle at a number, null when there's none (the table's lock guards the lookup; the handle itself outlives it)
HostFile* HandleAt(s32 file)
{
    std::lock_guard<std::mutex> lock(g_TableMutex);
    if (file < 0 || static_cast<size_t>(file) >= g_Files.size())
    {
        return nullptr;
    }
    return g_Files[file].get();
}
}

void Platform::Files::SetRoot(const char* path)
{
    g_Root.clear();
    if (path == nullptr)
    {
        return;
    }
    // Held with the slashes the rest of this file splits on (the case-insensitive fallback walks the components)
    for (const char* letter = path; *letter != '\0'; letter++)
    {
        g_Root += *letter == '\\' ? '/' : *letter;
    }
    while (g_Root.size() > 1 && g_Root.back() == '/')
    {
        g_Root.pop_back();
    }
}

void Platform::Files::Reset()
{
    // The handles stay open: the game keeps reading from them once the I/O processor is back
}

s32 Platform::Files::Open(const char* path, s32 flags)
{
    if (path == nullptr)
    {
        return -1;
    }

    const bool create = (flags & OpenCreate) != 0;
    const bool writing = (flags & (OpenWrite | OpenTruncate)) != 0;
    std::string host = HostPath(path);
    // A write of a file that isn't there fails, as the PS2's open does; the case-insensitive name still finds it
    if (!create && writing && !Exists(host))
    {
        host = ResolveInsensitive(host);
        if (host.empty())
        {
            return -1;
        }
    }

    std::FILE* stream = std::fopen(host.c_str(), HostMode(flags));
    if (stream == nullptr && !create)
    {
        const std::string resolved = ResolveInsensitive(HostPath(path));
        if (!resolved.empty())
        {
            stream = std::fopen(resolved.c_str(), HostMode(flags));
        }
    }
    if (stream == nullptr)
    {
        return -1;
    }

    auto file = std::make_unique<HostFile>();
    file->stream = stream;
    std::lock_guard<std::mutex> lock(g_TableMutex);
    s32 number = -1;
    for (size_t slot = 0; slot < g_Files.size(); slot++)
    {
        if (!g_Files[slot])
        {
            number = static_cast<s32>(slot);
            g_Files[slot] = std::move(file);
            break;
        }
    }
    if (number < 0)
    {
        number = static_cast<s32>(g_Files.size());
        g_Files.push_back(std::move(file));
    }
    return number;
}

s32 Platform::Files::Close(s32 file)
{
    std::unique_ptr<HostFile> handle;
    {
        std::lock_guard<std::mutex> lock(g_TableMutex);
        if (file < 0 || static_cast<size_t>(file) >= g_Files.size() || !g_Files[file])
        {
            return -1;
        }
        handle = std::move(g_Files[file]);
    }

    std::lock_guard<std::mutex> lock(handle->mutex);
    return std::fclose(handle->stream) == 0 ? 0 : -1;
}

s32 Platform::Files::Read(s32 file, void* buffer, s32 size)
{
    HostFile* handle = HandleAt(file);
    if (handle == nullptr || size < 0 || (buffer == nullptr && size != 0))
    {
        return -1;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    return static_cast<s32>(std::fread(buffer, 1, static_cast<size_t>(size), handle->stream));
}

s32 Platform::Files::Write(s32 file, const void* buffer, s32 size)
{
    HostFile* handle = HandleAt(file);
    if (handle == nullptr || size < 0 || (buffer == nullptr && size != 0))
    {
        return -1;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    return static_cast<s32>(std::fwrite(buffer, 1, static_cast<size_t>(size), handle->stream));
}

s32 Platform::Files::Seek(s32 file, s32 offset, Whence whence)
{
    HostFile* handle = HandleAt(file);
    if (handle == nullptr)
    {
        return -1;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (std::fseek(handle->stream, static_cast<long>(offset), HostWhence(whence)) != 0)
    {
        return -1;
    }
    const long position = std::ftell(handle->stream);
    return position < 0 ? -1 : static_cast<s32>(position);
}

s32 Platform::Files::ReadAt(s32 file, u32 offset, void* buffer, s32 size)
{
    HostFile* handle = HandleAt(file);
    if (handle == nullptr || size < 0 || (buffer == nullptr && size != 0))
    {
        return -1;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (std::fseek(handle->stream, static_cast<long>(offset), SEEK_SET) != 0)
    {
        return -1;
    }
    return static_cast<s32>(std::fread(buffer, 1, static_cast<size_t>(size), handle->stream));
}
