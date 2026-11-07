// The ISO9660 reader: the layout follows the comment in iso9660.h. The image is read with positioned reads of a FILE under a
// lock, 64 bits of offset on every host (a 32-bit `long` would wrap a 2.6 GB disc on Windows), and every path the game asks
// for is resolved by walking the directories the volume records.
// A disc image is over 2 GB, and the host build is 32 bits: without this the C library's file calls carry a 32-bit off_t and
// refuse to open the image at all (EOVERFLOW, "value too large for defined data type"). It has to be set before any header
// of this file is read
#define _FILE_OFFSET_BITS 64

#include "iso9660.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define ISO_SEEK(file, offset) _fseeki64((file), static_cast<__int64>(offset), SEEK_SET)
#else
#define ISO_SEEK(file, offset) fseeko((file), static_cast<off_t>(offset), SEEK_SET)
#endif

namespace
{
constexpr u32 SectorSize = 2048;
constexpr u32 RawSectorSize = 2352;
constexpr u32 PvdSector = 16;
constexpr u32 RootRecordOffset = 156;
constexpr u32 VolumeIdOffset = 40;
constexpr u32 VolumeSpaceOffset = 80;

// The image: its FILE, the lock every read takes, and what its sectors hold
std::mutex g_Mutex;
std::FILE* g_Image = nullptr;
u64 g_Bytes = 0;          // the volume's logical bytes (sectors * 2048)
u32 g_Stride = SectorSize; // 2048 for a plain image, 2352 for a raw dump
u32 g_UserOffset = 0;      // raw: where a sector's 2048 user bytes start
u32 g_Sectors = 0;
char g_VolumeId[33] = {};
u32 g_RootLba = 0;
u32 g_RootSize = 0;

// A directory record's fields, as the volume records them
struct Record
{
    u32 lba = 0;
    u32 size = 0;
    bool directory = false;
    std::string name;
};

// The image's byte count: the file's end, found with 64-bit arithmetic on every host
u64 FileSize(std::FILE* file)
{
#if defined(_WIN32)
    if (_fseeki64(file, 0, SEEK_END) != 0)
    {
        return 0;
    }
    const __int64 end = _ftelli64(file);
#else
    if (fseeko(file, 0, SEEK_END) != 0)
    {
        return 0;
    }
    const off_t end = ftello(file);
#endif
    return end > 0 ? static_cast<u64>(end) : 0;
}

u32 Le32(const u8* bytes)
{
    return static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8) | (static_cast<u32>(bytes[2]) << 16) |
           (static_cast<u32>(bytes[3]) << 24);
}

// One record at bytes (its length is bytes[0], already known to be at least 33)
Record Decode(const u8* bytes)
{
    Record record;
    record.lba = Le32(bytes + 2);
    record.size = Le32(bytes + 10);
    record.directory = (bytes[25] & 0x02) != 0;
    record.name.assign(reinterpret_cast<const char*>(bytes + 33), bytes[32]);
    return record;
}

bool SameUpper(char a, char b)
{
    if (a >= 'a' && a <= 'z')
    {
        a = static_cast<char>(a - 'a' + 'A');
    }
    if (b >= 'a' && b <= 'z')
    {
        b = static_cast<char>(b - 'a' + 'A');
    }
    return a == b;
}

bool MatchesUpper(const std::string& left, const std::string& right)
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (u32 index = 0; index < left.size(); index++)
    {
        if (!SameUpper(left[index], right[index]))
        {
            return false;
        }
    }
    return true;
}

// Whether a recorded name ("CRASH.BH;1") is the component asked for ("crash.bh"): the version suffix is optional, and the
// little the volume keeps beside it ("." and "..") is not a component anyone asks for
bool NameMatches(const std::string& recorded, const std::string& wanted)
{
    if (recorded.size() == 1 && (recorded[0] == '\0' || recorded[0] == '\1'))
    {
        return false;
    }
    const size_t semicolon = recorded.find(';');
    const std::string base = semicolon == std::string::npos ? recorded : recorded.substr(0, semicolon);
    return MatchesUpper(recorded, wanted) || MatchesUpper(base, wanted);
}

