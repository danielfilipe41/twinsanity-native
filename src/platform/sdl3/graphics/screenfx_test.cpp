// The screenfx headless test's main: opens an SDL3 window, initializes the platform's bgfx context on it,
// uploads a synthetic depth gradient (left half near, right half far) through two palette entries, runs the
// fullscreen depth-as-palette filter, reads the output back and checksums it. Exits 0 when the checksum matches
// the expected split (left half the first colour's words, right half the second's); reachable via
// `ninja screenfx-test`. Not part of the game build (configure.py keeps it out of the game sources, like
// bgfx_test.cpp and draw2d_test.cpp).
#include "bgfx_context.h"
#include "host_graphics.h"

#include "game/math.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
// The proof's size and colours: a 64x8 gradient (cheap to read back), entry 0 opaque red, entry 1 opaque green
constexpr u32 ProofWidth = 64;
constexpr u32 ProofHeight = 8;
constexpr u32 NearColour = 0xFF0000FF;
constexpr u32 FarColour = 0xFF00FF00;
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::fprintf(stderr, "screenfx-test: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window =
        SDL_CreateWindow("Crash Twinsanity (screenfx test)", BgfxWindowWidth, BgfxWindowHeight, SDL_WINDOW_RESIZABLE);
    if (window == nullptr)
    {
        std::fprintf(stderr, "screenfx-test: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    if (!BgfxInit(window))
    {
        std::fprintf(stderr, "screenfx-test: BgfxInit failed\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    // Two palette entries through the game's screen-effect id: entry 0 red (near), entry 255 green (far), the rest
    // black
    u32 bank[Platform::Graphics::ScreenFxPaletteColours] = {};
    bank[0] = NearColour;
    bank[Platform::Graphics::ScreenFxPaletteColours - 1] = FarColour;
    Platform::Graphics::ScreenFxUploadPalette(0, bank);
    Platform::Graphics::ScreenFxSelectEffect(0);
    if (Platform::Graphics::ScreenFxSelectedEffect() != 0)
    {
        std::fprintf(stderr, "screenfx-test: selected effect not kept\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // The synthetic depth gradient: left half near (entry 0), right half far (entry 1), through the pre-pass
    std::vector<f32> depths(ProofWidth * ProofHeight);
    for (u32 row = 0; row < ProofHeight; row++)
    {
        for (u32 column = 0; column < ProofWidth; column++)
        {
            depths[row * ProofWidth + column] = column < ProofWidth / 2 ? 0.0f : 1.0f;
        }
    }
    Platform::Graphics::ScreenFxUploadDepth(ProofWidth, ProofHeight, depths.data());

    // The filter's pass over the gradient, then frames until the readback lands
    Platform::Graphics::ScreenFxDrawFilter();
    std::vector<u8> pixels(ProofWidth * ProofHeight * 4);
    bool landed = false;
    for (int spin = 0; spin < 60 && !landed; spin++)
    {
        SDL_PumpEvents();
        Platform::Graphics::ScreenFxDrawFilter();
        landed = Platform::Graphics::ScreenFxReadOutput(ProofWidth, ProofHeight, pixels.data());
    }
    if (!landed)
    {
        std::fprintf(stderr, "screenfx-test: readback never landed\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // The checksum: every word on the left the near colour, every word on the right the far one
    u64 checksum = 0;
    const auto* words = reinterpret_cast<const u32*>(pixels.data());
    for (u32 pixel = 0; pixel < ProofWidth * ProofHeight; pixel++)
    {
        checksum += words[pixel];
    }
    u64 half = (static_cast<u64>(ProofWidth) / 2) * ProofHeight;
    u64 expected = half * static_cast<u64>(NearColour) + half * static_cast<u64>(FarColour);
    std::printf("screenfx-test: checksum 0x%llX, expected 0x%llX\n", static_cast<unsigned long long>(checksum),
                static_cast<unsigned long long>(expected));
    BgfxShutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (checksum != expected || std::memcmp(words, bank, sizeof(u32)) != 0)
    {
        std::fprintf(stderr, "screenfx-test: mismatch (first word 0x%08X)\n", words[0]);
        return 1;
    }
    std::printf("screenfx-test: depth gradient through 2 palette entries, checksum matches\n");
    return 0;
}
