// Bgfx context: initializes bgfx on an SDL3 window's native handle, clears and presents
// frames. The game code doesn't touch bgfx directly; this wraps what its renderer needs.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>

// The 16:9 window the menus' fractions-of-the-screen drawing maps onto, kept in step with entry.cpp's
constexpr int BgfxWindowWidth = 960;
constexpr int BgfxWindowHeight = 540;

// Anti-aliasing, the settings file's `antialias` line: how the finished frame is cleaned up. Both modes are post passes over the composited frame,
// so they cover what the picture shows (the 3D geometry, the sprites and the menus' text alike). Hardware multisampling is
// deliberately not one of them: the scene target it would apply to is rendered into throughout the frame and left again
// several times (the sky, the shadow volumes, the screen effects and the fog each take the frame elsewhere and back), and
// bgfx resolves a multisample target on every one of those switches, depth included, which cost this port's GL driver a
// factor of five and more whatever the depth format
enum class AntiAlias : std::uint8_t
{
    Off,  // the default: nothing, which is how the render checks' reference pictures were drawn
    Fxaa, // one pass, no look-up tables: the cheapest, and the softest
    Smaa, // three passes with SMAA's area/search tables: sharper, and it keeps subpixel detail
};

// The antialiasing the frame is drawn with, and the way it is set: the port's own start-up reads the settings file's
// `antialias` line (settings.h's HostSettingsAntiAlias) and hands it over before BgfxInit, so this context needs no file of
// its own and the headless proofs, which never set it, draw as they always have (off)
AntiAlias HostAntiAliasing();
void BgfxSetAntiAliasing(AntiAlias mode);

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
