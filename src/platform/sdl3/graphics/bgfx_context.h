// Bgfx context: initializes bgfx on an SDL3 window's native handle, clears and presents
// frames. The game code doesn't touch bgfx directly; this wraps what its renderer needs.
#pragma once

#include <SDL3/SDL.h>

#include "platform/display.h"

#include <cstdint>

// The design's picture: 960x540, the 16:9 frame the menus' fractions-of-the-screen drawing maps onto and the resolution the
// scene target is drawn at when the resolution scale is 100%. The window it is presented in is the platform's own size
// (Platform::Display's), which is not this
constexpr int BgfxWindowWidth = 960;
constexpr int BgfxWindowHeight = 540;

// The antialiasing the frame is drawn with, and the way it is set: the port's own start-up reads the settings file's
// `antialias` line (settings.h's HostSettingsAntiAlias) and hands it over before BgfxInit, so this context needs no file of
// its own and the headless proofs, which never set it, draw as they always have (off)
AntiAlias HostAntiAliasing();
void BgfxSetAntiAliasing(AntiAlias mode);

// Initializes bgfx on window's native handle (X11 display + window through SDL's window properties, Wayland
// display + surface when SDL picked Wayland), with a swap chain of the given size (0: the design's) and the debug clear
// color; returns whether bgfx initialized. vsync is the display's (BGFX_RESET_VSYNC), off by default: a test drives its own
// frame count
bool BgfxInit(SDL_Window* window, int width = 0, int height = 0, bool vsync = false);

// The window bgfx draws into, for the platform's own display changes
SDL_Window* BgfxWindow();

// The backbuffer's size (the swap chain's, which is the window's drawable) and a reset to a new one: the window was resized or
// its mode changed, or vsync was switched. The scene target and the post passes are the host's own targets and are not
// touched: they are sized from the scene's, not the backbuffer's
u32 BgfxBackbufferWidth();
u32 BgfxBackbufferHeight();
void BgfxResetSwapchain(u32 width, u32 height, bool vsync);

// The scene target's size against the design's 960x540, in percent (Platform::Display's resolution scale): more pixels are
// a sharper picture at a higher cost, whatever the backbuffer's size. The scene target is remade on the next frame
void BgfxSetSceneScale(u32 percent);
u32 BgfxSceneWidth();
u32 BgfxSceneHeight();

// Whether the picture is fitted in the window (its 16:9 kept, black bars around it) rather than stretched over it
void BgfxSetPictureFit(bool fit);
bool BgfxPictureFit();

// Clears view 0 to the debug color and advances a frame; call once per presented frame
void BgfxFrame();

// Shuts bgfx down; safe to call without a matching BgfxInit
void BgfxShutdown();

// The swap chain size BgfxInit requested, for the report the test prints
void BgfxSwapSize(std::uint16_t* width, std::uint16_t* height);
