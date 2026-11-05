// The picture's presentation (platform/display.h): the window's size, its mode, vsync, the rate this host presents at, the
// antialiasing and the shape of the picture within the window. The options menu's display rows land here. A change applies at
// once (the window through SDL, the swap chain, the scene target and the post passes through the renderer) and is recorded in
// the settings file, which the game reads again at the next start.
//
// The window's size is the window server's to deliver: SDL's full screen call returns before the compositor has resized the
// window (on Wayland the new size arrives with the next event, and the call still reports the old one). Nothing reads the size
// back after a change, therefore; DisplaySync, once a frame, makes the swap chain the window's drawable and records a size the
// player set. A full screen's size is the display's and is never recorded as the resolution row's.
#include "platform/display.h"

#include "display_sdl.h"

#include "settings.h"

#include "graphics/bgfx_context.h"
#include "graphics/draws_host.h"

#include "platform/stream.h"
#include "platform/time.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
// The resolutions offered, when the desktop can show them: the design's and the common ones. The list is built once, at the
// first question, and always holds the one in use (a window resized by hand is added to it as the current entry)
struct Size
{
    u32 width;
    u32 height;
};

constexpr Size ResolutionPresets[] = {
    {960, 540}, {1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160},
};
constexpr u32 MaxResolutions = 10;
// The rates offered, and unlimited (0, which Platform::Time takes as no pacing at all): the list's last entry
constexpr u32 RatePresets[] = {30, 50, 60, 75, 100, 120, 144, 165, 240};
constexpr u32 UnlimitedRate = 0;
constexpr u32 MaxRates = 11;
// The resolution scale: the scene drawn at this multiple of the design's 960x540 and stretched over the window
constexpr float ScalePresets[] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
constexpr u32 MaxScales = 7;
// The camera's reach: the lens's far plane (1500 units) times this, so the game draws and culls further than retail's pop-in
constexpr float DrawDistancePresets[] = {1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
constexpr u32 MaxDrawDistances = 5;
// The words the mode, aspect and antialiasing are named with in the menu, and in the settings file (which stores the same words,
// so what the display wrote is what it reads back)
constexpr const char* WindowModeWords[3] = {"windowed", "borderless", "fullscreen"};
constexpr const char* AspectWords[2] = {"stretch", "fit"};
constexpr const char* AntiAliasWords[3] = {"off", "fxaa", "smaa"};
constexpr u32 MaxNameLength = 24;

Size g_resolutions[MaxResolutions];
const char* g_resolutionNames[MaxResolutions];
char g_resolutionText[MaxResolutions][MaxNameLength];
u32 g_resolutionCount;
u32 g_resolution;

u32 g_rates[MaxRates];
const char* g_rateNames[MaxRates];
char g_rateText[MaxRates][16];
u32 g_rateCount;
u32 g_rate;

float g_scales[MaxScales];
const char* g_scaleNames[MaxScales];
char g_scaleText[MaxScales][16];
u32 g_scaleCount;
u32 g_scale;

float g_drawDistances[MaxDrawDistances];
const char* g_drawDistanceNames[MaxDrawDistances];
char g_drawDistanceText[MaxDrawDistances][16];
u32 g_drawDistanceCount;
u32 g_drawDistance;

u32 g_windowMode;
bool g_vsync;
u32 g_aspect;
AntiAlias g_antiAliasing;

SDL_Window* g_window;
bool g_loaded;
// The size the window has reported while it settles (a player dragging it), and for how many frames
constexpr u32 SettleFrames = 5;
u32 g_settleWidth;
u32 g_settleHeight;
u32 g_settleFrames;

// The display the window is on, and the size its desktop mode has: the resolutions above it are not offered (a full screen
// cannot show them, and a window larger than the desktop is a window with something missing)
void DesktopSize(u32* width, u32* height)
{
    *width = static_cast<u32>(BgfxWindowWidth) * 2;
    *height = static_cast<u32>(BgfxWindowHeight) * 2;
    SDL_DisplayID display = g_window != nullptr ? SDL_GetDisplayForWindow(g_window) : SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* mode = display != 0 ? SDL_GetDesktopDisplayMode(display) : nullptr;
    if (mode != nullptr)
    {
        *width = static_cast<u32>(mode->w);
        *height = static_cast<u32>(mode->h);
    }
}

// What the window is asked for: the resolution row's size, or the file's when the player has never chosen one
Size WantedSize()
{
    const HostDisplaySettings settings = HostSettingsDisplay();
    if (settings.width != 0 && settings.height != 0)
    {
        return Size{settings.width, settings.height};
    }

    return Size{static_cast<u32>(BgfxWindowWidth), static_cast<u32>(BgfxWindowHeight)};
}

// The lists, built once: the presets the desktop can hold, and the value in use even when it is none of them
void BuildLists()
{
    u32 desktopWidth;
    u32 desktopHeight;
    DesktopSize(&desktopWidth, &desktopHeight);
    const HostDisplaySettings settings = HostSettingsDisplay();
    const Size wanted = WantedSize();

    g_resolutionCount = 0;
    g_resolution = 0;
    bool current = false;
    // The display's own mode first: whatever a player's screen is (a 1920x1200 laptop panel, an odd 1680x1050), it is the size
    // they most often want and the one no preset can be relied on to hold
    g_resolutions[g_resolutionCount] = Size{desktopWidth, desktopHeight};
    if (desktopWidth == wanted.width && desktopHeight == wanted.height)
    {
        g_resolution = g_resolutionCount;
        current = true;
    }
    g_resolutionCount++;

    for (const Size& preset : ResolutionPresets)
    {
        if (preset.width > desktopWidth || preset.height > desktopHeight)
        {
            continue;
        }

        if (preset.width == desktopWidth && preset.height == desktopHeight)
        {
            // The display's own mode is on the list already
            continue;
        }

        g_resolutions[g_resolutionCount] = preset;
        if (preset.width == wanted.width && preset.height == wanted.height)
        {
            g_resolution = g_resolutionCount;
            current = true;
        }
        g_resolutionCount++;
    }

    if (!current && g_resolutionCount < MaxResolutions)
    {
        // The file's size, or the design's when it has none: on the list even when no preset is it
        g_resolutions[g_resolutionCount] = wanted;
        g_resolution = g_resolutionCount;
        g_resolutionCount++;
    }

    for (u32 index = 0; index < g_resolutionCount; index++)
    {
        std::snprintf(g_resolutionText[index], sizeof(g_resolutionText[index]), "%u x %u", g_resolutions[index].width,
                      g_resolutions[index].height);
        g_resolutionNames[index] = g_resolutionText[index];
    }

    // The rates: the presets, the file's when it is none of them, and unlimited last (the file's 0)
    const s32 wantedRate = HostSettingsRenderRate() >= 0 ? HostSettingsRenderRate() : 0;
    g_rateCount = 0;
    g_rate = 0;
    current = false;
    for (const u32 preset : RatePresets)
    {
        g_rates[g_rateCount] = preset;
        if (static_cast<s32>(preset) == wantedRate)
        {
            g_rate = g_rateCount;
            current = true;
        }
        g_rateCount++;
    }

    if (!current && wantedRate != static_cast<s32>(UnlimitedRate) && g_rateCount < MaxRates - 1)
    {
        g_rates[g_rateCount] = static_cast<u32>(wantedRate);
        g_rate = g_rateCount;
        g_rateCount++;
    }

    g_rates[g_rateCount] = UnlimitedRate;
    if (wantedRate == static_cast<s32>(UnlimitedRate))
    {
        g_rate = g_rateCount;
    }
    g_rateCount++;

    for (u32 index = 0; index < g_rateCount; index++)
    {
        if (g_rates[index] == UnlimitedRate)
        {
            std::snprintf(g_rateText[index], sizeof(g_rateText[index]), "unlimited");
        }
        else
        {
            std::snprintf(g_rateText[index], sizeof(g_rateText[index]), "%u", g_rates[index]);
        }
        g_rateNames[index] = g_rateText[index];
    }

    // The resolution scale: the presets, the file's when it is none of them
    const float wantedScale = settings.scale > 0.0f ? settings.scale : 1.0f;
    g_scaleCount = 0;
    g_scale = 0;
    current = false;
    for (const float preset : ScalePresets)
    {
        g_scales[g_scaleCount] = preset;
        if (std::fabs(preset - wantedScale) < 0.001f)
        {
            g_scale = g_scaleCount;
            current = true;
        }
        g_scaleCount++;
    }

    if (!current && g_scaleCount < MaxScales)
    {
        g_scales[g_scaleCount] = wantedScale;
        g_scale = g_scaleCount;
        g_scaleCount++;
    }

    for (u32 index = 0; index < g_scaleCount; index++)
    {
        std::snprintf(g_scaleText[index], sizeof(g_scaleText[index]), "%gx", static_cast<double>(g_scales[index]));
        g_scaleNames[index] = g_scaleText[index];
    }

    // The camera's reach: the presets, the file's when it is none of them
    const float wantedReach = settings.drawDistance > 0.0f ? settings.drawDistance : 1.0f;
    g_drawDistanceCount = 0;
    g_drawDistance = 0;
    current = false;
    for (const float preset : DrawDistancePresets)
    {
        g_drawDistances[g_drawDistanceCount] = preset;
        if (std::fabs(preset - wantedReach) < 0.001f)
        {
            g_drawDistance = g_drawDistanceCount;
            current = true;
        }
        g_drawDistanceCount++;
    }

    if (!current && g_drawDistanceCount < MaxDrawDistances)
    {
        g_drawDistances[g_drawDistanceCount] = wantedReach;
        g_drawDistance = g_drawDistanceCount;
        g_drawDistanceCount++;
    }

    for (u32 index = 0; index < g_drawDistanceCount; index++)
    {
        std::snprintf(g_drawDistanceText[index], sizeof(g_drawDistanceText[index]), "%gx",
                      static_cast<double>(g_drawDistances[index]));
        g_drawDistanceNames[index] = g_drawDistanceText[index];
    }

    // The modes and the aspects: the file's word, or the first when it names none of them
    g_windowMode = 0;
    for (u32 index = 0; index < 3; index++)
    {
        if (std::strcmp(settings.window, WindowModeWords[index]) == 0)
        {
            g_windowMode = index;
        }
    }

    g_aspect = 0;
    for (u32 index = 0; index < 2; index++)
    {
        if (std::strcmp(settings.aspect, AspectWords[index]) == 0)
        {
            g_aspect = index;
        }
    }

    g_vsync = settings.vsync;
    g_antiAliasing = HostSettingsAntiAlias();
    // The renderer's own inputs, which the display is what changes: the scene's scale and the picture's fit
    BgfxSetSceneScale(static_cast<u32>(g_scales[g_scale] * 100.0f + 0.5f));
    BgfxSetPictureFit(g_aspect == 1);
}

void Load()
{
    if (g_loaded)
    {
        return;
    }

    g_loaded = true;
    BuildLists();
}

// The full screen mode of the size asked for, when the display has one: a full screen without a match would run at the
// display's own size
bool FullscreenMode(u32 width, u32 height, SDL_DisplayMode* mode)
{
    SDL_DisplayID display = g_window != nullptr ? SDL_GetDisplayForWindow(g_window) : SDL_GetPrimaryDisplay();
    if (display == 0)
    {
        return false;
    }

    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
    if (desktop != nullptr && static_cast<u32>(desktop->w) == width && static_cast<u32>(desktop->h) == height)
    {
        *mode = *desktop;
        return true;
    }

    return SDL_GetClosestFullscreenDisplayMode(display, static_cast<int>(width), static_cast<int>(height), 0.0f, false, mode) &&
           static_cast<u32>(mode->w) == width && static_cast<u32>(mode->h) == height;
}

// The window set to the mode and the size the settings hold. The size it ends up with is not read here: SDL's full screen call
// returns before the window server has resized it, and the size at that moment is neither the old one nor the new one.
// DisplaySync makes the swap chain the window's drawable once the window is what it will be
void ApplyWindow()
{
    if (g_window == nullptr)
    {
        return;
    }

    const Size size = g_resolutions[g_resolution];
    const bool fullscreen = g_windowMode == 2;
    if (fullscreen)
    {
        SDL_DisplayMode mode;
        SDL_SetWindowFullscreenMode(g_window, FullscreenMode(size.width, size.height, &mode) ? &mode : nullptr);
        SDL_SetWindowFullscreen(g_window, true);
        g_settleFrames = 0;
        return;
    }

    SDL_SetWindowFullscreen(g_window, false);
    SDL_SetWindowBordered(g_window, g_windowMode == 0);
    SDL_SetWindowSize(g_window, static_cast<int>(size.width), static_cast<int>(size.height));
    SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    g_settleFrames = 0;
}

// The settings file told what the display now is, so the next tick writes it
void Keep()
{
    const Size size = g_resolutions[g_resolution];
    HostDisplaySettings settings;
    settings.width = size.width;
    settings.height = size.height;
    settings.window = WindowModeWords[g_windowMode < 3 ? g_windowMode : 0];
    settings.vsync = g_vsync;
    settings.aspect = AspectWords[g_aspect < 2 ? g_aspect : 0];
    settings.scale = g_scales[g_scale];
    settings.drawDistance = g_drawDistances[g_drawDistance];
    HostSettingsSetDisplay(settings);
}

// The picture's rate handed over: Platform::Time paces the display's frames with it, 0 meaning no pacing
void ApplyRate()
{
    Platform::Time::SetRates(Platform::Time::FrameRate(), static_cast<s32>(g_rates[g_rate]));
    HostSettingsSetRenderRate(static_cast<s32>(g_rates[g_rate]));
}
}