// The image's logical bytes: a plain image is the file itself, a raw dump loses its sync headers and ECC to the remap
s32 ReadBytes(u64 offset, void* buffer, s32 size)
{
    u8* destination = static_cast<u8*>(buffer);
    if (g_Image == nullptr || size <= 0 || offset >= g_Bytes)
    {
        return 0;
    }
    const u64 available = g_Bytes - offset;
    const size_t wanted = available < static_cast<u64>(size) ? static_cast<size_t>(available) : static_cast<size_t>(size);

    if (g_Stride == SectorSize)
    {
        size_t done = 0;
        while (done < wanted)
        {
            if (ISO_SEEK(g_Image, offset + done) != 0)
            {
                break;
            }
            const size_t got = std::fread(destination + done, 1, wanted - done, g_Image);
            done += got;
            if (got == 0)
            {
                break;
            }
        }
        return static_cast<s32>(done);
    }

    // A raw dump: the sectors the read spans are taken in runs, the whole run read at once (sync headers and ECC with it) and
    // its user bytes copied out of it. A read was one seek and one read per 2048-byte sector before this, so a chunk of a
    // level cost dozens of system calls a read; the runs are capped so a very large read needs no very large buffer
    constexpr u32 SectorsPerRun = 128;
    static std::vector<u8> run;
    const u64 firstSector = offset / SectorSize;
    const size_t inside = static_cast<size_t>(offset % SectorSize);
    const u64 lastSector = (offset + wanted - 1) / SectorSize;
    size_t done = 0;
    for (u64 sector = firstSector; sector <= lastSector && done < wanted;)
    {
        const u64 left = lastSector - sector + 1;
        const u32 count = left < SectorsPerRun ? static_cast<u32>(left) : SectorsPerRun;
        const size_t bytes = static_cast<size_t>(count) * g_Stride;
        if (run.size() < bytes)
        {
            run.resize(bytes);
        }

        if (ISO_SEEK(g_Image, sector * g_Stride) != 0 || std::fread(run.data(), 1, bytes, g_Image) != bytes)
        {
            break;
        }

        for (u32 index = 0; index < count && done < wanted; index++)
        {
            // The first sector starts inside its user bytes, the ones after it at their first
            const size_t skip = sector + index == firstSector ? inside : 0;
            const size_t room = static_cast<size_t>(SectorSize) - skip;
            const size_t take = wanted - done < room ? wanted - done : room;
            std::memcpy(destination + done, run.data() + static_cast<size_t>(index) * g_Stride + g_UserOffset + skip,
                        take);
            done += take;
        }

        sector += count;
    }
    return static_cast<s32>(done);
}

u32 ReadSectors(u32 lba, u32 count, u8* destination)
{
    const s32 read = ReadBytes(static_cast<u64>(lba) * SectorSize, destination, static_cast<s32>(count * SectorSize));
    return read <= 0 ? 0 : static_cast<u32>(read) / SectorSize;
}

// The folders read so far, by the sector they start at: the volume is read-only, so a listing made once stays right, and a
// path's lookup walked the same folders from the root every time it opened a file
std::unordered_map<u32, std::vector<Record>> g_directories;

// What a path was found to be (or that it was not): a level asks for the same file over and over (its textures, its sounds,
// its chunks), and every ask walked the folders from the root again
struct PathAnswer
{
    bool found = false;
    u64 offset = 0;
    u32 size = 0;
};

std::unordered_map<std::string, PathAnswer> g_paths;

