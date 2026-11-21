// 3D draw submits: rigid models, skins and blend skins through the mesh_builder's parses, cached
// by packet handle, drawn with the instance's matrix and lights as uniforms. The game's matrices are row-vector
// (multiply from the left), bgfx's column-major, so the model-to-clip product is transposed on upload.

#include "draws_host.h"

#include <algorithm>

#include "host_graphics.h"
#include "mesh_builder.h"

#include <cmath>
#include <cstddef>
#include <deque>
#include <unordered_map>
#include <vector>

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
// The overdraw map's pair: the geometry's own vertex shaders with fs_overdraw's step of grey (HostSetOverdraw)
bgfx::ProgramHandle g_rigidOverdrawProgram = BGFX_INVALID_HANDLE;
bgfx::ProgramHandle g_skinOverdrawProgram = BGFX_INVALID_HANDLE;
bool g_overdraw = false;
// The two flat-shaded programs: the same vertex and fragment code over the flat-qualified varyings, which a shader with the
// GS's IIP bit cleared draws with (HostMaterialState::flat). A flat draw submits these in place of the interpolated ones and
// is otherwise identical, uniforms and all
bgfx::ProgramHandle g_rigidFlatProgram = BGFX_INVALID_HANDLE;
bgfx::ProgramHandle g_skinFlatProgram = BGFX_INVALID_HANDLE;
// And all four again with the testless fragment shader (fs_draw3dplain.sc): a material whose alpha test never rejects draws
// through these, so its fragments keep early-Z. Invalid on a backend with no blob for that shader, and then the general
// programs serve every draw. The fragment shader knows nothing of skinning, so the skinned pair is the same two fragments
// over the skin vertex shaders
bgfx::ProgramHandle g_rigidPlainProgram = BGFX_INVALID_HANDLE;
bgfx::ProgramHandle g_rigidFlatPlainProgram = BGFX_INVALID_HANDLE;
bgfx::ProgramHandle g_skinPlainProgram = BGFX_INVALID_HANDLE;
bgfx::ProgramHandle g_skinFlatPlainProgram = BGFX_INVALID_HANDLE;
// The volumes' program: the rigid vertex shader with a fragment shader that writes one constant colour, so a shell's
// contribution is the pass's blend factor and not a fraction of the mesh's own colours (fs_shadowvolume.sc)
bgfx::ProgramHandle g_shadowVolumeProgram = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_sampler = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_mvp = BGFX_INVALID_HANDLE;
// The light set as one uniform: the three directions, the three colours, then the ambient (the shaders' u_lights[7])
bgfx::UniformHandle g_lights = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_joints = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_blendWeights = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_alpha = BGFX_INVALID_HANDLE;
// The material's scroll and colour as one uniform (the shaders' u_material[2])
bgfx::UniformHandle g_material = BGFX_INVALID_HANDLE;
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

// The most shape pairs a shaped draw's own upload is keyed by: retail's blend parts name at most a handful, and a draw that names more
// than this keeps going through the transient pool alone (no upload is ever shared on a key it did not match exactly)
constexpr u32 ShapedSkinShapeMost = 8;

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
    // The frame and view this entry was last drawn in: the per-frame count of distinct packets reads it, nothing else does
    u32 lastFrame;
    u16 lastView;
    // The skin's parsed vertexes, kept for a shaped draw: that draw re-bakes its positions every frame and the packet would otherwise be
    // parsed again for every one. The bake still goes into a copy of its own that the draw uploads through the transient pool, so two
    // draws of one character in a frame keep their own shapes -- a shared baked buffer is what broke a cutscene before. Keyed by the
    // parse's own inputs: the same packet reached with a different shape total is a different parse
    std::vector<u8> skinVertices;
    u32 skinVertexCount;
    u32 skinShapes;
    // A shaped draw's own upload: the vertexes its shapes baked, kept on the GPU between frames, so a character holding an expression
    // sends its vertexes once rather than once a frame. The shapes they hold are kept with them (the key), and the frame the buffer was
    // last bound in: a second draw of this packet this frame with other shapes finds it taken and goes through the transient pool
    // instead, which is what keeps two draws of one character's shapes apart -- the same reason the pool path exists at all
    bgfx::DynamicVertexBufferHandle shapedVertices;
    u32 shapedBytes;
    u32 shapedFrame;
    u32 shapedShapeCount;
    s32 shapedIds[ShapedSkinShapeMost];
    f32 shapedWeights[ShapedSkinShapeMost];
};

// The uploads held, one entry per live packet. An entry is never destroyed while a draw may still bind it: deleting a
// packet's owner only retires its entries, and their bgfx buffers go a couple of frames later
// (HostCollectRetiredDrawBuffers), once the frames that could still bind them have run. A deque keeps entries'
// addresses stable as it grows, which the draws need while a packet is parsed.
std::deque<DrawCacheEntry> g_entries;

// What an entry was parsed from: the packet's bytes, their size and whether it was parsed as a skin. The three are what
// tell two uploads apart, so they are the lookup's key
struct DrawCacheKey
{
    const u8* packet;
    u32 packetSize;
    bool skinned;

    bool operator==(const DrawCacheKey& other) const
    {
        return packet == other.packet && packetSize == other.packetSize && skinned == other.skinned;
    }
};

struct DrawCacheKeyHash
{
    size_t operator()(const DrawCacheKey& key) const
    {
        // The packet's address carries the entropy (a level's meshes alone are thousands of entries); the size and the
        // kind tell a packet's rigid and skinned parses apart, and the odd multipliers spread the result
        size_t hash = reinterpret_cast<size_t>(key.packet);
        hash ^= static_cast<size_t>(key.packetSize) * 0x9E3779B1u;
        hash ^= static_cast<size_t>(key.skinned ? 1u : 0u) * 0x85EBCA77u;
        return hash ^ (hash >> 16);
    }
};

// The entries by what they were parsed from. Every draw of a live packet found its entry by walking all of g_entries, so
// the frame's cost was the draw count times the uploads the session held; one hash probe answers it instead. The entries
// are the deque's, whose addresses do not move
std::unordered_map<DrawCacheKey, DrawCacheEntry*, DrawCacheKeyHash> g_entryOfPacket;
// The entries no draw holds, so a miss takes one in constant time instead of walking the deque for the first free one
std::vector<DrawCacheEntry*> g_freeEntries;
// The entries a packet was parsed into, by its address: a model's death retires its packet's uploads, and finding them was
// a walk of every entry the session held (a level's meshes are thousands). A packet has at most two (its rigid and its
// skinned parse)
std::unordered_map<const u8*, std::vector<DrawCacheEntry*>> g_entriesOfPacket;
bool g_entriesMade = false;
Platform::Graphics::HostDrawCounters g_counters = {};

// The last lookup's answer: more than half of a frame's rigid draws are a packet it already drew (the level's repeated props, the
// shadow pass's casters), and they come in runs, so the same key is asked for again and again. An entry the retirement emptied fails
// the check in FindEntry, as does the entry a freed slot was handed to, so this only ever answers with the hash's own answer
const u8* g_lastEntryPacket = nullptr;
u32 g_lastEntrySize = 0;
bool g_lastEntrySkinned = false;
DrawCacheEntry* g_lastEntry = nullptr;

