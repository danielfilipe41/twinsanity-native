// The picture's presentation: the resolution it is shown at, the window's mode, the rate this host presents at, the
// antialiasing and the shape of the picture within the window. The options menu's display rows show and change these; a host
// with a window applies them at once and keeps them in its settings file. A host with none (the PS2 build) says it has
// nothing to present, and the menu shows no rows.
//
// The game's own screen is not touched by any of it: the renderer draws the frame into a scene target of its own and the
// composite stretches that onto the window, so the resolution and the scale are the host's picture and the game's coordinates
// stay the game's.
#pragma once

#include "common.h"

// How the finished frame is cleaned up: both modes are post passes over the composited frame, so they cover what the picture
// shows (the 3D geometry, the sprites and the menus' text alike). Hardware multisampling is deliberately not one of them: the
// scene target it would apply to is rendered into throughout the frame and left again several times (the sky, the shadow
// volumes, the screen effects and the fog each take the frame elsewhere and back), and bgfx resolves a multisample target on
// every one of those switches, depth included, which cost this port's GL driver a factor of five and more whatever the depth
// format
enum class AntiAlias : u8
{
    Off,  // nothing, which is how the render checks' reference pictures were drawn
    Fxaa, // one pass, no look-up tables: the cheapest, and the softest
    Smaa, // three passes with SMAA's area/search tables: sharper, and it keeps subpixel detail
};

namespace Platform
{
namespace Display
{
// Whether this host presents a picture of its own: false on a host that has no window (the PS2 build), where the settings
// below are kept and nothing is done with them
bool Presentable();

// The resolutions offered, named as the menu shows them ("1280 x 720"), the one in use and how to change it. The list holds
// what the display can show, and the one in use even when it is none of those (a window resized by hand)
u32 ResolutionCount();
const char* const* ResolutionNames();
u32 Resolution();
void SetResolution(u32 index);

// The window's modes (windowed, borderless, full screen) and their words
u32 WindowModeCount();
const char* const* WindowModeNames();
u32 WindowMode();
void SetWindowMode(u32 index);

// The rates this host presents at (the picture's own; the game's logic stays the disc's field rate) and their words ("120")
u32 FrameRateCount();
const char* const* FrameRateNames();
u32 FrameRate();
void SetFrameRateIndex(u32 index);

// Whether presenting waits for the display's refresh: no tearing, and no frame more often than the display can show one
bool Vsync();
void SetVsync(bool on);

// The shape the picture keeps: stretched over the window, or fitted (its 16:9 kept, black bars filling the rest)
u32 AspectCount();
const char* const* AspectNames();
u32 Aspect();
void SetAspect(u32 index);

// The scene target's size against the design's 960x540 ("100%" is the design's): more pixels are a sharper picture at a higher
// cost, fewer a blurrier one at a lower cost, whatever the window's size
u32 ScaleCount();
const char* const* ScaleNames();
u32 Scale();
void SetScale(u32 index);
// The scene target's size the current scale asks for (the design's 960x540 times the scale, rounded to even pixels)
u32 SceneWidth();
u32 SceneHeight();

// The camera's reach, as a multiple of the lens's far plane (1 is retail's 1500 units): what the game draws is culled at it, so
// a longer reach shows what retail's pop-in left out, and costs what the extra geometry costs. The game's own fog is unchanged,
// so the farthest of it may stay in the haze
u32 DrawDistanceCount();
const char* const* DrawDistanceNames();
u32 DrawDistance();
void SetDrawDistance(u32 index);

// The antialiasing and its words ("off", "fxaa", "smaa")
u32 AntiAliasCount();
const char* const* AntiAliasNames();
AntiAlias AntiAliasing();
void SetAntiAliasing(AntiAlias mode);
}
}