// The directory's records, or an empty list when it does not read whole
std::vector<Record> ListDirectory(u32 lba, u32 size)
{
    std::vector<Record> records;
    if (size == 0)
    {
        return records;
    }
    const auto known = g_directories.find(lba);
    if (known != g_directories.end())
    {
        return known->second;
    }
    const u32 sectors = (size + SectorSize - 1) / SectorSize;
    std::vector<u8> bytes(static_cast<size_t>(sectors) * SectorSize);
    if (ReadSectors(lba, sectors, bytes.data()) != sectors)
    {
        return {};
    }

    size_t position = 0;
    while (position < size)
    {
        const u8 length = bytes[position];
        if (length == 0)
        {
            // The rest of the sector is padding: the next record starts at the next sector
            position = (position / SectorSize + 1) * SectorSize;
            continue;
        }
        if (length < 33 || position + length > bytes.size())
        {
            return {};
        }
        records.push_back(Decode(bytes.data() + position));
        position += length;
    }

    g_directories.emplace(lba, records);
    return records;
}

// The path as components, without the "cdrom0:" prefix and the separators either shape uses
std::vector<std::string> Components(const char* gamePath)
{
    std::string path(gamePath != nullptr ? gamePath : "");
    const size_t colon = path.find(':');
    if (colon != std::string::npos)
    {
        path = path.substr(colon + 1);
    }

    std::vector<std::string> components;
    size_t index = 0;
    while (index < path.size())
    {
        while (index < path.size() && (path[index] == '/' || path[index] == '\\'))
        {
            index++;
        }
        size_t end = index;
        while (end < path.size() && path[end] != '/' && path[end] != '\\')
        {
            end++;
        }
        if (end > index)
        {
            components.push_back(path.substr(index, end - index));
        }
        index = end;
    }
    return components;
}
}

bool Platform::Iso9660::LooksLikeImage(const char* path)
{
    if (path == nullptr || path[0] == '\0')
    {
        return false;
    }
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        return false;
    }

    u8 sector[RawSectorSize] = {};
    const size_t read = std::fread(sector, 1, sizeof(sector), file);

    // A raw dump starts with the 12-byte sync pattern; a plain image has the descriptor where the volume's sixteenth sector is
    static const u8 Sync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    bool isImage = false;
    if (read >= RawSectorSize && std::memcmp(sector, Sync, sizeof(Sync)) == 0)
    {
        isImage = true;
    }
    else if (ISO_SEEK(file, static_cast<u64>(PvdSector) * SectorSize) == 0)
    {
        u8 descriptor[6] = {};
        if (std::fread(descriptor, 1, sizeof(descriptor), file) == sizeof(descriptor))
        {
            isImage = descriptor[0] == 1 && std::memcmp(descriptor + 1, "CD001", 5) == 0;
        }
    }

    std::fclose(file);
    return isImage;
}

bool Platform::Iso9660::Open(const char* imagePath)
{
    Close();
    if (imagePath == nullptr || imagePath[0] == '\0')
    {
        return false;
    }

    std::FILE* file = std::fopen(imagePath, "rb");
    if (file == nullptr)
    {
        std::fprintf(stderr, "iso9660: could not open %s: %s\n", imagePath, std::strerror(errno));
        return false;
    }

    const u64 fileBytes = FileSize(file);
    ISO_SEEK(file, 0);

    u32 stride = SectorSize;
    u32 userOffset = 0;
    u64 bytes = fileBytes;
    {
        static const u8 Sync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
        u8 header[16] = {};
        if (fileBytes >= RawSectorSize && std::fread(header, 1, sizeof(header), file) == sizeof(header) &&
            std::memcmp(header, Sync, sizeof(Sync)) == 0)
        {
            stride = RawSectorSize;
            // Mode 1 keeps its user bytes at 16, Mode 2 Form 1 at 24 (after the 8-byte subheader)
            userOffset = header[15] == 2 ? 24 : 16;
            bytes = (fileBytes / RawSectorSize) * SectorSize;
        }
    }

    u8 descriptor[SectorSize] = {};
    if (ISO_SEEK(file, static_cast<u64>(PvdSector) * stride + userOffset) != 0 ||
        std::fread(descriptor, 1, sizeof(descriptor), file) != sizeof(descriptor) || descriptor[0] != 1 ||
        std::memcmp(descriptor + 1, "CD001", 5) != 0)
    {
        std::fprintf(stderr, "iso9660: %s has no ISO9660 volume descriptor at sector 16\n", imagePath);
        std::fclose(file);
        return false;
    }

    std::lock_guard<std::mutex> lock(g_Mutex);
    g_Image = file;
    g_Bytes = bytes;
    g_Stride = stride;
    g_UserOffset = userOffset;
    g_Sectors = Le32(descriptor + VolumeSpaceOffset);
    std::memcpy(g_VolumeId, descriptor + VolumeIdOffset, 32);
    g_VolumeId[32] = '\0';
    for (u32 index = 32; index > 0 && g_VolumeId[index - 1] == ' '; index--)
    {
        g_VolumeId[index - 1] = '\0';
    }

    if (descriptor[RootRecordOffset] < 33)
    {
        std::fprintf(stderr, "iso9660: %s has a malformed root directory record\n", imagePath);
        Close();
        return false;
    }
    const Record root = Decode(descriptor + RootRecordOffset);
    g_RootLba = root.lba;
    g_RootSize = root.size;
    return true;
}