// A retired upload waiting for the frames that could still bind it to run
struct RetiredDrawBuffers
{
    bgfx::VertexBufferHandle vertices;
    bgfx::IndexBufferHandle indexes;
    bgfx::DynamicVertexBufferHandle shapedVertices;
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
    // The flat-shaded pair: the rigid vertex shader is the skin one's base (both compute their world position, then light
    // it), so the two differ only in whether the joints move the vertex first
    bgfx::ShaderHandle rigidFlatVs = HostCreateShader(HostShader::Draw3DVertexFlat);
    bgfx::ShaderHandle flatFs = HostCreateShader(HostShader::Draw3DFragmentFlat);
    g_rigidFlatProgram = bgfx::createProgram(rigidFlatVs, flatFs, true);
    bgfx::ShaderHandle skinFlatVs = HostCreateShader(HostShader::Draw3DSkinVertexFlat);
    bgfx::ShaderHandle skinFlatFs = HostCreateShader(HostShader::Draw3DFragmentFlat);
    g_skinFlatProgram = bgfx::createProgram(skinFlatVs, skinFlatFs, true);
    // The four programs again with the testless fragment shader, for the draws whose alpha test always passes. A backend
    // with no blob for it (Direct3D, whose DXBC needs a Windows shaderc) leaves the handles invalid, and the vertex shaders
    // are the same ones as above, so this adds two fragment programs and nothing else
    bgfx::ShaderHandle plainFs = HostCreateShader(HostShader::Draw3DFragmentPlain);
    if (bgfx::isValid(plainFs))
    {
        bgfx::ShaderHandle plainVs = HostCreateShader(HostShader::Draw3DVertex);
        g_rigidPlainProgram = bgfx::createProgram(plainVs, plainFs, true);
        bgfx::ShaderHandle skinPlainVs = HostCreateShader(HostShader::Draw3DSkinVertex);
        g_skinPlainProgram = bgfx::createProgram(skinPlainVs, plainFs, true);
    }
    bgfx::ShaderHandle plainFlatFs = HostCreateShader(HostShader::Draw3DFragmentPlainFlat);
    if (bgfx::isValid(plainFlatFs))
    {
        bgfx::ShaderHandle plainFlatVs = HostCreateShader(HostShader::Draw3DVertexFlat);
        g_rigidFlatPlainProgram = bgfx::createProgram(plainFlatVs, plainFlatFs, true);
        bgfx::ShaderHandle skinPlainFlatVs = HostCreateShader(HostShader::Draw3DSkinVertexFlat);
        g_skinFlatPlainProgram = bgfx::createProgram(skinPlainFlatVs, plainFlatFs, true);
    }
    // The overdraw map's programs, from the same vertex shaders: only what the fragment shader writes changes
    bgfx::ShaderHandle overdrawRigidVs = HostCreateShader(HostShader::Draw3DVertex);
    bgfx::ShaderHandle overdrawRigidFs = HostCreateShader(HostShader::OverdrawFragment);
    g_rigidOverdrawProgram = bgfx::createProgram(overdrawRigidVs, overdrawRigidFs, true);
    bgfx::ShaderHandle overdrawSkinVs = HostCreateShader(HostShader::Draw3DSkinVertex);
    bgfx::ShaderHandle overdrawSkinFs = HostCreateShader(HostShader::OverdrawFragment);
    g_skinOverdrawProgram = bgfx::createProgram(overdrawSkinVs, overdrawSkinFs, true);
    g_sampler = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);
    g_mvp = bgfx::createUniform("u_mvp", bgfx::UniformType::Mat4);
    g_lights = bgfx::createUniform("u_lights", bgfx::UniformType::Vec4, 7);
    // Three vectors a joint, as the shader's array is (mesh_builder.h SkinMaxJoints, vs_draw3dskin.sc)
    g_joints = bgfx::createUniform("u_joints", bgfx::UniformType::Vec4, SkinMaxJoints * 3);
    g_blendWeights = bgfx::createUniform("u_blendWeights", bgfx::UniformType::Vec4, 4);
    g_alpha = bgfx::createUniform("u_materialAlpha", bgfx::UniformType::Vec4);
    g_material = bgfx::createUniform("u_material", bgfx::UniformType::Vec4, 2);
}

// The joint matrices a skinned draw uploads, and what the last fill of them was made from: the block's address and how
// many joints it held. Retail's WriteJoints keeps the same pair per bucket (lastJoints), so a character's submodels and
// passes do not carry the same joints to VU1 again within one batch. The host's batch is the frame: the cache is
// cleared once a frame (HostCollectRetiredDrawBuffers), which is as long as a joint block's matrices can stay the same --
// an animator refills its block every step, and the same address is another object's block by the next frame
Matrix4x4 g_jointUniforms[SkinMaxJoints];
// The same matrices as the shader reads them: three vectors a joint, its matrix's three rows (the shader rebuilds the
// constant fourth row's column), packed when the joints change rather than per draw
f32 g_jointVectors[SkinMaxJoints * 3 * 4];
const Matrix4x4* g_jointCacheKey = nullptr;
u32 g_jointCacheCount = 0;
// The lanes of g_jointUniforms the last fill wrote real joints into (the rest are identity)
u32 g_jointCacheFilled = 0;
bool g_jointIdentitiesMade = false;
// Whether a draw has already been reported as asking for more joints than the shader's array holds
bool g_jointOverflowReported = false;

// Where a shaped skin draw bakes its vertexes before they are sent to its entry's own upload (one buffer, reused: the bake needs a
// CPU-side copy of the mesh and the send takes it from there)
std::vector<u8> g_shapedStage;

Matrix4x4 JointIdentity()
{
    Matrix4x4 identity = {};
    identity.m[0][0] = 1.0f;
    identity.m[1][1] = 1.0f;
    identity.m[2][2] = 1.0f;
    identity.m[3][3] = 1.0f;
    return identity;
}

