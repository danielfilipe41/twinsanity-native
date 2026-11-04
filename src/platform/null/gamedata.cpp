// Where the game's files are: the disc's folder for this run, or a disc image read through its own file system, found without
// asking when it can be (the run's own --disc, the environment, the working directory, or what the player pointed at last
// time) and asked for when it can't. The answer is kept beside the saves in gamedata.txt, so the next run opens straight away,
// and a folder or image that has lost the files asks again.
//
// This is the part every host shares; the asking itself is the platform's (PickFolder and PickImage: dialogs on the SDL3 hosts).
#include "platform/gamedata.h"

#include "iso9660.h"
#include "platform/files.h"
#include "platform/saves.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
// The disc root's marker: SYSTEM.CNF sits at the top of every PS2 disc, extracted or in its image, and the game's chunk files
// sit beside it
constexpr const char* DataMarker = "SYSTEM.CNF";
constexpr const char* RecordName = "gamedata.txt";
constexpr u32 PathLength = 1024;

// The environment can name the folder or the image: a launcher, a script, or a platform whose picker hands over something that
// isn't a path (Android's storage access framework gives a content:// URI, so getting the files there is a copy, not a path)
constexpr const char* DataVariable = "TWINSANITY_DATA";

// What this run's Locate resolved to, for the run's own reporting (--verify-disc names the image it hashed)
std::string g_LocatedPath;

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

// Whether a disc image holds the game's files: its volume is opened for the question and closed again (opening parses the
// volume descriptor only, so this is a handful of reads)
bool ImageHasGameFiles(const std::string& path)
{
    if (!Platform::Iso9660::Open(path.c_str()))
    {
        return false;
    }
    u64 offset = 0;
    u32 size = 0;
    const bool has = Platform::Iso9660::Find(DataMarker, &offset, &size);
    Platform::Iso9660::Close();
    return has;
}

// The disc images directly inside a folder, by name: what the player is choosing between when a folder holds several
std::vector<std::string> ImagesIn(const std::filesystem::path& folder)
{
    std::vector<std::string> images;
    std::error_code error;
    for (const std::filesystem::directory_iterator::value_type& entry : std::filesystem::directory_iterator(folder, error))
    {
        std::error_code typeError;
        if (!entry.is_regular_file(typeError))
        {
            continue;
        }
        const std::string path = entry.path().string();
        if (Platform::Iso9660::LooksLikeImage(path.c_str()))
        {
            images.push_back(path);
        }
    }
    std::sort(images.begin(), images.end());
    return images;
}

// The disc the path names: a disc image, the folder the files were extracted into (or this repository's `disc` folder inside
// the one picked), or a folder holding one disc image. A folder holding several reports itself through `several`, because only
// the player can say which one they meant
bool FindGameFiles(const std::string& path, std::string* found, std::string* several)
{
    if (path.empty())
    {
        return false;
    }

    const std::filesystem::path root(path);
    std::error_code error;
    if (std::filesystem::is_regular_file(root, error))
    {
        // An image: the volume says whether the game's files are in it
        if (ImageHasGameFiles(root.string()))
        {
            *found = root.string();
            return true;
        }
        return false;
    }

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

    // A folder of disc images: the only one, or a question for the player
    const std::vector<std::string> images = ImagesIn(root);
    if (images.size() == 1)
    {
        *found = images[0];
        return true;
    }
    if (images.size() > 1)
    {
        *several = root.string();
    }
    return false;
}

// The path kept last time: the file's first line, empty when there is none
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

// What the files are being read from, in the run's own log: the folder, or the image and the volume identifier it records
void Announce(const std::string& path)
{
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error) && Platform::Iso9660::Open(path.c_str()))
    {
        std::fprintf(stderr, "gamedata: reading the game's files from the disc image %s (volume \"%s\")\n", path.c_str(),
                     Platform::Iso9660::VolumeId());
        Platform::Iso9660::Close();
        return;
    }
    std::fprintf(stderr, "gamedata: reading the game's files from %s\n", path.c_str());
}

