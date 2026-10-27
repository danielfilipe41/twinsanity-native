// Sky: the PS2's half-size sky buffer (src/platform/ps2/renderer/sky.cpp) as a bgfx offscreen target.
//
// A chunk's sky draws into the half-size buffer through the half-size screen matrix (view 0, the Sky bucket) and is
// copied point-sampled over the screen (view 1). It is unlit and colour-only. Each part uses its material's GS
// state, so blended sun/glow/haze and alpha-masked clouds stay as retail draws them, and the texture is sampled
// from its largest level alone (shader type 0x0A never minifies through mips).
#include "host_graphics.h"
#include "draws_host.h"

#include "game/controllers.h"
#include "game/math.h"
#include "game/place.h"
#include "game/reference.h"
#include "game/view.h"

namespace
{
// The GS units HalfSizeScreenMatrix works in (src/platform/ps2/renderer/renderer.h): the screen's middle at 2048
// pixels, 16 subpixels to a pixel, 8 to half a one, and half the 24-bit depth's range
constexpr f32 GsScreenMiddleFloat = 2048.0f;
constexpr f32 GsSubpixels = 16.0f;
constexpr f32 GsHalfSubpixels = 8.0f;
constexpr f32 GsDepthRange = 8388607.0f;

// A chunk's sky is HostSky (draws_host.h), shared with ReadSky so the resource read and the draw agree on the layout

// src/platform/ps2/renderer/sky.cpp's HalfSizeScreenMatrix, ported verbatim: the clip space to the half-size
// buffer's GS screen coordinates (1/16th of a pixel, the screen's middle at 2048), the half-size buffer's frame
// being the middle half of the screen. On the host the same mapping is the offscreen view's viewport (a frame
// buffer of target->width/2 by target->height/2), so the matrix itself is kept for the game's Sky::toScreen field
// and checked against the offscreen mapping in the headless proof.
void HalfSizeScreenMatrix(const RenderTargetDescription* target, Matrix4x4* matrix)
{
    f32 x = GsScreenMiddleFloat - static_cast<f32>(target->displayWidth >> 2);
    f32 y = GsScreenMiddleFloat - static_cast<f32>(target->displayHeight >> 2);
    f32 halfWidth = static_cast<f32>(target->width >> 1);
    f32 halfHeight = static_cast<f32>(target->height >> 1);
    x = x + static_cast<f32>(target->offsetX);
    y = y + static_cast<f32>(target->offsetY);
    InitIdentityMatrix(matrix);
    matrix->m[0][0] = halfWidth * GsHalfSubpixels;
    matrix->m[1][1] = -halfHeight * GsHalfSubpixels;
    matrix->m[2][2] = -GsDepthRange;
    matrix->m[3][0] = (x + halfWidth * 0.5f) * GsSubpixels;
    matrix->m[3][1] = (y + halfHeight * 0.5f) * GsSubpixels;
    matrix->m[3][2] = GsDepthRange;
}

// The view's camera's place (renderer.h's CameraPlace), the null page the PS2 reads at address 8 stood in for by a
// null place: a host sky is only drawn with a camera, and reading page 0 would fault
ObjectPlace* CameraPlaceOf(const RenderView* view)
{
    Reference* reference = view->cameraObject;
    ReferencedObject* camera = reference != nullptr ? reference->object : nullptr;
    return camera != nullptr ? camera->place : nullptr;
}

// The views the sky owns: the half-size buffer's pass (the Sky bucket) and the copy over the screen (the first
// chain's spare view, 1, which follows the Sky bucket and precedes Opaque). The readback blit rides a spare view
// past the buckets (bgfx runs a view's blits before its draws, so it can't share the copy's view)
constexpr bgfx::ViewId HostSkyView = 0;
constexpr bgfx::ViewId HostSkyCopyView = 1;
constexpr bgfx::ViewId HostSkyReadbackView = 30;
// The half-size buffer's clear: opaque black (the PS2 draws the sky over the buffer's leftovers; the host needs a
// defined background, and black is what an unwritten frame buffer holds)
constexpr u32 HostSkyClearColour = 0x000000ff;

// A sky part's material (graphics.cpp's SubModelMaterial: the draw takes its state, its texture and its shader's scroll
// from it; null when the resource or its material is missing). Every part goes through retail's RenderSkyMaterial
// (materials.cpp, which writes the material's own shader's GS registers), so the host needs the material itself and not
// only HostSubModelTexture's lookup
const Platform::Graphics::HostMaterial* SubModelMaterial(MaterialResource** materials, u32 index)
{
    if (materials == nullptr || materials[index] == nullptr || materials[index]->material == nullptr)
    {
        return nullptr;
    }

    return reinterpret_cast<const Platform::Graphics::HostMaterial*>(materials[index]->material);
}

// A sky part's rigid draw: the shared 3D path through the rigid program, with the sky's own two differences. It is
// unlit, so the lights are null and the meshes' colours and textures carry the shading (DrawSkyModel's own VU1 program
// does the same); and its shader never minifies through a mip, so its texture is bound to the largest level alone
// (HostDrawRigidPacket's minifiesThroughMips, the sky's style in shadersettings.cpp)
void DrawSkyPacket(const u8* packet, u32 size, u32 vertexCount, const Matrix4x4* skyToCamera,
                   bgfx::TextureHandle texture, u64 state, const f32* alphaRef)
{
    Platform::Graphics::HostDrawRigidPacket(packet, size, vertexCount, skyToCamera, nullptr, nullptr, texture,
                                            HostSkyView, state, alphaRef, UINT32_MAX, BGFX_INVALID_HANDLE, false);
}

// The sky's objects, made lazily on first use (after BgfxInit, owned by the process): the offscreen buffer's
// colour (RGBA8, read back by the headless proof) and depth textures, the readback texture, and the target the
// copy draws into (invalid: the back buffer; a proof sets its own to read the copy back)
bgfx::FrameBufferHandle g_skyBuffer = BGFX_INVALID_HANDLE;
bgfx::TextureHandle g_skyColour = BGFX_INVALID_HANDLE;
bgfx::TextureHandle g_skyDepth = BGFX_INVALID_HANDLE;
bgfx::TextureHandle g_skyReadback = BGFX_INVALID_HANDLE;
bgfx::FrameBufferHandle g_skyCopyTarget = BGFX_INVALID_HANDLE;
u32 g_skyWidth = 0;
u32 g_skyHeight = 0;

// The half-size buffer for a width by height frame, made (or remade when the size changed): whether it's live
bool EnsureSkyBuffer(u32 width, u32 height)
{
    if (width == 0 || height == 0)
    {
        return false;
    }
    if (bgfx::isValid(g_skyBuffer) && width == g_skyWidth && height == g_skyHeight)
    {
        return true;
    }
    if (bgfx::isValid(g_skyBuffer))
    {
        bgfx::destroy(g_skyBuffer);
        g_skyBuffer = BGFX_INVALID_HANDLE;
        g_skyColour = BGFX_INVALID_HANDLE;
        g_skyDepth = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(g_skyReadback))
    {
        bgfx::destroy(g_skyReadback);
        g_skyReadback = BGFX_INVALID_HANDLE;
    }
    g_skyWidth = width;
    g_skyHeight = height;
    // The colour attachment (the readback is a blit into a CPU-visible texture: an RT texture can't also carry
    // BGFX_TEXTURE_READ_BACK, which makes bgfx refuse it); the depth is the sky's own, unused for anything but a
    // defined attachment
    g_skyColour = bgfx::createTexture2D(static_cast<u16>(width), static_cast<u16>(height), false, 1,
                                        bgfx::TextureFormat::RGBA8,
                                        BGFX_TEXTURE_RT | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
                                            BGFX_SAMPLER_MIP_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    g_skyDepth = bgfx::createTexture2D(static_cast<u16>(width), static_cast<u16>(height), false, 1,
                                       bgfx::TextureFormat::D16, BGFX_TEXTURE_RT_WRITE_ONLY);
    if (!bgfx::isValid(g_skyColour) || !bgfx::isValid(g_skyDepth))
    {
        return false;
    }
    bgfx::TextureHandle attachments[2] = {g_skyColour, g_skyDepth};
    g_skyBuffer = bgfx::createFrameBuffer(2, attachments, false);
    return bgfx::isValid(g_skyBuffer);
}

// The copy quad's vertex: clip-space position, the half-size buffer's texture fractions, opaque white (the 2D
// program samples texture * colour, so white passes the sky through)
struct SkyCopyVertex
{
    f32 x;
    f32 y;
    f32 z;
    f32 s;
    f32 t;
    f32 red;
    f32 green;
    f32 blue;
    f32 alpha;
};

bgfx::VertexLayout SkyCopyLayout()
{
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Float)
        .end();
    return layout;
}

// The half-size buffer over the target (the screen's display size): one fullscreen triangle strip, point-sampled
// (the PS2's FinishSky has bilinear off: two screen pixels for one texel), no depth, straight colour write
void DrawSkyCopy(u32 width, u32 height)
{
    if (!bgfx::isValid(g_skyColour) || width == 0 || height == 0 ||
        !bgfx::getAvailTransientVertexBuffer(4, SkyCopyLayout()))
    {
        return;
    }
    bgfx::TransientVertexBuffer buffer;
    bgfx::allocTransientVertexBuffer(&buffer, 4, SkyCopyLayout());
    auto* at = reinterpret_cast<SkyCopyVertex*>(buffer.data);
    // The shader's fractions map to NDC 2f - 1, y flipped (HostMapToClip); the texture's top-left shows at the
    // screen's top-left (the PS2's sprite shows the buffer's corner at the frame's). bgfx's OpenGL render targets
    // are GL-oriented (BGFX_CONFIG_GL_NORMALIZE_NDC_CONVENTIONS off): NDC y = +1 is the texture's v = 1, so the
    // copy reads v inverted, 1 - fraction
    const f32 corners[4][2] = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}};
    for (u32 vertex = 0; vertex < 4; vertex++)
    {
        f32 clipX = 0.0f;
        f32 clipY = 0.0f;
        Platform::Graphics::HostMapToClip(corners[vertex][0], corners[vertex][1], &clipX, &clipY);
        at[vertex] = {clipX, clipY, 0.0f, corners[vertex][0], 1.0f - corners[vertex][1], 1.0f, 1.0f, 1.0f,
                      1.0f};
    }
    bgfx::setViewMode(HostSkyCopyView, bgfx::ViewMode::Sequential);
    // The copy lands on the scene target (the frame the 3D passes drew into); a proof sets its own target instead
    bgfx::setViewFrameBuffer(HostSkyCopyView,
                             bgfx::isValid(g_skyCopyTarget) ? g_skyCopyTarget : Platform::Graphics::HostSceneTarget());
    bgfx::setViewRect(HostSkyCopyView, 0, 0, static_cast<u16>(width), static_cast<u16>(height));
    // The scene target's depth is cleared here, at the sky's own view (1), NOT left to StartFrame's view-2 clear: on the PS2
    // the sky is drawn into its half-size buffer and then over the frame, leaving the frame's depth at the far plane, so the
    // scene drawn afterwards passes. bgfx runs view 1 before view 2 (it is the smaller id), so a depth clear here lands
    // before any scene geometry while the sky copy itself (colour only, no depth) is unaffected. Without this the scene's
    // depth keeps the half-size buffer's D16 depths, which reject geometry sitting between them and the camera
    bgfx::setViewClear(HostSkyCopyView, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_PT_TRISTRIP);
    bgfx::setTexture(0, Platform::Graphics::HostDraw2DSampler(), g_skyColour,
                     BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    bgfx::setVertexBuffer(0, &buffer);
    bgfx::submit(HostSkyCopyView, Platform::Graphics::HostDraw2DProgram());
}
}