bool Platform::Display::Presentable()
{
    return true;
}

u32 Platform::Display::ResolutionCount()
{
    Load();
    return g_resolutionCount;
}

const char* const* Platform::Display::ResolutionNames()
{
    Load();
    return g_resolutionNames;
}

u32 Platform::Display::Resolution()
{
    Load();
    return g_resolution;
}

// The window resized to the resolution, and the change recorded. A full screen has its mode switched when the display has one
// of that size; on a compositor that owns the mode (Wayland's) the picture stays the display's, and the recorded resolution is
// the one the window returns to
void Platform::Display::SetResolution(u32 index)
{
    Load();
    if (index >= g_resolutionCount || index == g_resolution)
    {
        return;
    }

    g_resolution = index;
    ApplyWindow();
    Keep();
}

u32 Platform::Display::WindowModeCount()
{
    return 3;
}

const char* const* Platform::Display::WindowModeNames()
{
    return WindowModeWords;
}

u32 Platform::Display::WindowMode()
{
    Load();
    return g_windowMode;
}

// The window's mode, the size it is put at coming from the resolution row
void Platform::Display::SetWindowMode(u32 index)
{
    Load();
    if (index >= 3 || index == g_windowMode)
    {
        return;
    }

    g_windowMode = index;
    ApplyWindow();
    Keep();
    std::fprintf(stderr, "sdl3: window %s\n", WindowModeWords[g_windowMode]);
}

