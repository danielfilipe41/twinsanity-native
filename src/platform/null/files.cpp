// Files: the host's file system under the working directory, in standard C++ (std::filesystem for the paths, std::FILE for
// the handles) so every host platform uses the same file. The game names paths with either slash in any case; the disc's
// files are uppercase while the game asks lowercase, so a miss falls back to a case-insensitive walk of the components.
//
// The root is either the folder the disc was extracted into (the usual case) or a disc image, which the player may point the
// port at instead (Platform::GameData): the image's own file system answers every path then, through the reader in
// iso9660.h, and the volume is read-only.
//
// A disc folder's files run to hundreds of megabytes, and the 32-bit build's off_t is 32 bits unless it is told otherwise:
// the calls below carry 64-bit offsets the way iso9660.cpp's do. It has to be set before any header of this file is read
#define _FILE_OFFSET_BITS 64

#include "platform/files.h"

#include "iso9660.h"
#include "platform/mods.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
// A file call with a 64-bit offset on every host (a 32-bit `long` would wrap a large disc file on Windows)
#if defined(_WIN32)
#define HOST_SEEK(file, offset, whence) _fseeki64((file), static_cast<__int64>(offset), (whence))
#define HOST_TELL(file) _ftelli64(file)
#else
#define HOST_SEEK(file, offset, whence) fseeko((file), static_cast<off_t>(offset), (whence))
#define HOST_TELL(file) ftello(file)
#endif

// A host file a composed disc file reads from (a mod's entry of the archive, the disc's own data file), kept open for it
struct ComposedSource
{
    std::FILE* stream = nullptr;
    std::mutex mutex;

    ~ComposedSource()
    {
        if (stream != nullptr)
        {
            std::fclose(stream);
        }
    }
};

// A piece of a composed disc file: bytes of the disc image, of a host file, or of memory
struct ComposedPiece
{
    u64 start = 0;
    u64 length = 0;
    bool image = false;
    u64 imageOffset = 0;
    std::shared_ptr<ComposedSource> source;
    u64 sourceOffset = 0;

    u64 End() const
    {
        return start + length;
    }
};

// A disc file as the mods make it: the disc's own bytes with the mods' entries appended after them (CRASH6/CRASH.BD)
struct Composed
{
    std::vector<ComposedPiece> pieces;
    u64 size = 0;
};

// The piece holding an offset (the pieces are in order and do not overlap, so this is a binary search)
const ComposedPiece* ComposedPieceAt(const Composed& composed, u64 offset)
{
    size_t low = 0;
    size_t high = composed.pieces.size();
    while (low < high)
    {
        const size_t middle = (low + high) / 2;
        if (offset < composed.pieces[middle].start)
        {
            high = middle;
        }
        else if (offset >= composed.pieces[middle].End())
        {
            low = middle + 1;
        }
        else
        {
            return &composed.pieces[middle];
        }
    }
    return nullptr;
}

// A read of a composed file: the pieces' bytes, in order, up to its end
s32 ReadComposed(const std::shared_ptr<const Composed>& composed, u64 offset, void* buffer, s32 size)
{
    if (composed == nullptr || offset >= composed->size)
    {
        return 0;
    }

    const u64 left = composed->size - offset;
    const s32 wanted = left < static_cast<u64>(size) ? static_cast<s32>(left) : size;
    u8* destination = static_cast<u8*>(buffer);
    s32 done = 0;
    while (done < wanted)
    {
        const u64 at = offset + static_cast<u64>(done);
        const ComposedPiece* piece = ComposedPieceAt(*composed, at);
        if (piece == nullptr)
        {
            break;
        }

        const u64 within = at - piece->start;
        const u64 available = piece->length - within;
        const s32 chunk = available < static_cast<u64>(wanted - done) ? static_cast<s32>(available) : wanted - done;
        s32 read = -1;
        if (piece->image)
        {
            read = Platform::Iso9660::ReadAt(piece->imageOffset + within, destination + done, chunk);
        }
        else if (piece->source != nullptr)
        {
            // The channels read in the background: a handle's read position is its own, never shared
            std::lock_guard<std::mutex> lock(piece->source->mutex);
            if (HOST_SEEK(piece->source->stream, piece->sourceOffset + within, SEEK_SET) == 0)
            {
                read = static_cast<s32>(std::fread(destination + done, 1, static_cast<size_t>(chunk), piece->source->stream));
            }
        }
        if (read <= 0)
        {
            break;
        }
        done += read;
    }
    return done;
}

// A handle's slot: its stream and the lock its operations take (the stream's read position is shared state; the stream
// channels read through ReadAt, which doesn't move it)
struct HostFile
{
    std::FILE* stream = nullptr;

    // The image's share of a handle, when the root is a disc image: the file's bytes at imageOffset, imageSize long, with the
    // position the game seeks to kept here instead of in a FILE
    bool image = false;
    u64 imageOffset = 0;
    u32 imageSize = 0;
    u64 imagePosition = 0;

    // A handle served from memory: the disc's archive index with its build's names put back to the game's (see Open)
    std::shared_ptr<const std::vector<u8>> memory;
    u64 memoryPosition = 0;

    // A handle served from the pieces of a composed disc file: the archive's data with the mods' entries appended (see Open)
    bool composed = false;
    std::shared_ptr<const Composed> composition;
    u64 composedPosition = 0;

