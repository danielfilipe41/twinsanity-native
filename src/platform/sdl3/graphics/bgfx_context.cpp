// Bgfx context: initializes bgfx on an SDL3 window's native handle, clears and presents
// frames. SDL3 retired SDL_GetWindowWMInfo; the native handles come from SDL_GetWindowProperties instead:
// X11's display + window (what bgfx's EGL backend wants as ndt/nwh) or Wayland's display + surface (bgfx wraps
// the surface in a wl_egl_window itself when platformData.type is Wayland), and on Windows the HWND alone.
#include "bgfx_context.h"

#include "common.h"

#include <bgfx/bgfx.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
// Backbuffer clear in its own view 28: not 0, which sky.cpp's sky pass (the PS2 Sky bucket, drawn before the opaque
// bucket at view 2) clears over every frame, so the clear never landed. 27 is the Movies bucket (movie.cpp's
// MovieView), so it sits past the buckets instead, ahead of the composite at 31.
constexpr bgfx::ViewId ClearView = 28;
// The debug clear color: opaque magenta (0xRRGGBBAA, bgfx's order: red is the top byte, alpha the bottom),
// impossible to mistake for game art or a black dead surface
constexpr std::uint32_t DebugClearColor = 0xff00ffff;
// The swap chain's size, which is the window's drawable: BgfxInit takes it from the caller (0: the design's) and
// BgfxResetSwapchain follows the window afterwards
u32 g_backbufferWidth = static_cast<u32>(BgfxWindowWidth);
u32 g_backbufferHeight = static_cast<u32>(BgfxWindowHeight);
bool g_vsync = false;
SDL_Window* g_window = nullptr;
// The scene target's size in percent of the design's, and whether the picture is fitted in the window
u32 g_scenePercent = 100;
bool g_pictureFit = false;

// The design's size times a percentage, rounded to even pixels: a half-pixel scene would put the post passes' texel offsets
// between texels
u32 ScaledDimension(int design, u32 percent)
{
    u32 size = static_cast<u32>(static_cast<float>(design) * static_cast<float>(percent) * 0.01f + 0.5f);
    if (size != 0 && (size & 1) != 0)
    {
        size++;
    }

    return size;
}

