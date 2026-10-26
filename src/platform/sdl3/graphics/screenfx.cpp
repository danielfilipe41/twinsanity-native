// Screen effects: the PS2's depth-as-palette fullscreen filter (src/platform/ps2/renderer/effects.cpp's
// FilterColours over CopyDepthBuffer).
//
// An R32F depth texture holds normalized depth; the filter shader indexes the 256-entry palette by depth * 255,
// matching retail's readback of the depth buffer's top bytes as palette indexes. Blur and waves never switch on
// in retail.
#include "host_graphics.h"
#include "draws_host.h"

#include "game/effects.h"

#include "shaders/fs_scenefog.bin.h"
#include "shaders/fs_screenfx.bin.h"
#include "shaders/vs_scenefog.bin.h"
#include "shaders/vs_screenfx.bin.h"

#include <cstdio>
#include <cstdlib>

extern "C"
{
    // The retail filter palettes (ps2/renderer/effects.cpp's g_FilterPalettes / g_FilterPaletteColours): eight
    // 256-entry CLUTs, the chunks' colourFilterPalette selecting one. The pointer table holds the eight addresses.
    extern const u32* const g_FilterPaletteColours[8] RETAIL(D_002E73C0);
    // The start-up flag that switches the whole screen-effects pass on (gamecontext sets it from the context's
    // colourFilter flag; ps2/renderer/effects.cpp reads it as its own g_ColourFilterEffect)
    extern u8 g_ColourFilterEffect RETAIL(D_00309C51);
}