void Platform::Iso9660::Close()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    if (g_Image != nullptr)
    {
        std::fclose(g_Image);
    }
    g_Image = nullptr;
    g_Bytes = 0;
    g_Stride = SectorSize;
    g_UserOffset = 0;
    g_Sectors = 0;
    g_VolumeId[0] = '\0';
    g_RootLba = 0;
    g_RootSize = 0;
    // The answers were this volume's: another image has its own folders and its own files at other sectors
    g_paths.clear();
    g_directories.clear();
}

bool Platform::Iso9660::IsOpen()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    return g_Image != nullptr;
}

u64 Platform::Iso9660::Bytes()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    return g_Bytes;
}

const char* Platform::Iso9660::VolumeId()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    return g_VolumeId;
}

namespace
{
// The volume's answer for a path's components, walking the folders from the root: the callers cache it
PathAnswer LookUpPath(const std::vector<std::string>& components)
{
    PathAnswer answer;
    u32 directoryLba = g_RootLba;
    u32 directorySize = g_RootSize;
    for (size_t index = 0; index < components.size(); index++)
    {
        const std::vector<Record> records = ListDirectory(directoryLba, directorySize);
        if (records.empty())
        {
            return answer;
        }

        const Record* found = nullptr;
        for (const Record& record : records)
        {
            if (NameMatches(record.name, components[index]))
            {
                found = &record;
                break;
            }
        }
        if (found == nullptr)
        {
            return answer;
        }

        const bool last = index + 1 == components.size();
        if (last)
        {
            if (found->directory)
            {
                return answer;
            }
            answer.found = true;
            answer.offset = static_cast<u64>(found->lba) * SectorSize;
            answer.size = found->size;
            return answer;
        }

        if (!found->directory)
        {
            return answer;
        }
        directoryLba = found->lba;
        directorySize = found->size;
    }
    return answer;
}

// The path in the case the volume records its names in, which is what makes two asks for one file the same ask
std::string UpperPath(const char* gamePath)
{
    std::string key(gamePath != nullptr ? gamePath : "");
    for (char& letter : key)
    {
        if (letter >= 'a' && letter <= 'z')
        {
            letter = static_cast<char>(letter - 'a' + 'A');
        }
    }
    return key;
}
}

bool Platform::Iso9660::Find(const char* gamePath, u64* offset, u32* size)
{
    const std::vector<std::string> components = Components(gamePath);
    if (components.empty())
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_Mutex);
    if (g_Image == nullptr)
    {
        return false;
    }

    const std::string key = UpperPath(gamePath);
    const auto known = g_paths.find(key);
    const PathAnswer& answer = known != g_paths.end() ? known->second : g_paths.emplace(key, LookUpPath(components)).first->second;
    if (!answer.found)
    {
        return false;
    }

    if (offset != nullptr)
    {
        *offset = answer.offset;
    }
    if (size != nullptr)
    {
        *size = answer.size;
    }
    return true;
}

s32 Platform::Iso9660::ReadAt(u64 offset, void* buffer, s32 size)
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    return ReadBytes(offset, buffer, size);
}
