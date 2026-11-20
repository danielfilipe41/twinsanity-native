#define _FILE_OFFSET_BITS 64

// Mods: the community's mods read from one folder, so the game plays them without a launcher (platform/mods.h).
//
// Nothing here touches the disc: a mod's disc files are answered by Platform::Files (whole, read-only), and its archive
// entries become a rebuilt index and a composed data file (files.cpp). This file only finds them and reads what they say
// about themselves.
#include "platform/mods.h"

#include "platform/discpatch.h"
#include "platform/files.h"
#include "platform/saves.h"

#include "iso9660.h"

// The zip reader the community's crates arrive in (third_party/miniz, public domain)
#include "miniz.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;

using Platform::Mods::ArchiveFile;
using Platform::Mods::Mod;

std::string g_Folder;
std::vector<Mod> g_Mods;
// The whole disc files the mods replace or add, by the game's path (upper case, '/' separators)
std::map<std::string, std::string> g_Disc;
// The files of the disc the mods give as ranges rather than whole (a TwinsanityEvolved recipe), by the same path
std::map<std::string, Platform::Mods::Ranges> g_Ranges;
// The archive's entries they replace or add, by the game's name (upper case, '\\' separators)
std::map<std::string, std::string> g_Archive;
std::vector<ArchiveFile> g_ArchiveFiles;
// mods.txt's list as it was read, in the order it applies
std::vector<std::string> g_Listed;
// A mod's whole CRASH6/CRASH.BH and CRASH6/CRASH.BD (a rebuilt archive pair)
std::string g_IndexHost;
std::string g_DataHost;
bool g_Enabled = false;

// What one mod put in (it may replace another's file: the map has the later one)
struct Applied
{
    u32 disc = 0;
    u32 archive = 0;
};

std::string Slashes(const std::string& path)
{
    std::string out = path;
    for (char& letter : out)
    {
        if (letter == '\\')
        {
            letter = '/';
        }
    }
    return out;
}

std::string Lower(std::string text)
{
    for (char& letter : text)
    {
        letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
    }
    return text;
}

std::string Upper(std::string text)
{
    for (char& letter : text)
    {
        letter = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
    }
    return text;
}

std::string Trim(const std::string& text)
{
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0)
    {
        begin++;
    }
    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
    {
        end--;
    }
    return text.substr(begin, end - begin);
}

// The game's path for a file of a mod's disc tree: upper case, '/' separators, no leading or trailing separator
std::string DiscKey(const std::string& relative)
{
    std::string key = Upper(Slashes(relative));
    size_t begin = 0;
    while (begin < key.size() && key[begin] == '/')
    {
        begin++;
    }
    size_t end = key.size();
    while (end > begin && key[end - 1] == '/')
    {
        end--;
    }
    return key.substr(begin, end - begin);
}

// The archive's name for a file of a mod's layer1 (or a loose built file): upper case, '\\' separators, no leading one
std::string ArchiveKey(const std::string& relative)
{
    std::string key = Upper(relative);
    for (char& letter : key)
    {
        if (letter == '/')
        {
            letter = '\\';
        }
    }
    size_t begin = 0;
    while (begin < key.size() && key[begin] == '\\')
    {
        begin++;
    }
    return key.substr(begin);
}

u32 Get32(const u8* bytes)
{
    return static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8) | (static_cast<u32>(bytes[2]) << 16) |
           (static_cast<u32>(bytes[3]) << 24);
}

// Bytes of a host file at an offset (the mod's own copy of an entry)
bool ReadHostRange(const std::string& host, u64 offset, u8* bytes, size_t size)
{
    std::FILE* stream = std::fopen(host.c_str(), "rb");
    if (stream == nullptr)
    {
        return false;
    }
#if defined(_WIN32)
    bool ok = _fseeki64(stream, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    bool ok = fseeko(stream, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
    if (ok)
    {
        ok = std::fread(bytes, 1, size, stream) == size;
    }
    std::fclose(stream);
    return ok;
}

// Names that are never content: the hosts' own droppings, editors' backups, a mod's own notes
bool Clutter(const std::string& name)
{
    const std::string lower = Lower(name);
    return lower == ".ds_store" || lower == "thumbs.db" || lower == "desktop.ini" || lower == "__macosx" ||
           (!lower.empty() && lower.back() == '~');
}

// The files that describe a mod rather than being what it changes
bool Metadata(const std::string& name)
{
    const std::string lower = Lower(name);
    return lower == "modcrateinfo.txt" || lower == "modcrateicon.png" || lower == "modcratesettings.txt" ||
           lower == "mod.json" || lower == "mods.txt" || lower == "enabled.txt" || lower == ".from-zip" ||
           lower.compare(0, 6, "readme") == 0 || lower.compare(0, 7, "license") == 0;
}

// A folder's files, made whole (small text files: manifests, notes, mods.txt)
bool ReadText(const fs::path& path, std::string* text)
{
    std::FILE* file = std::fopen(Slashes(path.string()).c_str(), "rb");
    if (file == nullptr)
    {
        return false;
    }

    text->clear();
    char buffer[8192];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) != 0)
    {
        text->append(buffer, read);
    }

    std::fclose(file);
    return true;
}

// A regular file of `folder` by its lower case name, or an empty path
fs::path FindNamed(const fs::path& folder, const char* name)
{
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(folder, error))
    {
        std::error_code type;
        if (!entry.is_regular_file(type))
        {
            continue;
        }
        if (Lower(entry.path().filename().string()) == name)
        {
            return entry.path();
        }
    }
    return {};
}

