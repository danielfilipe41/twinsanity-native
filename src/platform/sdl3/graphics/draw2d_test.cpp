// The draw2d headless test's main: opens an SDL3 window, initializes the platform's bgfx context on it, then
// draws 100 sprites/strips through the host DrawSprite/DrawTurnedSprite/DrawStrip (the flat material made by
// MakeFlatMaterial, storage filled by ConstructMaterial) and presents the frames. Exits 0 when every draw submitted
// without aborting; reachable via `ninja draw2d-test`. Not part of the game build (configure.py keeps it out of the
// game sources, like bgfx_test.cpp).
#include "bgfx_context.h"
#include "host_graphics.h"

#include "game/math.h"
#include "platform/graphics.h"

#include <SDL3/SDL.h>

#include <cstdio>

namespace
{
// How many sprites and strips the test pushes through the host functions (50 each, 100 draws), and how many frames
// it presents them over
constexpr int SpriteDraws = 50;
constexpr int StripDraws = 50;
constexpr int TestFrames = 60;
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::fprintf(stderr, "draw2d-test: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window =
        SDL_CreateWindow("Crash Twinsanity (draw2d test)", BgfxWindowWidth, BgfxWindowHeight, SDL_WINDOW_RESIZABLE);
    if (window == nullptr)
    {
        std::fprintf(stderr, "draw2d-test: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    if (!BgfxInit(window))
    {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // The flat material and a ConstructMaterial-filled storage: the draws run through the real material paths
    Material* flat = Platform::Graphics::MakeFlatMaterial();
    alignas(8) u8 storage[Platform::Graphics::MaterialStorage];
    Platform::Graphics::ConstructMaterial(reinterpret_cast<Material*>(storage));
    if (flat == nullptr)
    {
        std::fprintf(stderr, "draw2d-test: MakeFlatMaterial returned null\n");
        BgfxShutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    if (Platform::Graphics::FlatMaterial() != flat)
    {
        std::fprintf(stderr, "draw2d-test: FlatMaterial is not the made material\n");
        BgfxShutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // The frame the strips' pixels are fractions of (the window); StartFrame (graphics.cpp) does exactly these two
    // sets, and the test links without that translation unit's game dependencies
    Platform::Graphics::HostSetScreenSize(BgfxWindowWidth, BgfxWindowHeight);
    Platform::Graphics::HostSetDisplaySize(BgfxWindowWidth, BgfxWindowHeight);

    for (int presented = 0; presented < TestFrames; presented++)
    {
        SDL_PumpEvents();
        // 50 sprites sweeping the screen (place and texture fractions walk so every draw differs)
        for (int sprite = 0; sprite < SpriteDraws; sprite++)
        {
            f32 at = static_cast<f32>((sprite + presented) % SpriteDraws) / static_cast<f32>(SpriteDraws);
            Platform::Graphics::Rectangle place = {at * 0.8f, at * 0.6f, 0.2f, 0.2f};
            Platform::Graphics::Rectangle texture = {0.0f, 0.0f, 1.0f, 1.0f};
            u32 colour = 0x80808080;
            Platform::Graphics::DrawSprite(flat, colour, place, texture);
            if ((sprite & 1) != 0)
            {
                Vector2 corners[4] = {{at * 0.8f, at * 0.6f},
                                      {at * 0.8f, at * 0.6f + 0.2f},
                                      {at * 0.8f + 0.2f, at * 0.6f},
                                      {at * 0.8f + 0.2f, at * 0.6f + 0.2f}};
                Platform::Graphics::DrawTurnedSprite(flat, colour, corners, texture);
            }
        }
        // 50 strips across the screen (pixel places, per-vertex colours)
        for (int strip = 0; strip < StripDraws; strip++)
        {
            f32 row = static_cast<f32>((strip * 10 + presented) % BgfxWindowHeight);
            Vector2 places[4] = {
                {0.0f, row}, {static_cast<f32>(BgfxWindowWidth) * 0.5f, row + 5.0f},
                {static_cast<f32>(BgfxWindowWidth) * 0.5f, row}, {static_cast<f32>(BgfxWindowWidth), row + 5.0f}};
            u32 colours[4] = {0xFF0000FF, 0xFF00FF00, 0xFFFF0000, 0xFFFFFFFF};
            Platform::Graphics::DrawStrip(flat, 4, places, colours);
        }
        BgfxFrame();
    }

    Platform::Graphics::DestroyMaterial(reinterpret_cast<Material*>(storage));
    std::printf("draw2d-test: drew %d draws per frame over %d frames\n", (SpriteDraws + SpriteDraws / 2 + StripDraws),
                TestFrames);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

// This proof drives the 2D draws only; it does not link graphics.cpp, which owns the presented-frame counter that
// the shared 3D draw code reads for its debug gates. Without a definition here the link fails. No frames are
// presented in this test, so 0 is the honest answer.
namespace Platform
{
namespace Graphics
{
u32 HostFrameCounter()
{
    return 0;
}
void HostShadowSetFrameTarget(bgfx::FrameBufferHandle, u32, u32, bool)
{
    // This proof's 2D draws never need the shadow frame; shadows.cpp owns the real hook.
}
}
}
