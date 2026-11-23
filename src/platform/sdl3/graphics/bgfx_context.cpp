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

// The order the port starts a renderer in when it is not told which (no TWINSANITY_RENDERER, or "auto"), best first:
//   Windows  vulkan, direct3d12, direct3d11, opengl
//   Linux    vulkan, opengl
//   Android  opengles (its GL; Vulkan on Android is untested here)
//   macOS    opengl (Metal has no blobs yet)
//   web      webgl/opengles
// Vulkan leads everywhere it exists: on the Windows test machine it drew the same scene at about four times OpenGL's
// frame rate, which is the difference between the two backends' submit cost. OpenGL is the last resort and is still
// tried, because it is the one renderer every driver has. The chain is the port's own rather than bgfx's: bgfx::init
// takes one renderer and a bool, so a machine without a Vulkan driver is walked down the list by BgfxInit instead
// (bgfx's own examples retry an init the same way). Each attempt is a real init, so a renderer that starts is the one
// that drew -- the log names it
void DefaultRendererChain(bgfx::RendererType::Enum* chain, u32* count)
{
    u32 n = 0;
#if defined(__EMSCRIPTEN__)
    // WebGPU first: it measured about ten frames a second more than WebGL on a phone, and WebGL stalls on its first frame
    // there. Browsers without WebGPU (or a bgfx built without that renderer) fail the first attempt and start on WebGL
    chain[n++] = bgfx::RendererType::WebGPU;
    chain[n++] = bgfx::RendererType::OpenGLES;
#elif defined(__ANDROID__)
    chain[n++] = bgfx::RendererType::OpenGLES;
#elif defined(_WIN32)
    chain[n++] = bgfx::RendererType::Vulkan;
#if defined(TWINSANITY_HAVE_DXBC)
    chain[n++] = bgfx::RendererType::Direct3D12;
    chain[n++] = bgfx::RendererType::Direct3D11;
#endif
    chain[n++] = bgfx::RendererType::OpenGL;
#else
    // Linux' and the other X11/Wayland platforms': Vulkan through bgfx's own backend, then GL
    chain[n++] = bgfx::RendererType::Vulkan;
    chain[n++] = bgfx::RendererType::OpenGL;
#endif
    *count = n;
}

// Whether TWINSANITY_RENDERER named one renderer, which is then tried alone ("auto" asks for the chain, not a name)
bool RendererNamed()
{
    const char* requested = std::getenv("TWINSANITY_RENDERER");
    if (requested == nullptr || *requested == '\0')
    {
        return false;
    }
    return std::strcmp(requested, "auto") != 0;
}

