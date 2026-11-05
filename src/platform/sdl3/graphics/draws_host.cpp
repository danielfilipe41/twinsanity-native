// 3D draw submits: rigid models, skins and blend skins through the mesh_builder's parses, cached
// by packet handle, drawn with the instance's matrix and lights as uniforms. The game's matrices are row-vector
// (multiply from the left), bgfx's column-major, so the model-to-clip product is transposed on upload.
#include <cstdlib>

#include "draws_host.h"

#include "host_graphics.h"
#include "mesh_builder.h"

#include <cmath>
#include <deque>

#include "game/graphicstables.h"
#include "game/camerarig.h"
#include "game/followcamera.h"
#include "game/instances.h"
#include "game/place.h"
#include "game/reference.h"
#include "game/view.h"

#include "game/math.h"

#include "host_shaders.h"

// SMAA's area and search look-up tables (tools/build_smaa_textures.py): the generated arrays the weights pass indexes
#include "shaders/smaa_area_tex.bin.h"
#include "shaders/smaa_search_tex.bin.h"

using Platform::Graphics::HostCreateShader;
using Platform::Graphics::HostShader;

#include <cstdio>
#include <cstring>

namespace
{
// The shared 3D objects, made once: the rigid and skinned programs from the
// embedded blobs, the s_tex sampler and the light/matrix uniforms
bgfx::ProgramHandle g_rigidProgram = BGFX_INVALID_HANDLE;
bgfx::ProgramHandle g_skinProgram = BGFX_INVALID_HANDLE;
// The volumes' program: the rigid vertex shader with a fragment shader that writes one constant colour, so a shell's
// contribution is the pass's blend factor and not a fraction of the mesh's own colours (fs_shadowvolume.sc)
bgfx::ProgramHandle g_shadowVolumeProgram = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_sampler = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_mvp = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_lightDirs = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_lightColours = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_ambient = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_joints = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_blendWeights = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_alpha = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_scroll = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_materialColour = BGFX_INVALID_HANDLE;
bool g_made = false;

// The alpha test a draw with no material state uses: the fragment shader's u_materialAlpha set to ALWAYS, so a pass
// that draws geometry of its own (the sky, the shadow volumes) discards nothing. The draws that do have a material
// override it with the material's own ATST and AREF
const f32 g_noAlphaTest[4] = {static_cast<f32>(Platform::Graphics::HostAlphaAlways), 0.0f, 0.0f, 0.0f};

// The texture scroll the next 3D draw takes (the current submodel's material: HostSetDrawScroll sets it before each
// one). A material with no scrolling shader leaves it zero, so every other draw's UVs are unmoved
f32 g_drawScroll[4] = {0.0f, 0.0f, 0.0f, 0.0f};

// The colour a type-0x1F material's draw tints its lit vertex colour with (renderer.h's Shader::shaderColour, which that
// type's VU1 program loads and multiplies in). The fourth lane is the "set" flag: a draw whose submit leaves the uniform
// unwritten has it at zero, which the shader reads as no tint, so no draw can be blacked out by a missing value
f32 g_drawColour[4] = {1.0f, 1.0f, 1.0f, 1.0f};

// A cached upload: the packet bytes' identity plus the parsed kind, with the
// bgfx buffers the draws bind
struct DrawCacheEntry
{
    const u8* packet;
    u32 packetSize;
    bool skinned;
    bgfx::VertexBufferHandle vertices;
    bgfx::IndexBufferHandle indexes;
    u32 indexCount;
    u32 checksum;
    bool used;
};

// The uploads held, one entry per live packet. An entry is never destroyed while a draw may still bind it: deleting a
// packet's owner only retires its entries, and their bgfx buffers go a couple of frames later
// (HostCollectRetiredDrawBuffers), once the frames that could still bind them have run. A deque keeps entries'
// addresses stable as it grows, which the draws need while a packet is parsed.
std::deque<DrawCacheEntry> g_entries;
bool g_entriesMade = false;
Platform::Graphics::HostDrawCounters g_counters = {};

// A retired upload waiting for the frames that could still bind it to run
struct RetiredDrawBuffers
{
    bgfx::VertexBufferHandle vertices;
    bgfx::IndexBufferHandle indexes;
    u32 frame;
};
std::vector<RetiredDrawBuffers> g_retiredBuffers;
u32 g_drawFrame = 0;

// The host vertex layouts: position + UV + colour + normal, the skin's with
// the joint index/weight bytes (raw indices, normalized weights)
bgfx::VertexLayout RigidLayout()
{
    // The mesh_builder's MeshVertex order: position, UV, normal bytes, colour bytes
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal, 4, bgfx::AttribType::Uint8, true)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();
    return layout;
}

// The vertex a screen model's strip goes to the GPU as (HostDrawScreenModel: RigidLayout's own attributes, the
// mesh_builder's MeshVertex order without the joint bytes, so the shared 3D program draws it). The normal is the
// strip's own: a screen model has none (its material is untextured and unlit), so every vertex carries up, which the
// draw's ambient-only lights leave out of the fragment colour either way
struct ScreenModelVertexGpu
{
    f32 x;
    f32 y;
    f32 z;
    f32 u;
    f32 v;
    u8 normal[4];
    u8 colour[4];
};
CHECK_SIZE(ScreenModelVertexGpu, 0x1C);

// A colour channel of the game's bytes made the window's full range: the game's red, green and blue are full at 192 and
// its alpha at 128, scaled to the window's 255 -- mesh_builder.cpp's ScaleChannel, which every other 3D vertex the host
// draws goes through, kept local here because the mesh builder doesn't export it. The screen models' colours are the
// game's own bytes (SetScreenModelColour): every mark's red, green and blue is the white 0xFF, so 192 and the GS's own
// 0x80-is-one convention (draw2d.cpp's MultiplyFlat) agree on them, and the alpha -- the strip's shade, a small
// magnitude -- is the channel that varies
u8 ScaleScreenModelChannel(u8 channel, u32 full)
{
    const u32 scaled = static_cast<u32>(channel) * 255u / full;
    return static_cast<u8>(scaled > 255 ? 255 : scaled);
}

// The render view's matrix to the screen the view-less draws go through
// (identity until the culling owner sets it)
Matrix4x4 g_viewMatrix = {};
bool g_viewSet = false;

bgfx::VertexLayout SkinLayout()
{
    // The mesh_builder's Ps2SkinVertex order: position, UV, normal, colour, joint index floats (0-255),
    // then weight floats. Indices stored as floats to avoid GPU normalization of Uint8 attributes.
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal, 4, bgfx::AttribType::Uint8, true)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .add(bgfx::Attrib::Indices, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Weight, 3, bgfx::AttribType::Float)
        .end();
    return layout;
}

void MakeShared()
{
    if (g_made)
    {
        return;
    }

    g_made = true;
    bgfx::ShaderHandle rigidVs = HostCreateShader(HostShader::Draw3DVertex);
    bgfx::ShaderHandle skinVs = HostCreateShader(HostShader::Draw3DSkinVertex);
    bgfx::ShaderHandle fs = HostCreateShader(HostShader::Draw3DFragment);
    g_rigidProgram = bgfx::createProgram(rigidVs, fs, true);
    bgfx::ShaderHandle volumeVs = HostCreateShader(HostShader::Draw3DVertex);
    bgfx::ShaderHandle volumeFs = HostCreateShader(HostShader::ShadowVolumeFragment);
    g_shadowVolumeProgram = bgfx::createProgram(volumeVs, volumeFs, true);
    bgfx::ShaderHandle skinFs = HostCreateShader(HostShader::Draw3DFragment);
    g_skinProgram = bgfx::createProgram(skinVs, skinFs, true);
    g_sampler = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);
    g_mvp = bgfx::createUniform("u_mvp", bgfx::UniformType::Mat4);
    g_lightDirs = bgfx::createUniform("u_lightDirs", bgfx::UniformType::Vec4, 3);
    g_lightColours = bgfx::createUniform("u_lightCols", bgfx::UniformType::Vec4, 3);
    g_ambient = bgfx::createUniform("u_ambient", bgfx::UniformType::Vec4);
    g_joints = bgfx::createUniform("u_joints", bgfx::UniformType::Mat4, SkinMaxJoints);
    g_blendWeights = bgfx::createUniform("u_blendWeights", bgfx::UniformType::Vec4, 4);
    g_alpha = bgfx::createUniform("u_materialAlpha", bgfx::UniformType::Vec4);
    g_scroll = bgfx::createUniform("u_scroll", bgfx::UniformType::Vec4);
    g_materialColour = bgfx::createUniform("u_materialColour", bgfx::UniformType::Vec4);
}

