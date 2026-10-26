// Entry: opens the game's window, initializes bgfx on it, then runs the game. The game code
// creates bgfx objects during startup (materials, textures), so the context must exist before Main runs.
#include "common.h"
#include "graphics/bgfx_context.h"

#include <SDL3/SDL.h>

#include <cstdio>

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

    SDL_Window* window = SDL_CreateWindow("Crash Twinsanity", WindowWidth, WindowHeight, SDL_WINDOW_RESIZABLE);
    if (window == nullptr)
    {
        std::fprintf(stderr, "sdl3: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

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