// A zipped mod: the community ships a crate as one. It is unpacked once into the folder of the zip's name (the id mods.txt
// lists), with a marker saying which zip was unpacked, so a newer zip is unpacked again. Nothing of the disc is written.
void ExtractZips(const fs::path& folder)
{
    std::vector<fs::path> zips;
    {
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(folder, error))
        {
            std::error_code type;
            if (entry.is_regular_file(type) && Lower(entry.path().extension().string()) == ".zip")
            {
                zips.push_back(entry.path());
            }
        }
    }

    for (const fs::path& zip : zips)
    {
        const fs::path target = folder / zip.stem();
        const fs::path marker = target / ".from-zip";
        const std::string zipName = Slashes(zip.string());
        const std::string targetName = Slashes(target.string());

        // Which zip was unpacked here (its size and when it was written)
        std::error_code error;
        char state[64];
        std::snprintf(state, sizeof(state), "%llu %lld",
                      static_cast<unsigned long long>(fs::file_size(zip, error)),
                      static_cast<long long>(fs::last_write_time(zip, error).time_since_epoch().count()));

        std::string unpacked;
        if (fs::is_regular_file(marker, error) && ReadText(marker, &unpacked) && Trim(unpacked) == state)
        {
            continue;
        }

        // A folder of the same name this build did not make is the player's: it is read as it is
        if (fs::is_directory(target, error) && !fs::is_regular_file(marker, error))
        {
            std::fprintf(stderr, "mods: %s is a zip and %s is a folder already: the folder is read as it is\n",
                         zip.filename().string().c_str(), targetName.c_str());
            continue;
        }

        // This build's own unpacked copy, made again
        if (fs::is_directory(target, error))
        {
            std::error_code cleared;
            fs::remove_all(target, cleared);
            if (cleared)
            {
                std::fprintf(stderr, "mods: %s cannot be unpacked: %s cannot be cleared\n", zip.filename().string().c_str(),
                             targetName.c_str());
                continue;
            }
        }

        mz_zip_archive archive = {};
        if (mz_zip_reader_init_file(&archive, zipName.c_str(), 0) == 0)
        {
            std::fprintf(stderr, "mods: %s does not read as a zip: extract it by hand into %s\n",
                         zip.filename().string().c_str(), targetName.c_str());
            continue;
        }

        const mz_uint files = mz_zip_reader_get_num_files(&archive);
        u32 unpackedFiles = 0;
        bool ok = true;
        for (mz_uint index = 0; index < files && ok; index++)
        {
            if (mz_zip_reader_is_file_a_directory(&archive, index) != 0)
            {
                continue;
            }

            mz_zip_archive_file_stat stat;
            if (mz_zip_reader_file_stat(&archive, index, &stat) == 0)
            {
                ok = false;
                break;
            }

            // Nothing outside the folder (nor the host's own droppings)
            const std::string name = Slashes(stat.m_filename);
            bool safe = !name.empty() && name[0] != '/';
            for (size_t part = 0; safe && part < name.size();)
            {
                const size_t end = name.find('/', part);
                const std::string piece = name.substr(part, end == std::string::npos ? std::string::npos : end - part);
                safe = !piece.empty() && piece != ".." && piece.find(':') == std::string::npos;
                part = end == std::string::npos ? name.size() : end + 1;
            }
            if (!safe || Lower(name).rfind("__macosx/", 0) == 0)
            {
                continue;
            }

            const fs::path out = target / name;
            std::error_code made;
            fs::create_directories(out.parent_path(), made);
            if (mz_zip_reader_extract_to_file(&archive, index, Slashes(out.string()).c_str(), 0) == 0)
            {
                ok = false;
                break;
            }
            unpackedFiles++;
        }
        mz_zip_reader_end(&archive);

        if (!ok)
        {
            std::fprintf(stderr, "mods: %s does not unpack (one of its files is bad): extract it by hand into %s\n",
                         zip.filename().string().c_str(), targetName.c_str());
            std::error_code cleared;
            fs::remove_all(target, cleared);
            continue;
        }

        // The marker: what was unpacked, so the next run has it already
        std::error_code made;
        fs::create_directories(target, made);
        if (std::FILE* file = std::fopen(Slashes(marker.string()).c_str(), "wb"))
        {
            std::fprintf(file, "%s\n", state);
            std::fclose(file);
        }

        std::fprintf(stderr, "mods: %s unpacked into %s (%u files)\n", zip.filename().string().c_str(), targetName.c_str(),
                     unpackedFiles);
    }
}

// A sub-folder of `folder` by its lower case name, or an empty path
fs::path FindFolder(const fs::path& folder, const char* name)
{
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(folder, error))
    {
        std::error_code type;
        if (!entry.is_directory(type))
        {
            continue;
        }
        if (Lower(entry.path().filename().string()) == name)
        {
            return entry.path();
        }
    }
    return {};
}