void MakeEntries()
{
    if (g_entriesMade)
    {
        return;
    }

    g_entriesMade = true;
    // The handles in the retire list are the bgfx instance's that is gone with it
    g_retiredBuffers.clear();
    g_drawFrame = 0;
    for (DrawCacheEntry& entry : g_entries)
    {
        entry.packet = nullptr;
        entry.packetSize = 0;
        entry.skinned = false;
        entry.vertices.idx = bgfx::kInvalidHandle;
        entry.indexes.idx = bgfx::kInvalidHandle;
        entry.indexCount = 0;
        entry.checksum = 0;
        entry.used = false;
    }
}

// The entry of a packet (nullptr: none)
DrawCacheEntry* FindEntry(const u8* packet, u32 packetSize, bool skinned)
{
    for (DrawCacheEntry& entry : g_entries)
    {
        if (entry.used && entry.packet == packet && entry.packetSize == packetSize && entry.skinned == skinned)
        {
            return &entry;
        }
    }

    return nullptr;
}

// A free entry, else a new one. Entries are never evicted: a live upload must not be destroyed while a draw the
// frame has already queued still binds it (see g_entries' comment).
DrawCacheEntry* TakeEntry()
{
    for (DrawCacheEntry& entry : g_entries)
    {
        if (!entry.used)
        {
            return &entry;
        }
    }

    g_entries.emplace_back();
    DrawCacheEntry& entry = g_entries.back();
    entry.packet = nullptr;
    entry.packetSize = 0;
    entry.skinned = false;
    entry.vertices.idx = bgfx::kInvalidHandle;
    entry.indexes.idx = bgfx::kInvalidHandle;
    entry.indexCount = 0;
    entry.checksum = 0;
    entry.used = false;
    return &entry;
}

// The lights as shader vec4s: the model's rotation transposed takes a world light direction into the model's own space, which
// is what the shader's dot(normal, u_lightDirs) needs; the PS2 does the same in SetRigidModelRenderDMA (VuTransposeRotation
// then VuRotateVector per light). A draw without lights (the shadow pass, the sky, the proofs) keeps the neutral wash
// (no directional, ambient white), which reduces the shader's light sum to the vertex colour.
void LightUniforms(const Platform::Graphics::ModelLights* lights, const Matrix4x4* matrix, Vector4* dirs, Vector4* colours,
                   Vector4* ambient)
{
    if (lights == nullptr || matrix == nullptr || lights->directions == nullptr || lights->colours == nullptr ||
        lights->ambient == nullptr)
    {
        for (u32 light = 0; light < 3; light++)
        {
            dirs[light] = {0.0f, 0.0f, 1.0f, 0.0f};
            colours[light] = {0.0f, 0.0f, 0.0f, 0.0f};
        }

        *ambient = {1.0f, 1.0f, 1.0f, 1.0f};
        return;
    }

    for (u32 light = 0; light < 3; light++)
    {
        const Vector4& direction = lights->directions[light];
        dirs[light] = {matrix->m[0][0] * direction.x + matrix->m[1][0] * direction.y + matrix->m[2][0] * direction.z,
                       matrix->m[0][1] * direction.x + matrix->m[1][1] * direction.y + matrix->m[2][1] * direction.z,
                       matrix->m[0][2] * direction.x + matrix->m[1][2] * direction.y + matrix->m[2][2] * direction.z, 0.0f};
        colours[light] = lights->colours[light];
    }

    *ambient = *lights->ambient;
}

// Bakes a blend skin part's active shapes into its parsed vertex positions. Retail's WriteBlendPart
// (ps2/renderer/skins.cpp) sends each shape as one 4-byte element per vertex (signed x, y, z bytes), and the shape
// programs add element * factors * weight to the vertex. Active shapes add up. The fourth lane is the normal's X
// on the PS2, which the host's packed normal does not carry.
void BakeBlendShapes(const Platform::Graphics::HostBlendPart* part, const s32* shapeIds, const f32* weights,
                     u32 shapeCount, Ps2SkinMesh* mesh)
{
    auto* vertices = reinterpret_cast<Ps2SkinVertex*>(mesh->vertices.data());
    for (u32 shape = 0; shape < shapeCount; shape++)
    {
        const s32 id = shapeIds[shape];
        if (id < 0 || static_cast<u32>(id) >= part->shapeCount || part->shapeAddresses == nullptr ||
            part->shapeCounts == nullptr)
        {
            continue;  // a weight for a shape the table does not hold
        }

        const u8* bytes = reinterpret_cast<const u8*>(static_cast<size_t>(part->shapeAddresses[id]));
        if (bytes == nullptr)
        {
            continue;
        }

        // A short shape (or a longer one) covers the vertexes it has
        const u32 count = part->shapeCounts[id] < mesh->vertexCount ? part->shapeCounts[id] : mesh->vertexCount;
        const f32 factorX = part->shapeFactors[0] * weights[shape];
        const f32 factorY = part->shapeFactors[1] * weights[shape];
        const f32 factorZ = part->shapeFactors[2] * weights[shape];
        for (u32 vertex = 0; vertex < count; vertex++)
        {
            const s8* lanes = reinterpret_cast<const s8*>(bytes + vertex * 4);
            vertices[vertex].x += factorX * lanes[0];
            vertices[vertex].y += factorY * lanes[1];
            vertices[vertex].z += factorZ * lanes[2];
        }
    }
}
}

namespace Platform::Graphics
{
// The shadow-caster mark: a character's draw leaves the frame's alpha bit 7 = 0 so the shadow volumes skip it
// (retail's destination-alpha test, DATE/DATM=0; the resolve's gate reads that bit). An opaque draw has no RGB
// blend factors, so forcing only the alpha half would leave them at 0 -- enum 0 is not a valid blend factor and
// the driver draws black. Opaque draws take the full mark (RGB ONE/ZERO identity, alpha ZERO/ZERO writes 0),
// blended ones keep their RGB blend and lose only the alpha half (bits 20-27, srcA and dstA)
u64 MarkedForCaster(u64 state)
{
    constexpr u64 FullMark =
        BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ZERO, BGFX_STATE_BLEND_ZERO,
                                       BGFX_STATE_BLEND_ZERO);
    if ((state & BGFX_STATE_BLEND_MASK) == 0)
    {
        return state | FullMark;
    }

    constexpr u64 AlphaHalf = static_cast<u64>(0xFF) << 20;
    constexpr u64 ZeroAlpha =
        BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_ZERO, BGFX_STATE_BLEND_ZERO, BGFX_STATE_BLEND_ZERO,
                                       BGFX_STATE_BLEND_ZERO) &
        AlphaHalf;
    return (state & ~AlphaHalf) | ZeroAlpha;
}

bgfx::ProgramHandle HostDraw3DProgram()
{
    MakeShared();
    return g_rigidProgram;
}
bgfx::ProgramHandle HostShadowVolumeProgram()
{
    MakeShared();
    return g_shadowVolumeProgram;
}

bgfx::ProgramHandle HostDraw3DSkinProgram()
{
    MakeShared();
    return g_skinProgram;
}

bgfx::TextureHandle HostSubModelTexture(MaterialResource** materials, u32 index)
{
    if (materials == nullptr || materials[index] == nullptr || materials[index]->material == nullptr)
    {
        return HostWhiteTexture();
    }

    const HostMaterial* material = reinterpret_cast<HostMaterial*>(materials[index]->material);
    bgfx::TextureHandle texture = HostMaterialBoundTexture(material);
    return bgfx::isValid(texture) ? texture : HostWhiteTexture();
}

bgfx::UniformHandle HostDraw3DSampler()
{
    MakeShared();
    return g_sampler;
}

bgfx::UniformHandle HostDraw3DMvp()
{
    MakeShared();
    return g_mvp;
}

bgfx::UniformHandle HostDraw3DLightDirs()
{
    MakeShared();
    return g_lightDirs;
}

bgfx::UniformHandle HostDraw3DLightColours()
{
    MakeShared();
    return g_lightColours;
}

bgfx::UniformHandle HostDraw3DAmbient()
{
    MakeShared();
    return g_ambient;
}

bgfx::UniformHandle HostDraw3DJoints()
{
    MakeShared();
    return g_joints;
}

bgfx::UniformHandle HostDraw3DBlendWeights()
{
    MakeShared();
    return g_blendWeights;
}

bgfx::UniformHandle HostDraw3DAlphaUniform()
{
    MakeShared();
    return g_alpha;
}
void HostSetViewMatrix(const Matrix4x4* toScreen)
{
    if (toScreen == nullptr)
    {
        g_viewSet = false;
        return;
    }

    g_viewMatrix = *toScreen;
    g_viewSet = true;
}
const Matrix4x4* HostViewMatrix()
{
    if (!g_viewSet)
    {
        static Matrix4x4 identity = {};
        static bool made = false;
        if (!made)
        {
            made = true;
            identity.m[0][0] = 1.0f;
            identity.m[1][1] = 1.0f;
            identity.m[2][2] = 1.0f;
            identity.m[3][3] = 1.0f;
        }

        return &identity;
    }

    return &g_viewMatrix;
}