    std::mutex mutex;
};

// The handles, by the numbers the game is given. A closed handle's slot is null and reused by the next Open
std::mutex g_TableMutex;
std::vector<std::unique_ptr<HostFile>> g_Files;

// The disc's folder when the game wasn't started in it (Platform::GameData::Locate), empty for the working directory
std::string g_Root;

// Whether the root is a disc image rather than a folder: every path is then looked up in the volume (iso9660.h) and nothing
// is written to it
bool g_ImageRoot = false;

// The archive as the mods make it (platform/mods.h): the rebuilt CRASH6/CRASH.BH and the composed CRASH6/CRASH.BD, made
// once when the game opens the archive, and null when no mod touches it
std::mutex g_ArchiveMutex;
bool g_ArchiveKnown = false;
std::shared_ptr<const std::vector<u8>> g_ArchiveIndex;
std::shared_ptr<const Composed> g_ArchiveData;

// What a disc's build files differently from the game's own names. The game's C++ and its data table ask for the European
// release's names; the North American one calls its single language "American" where the game says "English", and set its font
// in "Crash" where the game asks for "Crash_Euro". This is the table TwinsanityEvolved's SLUS-20909 region map carries, whose
// names were read out of both releases' data images: nothing else of the two discs' naming differs
struct NameAlias
{
    const char* game;
    const char* disc;
};

// A name every occurrence of which differs is the language: a folder of its pictures, the stem of its texts, its voice bank. A
// whole name is the font its lettering was set in
constexpr NameAlias NorthAmericanNames[] = {
    {"English", "American"},
    {"StartUp\\Fonts\\Crash_Euro", "StartUp\\Fonts\\Crash"},
};
constexpr u32 NorthAmericanNameCount = sizeof(NorthAmericanNames) / sizeof(NorthAmericanNames[0]);

constexpr NameAlias JapaneseNames[] = {
    {"English", "Japanese"},
    {"StartUp\\Fonts\\Crash_Euro", "StartUp\\Fonts\\Crash_Jpn"},
};
constexpr u32 JapaneseNameCount = sizeof(JapaneseNames) / sizeof(JapaneseNames[0]);

// The disc's build, by the serial at its root (the program's name on the disc, not the file's name on the host)
enum class DiscBuild
{
    European,
    NorthAmerican,
    Japanese,
};

DiscBuild g_Build = DiscBuild::European;
bool g_BuildKnown = false;
// The serial the build was read from ("" when the disc's root has none this port knows)
const char* g_Serial = "";

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

std::string LowerString(const std::string& text)
{
    std::string lower = text;
    for (char& letter : lower)
    {
        if (letter >= 'A' && letter <= 'Z')
        {
            letter = static_cast<char>(letter - 'A' + 'a');
        }
    }
    return lower;
}

// A folder's entries by their lower case names, read once: the disc is read-only, so its listing cannot change under the
// walk, and the game opens hundreds of its files (every one of them a miss in the file system's own lookup, since the disc
// is upper case and the game asks in lower). Each folder was listed again per open before this
std::mutex g_NamesMutex;
std::unordered_map<std::string, std::map<std::string, std::string>> g_DirectoryNames;
// A host path the walk has resolved, by the path asked for: another lookup of the same name needs no walk at all
std::unordered_map<std::string, std::string> g_ResolvedPaths;

const std::map<std::string, std::string>& DirectoryNames(const std::string& directory)
{
    std::lock_guard<std::mutex> lock(g_NamesMutex);
    const auto found = g_DirectoryNames.find(directory);
    if (found != g_DirectoryNames.end())
    {
        return found->second;
    }

    std::map<std::string, std::string> names;
    std::error_code error;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory, error))
    {
        const std::string name = entry.path().filename().string();
        names.emplace(LowerString(name), name);
    }

    return g_DirectoryNames.emplace(directory, std::move(names)).first->second;
}

// The path with each component resolved case-insensitively, or empty when one has no match: a component is taken as it
// is when the file system has it (a case-insensitive one does, so the walk only runs where it's needed)
std::string ResolveInsensitiveWalk(const std::string& path)
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

        const std::filesystem::path directory = resolved.empty() ? std::filesystem::path(".") : resolved;
        const std::map<std::string, std::string>& names = DirectoryNames(directory.string());
        const auto found = names.find(LowerString(part.string()));
        if (found == names.end())
        {
            return {};
        }

        resolved = directory / found->second;
    }
    return resolved.string();
}

// The walk, remembered. Only the disc's own tree is remembered as missing: the saves' folders are written while the game
// runs, and a name that is not there yet has to be looked for again
std::string ResolveInsensitive(const std::string& path)
{
    {
        std::lock_guard<std::mutex> lock(g_NamesMutex);
        const auto found = g_ResolvedPaths.find(path);
        if (found != g_ResolvedPaths.end())
        {
            return found->second;
        }
    }

    const std::string resolved = ResolveInsensitiveWalk(path);
    const bool discPath = !g_Root.empty() && path.compare(0, g_Root.size(), g_Root) == 0 &&
                          (path.size() == g_Root.size() || path[g_Root.size()] == '/');
    if (!resolved.empty() || discPath)
    {
        std::lock_guard<std::mutex> lock(g_NamesMutex);
        g_ResolvedPaths[path] = resolved;
    }

    return resolved;
}

