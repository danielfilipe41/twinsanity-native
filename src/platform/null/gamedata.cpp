// Where the game's files are: the disc's folder for this run, found without asking when it can be (the environment, the
// working directory, or the folder the player picked last time) and asked for when it can't. The answer is kept beside the
// saves in gamedata.txt, so the next run opens straight away, and a folder that has lost the files asks again.
//
// This is the part every host shares; the asking itself is the platform's (PickFolder: a dialog on the SDL3 hosts).
#include "platform/gamedata.h"

#include "platform/files.h"
#include "platform/saves.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace
{
// The disc root's marker: SYSTEM.CNF sits at the top of every extracted PAL disc, and the game's chunk files sit beside it
constexpr const char* DataMarker = "SYSTEM.CNF";
constexpr const char* RecordName = "gamedata.txt";
constexpr u32 PathLength = 1024;

// The environment can name the folder: a launcher, a script, or a platform whose picker hands over something that isn't a
// path (Android's storage access framework gives a content:// URI, so getting the files there is a copy, not a path)
constexpr const char* DataVariable = "TWINSANITY_DATA";

std::string RecordPath()
{
    char base[PathLength - 32];
    Platform::Saves::DataDirectory(base, sizeof(base));
    return std::string(base) + "/" + RecordName;
}

bool HasGameFiles(const std::filesystem::path& directory)
{
    std::error_code error;
    return std::filesystem::is_regular_file(directory / DataMarker, error);
}

// The disc's root itself, or the `disc` folder inside it (this repository's own layout: the folder is picked, its disc holds
// the files)
bool FindGameFiles(const std::string& directory, std::string* found)
{
    if (directory.empty())
    {
        return false;
    }

    const std::filesystem::path root(directory);
    if (HasGameFiles(root))
    {
        *found = root.string();
        return true;
    }

    const std::filesystem::path nested = root / "disc";
    if (HasGameFiles(nested))
    {
        *found = nested.string();
        return true;
    }

    return false;
}

// The folder kept last time: the file's first line, empty when there is none
std::string ReadRecord(const std::string& path)
{
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
    {
        return {};
    }

    char line[PathLength] = {};
    const size_t read = std::fread(line, 1, sizeof(line) - 1, file);
    std::fclose(file);

    std::string kept(line, read);
    while (!kept.empty() && (kept.back() == '\n' || kept.back() == '\r' || kept.back() == ' ' || kept.back() == '\t'))
    {
        kept.pop_back();
    }
    return kept;
}

void WriteRecord(const std::string& path, const std::string& folder)
{
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
    {
        std::fprintf(stderr, "gamedata: could not keep the folder in %s; it will be asked for again\n", path.c_str());
        return;
    }

    std::fprintf(file, "%s\n", folder.c_str());
    std::fclose(file);
}
}

bool Platform::GameData::Locate()
{
#if defined(__EMSCRIPTEN__)
    // The page hands the game its data, so a browser has no folder to find
    return true;
#else
    std::string found;

    // The environment first, then the folder the run started in (the port's usual way to be run), then the one kept last time
    if (const char* named = std::getenv(DataVariable); named != nullptr && FindGameFiles(named, &found))
    {
        Platform::Files::SetRoot(found.c_str());
        return true;
    }

    if (FindGameFiles(".", &found))
    {
        // The working directory: relative paths already resolve against it, so there is no root to set
        return true;
    }

    const std::string record = RecordPath();
    const std::string kept = ReadRecord(record);
    if (FindGameFiles(kept, &found))
    {
        Platform::Files::SetRoot(found.c_str());
        return true;
    }

    // Nothing this run knows has the files: ask, and keep the answer for the next run
    char chosen[PathLength] = {};
    if (!PickFolder(kept.empty() ? nullptr : kept.c_str(), chosen, sizeof(chosen)))
    {
        std::fprintf(stderr,
                     "gamedata: this run has no disc files: the game's files are in neither the working directory nor the "
                     "folder kept last time.\n"
                     "gamedata: run the game from the disc's folder (the one with %s), or set %s to it.\n",
                     DataMarker, DataVariable);
        return false;
    }

    if (!FindGameFiles(chosen, &found))
    {
        std::fprintf(stderr, "gamedata: %s has no %s, so it isn't the disc's folder.\n", chosen, DataMarker);
        return false;
    }

    WriteRecord(record, found);
    std::fprintf(stderr, "gamedata: reading the game's files from %s\n", found.c_str());
    Platform::Files::SetRoot(found.c_str());
    return true;
#endif
}
