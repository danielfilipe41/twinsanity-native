// Shadow pass: the PS2's half-size shadow buffer and volumes (src/platform/ps2/renderer/shadows.cpp).
//
// The volumes draw through the rigid-mesh path into a half-res RGBA8 + D24S8 target (view 25). The resolve then
// samples its coverage in view 21 (the Shadows bucket), looks the attenuation up in a 256-entry LUT selected by
// depth, and subtracts it from the frame.
//
// Two PS2 steps are missing because a bgfx view cannot sample or copy the backbuffer: the frame's alpha and depth
// are not shrunk into the target. Its depth starts at the near end, so only the volumes' own surfaces test
// against it.
#include "host_graphics.h"

#include "game/resources.h"
#include "game/shadows.h"
#include "game/view.h"

#include "draws_host.h"

#include "shaders/fs_shadowresolve.bin.h"
#include "shaders/vs_shadowresolve.bin.h"

namespace
{
// How much of the frame a fully covered pixel loses. Retail's ApplyShadows uses 32/128; this is halved because the
// coverage saturates at one shell, and the retail amount would make a single capsule as dark as a fully crossed
// ground.
constexpr f32 ShadowAmount = 16.0f / 128.0f;
// Value for the resolve's u_shadowAmount.y. One shell is a whole shadow now, so the count is unused; the uniform
// keeps the field.
constexpr f32 ShadowCountFull = 1.0f;
// The target's clear: no coverage, and the depth at the near end. Only the fallback (no scene depth) tests against that
constexpr u32 ShadowClearColour = 0x00000000;
constexpr f32 ShadowClearDepth = 0.0f;

// The pass's objects, made on SetUpShadowPass (the game calls it when the default chunk loads, bgfx is up by
// then; owned by the process like the other host graphics objects)
bgfx::FrameBufferHandle g_half = BGFX_INVALID_HANDLE;
bgfx::TextureHandle g_lut = BGFX_INVALID_HANDLE;
bgfx::ProgramHandle g_resolve = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_shadowSampler = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_depthSampler = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_lutSampler = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_frameSampler = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_amount = BGFX_INVALID_HANDLE;
bool g_made = false;
// The LUT's entries kept as they were uploaded (the proof checks them), and the LUT's texture size
u32 g_lutEntries[Platform::Graphics::HostShadowLutEntries] = {};
// The half-res target's size (remade when the screen's changes) and whether BeginShadows is open
u32 g_halfWidth = 0;
u32 g_halfHeight = 0;
bool g_open = false;
// Spare view clearing the coverage target's own depth for the depth-less path (the ramp falls back to it, so it must hold
// this frame's clear, not last frame's). View 33: outside the retail buckets' 0..27 range, and OrderFrameViews runs it
// just before the coverage pass (25) and the resolve (21)
constexpr bgfx::ViewId HostShadowVolumeDepthView = 33;
// The frame the resolve darkens (invalid: the window's backbuffer)
bgfx::FrameBufferHandle g_frame = BGFX_INVALID_HANDLE;
// The frame's size, given with it: the shadow pass sizes its offscreen targets and its overdraw by it, since the volumes test
// against the scene's depth (which is that frame's) and the alpha copy is taken from that frame
u32 g_frameWidth;
u32 g_frameHeight;
bool g_frameHasDepth = false;
// The resolve pass's uniform block (x: the amount, the rest unused)
f32 g_amountValue[4] = {ShadowAmount, ShadowCountFull, 0.0f, 0.0f};
// The near and far planes the ramp turns the frame's perspective depth into a distance with (the same object's x and y)
bgfx::UniformHandle g_shadowLinearU = BGFX_INVALID_HANDLE;
// w: the resolve's destination-alpha gate (shader u_shadowLinear.w): 1 samples the frame's own alpha per pixel and only
// opaque pixels take a shadow, retail's TEST{DEST=1}. Was 0 (gate off, everything takes the shadow, casters included)
f32 g_shadowLinearValue[4] = {0.1f, 1000.0f, 0.0f, 1.0f};

// The resolve's fullscreen triangle: clip-space positions only (the shader derives the target's UVs from them)
struct ResolveVertex
{
    f32 x;
    f32 y;
    f32 z;
};

bgfx::VertexLayout ResolveLayout()
{
    bgfx::VertexLayout layout;
    layout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).end();
    return layout;
}