std::vector<std::string> Subfolders(const fs::path& folder)
{
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(folder, error))
    {
        std::error_code type;
        if (!entry.is_directory(type))
        {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (Clutter(name))
        {
            continue;
        }
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

// Every file of a tree, as paths relative to it ('/' separators)
std::vector<std::string> FilesRecursive(const fs::path& tree)
{
    std::vector<std::string> files;
    std::error_code error;
    fs::recursive_directory_iterator at(tree, error);
    const fs::recursive_directory_iterator end;
    for (; at != end; at.increment(error))
    {
        if (error)
        {
            break;
        }
        std::error_code type;
        if (at->is_symlink(type))
        {
            at.disable_recursion_pending();
            continue;
        }
        const std::string name = at->path().filename().string();
        if (Clutter(name))
        {
            if (at->is_directory(type))
            {
                at.disable_recursion_pending();
            }
            continue;
        }
        if (!at->is_regular_file(type))
        {
            continue;
        }
        files.push_back(Slashes(at->path().lexically_relative(tree).string()));
    }
    std::sort(files.begin(), files.end());
    return files;
}

// Whether a folder has a regular file of its own (a build folder's output is all in its sub-folders)
bool HasFiles(const fs::path& folder)
{
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(folder, error))
    {
        std::error_code type;
        if (entry.is_regular_file(type) && !Metadata(entry.path().filename().string()))
        {
            return true;
        }
    }
    return false;
}

bool DiscName(const std::string& first);
bool ArchiveName(const std::string& first);

// A build's output nested in folders of its own (build/archives/ps2): the folder that holds the files
fs::path PayloadRoot(const fs::path& folder)
{
    fs::path at = folder;
    for (u32 depth = 0; depth < 3; depth++)
    {
        if (HasFiles(at))
        {
            break;
        }
        const std::vector<std::string> subs = Subfolders(at);
        if (subs.size() != 1)
        {
            break;
        }
        // The game's own folders are the payload, not a wrapper around it (a mod folder holding only CRASH6/)
        const std::string name = Upper(subs[0]);
        if (DiscName(name) || ArchiveName(name))
        {
            break;
        }
        at = at / subs[0];
    }
    return at;
}

// The crate's root: the folder holding its ModCrateInfo.txt (the folder itself, or the one folder it was zipped in)
bool FindCrateRoot(const fs::path& folder, fs::path* root)
{
    if (!FindNamed(folder, "modcrateinfo.txt").empty())
    {
        *root = folder;
        return true;
    }

    const std::vector<std::string> subs = Subfolders(folder);
    if (subs.size() == 1 && !FindNamed(folder / subs[0], "modcrateinfo.txt").empty())
    {
        *root = folder / subs[0];
        return true;
    }
    return false;
}

// A ModCrateInfo.txt: `key=value` lines, '!' and '#' comments, keys lower case
std::map<std::string, std::string> ReadValues(const std::string& text)
{
    std::map<std::string, std::string> values;
    size_t at = 0;
    if (text.size() >= 3 && static_cast<u8>(text[0]) == 0xEF && static_cast<u8>(text[1]) == 0xBB &&
        static_cast<u8>(text[2]) == 0xBF)
    {
        at = 3;
    }

    while (at <= text.size())
    {
        const size_t end = text.find('\n', at);
        std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? text.size() + 1 : end + 1;
        line = Trim(line);
        if (line.empty() || line[0] == '!' || line[0] == '#')
        {
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos)
        {
            continue;
        }
        values[Lower(Trim(line.substr(0, equals)))] = Trim(line.substr(equals + 1));
    }
    return values;
}

// A string member of a mod.json, without a JSON reader (the port reads the few fields a mod is shown by)
std::string JsonString(const std::string& text, const char* field)
{
    const std::string name = std::string("\"") + field + "\"";
    size_t at = text.find(name);
    if (at == std::string::npos)
    {
        return {};
    }
    at = text.find(':', at + name.size());
    if (at == std::string::npos)
    {
        return {};
    }
    at = text.find('"', at);
    if (at == std::string::npos)
    {
        return {};
    }

    std::string value;
    for (size_t index = at + 1; index < text.size(); index++)
    {
        if (text[index] == '\\' && index + 1 < text.size())
        {
            value += text[++index];
            continue;
        }
        if (text[index] == '"')
        {
            break;
        }
        value += text[index];
    }
    return value;
}

// The disc's own folders: anything else in a mod is an entry of the archive
bool DiscName(const std::string& first)
{
    return first == "CRASH6" || first == "FMV" || first == "SYSTEM.CNF" || first == "SLES_525.68" ||
           first == "SLUS_209.09" || first == "SLPM_658.01";
}

// The archive's own folders (CRASH6/CRASH.BH's names)
bool ArchiveName(const std::string& first)
{
    return first == "STARTUP" || first == "LEVELS" || first == "LANGUAGE" || first == "EXTRAS";
}

void AddDisc(const std::string& key, const std::string& host, Applied* applied)
{
    if (key.empty())
    {
        return;
    }
    g_Disc[key] = host;
    // A whole file is the file: a recipe for the same path (an earlier mod's) is no longer what it is made of
    g_Ranges.erase(key);
    if (applied != nullptr)
    {
        applied->disc++;
    }
}

// A TwinsanityEvolved recipe's name (`<disc path>.ranges`)
bool IsRanges(const std::string& name)
{
    const std::string lower = Lower(name);
    return lower.size() > 7 && lower.compare(lower.size() - 7, 7, ".ranges") == 0;
}

// A decimal number of a recipe's own (digits only, and no more than a 64-bit one)
bool Number(const std::string& text, u64* value)
{
    if (text.empty() || text.size() > 20)
    {
        return false;
    }
    u64 number = 0;
    for (const char digit : text)
    {
        if (digit < '0' || digit > '9')
        {
            return false;
        }
        const u64 add = static_cast<u64>(digit - '0');
        if (number > (0xFFFFFFFFFFFFFFFFull - add) / 10)
        {
            return false;
        }
        number = number * 10 + add;
    }
    *value = number;
    return true;
}

// Whether a name is a patch of the whole disc image: xdelta3's VCDIFF, a PPF or a BPS, by the names the community gives them
bool PatchName(const std::string& name)
{
    const std::string lower = Lower(name);
    for (const char* extension : {".xdelta", ".xdelta3", ".vcdiff", ".ppf", ".bps"})
    {
        const size_t length = std::strlen(extension);
        if (lower.size() > length && lower.compare(lower.size() - length, length, extension) == 0)
        {
            return true;
        }
    }
    return false;
}

// The folder's patch, when the folder holds one patch and nothing of the disc's own (a patch dropped in the mods folder as a
// folder of its own): an empty path when it is not one
fs::path PatchIn(const fs::path& folder)
{
    fs::path patch;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(folder, error))
    {
        std::error_code type;
        if (entry.is_directory(type))
        {
            return {};
        }
        if (!entry.is_regular_file(type))
        {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (Metadata(name))
        {
            continue;
        }
        if (!PatchName(name))
        {
            return {};
        }
        if (!patch.empty())
        {
            return {};
        }
        patch = entry.path();
    }
    return patch;
}

// A path inside the mod's folder: relative, with no `..` and no drive or device in it
bool SafeRelative(const std::string& path, std::string* clean)
{
    const std::string text = Slashes(path);
    if (text.empty() || text[0] == '/' || (text.size() > 1 && text[1] == ':'))
    {
        return false;
    }
    std::string joined;
    size_t at = 0;
    while (at <= text.size())
    {
        const size_t end = text.find('/', at);
        const std::string part = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? text.size() + 1 : end + 1;
        if (part.empty() || part == ".")
        {
            continue;
        }
        if (part == ".." || part.find(':') != std::string::npos)
        {
            return false;
        }
        joined += (joined.empty() ? "" : "/") + part;
    }
    if (joined.empty())
    {
        return false;
    }
    *clean = joined;
    return true;
}

// A TwinsanityEvolved recipe: a file of the disc kept as the pieces it changes -- `base <offset> <length>` of the disc's own
// file, `file <payload> <offset> <length>` of one of the mod's own files, `zero <length>`, with '#' a comment when it starts a
// line or follows a blank. Platform::Files makes the file out of them (files.cpp), so a mod keeps only what it changes
bool ReadRanges(const fs::path& root, const fs::path& recipe, const std::string& key, const std::string& label, Applied* applied)
{
    std::string text;
    if (!ReadText(recipe, &text))
    {
        std::fprintf(stderr, "mods: %s does not read: the file is left out\n", Slashes(recipe.string()).c_str());
        return false;
    }
    if (text.size() > (64u << 20))
    {
        std::fprintf(stderr, "mods: %s is over 64 MB: not a recipe, the file is left out\n", Slashes(recipe.string()).c_str());
        return false;
    }

    Platform::Mods::Ranges ranges;
    ranges.path = key;
    size_t line = 0;
    size_t at = 0;
    if (text.size() >= 3 && static_cast<u8>(text[0]) == 0xEF && static_cast<u8>(text[1]) == 0xBB &&
        static_cast<u8>(text[2]) == 0xBF)
    {
        at = 3;
    }
    while (at <= text.size())
    {
        const size_t end = text.find('\n', at);
        std::string lineText = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? text.size() + 1 : end + 1;
        line++;

        // The comment, the way TwinsanityEvolved writes one
        for (size_t index = 0; index < lineText.size(); index++)
        {
            if (lineText[index] == '#' && (index == 0 || lineText[index - 1] == ' ' || lineText[index - 1] == '\t'))
            {
                lineText.resize(index);
                break;
            }
        }

        // The line's words, and where each starts (a payload's name may hold blanks)
        std::vector<std::string> words;
        std::vector<size_t> starts;
        for (size_t index = 0; index < lineText.size();)
        {
            while (index < lineText.size() && (lineText[index] == ' ' || lineText[index] == '\t' || lineText[index] == '\r'))
            {
                index++;
            }
            if (index >= lineText.size())
            {
                break;
            }
            const size_t begin = index;
            while (index < lineText.size() && lineText[index] != ' ' && lineText[index] != '\t' && lineText[index] != '\r')
            {
                index++;
            }
            words.push_back(lineText.substr(begin, index - begin));
            starts.push_back(begin);
        }
        if (words.empty())
        {
            continue;
        }

        const std::string where = "mods: " + label + " line " + std::to_string(line) + ": ";
        const std::string operation = Lower(words[0]);
        Platform::Mods::RangePiece piece;
        if (operation == "base")
        {
            if (words.size() != 3 || !Number(words[1], &piece.offset) || !Number(words[2], &piece.length))
            {
                std::fprintf(stderr, "%sbase <offset> <length>: the file is left out\n", where.c_str());
                return false;
            }
            piece.kind = Platform::Mods::RangePiece::Kind::Base;
        }
        else if (operation == "file")
        {
            if (words.size() < 4 || !Number(words[words.size() - 2], &piece.offset) || !Number(words.back(), &piece.length))
            {
                std::fprintf(stderr, "%sfile <path> <offset> <length>: the file is left out\n", where.c_str());
                return false;
            }
            std::string relative = lineText.substr(starts[1], starts[words.size() - 2] - starts[1]);
            while (!relative.empty() && (relative.back() == ' ' || relative.back() == '\t'))
            {
                relative.pop_back();
            }
            std::string clean;
            if (!SafeRelative(relative, &clean))
            {
                std::fprintf(stderr, "%s'%s' is not a path inside the mod's folder: the file is left out\n", where.c_str(),
                             relative.c_str());
                return false;
            }
            piece.host = Slashes((root / clean).string());
            piece.kind = Platform::Mods::RangePiece::Kind::Payload;
        }
        else if (operation == "zero")
        {
            if (words.size() != 2 || !Number(words[1], &piece.length))
            {
                std::fprintf(stderr, "%szero <length>: the file is left out\n", where.c_str());
                return false;
            }
            piece.kind = Platform::Mods::RangePiece::Kind::Zero;
        }
        else
        {
            std::fprintf(stderr, "%s'%s' is not base, file or zero: the file is left out\n", where.c_str(), words[0].c_str());
            return false;
        }

        if (piece.length == 0)
        {
            continue;
        }
        if (ranges.size + piece.length > 0xFFFFFFFFull)
        {
            std::fprintf(stderr, "mods: %s is over 4 GB: the file is left out\n", label.c_str());
            return false;
        }
        ranges.size += piece.length;
        ranges.pieces.push_back(std::move(piece));
    }

    if (ranges.pieces.empty())
    {
        std::fprintf(stderr, "mods: %s has no base, file or zero in it: the file is left out\n", label.c_str());
        return false;
    }

    if (g_Disc.erase(key) != 0)
    {
        std::fprintf(stderr, "mods: %s: %s is a recipe of a file an earlier mod gives whole: the recipe is the one read\n",
                     label.c_str(), key.c_str());
    }
    g_Ranges[key] = std::move(ranges);
    if (applied != nullptr)
    {
        applied->disc++;
    }
    return true;
}

void AddArchive(const std::string& key, const std::string& host, Applied* applied)
{
    if (key.empty())
    {
        return;
    }
    g_Archive[key] = host;
    if (applied != nullptr)
    {
        applied->archive++;
    }
}

// A folder of the game's own files: CRASH6/ and FMV/ are the disc's, the archive's folders are its entries
void ScanFiles(const fs::path& tree, const Mod* mod, Applied* applied)
{
    for (const std::string& relative : FilesRecursive(tree))
    {
        // A recipe is not a file of the disc: ScanMod reads what it says the file is made of
        if (IsRanges(relative))
        {
            continue;
        }

        // A mod's own notes are not content
        if (Metadata(relative.substr(relative.find_last_of('/') + 1)))
        {
            continue;
        }

        const std::string key = DiscKey(relative);
        const std::string first = key.substr(0, key.find('/'));
        const std::string host = Slashes((tree / relative).string());
        if (ArchiveName(first))
        {
            AddArchive(ArchiveKey(relative), host, applied);
        }
        else if (DiscName(first))
        {
            if (key == "CRASH6/CRASH.BH")
            {
                g_IndexHost = host;
            }
            else if (key == "CRASH6/CRASH.BD")
            {
                g_DataHost = host;
            }
            AddDisc(key, host, applied);
        }
        else
        {
            // A bare file as the tools write it (a level chunk, a picture, a text bank): placed by its name when the archive has
            // one entry of that name (files.cpp), the way TwinsanityEvolved places one
            AddArchive(ArchiveKey(relative), host, applied);
        }
    }
    (void)mod;
}

// A Mod Crate: layer0 is the disc's files, layer1 the archive's entries (Crate Mod Loader's own layers for this game)
bool ScanCrate(const fs::path& root, Mod* mod, Applied* applied)
{
    std::string text;
    std::map<std::string, std::string> info;
    const fs::path manifest = FindNamed(root, "modcrateinfo.txt");
    if (!manifest.empty() && ReadText(manifest, &text))
    {
        info = ReadValues(text);
    }

    if (info.count("name") != 0)
    {
        mod->name = info["name"];
    }
    mod->author = info.count("author") != 0 ? info["author"] : "";
    mod->version = info.count("version") != 0 ? info["version"] : "";
    mod->description = info.count("description") != 0 ? info["description"] : "";

    const std::string game = Lower(info.count("game") != 0 ? info["game"] : "");
    if (!game.empty() && game != "crashts" && game != "all")
    {
        std::fprintf(stderr, "mods: %s is a Mod Crate for %s, not Crash Twinsanity: left out\n", mod->id.c_str(),
                     info["game"].c_str());
        return false;
    }

    // Crate Mod Loader's code: the property edits it makes, and the scripts it runs. Their files still apply, the code does not.
    if (!FindNamed(root, "modcratesettings.txt").empty())
    {
        std::fprintf(stderr, "mods: %s: its ModCrateSettings.txt changes the game's code, which this port cannot apply: left out of the mod\n",
                     mod->id.c_str());
    }
    {
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(root, error))
        {
            std::error_code type;
            if (entry.is_directory(type) && Lower(entry.path().filename().string()).find("script") != std::string::npos)
            {
                std::fprintf(stderr, "mods: %s: its %s folder runs Crate Mod Loader's own code, which this port cannot run: left out\n",
                             mod->id.c_str(), entry.path().filename().string().c_str());
            }
        }
    }

    const fs::path layer0 = FindFolder(root, "layer0");
    const fs::path layer1 = FindFolder(root, "layer1");
    if (layer0.empty() && layer1.empty())
    {
        std::fprintf(stderr, "mods: %s has no layer0 or layer1 folder: left out\n", mod->id.c_str());
        return false;
    }

    if (!layer0.empty())
    {
        for (const std::string& relative : FilesRecursive(layer0))
        {
            const std::string key = DiscKey(relative);
            const std::string host = Slashes((layer0 / relative).string());
            if (key == "CRASH6/CRASH.BH")
            {
                g_IndexHost = host;
            }
            else if (key == "CRASH6/CRASH.BD")
            {
                g_DataHost = host;
            }
            AddDisc(key, host, applied);
        }
    }

    if (!layer1.empty())
    {
        for (const std::string& relative : FilesRecursive(layer1))
        {
            AddArchive(ArchiveKey(relative), Slashes((layer1 / relative).string()), applied);
        }
    }

    return applied->disc != 0 || applied->archive != 0;
}