void HostModelClipMatrix(const Matrix4x4* world, const Matrix4x4* toScreen, Matrix4x4* mvp)
{
    // The game's row-vector product (world then the view's to-screen),
// passed through as-is: bgfx uploads the mat4 float array as GLSL columns,
// so mul(u_mvp, v) computes exactly the game's p*v (SkyPass-2 verified:
// the transposed upload rendered geometry transposed)
    for (u32 row = 0; row < 4; row++)
    {
        for (u32 column = 0; column < 4; column++)
        {
            mvp->m[row][column] = world->m[row][0] * toScreen->m[0][column] +
                                  world->m[row][1] * toScreen->m[1][column] +
                                  world->m[row][2] * toScreen->m[2][column] +
                                  world->m[row][3] * toScreen->m[3][column];
        }
    }
}

bool HostDrawRigidPacket(const u8* packet, u32 packetSize, u32 vertexCount, const Matrix4x4* mvp,
                         const Matrix4x4* world, const ModelLights* lights, bgfx::TextureHandle texture,
                         bgfx::ViewId view, u64 state, const f32* alphaRef, u32 blendFactor,
                         bgfx::ProgramHandle program, bool minifiesThroughMips)
{
    if (packet == nullptr || packetSize == 0 || mvp == nullptr)
    {
        return false;
    }

    MakeShared();
    MakeEntries();
    DrawCacheEntry* entry = FindEntry(packet, packetSize, false);
    if (entry == nullptr)
    {
        Ps2RigidMesh mesh;
        if (!ParseRigidSubModel(packet, packetSize, vertexCount, &mesh) || mesh.vertices.empty() ||
            mesh.indexes.empty())
        {
            return false;
        }

        entry = TakeEntry();
        const bgfx::Memory* vertices = bgfx::copy(mesh.vertices.data(), static_cast<u32>(mesh.vertices.size()));
        const bgfx::Memory* indexes =
            bgfx::copy(mesh.indexes.data(), static_cast<u32>(mesh.indexes.size() * sizeof(u16)));
        entry->vertices = bgfx::createVertexBuffer(vertices, RigidLayout());
        entry->indexes = bgfx::createIndexBuffer(indexes);
        if (!bgfx::isValid(entry->vertices) || !bgfx::isValid(entry->indexes))
        {
            return false;
        }

        entry->packet = packet;
        entry->packetSize = packetSize;
        entry->skinned = false;
        entry->indexCount = mesh.indexCount;
        entry->checksum = mesh.checksum;
        entry->used = true;
        g_counters.rigidChecksum ^= mesh.checksum;
    }

    Vector4 dirs[3];
    Vector4 colours[3];
    Vector4 ambient = {};
    LightUniforms(lights, world, dirs, colours, &ambient);
    // The material's own texture when it has one (the PS2 uploads each material's shader texture before its packet),
    // else the 1x1 white texture the fragment shader reduces to the vertex colour
    bgfx::setState(state, blendFactor);
    if (minifiesThroughMips)
    {
        bgfx::setTexture(0, g_sampler, bgfx::isValid(texture) ? texture : HostWhiteTexture());
    }
    else
    {
        // A shader that never minifies through a mip samples the largest level bilinearly (the GS's TEX1 minifying
        // filter is the shader's own, not a mip one; TT-Lab's MinifiesThroughMips(UnlitSkydome) is false). The bind
        // covers the largest level alone -- firstMip 0, one level -- and the sampler's LOD is clamped to it, so a
        // texture carrying a mip chain is read from level 0 exactly as the console reads it. The flags stay
        // UINT32_MAX (the cached texture's own: bilinear min/mag, MIP_POINT unreachable within one level) and the
        // level range rides the binding alone, so the texture's chain and every other draw of it are untouched
        bgfx::setTexture(0, g_sampler, bgfx::isValid(texture) ? texture : HostWhiteTexture(), 0, UINT16_MAX, 0, 1,
                         UINT32_MAX, 0, 0);
    }
    bgfx::setVertexBuffer(0, entry->vertices);
    bgfx::setIndexBuffer(entry->indexes);
    bgfx::setUniform(g_mvp, mvp, 1);
    bgfx::setUniform(g_lightDirs, dirs, 3);
    bgfx::setUniform(g_lightColours, colours, 3);
    bgfx::setUniform(g_ambient, &ambient, 1);
    bgfx::setUniform(g_scroll, g_drawScroll, 1);
    bgfx::setUniform(g_materialColour, g_drawColour, 1);
    // The uniform's w is the brighten flag, not AFAIL (which only the host reads, for the rejected samples' second
    // pass): a brighten draw's fragment shader must write its alpha as its colour for DST_COLOR, ONE to multiply the
    // background by 1 + As
    if (alphaRef != nullptr)
    {
        const f32 alpha[4] = {alphaRef[0], alphaRef[1], alphaRef[2], alphaRef[4]};
        bgfx::setUniform(g_alpha, alpha, 1);
    }
    else
    {
        bgfx::setUniform(g_alpha, g_noAlphaTest, 1);
    }
    bgfx::submit(view, bgfx::isValid(program) ? program : g_rigidProgram);
    g_counters.rigidDraws++;
    return true;
}