// The draw's joints as the shader's uniform array: the joints it names, then the identity (a vertex naming a lane past
// the skin's own joint count -- which a well-formed mesh never does -- lands at the model's origin rather than taking
// another pose's joint). A fill of the same block the frame's last draw made is skipped: the matrices cannot have moved
// since, and a level's characters are several draws each
void UploadJoints(const Matrix4x4* joints, u32 jointCount)
{
    bool changed = false;
    if (!g_jointIdentitiesMade)
    {
        g_jointIdentitiesMade = true;
        changed = true;
        for (u32 joint = 0; joint < SkinMaxJoints; joint++)
        {
            g_jointUniforms[joint] = JointIdentity();
        }
    }

    if (joints != g_jointCacheKey || jointCount != g_jointCacheCount)
    {
        changed = true;
        g_jointCacheKey = joints;
        g_jointCacheCount = jointCount;
        if (jointCount > SkinMaxJoints && !g_jointOverflowReported)
        {
            // Said once, so a model past the shader's array (none of the disc's is) is a log line rather than a puzzle:
            // the joints past this one follow the last joint the array holds
            g_jointOverflowReported = true;
            std::fprintf(stderr, "skin: a draw has %u joints, past the shader's %u: its last joints hold the %u\n",
                         jointCount, SkinMaxJoints, SkinMaxJoints);
        }
        const u32 upto = jointCount < SkinMaxJoints ? jointCount : SkinMaxJoints;
        for (u32 joint = 0; joint < upto; joint++)
        {
            g_jointUniforms[joint] = joints[joint];
        }

        // The lanes this pose is shorter than the last one were that pose's joints: back to the identity, so a short pose
        // after a long one does not leave a model's vertexes on another's joints
        for (u32 joint = upto; joint < g_jointCacheFilled; joint++)
        {
            g_jointUniforms[joint] = JointIdentity();
        }
        g_jointCacheFilled = upto;
    }

    if (changed)
    {
        // The three rows the shader rebuilds its matrices from, three vectors a joint: an axis's three values and, in the
        // lane the transposed affine matrix has a zero for, the position's value for that axis (PositionRow). A joint's
        // own fourth lane is zero for a rotation and a scale, so nothing is lost by putting the position there instead
        for (u32 joint = 0; joint < g_jointCacheFilled; joint++)
        {
            for (u32 row = 0; row < 3; row++)
            {
                for (u32 lane = 0; lane < 3; lane++)
                {
                    g_jointVectors[joint * 12 + row * 4 + lane] = g_jointUniforms[joint].m[row][lane];
                }
                g_jointVectors[joint * 12 + row * 4 + 3] = g_jointUniforms[joint].m[PositionRow][row];
            }
        }
    }

    // The joints this draw can name and no more: every vertex's joint index is under the mesh's joint count (the parse widens that
    // count to the largest index it saw), so the lanes past this pose are never read and the array's tail stays the previous pose's.
    // A whole frame's worth of characters was sending 228 vectors a draw where a character's 20 to 60 joints are 60 to 180
    bgfx::setUniform(g_joints, g_jointVectors, static_cast<uint16_t>(g_jointCacheFilled * 3));
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
    g_entryOfPacket.clear();
    g_entriesOfPacket.clear();
    g_freeEntries.clear();
    g_lastEntry = nullptr;
    g_lastEntryPacket = nullptr;
    g_lastEntrySize = 0;
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
        entry.shapedVertices.idx = bgfx::kInvalidHandle;
        entry.shapedBytes = 0;
        entry.shapedFrame = ~0u;
        entry.shapedShapeCount = ~0u;
        g_freeEntries.push_back(&entry);
    }
}

// The entry of a packet (nullptr: none)
DrawCacheEntry* FindEntry(const u8* packet, u32 packetSize, bool skinned)
{
    if (g_lastEntry != nullptr && g_lastEntry->used && g_lastEntryPacket == packet && g_lastEntrySize == packetSize &&
        g_lastEntrySkinned == skinned)
    {
        return g_lastEntry;
    }

    const auto found = g_entryOfPacket.find({packet, packetSize, skinned});
    if (found == g_entryOfPacket.end())
    {
        g_lastEntry = nullptr;
        return nullptr;
    }

    g_lastEntryPacket = packet;
    g_lastEntrySize = packetSize;
    g_lastEntrySkinned = skinned;
    g_lastEntry = found->second;
    return g_lastEntry;
}

// The entry a parsed packet was put in, under the key its draws look it up with and under the packet's address, which is
// what its retirement finds it by
void RegisterEntry(DrawCacheEntry* entry)
{
    g_entryOfPacket[{entry->packet, entry->packetSize, entry->skinned}] = entry;
    g_entriesOfPacket[entry->packet].push_back(entry);
    g_lastEntryPacket = entry->packet;
    g_lastEntrySize = entry->packetSize;
    g_lastEntrySkinned = entry->skinned;
    g_lastEntry = entry;
}