// A TwinsanityEvolved mod folder: mod.json and a disc/ tree laid out like the disc
bool ScanMod(const fs::path& folder, Mod* mod, Applied* applied)
{
    std::string text;
    const fs::path manifest = FindNamed(folder, "mod.json");
    if (manifest.empty() || !ReadText(manifest, &text))
    {
        return false;
    }

    const std::string name = JsonString(text, "name");
    mod->name = name.empty() ? mod->id : name;
    mod->author = JsonString(text, "author");
    mod->version = JsonString(text, "version");
    mod->description = JsonString(text, "description");

    const std::string kind = Lower(JsonString(text, "kind"));
    if (kind == "language")
    {
        std::fprintf(stderr, "mods: %s is a TwinsanityEvolved language pack: this port plays its disc's own languages, so it is left out\n",
                     mod->id.c_str());
        return false;
    }
    if (kind == "builtin")
    {
        std::fprintf(stderr, "mods: %s is one of TwinsanityEvolved's built-ins, not a disc mod: left out\n", mod->id.c_str());
        return false;
    }

    const fs::path disc = FindFolder(folder, "disc");
    if (disc.empty())
    {
        std::fprintf(stderr, "mods: %s has no disc/ folder: nothing to apply\n", mod->id.c_str());
        return false;
    }

    ScanFiles(disc, mod, applied);

    // TwinsanityEvolved's recipes: a file of the disc kept as the ranges a mod changes of it, made whole by Platform::Files
    for (const std::string& relative : FilesRecursive(disc))
    {
        if (!IsRanges(relative))
        {
            continue;
        }
        const std::string key = DiscKey(relative.substr(0, relative.size() - 7));
        ReadRanges(folder, disc / relative, key, mod->id + ": disc/" + relative, applied);
    }

    return applied->disc != 0 || applied->archive != 0;
}

