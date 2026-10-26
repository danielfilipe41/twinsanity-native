// Bgfx context: initializes bgfx on an SDL3 window's native handle, clears and presents
// frames. SDL3 retired SDL_GetWindowWMInfo; the native handles come from SDL_GetWindowProperties instead:
// X11's display + window (what bgfx's EGL backend wants as ndt/nwh) or Wayland's display + surface (bgfx wraps
// the surface in a wl_egl_window itself when platformData.type is Wayland).
#include "bgfx_context.h"

#include "common.h"

#include <bgfx/bgfx.h>

#include <cstdio>

namespace
{
// Backbuffer clear in its own view 28: not 0, which sky.cpp's sky pass (the PS2 Sky bucket, drawn before the opaque
// bucket at view 2) clears over every frame, so the clear never landed. 27 is the Movies bucket (movie.cpp's
// MovieView), so it sits past the buckets instead, ahead of the composite at 31.
constexpr bgfx::ViewId ClearView = 28;
// The debug clear color: opaque magenta (0xRRGGBBAA, bgfx's order: red is the top byte, alpha the bottom),
// impossible to mistake for game art or a black dead surface
constexpr std::uint32_t DebugClearColor = 0xff00ffff;
// The swap chain BgfxInit requests: 960x540, entry.cpp's window size
constexpr std::uint16_t SwapWidth = BgfxWindowWidth;
constexpr std::uint16_t SwapHeight = BgfxWindowHeight;

bool g_initialized = false;

// Sets the frame's view order. Views 0..27 are the retail buckets in their own order; the host's own passes are
// placed where the retail pass they stand in for draws: the scene fog (32), the shadow volumes' offscreen depth (33),
// the coverage pass (25) just before the resolve it feeds, and the screen effects' readback blit (34).
void OrderViews()
{
    static constexpr bgfx::ViewId Order[] = {
        0,  1,  2,  3,  4,  32,
        5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
        33, 25, 21,
        22, 23, 24,
        26, 27,
        34, 28, 30, 31, 29,
    };
    static bool ordered = false;
    if (ordered)
    {
        return;
    }

    ordered = true;
    bgfx::setViewOrder(0, static_cast<bgfx::ViewId>(sizeof(Order) / sizeof(Order[0])), Order);
}
}

bool BgfxInit(SDL_Window* window)
{
    if (g_initialized)
    {
        return true;
    }
    if (window == nullptr)
    {
        std::fprintf(stderr, "bgfx: BgfxInit got a null window\n");
        return false;
    }

    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    if (props == 0)
    {
        std::fprintf(stderr, "bgfx: SDL_GetWindowProperties failed: %s\n", SDL_GetError());
        return false;
    }

    bgfx::Init init;
    init.type = bgfx::RendererType::OpenGL;
    init.reset = BGFX_RESET_NONE;
    init.fallback = false;
    init.swapChain.width = SwapWidth;
    init.swapChain.height = SwapHeight;

    void* xDisplay = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    std::int64_t xWindow = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    void* wlDisplay = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
    void* wlSurface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    if (xDisplay != nullptr && xWindow != 0)
    {
        init.platformData.type = bgfx::NativeWindowHandleType::Default;
        init.swapChain.ndt = xDisplay;
        init.swapChain.nwh = reinterpret_cast<void*>(static_cast<std::uintptr_t>(xWindow));
    }
    else if (wlDisplay != nullptr && wlSurface != nullptr)
    {
        init.platformData.type = bgfx::NativeWindowHandleType::Wayland;
        init.swapChain.ndt = wlDisplay;
        init.swapChain.nwh = wlSurface;
    }
    else
    {
        std::fprintf(stderr, "bgfx: window has neither X11 nor Wayland native handles\n");
        return false;
    }

    if (!bgfx::init(init))
    {
        std::fprintf(stderr, "bgfx: bgfx::init failed\n");
        return false;
    }

    bgfx::reset(BGFX_RESET_NONE);
    bgfx::setDebug(BGFX_DEBUG_TEXT);
    bgfx::setViewClear(ClearView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, DebugClearColor);
    bgfx::setViewRect(ClearView, 0, 0, SwapWidth, SwapHeight);
    // The frame's order: the retail buckets in their own order, with the host's own passes placed among them
    OrderViews();
    g_initialized = true;
    return true;
}

void BgfxFrame()
{
    if (!g_initialized)
    {
        return;
    }
    bgfx::touch(ClearView);
    bgfx::frame();
}

void BgfxShutdown()
{
    if (!g_initialized)
    {
        return;
    }
    bgfx::shutdown();
    g_initialized = false;
}

void BgfxSwapSize(std::uint16_t* width, std::uint16_t* height)
{
    if (width != nullptr)
    {
        *width = SwapWidth;
    }
    if (height != nullptr)
    {
        *height = SwapHeight;
    }
}