// A free entry, else a new one. Entries are never evicted: a live upload must not be destroyed while a draw the
// frame has already queued still binds it (see g_entries' comment).
DrawCacheEntry* TakeEntry()
{
    if (!g_freeEntries.empty())
    {
        DrawCacheEntry* entry = g_freeEntries.back();
        g_freeEntries.pop_back();
        return entry;
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
    entry.lastFrame = ~0u;
    entry.lastView = 0xFFFF;
    entry.skinVertices.clear();
    entry.skinVertexCount = 0;
    entry.skinShapes = 0;
    entry.shapedVertices.idx = bgfx::kInvalidHandle;
    entry.shapedBytes = 0;
    entry.shapedFrame = ~0u;
    entry.shapedShapeCount = ~0u;
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
                     u32 shapeCount, void* vertexBytes, u32 vertexCount)
{
    auto* vertices = reinterpret_cast<Ps2SkinVertex*>(vertexBytes);
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
        const u32 count = part->shapeCounts[id] < vertexCount ? part->shapeCounts[id] : vertexCount;
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

bgfx::ProgramHandle HostDraw3DFlatProgram()
{
    MakeShared();
    return g_rigidFlatProgram;
}

bgfx::ProgramHandle HostDraw3DSkinFlatProgram()
{
    MakeShared();
    return g_skinFlatProgram;
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

bgfx::UniformHandle HostDraw3DLights()
{
    MakeShared();
    return g_lights;
}

namespace
{
// The light set as the shaders' u_lights takes it: three directions, three colours, the ambient, in one upload
void SetDrawLights(const f32* directions, const f32* colours, const f32* ambient)
{
    f32 packed[28];
    std::memcpy(packed, directions, 12 * sizeof(f32));
    std::memcpy(packed + 12, colours, 12 * sizeof(f32));
    std::memcpy(packed + 24, ambient, 4 * sizeof(f32));
    bgfx::setUniform(g_lights, packed, 7);
}

// The material's scroll and colour as the shaders' u_material[2] takes them: one upload for the pair
void SetDrawMaterial(const f32* scroll, const f32* colour)
{
    f32 packed[8];
    std::memcpy(packed, scroll, 4 * sizeof(f32));
    std::memcpy(packed + 4, colour, 4 * sizeof(f32));
    bgfx::setUniform(g_material, packed, 2);
}
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

// One instance of a rigid packet: the draw's model-to-clip matrix and nothing else. The light set stays the submit's uniform (a group
// only holds draws whose light matches), so an instance is the sixteen floats the vertex shader multiplies by
struct RigidInstance
{
    f32 mvp[16];
    // The three scene light directions after the model rotation, computed once per model instead of in every vertex
    f32 lightDirs[12];
};

// A rigid draw waiting for the frame's flush: everything its submit sets, plus its matrix. A submission is one draw call through bgfx
// whatever it carries, and more than half of a scene frame's rigid draws are a packet the frame already drew
struct RigidQueued
{
    DrawCacheEntry* entry;
    bgfx::ViewId view;
    u64 state;
    u32 blendFactor;
    bgfx::TextureHandle texture;
    bgfx::ProgramHandle program;
    bool minifiesThroughMips;
    f32 alpha[4];
    f32 scroll[4];
    f32 colour[4];
    // The scene's lights (not the model's own, which ride the instance data) and the model's rotation
    f32 dirs[12];
    f32 cols[12];
    f32 ambient[4];
    f32 mvp[16];
    f32 lightDirs[12];
};

// The uniforms a submission sets, as the sort and the grouping compare them: alpha, scroll, colour, the lights' directions and colours
// and the ambient. They sit next to each other in RigidQueued on purpose, so one memcmp decides what six calls did
constexpr size_t RigidUniformBytes = (4 + 4 + 4 + 12 + 12 + 4) * sizeof(f32);
static_assert(offsetof(RigidQueued, scroll) == offsetof(RigidQueued, alpha) + 4 * sizeof(f32), "alpha and scroll must touch");
static_assert(offsetof(RigidQueued, colour) == offsetof(RigidQueued, scroll) + 4 * sizeof(f32), "scroll and colour must touch");
static_assert(offsetof(RigidQueued, dirs) == offsetof(RigidQueued, colour) + 4 * sizeof(f32), "colour and dirs must touch");
static_assert(offsetof(RigidQueued, cols) == offsetof(RigidQueued, dirs) + 12 * sizeof(f32), "dirs and cols must touch");
static_assert(offsetof(RigidQueued, ambient) == offsetof(RigidQueued, cols) + 12 * sizeof(f32), "cols and ambient must touch");

// The draws a frame holds before submitting them. Only opaque draws are held: a blended draw's place in the frame's order is its own, so
// it and everything held before it are submitted where it comes
constexpr u32 RigidQueueMost = 8192;
std::vector<RigidQueued> g_rigidQueue;

// TEMPORARY probe (TWINSANITY_TEST_AUDIO): why two draws of one packet in one view still do not make one submission, and how much of
// a frame the shadow volume pass is (the volumes blend with a constant factor, so the queue submits each of them alone)
// What a draw's blend allows: an opaque draw or one that only ever adds to the frame commutes with its neighbours in the run, so
// the queue may hold a run of them and submit it as one. Every other blended draw keeps its exact place in the frame and goes alone
enum RigidKind : u32
{
    RigidKindNone,
    RigidKindOpaque,
    RigidKindAdditive,
    RigidKindOrdered,
};
u32 g_rigidQueueKind = RigidKindNone;


// The sort's order: a submission's shared state first, so the draws that make one submission end up side by side. bgfx sorts the
// frame's own submits by view, then blend, then programme (SortKey::encodeDraw), and keeps this order only as the tie-break inside
// that -- so the view leads here too, and the programme comes before the texture: a texture run that crossed either boundary was going
// to be taken apart anyway. What is left for this order to decide is which draws of one programme and blend state end up adjacent
bool RigidLess(const RigidQueued& a, const RigidQueued& b)
{
    // The packet comes near the last (below), because it only has to keep one packet's draws adjacent, and sorting it first made every
    // submit walk the packet order -- 87% of submits bound a different texture from the one before
    if (a.view != b.view) return a.view < b.view;
    if (a.program.idx != b.program.idx) return a.program.idx < b.program.idx;
    if (a.texture.idx != b.texture.idx) return a.texture.idx < b.texture.idx;
    if (a.state != b.state) return a.state < b.state;
    if (a.blendFactor != b.blendFactor) return a.blendFactor < b.blendFactor;
    if (a.minifiesThroughMips != b.minifiesThroughMips) return a.minifiesThroughMips < b.minifiesThroughMips;

    // The two draws' packets, before the uniform run: a level's props share a handful of textures, so the draws that reach here
    // are mostly different packets with the same programme, texture and state, and they settle on one integer rather than a
    // 144-byte comparison. Everything a level's picture depends on is already equal above this line, and the order among
    // draws that share it is no order (opaque and additive runs both commute), so this only moves the comparisons off the
    // frame's lap: the bulk compare now runs for the repeats, which are the draws that merge anyway
    if (a.entry != b.entry) return a.entry < b.entry;

    // The six uniform blocks are contiguous and all the same width in both draws, so one comparison over the run of them orders
    // them exactly as comparing each in turn did
    const int order = std::memcmp(&a.alpha, &b.alpha, RigidUniformBytes);
    return order < 0;
}

// Whether two draws make the same submission (everything but the matrix)
bool RigidSame(const RigidQueued& a, const RigidQueued& b)
{
    return a.entry == b.entry && a.view == b.view && a.state == b.state && a.blendFactor == b.blendFactor &&
           a.texture.idx == b.texture.idx && a.program.idx == b.program.idx &&
           a.minifiesThroughMips == b.minifiesThroughMips && std::memcmp(&a.alpha, &b.alpha, RigidUniformBytes) == 0;
}

// Whether two draws are the same draw twice: one submission's work and one matrix, so the two instances are the same triangles in the
// same place and the order between them is no order at all. A blended draw keeps its place in the frame's order, which is why the queue
// only ever holds a run of these together (see HostDrawRigidPacket)
bool RigidSameDraw(const RigidQueued& a, const RigidQueued& b)
{
    return RigidSame(a, b) && std::memcmp(a.mvp, b.mvp, sizeof(a.mvp)) == 0;
}

u32 RigidDrawKind(u64 state)
{
    if ((state & BGFX_STATE_BLEND_MASK) == 0)
    {
        return RigidKindOpaque;
    }

    // A draw that writes depth is tested against what it has already written, so the order inside a run can change which of its
    // fragments pass: it keeps its place
    if ((state & BGFX_STATE_WRITE_Z) != 0)
    {
        return RigidKindOrdered;
    }

    if ((state & BGFX_STATE_BLEND_EQUATION_MASK) != BGFX_STATE_BLEND_EQUATION(BGFX_STATE_BLEND_EQUATION_ADD))
    {
        return RigidKindOrdered;
    }

    // The destination is only ever added to (dst ONE, ADD), which is what makes the run's order the same picture: the sums commute.
    // The shadow volumes add a constant factor per shell and the additive material preset adds the texture's alpha
    const u64 func = state & BGFX_STATE_BLEND_MASK;
    if (func == BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE) ||
        func == BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_FACTOR, BGFX_STATE_BLEND_ONE) ||
        func == BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_ONE))
    {
        return RigidKindAdditive;
    }

    return RigidKindOrdered;
}

// A 3D draw's program and state as the overdraw map draws them: the geometry's own vertex shader still places the triangles
// (skinning included), and the fragment shader adds one step of grey where they are, with the depth test off so nothing hides
// anything and the draw's own culling kept so no triangle is counted twice for a face the frame never saw. A build with no
// blobs for the overdraw shader (a backend tools/build_shaders.py did not make) leaves the draw as it is, so the map falls
// back to the normal picture
void OverdrawDraw(bgfx::ProgramHandle& program, u64& state, bool skinned)
{
    if (!g_overdraw)
    {
        return;
    }

    const bgfx::ProgramHandle map = skinned ? g_skinOverdrawProgram : g_rigidOverdrawProgram;
    if (!bgfx::isValid(map))
    {
        return;
    }

    program = map;
    // What the draw keeps of its own state: which way its faces are culled, and whether they are a strip (the screen models')
    state = (state & (BGFX_STATE_CULL_MASK | BGFX_STATE_PT_MASK)) | BGFX_STATE_WRITE_RGB |
            BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE) | BGFX_STATE_DEPTH_TEST_ALWAYS;
}