// mods.txt (or enabled.txt): the mods that are on, in the order they apply. A line of '*' or 'all' is every mod.
std::vector<std::string> ReadEnabled(const fs::path& folder, bool* listed)
{
    *listed = false;
    fs::path path = FindNamed(folder, "mods.txt");
    if (path.empty())
    {
        path = FindNamed(folder, "enabled.txt");
    }
    if (path.empty())
    {
        return {};
    }

    std::string text;
    if (!ReadText(path, &text))
    {
        return {};
    }
    *listed = true;

    std::vector<std::string> ids;
    size_t at = 0;
    if (text.size() >= 3 && static_cast<u8>(text[0]) == 0xEF && static_cast<u8>(text[1]) == 0xBB &&
        static_cast<u8>(text[2]) == 0xBF)
    {
        at = 3;
    }
    while (at <= text.size())
    {
        const size_t end = text.find('\n', at);
        std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? text.size() + 1 : end + 1;
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
        {
            continue;
        }
        ids.push_back(line);
    }
    return ids;
}

// Whether the game can write in a folder (its mods.txt, an unpacked zip), leaving nothing behind
bool WritableDirectory(const fs::path& folder)
{
    std::error_code error;
    const fs::path probe = folder / ".write-test";
    std::FILE* file = std::fopen(Slashes(probe.string()).c_str(), "wb");
    if (file == nullptr)
    {
        return false;
    }
    std::fclose(file);
    fs::remove(probe, error);
    return true;
}

// The folder to read: the run's own, the environment's, the player's own (the settings file's `mods_dir`), mods/ beside the
// disc's files, ./mods, else the host's writable folder (where a phone or a page keeps the game's files)
std::string ResolveFolder(const char* named, const char* preferred, const char* writable)
{
    if (named != nullptr && named[0] != '\0')
    {
        return Slashes(named);
    }

    if (const char* environment = std::getenv("TWINSANITY_MODS"); environment != nullptr && environment[0] != '\0')
    {
        return Slashes(environment);
    }

    if (preferred != nullptr && preferred[0] != '\0')
    {
        return Slashes(preferred);
    }

    std::error_code error;
    const char* root = Platform::Files::Root();
    if (root != nullptr && root[0] != '\0')
    {
        const fs::path base(root);
        const fs::path beside = fs::is_directory(base, error) ? base / "mods" : base.parent_path() / "mods";
        if (fs::is_directory(beside, error))
        {
            return Slashes(beside.string());
        }
    }

    if (fs::is_directory("mods", error))
    {
        return "mods";
    }

    // Nothing anywhere: the host's own writable folder, which is all a phone's app or a page has
    if (writable != nullptr && writable[0] != '\0')
    {
        return Slashes(writable);
    }
    return {};
}

