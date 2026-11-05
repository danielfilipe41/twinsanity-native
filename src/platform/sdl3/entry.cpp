// Entry: opens the game's window, initializes bgfx on it, then runs the game. The game code
// creates bgfx objects during startup (materials, textures), so the context must exist before Main runs.
#include "common.h"
#include "graphics/bgfx_context.h"

#include "display_sdl.h"

#include "platform/discverify.h"
#include "platform/gamedata.h"
#include "settings.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>

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
        argv[count++] = argv[index];
    }
    argc = count;

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
