// A host with no window of its own (the null platform, and every headless test built on it): the display's settings are kept
// for whoever asks and nothing is done with them, and the options menu shows no display rows. The sdl3 platform replaces this
// file with its own (the build takes the platform's same-named source over the null one)
#include "platform/display.h"

namespace
{
u32 g_resolution;
u32 g_windowMode;
bool g_vsync;
u32 g_aspect;
u32 g_scale;
AntiAlias g_antiAliasing = AntiAlias::Off;
}

bool Platform::Display::Presentable()
{
    return false;
}

u32 Platform::Display::ResolutionCount()
{
    return 0;
}

const char* const* Platform::Display::ResolutionNames()
{
    return nullptr;
}

u32 Platform::Display::Resolution()
{
    return g_resolution;
}

void Platform::Display::SetResolution(u32 index)
{
    g_resolution = index;
}

u32 Platform::Display::WindowModeCount()
{
    return 0;
}

const char* const* Platform::Display::WindowModeNames()
{
    return nullptr;
}

u32 Platform::Display::WindowMode()
{
    return g_windowMode;
}

void Platform::Display::SetWindowMode(u32 index)
{
    g_windowMode = index;
}

u32 Platform::Display::FrameRateCount()
{
    return 0;
}

const char* const* Platform::Display::FrameRateNames()
{
    return nullptr;
}

u32 Platform::Display::FrameRate()
{
    return 0;
}

void Platform::Display::SetFrameRateIndex(u32)
{
}

bool Platform::Display::Vsync()
{
    return g_vsync;
}

void Platform::Display::SetVsync(bool on)
{
    g_vsync = on;
}

u32 Platform::Display::AspectCount()
{
    return 0;
}

const char* const* Platform::Display::AspectNames()
{
    return nullptr;
}

u32 Platform::Display::Aspect()
{
    return g_aspect;
}

void Platform::Display::SetAspect(u32 index)
{
    g_aspect = index;
}

u32 Platform::Display::ScaleCount()
{
    return 0;
}

const char* const* Platform::Display::ScaleNames()
{
    return nullptr;
}

u32 Platform::Display::Scale()
{
    return g_scale;
}

void Platform::Display::SetScale(u32 index)
{
    g_scale = index;
}

u32 Platform::Display::SceneWidth()
{
    return 0;
}

u32 Platform::Display::SceneHeight()
{
    return 0;
}

u32 Platform::Display::AntiAliasCount()
{
    return 0;
}

const char* const* Platform::Display::AntiAliasNames()
{
    return nullptr;
}

AntiAlias Platform::Display::AntiAliasing()
{
    return g_antiAliasing;
}

void Platform::Display::SetAntiAliasing(AntiAlias mode)
{
    g_antiAliasing = mode;
}