// One group of draws as one instanced submit: the state, the texture and the material's uniforms are its, the matrices its instance data.
// The pool clamps to what is left of the frame's space, so a short answer is drawn as the part that fits and the rest follows
// arena is the flush's one allocation and arenaStart the group's run inside it; null when the pool had no room, and the group
// allocates its own
void SubmitRigidGroup(const RigidQueued* group, u32 count, const bgfx::InstanceDataBuffer* arena, u32 arenaStart)
{
    const RigidQueued& first = group[0];
    bgfx::ProgramHandle program = first.program;
    u64 state = first.state;
    OverdrawDraw(program, state, false);
    bgfx::setState(state, first.blendFactor);
    const bgfx::TextureHandle texture = bgfx::isValid(first.texture) ? first.texture : HostWhiteTexture();
    if (first.minifiesThroughMips)
    {
        bgfx::setTexture(0, g_sampler, texture);
    }
    else
    {
        // The largest mip level alone, as a shader that never minifies through its chain reads it
        bgfx::setTexture(0, g_sampler, texture, 0, UINT16_MAX, 0, 1, UINT32_MAX, 0, 0);
    }

    bgfx::setVertexBuffer(0, first.entry->vertices);
    bgfx::setIndexBuffer(first.entry->indexes);
    // Uploaded straight from the queue's own storage rather than through the packed copies: the light set and the
    // material's pair sit contiguous and in the order the shaders' arrays take them (the static_asserts above hold that
    // layout), so a submit saves one 112-byte and one 32-byte copy -- a thousand of each a frame in a busy scene
    bgfx::setUniform(g_lights, first.dirs, 7);
    bgfx::setUniform(g_material, first.scroll, 2);
    bgfx::setUniform(g_alpha, first.alpha, 1);

    if (arena != nullptr)
    {
        bgfx::setInstanceDataBuffer(arena, arenaStart, count);
        bgfx::submit(first.view, program);
        g_counters.rigidSubmits++;
        return;
    }

    u32 done = 0;
    while (done < count)
    {
        const u32 available = bgfx::getAvailInstanceDataBuffer(count - done, sizeof(RigidInstance));
        if (available == 0)
        {
            return;
        }

        bgfx::InstanceDataBuffer buffer;
        bgfx::allocInstanceDataBuffer(&buffer, available, sizeof(RigidInstance));
        auto* instances = reinterpret_cast<RigidInstance*>(buffer.data);
        for (u32 index = 0; index < available; index++)
        {
            std::memcpy(instances[index].mvp, group[done + index].mvp, sizeof(instances[index].mvp));
            std::memcpy(instances[index].lightDirs, group[done + index].lightDirs, sizeof(instances[index].lightDirs));
        }

        bgfx::setInstanceDataBuffer(&buffer, 0, available);
        bgfx::submit(first.view, program);
        g_counters.rigidSubmits++;
        done += available;
    }
}

void FlushRigidQueue()
{
    if (g_rigidQueue.empty())
    {
        return;
    }

    g_counters.rigidFlushes++;

    // Grouped by what a submission shares: the opaque draws held are depth tested and write their own samples, so their order among
    // themselves is not the picture's -- their order against a blended draw is, which is why a blended draw flushes them. A held run of
    // ordered draws is already the frame's order and all one submission, so sorting it would only say so again
    if (g_rigidQueueKind != RigidKindOrdered)
    {
        std::sort(g_rigidQueue.begin(), g_rigidQueue.end(), RigidLess);
    }

    // One instance allocation for the whole flush: the pool is a bump allocator that resets every frame, so a group asking for its own
    // share was one call (and one copy) per submission for what is a contiguous run of matrices
    const u32 total = static_cast<u32>(g_rigidQueue.size());
    bgfx::InstanceDataBuffer arena = {};
    u32 arenaCount = 0;
    if (total > 0)
    {
        arenaCount = bgfx::getAvailInstanceDataBuffer(total, sizeof(RigidInstance));
        if (arenaCount > 0)
        {
            bgfx::allocInstanceDataBuffer(&arena, arenaCount, sizeof(RigidInstance));
            auto* instances = reinterpret_cast<RigidInstance*>(arena.data);
            for (u32 index = 0; index < arenaCount; index++)
            {
                std::memcpy(instances[index].mvp, g_rigidQueue[index].mvp, sizeof(instances[index].mvp));
                std::memcpy(instances[index].lightDirs, g_rigidQueue[index].lightDirs, sizeof(instances[index].lightDirs));
            }
        }
    }

    size_t start = 0;
    for (size_t index = 1; index <= g_rigidQueue.size(); index++)
    {
        if (index == g_rigidQueue.size() || !RigidSame(g_rigidQueue[start], g_rigidQueue[index]))
        {
            const u32 count = static_cast<u32>(index - start);
            const bool inArena = static_cast<u32>(start) + count <= arenaCount;
            SubmitRigidGroup(&g_rigidQueue[start], count, inArena ? &arena : nullptr, static_cast<u32>(start));
            start = index;
        }
    }

    g_rigidQueue.clear();
    g_rigidQueueKind = RigidKindNone;
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
        RegisterEntry(entry);
        g_counters.rigidChecksum ^= mesh.checksum;
    }

    // The frame's distinct packets against its draws: a packet drawn more than once in one view (a prop the level repeats) is what an
    // instanced submit would collapse into a single draw
    if (entry->lastFrame == g_drawFrame && entry->lastView == static_cast<u16>(view))
    {
        g_counters.rigidRepeats++;
    }
    else
    {
        entry->lastFrame = g_drawFrame;
        entry->lastView = static_cast<u16>(view);
        g_counters.rigidPackets++;
    }

    // Scene light colours and ambient stay submit uniforms. Rotate each direction once per model here, rather than multiplying
    // every vertex's normal by the model rotation three times in the vertex shader.
    f32 lightDirs[12];
    f32 lightCols[12];
    f32 lightAmbient[4];
    f32 instanceLightDirs[12] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                 0.0f, 0.0f, 1.0f, 0.0f};
    if (lights != nullptr && world != nullptr && lights->directions != nullptr && lights->colours != nullptr &&
        lights->ambient != nullptr)
    {
        std::memcpy(lightDirs, lights->directions, sizeof(lightDirs));
        std::memcpy(lightCols, lights->colours, sizeof(lightCols));
        std::memcpy(lightAmbient, lights->ambient, sizeof(lightAmbient));
        for (u32 light = 0; light < 3; light++)
        {
            const Vector4& direction = lights->directions[light];
            instanceLightDirs[light * 4 + 0] = world->m[0][0] * direction.x + world->m[1][0] * direction.y +
                                               world->m[2][0] * direction.z;
            instanceLightDirs[light * 4 + 1] = world->m[0][1] * direction.x + world->m[1][1] * direction.y +
                                               world->m[2][1] * direction.z;
            instanceLightDirs[light * 4 + 2] = world->m[0][2] * direction.x + world->m[1][2] * direction.y +
                                               world->m[2][2] * direction.z;
        }
    }
    else
    {
        // LightUniforms' neutral wash: no directional contributes, the ambient is white
        const f32 neutralDirs[12] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        const f32 white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        std::memcpy(lightDirs, neutralDirs, sizeof(lightDirs));
        std::memset(lightCols, 0, sizeof(lightCols));
        std::memcpy(lightAmbient, white, sizeof(lightAmbient));
    }
    // The material's own texture when it has one (the PS2 uploads each material's shader texture before its packet),
    // else the 1x1 white texture the fragment shader reduces to the vertex colour
    f32 alpha[4];
    if (alphaRef != nullptr)
    {
        // The uniform's w is the brighten flag, not AFAIL (which only the host reads, for the rejected samples' second pass): a
        // brighten draw's fragment shader must write its alpha as its colour for DST_COLOR, ONE to multiply the background by 1 + As
        alpha[0] = alphaRef[0];
        alpha[1] = alphaRef[1];
        alpha[2] = alphaRef[2];
        alpha[3] = alphaRef[4];
    }
    else
    {
        std::memcpy(alpha, g_noAlphaTest, sizeof(alpha));
    }

    RigidQueued queued = {};
    queued.entry = entry;
    queued.view = view;
    queued.state = state;
    queued.blendFactor = blendFactor;
    queued.texture = texture;
    queued.program = bgfx::isValid(program) ? program : g_rigidProgram;
    // A material whose alpha test rejects nothing writes what fs_draw3dplain.sc writes, so it takes that program: no
    // comparison chain and no discard. Both matter for a phone, where a fragment shader that may discard cannot be
    // depth-tested before it samples, and almost every opaque draw of a level is this case (g_noAlphaTest's always, no
    // AFAIL, no brighten preset). A draw that came in with a program of its own keeps it
    if (alpha[0] == g_noAlphaTest[0] && alpha[2] == g_noAlphaTest[2] && alpha[3] == g_noAlphaTest[3])
    {
        if (queued.program.idx == g_rigidProgram.idx && bgfx::isValid(g_rigidPlainProgram))
        {
            queued.program = g_rigidPlainProgram;
            g_counters.plainDraws++;
        }
        else if (queued.program.idx == g_rigidFlatProgram.idx && bgfx::isValid(g_rigidFlatPlainProgram))
        {
            queued.program = g_rigidFlatPlainProgram;
            g_counters.plainDraws++;
        }
    }
    queued.minifiesThroughMips = minifiesThroughMips;
    std::memcpy(queued.alpha, alpha, sizeof(queued.alpha));
    std::memcpy(queued.scroll, &g_drawScroll, sizeof(queued.scroll));
    std::memcpy(queued.colour, &g_drawColour, sizeof(queued.colour));
    std::memcpy(queued.dirs, lightDirs, sizeof(queued.dirs));
    std::memcpy(queued.cols, lightCols, sizeof(queued.cols));
    std::memcpy(queued.ambient, lightAmbient, sizeof(queued.ambient));
    std::memcpy(queued.mvp, mvp, sizeof(queued.mvp));
    std::memcpy(queued.lightDirs, instanceLightDirs, sizeof(queued.lightDirs));


    const u32 kind = RigidDrawKind(state);
    if (kind == RigidKindOrdered)
    {
        // Its place in the frame's order is its own: everything held is submitted before it and it goes where it came. The game draws
        // some geometry twice in one bucket (a prop's second draw, a wipe's repeat), and those two are the same triangles at the same
        // place with the same state: their order is no order, so they are held together and go to the driver as one instanced submit.
        // Anything else goes alone, as it must
        g_counters.rigidBlended++;
        const bool joinsRun = g_rigidQueueKind == RigidKindOrdered && !g_rigidQueue.empty() &&
                              g_rigidQueue.size() < RigidQueueMost && RigidSameDraw(g_rigidQueue.back(), queued);
        if (!joinsRun)
        {
            if (!g_rigidQueue.empty())
            {
                g_counters.rigidFlushBlended++;
            }

            FlushRigidQueue();
            g_rigidQueueKind = RigidKindOrdered;
        }

        g_rigidQueue.push_back(queued);
        if (!joinsRun)
        {
            FlushRigidQueue();
        }
    }
    else
    {
        // A run only ever holds one kind: the draws held are submitted when the kind changes, so an additive run keeps the place in
        // the frame it was drawn at
        if (g_rigidQueueKind != RigidKindNone && g_rigidQueueKind != kind)
        {
            FlushRigidQueue();
        }

        if (g_rigidQueue.size() >= RigidQueueMost)
        {
            FlushRigidQueue();
        }

        g_rigidQueueKind = kind;
        g_rigidQueue.push_back(queued);
    }

    g_counters.rigidDraws++;
    return true;
}