void Forget()
{
    Platform::DiscPatch::Forget();
    g_Folder.clear();
    g_Mods.clear();
    g_Disc.clear();
    g_Ranges.clear();
    g_Archive.clear();
    g_ArchiveFiles.clear();
    g_Listed.clear();
    g_IndexHost.clear();
    g_DataHost.clear();
    g_Enabled = false;
}
}

void Platform::Mods::Initialise(const char* folder, const std::vector<std::string>& extra, bool none, const char* preferred,
                                const char* writable)
{
    Forget();
    g_Folder = ResolveFolder(folder, preferred, writable);
    if (g_Folder.empty())
    {
        return;
    }

    const fs::path dir(g_Folder);
    std::error_code error;
    if (!fs::is_directory(dir, error))
    {
        // A folder the game chose itself (the settings file's, or the host's writable one) is made; one the run named is taken as
        // given, so a typo is said rather than turned into an empty folder
        const bool ours = (preferred != nullptr && g_Folder == Slashes(preferred)) ||
                          (writable != nullptr && g_Folder == Slashes(writable));
        if (!ours)
        {
            std::fprintf(stderr, "mods: %s is not a folder: no mods\n", g_Folder.c_str());
            g_Folder.clear();
            return;
        }

        std::error_code made;
        fs::create_directories(dir, made);
        if (made || !fs::is_directory(dir, error))
        {
            std::fprintf(stderr, "mods: %s cannot be made: no mods\n", g_Folder.c_str());
            g_Folder.clear();
            return;
        }
        std::fprintf(stderr, "mods: %s made for the mods\n", g_Folder.c_str());
    }

    // A folder that cannot be written (a disc kept in a phone's or a page's read-only place) keeps its mods: the mods page cannot
    // save mods.txt there and zips cannot be unpacked there, which is said once
    if (!WritableDirectory(dir))
    {
        std::fprintf(stderr,
                     "mods: %s cannot be written: the mods page cannot change mods.txt and zips cannot be unpacked there\n",
                     g_Folder.c_str());
    }

    // A zipped mod (the community's crates arrive so) is unpacked into the folder of its name, once
    ExtractZips(dir);

    // Every folder is a mod, told apart by what it holds
    for (const std::string& name : Subfolders(dir))
    {
        const fs::path modDir = dir / name;
        Mod mod;
        mod.id = name;
        mod.name = name;
        mod.dir = Slashes(modDir.string());

        fs::path crateRoot;
        const fs::path patch = PatchIn(modDir);
        if (!patch.empty())
        {
            mod.kind = "patch";
            mod.patch = Slashes(patch.string());
        }
        else if (FindCrateRoot(modDir, &crateRoot))
        {
            mod.kind = "crate";
            std::string text;
            if (const fs::path manifest = FindNamed(crateRoot, "modcrateinfo.txt"); !manifest.empty() && ReadText(manifest, &text))
            {
                const std::map<std::string, std::string> info = ReadValues(text);
                if (info.count("name") != 0 && !info.at("name").empty())
                {
                    mod.name = info.at("name");
                }
                mod.author = info.count("author") != 0 ? info.at("author") : "";
                mod.version = info.count("version") != 0 ? info.at("version") : "";
            }
        }
        else if (!FindNamed(modDir, "mod.json").empty())
        {
            mod.kind = "mod";
            std::string text;
            if (ReadText(FindNamed(modDir, "mod.json"), &text))
            {
                const std::string shown = JsonString(text, "name");
                mod.name = shown.empty() ? name : shown;
                mod.author = JsonString(text, "author");
                mod.version = JsonString(text, "version");
            }
        }
        else
        {
            mod.kind = "files";
        }

        g_Mods.push_back(std::move(mod));
    }

    // A patch dropped in the mods folder as itself is a mod too, named by the file's own name
    {
        std::error_code listError;
        for (const auto& entry : fs::directory_iterator(dir, listError))
        {
            std::error_code type;
            if (!entry.is_regular_file(type))
            {
                continue;
            }
            const std::string file = entry.path().filename().string();
            if (!PatchName(file))
            {
                continue;
            }
            Mod mod;
            mod.id = file.substr(0, file.find_last_of('.'));
            mod.name = mod.id;
            mod.kind = "patch";
            mod.dir = Slashes(entry.path().string());
            mod.patch = Slashes(entry.path().string());
            g_Mods.push_back(std::move(mod));
        }
    }

    bool listed = false;
    std::vector<std::string> order = ReadEnabled(dir, &listed);
    bool all = false;
    for (const std::string& id : order)
    {
        const std::string lower = Lower(id);
        all = all || lower == "*" || lower == "all";
    }
    if (all)
    {
        order.clear();
        for (const Mod& mod : g_Mods)
        {
            order.push_back(mod.id);
        }
    }
    for (const std::string& id : extra)
    {
        order.push_back(id);
    }

    // Nothing named: no mod is on (a folder listed in mods.txt is what turns one on)
    for (const std::string& id : order)
    {
        const std::string lower = Lower(id);
        if (lower == "*" || lower == "all")
        {
            continue;
        }

        Mod* found = nullptr;
        for (Mod& mod : g_Mods)
        {
            if (Lower(mod.id) == lower)
            {
                found = &mod;
                break;
            }
        }
        if (found == nullptr)
        {
            std::fprintf(stderr, "mods: %s is listed in mods.txt, but the mods folder has no %s\n", id.c_str(), id.c_str());
            continue;
        }
        if (!found->listed)
        {
            found->listed = true;
            g_Listed.push_back(found->id);
        }
        if (found->on || none)
        {
            continue;
        }

        Applied applied;
        bool ok = false;
        const fs::path modDir = dir / found->id;
        if (found->kind == "crate")
        {
            fs::path root;
            FindCrateRoot(modDir, &root);
            ok = ScanCrate(root, found, &applied);
        }
        else if (found->kind == "mod")
        {
            ok = ScanMod(modDir, found, &applied);
        }
        else if (found->kind == "patch")
        {
            // A patch of the whole image: the game reads the patched one as it goes, so nothing is unpacked or copied
            const char* image = Platform::Iso9660::ImagePath();
            if (image == nullptr || image[0] == '\0')
            {
                std::fprintf(stderr, "mods: %s is a patch of the disc image, and the game is playing a folder of files: no patch\n",
                             found->id.c_str());
            }
            else if (Platform::DiscPatch::Loaded())
            {
                std::fprintf(stderr, "mods: %s is a patch, and another patch is already on: a patch of the whole image cannot be "
                                     "put over another's, so it is left out\n",
                             found->id.c_str());
            }
            else if (Platform::DiscPatch::Load(found->patch.c_str(), image))
            {
                Platform::Iso9660::Repatched();
                ok = true;
            }
        }
        else
        {
            ScanFiles(PayloadRoot(modDir), found, &applied);
            ok = applied.disc != 0 || applied.archive != 0;
        }

        found->on = ok;
        found->discFiles = applied.disc;
        found->archiveFiles = applied.archive;
        if (ok && found->kind == "patch")
        {
            std::fprintf(stderr, "mods: %s on (patch, %s): the disc image is the patched one\n", found->id.c_str(),
                         Platform::DiscPatch::Label());
        }
        else if (ok)
        {
            std::fprintf(stderr, "mods: %s on (%s%s%s): %u files of the disc, %u entries of the archive\n", found->id.c_str(),
                         found->kind.c_str(), found->name == found->id ? "" : ", ", found->name == found->id ? "" : found->name.c_str(),
                         applied.disc, applied.archive);
        }
        else if (found->kind == "files")
        {
            std::fprintf(stderr, "mods: %s has no file of the disc or of its archive: left out\n", found->id.c_str());
        }
    }

    // The archive's entries: a later mod's entry is the one the map kept
    for (const auto& entry : g_Archive)
    {
        g_ArchiveFiles.push_back({entry.first, entry.second});
    }

    g_Enabled = !g_Disc.empty() || !g_Ranges.empty() || !g_Archive.empty() || !g_IndexHost.empty() || !g_DataHost.empty() ||
                Platform::DiscPatch::Loaded();
}