bool HostDrawSkinPacket(const u8* packet, u32 packetSize, u32 vertexCount, u32 shapeCount, const Matrix4x4* mvp,
                        const Matrix4x4* world, const ModelLights* lights, const Matrix4x4* joints, u32 jointCount,
                        const f32* blendWeights, u32 blendCount, bgfx::TextureHandle texture, bgfx::ViewId view,
                        u64 state, const f32* alphaRef, const HostBlendPart* blendPart, const s32* blendShapeIds)
{
    if (packet == nullptr || packetSize == 0 || mvp == nullptr || joints == nullptr || jointCount == 0)
    {
        return false;
    }

    // A blend skin part with shapes the draw gives a weight: this frame's
    // offsets bake into a copy of the parsed vertexes, so the draw cannot use
    // the packet keyed cache (its vertexes hold no shapes) and uploads the
    // copy into a transient buffer instead. Without active shapes the cached
    // upload is the whole mesh, exactly what a plain skin draws
    const bool shaped = blendPart != nullptr && blendShapeIds != nullptr && blendWeights != nullptr &&
                        blendCount != 0 && blendPart->shapeCount != 0;

    MakeShared();
    MakeEntries();
    DrawCacheEntry* entry = FindEntry(packet, packetSize, true);
    Ps2SkinMesh mesh;
    if (entry == nullptr || shaped)
    {
        if (!ParseSkinSubModel(packet, packetSize, vertexCount, shapeCount, &mesh) || mesh.vertices.empty() ||
            mesh.indexes.empty())
        {
            return false;
        }
    }

    if (entry == nullptr)
    {
        entry = TakeEntry();
        const bgfx::Memory* vertices = bgfx::copy(mesh.vertices.data(), static_cast<u32>(mesh.vertices.size()));
        const bgfx::Memory* indexes =
            bgfx::copy(mesh.indexes.data(), static_cast<u32>(mesh.indexes.size() * sizeof(u16)));
        entry->vertices = bgfx::createVertexBuffer(vertices, SkinLayout());
        entry->indexes = bgfx::createIndexBuffer(indexes);
        if (!bgfx::isValid(entry->vertices) || !bgfx::isValid(entry->indexes))
        {
            return false;
        }

        entry->packet = packet;
        entry->packetSize = packetSize;
        entry->skinned = true;
        entry->indexCount = mesh.indexCount;
        entry->checksum = mesh.checksum;
        entry->used = true;
        g_counters.skinChecksum ^= mesh.checksum;
    }

    // The cached upload holds the mesh without shapes (a cache entry outlives
    // the frame's weights): the draw's own offsets go into the parsed copy
    if (shaped)
    {
        BakeBlendShapes(blendPart, blendShapeIds, blendWeights, blendCount, &mesh);
    }

    // The joint matrices upload as-is (same row-vector convention as the
    // mvp); past SkinMaxJoints the tail is dropped (the retail skins' indices
    // stop at 61), short of it the rest is identity
    static Matrix4x4 jointUniforms[SkinMaxJoints];
    for (u32 joint = 0; joint < SkinMaxJoints; joint++)
    {
        if (joint < jointCount)
        {
            jointUniforms[joint] = joints[joint];
        }
        else
        {
            // An unused slot: the identity, so a vertex naming one (an index past the skin's own joint count, which a
            // well-formed mesh never does) lands at the model's origin rather than taking joint 0's transform
            std::memset(&jointUniforms[joint], 0, sizeof(Matrix4x4));
            jointUniforms[joint].m[0][0] = 1.0f;
            jointUniforms[joint].m[1][1] = 1.0f;
            jointUniforms[joint].m[2][2] = 1.0f;
            jointUniforms[joint].m[3][3] = 1.0f;
        }
    }

    Vector4 blendUniforms[4] = {};
    for (u32 blend = 0; blend < 4 && blend < blendCount && blendWeights != nullptr; blend++)
    {
        blendUniforms[blend].x = blendWeights[blend];
    }

    SkinJointUploadNeeded(static_cast<u32>(reinterpret_cast<size_t>(joints)));
    Vector4 dirs[3];
    Vector4 colours[3];
    Vector4 ambient = {};
    LightUniforms(lights, world, dirs, colours, &ambient);
    // The material's own texture, else the 1x1 white texture the fragment shader reduces to the vertex colour.
    //
    // Shadow-caster mark: the draw leaves the frame alpha's bit 7 at 0, which retail's DATE/DATM test uses to keep
    // a caster out of its own shadow. The RGB is unchanged; only the written alpha is forced to zero. Opaque draws
    // need the full mark (identity RGB, alpha ZERO/ZERO), since forcing only the alpha half leaves the RGB factors at
    // enum 0 and the driver draws black.
    bgfx::setState(MarkedForCaster(state));
    bgfx::setTexture(0, g_sampler, bgfx::isValid(texture) ? texture : HostWhiteTexture());
    if (shaped)
    {
        // This frame's vertexes only (the cache's index buffer still holds:
        // the shapes move positions, never the triangle list)
        bgfx::TransientVertexBuffer bound;
        bgfx::allocTransientVertexBuffer(&bound, mesh.vertexCount, SkinLayout());
        if (bound.data == nullptr)
        {
            return false;  // the frame's transient pool is full: nothing to draw with
        }

        std::memcpy(bound.data, mesh.vertices.data(), mesh.vertices.size());
        bgfx::setVertexBuffer(0, &bound);
        bgfx::setIndexBuffer(entry->indexes);
    }
    else
    {
        bgfx::setVertexBuffer(0, entry->vertices);
        bgfx::setIndexBuffer(entry->indexes);
    }

    bgfx::setUniform(g_mvp, mvp, 1);
    bgfx::setUniform(g_lightDirs, dirs, 3);
    bgfx::setUniform(g_lightColours, colours, 3);
    bgfx::setUniform(g_ambient, &ambient, 1);
    bgfx::setUniform(g_joints, jointUniforms, SkinMaxJoints);
    bgfx::setUniform(g_blendWeights, blendUniforms, 4);
    bgfx::setUniform(g_scroll, g_drawScroll, 1);
    bgfx::setUniform(g_materialColour, g_drawColour, 1);
    if (alphaRef != nullptr)
    {
        const f32 alpha[4] = {alphaRef[0], alphaRef[1], alphaRef[2], alphaRef[4]};
        bgfx::setUniform(g_alpha, alpha, 1);
    }
    else
    {
        bgfx::setUniform(g_alpha, g_noAlphaTest, 1);
    }
    bgfx::submit(view, g_skinProgram);
    g_counters.skinDraws++;
    return true;
}

bool HostDrawScreenModel(const HostScreenModel* model, const Matrix4x4* mvp, bgfx::ViewId view, u64 state,
                         const f32* alphaRef)
{
    // A screen model's strip (the skid marks are the game's only models): its world-space places through the
    // model-to-clip matrix, drawn with the vertexes' own colours through the 1x1 white texture. Unlike the rigid and
    // skin paths there is no packet to parse and cache -- the model's bytes already are the strip the game's builder
    // laid out (HostScreenModelVertex) -- so the vertices go into a transient buffer a draw
    if (model == nullptr || model->packet == nullptr || model->count == 0 || mvp == nullptr)
    {
        return false;
    }

    MakeShared();
    const bgfx::VertexLayout layout = RigidLayout();
    // The transient vertex buffer the strip is written into: bgfx clamps the request to what is left of the frame's
    // transient space, so a draw whose vertices do not all fit writes past it -- a strip that comes up short is not drawn
    // at all (the particle and decal paths bail the same way)
    if (bgfx::getAvailTransientVertexBuffer(model->count, layout) != model->count)
    {
        return false;
    }

    bgfx::TransientVertexBuffer buffer;
    bgfx::allocTransientVertexBuffer(&buffer, model->count, layout);
    const u16 stride = layout.getStride();
    const auto* vertices = reinterpret_cast<const HostScreenModelVertex*>(model->packet);
    for (u32 index = 0; index < model->count; index++)
    {
        const HostScreenModelVertex& source = vertices[index];
        auto* vertex = reinterpret_cast<ScreenModelVertexGpu*>(buffer.data + static_cast<size_t>(index) * stride);
        vertex->x = source.x;
        vertex->y = source.y;
        vertex->z = source.z;
        // No screen model is textured (nothing gives the PS2's builder a texture coordinate either), so the UVs are
        // the white texel's
        vertex->u = 0.0f;
        vertex->v = 0.0f;
        // Up: the strip has no normal of its own and the lights below leave it out of the colour
        vertex->normal[0] = 128;
        vertex->normal[1] = 255;
        vertex->normal[2] = 128;
        vertex->normal[3] = 255;
        // The colour's bytes made the window's full range as every 3D vertex the host draws has them (the helper above:
        // the game's red, green and blue full at 192, its alpha at 128)
        vertex->colour[0] = ScaleScreenModelChannel(static_cast<u8>(source.colour), 255);
        vertex->colour[1] = ScaleScreenModelChannel(static_cast<u8>(source.colour >> 8), 255);
        vertex->colour[2] = ScaleScreenModelChannel(static_cast<u8>(source.colour >> 16), 255);
        vertex->colour[3] = ScaleScreenModelChannel(static_cast<u8>(source.colour >> 24), 128);
    }

    // The strip's lights: the ambient white and no directionals, so the shared 3D shader reduces to the vertex colours
    // (its lighting is the ambient plus the three NdotL terms)
    const Vector4 noDirections[3] = {};
    const Vector4 noLightColours[3] = {};
    const Vector4 ambient = {1.0f, 1.0f, 1.0f, 1.0f};
    const auto* material = reinterpret_cast<const HostMaterial*>(model->material);
    // The material's scroll first (a screen model's own material has none, but HostSetDrawScroll also clears it after
    // a scrolling 3D draw), then the fragment shader's alpha test the caller mapped from the material
    HostSetDrawScroll(material);
    const bgfx::TextureHandle texture = HostMaterialBoundTexture(material);
    bgfx::setState(state | BGFX_STATE_PT_TRISTRIP);
    bgfx::setTexture(0, g_sampler, bgfx::isValid(texture) ? texture : HostWhiteTexture());
    bgfx::setVertexBuffer(0, &buffer);
    bgfx::setUniform(g_mvp, mvp, 1);
    bgfx::setUniform(g_lightDirs, noDirections, 3);
    bgfx::setUniform(g_lightColours, noLightColours, 3);
    bgfx::setUniform(g_ambient, &ambient, 1);
    bgfx::setUniform(g_scroll, g_drawScroll, 1);
    bgfx::setUniform(g_materialColour, g_drawColour, 1);
    if (alphaRef != nullptr)
    {
        const f32 alpha[4] = {alphaRef[0], alphaRef[1], alphaRef[2], alphaRef[4]};
        bgfx::setUniform(g_alpha, alpha, 1);
    }
    else
    {
        bgfx::setUniform(g_alpha, g_noAlphaTest, 1);
    }
    bgfx::submit(view, g_rigidProgram);
    return true;
}

const HostDrawCounters& HostDrawCounts()
{
    return g_counters;
}


void HostSetSubModelScroll(MaterialResource** materials, u32 index)
{
    // The submodel's material's scroll for the draw about to be submitted (HostSubModelTexture's sibling: the 3D
    // loops and the sky both name their materials the same way)
    const Platform::Graphics::HostMaterial* material = nullptr;
    if (materials != nullptr && materials[index] != nullptr && materials[index]->material != nullptr)
    {
        material = reinterpret_cast<const Platform::Graphics::HostMaterial*>(materials[index]->material);
    }
    HostSetDrawScroll(material);
}