bool HostDrawSkinPacket(const u8* packet, u32 packetSize, u32 vertexCount, u32 shapeCount, const Matrix4x4* mvp,
                        const Matrix4x4* world, const ModelLights* lights, const Matrix4x4* joints, u32 jointCount,
                        const f32* blendWeights, u32 blendCount, bgfx::TextureHandle texture, bgfx::ViewId view,
                        u64 state, const f32* alphaRef, const HostBlendPart* blendPart, const s32* blendShapeIds,
                        bgfx::ProgramHandle program)
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

    // The rigid draws held are submitted first: a skin draw may blend, and its order against them is its own
    FlushRigidQueue();

    MakeShared();
    MakeEntries();
    DrawCacheEntry* entry = FindEntry(packet, packetSize, true);
    Ps2SkinMesh mesh;
    // A shaped draw of a packet this entry already parsed: the unshaped vertexes it baked from last frame are what it bakes from again
    // (the shapes move positions, never the triangle list), so the entry's copy is used and the packet is not parsed once a draw
    // vertexCount is the caller's parse budget, and a blend skin's draw gives none (0), so the entry's own count is what the check is
    // about: comparing it to a budget of 0 never matched, which re-parsed every blend skin's packet and re-Crc32'd its bytes every
    // frame, for a mesh the entry already held
    const bool parsedCopy = entry != nullptr && shaped && entry->skinShapes == shapeCount &&
                            !entry->skinVertices.empty() && (vertexCount == 0 || entry->skinVertexCount == vertexCount);
    if (parsedCopy)
    {
        mesh.vertexCount = entry->skinVertexCount;
        mesh.indexCount = entry->indexCount;
    }
    else if (entry == nullptr || shaped)
    {
        g_counters.skinParses++;
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
        RegisterEntry(entry);
        g_counters.skinChecksum ^= mesh.checksum;
    }

    // The parsed vertexes kept for the shaped draws that follow (see skinVertices): a plain skin never needs them, its upload is whole
    if (shaped && !parsedCopy)
    {
        entry->skinVertices = mesh.vertices;
        entry->skinVertexCount = mesh.vertexCount;
        entry->skinShapes = shapeCount;
    }

    // The joint matrices upload as-is (same row-vector convention as the
    // mvp); past SkinMaxJoints the tail is dropped (the retail skins' indices
    // stop at 61), short of it the rest is identity (UploadJoints)
    Vector4 blendUniforms[4] = {};
    for (u32 blend = 0; blend < 4 && blend < blendCount && blendWeights != nullptr; blend++)
    {
        blendUniforms[blend].x = blendWeights[blend];
    }

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
        // This frame's vertexes only (the cache's index buffer still holds: the shapes move positions, never the triangle list).
        //
        // The entry's own buffer holds them between frames: a character holding an expression asks for the vertexes it already sent,
        // and those are bound with nothing uploaded at all. The shapes are compared exactly (never a hash), and a buffer this frame
        // already bound with other shapes belongs to a submit the frame has queued, so that draw takes the transient pool instead --
        // the pool is per frame, which is what keeps two draws of one character's shapes apart
        const u32 stride = SkinLayout().getStride();
        const u32 bytes = mesh.vertexCount * stride;
        const bool cacheable = blendCount <= ShapedSkinShapeMost && blendShapeIds != nullptr;
        bool sameShapes = false;
        if (cacheable)
        {
            sameShapes = entry->shapedShapeCount == blendCount && entry->shapedBytes >= bytes &&
                         std::memcmp(entry->shapedIds, blendShapeIds, blendCount * sizeof(s32)) == 0 &&
                         std::memcmp(entry->shapedWeights, blendWeights, blendCount * sizeof(f32)) == 0;
            if (!sameShapes && (!bgfx::isValid(entry->shapedVertices) || entry->shapedBytes < bytes))
            {
                // The first shaped draw of this packet, or a bigger parse of it: the upload is the mesh's size
                if (bgfx::isValid(entry->shapedVertices))
                {
                    bgfx::destroy(entry->shapedVertices);
                }

                entry->shapedVertices = bgfx::createDynamicVertexBuffer(mesh.vertexCount, SkinLayout());
                entry->shapedBytes = bgfx::isValid(entry->shapedVertices) ? bytes : 0;
                entry->shapedFrame = ~0u;
                entry->shapedShapeCount = ~0u;
            }
        }

        const bool freeBuffer = cacheable && bgfx::isValid(entry->shapedVertices) &&
                                (entry->shapedFrame != g_drawFrame || sameShapes);
        if (freeBuffer)
        {
            if (!sameShapes)
            {
                // The unshaped vertexes into a staging copy, this frame's offsets baked into it, and the copy sent once
                g_shapedStage.resize(bytes);
                std::memcpy(g_shapedStage.data(), parsedCopy ? entry->skinVertices.data() : mesh.vertices.data(), bytes);
                BakeBlendShapes(blendPart, blendShapeIds, blendWeights, blendCount, g_shapedStage.data(), mesh.vertexCount);
                bgfx::update(entry->shapedVertices, 0, bgfx::copy(g_shapedStage.data(), bytes));
                entry->shapedShapeCount = blendCount;
                std::memcpy(entry->shapedIds, blendShapeIds, blendCount * sizeof(s32));
                std::memcpy(entry->shapedWeights, blendWeights, blendCount * sizeof(f32));
            }

            entry->shapedFrame = g_drawFrame;
            bgfx::setVertexBuffer(0, entry->shapedVertices);
            bgfx::setIndexBuffer(entry->indexes);
        }
        else
        {
            // The pool's answer is the allocation bgfx would give: it clamps to what is left of the frame's space and still hands
            // back a non-null, shorter buffer, so a request that comes back short must not be written into at all
            if (bgfx::getAvailTransientVertexBuffer(mesh.vertexCount, SkinLayout()) != mesh.vertexCount)
            {
                return false;  // the frame's transient pool is full: nothing to draw with
            }

            bgfx::TransientVertexBuffer bound;
            bgfx::allocTransientVertexBuffer(&bound, mesh.vertexCount, SkinLayout());

            // The cached upload holds the mesh without shapes (a cache entry outlives the frame's weights): this draw's own offsets
            // are baked into the pool's copy of it, in place, so a shaped draw copies its vertexes once rather than three times (out
            // of the entry, into a baked copy, and into the pool)
            if (parsedCopy)
            {
                std::memcpy(bound.data, entry->skinVertices.data(), entry->skinVertices.size());
            }
            else
            {
                std::memcpy(bound.data, mesh.vertices.data(), mesh.vertices.size());
            }

            BakeBlendShapes(blendPart, blendShapeIds, blendWeights, blendCount, bound.data, mesh.vertexCount);
            bgfx::setVertexBuffer(0, &bound);
            bgfx::setIndexBuffer(entry->indexes);
        }
    }
    else
    {
        bgfx::setVertexBuffer(0, entry->vertices);
        bgfx::setIndexBuffer(entry->indexes);
    }

    bgfx::setUniform(g_mvp, mvp, 1);
    SetDrawLights(reinterpret_cast<const f32*>(dirs), reinterpret_cast<const f32*>(colours),
                  reinterpret_cast<const f32*>(&ambient));
    UploadJoints(joints, jointCount);
    bgfx::setUniform(g_blendWeights, blendUniforms, 4);
    SetDrawMaterial(reinterpret_cast<const f32*>(&g_drawScroll), reinterpret_cast<const f32*>(&g_drawColour));
    // The alpha test the caller mapped from the material: the one material state whose test rejects nothing is the plain
    // fragment shader's, and the draw picks that programme below
    f32 alpha[4];
    if (alphaRef != nullptr)
    {
        alpha[0] = alphaRef[0];
        alpha[1] = alphaRef[1];
        alpha[2] = alphaRef[2];
        alpha[3] = alphaRef[4];
    }
    else
    {
        std::memcpy(alpha, g_noAlphaTest, sizeof(alpha));
    }
    bgfx::setUniform(g_alpha, alpha, 1);
    // The flat-shaded program for a shader with IIP cleared, else the interpolating one; the overdraw map swaps both the
    // program and what the draw writes, keeping the vertex shader that placed the triangles
    bgfx::ProgramHandle drawProgram = bgfx::isValid(program) ? program : g_skinProgram;
    if (g_overdraw)
    {
        u64 mapState = MarkedForCaster(state);
        OverdrawDraw(drawProgram, mapState, true);
        bgfx::setState(mapState);
    }
    // The testless programme for the skin draw whose alpha test passes: the map has taken the program above when it is on,
    // so this only matches the frame's own picture
    if (alpha[0] == g_noAlphaTest[0] && alpha[1] == g_noAlphaTest[1] && alpha[2] == g_noAlphaTest[2] &&
        alpha[3] == g_noAlphaTest[3])
    {
        if (drawProgram.idx == g_skinProgram.idx && bgfx::isValid(g_skinPlainProgram))
        {
            drawProgram = g_skinPlainProgram;
            g_counters.plainDraws++;
        }
        else if (drawProgram.idx == g_skinFlatProgram.idx && bgfx::isValid(g_skinFlatPlainProgram))
        {
            drawProgram = g_skinFlatPlainProgram;
            g_counters.plainDraws++;
        }
    }
    bgfx::submit(view, drawProgram);
    g_counters.skinDraws++;
    return true;
}