// The renderers each platform has, so a build carries only its own (and tools/build_shaders.py's platform backends hold
// the shader side of the same sets):
//   Linux    opengl (the default), vulkan
//   Windows  opengl (the default), vulkan, direct3d11, direct3d12
//   Android  opengles (its GL, the default), vulkan
//   macOS    metal (no blobs yet)
//   web      webgpu, webgl/opengles (no blobs yet)
// A named renderer is tried alone, so a driver that cannot take it fails here instead of quietly rendering on another
// backend; "auto" leaves the choice (and the fallback when it fails) to bgfx. A renderer this build has no shader blobs
// for is refused with a word rather than started and failing at its first draw (HostCreateShader)
bgfx::RendererType::Enum RendererFromEnvironment(bool* fallback)
{
    *fallback = false;
    const char* requested = std::getenv("TWINSANITY_RENDERER");
#if defined(__EMSCRIPTEN__)
    // The web's GL is WebGL (bgfx's GL ES renderer), and its other renderer is WebGPU
    if (requested == nullptr || *requested == '\0' || std::strcmp(requested, "opengl") == 0 ||
        std::strcmp(requested, "webgl") == 0 || std::strcmp(requested, "opengles") == 0 ||
        std::strcmp(requested, "gles") == 0)
    {
        return bgfx::RendererType::OpenGLES;
    }
    if (std::strcmp(requested, "webgpu") == 0)
    {
        return bgfx::RendererType::WebGPU;
    }
#elif defined(__ANDROID__)
    // Android's GL is GL ES: ES has no desktop GLSL, so the default here is the ES renderer
    if (requested == nullptr || *requested == '\0' || std::strcmp(requested, "opengl") == 0 ||
        std::strcmp(requested, "opengles") == 0 || std::strcmp(requested, "gles") == 0)
    {
        return bgfx::RendererType::OpenGLES;
    }
#else
    if (requested == nullptr || *requested == '\0' || std::strcmp(requested, "opengl") == 0)
    {
        return bgfx::RendererType::OpenGL;
    }
    if (std::strcmp(requested, "opengles") == 0 || std::strcmp(requested, "gles") == 0)
    {
        std::fprintf(stderr, "bgfx: TWINSANITY_RENDERER=%s is Android's renderer; this build's GL is desktop OpenGL\n",
                     requested);
        return bgfx::RendererType::OpenGL;
    }
#endif
#if !defined(__EMSCRIPTEN__)
    // Vulkan is every native platform's second renderer (the web has none)
    if (std::strcmp(requested, "vulkan") == 0)
    {
        return bgfx::RendererType::Vulkan;
    }
#endif
#if defined(TWINSANITY_HAVE_DXBC)
    // Windows' two Direct3D renderers (both take the DXBC blobs host_shaders.cpp carries)
    if (std::strcmp(requested, "direct3d11") == 0)
    {
        return bgfx::RendererType::Direct3D11;
    }
    if (std::strcmp(requested, "direct3d12") == 0)
    {
        return bgfx::RendererType::Direct3D12;
    }
#else
    // No DXBC blobs in this build: naming a Direct3D renderer says so here instead of starting one whose first draw fails
    if (std::strcmp(requested, "direct3d11") == 0 || std::strcmp(requested, "direct3d12") == 0)
    {
        std::fprintf(stderr,
                     "bgfx: TWINSANITY_RENDERER=%s needs the Direct3D shader blobs, which this build has none of (see "
                     "docs/BUILD_WINDOWS.md); using OpenGL\n",
                     requested);
        return bgfx::RendererType::OpenGL;
    }
#endif
#if defined(__APPLE__)
    // Same for Metal: bgfx would build the renderer, but no MSL blobs are generated yet
    if (std::strcmp(requested, "metal") == 0)
    {
        std::fprintf(stderr, "bgfx: TWINSANITY_RENDERER=metal has no shader blobs in this build yet\n");
        return bgfx::RendererType::OpenGL;
    }
#endif
    if (std::strcmp(requested, "auto") == 0)
    {
        *fallback = true;
        return bgfx::RendererType::Count;
    }

    std::fprintf(stderr, "bgfx: TWINSANITY_RENDERER=%s is not a renderer name, using OpenGL\n", requested);
    return bgfx::RendererType::OpenGL;
}

// The anti-aliasing every frame draws with: BgfxSetAntiAliasing puts the settings file's choice here before BgfxInit, and
// nothing sets it in a headless proof, which draws as it always has
AntiAlias g_antiAliasing = AntiAlias::Off;

bool g_initialized = false;