namespace
{
// The scene fog's own objects: the program, its samplers and the palette texture. Separate from the screenfx
// track's proof objects below because the fog runs every frame off the live scene depth
bgfx::ProgramHandle g_fogProgram = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_fogDepthSampler = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_fogPaletteSampler = BGFX_INVALID_HANDLE;
// The fog's mode (x): the copy's byte as retail indexes it, or the packed leading byte this pass started with
bgfx::UniformHandle g_fogUniform = BGFX_INVALID_HANDLE;
bgfx::TextureHandle g_fogPalette = BGFX_INVALID_HANDLE;
u32 g_fogPaletteId = 0xFFFFFFFF;
bool g_fogMade = false;

// The CLUT's alpha is a GS blend factor, full at 128 (0x80 is 1.0), so an alpha is doubled into the window's 255.
// The colour channels are not: the retail palettes carry real 8-bit hues (the beach's palette 5 is the sunset's
// 255, 200, 120), and doubling them clamps orange to white. Only alpha needs the 7-bit-to-8-bit conversion
u8 FogAlpha(u8 alpha)
{
    u32 scaled = static_cast<u32>(alpha) << 1;
    return static_cast<u8>(scaled > 255 ? 255 : scaled);
}

void MakeFogShared()
{
    if (g_fogMade)
    {
        return;
    }
    g_fogMade = true;
    bgfx::ShaderHandle vs = bgfx::createShader(bgfx::makeRef(vs_scenefog_glsl, sizeof(vs_scenefog_glsl)));
    bgfx::ShaderHandle fs = bgfx::createShader(bgfx::makeRef(fs_scenefog_glsl, sizeof(fs_scenefog_glsl)));
    g_fogProgram = bgfx::createProgram(vs, fs, true);
    g_fogDepthSampler = bgfx::createUniform("s_depth", bgfx::UniformType::Sampler);
    g_fogPaletteSampler = bgfx::createUniform("s_palette", bgfx::UniformType::Sampler);
    g_fogUniform = bgfx::createUniform("u_fog", bgfx::UniformType::Vec4);
}

// The chunk's filter palette uploaded (its CLUT the fog shader samples by depth): the retail table's entry for the
// game's screen-effect id, kept until the id changes
void MakeFogPalette(u32 paletteId)
{
    MakeFogShared();
    if (paletteId >= 8)
    {
        paletteId = 0;
    }
    if (bgfx::isValid(g_fogPalette) && paletteId == g_fogPaletteId)
    {
        return;
    }
    const u32* source = g_FilterPaletteColours[paletteId];
    if (source == nullptr)
    {
        return;
    }
    u32 colours[Platform::Graphics::ScreenFxPaletteColours];
    for (u32 entry = 0; entry < Platform::Graphics::ScreenFxPaletteColours; entry++)
    {
        u32 word = source[entry];
        u32 r = static_cast<u8>(word & 0xFF);
        u32 g = static_cast<u8>((word >> 8) & 0xFF);
        u32 b = static_cast<u8>((word >> 16) & 0xFF);
        u32 a = FogAlpha(static_cast<u8>((word >> 24) & 0xFF));
        // bgfx's RGBA8 memory is R lowest, the same order the CLUT's word holds
        colours[entry] = r | g << 8 | b << 16 | a << 24;
    }
    // TS_DIAG=1: which palette the live chunk selected and how its far/near ends converted, so a flat or white filter
    // can be told from a wrong palette or a wrong channel convention without a debugger. The ramp's far end is the
    // pixel at the far plane and its near end the one at the copy's z test (about 26 units), so these six entries are
    // what the distance sweep passes through
    if (std::getenv("TS_DIAG") != nullptr)
    {
        std::fprintf(stderr,
                     "fog: palette %u entry 0 %08X -> %08X, entry 255 %08X -> %08X (alpha doubled, rgb kept); "
                     "ramp alphas 0/32/64/128/192/255 = %u/%u/%u/%u/%u/%u, packed=%d\n",
                     paletteId, source[0], colours[0], source[255], colours[255],
                     static_cast<u8>(colours[0] >> 24), static_cast<u8>(colours[32] >> 24),
                     static_cast<u8>(colours[64] >> 24), static_cast<u8>(colours[128] >> 24),
                     static_cast<u8>(colours[192] >> 24), static_cast<u8>(colours[255] >> 24),
                     std::getenv("TS_FOGPACKED") != nullptr ? 1 : 0);
    }
    if (bgfx::isValid(g_fogPalette))
    {
        bgfx::destroy(g_fogPalette);
    }
    const bgfx::Memory* memory = bgfx::copy(colours, sizeof(colours));
    g_fogPalette = bgfx::createTexture2D(Platform::Graphics::ScreenFxPaletteColours, 1, false, 1,
                                         bgfx::TextureFormat::RGBA8,
                                         BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT |
                                             BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
                                         memory);
    g_fogPaletteId = paletteId;
}

// The filter's objects, made lazily on first use (after BgfxInit; owned by the process like the shared 2D
// objects): the program, the depth and palette samplers, the R32F depth texture (the pre-pass), the 256x1 RGBA8
// palette bank's upload texture, and the RGBA8 output target the filter draws into with its readback texture
bgfx::ProgramHandle g_program = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_depthSampler = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_paletteSampler = BGFX_INVALID_HANDLE;
bgfx::TextureHandle g_depth = BGFX_INVALID_HANDLE;
bgfx::TextureHandle g_palette = BGFX_INVALID_HANDLE;
bgfx::FrameBufferHandle g_output = BGFX_INVALID_HANDLE;
bgfx::TextureHandle g_readback = BGFX_INVALID_HANDLE;
bool g_made = false;
// The game's screen-effect id (the chunks' colourFilterPalette: which of the eight filter palettes is live),
// the bank's eight uploaded palettes, and whether the depth and the output target are live
u32 g_selectedEffect = 0;
u32 g_bank[Platform::Graphics::ScreenFxPaletteCount][Platform::Graphics::ScreenFxPaletteColours] = {};
u32 g_depthWidth = 0;
u32 g_depthHeight = 0;
bool g_depthLive = false;

void MakeShared()
{
    if (g_made)
    {
        return;
    }
    g_made = true;
    bgfx::ShaderHandle vs = bgfx::createShader(bgfx::makeRef(vs_screenfx_glsl, sizeof(vs_screenfx_glsl)));
    bgfx::ShaderHandle fs = bgfx::createShader(bgfx::makeRef(fs_screenfx_glsl, sizeof(fs_screenfx_glsl)));
    g_program = bgfx::createProgram(vs, fs, true);
    g_depthSampler = bgfx::createUniform("s_depth", bgfx::UniformType::Sampler);
    g_paletteSampler = bgfx::createUniform("s_palette", bgfx::UniformType::Sampler);
}

// A fullscreen triangle's vertex: clip-space position only (the shader derives the depth UVs from it)
struct ScreenFxVertex
{
    f32 x;
    f32 y;
    f32 z;
};

bgfx::VertexLayout ScreenFxLayout()
{
    bgfx::VertexLayout layout;
    layout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).end();
    return layout;
}
}