namespace Platform::Graphics
{
// The copy's target (the headless proof's offscreen screen); invalid is the back buffer
void HostSkySetCopyTarget(bgfx::FrameBufferHandle framebuffer)
{
    g_skyCopyTarget = framebuffer;
}

// The half-size buffer read back as RGBA8 rows (the proof's checksum): the offscreen target's own readback blit,
// then bgfx's read (frames advance until the read's frame is done, like ScreenFxReadOutput)
bool HostSkyReadBuffer(u32 width, u32 height, u8* out)
{
    if (!bgfx::isValid(g_skyColour) || out == nullptr || width != g_skyWidth || height != g_skyHeight)
    {
        return false;
    }
    if (!bgfx::isValid(g_skyReadback))
    {
        g_skyReadback = bgfx::createTexture2D(static_cast<u16>(width), static_cast<u16>(height), false, 1,
                                              bgfx::TextureFormat::RGBA8,
                                              BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK |
                                                  BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
                                                  BGFX_SAMPLER_MIP_POINT | BGFX_SAMPLER_U_CLAMP |
                                                  BGFX_SAMPLER_V_CLAMP);
    }
    if (!bgfx::isValid(g_skyReadback))
    {
        return false;
    }
    bgfx::setViewRect(HostSkyReadbackView, 0, 0, static_cast<u16>(width), static_cast<u16>(height));
    bgfx::blit(HostSkyReadbackView, {.handle = g_skyReadback}, {.handle = g_skyColour});
    u32 ready = bgfx::read({.handle = g_skyReadback}, out);
    for (u32 spin = 0; spin < 60; spin++)
    {
        if (bgfx::frame() == ready)
        {
            return true;
        }
    }
    return false;
}

void DrawChunkSky(Sky* sky, RenderView* view)
{
    if (sky == nullptr || view == nullptr)
    {
        return;
    }
    const RenderTargetDescription* target = g_RenderTarget;
    if (target == nullptr || target->width <= 0 || target->height <= 0)
    {
        // No render target (a headless caller that set neither): nothing to size the half-size buffer from
        return;
    }
    auto* host = reinterpret_cast<HostSky*>(sky);
    // StartSkyBuffer: the sky's matrix to the half-size buffer's screen is the view's clip matrix through the
    // half-size screen matrix. DrawSky then translates by the camera's place (the sky follows it)
    Matrix4x4 projection;
    HalfSizeScreenMatrix(target, &projection);
    VuMultiplyMatrices(&view->toClip, &projection, &host->toScreen);
    Matrix4x4 translation;
    InitIdentityMatrix(&translation);
    ObjectPlace* place = CameraPlaceOf(view);
    if (place != nullptr)
    {
        place->SyncPosition();
        *reinterpret_cast<Vector4*>(translation.m[3]) = place->position;
    }
    // The matrix the meshes go through is skyToCamera, the game's own object-to-CLIP matrix (DrawRigidModel's
    // instance toCamera: the camera's translation through the view's clip matrix). The PS2 draws the sky meshes
    // into the half-size buffer through exactly this and lets the half-size screen's viewport place them, which on
    // the host is this offscreen view's half-size viewport; going through toScreen instead would need a
    // window-sized viewport and double the half-size screen matrix. Its rows go to bgfx's mat4 uniform as they
    // are: a column-major upload of a row-major game matrix composes as the transpose, which is exactly the game's
    // row-vector transform (mul(u_mvp, v) = v * skyToCamera)
    Matrix4x4 skyToCamera;
    VuMultiplyMatrices(&translation, &view->toClip, &skyToCamera);

    // HalfSizeScreenMatrix's half of the target's DRAWING size (target->width/height), not the display's: on the
    // PS2 the half-size buffer is half the frame the scene is drawn into, so sizing it from displayWidth made the
    // sky's buffer and copy disagree with the scene whenever the two differ
    u32 halfWidth = static_cast<u32>(target->width) >> 1;
    u32 halfHeight = static_cast<u32>(target->height) >> 1;
    if (!EnsureSkyBuffer(halfWidth, halfHeight))
    {
        return;
    }
    // The half-size buffer's pass: view 0 (the Sky bucket), sequential, cleared, its own frame buffer. The
    // matrix the meshes go through is skyToCamera (the view's toClip with the camera's translation): the
    // half-size screen matrix's mapping is this view's half-size viewport
    bgfx::setViewMode(HostSkyView, bgfx::ViewMode::Sequential);
    // The frame's colour is set up here, by the sky: retail's DrawSky replaces SetUpFrame when a chunk has a sky, so
    // StartFrame is not called for sky frames at all. StartFrame's view-2 colour clear (the opaque bucket, which runs
    // AFTER the sky copy at view 1) still holds last frame's setting though, and would wipe the copy. Set the clear to
    // depth only (the depth clear that matters now rides view 1, the sky copy): the sky fills the frame's colour, the
    // scene over it only tests and writes depth
    Platform::Graphics::HostSceneSetView(Platform::Graphics::HostOpaqueView);
    bgfx::setViewClear(Platform::Graphics::HostOpaqueView, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);
    bgfx::setViewFrameBuffer(HostSkyView, g_skyBuffer);
    bgfx::setViewRect(HostSkyView, 0, 0, static_cast<u16>(halfWidth), static_cast<u16>(halfHeight));
    bgfx::setViewClear(HostSkyView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, HostSkyClearColour, 1.0f, 0);
    // The sky's pass state: the colour write alone, both on the half-size buffer's own D16 and as the base each part's
    // material folds onto. StartSkyBuffer leaves the buffer's depth unwritten (ZBUF's DepthNotWritten) and never writes
    // TEST, and every part then goes through RenderSkyMaterial, whose WriteShaders writes that shader's own GS
    // registers -- PRMODE's ABE, FBA, ZBUF, ALPHA, TEST, TEX1 -- so each part's blend, alpha test and depth test are
    // its material's, not the pass's: the sun, the glow and the haze are semi-transparent, the clouds are
    // alpha-masked, and every sky shader's ZTST is ALWAYS (shadersettings.cpp forces it for the type's settings, which
    // is where the header's "colour only" comes from). A material the host read no settings for keeps this state,
    // which is what the whole sky drew with before
    constexpr u64 SkyPassState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
    for (u32 index = 0; index < host->count; index++)
    {
        Platform::Graphics::HostRigidModel* model = host->models[index];
        if (model == nullptr || model->model == nullptr)
        {
            continue;
        }
        u32 count = model->model->subModelCount < model->count ? model->model->subModelCount : model->count;
        for (u32 subModel = 0; subModel < count; subModel++)
        {
            const u8* packet = model->model->packets[subModel];
            u32 size = model->model->sizes[subModel];
            u32 vertexCount = model->model->vertexCounts[subModel];
            if (packet == nullptr || size == 0)
            {
                continue;
            }
            // The sky's own materials scroll too (the clouds and the haze move): the same material state the 3D
            // passes take, so the sky's UVs offset exactly as retail's do, and its blend, alpha test and depth test
            // are the ones its shader's GS registers ask for
            const HostMaterial* material = SubModelMaterial(model->materials, subModel);
            HostSetSubModelScroll(model->materials, subModel);
            const HostMaterialState partState = HostMaterialStateOf(material, SkyPassState, true);
            DrawSkyPacket(packet, size, vertexCount, &skyToCamera,
                          HostSubModelTexture(model->materials, subModel), partState.state,
                          partState.alphaReference);
        }
    }
    // FinishSky: the buffer over the screen, depth not written. The copy covers the scene target (the window's
    // size, where every scene pass draws); a proof reading its own target keeps the game's display size
    if (bgfx::isValid(g_skyCopyTarget))
    {
        DrawSkyCopy(static_cast<u32>(target->displayWidth), static_cast<u32>(target->displayHeight));
    }
    else
    {
        DrawSkyCopy(static_cast<u32>(BgfxWindowWidth), static_cast<u32>(BgfxWindowHeight));
    }
}
}
