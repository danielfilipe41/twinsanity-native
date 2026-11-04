// Entry: opens the game's window, initializes bgfx on it, then runs the game. The game code
// creates bgfx objects during startup (materials, textures), so the context must exist before Main runs.
#include "common.h"
#include "graphics/bgfx_context.h"

#include "platform/gamedata.h"
#include "settings.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>

extern "C" int Main(u32 argc, char** argv);

// The host's crash log (crashlog.cpp): a crash outside gdb still leaves where it happened beside the run
void InstallCrashLog();

namespace
{
// The game's window: 960x540, the 16:9 window the menus' fractions-of-the-screen drawing maps onto
constexpr int WindowWidth = 960;
constexpr int WindowHeight = 540;
}

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
        argv[count++] = argv[index];
    }
    argc = count;

    // The window comes first: the picker that finds the disc's files is a dialog a window belongs to, and until the files are
    // found there is nothing to draw in it
    SDL_Window* window = SDL_CreateWindow("Crash Twinsanity", WindowWidth, WindowHeight, SDL_WINDOW_RESIZABLE);
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

    // The picture's antialiasing: the settings file's `antialias` line, read before the renderer's first frame makes the
    // post passes' targets
    BgfxSetAntiAliasing(HostSettingsAntiAlias());

    if (!BgfxInit(window))
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