namespace Platform::Graphics
{
void ScreenFxUploadPalette(u32 palette, const u32* colours)
{
    MakeShared();
    if (colours == nullptr)
    {
        return;
    }
    u32 slot = palette & (ScreenFxPaletteCount - 1);
    for (u32 index = 0; index < ScreenFxPaletteColours; index++)
    {
        g_bank[slot][index] = colours[index];
    }
    // The live entry's upload: recreated point-sampled and clamped (exact texel fetches: the filter must land
    // on one palette entry, never blend two)
    if (slot == (g_selectedEffect & (ScreenFxPaletteCount - 1)))
    {
        if (bgfx::isValid(g_palette))
        {
            bgfx::destroy(g_palette);
        }
        const bgfx::Memory* memory = bgfx::copy(g_bank[slot], sizeof(g_bank[slot]));
        g_palette = bgfx::createTexture2D(ScreenFxPaletteColours, 1, false, 1, bgfx::TextureFormat::RGBA8,
                                          BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT |
                                              BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
                                          memory);
    }
}

void ScreenFxSelectEffect(u32 palette)
{
    MakeShared();
    g_selectedEffect = palette & (ScreenFxPaletteCount - 1);
    // Selecting re-uploads the bank's entry (the palette the filter samples follows the game's screen-effect id)
    ScreenFxUploadPalette(g_selectedEffect, g_bank[g_selectedEffect]);
}

u32 ScreenFxSelectedEffect()
{
    return g_selectedEffect;
}

void ScreenFxUploadDepth(u32 width, u32 height, const f32* depths)
{
    MakeShared();
    if (width == 0 || height == 0 || depths == nullptr)
    {
        return;
    }
    if (width != g_depthWidth || height != g_depthHeight)
    {
        if (bgfx::isValid(g_depth))
        {
            bgfx::destroy(g_depth);
        }
        if (bgfx::isValid(g_output))
        {
            bgfx::destroy(g_output);
        }
        if (bgfx::isValid(g_readback))
        {
            bgfx::destroy(g_readback);
        }
        g_depthWidth = width;
        g_depthHeight = height;
        g_depth = bgfx::createTexture2D(static_cast<u16>(width), static_cast<u16>(height), false, 1,
                                        bgfx::TextureFormat::R32F,
                                        BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT |
                                            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        g_output = bgfx::createFrameBuffer(static_cast<u16>(width), static_cast<u16>(height),
                                           bgfx::TextureFormat::RGBA8);
        g_readback = bgfx::createTexture2D(
            static_cast<u16>(width), static_cast<u16>(height), false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
                BGFX_SAMPLER_MIP_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    }
    const bgfx::Memory* memory = bgfx::copy(depths, width * height * sizeof(f32));
    bgfx::updateTexture2D(g_depth, 0, 0, 0, 0, static_cast<u16>(width), static_cast<u16>(height), memory);
    g_depthLive = true;
}

void ScreenFxDrawFilter()
{
    // The TS_NO switch: the colour filter's pass off
    if (Platform::Graphics::HostStageDisabled("screenfx"))
    {
        return;
    }

    MakeShared();
    if (!g_depthLive || !bgfx::isValid(g_depth) || !bgfx::isValid(g_palette) || !bgfx::isValid(g_output))
    {
        return;
    }
    if (!bgfx::getAvailTransientVertexBuffer(3, ScreenFxLayout()))
    {
        return;
    }
    // One fullscreen triangle (covers every pixel; the overhang samples clamp to the edge)
    bgfx::TransientVertexBuffer buffer;
    bgfx::allocTransientVertexBuffer(&buffer, 3, ScreenFxLayout());
    auto* at = reinterpret_cast<ScreenFxVertex*>(buffer.data);
    at[0] = {-1.0f, -1.0f, 0.0f};
    at[1] = {3.0f, -1.0f, 0.0f};
    at[2] = {-1.0f, 3.0f, 0.0f};
    bgfx::setViewMode(HostScreenFxView, bgfx::ViewMode::Sequential);
    bgfx::setViewRect(HostScreenFxView, 0, 0, static_cast<u16>(g_depthWidth), static_cast<u16>(g_depthHeight));
    bgfx::setViewFrameBuffer(HostScreenFxView, g_output);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::setTexture(0, g_depthSampler, g_depth);
    bgfx::setTexture(1, g_paletteSampler, g_palette);
    bgfx::setVertexBuffer(0, &buffer);
    bgfx::submit(HostScreenFxView, g_program);
}

bool ScreenFxReadOutput(u32 width, u32 height, u8* out)
{
    if (!bgfx::isValid(g_output) || !bgfx::isValid(g_readback) || out == nullptr || width != g_depthWidth ||
        height != g_depthHeight)
    {
        return false;
    }
    bgfx::TextureHandle output = bgfx::getTexture(g_output);
    if (!bgfx::isValid(output))
    {
        return false;
    }
    // The picking example's flow: render on the GPU, blit to the CPU texture, read it back (the blit rides its
    // own later view: bgfx runs a view's blits before its draws, so sharing the filter's view would copy the
    // previous frame)
    bgfx::setViewRect(HostScreenFxBlitView, 0, 0, static_cast<u16>(width), static_cast<u16>(height));
    bgfx::blit(HostScreenFxBlitView, {.handle = g_readback}, {.handle = output});
    u32 ready = bgfx::read({.handle = g_readback}, out);
    for (u32 spin = 0; spin < 60; spin++)
    {
        // The picking example's wait: frames advance until the read's frame is done
        if (bgfx::frame() == ready)
        {
            return true;
        }
    }
    return false;
}

void DrawSceneFog()
{
    // The TS_NO switch: the scene fog's pass off
    if (Platform::Graphics::HostStageDisabled("fog"))
    {
        return;
    }

    // The retail screen effects' colour filter (effects.cpp's DrawScreenEffects -> FilterColours): the chunk's
    // colourFilterPalette -- scenery.h calls it the fog colour -- read as a CLUT by the frame's depth, drawn over
    // the finished frame. g_ColourFilterEffect is the start-up flag that switches the whole effect on (gamecontext
    // sets it from the context flags) and g_ColourFilterOn the per-chunk one the game raises when a character is
    // placed in a chunk; without both there is no filter.
    //
    // The palette is not a guess: the eight CLUTs in the retail data (D_002E73C0's pointers) ramp alpha from about
    // 0x50 at entry 0 to 0x80 (1.0, the identity) at entry 255 while their colours fade from the chunk's haze colour
    // towards black, so the GS formula (Cd - Cs) * As + Cs ramps the mist from nothing to about 40% and never replaces
    // the frame. The beach's palette 5 is the flat one: (255, 200, 120) all the way along, alpha 90 at entry 0 to 128
    // at entry 255, i.e. no mist within reach and at most 30% orange at the far plane.
    //
    // Which entry a pixel takes is retail's own arithmetic (see the shader): the copy's byte, bits 8..15 of
    // 0xFFFFFE * (1 - depth), drawn only where Z <= 0x10000 -- about 26 units from the lens's 0.1 near plane. Retail's
    // copy of the depth exists because it draws into the same buffer it samples; the host draws into the scene's
    // colour-only target (HostSceneColourTarget), whose depth is not bound, so it can sample the scene depth directly
    if (g_ColourFilterEffect == 0 || g_ColourFilterOn == 0)
    {
        return;
    }
    MakeFogPalette(g_ColourFilterPalette);

    bgfx::TextureHandle depth = Platform::Graphics::HostSceneDepth();
    if (!bgfx::isValid(g_fogPalette) || !bgfx::isValid(depth) || !bgfx::isValid(g_fogProgram))
    {
        return;
    }
    if (!bgfx::getAvailTransientVertexBuffer(3, ScreenFxLayout()))
    {
        return;
    }
    // Over the frame the scene target holds (FinishScene runs before FinishFrame's composite, so the filter is
    // part of what is composited). It samples the scene target's own depth while drawing into its colour: retail
    // copies the depth out first (CopyDepthBuffer) for exactly that reason, but bgfx binds a texture and a target
    // attachment of the same framebuffer fine as long as the sampled depth is not the one being written, and this
    // pass writes no depth at all. The blend is retail's ALPHA formula (GS_ALPHA{a=1} = (Cd - Cs)·As + Cs): the
    // source colour weighted by 1 - As, the destination by As
    bgfx::TransientVertexBuffer buffer;
    bgfx::allocTransientVertexBuffer(&buffer, 3, ScreenFxLayout());
    auto* at = reinterpret_cast<ScreenFxVertex*>(buffer.data);
    at[0] = {-1.0f, -1.0f, 0.0f};
    at[1] = {3.0f, -1.0f, 0.0f};
    at[2] = {-1.0f, 3.0f, 0.0f};
    // The view rect is the scene target's own size, not the game's screen size: the game's frame is the internal
    // display (512x448 on PAL), while the scene target and its colour-only alias are the window's. Setting the pass
    // from HostScreenSize confined the filter to the lower-left corner of a scaled window (a hard-edged rectangle
    // over part of the water). HostSceneWidth/Height are the target's, so the filter covers exactly the frame it
    // tints whatever size the window is
    const u32 width = Platform::Graphics::HostSceneWidth();
    const u32 height = Platform::Graphics::HostSceneHeight();
    bgfx::setViewMode(HostSceneFogView, bgfx::ViewMode::Sequential);
    bgfx::setViewFrameBuffer(HostSceneFogView, Platform::Graphics::HostSceneColourTarget());
    bgfx::setViewRect(HostSceneFogView, 0, 0, static_cast<u16>(width), static_cast<u16>(height));
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_INV_SRC_ALPHA,
                                                                BGFX_STATE_BLEND_SRC_ALPHA));
    bgfx::setTexture(0, g_fogDepthSampler, depth);
    bgfx::setTexture(1, g_fogPaletteSampler, g_fogPalette);
    // TS_FOGPACKED=1: the packed leading byte (255 * (1 - depth), the flat wash this pass drew before), so the two
    // indexings can be compared in one build without a rebuild
    static const f32 packed = std::getenv("TS_FOGPACKED") != nullptr ? 1.0f : 0.0f;
    const f32 fogMode[4] = {packed, 0.0f, 0.0f, 0.0f};
    bgfx::setUniform(g_fogUniform, fogMode);
    bgfx::setVertexBuffer(0, &buffer);
    bgfx::submit(HostSceneFogView, g_fogProgram);
}

void FinishScene(bool effects)
{
    // The scene's drawing finished: the screen effects drawn when asked (the colour filter's fullscreen pass
    // over the depth the frame uploaded), then what the scene's models and materials gathered is put into the
    // buckets by the 3D submit (which owns the rest of this function)
    if (effects)
    {
        DrawSceneFog();
        ScreenFxDrawFilter();
    }
}
}
