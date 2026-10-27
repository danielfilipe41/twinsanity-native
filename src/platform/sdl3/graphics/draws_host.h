// 3D draw submits: the meshes the mesh_builder parses
// into bgfx vertex/index buffers, drawn through the material's program with
// the instance's matrix and lights as uniforms. Only the SDL3 platform's own
// files include this; the game sees the opaque RigidModel/Skin/BlendSkin/Lod
// through include/platform/graphics.h.
#pragma once

#include "common.h"
#include "game/math.h"
#include "game/resources.h"
#include "host_graphics.h"
#include "platform/graphics.h"

#include <bgfx/bgfx.h>

struct MaterialResource;

namespace Platform::Graphics
{

// A rigid model's submodels as the PS2 has them (src/platform/ps2/renderer/
// renderer.h's RigidModelData, 0x1C bytes): how many submodels, each one's
// vertex count, packet size, dropped extra size and packet (the disk
// manager's handle on the PS2; the packet bytes' address on the host, the
// parse cache's key)
struct HostRigidModelData
{
    ResourceHeader header;
    u32 subModelCount;
    u32* vertexCounts;
    u32* sizes;
    u32* extraSizes;
    const u8** packets;
};

// A rigid model as the game draws it (renderer.h's RigidModel, 0x20 bytes):
// its submodels' materials, the model, the block of its instances, how many
// submodels (materials) it has and the model's ID
struct HostRigidModel
{
    ResourceHeader header;
    MaterialResource** materials;
    HostRigidModelData* model;
    void* instances;
    u32 instanceCapacity;
    u32 count;
    u32 modelId;
};

// A skin as the game draws it (renderer.h's Skin, 0x24 bytes): its submodels'
// materials, vertex counts, packets, how many, their packets' sizes and the
// vertexes all of them have
struct HostSkin
{
    ResourceHeader header;
    MaterialResource** materials;
    u32* vertexCounts;
    const u8** packets;
    u32 count;
    u32* sizes;
    u32 vertexTotal;
    u32 unused20;
};

// A part of a blend skin's submodel (renderer.h's BlendPart, 0x28 bytes):
// each shape's vertex offsets, the part's packet (its bytes' address on the
// host), how many shapes, three factors and the packet's size
struct HostBlendPart
{
    u32* shapeAddresses;
    u32* shapeSizes;
    u32* shapeCounts;
    const u8* packet;
    u8 shapeCount;
    f32 shapeFactors[3];
    u32 unused20;
    u32 packetSize;
};

// A blend skin's submodel (renderer.h's BlendSubModel): the parts and how many
struct HostBlendSubModel
{
    HostBlendPart** parts;
    u32 count;
};

// A blend skin as the game draws it (renderer.h's BlendSkin, 0x18 bytes):
// its submodels' materials, the submodels, how many and how many shapes
struct HostBlendSkin
{
    ResourceHeader header;
    MaterialResource** materials;
    HostBlendSubModel** subModels;
    u32 count;
    u32 shapeCount;
};

// A chunk's sky as the host holds it (src/platform/ps2/renderer/renderer.h's Sky, 0x50 bytes): its meshes (taken from
// the mesh table by ReadSky) and the matrix of the half-size buffer's screen this frame (StartSkyBuffer writes it,
// DrawSky reads it back onto the sky's own toScreen and through the camera's translation)
struct HostSky
{
    ResourceHeader header;
    u32 count;
    HostRigidModel** models;
    Matrix4x4 toScreen;
};
CHECK_OFFSET(HostSky, count, 0x8);

// A scenery level of detail (renderer.h's Lod, 0x24 bytes): its meshes, how
// many, and the squares of its distances (the nearest and farthest it's drawn
// at, and where each mesh gives way to the next)
struct HostLod
{
    ResourceHeader header;
    HostRigidModel** meshes;
    u8 count;
    u32 nearest;
    u32 farthest;
    u32 switchDistances[3];
};
CHECK_SIZE(HostRigidModelData, 0x1C);
CHECK_OFFSET(HostRigidModel, count, 0x18);
CHECK_SIZE(HostRigidModel, 0x20);
CHECK_OFFSET(HostSkin, count, 0x14);
CHECK_SIZE(HostSkin, 0x24);
CHECK_OFFSET(HostBlendPart, shapeFactors, 0x14);
CHECK_SIZE(HostBlendPart, 0x28);
CHECK_OFFSET(HostLod, nearest, 0x10);
CHECK_OFFSET(HostLod, switchDistances, 0x18);
CHECK_SIZE(HostLod, 0x24);

// The 3D meshes' bucket views: the opaque objects the rigid models and the
// skins draw with
constexpr bgfx::ViewId HostOpaqueView = 2;
constexpr bgfx::ViewId HostGlobalOpaqueView = 3;

// The bucket a material's draws go in, as the game's own FrameBucket numbers it (renderer.h: 2 opaque, 3 the startup
// chunk's objects and the skid marks, 5..19 blended, 20 decals, 22 particles, 23 distortion, 24 UI, 26 fonts, 27
// movies), unless the material named none: then the opaque bucket, which is where a material the host makes (a page's,
// a skid mark's) draws. The retail code picks the bucket when it reads a material from its file, and the frame's DMA
// chains run in bucket order, so this is what keeps a blended surface behind or in front of what it belongs behind or in
// front of, which one view for everything loses
constexpr u32 HostNoBucket = 0;
// The bgfx view a bucket's draws are submitted in: one view a bucket. Views 32 and up are
// the host's own passes, kept out of the buckets' range so every bucket keeps its own view
bgfx::ViewId HostViewForBucket(u32 bucket);

// A screen model's vertex (the strip the builder in graphics.cpp accumulates): the place the game gave (its fourth word
// is only ever the PS2's no-draw flag, which the host's AddScreenModelVertex never sets) and the vertex's RGBA bytes
// (SetScreenModelColour: two to four bytes each, the blend reading 0x80 as one)
struct HostScreenModelVertex
{
    f32 x;
    f32 y;
    f32 z;
    u32 colour;
};
CHECK_SIZE(HostScreenModelVertex, 0x10);

// A screen model as the host holds it (src/platform/ps2/renderer/renderer.h's ScreenModel, 0x1C bytes on the PS2: a
// material, a packet and the block of its instances). The game only ever keeps what EndScreenModel returned and hands it
// back to DrawScreenModel and DeleteScreenModel (skidmarks.cpp's marks) or to DeleteScreenModel alone, so the layout is
// the host's: the material the model draws with, the strip's vertexes and how many. A screen model of no vertexes is
// never made (the game always adds a strip's four before ending it), and its packet is null then
struct HostScreenModel
{
    Material* material;
    u8* packet;
    u32 packetSize;
    u32 count;
};
CHECK_OFFSET(HostScreenModel, packet, 0x4);
CHECK_OFFSET(HostScreenModel, count, 0xC);
CHECK_SIZE(HostScreenModel, 0x10);

// The strip a screen model holds, submitted through the shared 3D program: its world-space places by the model-to-clip
// matrix (the view's to-screen one), its own colours through the 1x1 white texture (no screen model is textured). The
// caller owns the view's rect and frame buffer (HostSceneSetView) and the state the model's material produced
// (HostMaterialStateOf: the marks' add or subtract preset, no depth writes, the destination test's depth gate). Returns
// whether it submitted (false: no model, no vertexes, no matrix)
bool HostDrawScreenModel(const HostScreenModel* model, const Matrix4x4* mvp, bgfx::ViewId view, u64 state,
                         const f32* alphaRef);

// The scene's offscreen target the 3D passes draw into (RGBA8 colour carrying the frame's alpha + D24S8 depth),
// made lazily at the window's size. The PS2 draws into its frame buffer and the shadow pass copies that frame's
// depth and alpha into the half-size buffer (shadows.cpp's StartShadows); the host needs the same sampleable copy,
// which the backbuffer cannot give. All scene passes (opaque, sky copy, particles, decals, distortion, and the
// shadows' resolve) set this as their frame buffer; HostCompositeScene copies it to the backbuffer at the frame's
// end. Invalid before bgfx is up
bgfx::FrameBufferHandle HostSceneTarget();
// The scene target's colour with no depth attached: a pass that samples the scene's depth (the fog filter) must
// draw into a target that does not bind that same depth texture
bgfx::FrameBufferHandle HostSceneColourTarget();
bgfx::TextureHandle HostSceneColour();
bgfx::TextureHandle HostSceneDepth();
// The scene target's own size, which a copy of it must match (bgfx clamps a blit to the smaller of the two regions,
// so copying the display's 512x512 out of the 960x540 scene takes only its bottom-left corner)
u32 HostSceneWidth();
u32 HostSceneHeight();

// The scene's depth only if the scene target is already made: a pass that merely asks whether the frame it draws is the scene's
// must not bring the scene target into being (which would take the frame target over) for a caller with a frame of its own
bgfx::TextureHandle HostSceneDepthIfMade();

bgfx::FrameBufferHandle HostSceneTargetIfMade();
// Binds the scene target to a view that should draw into it, at the window's size
void HostSceneSetView(bgfx::ViewId view);
// Copies the finished scene (its colour, the frame's alpha included) over the backbuffer
void HostCompositeScene();
// The target HostCompositeScene copies onto (invalid: the backbuffer); a headless proof sets its own to read back
void HostCompositeSetTarget(bgfx::FrameBufferHandle framebuffer);

// The shared 3D programs and their uniforms (made lazily on first use, after
// bgfx is up; owned by the process, torn down with bgfx itself)
bgfx::ProgramHandle HostDraw3DProgram();
bgfx::ProgramHandle HostDraw3DSkinProgram();
// The texture a rigid/skin submodel's material draws with: the material's bound texture, else the 1x1 white
// texture (the fragment shader's sample * colour then reduces to the vertex colour, the PS2's untextured shaders).
// The PS2 uploads each material's shader texture before its packet (materials.cpp's WriteShaders), so every pass
// that draws a resource's submodels must bind this rather than assume white
bgfx::TextureHandle HostSubModelTexture(MaterialResource** materials, u32 index);
// The chunk view's matrix to the screen the rigid draws without a view go
// through (DrawRigidModel/DrawSkin/DrawBlendSkin carry no view: on the PS2
// the instance block bakes the render view's to-screen in). Set by the
// culling owner as the views change; identity until anyone sets it
void HostSetViewMatrix(const Matrix4x4* toScreen);
const Matrix4x4* HostViewMatrix();

bgfx::UniformHandle HostDraw3DLightDirs();
bgfx::UniformHandle HostDraw3DLightColours();
bgfx::UniformHandle HostDraw3DAmbient();
bgfx::UniformHandle HostDraw3DJoints();
bgfx::UniformHandle HostDraw3DBlendWeights();

// Parses and uploads a rigid submodel's packet on first call (ParseRigidSubModel), then draws from the cache.
// The caller owns the view's rect, framebuffer and clear.
//
// alphaRef is the material's alpha test (HostMaterialStateOf); null means no alpha test, which the sky and shadow
// passes use. blendFactor is the constant for BGFX_STATE_BLEND_FACTOR states, in bgfx's 0xRRGGBBAA order; the shadow
// volumes use it to add a fixed 8/255 per shell. program overrides the rigid program when valid.
// minifiesThroughMips selects mip sampling (shadersettings.cpp's SettingsStyle); cleared, only the largest level is
// sampled, as the sky's shader (type 0x0A) requires. Only this draw's binding changes. Returns false when the
// packet is null or unparsable.
bool HostDrawRigidPacket(const u8* packet, u32 packetSize, u32 vertexCount, const Matrix4x4* mvp,
                         const Matrix4x4* world, const ModelLights* lights, bgfx::TextureHandle texture,
                         bgfx::ViewId view, u64 state, const f32* alphaRef = nullptr,
                         u32 blendFactor = UINT32_MAX, bgfx::ProgramHandle program = BGFX_INVALID_HANDLE,
                         bool minifiesThroughMips = true);

// The skin equivalent (parses with ParseSkinSubModel, uploads the joint
// matrices and the blend weights): joints are the jointCount matrices in the
// model's space, blendCount/blendWeights the DrawBlendSkin weights (0/NULL
// for plain skins). Same view-state ownership as the rigid path. A blend skin
// part's shapes (blendPart with blendShapeIds, its shape indexes parallel to
// the weights) bake their vertex offsets into the positions before the
// upload; those vertexes follow the draw's weights, so a draw with active
// shapes uploads a transient vertex buffer rather than the cached one
bool HostDrawSkinPacket(const u8* packet, u32 packetSize, u32 vertexCount, u32 shapeCount, const Matrix4x4* mvp,
                        const Matrix4x4* world, const ModelLights* lights, const Matrix4x4* joints, u32 jointCount,
                        const f32* blendWeights, u32 blendCount, bgfx::TextureHandle texture, bgfx::ViewId view,
                        u64 state, const f32* alphaRef = nullptr, const HostBlendPart* blendPart = nullptr,
                        const s32* blendShapeIds = nullptr);

// The shadow-caster mark: a draw's state with the frame's alpha forced to bit 7 = 0, so the shadow volumes skip the
// pixel (retail's DATE/DATM=0 test; the resolve's gate reads that bit). Opaque draws get the full mark, blended ones
// keep their RGB blend and lose only the alpha. Both the rigid and the skin draws call it
u64 MarkedForCaster(u64 state);

// The submodel's material's texture scroll, which the next 3D draw's vertex shader adds to every UV (retail's
// Shader::scroll). Call before each HostDrawRigidPacket/HostDrawSkinPacket; a null material clears it
void HostSetDrawScroll(const HostMaterial* material);
// The same, from a model's material array and submodel index (the way HostSubModelTexture names one), so the 3D
// loops and the sky set it in a line each
void HostSetSubModelScroll(MaterialResource** materials, u32 index);

// A model-to-clip matrix from the model's world matrix and the chunk view's
// matrices: the game's row-vector product (world then to-screen), passed
// through as-is for the column-vector mul(u_mvp, v) shader
void HostModelClipMatrix(const Matrix4x4* world, const Matrix4x4* toScreen, Matrix4x4* mvp);

// The submit proof's counters: how many rigid and skinned draws went through
// the cache this process, and the checksums of the vertex bytes uploaded
struct HostDrawCounters
{
    u32 rigidDraws;
    u32 skinDraws;
    u32 rigidChecksum;
    u32 skinChecksum;
};
const HostDrawCounters& HostDrawCounts();
}