// The volumes' coverage pass must run before the resolve samples it, and a bgfx view has one framebuffer, so the two
// are different views: the coverage pass takes view 25 (the retail bucket number nothing uses, 25) and the resolve the
// Shadows bucket's own, 21. Their place in the frame's order is the frame's order table's business
// (the view order set at BgfxInit, which puts 25 just before 21 as retail's bucket does)
// The objects every pass uses: the palette-equivalent LUT and the resolve's program and uniforms. Made once,
// after BgfxInit (SetUpShadowPass runs when the game's default chunk loads)
void MakeShared()
{
    if (g_made)
    {
        return;
    }

    g_made = true;
    // The palette-equivalent LUT: 256 entries, white with the alpha ramp 255 down to 0 (the GS palette the
    // retail set-up uploads, entry i's alpha 255 - i), point sampled like the CLUT lookup it stands in for
    for (u32 index = 0; index < Platform::Graphics::HostShadowLutEntries; index++)
    {
        g_lutEntries[index] = 0x00FFFFFFu | ((Platform::Graphics::HostShadowLutEntries - 1 - index) << 24);
    }

    g_lut = bgfx::createTexture2D(static_cast<u16>(Platform::Graphics::HostShadowLutEntries), 1, false, 1, bgfx::TextureFormat::RGBA8,
                                  BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT |
                                      BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
                                  bgfx::copy(g_lutEntries, sizeof(g_lutEntries)));
    bgfx::ShaderHandle vs = bgfx::createShader(bgfx::makeRef(vs_shadowresolve_glsl, sizeof(vs_shadowresolve_glsl)));
    bgfx::ShaderHandle fs = bgfx::createShader(bgfx::makeRef(fs_shadowresolve_glsl, sizeof(fs_shadowresolve_glsl)));
    g_resolve = bgfx::createProgram(vs, fs, true);
    g_shadowSampler = bgfx::createUniform("s_shadow", bgfx::UniformType::Sampler);
    g_depthSampler = bgfx::createUniform("s_depth", bgfx::UniformType::Sampler);
    g_lutSampler = bgfx::createUniform("s_palette", bgfx::UniformType::Sampler);
    // The scene colour the resolve samples for the frame's destination alpha (the PS2's destination-alpha gate)
    g_frameSampler = bgfx::createUniform("s_frame", bgfx::UniformType::Sampler);
    g_amount = bgfx::createUniform("u_shadowAmount", bgfx::UniformType::Vec4);
    g_shadowLinearU = bgfx::createUniform("u_shadowLinear", bgfx::UniformType::Vec4);
}

// The frame's colour, copied for the resolve's destination-alpha gate. Retail shrinks the frame's colour into its half-size
// buffer; a pass cannot sample the colour it is drawing into, so the scene is copied once a frame and the resolve samples the
// copy where its shader's s_frame stands in for the PS2's frame alpha (only pixels the scene drew opaque take a shadow)
bgfx::TextureHandle g_frameCopy = BGFX_INVALID_HANDLE;
u32 g_frameCopyWidth;
u32 g_frameCopyHeight;

