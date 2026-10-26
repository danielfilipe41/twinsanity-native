// Bgfx context: initializes bgfx on an SDL3 window's native handle, clears and presents
// frames. The game code doesn't touch bgfx directly; this wraps what its renderer needs.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>

// The 16:9 window the menus' fractions-of-the-screen drawing maps onto, kept in step with entry.cpp's
constexpr int BgfxWindowWidth = 960;
constexpr int BgfxWindowHeight = 540;

// Initializes bgfx on window's native handle (X11 display + window through SDL's window properties, Wayland
// display + surface when SDL picked Wayland), with a 960x540 swap chain and the debug clear color; returns whether
// bgfx initialized (BGFX_RESET_NONE: no vsync, the test drives its own frame count)
bool BgfxInit(SDL_Window* window);

// Clears view 0 to the debug color and advances a frame; call once per presented frame
void BgfxFrame();

// Shuts bgfx down; safe to call without a matching BgfxInit
void BgfxShutdown();

// The swap chain size BgfxInit requested, for the report the test prints
void BgfxSwapSize(std::uint16_t* width, std::uint16_t* height);