bool HostDrawScreenModel(const HostScreenModel* model, const Matrix4x4* mvp, bgfx::ViewId view, u64 state,
                         const f32* alphaRef, bgfx::ProgramHandle program)
{
    // A screen model's strip (the skid marks are the game's only models): its world-space places through the
    // model-to-clip matrix, drawn with the vertexes' own colours through the 1x1 white texture. Unlike the rigid and
    // skin paths there is no packet to parse and cache -- the model's bytes already are the strip the game's builder
    // laid out (HostScreenModelVertex) -- so the vertices go into a transient buffer a draw
    if (model == nullptr || model->packet == nullptr || model->count == 0 || mvp == nullptr)
    {
        return false;
    }

    FlushRigidQueue();
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
    SetDrawLights(reinterpret_cast<const f32*>(noDirections), reinterpret_cast<const f32*>(noLightColours),
                  reinterpret_cast<const f32*>(&ambient));
    SetDrawMaterial(reinterpret_cast<const f32*>(&g_drawScroll), reinterpret_cast<const f32*>(&g_drawColour));
    f32 alpha[4];
    if (alphaRef != nullptr)
    {
        alpha[0] = alphaRef[0];
        alpha[1] = alphaRef[1];
        alpha[2] = alphaRef[2];
        alpha[3] = alphaRef[4];
    }
    else
    {
        std::memcpy(alpha, g_noAlphaTest, sizeof(alpha));
    }
    bgfx::setUniform(g_alpha, alpha, 1);
    // The rigid vertex shader takes its model-to-clip matrix per instance (the packet path's instancing), so the strip is one
    // instance of its own
    RigidInstance instance = {{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}};
    std::memcpy(instance.mvp, mvp, sizeof(instance.mvp));
    bgfx::InstanceDataBuffer instances;
    bgfx::allocInstanceDataBuffer(&instances, 1, sizeof(RigidInstance));
    std::memcpy(instances.data, &instance, sizeof(instance));
    bgfx::setInstanceDataBuffer(&instances, 0, 1);
    // The flat-shaded program for a material whose shader cleared IIP (the skid marks' own does not: it sets gouraud 1),
    // else the interpolating one -- the same choice the rigid and skin paths make per pass
    bgfx::ProgramHandle stripProgram = bgfx::isValid(program) ? program : g_rigidProgram;
    if (g_overdraw)
    {
        u64 mapState = state | BGFX_STATE_PT_TRISTRIP;
        OverdrawDraw(stripProgram, mapState, false);
        bgfx::setState(mapState);
    }
    // The testless programme, as the packet paths take it, and only for the frame's own picture (the map took the program
    // above when it is on)
    if (alpha[0] == g_noAlphaTest[0] && alpha[1] == g_noAlphaTest[1] && alpha[2] == g_noAlphaTest[2] &&
        alpha[3] == g_noAlphaTest[3])
    {
        if (stripProgram.idx == g_rigidProgram.idx && bgfx::isValid(g_rigidPlainProgram))
        {
            stripProgram = g_rigidPlainProgram;
            g_counters.plainDraws++;
        }
        else if (stripProgram.idx == g_rigidFlatProgram.idx && bgfx::isValid(g_rigidFlatPlainProgram))
        {
            stripProgram = g_rigidFlatPlainProgram;
            g_counters.plainDraws++;
        }
    }
    bgfx::submit(view, stripProgram);
    g_counters.screenDraws++;
    return true;
}