void MakeFrameCopy(u32 width, u32 height)
{
    if (width == 0 || height == 0 || (width == g_frameCopyWidth && height == g_frameCopyHeight && bgfx::isValid(g_frameCopy)))
    {
        return;
    }

    if (bgfx::isValid(g_frameCopy))
    {
        bgfx::destroy(g_frameCopy);
    }

    g_frameCopyWidth = width;
    g_frameCopyHeight = height;
    g_frameCopy = bgfx::createTexture2D(static_cast<u16>(width), static_cast<u16>(height), false, 1,
                                        bgfx::TextureFormat::RGBA8,
                                        BGFX_TEXTURE_BLIT_DST | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
}

// The coverage target's colour, kept so the volumes' pass can be given a target that shares it
bgfx::TextureHandle g_halfColour = BGFX_INVALID_HANDLE;
// The coverage target's own depth texture, the blit destination the frame's depth is copied into before the volumes draw
bgfx::TextureHandle g_halfDepth = BGFX_INVALID_HANDLE;
// Whether the volumes are testing against that copied scene depth (true) or against their own footprint depth (false)
bool g_volumesOverSceneDepth = false;

// The coverage target: the frame's size, an RGBA8 colour the volumes write their coverage's alpha into and a D24S8 depth
// (their own test, which they write as they draw). Remade when the frame's size changes
void MakeHalf(u32 width, u32 height)
{
    if (width == 0 || height == 0 || (width == g_halfWidth && height == g_halfHeight && bgfx::isValid(g_half)))
    {
        return;
    }

    if (bgfx::isValid(g_half))
    {
        bgfx::destroy(g_half);
    }

    g_halfWidth = width;
    g_halfHeight = height;
    bgfx::TextureHandle colour =
        bgfx::createTexture2D(static_cast<u16>(width), static_cast<u16>(height), false, 1, bgfx::TextureFormat::RGBA8,
                              BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    // BGFX_TEXTURE_BLIT_DST: the frame's depth is copied INTO this one (BeginShadows) rather than attached to a second
    // framebuffer, which is the attachment that reads as a constant
    bgfx::TextureHandle depth =
        bgfx::createTexture2D(static_cast<u16>(width), static_cast<u16>(height), false, 1, bgfx::TextureFormat::D24S8,
                              BGFX_TEXTURE_RT | BGFX_TEXTURE_BLIT_DST | BGFX_SAMPLER_MIN_POINT |
                                  BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    const bgfx::TextureHandle handles[2] = {colour, depth};
    g_half = bgfx::createFrameBuffer(2, handles, true);
    g_halfColour = colour;
    g_halfDepth = depth;
}
}

namespace Platform::Graphics
{
void SetUpShadowPass()
{
    MakeShared();
}

void BeginShadows()
{
    MakeShared();
    // The coverage target is the FRAME's size (what the resolve darkens), so the resolve's overdraw and the frame's alpha copy
    // line up with it. Neither the window nor the game's screen size is necessarily it (a harness darkens a target of its own),
    // so the frame tells the pass its size. The volumes' screen matrix is the clip matrix, so no size enters it
    const u32 width = g_frameWidth != 0 ? g_frameWidth : BgfxWindowWidth;
    const u32 height = g_frameHeight != 0 ? g_frameHeight : BgfxWindowHeight;
    MakeHalf(width, height);

    // The frame's colour into the copy the resolve's gate samples: the frame the resolve darkens (the scene target in the
    // game, whatever a harness sets otherwise), copied because a pass cannot sample the colour it is drawing into
    static u32 copiedFrame = 0xFFFFFFFFu;
    const u32 frame = bgfx::getStats()->gpuFrameNum;
    if (copiedFrame != frame)
    {
        copiedFrame = frame;
        MakeFrameCopy(width, height);
        if (bgfx::isValid(g_frameCopy) && bgfx::isValid(g_frame))
        {
            bgfx::blit(HostShadowVolumeView, {.handle = g_frameCopy}, {.handle = bgfx::getTexture(g_frame, 0)});
        }
    }

    // The volumes' pass: the offscreen view draws into the coverage target, cleared to no coverage each frame. Each volume
    // draws twice over the copy of the scene's depth: the shells away from the camera ADD (a surface inside the volume hides
    // them, so the ground the volume passes through counts) and the ones facing the camera are TAKEN BACK (they pass wherever
    // the whole volume is behind the surface, which is what cancels a buried volume). The pair is the retail pass's single
    // signed draw: the GS dump shows all 1,276 of its volume triangles under one state with the facing carried in the vertex
    // colour's low bits as +-8 and COLCLAMP off, so the 8-bit wrap is the take-back. The additive pass alone still shows the
    // volume through the ground, because a buried volume's away shells pass GEQUAL and paint a streak under the caster
    bgfx::setViewMode(HostShadowVolumeView, bgfx::ViewMode::Sequential);
    bgfx::setViewRect(HostShadowVolumeView, 0, 0, static_cast<u16>(g_halfWidth), static_cast<u16>(g_halfHeight));
    // The frame's depth is COPIED into the coverage target's own depth (the PS2's CopyDepthBuffer), not attached to a second
    // framebuffer: sharing a depth texture as another framebuffer's attachment reads as a constant here, while the copy is a
    // real transfer. The copy rides the spare view (its blits run before its draws, and its bucket sits before the volumes')
    bgfx::TextureHandle sceneDepth = BGFX_INVALID_HANDLE;
    if (bgfx::isValid(g_frame) && g_frameHasDepth)
    {
        sceneDepth = bgfx::getTexture(g_frame, 1);
    }
    g_volumesOverSceneDepth = bgfx::isValid(sceneDepth) && bgfx::isValid(g_halfDepth);
    if (g_volumesOverSceneDepth)
    {
        bgfx::setViewRect(HostShadowVolumeDepthView, 0, 0, static_cast<u16>(g_halfWidth), static_cast<u16>(g_halfHeight));
        bgfx::blit(HostShadowVolumeDepthView, {.handle = g_halfDepth}, {.handle = sceneDepth});
        // The copy's view has no framebuffer, so it needs a touch: bgfx submits a view's commands only when it has one
        bgfx::touch(HostShadowVolumeDepthView);
    }

    // Only the coverage target is bound. With the frame's depth copied in, the volumes test against THAT and the depth must not
    // be cleared; without one they test against their own depth, which is cleared to the near end for them to write
    bgfx::setViewFrameBuffer(HostShadowVolumeView, g_half);
    bgfx::setViewClear(HostShadowVolumeView,
                       g_volumesOverSceneDepth ? BGFX_CLEAR_COLOR : (BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH),
                       ShadowClearColour, ShadowClearDepth, 0);

    bgfx::touch(HostShadowVolumeView);

    // The shadows' screen matrix the game's DrawChunkShadows composes with: on the PS2 this is the half-size
    // screen's explicit GS pixel matrix (sky.cpp's HalfSizeScreenMatrix). The host's half-res target covers the
    // same clip space as the frame (its viewport is the half-size screen), so the clip matrix is the whole of it
    if (g_RenderView != nullptr)
    {
        g_ShadowToCamera = g_RenderView->toClip;
        g_ShadowToScreen = g_RenderView->toClip;
    }

    g_open = true;
}

void EndShadows()
{
    if (!g_open)
    {
        return;
    }

    g_open = false;
    if (!bgfx::isValid(g_half) || !bgfx::isValid(g_resolve) || !bgfx::getAvailTransientVertexBuffer(3, ResolveLayout()))
    {
        return;
    }

    // The resolve covers the frame (the target it darkens), which is the size it was given with the target
    const u32 width = g_frameWidth != 0 ? g_frameWidth : BgfxWindowWidth;
    const u32 height = g_frameHeight != 0 ? g_frameHeight : BgfxWindowHeight;

    // One fullscreen triangle over the frame (the overhang samples clamp to the edge)
    bgfx::TransientVertexBuffer buffer;
    bgfx::allocTransientVertexBuffer(&buffer, 3, ResolveLayout());
    auto* at = reinterpret_cast<ResolveVertex*>(buffer.data);
    at[0] = {-1.0f, -1.0f, 0.0f};
    at[1] = {3.0f, -1.0f, 0.0f};
    at[2] = {-1.0f, 3.0f, 0.0f};

    // The resolve: the half-res coverage through the depth-selected palette ramp, the frame losing it through
    // the reverse-subtract blend (dst - src, the retail Cd - Cs * 32 / 128); the colour's alpha is left alone
    bgfx::setViewMode(HostShadowView, bgfx::ViewMode::Sequential);
    bgfx::setViewRect(HostShadowView, 0, 0, static_cast<u16>(width), static_cast<u16>(height));
    // The resolve samples the scene's depth for the ramp, and a texture cannot be sampled while it is bound to the
    // framebuffer being drawn into: the game sets the scene target (colour and depth), whose colour alone is reached through
    // the colour-only target (the same texture). A target set by anything else (a harness) has no scene behind it, so it keeps
    // the coverage target's own depth for the ramp
    const bool sceneFrame = bgfx::isValid(g_frame) && g_frame.idx == Platform::Graphics::HostSceneTargetIfMade().idx;
    bgfx::FrameBufferHandle target = sceneFrame ? HostSceneColourTarget() : g_frame;

    bgfx::setViewFrameBuffer(HostShadowView, target);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_EQUATION(BGFX_STATE_BLEND_EQUATION_REVSUB) |
                   BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE));
    // Every lookup in this pass is NEAREST on the console, and that is what keeps a shadow crisp rather than soft: the
    // half-size buffer is copied to the screen at 1:1 with TEX1 unwritten (shadows.cpp's ApplyShadows: no filtering), the
    // frame depth is read as a byte, and that byte indexes a 256-entry palette, an integer lookup filtering can only
    // smear. Bilinear here averaged neighbouring coverage and neighbouring ramp entries into a smooth gradient
    constexpr u64 PointClamp = BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_U_CLAMP |
                               BGFX_SAMPLER_V_CLAMP;
    bgfx::setTexture(0, g_shadowSampler, bgfx::getTexture(g_half, 0), PointClamp);
    // The ramp indexes the LUT by the SCENE's depth - the frame the volumes were tested against - as retail indexes its palette by
    // the frame-depth copy's top byte. The coverage target's own depth is no use here: the volumes write no depth in the
    // frame-depth path, so it holds only the clear value and the ramp goes constant (the silhouette, not a footprint)
    const bool useFrameDepth = bgfx::isValid(g_frame) && g_frameHasDepth;
    bgfx::TextureHandle rampDepth = useFrameDepth ? bgfx::getTexture(g_frame, 1) : bgfx::getTexture(g_half, 1);

    bgfx::setTexture(1, g_depthSampler, rampDepth, PointClamp);
    bgfx::setTexture(2, g_lutSampler, g_lut, PointClamp);
    // The frame's alpha the coverage is gated on (shader s_frame, the PS2's destination-alpha test). Unbound, the sampler reads
    // black and the gate zeroes every shadow
    bgfx::setTexture(3, g_frameSampler, g_frameCopy);
    bgfx::setUniform(g_amount, g_amountValue);

    // The ramp indexes the LUT by distance, so it needs the game's near and far planes (view.cpp SetProjection:
    // m[2][2] = (f+n)/(f-n), m[3][2] = -2fn/(f-n)). Raw perspective depth packs the visible world into roughly
    // 0.95..1.0, which cannot separate the ground at the caster's feet from the horizon.
    {
        const f32 plusOne = g_ShadowToScreen.m[2][2] + 1.0f;
        const f32 minusOne = g_ShadowToScreen.m[2][2] - 1.0f;
        const f32 nearPlane = plusOne != 0.0f ? -g_ShadowToScreen.m[3][2] / plusOne : 0.0f;
        const f32 farPlane = minusOne != 0.0f ? -g_ShadowToScreen.m[3][2] / minusOne : 0.0f;
        if (nearPlane > 0.0f && nearPlane < farPlane)
        {
            g_shadowLinearValue[0] = nearPlane;
            g_shadowLinearValue[1] = farPlane;
        }
    }

    // z: the switch between the two palette indexings (see fs_shadowresolve.sc). 0 or less -- the DEFAULT -- is the retail form
    // itself: the frame-depth byte into retail's 256-entry white palette, alpha ramping 255 down to 0. A positive value is a
    // distance in metres the ramp falls from full to none over, which is NOT retail (retail has no such number): it exists only
    // because the host's perspective depth cannot be used as that index directly, packing the whole visible world into its last
    // few percent.
    bgfx::setUniform(g_shadowLinearU, g_shadowLinearValue);
    bgfx::setVertexBuffer(0, &buffer);
    bgfx::submit(HostShadowView, g_resolve);
}

void DrawShadowMesh(RigidModel* mesh, const Matrix4x4* toScreen, const Matrix4x4* toCamera, const Matrix4x4* world)
{
    if (!g_open || mesh == nullptr || toScreen == nullptr)
    {
        return;
    }

    const auto* model = reinterpret_cast<const HostRigidModel*>(mesh);
    const HostRigidModelData* data = model->model;
    if (data == nullptr || data->subModelCount == 0 || data->packets == nullptr)
    {
        return;
    }

    // Depth GEQUAL counts fragments where the volume lies behind the frame's surface. Retail carries facing in the vertex
    // colour (+8 one winding, -8 the other) and adds with COLCLAMP off, so the closed volume nets to one shell for the
    // ground inside it and zero for fully buried pixels.
    //
    // bgfx cannot wrap, so away shells count into R and facing shells into G, 8/255 each, and the resolve takes their
    // difference. A surface inside the volume has its away shell behind it and its facing shell in front, counting one;
    // a surface with the whole volume behind it counts none.
    //
    // No depth is written: the target is the frame's own copy, which other buckets still test against.
    constexpr u64 VolumeBlend = BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_FACTOR, BGFX_STATE_BLEND_ONE);
    // 8/255 in every channel: the state's write mask picks which one a pass may land in
    constexpr u32 VolumeShellFactor = 0x08080808u;
    constexpr u64 VolumeStateScene = BGFX_STATE_WRITE_R | BGFX_STATE_DEPTH_TEST_GEQUAL | VolumeBlend;
    // With no frame depth to copy (a target of its own, a harness's) the volumes test against their own depth, cleared near and
    // written as they draw: only faces past the first pass, so the away shells count once and the coverage is the volume's own
    // footprint
    constexpr u64 VolumeStateOwn =
        BGFX_STATE_WRITE_R | BGFX_STATE_DEPTH_TEST_GEQUAL | BGFX_STATE_WRITE_Z | VolumeBlend;
    constexpr u64 VolumeTakeBack = BGFX_STATE_WRITE_G | BGFX_STATE_DEPTH_TEST_GEQUAL | VolumeBlend;
    for (u32 subModel = 0; subModel < data->subModelCount; subModel++)
    {
        // The rigid-mesh path (draws_host.cpp): parses the packet once, caches its buffers, lights are the
        // neutral ones (a shadow volume has no lighting of its own), the matrix is the game's model-to-clip one. The
        // additive pass keeps the shells away from the camera (CULL_CCW on this data) and the take-back the ones facing it
        HostDrawRigidPacket(data->packets[subModel], data->sizes[subModel], data->vertexCounts[subModel], toScreen,
                            nullptr, nullptr, Platform::Graphics::HostWhiteTexture(), HostShadowVolumeView,
                            g_volumesOverSceneDepth ? (VolumeStateScene | BGFX_STATE_CULL_CCW)
                                                    : (VolumeStateOwn | BGFX_STATE_CULL_CCW),
                            nullptr, VolumeShellFactor, Platform::Graphics::HostShadowVolumeProgram());
        HostDrawRigidPacket(data->packets[subModel], data->sizes[subModel], data->vertexCounts[subModel], toScreen,
                            nullptr, nullptr, Platform::Graphics::HostWhiteTexture(), HostShadowVolumeView,
                            VolumeTakeBack | BGFX_STATE_CULL_CW, nullptr, VolumeShellFactor,
                            Platform::Graphics::HostShadowVolumeProgram());
    }

    // The camera-space and world matrices the PS2's VU1 program lights and fogs with have no host use here
    (void)toCamera;
    (void)world;
}

const u32* HostShadowLut()
{
    return g_made ? g_lutEntries : nullptr;
}

void HostShadowSetFrameTarget(bgfx::FrameBufferHandle frame, u32 width, u32 height, bool sceneTarget)
{
    g_frame = frame;
    // The frame's own size: the coverage target's depth attachment is the scene's depth, so the two must match, and the
    // resolve's overdraw and the frame's alpha copy are the frame's size too. A harness's target is not the scene's
    g_frameWidth = width;
    g_frameHeight = height;
    // Whether the frame carries a depth attachment the volumes should test against (the game's scene target does). Said by
    // the caller rather than inferred: finding it out here would create the scene target for a caller that has a frame of its
    // own
    g_frameHasDepth = sceneTarget;
}
}