// The renderers each platform has, so a build carries only its own (and tools/build_shaders.py's platform backends hold
// the shader side of the same sets):
//   Linux    opengl, vulkan
//   Windows  opengl, vulkan, direct3d11, direct3d12
//   Android  opengles (its GL), vulkan
//   macOS    metal (no blobs yet)
//   web      webgpu, webgl/opengles
// A named renderer is tried alone, so a driver that cannot take it fails here instead of quietly rendering on another
// backend; the default order is DefaultRendererChain's. A renderer this build has no shader blobs for is refused with a
// word rather than started and failing at its first draw (HostCreateShader)
bgfx::RendererType::Enum RendererFromEnvironment(bool* fallback)
{
    *fallback = false;
    const char* requested = std::getenv("TWINSANITY_RENDERER");
#if defined(__EMSCRIPTEN__)
    // WebGPU is the web's renderer where it exists (the chain above puts it first, WebGL second for a browser without it),
    // and GL ES is what a page names as webgl/opengles. Neither is named here for the default: the chain decides
    if (requested == nullptr || *requested == '\0')
    {
        return bgfx::RendererType::WebGPU;
    }
    if (std::strcmp(requested, "opengl") == 0 || std::strcmp(requested, "webgl") == 0 ||
        std::strcmp(requested, "opengles") == 0 || std::strcmp(requested, "gles") == 0)
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

namespace
{
// bgfx's own log, to the terminal: a shader that fails to compile says why here, and a page's log shows it
class BgfxLog : public bgfx::CallbackI
{
public:
    void fatal(const char* filePath, std::uint16_t line, bgfx::Fatal::Enum code, const char* message) override
    {
        (void)code;
        std::fprintf(stderr, "bgfx: %s (%s:%u)\n", message, filePath, static_cast<unsigned>(line));
        std::abort();
    }

    void traceVargs(const char* filePath, std::uint16_t line, const char* format, va_list args) override
    {
        (void)filePath;
        (void)line;
        std::vfprintf(stderr, format, args);
    }

    void profilerBegin(const char*, std::uint32_t, const char*, std::uint16_t) override {}
    void profilerBeginLiteral(const char*, std::uint32_t, const char*, std::uint16_t) override {}
    void profilerEnd() override {}
    std::uint32_t cacheReadSize(std::uint64_t) override { return 0; }
    bool cacheRead(std::uint64_t, void*, std::uint32_t) override { return false; }
    void cacheWrite(std::uint64_t, const void*, std::uint32_t) override {}
    void screenShot(const char*, std::uint32_t, std::uint32_t, std::uint32_t, bgfx::TextureFormat::Enum,
                    const void*, std::uint32_t, bool) override {}
    void captureBegin(std::uint32_t, std::uint32_t, std::uint32_t, bgfx::TextureFormat::Enum, bool) override {}
    void captureEnd() override {}
    void captureFrame(const void*, std::uint32_t) override {}
};

BgfxLog g_bgfxLog;
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
    init.callback = &g_bgfxLog;
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
    // The frame's transient vertex space: what the rigid draws' instance data, the particles, the decals and the 2D batches go
    // through before bgfx copies them to the GPU once a frame. bgfx orphans all of it every frame (delete, create, then copy
    // what the frame used), so its size is a per-frame cost and not just a floor: a picture that spends 300 KB a frame was
    // paying 175 us for the 2 MB, the heaviest frame measured (a boss arena, 2203 rigid draws) leaving 1749 of 2048 KB free.
    // Three of bgfx's defaults' worth of slack were not buying anything. 768 KB is two and a half times what any frame measured
    // has asked for; bgfx grows the buffer on demand past this size now rather than capping it, which is the better of the two,
    // and every draw still checks the space it is handed before writing into it (the frame log's `tfree ... least ...` says how
    // close a frame came to the size it started with)
    init.limits.minTransientVbSize = 768 << 10;

#if defined(__EMSCRIPTEN__)
    // The page's canvas: bgfx's WebGL backend takes a context (made on the canvas its swap chain names) and its WebGPU one
    // the canvas itself, both by the selector the shell page gives the element
    init.platformData.type = bgfx::NativeWindowHandleType::Default;
    init.swapChain.nwh = const_cast<char*>("#canvas");
#elif defined(_WIN32)
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

    // The renderers tried in turn. A name from the environment is the whole list -- a driver that cannot take it fails
    // here rather than quietly rendering on another backend -- and otherwise the platform's chain runs best first, so a
    // machine with no Vulkan driver still starts on Direct3D, and one with neither on OpenGL
    bgfx::RendererType::Enum chain[4] = {};
    u32 chainCount = 0;
    if (RendererNamed())
    {
        chain[0] = init.type;
        chainCount = 1;
    }
    else
    {
        DefaultRendererChain(chain, &chainCount);
    }

    bool started = false;
    for (u32 attempt = 0; attempt < chainCount && !started; attempt++)
    {
        init.type = chain[attempt];
        init.fallback = false;
        started = bgfx::init(init);
        if (!started)
        {
            std::fprintf(stderr, "bgfx: %s did not start%s\n", bgfx::getRendererName(init.type),
                         attempt + 1 < chainCount ? ", trying the next renderer" : "");
        }
    }
    if (!started)
    {
        std::fprintf(stderr, "bgfx: bgfx::init failed for every renderer this build can start\n");
        return false;
    }

    // The backend actually running (TWINSANITY_RENDERER may have been unset or "auto"), so a log says which one drew,
    // with the clip-space conventions the host's shaders and render-target sampling have to follow
    const bgfx::Caps* caps = bgfx::getCaps();
    std::fprintf(stderr, "bgfx: renderer %s (NDC depth [%d, 1], origin %s left)\n",
                 bgfx::getRendererName(bgfx::getRendererType()), caps->homogeneousDepth ? -1 : 0,
                 caps->originBottomLeft ? "bottom" : "top");
    // The transient vertex space the frame's instance data, particles, decals and 2D batches share (see the init's limit): a run
    // that ever outgrew it loses the draws that no longer fit, so the size is in the log beside the renderer
    std::fprintf(stderr, "bgfx: transient vertex space %u KB\n", bgfx::getCaps()->limits.minTransientVbSize >> 10);
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
