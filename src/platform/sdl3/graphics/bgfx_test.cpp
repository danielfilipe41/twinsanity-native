// The bgfx standalone test's main: opens an SDL3 window, initializes the platform's bgfx context on it, clears
// and presents 120 frames, then exits 0. Not part of the game build (configure.py keeps bgfx_test.cpp out of
// the game sources); reachable via `ninja bgfx-test`.
#include "bgfx_context.h"

#include <SDL3/SDL.h>

#include <cstdio>

namespace
{
// How many cleared frames the test presents before exiting successfully
constexpr int TestFrames = 120;
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::fprintf(stderr, "bgfx-test: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window =
        SDL_CreateWindow("Crash Twinsanity (bgfx test)", BgfxWindowWidth, BgfxWindowHeight, SDL_WINDOW_RESIZABLE);
    if (window == nullptr)
    {
        std::fprintf(stderr, "bgfx-test: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    if (!BgfxInit(window))
    {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    std::uint16_t width = 0;
    std::uint16_t height = 0;
    BgfxSwapSize(&width, &height);
    std::printf("bgfx-test: initialized %ux%u, presenting %d frames\n", width, height, TestFrames);

    for (int frame = 0; frame < TestFrames; frame++)
    {
        SDL_PumpEvents();
        BgfxFrame();
    }

    std::printf("bgfx-test: presented %d frames\n", TestFrames);
    BgfxShutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
