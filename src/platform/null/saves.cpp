// Saves: the memory cards as directories of the host's file system under the SDL_GetPrefPath-
// equivalent ($XDG_DATA_HOME/twinsanity, ~/.local/share/twinsanity when unset), one subdirectory per port and slot,
// each save a directory named as on the card ("<region><product><save>") with the game's file names verbatim inside
// (icon.sys among them). The host's I/O is synchronous, so an operation only queues and the next Update runs it.
#include "platform/saves.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace
{
using Platform::Saves::FileEntry;
using Platform::Saves::Operation;
using Platform::Saves::Result;
using Platform::Saves::StorageInfo;

// A memory card cluster is a kilobyte: a directory takes two of them and one more for every two entries
constexpr u32 ClusterSize = 0x400;
constexpr u32 ClusterShift = 10;
constexpr s32 DirectoryClusters = 2;
// The longest save and file names a write takes are a character shorter, as on the PS2
constexpr u32 SaveNameLimit = 10;
constexpr u32 FileNameLimit = 0x21;
constexpr s32 ResultCount = 12;
constexpr u32 PathLength = 1024;

char g_Region[8];
char g_Product[16];
Operation g_Operation = Operation::None;
Result g_Results[ResultCount];
StorageInfo g_Info = {Platform::Saves::StorageMemoryCard, 0, 1};

// What the queued operation was asked for (the write's data points at the game's buffer, which outlives the queueing)
struct Context
{
    s32 port;
    s32 slot;
    FileEntry* entry;
    s32* kilobytes;
    s32* size;
    const void* data;
    void* buffer;
    s32 dataSize;
    char save[32];
    char file[32];
};
Context g_Context;

void ForgetResults()
{
    for (Result& result : g_Results)
    {
        result = {-1, -1, 0};
    }
}

// The operation ends, how it went kept
void Finish(s32 succeeded, s32 step, s32 error)
{
    g_Results[static_cast<s32>(g_Operation)] = {succeeded, step, error};
    g_Operation = Operation::None;
}

// The base directory all cards live under ($XDG_DATA_HOME/twinsanity, ~/.local/share/twinsanity without it)
void BasePath(char* path)
{
    const char* dataHome = ::getenv("XDG_DATA_HOME");
    if (dataHome != nullptr && dataHome[0] != '\0')
    {
        ::snprintf(path, PathLength, "%s/twinsanity", dataHome);
        return;
    }

    const char* home = ::getenv("HOME");
    if (home != nullptr && home[0] != '\0')
    {
        ::snprintf(path, PathLength, "%s/.local/share/twinsanity", home);
        return;
    }

    ::snprintf(path, PathLength, "/tmp/twinsanity");
}

void CardPath(char* path, s32 port, s32 slot)
{
    char base[PathLength];
    BasePath(base);
    ::snprintf(path, PathLength, "%s/card%d_%d", base, port, slot);
}

// A save's directory, as on the card
void SavePath(char* path, s32 port, s32 slot, const char* save)
{
    char card[PathLength];
    CardPath(card, port, slot);
    ::snprintf(path, PathLength, "%s/%s%s%s", card, g_Region, g_Product, save);
}

void FilePath(char* path, s32 port, s32 slot, const char* save, const char* file)
{
    char directory[PathLength];
    SavePath(directory, port, slot, save);
    ::snprintf(path, PathLength, "%s/%s", directory, file);
}

// A save or file name the game gives: verbatim, but never a path of its own. An empty name is fine: the game's own save is
// named "" (savemanager.cpp's g_SaveName), its directory the region and product codes alone
bool ValidName(const char* name)
{
    if (::strlen(name) >= 32)
    {
        return false;
    }
    if (::strcmp(name, ".") == 0 || ::strcmp(name, "..") == 0)
    {
        return false;
    }
    return ::strchr(name, '/') == nullptr && ::strchr(name, '\\') == nullptr;
}

bool MakeDirectory(const char* path)
{
    if (::mkdir(path, 0777) == 0 || errno == EEXIST)
    {
        return true;
    }
    return false;
}

// Makes every directory path needs that isn't there yet (its own name left for MakeDirectory): a fresh host has neither the
// base nor its parents
bool MakeDirectories(char* path)
{
    for (char* at = path + 1; *at != '\0'; at++)
    {
        if (*at != '/')
        {
            continue;
        }

        *at = '\0';
        bool made = MakeDirectory(path);
        *at = '/';
        if (!made)
        {
            return false;
        }
    }

    return true;
}

// The card's free space in kilobytes (the base's filesystem, the root's when nothing's there yet)
s32 FreeKilobytes()
{
    char base[PathLength];
    BasePath(base);
    struct statvfs status;
    if (::statvfs(base, &status) != 0 && ::statvfs("/", &status) != 0)
    {
        return 0;
    }
    u64 freeBytes = static_cast<u64>(status.f_bavail) * static_cast<u64>(status.f_frsize);
    u64 freeKilobytes = freeBytes >> ClusterShift;
    return freeKilobytes > static_cast<u64>(0x7FFFFFFF) ? 0x7FFFFFFF : static_cast<s32>(freeKilobytes);
}

// A file entry's times from the host's: the reserved byte cleared, then second, minute, hour, day, month and the year in 16
// bits, as libmc lays them out
void EntryTimes(u8* stamp, const struct tm& broken)
{
    stamp[0] = 0;
    stamp[1] = static_cast<u8>(broken.tm_sec);
    stamp[2] = static_cast<u8>(broken.tm_min);
    stamp[3] = static_cast<u8>(broken.tm_hour);
    stamp[4] = static_cast<u8>(broken.tm_mday);
    stamp[5] = static_cast<u8>(broken.tm_mon + 1);
    stamp[6] = static_cast<u8>((broken.tm_year + 1900) & 0xFF);
    stamp[7] = static_cast<u8>(((broken.tm_year + 1900) >> 8) & 0xFF);
}

void FillEntry(FileEntry* entry, const char* name, const struct stat& status)
{
    struct tm broken;
    ::localtime_r(&status.st_mtime, &broken);
    EntryTimes(entry->created, broken);
    ::localtime_r(&status.st_mtime, &broken);
    EntryTimes(entry->modified, broken);
    entry->size = static_cast<u32>(status.st_size);
    entry->attributes = 0;
    entry->reserved0 = 0;
    entry->reserved1[0] = 0;
    entry->reserved1[1] = 0;
    ::snprintf(entry->name, sizeof(entry->name), "%s", name);
}

void FormatStep()
{
    char base[PathLength];
    BasePath(base);
    char card[PathLength];
    CardPath(card, g_Context.port, g_Context.slot);
    bool made = MakeDirectories(base) && MakeDirectory(base) && MakeDirectory(card);
    Finish(made ? 1 : 0, made ? 1 : 0, made ? 0 : errno);
}

void CreateSaveStep()
{
    if (!ValidName(g_Context.save))
    {
        Finish(0, 0, ENOENT);
        return;
    }
    if (FreeKilobytes() < DirectoryClusters)
    {
        Finish(0, 0, Platform::Saves::ErrorNotEnoughSpace);
        return;
    }
    // The card root only exists after a format, and fresh hosts never run one (the storage reports formatted): make the
    // parents here so the save directory's mkdir has somewhere to land
    char base[PathLength];
    BasePath(base);
    char card[PathLength];
    CardPath(card, g_Context.port, g_Context.slot);
    char path[PathLength];
    SavePath(path, g_Context.port, g_Context.slot, g_Context.save);
    bool made = MakeDirectories(base) && MakeDirectory(base) && MakeDirectory(card) && MakeDirectory(path);
    Finish(made ? 1 : 0, made ? 1 : 0, made ? 0 : errno);
}

// The save's files in whole kilobytes, half a kilobyte an entry ("." and ".." among them) and two for the directory. No such
// save measures -1, and the operation succeeds
void MeasureSaveStep()
{
    if (!ValidName(g_Context.save))
    {
        Finish(0, 0, ENOENT);
        return;
    }
    char path[PathLength];
    SavePath(path, g_Context.port, g_Context.slot, g_Context.save);
    DIR* directory = ::opendir(path);
    if (directory == nullptr)
    {
        *g_Context.kilobytes = -1;
        Finish(1, 1, 0);
        return;
    }
    s32 kilobytes = 0;
    s32 entries = 2;
    while (dirent* found = ::readdir(directory))
    {
        if (::strcmp(found->d_name, ".") == 0 || ::strcmp(found->d_name, "..") == 0)
        {
            continue;
        }
        char child[PathLength];
        ::snprintf(child, PathLength, "%s/%s", path, found->d_name);
        struct stat status;
        if (::stat(child, &status) == 0)
        {
            kilobytes += static_cast<s32>((static_cast<u32>(status.st_size) + ClusterSize - 1) >> ClusterShift);
        }
        entries++;
    }
    ::closedir(directory);
    *g_Context.kilobytes = kilobytes + (entries - 1) / 2 + DirectoryClusters;
    Finish(1, 1, 0);
}

// The file's size (-1 when it isn't there) and its directory entry
void FindFileStep()
{
    if (!ValidName(g_Context.save) || !ValidName(g_Context.file))
    {
        Finish(0, 0, ENOENT);
        return;
    }
    char path[PathLength];
    SavePath(path, g_Context.port, g_Context.slot, g_Context.save);
    DIR* directory = ::opendir(path);
    if (directory == nullptr)
    {
        Finish(0, 1, ENOENT);
        return;
    }
    bool found = false;
    while (dirent* listed = ::readdir(directory))
    {
        if (::strcmp(listed->d_name, g_Context.file) != 0)
        {
            continue;
        }
        char child[PathLength];
        ::snprintf(child, PathLength, "%s/%s", path, listed->d_name);
        struct stat status;
        if (::stat(child, &status) == 0)
        {
            if (g_Context.size != nullptr)
            {
                *g_Context.size = static_cast<s32>(status.st_size);
            }
            if (g_Context.entry != nullptr)
            {
                FillEntry(g_Context.entry, listed->d_name, status);
            }
            found = true;
        }
        break;
    }
    ::closedir(directory);
    if (!found && g_Context.size != nullptr)
    {
        *g_Context.size = -1;
    }
    Finish(1, 1, 0);
}

// Into the save's directory, which has to be there; the file is made when it isn't
void WriteStep()
{
    if (::strlen(g_Context.save) >= SaveNameLimit || ::strlen(g_Context.file) >= FileNameLimit ||
        !ValidName(g_Context.save) || !ValidName(g_Context.file))
    {
        Finish(0, 0, ENAMETOOLONG);
        return;
    }
    char directory[PathLength];
    SavePath(directory, g_Context.port, g_Context.slot, g_Context.save);
    DIR* save = ::opendir(directory);
    if (save == nullptr)
    {
        Finish(0, 0, Platform::Saves::ErrorNoSave);
        return;
    }
    ::closedir(save);
    // The card's free space bounds the write, as the PS2's would (the host's space is on statvfs)
    if (g_Context.dataSize > 0 &&
        static_cast<s32>(Platform::Saves::FileKilobytes(static_cast<u32>(g_Context.dataSize))) > FreeKilobytes())
    {
        Finish(0, 0, Platform::Saves::ErrorNotEnoughSpace);
        return;
    }
    char path[PathLength];
    FilePath(path, g_Context.port, g_Context.slot, g_Context.save, g_Context.file);
    s32 file = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (file < 0)
    {
        Finish(0, 0, errno == ENOSPC ? Platform::Saves::ErrorNotEnoughSpace : Platform::Saves::ErrorNoSave);
        return;
    }
    const u8* left = static_cast<const u8*>(g_Context.data);
    s32 remaining = g_Context.dataSize;
    while (remaining > 0)
    {
        ssize_t written = ::write(file, left, static_cast<size_t>(remaining));
        if (written <= 0)
        {
            s32 error = errno == ENOSPC ? Platform::Saves::ErrorNotEnoughSpace : Platform::Saves::ErrorNoSave;
            ::ftruncate(file, 0);
            ::close(file);
            ::unlink(path);
            Finish(0, 0, error);
            return;
        }
        left += written;
        remaining -= static_cast<s32>(written);
    }
    ::fsync(file);
    ::close(file);
    Finish(1, 1, 0);
}

void ReadStep()
{
    if (!ValidName(g_Context.save) || !ValidName(g_Context.file))
    {
        Finish(0, 0, Platform::Saves::ErrorNoSave);
        return;
    }
    char path[PathLength];
    FilePath(path, g_Context.port, g_Context.slot, g_Context.save, g_Context.file);
    s32 file = ::open(path, O_RDONLY);
    if (file < 0)
    {
        Finish(0, 0, Platform::Saves::ErrorNoSave);
        return;
    }
    u8* left = static_cast<u8*>(g_Context.buffer);
    s32 remaining = g_Context.dataSize;
    while (remaining > 0)
    {
        ssize_t read = ::read(file, left, static_cast<size_t>(remaining));
        if (read <= 0)
        {
            ::close(file);
            Finish(0, 1, Platform::Saves::ErrorNoSave);
            return;
        }
        left += read;
        remaining -= static_cast<s32>(read);
    }
    ::close(file);
    Finish(1, 1, 0);
}

// The queued operation's synchronous work, run to completion
void RunQueued()
{
    switch (g_Operation)
    {
    case Operation::Format:
        FormatStep();
        break;
    case Operation::CreateSave:
        CreateSaveStep();
        break;
    case Operation::MeasureSave:
        MeasureSaveStep();
        break;
    case Operation::FindFile:
        FindFileStep();
        break;
    case Operation::Read:
        ReadStep();
        break;
    case Operation::Write:
        WriteStep();
        break;
    default:
        break;
    }
}

bool Start(Operation operation, s32 port, s32 slot)
{
    if (g_Operation != Operation::None)
    {
        return false;
    }
    ForgetResults();
    g_Context.port = port;
    g_Context.slot = slot;
    g_Operation = operation;
    return true;
}
}