u32 Platform::Display::FrameRateCount()
{
    Load();
    return g_rateCount;
}

const char* const* Platform::Display::FrameRateNames()
{
    Load();
    return g_rateNames;
}

u32 Platform::Display::FrameRate()
{
    Load();
    return g_rate;
}

// The picture's rate: the display's own, the logic's untouched (the game keeps stepping at the disc's field rate)
void Platform::Display::SetFrameRateIndex(u32 index)
{
    Load();
    if (index >= g_rateCount || index == g_rate)
    {
        return;
    }

    g_rate = index;
    ApplyRate();
    Keep();
    std::fprintf(stderr, "sdl3: display %s\n", g_rateNames[index]);
}

bool Platform::Display::Vsync()
{
    Load();
    return g_vsync;
}

void Platform::Display::SetVsync(bool on)
{
    Load();
    if (on == g_vsync)
    {
        return;
    }

    g_vsync = on;
    BgfxResetSwapchain(BgfxBackbufferWidth(), BgfxBackbufferHeight(), g_vsync);
    Keep();
}

u32 Platform::Display::AspectCount()
{
    return 2;
}

const char* const* Platform::Display::AspectNames()
{
    return AspectWords;
}

u32 Platform::Display::Aspect()
{
    Load();
    return g_aspect;
}