// Sets the frame's view order. Views 0..27 are the retail buckets in their own order; the host's own passes are
// placed where the retail pass they stand in for draws: the scene fog (32), the shadow volumes' offscreen depth (33),
// the coverage pass (25) just before the resolve it feeds, and the screen effects' readback blit (34). The antialiasing
// passes (35..37) follow the composite at 31, which is the frame they clean up.
void OrderViews()
{
    static constexpr bgfx::ViewId Order[] = {
        0,  1,  2,  3,  4,  32,
        5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
        33, 25, 21,
        22, 23, 24,
        26, 27,
        34, 28, 30, 31, 35, 36, 37, 29,
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

AntiAlias HostAntiAliasing()
{
    return g_antiAliasing;
}

void BgfxSetAntiAliasing(AntiAlias mode)
{
    g_antiAliasing = mode;
}

bool BgfxInit(SDL_Window* window, int width, int height, bool vsync)
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
    bool fallback = false;
    init.type = RendererFromEnvironment(&fallback);
    init.reset = vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE;
    init.fallback = fallback;
    g_window = window;
    g_vsync = vsync;
    g_backbufferWidth = width > 0 ? static_cast<u32>(width) : static_cast<u32>(BgfxWindowWidth);
    g_backbufferHeight = height > 0 ? static_cast<u32>(height) : static_cast<u32>(BgfxWindowHeight);
    init.swapChain.width = static_cast<std::uint16_t>(g_backbufferWidth);
    init.swapChain.height = static_cast<std::uint16_t>(g_backbufferHeight);

#if defined(_WIN32)
    // Windows' backends (WGL, Direct3D, Vulkan) take the window as its HWND, with no display handle beside it
    void* hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (hwnd == nullptr)
    {
        std::fprintf(stderr, "bgfx: window has no Win32 HWND: %s\n", SDL_GetError());
        return false;
    }
    init.platformData.type = bgfx::NativeWindowHandleType::Default;
    init.swapChain.nwh = hwnd;
#else
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
#endif

    if (!bgfx::init(init))
    {
        std::fprintf(stderr, "bgfx: bgfx::init failed\n");
        return false;
    }

    // The backend actually running (TWINSANITY_RENDERER may have been unset or "auto"), so a log says which one drew,
    // with the clip-space conventions the host's shaders and render-target sampling have to follow
    const bgfx::Caps* caps = bgfx::getCaps();
    std::fprintf(stderr, "bgfx: renderer %s (NDC depth [%d, 1], origin %s left)\n",
                 bgfx::getRendererName(bgfx::getRendererType()), caps->homogeneousDepth ? -1 : 0,
                 caps->originBottomLeft ? "bottom" : "top");
    // What the frame's picture goes through, so a log says which antialiasing drew it (the settings file's antialias line)
    const AntiAlias antiAliasing = HostAntiAliasing();
    switch (antiAliasing)
    {
    case AntiAlias::Off:
        std::fprintf(stderr, "bgfx: anti-aliasing off\n");
        break;
    case AntiAlias::Fxaa:
        std::fprintf(stderr, "bgfx: anti-aliasing fxaa\n");
        break;
    case AntiAlias::Smaa:
        std::fprintf(stderr, "bgfx: anti-aliasing smaa\n");
        break;
    }

    bgfx::reset(vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE);
    bgfx::setDebug(BGFX_DEBUG_TEXT);
    bgfx::setViewClear(ClearView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, DebugClearColor);
    bgfx::setViewRect(ClearView, 0, 0, static_cast<std::uint16_t>(g_backbufferWidth),
                      static_cast<std::uint16_t>(g_backbufferHeight));
    // The frame's order: the retail buckets in their own order, with the host's own passes placed among them
    OrderViews();
    g_initialized = true;
    return true;
}

SDL_Window* BgfxWindow()
{
    return g_window;
}

u32 BgfxBackbufferWidth()
{
    return g_backbufferWidth;
}

u32 BgfxBackbufferHeight()
{
    return g_backbufferHeight;
}

// The window's drawable changed (a resize, a mode, a resolution): the swap chain follows it, and the clear view with it. The
// scene target and the post passes are the host's own and are sized from the scene's, so nothing else moves
void BgfxResetSwapchain(u32 width, u32 height, bool vsync)
{
    if (!g_initialized || width == 0 || height == 0)
    {
        return;
    }

    if (width == g_backbufferWidth && height == g_backbufferHeight && vsync == g_vsync)
    {
        return;
    }

    g_backbufferWidth = width;
    g_backbufferHeight = height;
    g_vsync = vsync;
    // The swap chain's own size: this bgfx's reset takes the flags and a chain, not the two numbers, and a field left empty
    // keeps the main window's (the handles, the formats)
    bgfx::SwapChain chain;
    chain.width = width;
    chain.height = height;
    bgfx::reset(vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE, &chain);
    bgfx::setViewRect(ClearView, 0, 0, static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height));
    std::fprintf(stderr, "bgfx: backbuffer %ux%u, vsync %s\n", width, height, vsync ? "on" : "off");
}

void BgfxSetSceneScale(u32 percent)
{
    g_scenePercent = percent != 0 ? percent : 100;
}

u32 BgfxSceneWidth()
{
    return ScaledDimension(BgfxWindowWidth, g_scenePercent);
}

u32 BgfxSceneHeight()
{
    return ScaledDimension(BgfxWindowHeight, g_scenePercent);
}

void BgfxSetPictureFit(bool fit)
{
    g_pictureFit = fit;
}

bool BgfxPictureFit()
{
    return g_pictureFit;
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
        *width = static_cast<std::uint16_t>(g_backbufferWidth);
    }
    if (height != nullptr)
    {
        *height = static_cast<std::uint16_t>(g_backbufferHeight);
    }
}
