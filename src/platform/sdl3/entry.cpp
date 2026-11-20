// Entry: opens the game's window, initializes bgfx on it, then runs the game. The game code
// creates bgfx objects during startup (materials, textures), so the context must exist before Main runs.
#include "common.h"
#include "graphics/bgfx_context.h"

#include "display_sdl.h"

#include "platform/discverify.h"
#include "platform/gamedata.h"
#include "platform/mods.h"
#include "platform/retaildata.h"
#include "settings.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" int Main(u32 argc, char** argv);

// The host's crash log (crashlog.cpp): a crash outside gdb still leaves where it happened beside the run
void InstallCrashLog();

int main(int argc, char** argv)
{
    InstallCrashLog();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD))
    {
        std::fprintf(stderr, "sdl3: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // The run's own options are read and taken out of argv before the game's parser sees the rest: the retail parser takes the
    // words as its own arguments and would make `--disc` one of them
    const char* disc = nullptr;
    bool verifyDisc = false;
    bool verifyData = false;
    const char* dumpData = nullptr;
    const char* executable = nullptr;
    const char* modsFolder = nullptr;
    std::vector<std::string> mods;
    bool noMods = false;
    bool verifyMods = false;
    bool skipIntro = false;
    int count = 1;
    for (int index = 1; index < argc; index++)
    {
        if (std::strcmp(argv[index], "--disc") == 0 && index + 1 < argc)
        {
            disc = argv[index + 1];
            index++;
            continue;
        }
        if (std::strncmp(argv[index], "--disc=", 7) == 0)
        {
            disc = argv[index] + 7;
            continue;
        }
        if (std::strcmp(argv[index], "--verify-disc") == 0)
        {
            verifyDisc = true;
            continue;
        }
        if (std::strcmp(argv[index], "--verify-data") == 0)
        {
            verifyData = true;
            continue;
        }
        if (std::strcmp(argv[index], "--dump-data") == 0 && index + 1 < argc)
        {
            dumpData = argv[++index];
            continue;
        }
        if (std::strcmp(argv[index], "--exe") == 0 && index + 1 < argc)
        {
            executable = argv[++index];
            continue;
        }
        if (std::strncmp(argv[index], "--exe=", 6) == 0)
        {
            executable = argv[index] + 6;
            continue;
        }
        if (std::strcmp(argv[index], "--mods-dir") == 0 && index + 1 < argc)
        {
            modsFolder = argv[index + 1];
            index++;
            continue;
        }
        if (std::strncmp(argv[index], "--mods-dir=", 11) == 0)
        {
            modsFolder = argv[index] + 11;
            continue;
        }
        if (std::strcmp(argv[index], "--mod") == 0 && index + 1 < argc)
        {
            mods.push_back(argv[index + 1]);
            index++;
            continue;
        }
        if (std::strncmp(argv[index], "--mod=", 6) == 0)
        {
            mods.push_back(argv[index] + 6);
            continue;
        }
        if (std::strcmp(argv[index], "--no-mods") == 0)
        {
            noMods = true;
            continue;
        }
        if (std::strcmp(argv[index], "--verify-mods") == 0)
        {
            verifyMods = true;
            continue;
        }
        if (std::strcmp(argv[index], "--skip-intro") == 0)
        {
            skipIntro = true;
            continue;
        }
#if defined(__EMSCRIPTEN__)
        // The page cannot set a variable for the run it starts, and its log is where the frame's numbers are read: the flag
        // turns on the line graphics.cpp prints once a second (the same one TWINSANITY_FRAME_LOG does)
        if (std::strcmp(argv[index], "--frame-log") == 0)
        {
            setenv("TWINSANITY_FRAME_LOG", "1", 1);
            continue;
        }
#endif
        argv[count++] = argv[index];
    }
    argc = count;

    // The boot logos, for this run only (the settings file's `skip_intro` keeps it): the movies after them, a new game's intro
    // included, still play
    if (skipIntro)
    {
        Platform::System::SetSkipIntro(true);
    }

    // The window comes first: the picker that finds the disc's files is a dialog a window belongs to, and until the files are
    // found there is nothing to draw in it. Its size and mode are the settings file's (the options menu's display rows write
    // them), 960x540 windowed when it has none
    SDL_Window* window = SDL_CreateWindow("Crash Twinsanity", static_cast<int>(DisplayWindowWidth()),
                                          static_cast<int>(DisplayWindowHeight()),
                                          DisplayWindowFlags() | SDL_WINDOW_RESIZABLE);
    if (window == nullptr)
    {
        std::fprintf(stderr, "sdl3: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // The disc's files: what --disc named, the working directory's, or what the player picks (and the run keeps for next time)
    if (!Platform::GameData::Locate(disc, window))
    {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // The retail data: a DATA=runtime build carries none of it, and reads it out of the disc's own executable here, before
    // anything touches a table. A build from the split already has it and this does nothing. --exe (or TWINSANITY_EXE) names
    // the European release's own file instead, which is what a disc of another region is played with
    const char* namedExecutable = executable != nullptr ? executable : std::getenv("TWINSANITY_EXE");
    Platform::RetailData::SetExecutable(namedExecutable);
    if (!Platform::RetailData::Load(verifyData))
    {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    if (dumpData != nullptr)
    {
        const bool dumped = Platform::RetailData::Dump(dumpData);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return dumped ? 0 : 1;
    }
    if (verifyData)
    {
        std::fprintf(stderr, "verify-data: %s\n", Platform::RetailData::Baked() ? "this build carries the data baked in"
                                                                                : "the disc's data is loaded");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }

    // The mods: the disc's files are located now. The folder is --mods-dir, else TWINSANITY_MODS, else the settings file's
    // `mods_dir`, else mods/ beside the disc's files, else the host's own writable one (a phone's app storage, a page's file
    // system, where the disc itself cannot be written). Every file the game opens from here on is the mods' first
    char* preferences = SDL_GetPrefPath("twinsanity", "twinsanity");
    std::string writableMods;
    if (preferences != nullptr)
    {
        writableMods = std::string(preferences) + "mods";
        SDL_free(preferences);
    }
    const std::string preferredMods = HostSettingsModsFolder();
    Platform::Mods::Initialise(modsFolder, mods, noMods, preferredMods.c_str(), writableMods.c_str());
    {
        const std::string line = Platform::Mods::Describe();
        if (!line.empty())
        {
            std::fprintf(stderr, "%s\n", line.c_str());
        }
    }
    if (verifyMods)
    {
        // What the game will read from the mods, byte for byte, then out before anything is drawn
        const bool modsOk = Platform::Mods::Verify();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return modsOk ? 0 : 1;
    }

    // The display's window is the host's from here: the mode the settings asked for is applied to it, and the picture's
    // antialiasing (the file's `antialias` line, which the display keeps too) is in place before the renderer's first frame
    // makes the post passes' targets
    DisplayAttach(window);
    BgfxSetAntiAliasing(Platform::Display::AntiAliasing());

    if (verifyDisc)
    {
        const char* located = Platform::GameData::LocatedPath();
        if (located == nullptr)
        {
            std::fprintf(stderr, "verify-disc: this run has no disc to verify\n");
        }
        else
        {
            Platform::DiscVerify::Image(located);
        }
    }

    // The swap chain is the window's drawable, which is the resolution the file asked for (or what the display gave for it)
    int backbufferWidth = 0;
    int backbufferHeight = 0;
    SDL_GetWindowSizeInPixels(window, &backbufferWidth, &backbufferHeight);
    if (!BgfxInit(window, backbufferWidth, backbufferHeight, Platform::Display::Vsync()))
    {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    int status = Main(static_cast<u32>(argc), argv);
    BgfxShutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return status;
}