void Platform::Display::SetAspect(u32 index)
{
    Load();
    if (index >= 2 || index == g_aspect)
    {
        return;
    }

    g_aspect = index;
    BgfxSetPictureFit(index == 1);
    Keep();
    std::fprintf(stderr, "sdl3: display aspect %s\n", AspectWords[index]);
}

u32 Platform::Display::ScaleCount()
{
    Load();
    return g_scaleCount;
}

const char* const* Platform::Display::ScaleNames()
{
    Load();
    return g_scaleNames;
}

u32 Platform::Display::Scale()
{
    Load();
    return g_scale;
}

void Platform::Display::SetScale(u32 index)
{
    Load();
    if (index >= g_scaleCount || index == g_scale)
    {
        return;
    }

    g_scale = index;
    BgfxSetSceneScale(static_cast<u32>(g_scales[g_scale] * 100.0f + 0.5f));
    Keep();
    std::fprintf(stderr, "sdl3: resolution scale %s (%ux%u)\n", g_scaleNames[index], BgfxSceneWidth(), BgfxSceneHeight());
}

u32 Platform::Display::SceneWidth()
{
    Load();
    return BgfxSceneWidth();
}

u32 Platform::Display::SceneHeight()
{
    Load();
    return BgfxSceneHeight();
}