bool Exists(const std::string& path)
{
    std::error_code error;
    return std::filesystem::exists(path, error);
}

// Whether the disc has a file, by the name on the disc: the image's volume, or the folder's (case-insensitively, as the
// game's opens are)
bool DiscHas(const std::string& gamePath)
{
    if (g_ImageRoot)
    {
        u64 offset = 0;
        u32 size = 0;
        return Platform::Iso9660::Find(gamePath.c_str(), &offset, &size);
    }
    return !ResolveInsensitive(HostPath(gamePath.c_str())).empty();
}

// The disc's build, read once from the serial file at its root
DiscBuild BuildOfDisc()
{
    if (g_BuildKnown)
    {
        return g_Build;
    }

    // Before the probe, so the opens it makes take no aliases of their own
    g_BuildKnown = true;
    if (DiscHas("SLES_525.68"))
    {
        g_Build = DiscBuild::European;
        g_Serial = "SLES_525.68";
    }
    else if (DiscHas("SLUS_209.09"))
    {
        g_Build = DiscBuild::NorthAmerican;
        g_Serial = "SLUS_209.09";
    }
    else if (DiscHas("SLPM_658.01"))
    {
        g_Build = DiscBuild::Japanese;
        g_Serial = "SLPM_658.01";
    }
    return g_Build;
}

const NameAlias* Aliases(u32* count)
{
    switch (BuildOfDisc())
    {
    case DiscBuild::NorthAmerican:
        *count = NorthAmericanNameCount;
        return NorthAmericanNames;
    case DiscBuild::Japanese:
        *count = JapaneseNameCount;
        return JapaneseNames;
    case DiscBuild::European:
        break;
    }
    *count = 0;
    return nullptr;
}

// Whether an alias names a whole path (the font: StartUp\Fonts\Crash) or a bare name every occurrence of which differs (a
// language: English, as a folder of its pictures and in the stem of its texts)
bool IsPathAlias(const NameAlias& alias)
{
    return std::strchr(alias.game, '\\') != nullptr || std::strchr(alias.game, '/') != nullptr;
}

// The path as the other build names it, or empty when it has no name either build files differently. A whole-path alias matches
// the path with its extension kept; a bare name matches a component, whole or as a file's stem
std::string RenamedPath(const std::string& path, bool toDisc)
{
    u32 count = 0;
    const NameAlias* aliases = Aliases(&count);
    if (aliases == nullptr)
    {
        return {};
    }

    auto withExtension = [&path](const std::string& name) -> const char* {
        // The part of the path past a name that is its stem: empty, or ".psf" and the like
        if (path.size() < name.size() || !MatchesLower(path.substr(0, name.size()), name))
        {
            return nullptr;
        }
        if (path.size() == name.size())
        {
            return path.c_str() + name.size();
        }
        return path[name.size()] == '.' ? path.c_str() + name.size() : nullptr;
    };

    for (u32 index = 0; index < count; index++)
    {
        const NameAlias& alias = aliases[index];
        if (!IsPathAlias(alias))
        {
            continue;
        }
        const std::string from = toDisc ? alias.game : alias.disc;
        const std::string to = toDisc ? alias.disc : alias.game;
        if (const char* extension = withExtension(from))
        {
            return to + extension;
        }
    }

    // The bare names: a component the build spells its own way, whole or as a file's stem (American.txt -> English.txt)
    std::string out;
    bool changed = false;
    size_t at = 0;
    while (at <= path.size())
    {
        const size_t end = path.find_first_of("\\/", at);
        const std::string component = path.substr(at, end == std::string::npos ? std::string::npos : end - at);
        std::string replaced = component;
        for (u32 index = 0; index < count; index++)
        {
            const NameAlias& alias = aliases[index];
            if (IsPathAlias(alias))
            {
                continue;
            }
            const std::string from = toDisc ? alias.game : alias.disc;
            const std::string to = toDisc ? alias.disc : alias.game;
            if (MatchesLower(component, from))
            {
                replaced = to;
            }
            else if (component.size() > from.size() && MatchesLower(component.substr(0, from.size()), from) &&
                     component[from.size()] == '.')
            {
                replaced = to + component.substr(from.size());
            }
            else
            {
                continue;
            }
            changed = true;
            break;
        }
        out += replaced;
        if (end == std::string::npos)
        {
            break;
        }
        out += path[end];
        at = end + 1;
    }
    return changed ? out : std::string();
}