void HostSetDrawScroll(const Platform::Graphics::HostMaterial* material)
{
    // The submodel's material's scroll, which the next 3D draw's vertex shader adds to every UV (the retail
    // Shader::scroll WriteScroll hands the VU1 as the fourth component of the scroll uniform, the offset the
    // animated scrolls and the animation's own UV offset produce). A material without scroll state (or with no
    // resource) leaves the draw unmoved
    if (material == nullptr)
    {
        g_drawScroll[0] = 0.0f;
        g_drawScroll[1] = 0.0f;
        g_drawScroll[2] = 0.0f;
        g_drawScroll[3] = 0.0f;
        g_drawColour[0] = 1.0f;
        g_drawColour[1] = 1.0f;
        g_drawColour[2] = 1.0f;
        g_drawColour[3] = 1.0f;
        return;
    }

    g_drawScroll[0] = material->scroll[0];
    g_drawScroll[1] = material->scroll[1];
    g_drawScroll[2] = 0.0f;
    g_drawScroll[3] = 0.0f;
    // The material's colour reaches the vertex shader for the one type whose program reads it (0x1F); anything else tints
    // with nothing, which is the white the flag lane marks as set
    constexpr u32 ShaderTypeMaterialColour = 0x1F;
    const bool tints = material->shaderType == ShaderTypeMaterialColour;
    g_drawColour[0] = tints ? static_cast<f32>(material->colour[0]) * (1.0f / 255.0f) : 1.0f;
    g_drawColour[1] = tints ? static_cast<f32>(material->colour[1]) * (1.0f / 255.0f) : 1.0f;
    g_drawColour[2] = tints ? static_cast<f32>(material->colour[2]) * (1.0f / 255.0f) : 1.0f;
    g_drawColour[3] = 1.0f;
}

// The scene's offscreen target and its composite over the backbuffer

namespace
{
bgfx::FrameBufferHandle g_sceneTarget = BGFX_INVALID_HANDLE;
bgfx::TextureHandle g_sceneColour = BGFX_INVALID_HANDLE;
bgfx::TextureHandle g_sceneDepth = BGFX_INVALID_HANDLE;
// The same colour, attached without the depth: a pass that samples the scene's depth cannot sample it while it is
// bound as the target's own depth (GL's feedback loop), so the fog filter draws into this one instead
bgfx::FrameBufferHandle g_sceneColourTarget = BGFX_INVALID_HANDLE;
u32 g_sceneWidth = 0;
u32 g_sceneHeight = 0;
// The composite's fullscreen triangle and the program copying the scene colour over the backbuffer
bgfx::ProgramHandle g_compositeProgram = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_compositeSampler = BGFX_INVALID_HANDLE;
bgfx::VertexBufferHandle g_compositeTriangle = BGFX_INVALID_HANDLE;
// The display's offset the triangle was built at, in pixels (Platform::Graphics::MoveDisplay's, handed over by
// HostSetDisplayOffset): the triangle is remade when it moves
f32 g_compositeOffsetX = 0.0f;
f32 g_compositeOffsetY = 0.0f;
// MoveDisplay's own offset: how far the picture moves as a fraction of the frame each way (the composite turns it into
// pixels of the frame it is showing, so a frame of any size moves the picture the same share of itself)
f32 g_displayOffsetAcross = 0.0f;
f32 g_displayOffsetDown = 0.0f;
// Where the composite lands (invalid: the backbuffer); a headless proof sets its own target to read the frame back
bgfx::FrameBufferHandle g_compositeTarget = BGFX_INVALID_HANDLE;
constexpr bgfx::ViewId HostCompositeView = 31;

// ---- Anti-aliasing ----
// The settings file's `antialias` line (bgfx_context.h's AntiAlias) asks for no antialiasing, FXAA or SMAA: post passes
// over the composited frame that find its edges and blend across them. A post pass reads the composite's output, so the composite has to land in a
// render target of its own first: that is g_aaInput, and the pass writes where the composite would have.
constexpr bgfx::ViewId HostAaEdgesView = 35;
constexpr bgfx::ViewId HostAaWeightsView = 36;
constexpr bgfx::ViewId HostAaBlendView = 37;
// SMAA's tables' sizes (tools/build_smaa_textures.py's arrays)
constexpr u16 SmaaAreaWidth = 160;
constexpr u16 SmaaAreaHeight = 560;
constexpr u16 SmaaSearchWidth = 64;
constexpr u16 SmaaSearchHeight = 16;

// The composite's target while a post pass follows it, and the SMAA intermediates: the edges texture the weights pass
// searches, and the weights the blend pass reads
bgfx::FrameBufferHandle g_aaInput = BGFX_INVALID_HANDLE;
bgfx::TextureHandle g_aaInputColour = BGFX_INVALID_HANDLE;
bgfx::FrameBufferHandle g_aaEdges = BGFX_INVALID_HANDLE;
bgfx::FrameBufferHandle g_aaWeights = BGFX_INVALID_HANDLE;
bgfx::TextureHandle g_smaaArea = BGFX_INVALID_HANDLE;
bgfx::TextureHandle g_smaaSearch = BGFX_INVALID_HANDLE;
// What the post passes draw with, made on first use: one program per pass, the u_aaPt uniform all of them take, the
// samplers two of them declare, and their fullscreen triangles
bool g_aaMade = false;
// The backbuffer's size the post passes' targets were made at: the frame buffer is the backbuffer's size and draws use pixel
// fractions of it, so a resized window (or a display mode the window moved to) needs them remade, or the passes stretch the
// picture across the new one
u32 g_aaWidth = 0;
u32 g_aaHeight = 0;
bgfx::ProgramHandle g_aaFxaa = BGFX_INVALID_HANDLE;
bgfx::ProgramHandle g_aaSmaaEdges = BGFX_INVALID_HANDLE;
bgfx::ProgramHandle g_aaSmaaWeights = BGFX_INVALID_HANDLE;
bgfx::ProgramHandle g_aaSmaaBlend = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_aaPt = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_aaEdgesSampler = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_aaAreaSampler = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_aaSearchSampler = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_aaBlendSampler = BGFX_INVALID_HANDLE;
bgfx::VertexBufferHandle g_aaTriangleTarget = BGFX_INVALID_HANDLE;
bgfx::VertexBufferHandle g_aaTriangleScreen = BGFX_INVALID_HANDLE;
// Whether the composited picture is fitted (its 16:9 kept, bars around it) rather than stretched over the window: the
// composite's view clears when it is, so the bars are black
bool g_pictureFit = false;
// What the composite's triangle was made for: the offsets, the fit and the backbuffer's size it was built at
f32 g_compositeFitX = 1.0f;
f32 g_compositeFitY = 1.0f;
u32 g_compositeWidth = 0;
u32 g_compositeHeight = 0;

struct CompositeVertex
{
    f32 x;
    f32 y;
    f32 z;
    f32 u;
    f32 v;
    // White, so the 2D program's sample * colour reduces to the sample (the composite reuses that program rather
    // than a second one); without this attribute the fragment colour would be undefined
    f32 red;
    f32 green;
    f32 blue;
    f32 alpha;
};

bgfx::VertexLayout CompositeLayout()
{
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Float)
        .end();
    return layout;
}