u32 Platform::Display::DrawDistanceCount()
{
    Load();
    return g_drawDistanceCount;
}

const char* const* Platform::Display::DrawDistanceNames()
{
    Load();
    return g_drawDistanceNames;
}

u32 Platform::Display::DrawDistance()
{
    Load();
    return g_drawDistance;
}

// The reach the next frame's projection and culling take: nothing to rebuild, both read it as they are built
void Platform::Display::SetDrawDistance(u32 index)
{
    Load();
    if (index >= g_drawDistanceCount || index == g_drawDistance)
    {
        return;
    }

    g_drawDistance = index;
    Keep();
    std::fprintf(stderr, "sdl3: pop-in distance %s (%.0f units, load walls %.0f, chunks %u links deep)\n",
                 g_drawDistanceNames[index], static_cast<double>(1500.0f * g_drawDistances[index]),
                 static_cast<double>(2.0f * g_drawDistances[index]), Platform::Stream::ChunkDepth());
}

u32 Platform::Display::AntiAliasCount()
{
    return 3;
}

const char* const* Platform::Display::AntiAliasNames()
{
    return AntiAliasWords;
}

AntiAlias Platform::Display::AntiAliasing()
{
    Load();
    return g_antiAliasing;
}

// The post passes the frame is cleaned with: their targets are made for the mode, so a change drops them and the next frame
// makes the new mode's
void Platform::Display::SetAntiAliasing(AntiAlias mode)
{
    Load();
    if (mode == g_antiAliasing)
    {
        return;
    }

    g_antiAliasing = mode;
    BgfxSetAntiAliasing(mode);
    Platform::Graphics::HostInvalidateAntiAliasing();
    HostSettingsSetAntiAlias(mode);
    Keep();
    std::fprintf(stderr, "sdl3: anti-aliasing %s\n", AntiAliasWords[static_cast<u32>(mode)]);
}

u32 DisplayWindowWidth()
{
    return WantedSize().width;
}

u32 DisplayWindowHeight()
{
    return WantedSize().height;
}

// The window is opened windowed (or borderless) even when the settings ask for a full screen: SDL's creation-time full screen
// stays at the size it was created with (its full screen call would then be a no-op), so the mode is applied afterwards, by
// DisplayAttach, the way the options screen applies it
SDL_WindowFlags DisplayWindowFlags()
{
    return SDL_WINDOW_RESIZABLE | (Platform::Display::WindowMode() == 1 ? SDL_WINDOW_BORDERLESS : 0);
}