// The disc's archive index (CRASH6\CRASH.BH: a header word, then {length, name, offset, size} an entry) with the build's names
// put back to the game's, so the game finds its font and its texts under the names its C++ and data carry. Only the names
// change: every offset and size stays as the disc has it
std::shared_ptr<const std::vector<u8>> RenamedArchiveIndex(const std::vector<u8>& index)
{
    if (index.size() < 4)
    {
        return nullptr;
    }

    auto read32 = [](const std::vector<u8>& bytes, size_t at) {
        return static_cast<u32>(bytes[at]) | (static_cast<u32>(bytes[at + 1]) << 8) | (static_cast<u32>(bytes[at + 2]) << 16) |
               (static_cast<u32>(bytes[at + 3]) << 24);
    };

    struct Row
    {
        std::string name;
        u32 offset;
        u32 size;
    };
    std::vector<Row> rows;
    size_t at = 4;
    while (at + 4 <= index.size())
    {
        const u32 length = read32(index, at);
        if (length == 0 || length > 1024 || at + 4 + length + 8 > index.size())
        {
            return nullptr;
        }
        Row row;
        row.name.assign(reinterpret_cast<const char*>(index.data() + at + 4), length);
        row.offset = read32(index, at + 4 + length);
        row.size = read32(index, at + 4 + length + 4);
        rows.push_back(row);
        at += 4 + length + 8;
    }
    if (rows.empty())
    {
        return nullptr;
    }

    // The names the disc already has: one the game asks for and the disc carries is left alone
    std::vector<std::string> present;
    for (const Row& row : rows)
    {
        present.push_back(row.name);
    }
    u32 renamedCount = 0;
    for (Row& row : rows)
    {
        const std::string renamed = RenamedPath(row.name, false);
        if (renamed.empty())
        {
            continue;
        }
        bool taken = false;
        for (const std::string& name : present)
        {
            if (MatchesLower(name, renamed))
            {
                taken = true;
                break;
            }
        }
        if (!taken)
        {
            row.name = renamed;
            present.push_back(renamed);
            renamedCount++;
        }
    }

    if (renamedCount != 0)
    {
        // One line for the lot: the disc's build files a handful of names its own way and the game is given its own back
        std::fprintf(stderr, "files: the disc's build files %u of its archive's names differently; the game is given its own\n",
                     renamedCount);
    }

    auto share = std::make_shared<std::vector<u8>>();
    share->reserve(index.size() + 1024);
    for (size_t byte = 0; byte < 4; byte++)
    {
        share->push_back(index[byte]);
    }
    for (const Row& row : rows)
    {
        const u32 length = static_cast<u32>(row.name.size());
        for (u32 byte = 0; byte < 4; byte++)
        {
            share->push_back(static_cast<u8>(length >> (byte * 8)));
        }
        share->insert(share->end(), row.name.begin(), row.name.end());
        for (u32 byte = 0; byte < 4; byte++)
        {
            share->push_back(static_cast<u8>(row.offset >> (byte * 8)));
        }
        for (u32 byte = 0; byte < 4; byte++)
        {
            share->push_back(static_cast<u8>(row.size >> (byte * 8)));
        }
    }
    return share;
}

// A read the disc could not serve, said once per name: a disc of a region this port has no names for fails on all of its files
// at once, and a silent black screen says nothing about why
void NoteMissing(const char* path)
{
    static std::mutex mutex;
    static std::vector<std::string> noted;
    std::lock_guard<std::mutex> lock(mutex);
    for (const std::string& name : noted)
    {
        if (MatchesLower(name, path))
        {
            return;
        }
    }
    if (noted.size() >= 8)
    {
        return;
    }
    noted.emplace_back(path);
    std::fprintf(stderr, "files: the disc has no %s\n", path);
}

// Whether a name is the disc's archive index, whatever its case and separators
bool IsArchiveIndex(const char* path)
{
    if (path == nullptr)
    {
        return false;
    }
    std::string name(path);
    const size_t slash = name.find_last_of("\\/");
    name = slash == std::string::npos ? name : name.substr(slash + 1);
    return MatchesLower(name, "CRASH.BH");
}