// The scene target at the window's size, made (or remade on a size change)
void EnsureSceneTarget()
{
    // The scene's own size: the design's 960x540 times the resolution scale (Platform::Display's), not the window's. The
    // composite stretches it over the backbuffer, so the scene is the picture's detail and the window is its size
    u32 width = BgfxSceneWidth();
    u32 height = BgfxSceneHeight();
    if (bgfx::isValid(g_sceneTarget) && width == g_sceneWidth && height == g_sceneHeight)
    {
        return;
    }
    if (bgfx::isValid(g_sceneTarget))
    {
        // The colour-only target references the scene colour, so it goes first: destroying the scene target first
        // would free the texture out from under it
        if (bgfx::isValid(g_sceneColourTarget))
        {
            bgfx::destroy(g_sceneColourTarget);
            g_sceneColourTarget = BGFX_INVALID_HANDLE;
        }
        bgfx::destroy(g_sceneTarget);
        g_sceneTarget = BGFX_INVALID_HANDLE;
        g_sceneColour = BGFX_INVALID_HANDLE;
        g_sceneDepth = BGFX_INVALID_HANDLE;
        g_sceneColourTarget = BGFX_INVALID_HANDLE;
    }
    g_sceneWidth = width;
    g_sceneHeight = height;
    // The colour is sampled by the composite and by the shadow pass's alpha copy, so it clamps (no wrap bleed)
    g_sceneColour = bgfx::createTexture2D(static_cast<u16>(width), static_cast<u16>(height), false, 1,
                                          bgfx::TextureFormat::RGBA8,
                                          BGFX_TEXTURE_RT | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
                                              BGFX_SAMPLER_MIP_POINT |
                                              BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    // The scene's depth: the shadow pass samples it for the resolve's ramp (it is also the depth test the 3D passes
    // rely on, which is why the scene has to be one target rather than the backbuffer). The format has to be one the
    // renderer can both attach and sample, which D24S8 is not on every Vulkan driver (AMD's radv exposes it as
    // attachment-only); D32F carries the same normalized depth and samples everywhere, so the first format the
    // renderer accepts is taken
    static constexpr bgfx::TextureFormat::Enum DepthFormats[] = {bgfx::TextureFormat::D24S8,
                                                                 bgfx::TextureFormat::D32F,
                                                                 bgfx::TextureFormat::D16};
    constexpr u64 DepthFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
                           BGFX_SAMPLER_MIP_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    for (bgfx::TextureFormat::Enum format : DepthFormats)
    {
        g_sceneDepth =
            bgfx::createTexture2D(static_cast<u16>(width), static_cast<u16>(height), false, 1, format, DepthFlags);
        if (bgfx::isValid(g_sceneDepth))
        {
            break;
        }
    }
    if (!bgfx::isValid(g_sceneColour) || !bgfx::isValid(g_sceneDepth))
    {
        // Fail here rather than let createFrameBuffer walk an invalid handle
        std::fprintf(stderr, "bgfx: no scene target format the renderer accepts\n");
        return;
    }
    bgfx::TextureHandle handles[2] = {g_sceneColour, g_sceneDepth};
    g_sceneTarget = bgfx::createFrameBuffer(2, handles, true);
    // The shadow resolve darkens the frame as it is drawn, and the frame is this target (the composite at the frame's end
    // copies it out): without this the resolve would land on the backbuffer and be covered by that composite, which is how
    // shadows came out as nothing at all
    Platform::Graphics::HostShadowSetFrameTarget(g_sceneTarget, width, height, true);
    // The same colour with no depth attachment: the fog filter samples the scene's depth texture, which cannot also
    // be bound to the framebuffer being drawn into, so the filter's pass takes this target. The colour handle is not
    // owned (false), the scene target still owns it
    bgfx::TextureHandle colourOnly[1] = {g_sceneColour};
    g_sceneColourTarget = bgfx::createFrameBuffer(1, colourOnly, false);
}

void EnsureComposite()
{
    if (bgfx::isValid(g_compositeProgram))
    {
        return;
    }
    // A program of its own: the 2D quad's program multiplies the sample by the vertex colour, and the composite's
    // alpha must be forced opaque, so it cannot share that one
    bgfx::ShaderHandle vs = HostCreateShader(HostShader::SceneCompositeVertex);
    bgfx::ShaderHandle fs = HostCreateShader(HostShader::SceneCompositeFragment);
    g_compositeProgram = bgfx::createProgram(vs, fs, true);
    g_compositeSampler = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);
}