const char* Platform::Mods::Folder()
{
    return g_Folder.c_str();
}

const std::vector<Platform::Mods::Mod>& Platform::Mods::Mods()
{
    return g_Mods;
}

const std::vector<std::string>& Platform::Mods::Listed()
{
    return g_Listed;
}

bool Platform::Mods::WriteList(const std::vector<std::string>& ids)
{
    if (g_Folder.empty())
    {
        return false;
    }

    const std::string path = g_Folder + "/mods.txt";
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
    {
        std::fprintf(stderr, "mods: %s cannot be written: the mods stay as they were\n", path.c_str());
        return false;
    }

    std::fprintf(file, "# Crash Twinsanity mods: one folder per line, in the order they apply. The game's mods page writes this.\n");
    for (const std::string& id : ids)
    {
        const std::string trimmed = Trim(id);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';')
        {
            continue;
        }
        std::fprintf(file, "%s\n", trimmed.c_str());
    }

    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    if (!ok)
    {
        std::fprintf(stderr, "mods: %s did not write whole: the mods stay as they were\n", path.c_str());
        return false;
    }

    // The list from now on: what a second look at the folder would read
    g_Listed.clear();
    for (const std::string& id : ids)
    {
        const std::string trimmed = Trim(id);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';')
        {
            continue;
        }
        g_Listed.push_back(trimmed);
    }
    for (Mod& mod : g_Mods)
    {
        mod.listed = false;
    }
    for (const std::string& id : g_Listed)
    {
        for (Mod& mod : g_Mods)
        {
            if (Lower(mod.id) == Lower(id))
            {
                mod.listed = true;
            }
        }
    }
    return true;
}

bool Platform::Mods::Enabled()
{
    return g_Enabled;
}

u32 Platform::Mods::DiscFileCount()
{
    return static_cast<u32>(g_Disc.size() + g_Ranges.size());
}

u32 Platform::Mods::ArchiveFileCount()
{
    return static_cast<u32>(g_Archive.size());
}

const std::string* Platform::Mods::File(const char* discPath)
{
    if (!g_Enabled || discPath == nullptr)
    {
        return nullptr;
    }

    const auto found = g_Disc.find(DiscKey(discPath));
    return found == g_Disc.end() ? nullptr : &found->second;
}

const Platform::Mods::Ranges* Platform::Mods::RangesFile(const char* discPath)
{
    if (!g_Enabled || discPath == nullptr)
    {
        return nullptr;
    }

    const auto found = g_Ranges.find(DiscKey(discPath));
    return found == g_Ranges.end() ? nullptr : &found->second;
}

const std::string* Platform::Mods::ArchiveIndex()
{
    return g_Enabled && !g_IndexHost.empty() ? &g_IndexHost : nullptr;
}

const std::string* Platform::Mods::ArchiveData()
{
    return g_Enabled && !g_DataHost.empty() ? &g_DataHost : nullptr;
}

const std::vector<Platform::Mods::ArchiveFile>& Platform::Mods::ArchiveFiles()
{
    return g_ArchiveFiles;
}

std::string Platform::Mods::Describe()
{
    if (g_Folder.empty())
    {
        return {};
    }

    if (g_Mods.empty())
    {
        return "mods: " + g_Folder + ": no mods in it (one folder per mod, mods.txt lists the ones that are on)";
    }

    u32 enabled = 0;
    std::string names;
    for (const Mod& mod : g_Mods)
    {
        if (!mod.on)
        {
            continue;
        }
        enabled++;
        names += (names.empty() ? "" : ", ") + mod.id;
    }

    std::string line = "mods: " + g_Folder + ": " + std::to_string(g_Mods.size()) +
                       (g_Mods.size() == 1 ? " mod, " : " mods, ") + std::to_string(enabled) + " on";
    if (enabled == 0)
    {
        line += " (list a mod's folder in mods.txt to turn it on)";
        return line;
    }

    line += " (" + names + "): " + std::to_string(DiscFileCount()) + " files of the disc, " +
            std::to_string(ArchiveFileCount()) + " entries of the archive";
    if (Platform::DiscPatch::Loaded())
    {
        line += ", the image patched by " + std::string(Platform::DiscPatch::Label());
    }
    line += "; saves go to " + Platform::Saves::CardsFolder();
    return line;
}