// Whether a name is the disc's archive data, whatever its case and separators
bool IsArchiveData(const char* path)
{
    if (path == nullptr)
    {
        return false;
    }
    std::string name(path);
    const size_t slash = name.find_last_of("\\/");
    name = slash == std::string::npos ? name : name.substr(slash + 1);
    return MatchesLower(name, "CRASH.BD");
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
    g_ImageRoot = false;
    g_BuildKnown = false;
    g_Serial = "";
    // The names and paths were the disc the last root pointed at
    {
        std::lock_guard<std::mutex> lock(g_NamesMutex);
        g_ResolvedPaths.clear();
        g_DirectoryNames.clear();
    }
    Platform::Iso9660::Close();
    {
        std::lock_guard<std::mutex> lock(g_ArchiveMutex);
        g_ArchiveKnown = false;
        g_ArchiveIndex = nullptr;
        g_ArchiveData = nullptr;
    }
    if (path == nullptr)
    {
        return;
    }

    std::error_code error;
    if (std::filesystem::is_regular_file(path, error))
    {
        // An image: its volume stands in for the folder, and the game's paths are found inside it
        if (Platform::Iso9660::Open(path))
        {
            g_Root = path;
            g_ImageRoot = true;
            return;
        }
        std::fprintf(stderr, "files: %s is not a disc image, so its files cannot be read\n", path);
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

const char* Platform::Files::Root()
{
    return g_Root.c_str();
}

bool Platform::Files::DiscIsPal()
{
    const DiscBuild build = BuildOfDisc();
    // A disc whose root carries none of the serials is played the way the port was made: the European release's 50 fields
    return g_Serial[0] == '\0' || build == DiscBuild::European;
}

bool Platform::Files::DiscIsJapanese()
{
    return BuildOfDisc() == DiscBuild::Japanese;
}

void Platform::Files::Reset()
{
    // The handles stay open: the game keeps reading from them once the I/O processor is back
}

namespace
{
// A handle in a free slot, or a new one
s32 Install(std::unique_ptr<HostFile> file)
{
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

// The file by exactly the name given (the image's volume or the folder, case-insensitively), an alias not applied
s32 OpenExact(const char* path, s32 flags)
{
    const bool create = (flags & Platform::Files::OpenCreate) != 0;
    const bool writing = (flags & (Platform::Files::OpenWrite | Platform::Files::OpenTruncate)) != 0;

    if (g_ImageRoot)
    {
        // The volume is read-only: the game writes its saves through Platform::Saves, never into the disc
        if (writing || (flags & Platform::Files::OpenAppend) != 0)
        {
            return -1;
        }
        u64 offset = 0;
        u32 size = 0;
        if (!Platform::Iso9660::Find(path, &offset, &size))
        {
            return -1;
        }

        auto file = std::make_unique<HostFile>();
        file->image = true;
        file->imageOffset = offset;
        file->imageSize = size;
        return Install(std::move(file));
    }

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
    return Install(std::move(file));
}

// A whole host file into memory (a mod's rebuilt CRASH6/CRASH.BH)
bool ReadWholeBytes(const std::string& path, std::vector<u8>* bytes)
{
    std::FILE* stream = std::fopen(path.c_str(), "rb");
    if (stream == nullptr)
    {
        return false;
    }
    if (HOST_SEEK(stream, 0, SEEK_END) != 0)
    {
        std::fclose(stream);
        return false;
    }
    const s64 size = HOST_TELL(stream);
    if (size < 0 || static_cast<u64>(size) > (64u << 20) || HOST_SEEK(stream, 0, SEEK_SET) != 0)
    {
        std::fclose(stream);
        return false;
    }
    bytes->resize(static_cast<size_t>(size));
    const bool read = size == 0 || std::fread(bytes->data(), 1, static_cast<size_t>(size), stream) == static_cast<size_t>(size);
    std::fclose(stream);
    return read;
}

u32 Get32(const u8* bytes)
{
    return static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8) | (static_cast<u32>(bytes[2]) << 16) |
           (static_cast<u32>(bytes[3]) << 24);
}

// A host file opened to be read from by a composed file, with its size
std::shared_ptr<ComposedSource> OpenSource(const std::string& path, u64* size)
{
    std::FILE* stream = std::fopen(path.c_str(), "rb");
    if (stream == nullptr)
    {
        return nullptr;
    }
    if (HOST_SEEK(stream, 0, SEEK_END) != 0)
    {
        std::fclose(stream);
        return nullptr;
    }
    const s64 end = HOST_TELL(stream);
    if (end < 0)
    {
        std::fclose(stream);
        return nullptr;
    }
    *size = static_cast<u64>(end);
    auto source = std::make_shared<ComposedSource>();
    source->stream = stream;
    return source;
}

// A handle on a host file by its own path (a mod's), not by a path of the disc's
s32 OpenHostFile(const std::string& host, s32 flags)
{
    std::FILE* stream = std::fopen(host.c_str(), HostMode(flags));
    if (stream == nullptr)
    {
        return -1;
    }
    auto file = std::make_unique<HostFile>();
    file->stream = stream;
    return Install(std::move(file));
}

// The game's name for an archive entry: upper case, '\\' separators, no leading one
std::string ArchiveKey(const std::string& name)
{
    std::string key;
    key.reserve(name.size());
    for (const char letter : name)
    {
        const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
        key += upper == '/' ? '\\' : upper;
    }
    size_t begin = 0;
    while (begin < key.size() && key[begin] == '\\')
    {
        begin++;
    }
    return key.substr(begin);
}

// The archive as the mods make it: the base index (a mod's whole CRASH6/CRASH.BH, or the disc's), the entries the mods'
// archive files replace or add, and the data file those are appended to after the disc's own bytes. False when no mod touches
// the archive (its own files are read then, whole and untouched)
bool ComposeModsArchive()
{
    std::lock_guard<std::mutex> lock(g_ArchiveMutex);
    if (g_ArchiveKnown)
    {
        return g_ArchiveIndex != nullptr;
    }
    g_ArchiveKnown = true;

    const std::vector<Platform::Mods::ArchiveFile>& files = Platform::Mods::ArchiveFiles();
    if (files.empty())
    {
        return false;
    }

    const std::string* modIndex = Platform::Mods::ArchiveIndex();
    const std::string* modData = Platform::Mods::ArchiveData();

    std::vector<u8> index;
    if (modIndex != nullptr)
    {
        if (!ReadWholeBytes(*modIndex, &index))
        {
            std::fprintf(stderr, "mods: %s does not read: the disc's own archive is used\n", modIndex->c_str());
            return false;
        }
    }
    else
    {
        const s32 file = OpenExact("Crash6\\Crash.BH", Platform::Files::OpenRead);
        if (file < 0)
        {
            std::fprintf(stderr, "mods: the disc has no CRASH6/CRASH.BH for its archive entries\n");
            return false;
        }
        u8 chunk[0x10000];
        while (index.size() <= (64u << 20))
        {
            const s32 read = Platform::Files::Read(file, chunk, sizeof(chunk));
            if (read <= 0)
            {
                break;
            }
            index.insert(index.end(), chunk, chunk + read);
        }
        Platform::Files::Close(file);
    }
    if (index.size() < 4)
    {
        std::fprintf(stderr, "mods: the archive index does not read: its entries are not applied\n");
        return false;
    }

    // The data below the mods' entries: a mod's whole CRASH.BD, or the disc's
    auto composed = std::make_shared<Composed>();
    if (modData != nullptr)
    {
        u64 size = 0;
        std::shared_ptr<ComposedSource> source = OpenSource(*modData, &size);
        if (source == nullptr)
        {
            std::fprintf(stderr, "mods: %s does not read: the disc's own archive is used\n", modData->c_str());
            return false;
        }
        ComposedPiece piece;
        piece.length = size;
        piece.source = source;
        composed->pieces.push_back(std::move(piece));
        composed->size = size;
    }
    else if (g_ImageRoot)
    {
        u64 offset = 0;
        u32 size = 0;
        if (!Platform::Iso9660::Find("Crash6\\Crash.BD", &offset, &size))
        {
            std::fprintf(stderr, "mods: the disc image has no CRASH6/CRASH.BD for its archive entries\n");
            return false;
        }
        ComposedPiece piece;
        piece.length = size;
        piece.image = true;
        piece.imageOffset = offset;
        composed->pieces.push_back(std::move(piece));
        composed->size = size;
    }
    else
    {
        const std::string host = ResolveInsensitive(HostPath("Crash6\\Crash.BD"));
        u64 size = 0;
        std::shared_ptr<ComposedSource> source = host.empty() ? nullptr : OpenSource(host, &size);
        if (source == nullptr)
        {
            std::fprintf(stderr, "mods: the disc has no CRASH6/CRASH.BD for its archive entries\n");
            return false;
        }
        ComposedPiece piece;
        piece.length = size;
        piece.source = source;
        composed->pieces.push_back(std::move(piece));
        composed->size = size;
    }
    const u64 baseSize = composed->size;

    // The index's rows
    struct Row
    {
        std::string name;
        u64 offset = 0;
        u64 size = 0;
    };
    const u32 header = Get32(index.data());
    std::vector<Row> rows;
    size_t at = 4;
    while (at + 4 <= index.size())
    {
        const u32 length = Get32(index.data() + at);
        if (length == 0 || length > 0x3FF || at + 4 + length + 8 > index.size())
        {
            std::fprintf(stderr, "mods: the archive index does not read as this game's: its entries are not applied\n");
            return false;
        }
        Row row;
        row.name.assign(reinterpret_cast<const char*>(index.data() + at + 4), length);
        row.offset = Get32(index.data() + at + 4 + length);
        row.size = Get32(index.data() + at + 4 + length + 4);
        rows.push_back(std::move(row));
        at += 4 + length + 8;
    }
    if (rows.empty())
    {
        std::fprintf(stderr, "mods: the archive index has no entries: its entries are not applied\n");
        return false;
    }

    std::map<std::string, size_t> byName;
    for (size_t row = 0; row < rows.size(); row++)
    {
        byName.emplace(ArchiveKey(rows[row].name), row);
    }

    u64 cursor = composed->size;
    u32 replaced = 0;
    u32 added = 0;
    for (const Platform::Mods::ArchiveFile& file : files)
    {
        u64 size = 0;
        std::shared_ptr<ComposedSource> source = OpenSource(file.host, &size);
        if (source == nullptr)
        {
            std::fprintf(stderr, "mods: %s does not read: left out\n", file.host.c_str());
            continue;
        }
        if (size > 0xFFFFFFFFull || cursor + size > 0xFFFFFFFFull)
        {
            std::fprintf(stderr, "mods: %s is larger than the archive can hold: left out\n", file.host.c_str());
            continue;
        }

        const std::string key = ArchiveKey(file.path);
        size_t row = SIZE_MAX;
        if (const auto found = byName.find(key); found != byName.end())
        {
            row = found->second;
        }
        else
        {
            // The build's own name for the entry (a crate made for another region's disc names its texts American.txt where
            // this disc says English.txt): the row it replaces keeps the game's name
            const std::string other = ArchiveKey(RenamedPath(key, false));
            if (!other.empty())
            {
                if (const auto found = byName.find(other); found != byName.end())
                {
                    row = found->second;
                }
            }
        }

        if (row == SIZE_MAX)
        {
            // A file passed by its bare name (a level chunk, a picture, a text bank): the archive's one entry of that name. A
            // name several entries share needs the folders that tell them apart
            const size_t slash = key.rfind('\\');
            const std::string bare = slash == std::string::npos ? key : key.substr(slash + 1);
            size_t matches = 0;
            for (size_t index = 0; index < rows.size(); index++)
            {
                const std::string name = ArchiveKey(rows[index].name);
                const size_t at = name.rfind('\\');
                if ((at == std::string::npos ? name : name.substr(at + 1)) != bare)
                {
                    continue;
                }
                matches++;
                row = index;
            }
            if (matches > 1)
            {
                std::fprintf(stderr,
                             "mods: %s: %zu entries of the archive are named %s: put it in folders named like the one it replaces\n",
                             file.host.c_str(), matches, bare.c_str());
                row = SIZE_MAX;
            }
        }

        if (row != SIZE_MAX)
        {
            rows[row].offset = cursor;
            rows[row].size = size;
            replaced++;
        }
        else
        {
            Row fresh;
            fresh.name = key;
            fresh.offset = cursor;
            fresh.size = size;
            rows.push_back(std::move(fresh));
            byName.emplace(key, rows.size() - 1);
            added++;
        }

        ComposedPiece piece;
        piece.start = cursor;
        piece.length = size;
        piece.source = source;
        composed->pieces.push_back(std::move(piece));
        cursor += size;
        composed->size = cursor;
    }

    if (composed->size > 0xFFFFFFFFull)
    {
        std::fprintf(stderr, "mods: the archive with the mods' entries would be larger than 4 GB: its entries are not applied\n");
        return false;
    }

    // The rebuilt index (the game upper-cases the names and keeps them sorted itself, so the order stays the disc's)
    auto rebuilt = std::make_shared<std::vector<u8>>();
    rebuilt->reserve(index.size() + 4096);
    for (u32 byte = 0; byte < 4; byte++)
    {
        rebuilt->push_back(static_cast<u8>(header >> (byte * 8)));
    }
    for (const Row& row : rows)
    {
        const u32 length = static_cast<u32>(row.name.size());
        for (u32 byte = 0; byte < 4; byte++)
        {
            rebuilt->push_back(static_cast<u8>(length >> (byte * 8)));
        }
        rebuilt->insert(rebuilt->end(), row.name.begin(), row.name.end());
        const u32 offset = static_cast<u32>(row.offset);
        for (u32 byte = 0; byte < 4; byte++)
        {
            rebuilt->push_back(static_cast<u8>(offset >> (byte * 8)));
        }
        const u32 size = static_cast<u32>(row.size);
        for (u32 byte = 0; byte < 4; byte++)
        {
            rebuilt->push_back(static_cast<u8>(size >> (byte * 8)));
        }
    }

    // The build's names put back to the game's, as the disc's own index gets them
    if (std::shared_ptr<const std::vector<u8>> renamed = RenamedArchiveIndex(*rebuilt))
    {
        g_ArchiveIndex = renamed;
    }
    else
    {
        g_ArchiveIndex = rebuilt;
    }
    g_ArchiveData = composed;

    std::fprintf(stderr, "mods: the archive rebuilt: %u entries replaced, %u added; index %zu bytes, data %llu bytes (the disc's %llu)\n",
                 replaced, added, g_ArchiveIndex->size(), static_cast<unsigned long long>(composed->size),
                 static_cast<unsigned long long>(baseSize));
    return true;
}
}

s32 Platform::Files::Open(const char* path, s32 flags)
{
    if (path == nullptr)
    {
        return -1;
    }

    const bool writing = (flags & (OpenWrite | OpenCreate | OpenTruncate | OpenAppend)) != 0;

    // The archive, as the mods make it: a rebuilt CRASH6/CRASH.BH, and a CRASH6/CRASH.BD of the disc's bytes with the mods'
    // entries appended after them
    if (!writing && IsArchiveIndex(path) && ComposeModsArchive())
    {
        auto file = std::make_unique<HostFile>();
        file->memory = g_ArchiveIndex;
        return Install(std::move(file));
    }
    if (!writing && IsArchiveData(path) && ComposeModsArchive())
    {
        auto file = std::make_unique<HostFile>();
        file->composed = true;
        file->composition = g_ArchiveData;
        return Install(std::move(file));
    }

    // A disc file a mod replaces or adds, whole (the disc is never written)
    if (!writing)
    {
        if (const std::string* host = Platform::Mods::File(path))
        {
            const s32 modded = OpenHostFile(*host, flags);
            if (modded >= 0)
            {
                return modded;
            }
            std::fprintf(stderr, "mods: %s does not open: the disc's own %s is read\n", host->c_str(), path);
        }
    }

    // The disc's archive index, when its build names things differently: the game is given the index with its own names, so its
    // C++ and its data find the font and the texts of the language they ask for
    static std::shared_ptr<const std::vector<u8>> archiveIndex;
    if (!writing && IsArchiveIndex(path))
    {
        u32 aliasCount = 0;
        if (Aliases(&aliasCount) != nullptr)
        {
            if (archiveIndex != nullptr)
            {
                auto file = std::make_unique<HostFile>();
                file->memory = archiveIndex;
                return Install(std::move(file));
            }

            // Read it once through the layer that serves it, and keep the renamed copy
            const std::string* moddedIndex = Platform::Mods::File(path);
            const s32 plain = moddedIndex != nullptr ? OpenHostFile(*moddedIndex, OpenRead) : OpenExact(path, OpenRead);
            if (plain < 0)
            {
                return -1;
            }
            std::vector<u8> bytes;
            u8 chunk[4096];
            while (true)
            {
                const s32 read = Read(plain, chunk, sizeof(chunk));
                if (read <= 0)
                {
                    break;
                }
                bytes.insert(bytes.end(), chunk, chunk + read);
            }
            Close(plain);

            archiveIndex = RenamedArchiveIndex(bytes);
            if (archiveIndex != nullptr)
            {
                auto file = std::make_unique<HostFile>();
                file->memory = archiveIndex;
                return Install(std::move(file));
            }
        }
    }

    const s32 file = OpenExact(path, flags);
    if (file >= 0 || writing)
    {
        return file;
    }

    // The build files this name under its own: the game asks for Crash6\English.mb, the disc has Crash6\American.mb
    const std::string renamed = RenamedPath(path, true);
    if (!renamed.empty())
    {
        const s32 renamedFile = OpenExact(renamed.c_str(), flags);
        if (renamedFile >= 0)
        {
            return renamedFile;
        }
    }

    // Neither the name nor the disc's own for it: said here, once per name, since a disc whose names are none of the ones this
    // port serves fails on all of its files at once and the game's own "cannot open" says nothing about why (the README lists this
    // line as how a disc it has no names for is heard from)
    NoteMissing(path);
    return -1;
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
    if (handle->image || handle->memory != nullptr || handle->composed)
    {
        return 0;
    }
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
    if (handle->composed)
    {
        const s32 read = ReadComposed(handle->composition, handle->composedPosition, buffer, size);
        if (read > 0)
        {
            handle->composedPosition += static_cast<u64>(read);
        }
        return read;
    }
    if (handle->memory != nullptr)
    {
        const std::vector<u8>& bytes = *handle->memory;
        if (handle->memoryPosition >= bytes.size())
        {
            return 0;
        }
        const u64 left = bytes.size() - handle->memoryPosition;
        const s32 wanted = left < static_cast<u64>(size) ? static_cast<s32>(left) : size;
        std::memcpy(buffer, bytes.data() + handle->memoryPosition, static_cast<size_t>(wanted));
        handle->memoryPosition += static_cast<u64>(wanted);
        return wanted;
    }
    if (handle->image)
    {
        // At the end of its own file, never past it: the image's next files are not this one's
        if (handle->imagePosition >= handle->imageSize)
        {
            return 0;
        }
        const u64 left = handle->imageSize - handle->imagePosition;
        const s32 wanted = left < static_cast<u64>(size) ? static_cast<s32>(left) : size;
        const s32 read = Platform::Iso9660::ReadAt(handle->imageOffset + handle->imagePosition, buffer, wanted);
        if (read > 0)
        {
            handle->imagePosition += static_cast<u64>(read);
        }
        return read < 0 ? -1 : read;
    }
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
    if (handle->image || handle->memory != nullptr || handle->composed)
    {
        return -1;
    }
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
    if (handle->memory != nullptr)
    {
        const s64 start = whence == SeekSet ? 0 : whence == SeekCurrent ? static_cast<s64>(handle->memoryPosition)
                                                                        : static_cast<s64>(handle->memory->size());
        const s64 position = start + offset;
        if (position < 0)
        {
            return -1;
        }
        handle->memoryPosition = static_cast<u64>(position);
        return static_cast<s32>(handle->memoryPosition);
    }
    if (handle->composed)
    {
        // Past the end is allowed, as the console's is; before the start is not
        const s64 start = whence == SeekSet ? 0 : whence == SeekCurrent ? static_cast<s64>(handle->composedPosition)
                                                                       : static_cast<s64>(handle->composition->size);
        const s64 position = start + offset;
        if (position < 0)
        {
            return -1;
        }
        handle->composedPosition = static_cast<u64>(position);
        return static_cast<s32>(handle->composedPosition);
    }
    if (handle->image)
    {
        // Past the end is allowed, as the console's is; before the start is not
        const s64 start = whence == SeekSet ? 0 : whence == SeekCurrent ? static_cast<s64>(handle->imagePosition)
                                                                       : static_cast<s64>(handle->imageSize);
        const s64 position = start + offset;
        if (position < 0)
        {
            return -1;
        }
        handle->imagePosition = static_cast<u64>(position);
        return static_cast<s32>(handle->imagePosition);
    }
    if (HOST_SEEK(handle->stream, offset, HostWhence(whence)) != 0)
    {
        return -1;
    }
    const s64 position = HOST_TELL(handle->stream);
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
    if (handle->memory != nullptr)
    {
        const std::vector<u8>& bytes = *handle->memory;
        if (offset >= bytes.size())
        {
            return 0;
        }
        const u64 left = bytes.size() - offset;
        const s32 wanted = left < static_cast<u64>(size) ? static_cast<s32>(left) : size;
        std::memcpy(buffer, bytes.data() + offset, static_cast<size_t>(wanted));
        return wanted;
    }
    if (handle->composed)
    {
        // The read position stays where the game left it: the stream channels read in the background
        return ReadComposed(handle->composition, offset, buffer, size);
    }
    if (handle->image)
    {
        // The read position stays where the game left it: the stream channels read in the background
        if (offset >= handle->imageSize)
        {
            return 0;
        }
        const u64 left = handle->imageSize - offset;
        const s32 wanted = left < static_cast<u64>(size) ? static_cast<s32>(left) : size;
        const s32 read = Platform::Iso9660::ReadAt(handle->imageOffset + offset, buffer, wanted);
        return read < 0 ? -1 : read;
    }
    if (HOST_SEEK(handle->stream, offset, SEEK_SET) != 0)
    {
        return -1;
    }
    return static_cast<s32>(std::fread(buffer, 1, static_cast<size_t>(size), handle->stream));
}