bool Platform::Saves::Initialise(const char* region, const char* product)
{
    ::snprintf(g_Region, sizeof(g_Region), "%s", region);
    ::snprintf(g_Product, sizeof(g_Product), "%s", product);
    ForgetResults();
    return true;
}

Platform::Saves::Operation Platform::Saves::Update(s32 port, s32 slot, StorageInfo* info)
{
    (void)port;
    (void)slot;
    if (g_Operation != Operation::None)
    {
        RunQueued();
    }
    g_Info.freeKilobytes = FreeKilobytes();
    g_Info.formatted = 1;
    if (info != nullptr)
    {
        *info = g_Info;
    }
    return g_Operation;
}

Platform::Saves::Result Platform::Saves::GetResult(Operation operation)
{
    s32 index = static_cast<s32>(operation);
    if (index < 0 || index >= ResultCount)
    {
        return {-1, 0, 0};
    }
    return g_Results[index];
}

bool Platform::Saves::Format(s32 port, s32 slot)
{
    return Start(Operation::Format, port, slot);
}

bool Platform::Saves::CreateSave(s32 port, s32 slot, const char* save)
{
    bool started = Start(Operation::CreateSave, port, slot);
    ::snprintf(g_Context.save, sizeof(g_Context.save), "%s", save);
    return started;
}