// The window is the display's from here: the mode the settings asked for is applied to it (the size came with the creation),
// and the renderer is told the scale and the fit the file holds
void DisplayAttach(SDL_Window* window)
{
    g_window = window;
    // The desktop the window is on decides which resolutions are offered, so the lists are the window's
    if (g_loaded)
    {
        const u32 resolution = g_resolution;
        const u32 windowMode = g_windowMode;
        const u32 rate = g_rate;
        const u32 scale = g_scale;
        g_loaded = false;
        BuildLists();
        g_resolution = resolution < g_resolutionCount ? resolution : 0;
        g_windowMode = windowMode;
        g_rate = rate < g_rateCount ? rate : 0;
        g_scale = scale < g_scaleCount ? scale : 0;
    }

    Load();
    ApplyWindow();
    // What the picture is, so a log says which of the file's display lines took: the window's size and mode, vsync, the shape
    // the picture keeps and the size the scene is drawn at
    char fullscreen[32] = "";
    if (g_windowMode == 2)
    {
        SDL_DisplayID display = SDL_GetDisplayForWindow(window);
        const SDL_DisplayMode* mode = display != 0 ? SDL_GetDesktopDisplayMode(display) : nullptr;
        if (mode != nullptr)
        {
            std::snprintf(fullscreen, sizeof(fullscreen), " at the display's %dx%d", mode->w, mode->h);
        }
    }

    const Size size = g_resolutions[g_resolution];
    std::fprintf(stderr, "sdl3: window %ux%u %s%s, vsync %s, picture %s, scale %s (scene %ux%u, antialias %s)\n", size.width,
                 size.height, WindowModeWords[g_windowMode], fullscreen, g_vsync ? "on" : "off", AspectWords[g_aspect],
                 g_scaleNames[g_scale], BgfxSceneWidth(), BgfxSceneHeight(),
                 AntiAliasWords[static_cast<u32>(g_antiAliasing)]);
    std::fprintf(stderr, "sdl3: pop-in distance %s (%.0f units, load walls %.0f, chunks %u links deep)\n",
                 g_drawDistanceNames[g_drawDistance], static_cast<double>(1500.0f * g_drawDistances[g_drawDistance]),
                 static_cast<double>(2.0f * g_drawDistances[g_drawDistance]), Platform::Stream::ChunkDepth());
}

// Once a frame: the window's drawable against the swap chain's. A full screen mode, a resolution the row set and a window the
// player dragged all arrive here, so the swap chain follows the window and never the other way round
void DisplaySync()
{
    if (g_window == nullptr || !g_loaded)
    {
        return;
    }

    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(g_window, &width, &height);
    if (width <= 0 || height <= 0)
    {
        return;
    }

    // What the frame is drawn at: the window's drawable, except on a full screen, which is the display's. A compositor sizes a
    // full screen surface itself and Wayland leaves the window reporting the size it had before (the surface is the
    // compositor's, the buffer is scaled into it), so the display's own mode is what the picture is drawn at
    u32 drawWidth = static_cast<u32>(width);
    u32 drawHeight = static_cast<u32>(height);
    if (g_windowMode == 2)
    {
        SDL_DisplayID display = SDL_GetDisplayForWindow(g_window);
        const SDL_DisplayMode* mode = display != 0 ? SDL_GetDesktopDisplayMode(display) : nullptr;
        if (mode != nullptr && mode->w > 0 && mode->h > 0)
        {
            drawWidth = static_cast<u32>(mode->w);
            drawHeight = static_cast<u32>(mode->h);
        }
    }

    if (drawWidth != BgfxBackbufferWidth() || drawHeight != BgfxBackbufferHeight())
    {
        BgfxResetSwapchain(drawWidth, drawHeight, g_vsync);
    }

    // The resolution row is the window's own size: a full screen's is the display's, and recording it would open the next run at
    // the desktop's size instead of the player's
    if (g_windowMode == 2)
    {
        g_settleFrames = 0;
        return;
    }

    if (static_cast<u32>(width) == g_resolutions[g_resolution].width &&
        static_cast<u32>(height) == g_resolutions[g_resolution].height)
    {
        g_settleFrames = 0;
        return;
    }

    // Another size: the player's when it holds for a few frames (a drag crosses sizes one frame at a time), the window server's
    // leftover when it doesn't
    if (static_cast<u32>(width) == g_settleWidth && static_cast<u32>(height) == g_settleHeight)
    {
        if (g_settleFrames < SettleFrames)
        {
            g_settleFrames++;
        }
    }
    else
    {
        g_settleWidth = static_cast<u32>(width);
        g_settleHeight = static_cast<u32>(height);
        g_settleFrames = 1;
    }

    if (g_settleFrames < SettleFrames)
    {
        return;
    }

    // The player's size: the current resolution becomes it (so the menu and the file tell it, and the next start opens at it)
    g_resolutions[g_resolution] = Size{g_settleWidth, g_settleHeight};
    std::snprintf(g_resolutionText[g_resolution], sizeof(g_resolutionText[g_resolution]), "%u x %u", g_settleWidth,
                  g_settleHeight);
    Keep();
}