// The composite's triangle with the display's offset (Platform::Graphics::MoveDisplay) added to its clip-space
// positions: the whole picture slides and the strip it uncovers keeps the frame's clear colour, which is what the PS2's
// GS does when its display window moves on the TV. The buffer is permanent, not transient (a transient one is refused
// on a frame that has already spent its transient pool, the intermittent magenta flash), and is remade only when the
// offset moves, which is a rare event (the debug menu's screen-position page, a loaded save)
void EnsureCompositeTriangle(f32 offsetX, f32 offsetY)
{
    // The fitted picture's scale: the scene's shape put in the window's. Stretched it is 1 either way (the picture fills the
    // window, however its shape); fitted the smaller of the two fills one axis and the other gets bars
    const f32 sceneAspect = static_cast<f32>(BgfxSceneWidth()) / static_cast<f32>(BgfxSceneHeight());
    const f32 windowAspect = static_cast<f32>(BgfxBackbufferWidth()) / static_cast<f32>(BgfxBackbufferHeight());
    const bool fit = BgfxPictureFit();
    g_pictureFit = fit && std::fabs(sceneAspect - windowAspect) > 0.001f;
    const f32 fitX = g_pictureFit && windowAspect > sceneAspect ? sceneAspect / windowAspect : 1.0f;
    const f32 fitY = g_pictureFit && windowAspect < sceneAspect ? windowAspect / sceneAspect : 1.0f;
    if (bgfx::isValid(g_compositeTriangle) && offsetX == g_compositeOffsetX && offsetY == g_compositeOffsetY &&
        fitX == g_compositeFitX && fitY == g_compositeFitY && BgfxBackbufferWidth() == g_compositeWidth &&
        BgfxBackbufferHeight() == g_compositeHeight)
    {
        return;
    }

    if (bgfx::isValid(g_compositeTriangle))
    {
        bgfx::destroy(g_compositeTriangle);
        g_compositeTriangle = BGFX_INVALID_HANDLE;
    }

    // A frame pixel is two clip units across (NDC runs -1 to 1 over the frame). The composite is the only pass that
    // lands on the backbuffer (every other pass draws into the scene target, whose v bgfx measures from the same edge on
    // every backend), so it alone has to answer the backend's clip-space Y: Vulkan and the other Direct3D-style backends
    // put -1 at the top where OpenGL puts +1, and mirroring the triangle's Y here is what shows the picture upright
    // there. The UVs travel with their vertices, so they need no separate mirror. On OpenGL the triangle is exactly the
    // one it was
    const bool yDown = Platform::Graphics::HostClipSpaceYDown();
    const f32 ySign = yDown ? -1.0f : 1.0f;
    const f32 clipX = 2.0f * offsetX / static_cast<f32>(BgfxBackbufferWidth());
    // The offset counts down the frame, which is +Y in a y-down backend's clip space and -Y in OpenGL's
    const f32 clipY = (yDown ? 2.0f : -2.0f) * offsetY / static_cast<f32>(BgfxBackbufferHeight());
    // The fit scales the triangle's clip positions (and not its UVs, which stay 0..2 so the picture is still the whole scene)
    const CompositeVertex triangle[3] = {
        {(-1.0f + clipX) * fitX, (ySign * -1.0f + clipY) * fitY, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
        {(3.0f + clipX) * fitX, (ySign * -1.0f + clipY) * fitY, 0.0f, 2.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
        {(-1.0f + clipX) * fitX, (ySign * 3.0f + clipY) * fitY, 0.0f, 0.0f, 2.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    };
    const bgfx::Memory* memory = bgfx::copy(triangle, sizeof(triangle));
    g_compositeTriangle = bgfx::createVertexBuffer(memory, CompositeLayout());
    g_compositeOffsetX = offsetX;
    g_compositeOffsetY = offsetY;
    g_compositeFitX = fitX;
    g_compositeFitY = fitY;
    // Fitted, the picture covers part of the window and the rest is the bars: said once per rebuild, so a log tells the shape
    if (g_pictureFit)
    {
        std::fprintf(stderr, "bgfx: picture %.0fx%.0f centred in %ux%u (fit)\n",
                     static_cast<f32>(BgfxBackbufferWidth()) * fitX, static_cast<f32>(BgfxBackbufferHeight()) * fitY,
                     BgfxBackbufferWidth(), BgfxBackbufferHeight());
    }
    g_compositeWidth = BgfxBackbufferWidth();
    g_compositeHeight = BgfxBackbufferHeight();
}

// The post passes' fullscreen triangle, in the composite's own layout (its vertex shader is what every one of them draws
// with, so the vertex has to look like the composite's own): yDown selects the composite's mirroring, which is what a pass
// landing on the backbuffer needs and what a pass writing a render target must not have
bgfx::VertexBufferHandle MakeAaTriangle(bool yDown)
{
    const f32 ySign = yDown ? -1.0f : 1.0f;
    const CompositeVertex triangle[3] = {
        {-1.0f, ySign * -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
        {3.0f, ySign * -1.0f, 0.0f, 2.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
        {-1.0f, ySign * 3.0f, 0.0f, 0.0f, 2.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    };
    const bgfx::Memory* memory = bgfx::copy(triangle, sizeof(triangle));
    return bgfx::createVertexBuffer(memory, CompositeLayout());
}

// A post pass's program: the composite's vertex shader (a fullscreen triangle whose texture fractions come from its clip
// position) and the pass's own fragment shader
bgfx::ProgramHandle MakeAaProgram(Platform::Graphics::HostShader fragment)
{
    bgfx::ShaderHandle vertex = HostCreateShader(HostShader::SceneCompositeVertex);
    return bgfx::createProgram(vertex, HostCreateShader(fragment), true);
}

// The post passes' target and objects, made on first use and only what the mode asked for
void EnsureAntiAliasing()
{
    // A backbuffer of another size: the targets are the frame's and must not outlive it
    if (g_aaMade && (g_aaWidth != BgfxBackbufferWidth() || g_aaHeight != BgfxBackbufferHeight()))
    {
        Platform::Graphics::HostInvalidateAntiAliasing();
    }

    if (g_aaMade)
    {
        return;
    }

    g_aaMade = true;
    g_aaWidth = BgfxBackbufferWidth();
    g_aaHeight = BgfxBackbufferHeight();

    const u16 width = static_cast<u16>(g_aaWidth);
    const u16 height = static_cast<u16>(g_aaHeight);
    // Linear and clamped: a post pass reads its source at fractions of a texel
    const u64 sampleFlags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    g_aaInput = bgfx::createFrameBuffer(width, height, bgfx::TextureFormat::RGBA8, sampleFlags);
    g_aaInputColour = bgfx::getTexture(g_aaInput);
    g_aaPt = bgfx::createUniform("u_aaPt", bgfx::UniformType::Vec4);
    g_aaTriangleTarget = MakeAaTriangle(false);
    g_aaTriangleScreen = MakeAaTriangle(Platform::Graphics::HostClipSpaceYDown());

    const AntiAlias mode = HostAntiAliasing();
    if (mode == AntiAlias::Fxaa)
    {
        g_aaFxaa = MakeAaProgram(HostShader::FxaaFragment);
        return;
    }
    if (mode != AntiAlias::Smaa)
    {
        return;
    }

    g_aaSmaaEdges = MakeAaProgram(HostShader::SmaaEdgeFragment);
    g_aaSmaaWeights = MakeAaProgram(HostShader::SmaaWeightsFragment);
    g_aaSmaaBlend = MakeAaProgram(HostShader::SmaaBlendFragment);
    g_aaEdgesSampler = bgfx::createUniform("s_edges", bgfx::UniformType::Sampler);
    g_aaAreaSampler = bgfx::createUniform("s_area", bgfx::UniformType::Sampler);
    g_aaSearchSampler = bgfx::createUniform("s_search", bgfx::UniformType::Sampler);
    g_aaBlendSampler = bgfx::createUniform("s_blend", bgfx::UniformType::Sampler);
    g_aaEdges = bgfx::createFrameBuffer(width, height, bgfx::TextureFormat::RGBA8, sampleFlags);
    g_aaWeights = bgfx::createFrameBuffer(width, height, bgfx::TextureFormat::RGBA8, sampleFlags);
    // SMAA's two tables, from the arrays the generator wrote: the area table the weights pass indexes by edge shape and
    // distance, and the search table that says how far a pattern runs. Both are look-ups, so they clamp: wrapping would
    // read another row's weights
    g_smaaArea = bgfx::createTexture2D(SmaaAreaWidth, SmaaAreaHeight, false, 1, bgfx::TextureFormat::RGBA8, sampleFlags,
                                       bgfx::copy(smaa_area_tex, sizeof(smaa_area_tex)));
    g_smaaSearch = bgfx::createTexture2D(SmaaSearchWidth, SmaaSearchHeight, false, 1, bgfx::TextureFormat::R8,
                                         sampleFlags, bgfx::copy(smaa_search_tex, sizeof(smaa_search_tex)));
}

// Submits the mode's passes, once the composite has filled g_aaInput
void SubmitAntiAliasing()
{
    EnsureAntiAliasing();
    EnsureComposite();
    const AntiAlias mode = HostAntiAliasing();
    const f32 point[4] = {1.0f / static_cast<f32>(BgfxBackbufferWidth()),
                          1.0f / static_cast<f32>(BgfxBackbufferHeight()),
                          static_cast<f32>(BgfxBackbufferWidth()), static_cast<f32>(BgfxBackbufferHeight())};
    const u16 width = static_cast<u16>(BgfxBackbufferWidth());
    const u16 height = static_cast<u16>(BgfxBackbufferHeight());
    const u64 linear = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;

    if (mode == AntiAlias::Fxaa)
    {
        // One pass, reading the frame at whole and half texels: point sampling would sample the same texels a linear tap at
        // a texel centre does, but its subpixel offset is fractional, so the frame is bound linearly
        bgfx::setViewMode(HostAaBlendView, bgfx::ViewMode::Sequential);
        bgfx::setViewFrameBuffer(HostAaBlendView, g_compositeTarget);
        bgfx::setViewRect(HostAaBlendView, 0, 0, width, height);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
        bgfx::setTexture(0, g_compositeSampler, g_aaInputColour, linear);
        bgfx::setUniform(g_aaPt, point);
        bgfx::setVertexBuffer(0, g_aaTriangleScreen);
        bgfx::submit(HostAaBlendView, g_aaFxaa);
        return;
    }

    // SMAA: edges, then weights, then the blend that lands where the composite would have
    bgfx::setViewMode(HostAaEdgesView, bgfx::ViewMode::Sequential);
    bgfx::setViewFrameBuffer(HostAaEdgesView, g_aaEdges);
    bgfx::setViewRect(HostAaEdgesView, 0, 0, width, height);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    // The edge detection reads the frame's own texels, so this one binds it point-sampled
    bgfx::setTexture(0, g_compositeSampler, g_aaInputColour,
                     BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    bgfx::setUniform(g_aaPt, point);
    bgfx::setVertexBuffer(0, g_aaTriangleTarget);
    bgfx::submit(HostAaEdgesView, g_aaSmaaEdges);

    // The searches walk the edges texture a texel at a time and read the two tables at fractions, so all three bind
    // linearly
    bgfx::setViewMode(HostAaWeightsView, bgfx::ViewMode::Sequential);
    bgfx::setViewFrameBuffer(HostAaWeightsView, g_aaWeights);
    bgfx::setViewRect(HostAaWeightsView, 0, 0, width, height);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::setTexture(0, g_aaEdgesSampler, bgfx::getTexture(g_aaEdges), linear);
    bgfx::setTexture(1, g_aaAreaSampler, g_smaaArea, linear);
    bgfx::setTexture(2, g_aaSearchSampler, g_smaaSearch, linear);
    bgfx::setUniform(g_aaPt, point);
    bgfx::setVertexBuffer(0, g_aaTriangleTarget);
    bgfx::submit(HostAaWeightsView, g_aaSmaaWeights);

    bgfx::setViewMode(HostAaBlendView, bgfx::ViewMode::Sequential);
    bgfx::setViewFrameBuffer(HostAaBlendView, g_compositeTarget);
    bgfx::setViewRect(HostAaBlendView, 0, 0, width, height);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::setTexture(0, g_compositeSampler, g_aaInputColour, linear);
    bgfx::setTexture(1, g_aaBlendSampler, bgfx::getTexture(g_aaWeights), linear);
    bgfx::setUniform(g_aaPt, point);
    bgfx::setVertexBuffer(0, g_aaTriangleScreen);
    bgfx::submit(HostAaBlendView, g_aaSmaaBlend);
}
}

// The V sign uniform of the fullscreen passes that read one render target while writing another (HostRenderTargetVSign):
// a handle by the uniform's name, so one handle serves every program that declares u_rtVSign
bgfx::UniformHandle HostRenderTargetVSignUniform()
{
    static bgfx::UniformHandle handle = BGFX_INVALID_HANDLE;
    if (!bgfx::isValid(handle))
    {
        handle = bgfx::createUniform("u_rtVSign", bgfx::UniformType::Vec4);
    }

    return handle;
}

bgfx::FrameBufferHandle HostSceneTarget()
{
    EnsureSceneTarget();
    return g_sceneTarget;
}

// The scene target only if it has already been made: a caller asking whether the frame it draws is the scene's must not bring
// the scene target into being (which would take the frame target over) for a caller with a frame of its own
bgfx::FrameBufferHandle HostSceneTargetIfMade()
{
    if (bgfx::isValid(g_sceneTarget))
    {
        return g_sceneTarget;
    }

    return BGFX_INVALID_HANDLE;
}

bgfx::FrameBufferHandle HostSceneColourTarget()
{
    EnsureSceneTarget();
    return g_sceneColourTarget;
}

bgfx::TextureHandle HostSceneColour()
{
    EnsureSceneTarget();
    return g_sceneColour;
}

bgfx::TextureHandle HostSceneDepth()
{
    EnsureSceneTarget();
    return g_sceneDepth;
}

u32 HostSceneWidth()
{
    EnsureSceneTarget();
    return g_sceneWidth;
}

u32 HostSceneHeight()
{
    EnsureSceneTarget();
    return g_sceneHeight;
}

// The scene's depth only if the scene target has already been made: a pass that merely wants to know whether the frame it
// darkens is the scene's must not bring the scene target into being (and take the frame target over) for a caller that has one
// of its own
bgfx::TextureHandle HostSceneDepthIfMade()
{
    if (bgfx::isValid(g_sceneTarget))
    {
        return g_sceneDepth;
    }

    return BGFX_INVALID_HANDLE;
}

void HostSceneSetView(bgfx::ViewId view)
{
    EnsureSceneTarget();
    if (bgfx::isValid(g_sceneTarget))
    {
        bgfx::setViewFrameBuffer(view, g_sceneTarget);
    }
    bgfx::setViewRect(view, 0, 0, static_cast<u16>(g_sceneWidth), static_cast<u16>(g_sceneHeight));
}

bgfx::ViewId HostViewForBucket(u32 bucket)
{
    // The buckets that have a view of their own: one to one with the retail numbering, so the frame's order is the game's
    // own order. Bucket 1 is the sky buffer's copy, which the sky pass draws itself, and
    // the sky's own bucket (0) along with it; a material in either of them (there is none in retail's data) is put with
    // the opaque ones rather than in the sky's views, whose frame buffer is the half-size sky buffer
    switch (bucket)
    {
    case 2:
    case 3:
    case 4:
        return static_cast<bgfx::ViewId>(bucket);
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 26:
    case 27:
        return static_cast<bgfx::ViewId>(bucket);
    default:
        break;
    }

    // The blended buckets (the second chain's 5 to 19): each its own view, so the game's sorted order holds there too
    if (bucket >= 5 && bucket <= 19)
    {
        return static_cast<bgfx::ViewId>(bucket);
    }

    // No bucket, or one without a view: the opaque ones
    return HostOpaqueView;
}

// The target HostCompositeScene copies onto (invalid: the backbuffer). A headless proof sets its own so it can read
// the composited frame back
void HostCompositeSetTarget(bgfx::FrameBufferHandle framebuffer)
{
    g_compositeTarget = framebuffer;
}

// The display's offset handed over by MoveDisplay (graphics.cpp), which keeps the picture's place on the TV: the
// composite moves by it
void HostSetDisplayOffset(f32 across, f32 down)
{
    g_displayOffsetAcross = across;
    g_displayOffsetDown = down;
}

// The picture's antialiasing changed: the post passes' targets, programs and triangles are dropped, and the next frame makes
// the new mode's. Also what a resize or a mode change needs (the targets are the backbuffer's size), which is why the
// platform's display calls it
void HostInvalidateAntiAliasing()
{
    if (bgfx::isValid(g_aaInput))
    {
        bgfx::destroy(g_aaInput);
    }
    if (bgfx::isValid(g_aaEdges))
    {
        bgfx::destroy(g_aaEdges);
    }
    if (bgfx::isValid(g_aaWeights))
    {
        bgfx::destroy(g_aaWeights);
    }
    if (bgfx::isValid(g_smaaArea))
    {
        bgfx::destroy(g_smaaArea);
    }
    if (bgfx::isValid(g_smaaSearch))
    {
        bgfx::destroy(g_smaaSearch);
    }
    if (bgfx::isValid(g_aaFxaa))
    {
        bgfx::destroy(g_aaFxaa);
    }
    if (bgfx::isValid(g_aaSmaaEdges))
    {
        bgfx::destroy(g_aaSmaaEdges);
    }
    if (bgfx::isValid(g_aaSmaaWeights))
    {
        bgfx::destroy(g_aaSmaaWeights);
    }
    if (bgfx::isValid(g_aaSmaaBlend))
    {
        bgfx::destroy(g_aaSmaaBlend);
    }
    if (bgfx::isValid(g_aaTriangleTarget))
    {
        bgfx::destroy(g_aaTriangleTarget);
    }
    if (bgfx::isValid(g_aaTriangleScreen))
    {
        bgfx::destroy(g_aaTriangleScreen);
    }

    g_aaInput = BGFX_INVALID_HANDLE;
    g_aaInputColour = BGFX_INVALID_HANDLE;
    g_aaEdges = BGFX_INVALID_HANDLE;
    g_aaWeights = BGFX_INVALID_HANDLE;
    g_smaaArea = BGFX_INVALID_HANDLE;
    g_smaaSearch = BGFX_INVALID_HANDLE;
    g_aaFxaa = BGFX_INVALID_HANDLE;
    g_aaSmaaEdges = BGFX_INVALID_HANDLE;
    g_aaSmaaWeights = BGFX_INVALID_HANDLE;
    g_aaSmaaBlend = BGFX_INVALID_HANDLE;
    g_aaTriangleTarget = BGFX_INVALID_HANDLE;
    g_aaTriangleScreen = BGFX_INVALID_HANDLE;
    g_aaMade = false;
}

void HostCompositeScene()
{
    EnsureSceneTarget();
    EnsureComposite();
    // The display's offset (Platform::Graphics::MoveDisplay, kept here), as pixels of the window the picture is shown
    // in: the composite's triangle carries it, so the whole picture moves the way the PS2's display window moving on
    // the TV moves it
    EnsureCompositeTriangle(g_displayOffsetAcross * static_cast<f32>(BgfxBackbufferWidth()),
                            g_displayOffsetDown * static_cast<f32>(BgfxBackbufferHeight()));
    // The permanent vertex buffer EnsureCompositeTriangle made is the triangle drawn here; a transient one would be
    // refused on a frame that has already spent its transient pool, and the early return would leave the backbuffer's
    // debug clear (opaque magenta) showing instead of the frame -- the intermittent magenta flash
    if (!bgfx::isValid(g_sceneTarget) || !bgfx::isValid(g_compositeTriangle))
    {
        return;
    }

    // With a post pass following, the composite lands in the pass's input instead of the backbuffer: the pass then writes
    // where the composite would have (HostAaBlendView's target is g_compositeTarget, the proof's own or the backbuffer)
    const AntiAlias antiAliasing = HostAntiAliasing();
    const bool postPass = antiAliasing == AntiAlias::Fxaa || antiAliasing == AntiAlias::Smaa;
    if (postPass)
    {
        EnsureAntiAliasing();
    }

    bgfx::setViewMode(HostCompositeView, bgfx::ViewMode::Sequential);
    bgfx::setViewFrameBuffer(HostCompositeView, postPass ? g_aaInput : g_compositeTarget);
    bgfx::setViewRect(HostCompositeView, 0, 0, static_cast<u16>(BgfxBackbufferWidth()),
                      static_cast<u16>(BgfxBackbufferHeight()));
    // Fitted, the picture does not cover the whole target and the rest is the bars: cleared here, so they are black whatever
    // the target had (stretched, the triangle covers every pixel and there is nothing to clear)
    if (g_pictureFit)
    {
        bgfx::setViewClear(HostCompositeView, BGFX_CLEAR_COLOR, 0x000000FFu, 1.0f, 0);
    }
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_PT_TRISTRIP);
    bgfx::setTexture(0, g_compositeSampler, g_sceneColour,
                     BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    bgfx::setVertexBuffer(0, g_compositeTriangle);
    bgfx::submit(HostCompositeView, g_compositeProgram);

    if (postPass)
    {
        SubmitAntiAliasing();
    }
}

void HostRetireDrawPacket(const u8* packet)
{
    if (packet == nullptr)
    {
        return;
    }

    for (DrawCacheEntry& entry : g_entries)
    {
        if (!entry.used || entry.packet != packet)
        {
            continue;
        }

        // The upload goes, but not yet: a draw submitted this frame may still bind it
        if (bgfx::isValid(entry.vertices) || bgfx::isValid(entry.indexes))
        {
            RetiredDrawBuffers retired;
            retired.vertices = entry.vertices;
            retired.indexes = entry.indexes;
            retired.frame = g_drawFrame;
            g_retiredBuffers.push_back(retired);
        }

        entry.packet = nullptr;
        entry.packetSize = 0;
        entry.used = false;
        entry.indexCount = 0;
        entry.checksum = 0;
        entry.vertices.idx = bgfx::kInvalidHandle;
        entry.indexes.idx = bgfx::kInvalidHandle;
    }
}

void HostCollectRetiredDrawBuffers()
{
    // Called once a frame, after bgfx::frame(): an upload retired in frame N is let go at N + 2, by which point the
    // encoder that bound it and the frame it ran in are both done
    g_drawFrame++;
    for (size_t index = 0; index < g_retiredBuffers.size();)
    {
        const RetiredDrawBuffers& retired = g_retiredBuffers[index];
        if (g_drawFrame < retired.frame + 2)
        {
            index++;
            continue;
        }

        if (bgfx::isValid(retired.vertices))
        {
            bgfx::destroy(retired.vertices);
        }
        if (bgfx::isValid(retired.indexes))
        {
            bgfx::destroy(retired.indexes);
        }

        g_retiredBuffers.erase(g_retiredBuffers.begin() + static_cast<long>(index));
    }
}
}