// The image picker, started in a folder that isn't the disc: it reaches an image kept anywhere (a folder of several, or one
// beside files that are not images). When it cannot ask, the images the folder does hold are listed for the command line
bool AskWhichImage(const std::string& folder, std::string* found, void* window)
{
    std::vector<std::string> images = ImagesIn(folder);
    char chosen[PathLength] = {};
    if (Platform::GameData::PickImage(folder.c_str(), chosen, sizeof(chosen), window))
    {
        std::string several;
        if (FindGameFiles(chosen, found, &several))
        {
            return true;
        }
        std::fprintf(stderr, "gamedata: %s is not a disc image with the game's files in it\n", chosen);
        return false;
    }

    if (images.empty())
    {
        std::fprintf(stderr, "gamedata: %s has no %s and holds no disc image of the game\n", folder.c_str(), DataMarker);
        return false;
    }
    std::fprintf(stderr, "gamedata: %s holds %zu disc images; name the one to play with it:\n", folder.c_str(),
                 images.size());
    for (const std::string& image : images)
    {
        std::fprintf(stderr, "gamedata:   --disc \"%s\"\n", image.c_str());
    }
    return false;
}
}

bool Platform::GameData::Locate(const char* named, void* window)
{
#if defined(__EMSCRIPTEN__)
    // The page hands the game its data, so a browser has no folder to find
    return true;
#else
    std::string found;
    std::string several;
    const std::string record = RecordPath();

    // The run's own answer first (--disc): used as given, and not kept, because it is this run's choice and not the player's
    // standing one
    if (named != nullptr && named[0] != '\0')
    {
        if (!FindGameFiles(named, &found, &several))
        {
            if (!several.empty())
            {
                if (!AskWhichImage(several, &found, window))
                {
                    return false;
                }
                WriteRecord(record, found);
                Announce(found);
                g_LocatedPath = found;
                Platform::Files::SetRoot(found.c_str());
                return true;
            }
            std::fprintf(stderr, "gamedata: --disc %s has no %s and is not a disc image of the game\n", named, DataMarker);
            return false;
        }
        // Kept like the picker's answer: --disc is a deliberate choice, and the next run then needs no option at all
        WriteRecord(record, found);
        Announce(found);
        g_LocatedPath = found;
        Platform::Files::SetRoot(found.c_str());
        return true;
    }

    // The environment, then the folder the run started in (the port's usual way to be run), then the one kept last time
    if (const char* environment = std::getenv(DataVariable); environment != nullptr)
    {
        if (FindGameFiles(environment, &found, &several))
        {
            Announce(found);
            g_LocatedPath = found;
            Platform::Files::SetRoot(found.c_str());
            return true;
        }
        if (!several.empty())
        {
            // Pointed at a folder of disc images: the one it holds, or the one the player names from it. Not falling through to
            // what was kept last time, because the environment is this run's explicit answer
            if (!AskWhichImage(several, &found, window))
            {
                return false;
            }
            Announce(found);
            g_LocatedPath = found;
            Platform::Files::SetRoot(found.c_str());
            return true;
        }
    }

    if (FindGameFiles(".", &found, &several))
    {
        // The working directory: relative paths already resolve against it, so there is no root to set
        g_LocatedPath = found;
        return true;
    }

    const std::string kept = ReadRecord(record);
    if (FindGameFiles(kept, &found, &several))
    {
        Announce(found);
        g_LocatedPath = found;
        Platform::Files::SetRoot(found.c_str());
        return true;
    }

    // Nothing this run knows has the files: ask, and keep the answer for the next run
    char chosen[PathLength] = {};
    if (!PickFolder(kept.empty() ? nullptr : kept.c_str(), chosen, sizeof(chosen), window))
    {
        std::fprintf(stderr,
                     "gamedata: this run has no disc files: the game's files are in neither the working directory nor what was "
                     "kept last time.\n"
                     "gamedata: run the game from the disc's folder (the one with %s), point it at the folder or at the disc "
                     "image (--disc, or %s), or let the picker find them.\n",
                     DataMarker, DataVariable);
        return false;
    }

    if (!FindGameFiles(chosen, &found, &several))
    {
        // Not the disc itself: the image picker from there, which reaches an image kept in that folder or anywhere else
        if (!AskWhichImage(several.empty() ? std::string(chosen) : several, &found, window))
        {
            return false;
        }
    }

    WriteRecord(record, found);
    Announce(found);
    g_LocatedPath = found;
    Platform::Files::SetRoot(found.c_str());
    return true;
#endif
}

const char* Platform::GameData::LocatedPath()
{
    return g_LocatedPath.empty() ? nullptr : g_LocatedPath.c_str();
}