bool Platform::Saves::MeasureSave(s32 port, s32 slot, const char* save, s32* kilobytes)
{
    bool started = Start(Operation::MeasureSave, port, slot);
    g_Context.kilobytes = kilobytes;
    ::snprintf(g_Context.save, sizeof(g_Context.save), "%s", save);
    return started;
}

bool Platform::Saves::FindFile(s32 port, s32 slot, const char* file, const char* save, s32* size, FileEntry* entry)
{
    bool started = Start(Operation::FindFile, port, slot);
    g_Context.entry = entry;
    g_Context.size = size;
    ::snprintf(g_Context.save, sizeof(g_Context.save), "%s", save);
    ::snprintf(g_Context.file, sizeof(g_Context.file), "%s", file);
    return started;
}

bool Platform::Saves::Write(s32 port, s32 slot, const char* save, const char* file, const void* data, s32 size)
{
    bool started = Start(Operation::Write, port, slot);
    g_Context.data = data;
    g_Context.dataSize = size;
    ::snprintf(g_Context.save, sizeof(g_Context.save), "%s", save);
    ::snprintf(g_Context.file, sizeof(g_Context.file), "%s", file);
    return started;
}

bool Platform::Saves::Read(s32 port, s32 slot, const char* save, const char* file, void* buffer, s32 size)
{
    bool started = Start(Operation::Read, port, slot);
    g_Context.buffer = buffer;
    g_Context.dataSize = size;
    ::snprintf(g_Context.save, sizeof(g_Context.save), "%s", save);
    ::snprintf(g_Context.file, sizeof(g_Context.file), "%s", file);
    return started;
}

u32 Platform::Saves::FileKilobytes(u32 size)
{
    return (size + ClusterSize - 1) >> ClusterShift;
}

u32 Platform::Saves::SaveBytes(u32 kilobytes, u32 files)
{
    return (kilobytes + (files + 1) / 2 + DirectoryClusters) << ClusterShift;
}