bool Platform::Mods::Verify()
{
    if (!g_Enabled)
    {
        std::fprintf(stderr, "mods: no mod is on: the disc is read as it is\n");
        return true;
    }

    // A patch of the image: the game reads it through Platform::DiscPatch, which is what the volume was read again through
    if (Platform::DiscPatch::Loaded())
    {
        std::fprintf(stderr, "mods: the disc image is patched (%s): %llu bytes as it leaves it, your image %llu\n",
                     Platform::DiscPatch::Label(), static_cast<unsigned long long>(Platform::DiscPatch::Size()),
                     static_cast<unsigned long long>(Platform::Iso9660::Bytes()));
    }

    // The index the game is given (through the file layer, so the rebuilt one when the mods make one)
    std::vector<u8> index;
    const s32 indexFile = Platform::Files::Open("Crash6\\Crash.BH", Platform::Files::OpenRead);
    if (indexFile < 0)
    {
        std::fprintf(stderr, "mods: the game cannot open the archive's index\n");
        return false;
    }
    while (index.size() <= (32u << 20))
    {
        u8 chunk[0x10000];
        const s32 read = Platform::Files::Read(indexFile, chunk, sizeof(chunk));
        if (read <= 0)
        {
            break;
        }
        index.insert(index.end(), chunk, chunk + read);
    }
    Platform::Files::Close(indexFile);

    struct Row
    {
        std::string name;
        u64 offset = 0;
        u64 size = 0;
    };
    std::vector<Row> rows;
    if (index.size() < 4)
    {
        std::fprintf(stderr, "mods: the archive's index does not read\n");
        return false;
    }
    size_t at = 4;
    while (at + 4 <= index.size())
    {
        const u32 length = Get32(index.data() + at);
        if (length == 0 || length > 0x3FF || at + 4 + length + 8 > index.size())
        {
            std::fprintf(stderr, "mods: the archive's index does not read as this game's\n");
            return false;
        }
        Row row;
        row.name.assign(reinterpret_cast<const char*>(index.data() + at + 4), length);
        row.offset = Get32(index.data() + at + 4 + length);
        row.size = Get32(index.data() + at + 4 + length + 4);
        rows.push_back(std::move(row));
        at += 4 + length + 8;
    }

    // The data the game is given
    const s32 data = Platform::Files::Open("Crash6\\Crash.BD", Platform::Files::OpenRead);
    if (data < 0)
    {
        std::fprintf(stderr, "mods: the game cannot open the archive's data\n");
        return false;
    }
    const s32 end = Platform::Files::Seek(data, 0, Platform::Files::SeekEnd);
    const u64 dataSize = end > 0 ? static_cast<u64>(end) : 0;

    u32 problems = 0;
    for (const ArchiveFile& file : g_ArchiveFiles)
    {
        const std::string key = ArchiveKey(file.path);
        const Row* row = nullptr;
        for (const Row& candidate : rows)
        {
            if (ArchiveKey(candidate.name) == key)
            {
                row = &candidate;
                break;
            }
        }
        if (row == nullptr)
        {
            // A file placed by its bare name: the archive's one entry of that name
            const size_t slash = key.rfind('\\');
            const std::string bare = slash == std::string::npos ? key : key.substr(slash + 1);
            size_t matches = 0;
            for (const Row& candidate : rows)
            {
                const std::string name = ArchiveKey(candidate.name);
                const size_t cut = name.rfind('\\');
                if ((cut == std::string::npos ? name : name.substr(cut + 1)) != bare)
                {
                    continue;
                }
                matches++;
                row = &candidate;
            }
            if (matches != 1)
            {
                row = nullptr;
            }
        }
        if (row == nullptr)
        {
            std::fprintf(stderr, "mods: %s is not in the archive's index\n", file.path.c_str());
            problems++;
            continue;
        }
        if (row->offset + row->size > dataSize)
        {
            std::fprintf(stderr, "mods: %s is past the end of the archive (%llu + %llu over %llu)\n", file.path.c_str(),
                         static_cast<unsigned long long>(row->offset), static_cast<unsigned long long>(row->size),
                         static_cast<unsigned long long>(dataSize));
            problems++;
            continue;
        }

        // Its bytes, as the mod has them and as the game reads them: the first and the last 4 KB
        const u64 sample = row->size < 4096 ? row->size : 4096;
        bool same = true;
        for (u32 part = 0; part < 2 && same; part++)
        {
            if (sample == 0 || (part == 1 && row->size <= sample))
            {
                break;
            }
            const u64 from = part == 0 ? 0 : row->size - sample;
            std::vector<u8> mine(static_cast<size_t>(sample));
            std::vector<u8> theirs(static_cast<size_t>(sample));
            if (!ReadHostRange(file.host, from, mine.data(), mine.size()) ||
                Platform::Files::ReadAt(data, static_cast<u32>(row->offset + from), theirs.data(), static_cast<s32>(sample)) !=
                    static_cast<s32>(sample) ||
                std::memcmp(mine.data(), theirs.data(), mine.size()) != 0)
            {
                same = false;
            }
        }
        if (!same)
        {
            std::fprintf(stderr, "mods: %s: the bytes the game reads are not the mod's\n", file.path.c_str());
            problems++;
        }
    }
    Platform::Files::Close(data);

    // The disc files the mods replace: the game opens every one of them, whole, from the mod
    for (const auto& entry : g_Disc)
    {
        const s32 file = Platform::Files::Open(entry.first.c_str(), Platform::Files::OpenRead);
        if (file < 0)
        {
            std::fprintf(stderr, "mods: %s does not open\n", entry.first.c_str());
            problems++;
            continue;
        }
        Platform::Files::Close(file);
    }

    // The files the mods give as ranges: each composes from what it names (a payload that does not hold its ranges, or a base
    // range past the disc's file, is said as it composes) and reads as long as its recipe says
    for (const auto& entry : g_Ranges)
    {
        const s32 file = Platform::Files::Open(entry.first.c_str(), Platform::Files::OpenRead);
        if (file < 0)
        {
            std::fprintf(stderr, "mods: %s does not compose from its recipe\n", entry.first.c_str());
            problems++;
            continue;
        }
        const s32 end = Platform::Files::Seek(file, 0, Platform::Files::SeekEnd);
        if (end < 0 || static_cast<u64>(end) != entry.second.size)
        {
            std::fprintf(stderr, "mods: %s is %d bytes long where its recipe adds up to %llu\n", entry.first.c_str(), end,
                         static_cast<unsigned long long>(entry.second.size));
            problems++;
        }
        Platform::Files::Close(file);
    }

    std::fprintf(stderr, "mods: verified %zu entries of the archive and %zu files of the disc the mods change: ",
                 g_ArchiveFiles.size(), g_Disc.size() + g_Ranges.size());
    if (problems == 0)
    {
        std::fprintf(stderr, "what the game reads is the mods'\n");
    }
    else
    {
        std::fprintf(stderr, "%u problem(s)\n", problems);
    }
    return problems == 0;
}