const HostDrawCounters& HostDrawCounts()
{
    return g_counters;
}

void HostCountDraw2D()
{
    g_counters.draw2dDraws++;
}

void HostSetOverdraw(bool on)
{
    g_overdraw = on;
}

bool HostOverdraw()
{
    return g_overdraw;
}

void HostFlushRigidInstances()
{
    FlushRigidQueue();


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

void HostSetDrawScroll(const Platform::Graphics::HostMaterial* material, const f32* scrollUVs)
{
    // The scroll of the shader the next 3D draw's pass is, which its vertex shader adds to every UV (the retail
    // Shader::scroll WriteScroll hands the VU1 as the fourth component of the scroll uniform, the offset the animated
    // scrolls and the animation's own UV offset produce). Each shader has its own, so a material whose second pass
    // scrolls moves that pass's UVs. A material without scroll state (or with no resource) leaves the draw unmoved
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

    // The pass's offset when the caller names one, else the material's own (its first shader's)
    g_drawScroll[0] = scrollUVs != nullptr ? scrollUVs[0] : material->scroll[0];
    g_drawScroll[1] = scrollUVs != nullptr ? scrollUVs[1] : material->scroll[1];
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
    // The picture covers its rect exactly and nothing else: the rect's corners carry UV 0..1, so a fitted picture's edges sample
    // the scene's edges and the bars beside it are the view's clear. Stretched, the rect is the whole screen (fit 1) and the same
    // quad fills it. The triangle this replaces overhung the screen to (-1..3, UV 0..2), which was right stretched and, fitted,
    // put UVs past 1 on screen: they clamp to the edge texel, which stretched it over the bars (the black bar and the smeared one
    // people saw at sizes that are not 16:9)
    const f32 lowX = -fitX + clipX;
    const f32 highX = fitX + clipX;
    const f32 lowY = ySign * -fitY + clipY;
    const f32 highY = ySign * fitY + clipY;
    // The corners carry UV 0..1: lowY is where UV 0 was (the overhang's -1) and highY where UV 1 is, whatever the backend's clip
    // space (ySign flipped the positions there, not the UVs, and the same holds here). A triangle list draws three vertices at a
    // time, so the quad is written as two triangles, six vertices
    const CompositeVertex quad[6] = {
        {lowX, lowY, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
        {highX, lowY, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
        {lowX, highY, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
        {lowX, highY, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
        {highX, lowY, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
        {highX, highY, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    };
    const bgfx::Memory* memory = bgfx::copy(quad, sizeof(quad));
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
    // The picture is a quad of two triangles (the fitted rect's, or the whole screen's), in the order the vertices are in: bgfx's
    // default topology is a triangle list, so no PT state is set
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    // The scene is read with the bilinear filter: the resolution scale draws the scene larger than the picture it is
    // composited into (4x is 3840x2160 into 960x540), and point sampling that downscale threw away the detail the scale had
    // drawn -- the supersampled frame came out as the nearest texel's blocks, aliased rather than resolved. Bilinear averages
    // the texels under each picture pixel, which is what makes the scale's extra pixels count (linear is bgfx's default: the
    // point flags are what ask for the nearest texel, so leaving them off is the bilinear read). At 1x the scene and the
    // picture are the same size and every sample lands on a texel's centre, so there is nothing for the filter to change
    bgfx::setTexture(0, g_compositeSampler, g_sceneColour, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
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

    // A held batch would draw through vertexes this retires: it goes first
    FlushRigidQueue();

    const auto parsed = g_entriesOfPacket.find(packet);
    if (parsed == g_entriesOfPacket.end())
    {
        return;
    }

    for (DrawCacheEntry* entry : parsed->second)
    {
        if (!entry->used)
        {
            continue;
        }

        // The upload goes, but not yet: a draw submitted this frame may still bind it
        if (bgfx::isValid(entry->vertices) || bgfx::isValid(entry->indexes) || bgfx::isValid(entry->shapedVertices))
        {
            RetiredDrawBuffers retired;
            retired.vertices = entry->vertices;
            retired.indexes = entry->indexes;
            retired.shapedVertices = entry->shapedVertices;
            retired.frame = g_drawFrame;
            g_retiredBuffers.push_back(retired);
        }

        g_entryOfPacket.erase({entry->packet, entry->packetSize, entry->skinned});
        entry->packet = nullptr;
        entry->packetSize = 0;
        entry->skinned = false;
        entry->used = false;
        entry->indexCount = 0;
        entry->checksum = 0;
        entry->vertices.idx = bgfx::kInvalidHandle;
        entry->indexes.idx = bgfx::kInvalidHandle;
        entry->shapedVertices.idx = bgfx::kInvalidHandle;
        entry->shapedBytes = 0;
        entry->shapedFrame = ~0u;
        entry->shapedShapeCount = ~0u;
        g_freeEntries.push_back(entry);
    }

    g_entriesOfPacket.erase(parsed);
}

void HostCollectRetiredDrawBuffers()
{
    // Called once a frame, after bgfx::frame(): an upload retired in frame N is let go at N + 2, by which point the
    // encoder that bound it and the frame it ran in are both done
    g_drawFrame++;
    // The joint upload's dedup is the frame's (see g_jointUniforms): the next frame's first skinned draw fills again,
    // whatever block it draws, since the pose that block holds may have moved on
    g_jointCacheKey = nullptr;
    g_jointCacheCount = 0;
    // Compacted in one pass rather than erased where found: a chunk wipe retires a whole level's uploads at once, and
    // erasing each in turn moved every entry after it -- the count squared per frame the wipe happened
    size_t kept = 0;
    for (size_t index = 0; index < g_retiredBuffers.size(); index++)
    {
        const RetiredDrawBuffers retired = g_retiredBuffers[index];
        if (g_drawFrame < retired.frame + 2)
        {
            g_retiredBuffers[kept++] = retired;
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
        if (bgfx::isValid(retired.shapedVertices))
        {
            bgfx::destroy(retired.shapedVertices);
        }
    }

    g_retiredBuffers.resize(kept);
}
}
