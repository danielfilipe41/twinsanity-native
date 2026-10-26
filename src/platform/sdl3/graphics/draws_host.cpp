// 3D draw submits: rigid models, skins and blend skins through the mesh_builder's parses, cached
// by packet handle, drawn with the instance's matrix and lights as uniforms. The game's matrices are row-vector
// (multiply from the left), bgfx's column-major, so the model-to-clip product is transposed on upload.
#include "draws_host.h"

#include "host_graphics.h"
#include "mesh_builder.h"

#include <deque>

#include "game/graphicstables.h"
#include "game/camerarig.h"
#include "game/followcamera.h"
#include "game/instances.h"
#include "game/place.h"
#include "game/reference.h"
#include "game/view.h"

#include "game/math.h"

#include "shaders/fs_draw3d.bin.h"
#include "shaders/fs_shadowvolume.bin.h"

#include "shaders/fs_scenecomposite.bin.h"
#include "shaders/vs_draw3d.bin.h"
#include "shaders/vs_draw3dskin.bin.h"
#include "shaders/vs_scenecomposite.bin.h"

#include <cstdio>
#include <cstdlib>
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
bool g_made = false;

// The alpha test a draw with no material state uses: the fragment shader's u_materialAlpha set to ALWAYS, so a pass
// that draws geometry of its own (the sky, the shadow volumes) discards nothing. The draws that do have a material
// override it with the material's own ATST and AREF
const f32 g_noAlphaTest[4] = {static_cast<f32>(Platform::Graphics::HostAlphaAlways), 0.0f, 0.0f, 0.0f};

// The texture scroll the next 3D draw takes (the current submodel's material: HostSetDrawScroll sets it before each
// one). A material with no scrolling shader leaves it zero, so every other draw's UVs are unmoved
f32 g_drawScroll[4] = {0.0f, 0.0f, 0.0f, 0.0f};

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

// The uploads held: one entry per live packet, never evicted. bgfx records draws and runs them at bgfx::frame(), so
// evicting an entry would destroy buffers that already-queued draws still bind. A deque keeps entries' addresses
// stable as it grows, which the draws need while a packet is parsed.
std::deque<DrawCacheEntry> g_entries;
bool g_entriesMade = false;
Platform::Graphics::HostDrawCounters g_counters = {};

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

// What a screen model's red, green and blue are full at, the 3D vertexes' own (mesh_builder.cpp's VertexColourFull:
// 255 unless TS_VERTEXFULL says otherwise), so the strips and the meshes take the same gain
u32 ScreenModelFull()
{
    static const u32 full = [] {
        const char* set = std::getenv("TS_VERTEXFULL");
        if (set != nullptr)
        {
            const s32 value = std::atoi(set);
            if (value > 0 && value <= 255)
            {
                return static_cast<u32>(value);
            }
        }

        return 255u;
    }();

    return full;
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

// TS_NODEPTH: 3D draws submit with no depth test or write, telling a surface that changes with the depth state from
// one whose geometry itself moves. TS_DEPTHAB strips depth on odd frames only, for the same geometry with and without it
u64 DrawDepthState(u64 state)
{
    static const bool noDepth = std::getenv("TS_NODEPTH") != nullptr;
    static const bool depthAb = std::getenv("TS_DEPTHAB") != nullptr;
    const bool strip = noDepth || (depthAb && (Platform::Graphics::HostFrameCounter() & 1) != 0);
    return strip ? (state & ~(BGFX_STATE_DEPTH_TEST_MASK | BGFX_STATE_WRITE_Z)) : state;
}

void MakeShared()
{
    if (g_made)
    {
        return;
    }

    g_made = true;
    bgfx::ShaderHandle rigidVs = bgfx::createShader(bgfx::makeRef(vs_draw3d_glsl, sizeof(vs_draw3d_glsl)));
    bgfx::ShaderHandle skinVs = bgfx::createShader(bgfx::makeRef(vs_draw3dskin_glsl, sizeof(vs_draw3dskin_glsl)));
    bgfx::ShaderHandle fs = bgfx::createShader(bgfx::makeRef(fs_draw3d_glsl, sizeof(fs_draw3d_glsl)));
    g_rigidProgram = bgfx::createProgram(rigidVs, fs, true);
    bgfx::ShaderHandle volumeVs = bgfx::createShader(bgfx::makeRef(vs_draw3d_glsl, sizeof(vs_draw3d_glsl)));
    bgfx::ShaderHandle volumeFs = bgfx::createShader(bgfx::makeRef(fs_shadowvolume_glsl, sizeof(fs_shadowvolume_glsl)));
    g_shadowVolumeProgram = bgfx::createProgram(volumeVs, volumeFs, true);
    bgfx::ShaderHandle skinFs = bgfx::createShader(bgfx::makeRef(fs_draw3d_glsl, sizeof(fs_draw3d_glsl)));
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
}

void MakeEntries()
{
    if (g_entriesMade)
    {
        return;
    }

    g_entriesMade = true;
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

// TS_MESHDUMP2=<path>: one-shot dump for the first 3D submits - mvp/world/view matrices plus the first vertices
// projected on the CPU as the vertex shader does (clip[r] = sum_c p[c]*mvp.m[c][r]), checking upload against decode
FILE* TransformDumpFile()
{
    static bool tried = false;
    static FILE* file = nullptr;
    if (!tried)
    {
        tried = true;
        const char* path = std::getenv("TS_MESHDUMP2");
        if (path != nullptr && path[0] != '\0')
        {
            file = std::fopen(path, "a");
            if (file != nullptr)
            {
                std::setvbuf(file, nullptr, _IONBF, 0);
            }
        }
    }

    return file;
}

void DumpMatrix(FILE* file, const char* name, const Matrix4x4* matrix)
{
    if (matrix == nullptr)
    {
        std::fprintf(file, "  %s null\n", name);
        return;
    }

    std::fprintf(file, "  %s:\n", name);
    for (u32 row = 0; row < 4; row++)
    {
        std::fprintf(file, "    %12.4f %12.4f %12.4f %12.4f\n", matrix->m[row][0], matrix->m[row][1],
                     matrix->m[row][2], matrix->m[row][3]);
    }
}

// The ith MeshVertex of a rigid mesh's byte buffer (position 3xf32, uv 2xf32, normal 4xu8, colour 4xu8 = 28 bytes)
void DumpRigidVertex(FILE* file, size_t index, const u8* bytes, size_t size, const Matrix4x4* mvp)
{
    const size_t stride = 28;
    if ((index + 1) * stride > size)
    {
        return;
    }

    f32 p[3];
    f32 uv[2];
    std::memcpy(p, bytes + index * stride, sizeof(p));
    std::memcpy(uv, bytes + index * stride + 12, sizeof(uv));
    const u8* normal = bytes + index * stride + 20;
    const u8* colour = bytes + index * stride + 24;
    // The shader's clip position: (p,1) * mvp, the game's row-vector product
    f32 clip[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (u32 column = 0; column < 4; column++)
    {
        clip[column] = p[0] * mvp->m[0][column] + p[1] * mvp->m[1][column] + p[2] * mvp->m[2][column] + mvp->m[3][column];
    }
    std::fprintf(file, "  v%zu pos %.4f %.4f %.4f uv %.4f %.4f nrm %u %u %u %u col %u %u %u %u clip %.4f %.4f %.4f w %.4f\n",
                 index, p[0], p[1], p[2], uv[0], uv[1], normal[0], normal[1], normal[2], normal[3], colour[0], colour[1],
                 colour[2], colour[3], clip[0], clip[1], clip[2], clip[3]);
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

    // TS_DIAG=1: the first draws' lights, so a frame that comes out brighter than the console's can be told from a wrong
    // light gather, a light that should have been an ambient added to the directional sum, or ambient and the three
    // directions together summing past 1 (the vertex shader adds them and clamps the product, so a sum past 1 lights
    // every surface as if nothing were shadowed)
    static const bool lightDiag = std::getenv("TS_DIAG") != nullptr;
    static u32 lightLines;
    if (lightDiag && lightLines < 50)
    {
        lightLines++;
        std::fprintf(stderr,
                     "light ambient %.3f %.3f %.3f | 0 %.3f %.3f %.3f | 1 %.3f %.3f %.3f | 2 %.3f %.3f %.3f | sum %.3f\n",
                     static_cast<double>(ambient->x), static_cast<double>(ambient->y), static_cast<double>(ambient->z),
                     static_cast<double>(colours[0].x), static_cast<double>(colours[0].y),
                     static_cast<double>(colours[0].z), static_cast<double>(colours[1].x),
                     static_cast<double>(colours[1].y), static_cast<double>(colours[1].z),
                     static_cast<double>(colours[2].x), static_cast<double>(colours[2].y),
                     static_cast<double>(colours[2].z),
                     static_cast<double>(ambient->x + colours[0].x + colours[1].x + colours[2].x));
    }
}

// One vertex's skinned position: the shader's own sum of weight * (joint * (position, 1)) for the vertex's three
// lanes (vs_draw3dskin.sc's SkinPosition), with the four row-vector products the game's VuTransformPoint does
void SkinVertexWorld(const Ps2SkinVertex& v, const Matrix4x4* upload, f32* world)
{
    const u8 indices[3] = {static_cast<u8>(v.jointIdx[0]), static_cast<u8>(v.jointIdx[1]), static_cast<u8>(v.jointIdx[2])};
    f32 weights[3] = {v.jointWeight[0], v.jointWeight[1], v.jointWeight[2]};
    const f32 total = weights[0] + weights[1] + weights[2];
    if (total > 0.0f)
    {
        weights[0] /= total;
        weights[1] /= total;
        weights[2] /= total;
    }
    else
    {
        weights[0] = 1.0f;
        weights[1] = 0.0f;
        weights[2] = 0.0f;
    }

    world[0] = 0.0f;
    world[1] = 0.0f;
    world[2] = 0.0f;
    for (u32 joint = 0; joint < 3; joint++)
    {
        const Matrix4x4& m = upload[indices[joint] < SkinMaxJoints ? indices[joint] : 0];
        world[0] += weights[joint] * (v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0] + m.m[3][0]);
        world[1] += weights[joint] * (v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1] + m.m[3][1]);
        world[2] += weights[joint] * (v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2] + m.m[3][2]);
    }
}

void SkinClip(const Matrix4x4* mvp, const f32* world, f32* clip)
{
    for (u32 column = 0; column < 4; column++)
    {
        clip[column] = world[0] * mvp->m[0][column] + world[1] * mvp->m[1][column] + world[2] * mvp->m[2][column] +
                       mvp->m[3][column];
    }
}

// TS_SKINLOG=<frame>:<path>: every skinned submit of that frame - the joints about to be uploaded, each vertex's own
// joint data and the whole mesh skinned on the CPU as vs_draw3dskin.sc does (world = sum w * M[index] * (v,1),
// clip = world * mvp). Tells stale/zero joints, weights that blow the mesh up or indices past the uploaded slots
// from vertex bytes the GPU reads differently
void SkinProbe(const Matrix4x4* upload, const u8* packet, u32 packetSize, u32 vertexCount, u32 shapeCount,
               const Matrix4x4* mvp, const Matrix4x4* joints, u32 jointCount)
{
    static bool probeRead = false;
    static FILE* probeFile = nullptr;
    static u32 probeFrame = 0;
    static u32 probeLines = 0;
    if (!probeRead)
    {
        probeRead = true;
        const char* spec = std::getenv("TS_SKINLOG");
        if (spec != nullptr && spec[0] != '\0')
        {
            const char* colon = std::strchr(spec, ':');
            probeFrame = static_cast<u32>(std::strtoul(spec, nullptr, 10));
            const char* path = colon != nullptr ? colon + 1 : "/tmp/skin.log";
            probeFile = std::fopen(path, "w");
            if (probeFile != nullptr)
            {
                std::setvbuf(probeFile, nullptr, _IONBF, 0);
            }
        }
    }

    if (probeFile == nullptr || Platform::Graphics::HostFrameCounter() < probeFrame ||
        Platform::Graphics::HostFrameCounter() >= probeFrame + 30 || probeLines >= 400)
    {
        return;
    }

    probeLines++;
    Ps2SkinMesh mesh;
    const bool parsed = packet != nullptr &&
                        ParseSkinSubModel(packet, packetSize, vertexCount, shapeCount, &mesh) && !mesh.vertices.empty();
    // The uploaded array's envelope: the largest element over all 64 slots (a stale or half-written array shows here)
    f32 maxElement = 0.0f;
    for (u32 joint = 0; joint < SkinMaxJoints; joint++)
    {
        for (u32 row = 0; row < 4; row++)
        {
            for (u32 column = 0; column < 4; column++)
            {
                const f32 magnitude = upload[joint].m[row][column] < 0.0f ? -upload[joint].m[row][column]
                                                                         : upload[joint].m[row][column];
                maxElement = magnitude > maxElement ? magnitude : maxElement;
            }
        }
    }
    std::fprintf(probeFile,
                 "SKINDRAW #%u frame %llu packet %p size %u vc %u shape %u parsed %d joints %u addr %p identity %d "
                 "uploadmax %.4g verts %zu idx %zu",
                 probeLines, static_cast<unsigned long long>(Platform::Graphics::HostFrameCounter()), static_cast<const void*>(packet),
                 packetSize, vertexCount, shapeCount, parsed ? 1 : 0, jointCount, static_cast<const void*>(joints),
                 std::getenv("TS_IDENTITYJOINTS") != nullptr ? 1 : 0, static_cast<double>(maxElement),
                 mesh.vertices.size() / sizeof(Ps2SkinVertex), mesh.indexes.size());
    if (!parsed)
    {
        std::fprintf(probeFile, "\n");
        return;
    }

    const size_t stride = sizeof(Ps2SkinVertex);
    const size_t count = mesh.vertices.size() / stride;
    std::fprintf(probeFile, " mvp-trans %.3f %.3f %.3f\n", static_cast<double>(mvp->m[3][0]),
                 static_cast<double>(mvp->m[3][1]), static_cast<double>(mvp->m[3][2]));
    // The first four uploaded joint matrices in full, plus the raw joints the upload copied them from: a mismatch
    // between the two proves the fill, not the shader, is at fault
    for (u32 joint = 0; joint < 4 && joint < jointCount; joint++)
    {
        std::fprintf(probeFile, "  up%u %12.4f %12.4f %12.4f %12.4f | %12.4f %12.4f %12.4f %12.4f |"
                                " %12.4f %12.4f %12.4f %12.4f | %12.4f %12.4f %12.4f %12.4f\n",
                     joint, upload[joint].m[0][0], upload[joint].m[0][1], upload[joint].m[0][2], upload[joint].m[0][3],
                     upload[joint].m[1][0], upload[joint].m[1][1], upload[joint].m[1][2], upload[joint].m[1][3],
                     upload[joint].m[2][0], upload[joint].m[2][1], upload[joint].m[2][2], upload[joint].m[2][3],
                     upload[joint].m[3][0], upload[joint].m[3][1], upload[joint].m[3][2], upload[joint].m[3][3]);
    }
    for (u32 joint = 0; joint < 4 && joint < jointCount; joint++)
    {
        std::fprintf(probeFile, "  rj%u %12.4f %12.4f %12.4f %12.4f | %12.4f %12.4f %12.4f %12.4f |"
                                " %12.4f %12.4f %12.4f %12.4f | %12.4f %12.4f %12.4f %12.4f\n",
                     joint, joints[joint].m[0][0], joints[joint].m[0][1], joints[joint].m[0][2], joints[joint].m[0][3],
                     joints[joint].m[1][0], joints[joint].m[1][1], joints[joint].m[1][2], joints[joint].m[1][3],
                     joints[joint].m[2][0], joints[joint].m[2][1], joints[joint].m[2][2], joints[joint].m[2][3],
                     joints[joint].m[3][0], joints[joint].m[3][1], joints[joint].m[3][2], joints[joint].m[3][3]);
    }

    const Ps2SkinVertex* vertices = reinterpret_cast<const Ps2SkinVertex*>(mesh.vertices.data());
    for (size_t vertex = 0; vertex < count && vertex < 6; vertex++)
    {
        const Ps2SkinVertex& v = vertices[vertex];
        f32 world[3];
        f32 clip[4];
        SkinVertexWorld(v, upload, world);
        SkinClip(mvp, world, clip);
        std::fprintf(probeFile,
                     "  v%zu pos %.4f %.4f %.4f uv %.4f %.4f j %.0f %.0f %.0f w %.4f %.4f %.4f world %.3f %.3f %.3f "
                     "clip %.3f %.3f %.3f %.3f\n",
                     vertex, v.x, v.y, v.z, v.u, v.v, v.jointIdx[0], v.jointIdx[1], v.jointIdx[2],
                     static_cast<double>(v.jointWeight[0]), static_cast<double>(v.jointWeight[1]),
                     static_cast<double>(v.jointWeight[2]), static_cast<double>(world[0]),
                     static_cast<double>(world[1]), static_cast<double>(world[2]), static_cast<double>(clip[0]),
                     static_cast<double>(clip[1]), static_cast<double>(clip[2]), static_cast<double>(clip[3]));
    }

    // The whole mesh's extents: skinned by the uploaded joints and, for reference, bind-posed (identity joints), both
    // taken to the window's coordinates with the same mvp the draw submits
    f32 mn[3] = {1.0e30f, 1.0e30f, 1.0e30f};
    f32 mx[3] = {-1.0e30f, -1.0e30f, -1.0e30f};
    f32 mnWorld[3] = {1.0e30f, 1.0e30f, 1.0e30f};
    f32 mxWorld[3] = {-1.0e30f, -1.0e30f, -1.0e30f};
    f32 mnIdentity[3] = {1.0e30f, 1.0e30f, 1.0e30f};
    f32 mxIdentity[3] = {-1.0e30f, -1.0e30f, -1.0e30f};
    f32 spread = 0.0f;
    f32 weightMin = 1.0e30f;
    f32 weightMax = -1.0e30f;
    u32 zeroWeights = 0;
    u32 bigWeights = 0;
    u32 farVertices = 0;
    u32 badIndices = 0;
    u32 maxIndex = 0;
    for (size_t vertex = 0; vertex < count; vertex++)
    {
        const Ps2SkinVertex& v = vertices[vertex];
        for (u32 joint = 0; joint < 3; joint++)
        {
            const f32 weight = (&v.jointWeight[0])[joint];
            const u32 index = v.jointIdx[joint];
            if (index > maxIndex)
            {
                maxIndex = index;
            }
            if (index >= jointCount || index >= SkinMaxJoints)
            {
                badIndices++;
            }
            if (weight < weightMin)
            {
                weightMin = weight;
            }
            if (weight > weightMax)
            {
                weightMax = weight;
            }
        }
        const f32 total = v.jointWeight[0] + v.jointWeight[1] + v.jointWeight[2];
        zeroWeights += total <= 0.0f ? 1 : 0;
        bigWeights += total > 4.0f ? 1 : 0;
        f32 world[3];
        f32 clip[4];
        SkinVertexWorld(v, upload, world);
        SkinClip(mvp, world, clip);
        for (u32 lane = 0; lane < 3; lane++)
        {
            mnWorld[lane] = world[lane] < mnWorld[lane] ? world[lane] : mnWorld[lane];
            mxWorld[lane] = world[lane] > mxWorld[lane] ? world[lane] : mxWorld[lane];
        }
        f32 magnitude = 0.0f;
        for (u32 lane = 0; lane < 3; lane++)
        {
            magnitude += world[lane] * world[lane];
        }
        farVertices += magnitude > 1.0e8f ? 1 : 0;
        for (u32 lane = 0; lane < 3; lane++)
        {
            const f32 ndc = clip[lane] / (clip[3] != 0.0f ? clip[3] : 1.0e-9f);
            mn[lane] = ndc < mn[lane] ? ndc : mn[lane];
            mx[lane] = ndc > mx[lane] ? ndc : mx[lane];
        }
        // The bind pose (identity joints): the same mvp on the vertex's own position
        const f32 bind[3] = {v.x, v.y, v.z};
        f32 bindClip[4];
        SkinClip(mvp, bind, bindClip);
        for (u32 lane = 0; lane < 3; lane++)
        {
            const f32 ndc = bindClip[lane] / (bindClip[3] != 0.0f ? bindClip[3] : 1.0e-9f);
            mnIdentity[lane] = ndc < mnIdentity[lane] ? ndc : mnIdentity[lane];
            mxIdentity[lane] = ndc > mxIdentity[lane] ? ndc : mxIdentity[lane];
        }
        const f32 absolute = (clip[0] < 0.0f ? -clip[0] : clip[0]) + (clip[1] < 0.0f ? -clip[1] : clip[1]);
        const f32 divisor = clip[3] < 0.0f ? -clip[3] : clip[3];
        spread = absolute / (divisor != 0.0f ? divisor : 1.0e-9f) > spread ? absolute / divisor : spread;
    }
    std::fprintf(probeFile,
                 "  extent ndc [%.2f %.2f]x[%.2f %.2f] spread %.1f world [%.1f %.1f %.1f]-[%.1f %.1f %.1f] "
                 "bindndc [%.2f %.2f]x[%.2f %.2f] w [%.4f %.4f] zero %u big %u far %u badidx %u maxidx %u\n",
                 static_cast<double>(mn[0]), static_cast<double>(mx[0]), static_cast<double>(mn[1]),
                 static_cast<double>(mx[1]), static_cast<double>(spread), static_cast<double>(mnWorld[0]),
                 static_cast<double>(mnWorld[1]), static_cast<double>(mnWorld[2]), static_cast<double>(mxWorld[0]),
                 static_cast<double>(mxWorld[1]), static_cast<double>(mxWorld[2]), static_cast<double>(mnIdentity[0]),
                 static_cast<double>(mxIdentity[0]), static_cast<double>(mnIdentity[1]),
                 static_cast<double>(mxIdentity[1]), static_cast<double>(weightMin), static_cast<double>(weightMax),
                 zeroWeights, bigWeights, farVertices, badIndices, maxIndex);
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
    // TS_MATTEXLOG=<frame>:<path>: every HostSubModelTexture call in that frame - material, its first shader's game
    // texture, the bgfx texture resolved and its pixel size. A wrong run shows the white fallback, an unsized game
    // texture or one handle shared by materials naming different textures
    {
        static bool probeRead = false;
        static FILE* probeFile = nullptr;
        static u32 probeFrame = 0;
        static u32 probeLines = 0;
        if (!probeRead)
        {
            probeRead = true;
            const char* spec = std::getenv("TS_MATTEXLOG");
            if (spec != nullptr && spec[0] != '\0')
            {
                const char* colon = std::strchr(spec, ':');
                probeFrame = static_cast<u32>(std::strtoul(spec, nullptr, 10));
                const char* path = colon != nullptr ? colon + 1 : "/tmp/mattex.log";
                probeFile = std::fopen(path, "w");
                if (probeFile != nullptr)
                {
                    std::setvbuf(probeFile, nullptr, _IONBF, 0);
                }
            }
        }

        if (probeFile != nullptr && Platform::Graphics::HostFrameCounter() >= probeFrame && Platform::Graphics::HostFrameCounter() < probeFrame + 3 && probeLines < 8000)
        {
            probeLines++;
            u32 width = 0;
            u32 height = 0;
            const bool sized = HostTextures().FindSize(material->gameTexture, &width, &height);
            const bgfx::TextureHandle resolved = bgfx::isValid(texture) ? texture : HostWhiteTexture();
            std::fprintf(probeFile,
                         "MATTEX #%u materials %p index %u material %p flags %08x gameTexture %p stored %u bound %u "
                         "resolved %u %ux%u sized %d set", 
                         probeLines, static_cast<const void*>(materials), index, static_cast<const void*>(material),
                         material->flags, static_cast<const void*>(material->gameTexture), material->texture,
                         texture.idx, resolved.idx, width, height, sized ? 1 : 0);
            for (u32 field = 0; field < HostShaderSettings; field++)
            {
                std::fprintf(probeFile, " %02x", material->shaderSettings[field]);
            }
            std::fprintf(probeFile, "\n");
        }
    }
    // TS_NOTEX: every 3D submodel samples the 1x1 white texture, telling a frame coloured by bound textures from one
    // coloured by vertex colours
    if (std::getenv("TS_NOTEX") != nullptr)
    {
        return HostWhiteTexture();
    }

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
    // The TS_NO switch: every rigid submodel draw off (chunk scenery, OGIs, the sky's meshes, shadow volumes)
    if (HostStageDisabled("mesh"))
    {
        return false;
    }

    if (packet == nullptr || packetSize == 0 || mvp == nullptr)
    {
        return false;
    }

    MakeShared();
    MakeEntries();
    // The bound texture's size against the decoded UVs' range (TS_MESHDUMP2, the first 12 rigid draws): tells a
    // packet whose UVs are in texel units from one whose UVs are normalised fractions
    {
        static u32 probed = 0;
        FILE* probeFile = probed < 12 ? TransformDumpFile() : nullptr;
        if (probeFile != nullptr)
        {
            probed++;
            Ps2RigidMesh mesh;
            const bool ok = ParseRigidSubModel(packet, packetSize, vertexCount, &mesh);
            f32 minU = 1.0e30f;
            f32 maxU = -1.0e30f;
            f32 minV = 1.0e30f;
            f32 maxV = -1.0e30f;
            const size_t stride = 28;
            for (size_t index = 0; ok && (index + 1) * stride <= mesh.vertices.size(); index++)
            {
                f32 u;
                f32 v;
                std::memcpy(&u, mesh.vertices.data() + index * stride + 12, sizeof(u));
                std::memcpy(&v, mesh.vertices.data() + index * stride + 16, sizeof(v));
                minU = u < minU ? u : minU;
                maxU = u > maxU ? u : maxU;
                minV = v < minV ? v : minV;
                maxV = v > maxV ? v : maxV;
            }

            const bgfx::TextureHandle bound = bgfx::isValid(texture) ? texture : HostWhiteTexture();
            std::fprintf(probeFile, "RIGIDUV #%u packet %p verts %zu tex %u u [%g %g] v [%g %g]\n", probed,
                         static_cast<const void*>(packet), mesh.vertices.size() / stride, bound.idx,
                         static_cast<double>(minU), static_cast<double>(maxU), static_cast<double>(minV),
                         static_cast<double>(maxV));
        }
    }
    // TS_RIGIDLOG=<frame>:<path>: every rigid submit of that frame - parsed vertex/triangle counts, decoded UV and
    // vertex-colour ranges - telling flat triangles from varying ones and a colour-constant draw from a
    // texture-constant one
    {
        static bool rigidRead = false;
        static FILE* rigidFile = nullptr;
        static u32 rigidFrame = 0;
        static u32 rigidLines = 0;
        if (!rigidRead)
        {
            rigidRead = true;
            const char* spec = std::getenv("TS_RIGIDLOG");
            if (spec != nullptr && spec[0] != '\0')
            {
                const char* colon = std::strchr(spec, ':');
                rigidFrame = static_cast<u32>(std::strtoul(spec, nullptr, 10));
                const char* path = colon != nullptr ? colon + 1 : "/tmp/rigid.log";
                rigidFile = std::fopen(path, "w");
                if (rigidFile != nullptr)
                {
                    std::setvbuf(rigidFile, nullptr, _IONBF, 0);
                }
            }
        }

        if (rigidFile != nullptr && Platform::Graphics::HostFrameCounter() >= rigidFrame && Platform::Graphics::HostFrameCounter() < rigidFrame + 3 && rigidLines < 8000)
        {
            rigidLines++;
            Ps2RigidMesh mesh;
            const bool ok = ParseRigidSubModel(packet, packetSize, vertexCount, &mesh);
            f32 minU = 1.0e30f;
            f32 maxU = -1.0e30f;
            f32 minV = 1.0e30f;
            f32 maxV = -1.0e30f;
            u8 minC[4] = {255, 255, 255, 255};
            u8 maxC[4] = {0, 0, 0, 0};
            const size_t stride = 28;
            size_t verts = 0;
            for (size_t index = 0; ok && (index + 1) * stride <= mesh.vertices.size(); index++)
            {
                f32 u;
                f32 v;
                std::memcpy(&u, mesh.vertices.data() + index * stride + 12, sizeof(u));
                std::memcpy(&v, mesh.vertices.data() + index * stride + 16, sizeof(v));
                minU = u < minU ? u : minU;
                maxU = u > maxU ? u : maxU;
                minV = v < minV ? v : minV;
                maxV = v > maxV ? v : maxV;
                for (u32 channel = 0; channel < 4; channel++)
                {
                    const u8 byte = mesh.vertices.data()[index * stride + 24 + channel];
                    minC[channel] = byte < minC[channel] ? byte : minC[channel];
                    maxC[channel] = byte > maxC[channel] ? byte : maxC[channel];
                }
                verts++;
            }

            std::fprintf(rigidFile,
                         "RIGID #%u packet %p verts %zu tris %zu tex %u uv u[%.4f %.4f] v[%.4f %.4f] col "
                         "r[%u %u] g[%u %u] b[%u %u] a[%u %u] c4 %u c6 %u bnd [%.1f %.1f %.1f]-[%.1f %.1f %.1f] "
                         "w4 %08x %08x %08x %08x pos32 %u pos16 %u unk %u rc",
                         rigidLines, static_cast<const void*>(packet), verts, mesh.indexes.size() / 2, texture.idx,
                         static_cast<double>(minU), static_cast<double>(maxU), static_cast<double>(minV),
                         static_cast<double>(maxV), minC[0], maxC[0], minC[1], maxC[1], minC[2], maxC[2], minC[3],
                         maxC[3], mesh.address4Colours, mesh.address6Colours, static_cast<double>(mesh.boundsMin[0]),
                         static_cast<double>(mesh.boundsMin[1]), static_cast<double>(mesh.boundsMin[2]),
                         static_cast<double>(mesh.boundsMax[0]), static_cast<double>(mesh.boundsMax[1]),
                         static_cast<double>(mesh.boundsMax[2]), mesh.address4Sample[0], mesh.address4Sample[1],
                         mesh.address4Sample[2], mesh.address4Sample[3], mesh.positionRows32, mesh.positionRows16,
                         mesh.unknownRow);
            for (u32 row = 0; row < mesh.rowCodeCount; row++)
            {
                std::fprintf(rigidFile, " %08x", mesh.rowCodes[row]);
            }
            // The first vertex's clip z and w (the depth the depth test sees)
            if (!mesh.vertices.empty())
            {
                f32 p[3];
                std::memcpy(p, mesh.vertices.data(), sizeof(p));
                f32 clip[4] = {0, 0, 0, 0};
                for (u32 column = 0; column < 4; column++)
                {
                    clip[column] = p[0] * mvp->m[0][column] + p[1] * mvp->m[1][column] +
                                   p[2] * mvp->m[2][column] + mvp->m[3][column];
                }
                std::fprintf(rigidFile, " zw %.5f %.5f %.5f st %016llx\n", static_cast<double>(clip[2]),
                             static_cast<double>(clip[3]),
                             clip[3] != 0.0f ? static_cast<double>(clip[2] / clip[3]) : 0.0,
                             static_cast<unsigned long long>(state));
            }
            else
            {
                std::fprintf(rigidFile, "\n");
            }
        }
    }

    // The first few rigid submits' transform state (TS_MESHDUMP2)
    {
        static u32 called = 0;
        const bool want = (called < 60000 && (called % 2000) == 0);
        called++;
        FILE* dumpFile = want ? TransformDumpFile() : nullptr;
        if (dumpFile != nullptr)
        {
            Ps2RigidMesh mesh;
            const bool ok = ParseRigidSubModel(packet, packetSize, vertexCount, &mesh);
            std::fprintf(dumpFile, "RIGIDDRAW #%u packet %p size %u vertexCount %u parsed %d verts %zu idx %zu\n",
                         g_counters.rigidDraws, static_cast<const void*>(packet), packetSize, vertexCount, ok ? 1 : 0,
                         mesh.vertices.size(), mesh.indexes.size());
            DumpMatrix(dumpFile, "mvp", mvp);
            DumpMatrix(dumpFile, "world", world);
            DumpMatrix(dumpFile, "view", HostViewMatrix());
            if (ok)
            {
                for (size_t index = 0; index < 8; index++)
                {
                    DumpRigidVertex(dumpFile, index, mesh.vertices.data(), mesh.vertices.size(), mvp);
                }
            }
        }
    }
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
    bgfx::setState(DrawDepthState(state), blendFactor);
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
    bgfx::setUniform(g_alpha, alphaRef != nullptr ? alphaRef : g_noAlphaTest, 1);
    bgfx::submit(view, bgfx::isValid(program) ? program : g_rigidProgram);
    g_counters.rigidDraws++;
    return true;
}

bool HostDrawSkinPacket(const u8* packet, u32 packetSize, u32 vertexCount, u32 shapeCount, const Matrix4x4* mvp,
                        const Matrix4x4* world, const ModelLights* lights, const Matrix4x4* joints, u32 jointCount,
                        const f32* blendWeights, u32 blendCount, bgfx::TextureHandle texture, bgfx::ViewId view,
                        u64 state, const f32* alphaRef, const HostBlendPart* blendPart, const s32* blendShapeIds)
{
    // The TS_NO switch: every skinned submodel draw off
    if (HostStageDisabled("skin"))
    {
        return false;
    }

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

    // TS_IDENTITYJOINTS: every uploaded joint matrix is the identity, so a coherent bind pose faults the joint data
    // or its mapping and a shattered one faults the vertex data or the shader's attribute use
    static const bool identityJoints = std::getenv("TS_IDENTITYJOINTS") != nullptr;
    // The joint matrices upload as-is (same row-vector convention as the
    // mvp); past SkinMaxJoints the tail is dropped (the retail skins' indices
    // stop at 61), short of it the rest is identity
    static Matrix4x4 jointUniforms[SkinMaxJoints];
    for (u32 joint = 0; joint < SkinMaxJoints; joint++)
    {
        if (identityJoints)
        {
            std::memset(&jointUniforms[joint], 0, sizeof(Matrix4x4));
            jointUniforms[joint].m[0][0] = 1.0f;
            jointUniforms[joint].m[1][1] = 1.0f;
            jointUniforms[joint].m[2][2] = 1.0f;
            jointUniforms[joint].m[3][3] = 1.0f;
        }
        else if (joint < jointCount)
        {
            jointUniforms[joint] = joints[joint];
        }
        else if (joint == 0)
        {
            std::memset(&jointUniforms[joint], 0, sizeof(Matrix4x4));
            jointUniforms[joint].m[0][0] = 1.0f;
            jointUniforms[joint].m[1][1] = 1.0f;
            jointUniforms[joint].m[2][2] = 1.0f;
            jointUniforms[joint].m[3][3] = 1.0f;
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

    // TS_SKINLOG=<frame>:<path>: every skinned submit of one frame - the joints about to be uploaded and each
    // vertex's joint data projected on the CPU as the shader applies it - telling stale/zero joints, weights that
    // blow the mesh up or indices past the uploaded slots from vertex bytes the GPU reads differently
    SkinProbe(jointUniforms, packet, packetSize, vertexCount, shapeCount, mvp, joints, jointCount);

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
    bgfx::setState(DrawDepthState(MarkedForCaster(state)));
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
    bgfx::setUniform(g_alpha, alphaRef != nullptr ? alphaRef : g_noAlphaTest, 1);
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
        const u32 full = ScreenModelFull();
        vertex->colour[0] = ScaleScreenModelChannel(static_cast<u8>(source.colour), full);
        vertex->colour[1] = ScaleScreenModelChannel(static_cast<u8>(source.colour >> 8), full);
        vertex->colour[2] = ScaleScreenModelChannel(static_cast<u8>(source.colour >> 16), full);
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
    bgfx::setState(DrawDepthState(state | BGFX_STATE_PT_TRISTRIP));
    bgfx::setTexture(0, g_sampler, bgfx::isValid(texture) ? texture : HostWhiteTexture());
    bgfx::setVertexBuffer(0, &buffer);
    bgfx::setUniform(g_mvp, mvp, 1);
    bgfx::setUniform(g_lightDirs, noDirections, 3);
    bgfx::setUniform(g_lightColours, noLightColours, 3);
    bgfx::setUniform(g_ambient, &ambient, 1);
    bgfx::setUniform(g_scroll, g_drawScroll, 1);
    bgfx::setUniform(g_alpha, alphaRef != nullptr ? alphaRef : g_noAlphaTest, 1);
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
        return;
    }

    g_drawScroll[0] = material->scroll[0];
    g_drawScroll[1] = material->scroll[1];
    g_drawScroll[2] = 0.0f;
    g_drawScroll[3] = 0.0f;
    // The scroll probe (TS_MESHDUMP2, the first 10 materials with a non-zero scroll): the values the vertex shaders
    // add to the UVs, which the shaders read as texture fractions (retail's VU1 adds them to texel-space ST)
    {
        static u32 probed = 0;
        FILE* probeFile = (probed < 10 && (material->scroll[0] != 0.0f || material->scroll[1] != 0.0f))
                              ? TransformDumpFile()
                              : nullptr;
        if (probeFile != nullptr)
        {
            probed++;
            std::fprintf(probeFile, "SCROLL #%u material %p scroll %g %g phases %g %g speeds %g %g\n", probed,
                         static_cast<const void*>(material), static_cast<double>(material->scroll[0]),
                         static_cast<double>(material->scroll[1]), static_cast<double>(material->scrollPhases[0]),
                         static_cast<double>(material->scrollPhases[1]), static_cast<double>(material->scrollSpeeds[0]),
                         static_cast<double>(material->scrollSpeeds[1]));
        }
    }
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
    u32 width = static_cast<u32>(BgfxWindowWidth);
    u32 height = static_cast<u32>(BgfxWindowHeight);
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
                                              BGFX_SAMPLER_MIP_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    // The scene's depth: the shadow pass samples it for the resolve's ramp (it is also the depth test the 3D passes
    // rely on, which is why the scene has to be one target rather than the backbuffer)
    g_sceneDepth = bgfx::createTexture2D(static_cast<u16>(width), static_cast<u16>(height), false, 1,
                                         bgfx::TextureFormat::D24S8,
                                         BGFX_TEXTURE_RT | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
                                             BGFX_SAMPLER_MIP_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
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
    bgfx::ShaderHandle vs = bgfx::createShader(bgfx::makeRef(vs_scenecomposite_glsl, sizeof(vs_scenecomposite_glsl)));
    bgfx::ShaderHandle fs = bgfx::createShader(bgfx::makeRef(fs_scenecomposite_glsl, sizeof(fs_scenecomposite_glsl)));
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
    if (bgfx::isValid(g_compositeTriangle) && offsetX == g_compositeOffsetX && offsetY == g_compositeOffsetY)
    {
        return;
    }

    if (bgfx::isValid(g_compositeTriangle))
    {
        bgfx::destroy(g_compositeTriangle);
        g_compositeTriangle = BGFX_INVALID_HANDLE;
    }

    // A frame pixel is two clip units across (NDC runs -1 to 1 over the frame), and clip y grows up where the offset's
    // y counts down the frame
    const f32 clipX = 2.0f * offsetX / static_cast<f32>(BgfxWindowWidth);
    const f32 clipY = -2.0f * offsetY / static_cast<f32>(BgfxWindowHeight);
    const CompositeVertex triangle[3] = {
        {-1.0f + clipX, -1.0f + clipY, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
        {3.0f + clipX, -1.0f + clipY, 0.0f, 2.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
        {-1.0f + clipX, 3.0f + clipY, 0.0f, 0.0f, 2.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    };
    const bgfx::Memory* memory = bgfx::copy(triangle, sizeof(triangle));
    g_compositeTriangle = bgfx::createVertexBuffer(memory, CompositeLayout());
    g_compositeOffsetX = offsetX;
    g_compositeOffsetY = offsetY;
}
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

void HostCompositeScene()
{
    // The TS_NO switch: the whole 3D picture kept off the back buffer
    if (HostStageDisabled("composite"))
    {
        return;
    }

    EnsureSceneTarget();
    EnsureComposite();
    // The display's offset (Platform::Graphics::MoveDisplay, kept here), as pixels of the window the picture is shown
    // in: the composite's triangle carries it, so the whole picture moves the way the PS2's display window moving on
    // the TV moves it
    EnsureCompositeTriangle(g_displayOffsetAcross * static_cast<f32>(BgfxWindowWidth),
                            g_displayOffsetDown * static_cast<f32>(BgfxWindowHeight));
    // The permanent vertex buffer EnsureCompositeTriangle made is the triangle drawn here; a transient one would be
    // refused on a frame that has already spent its transient pool, and the early return would leave the backbuffer's
    // debug clear (opaque magenta) showing instead of the frame -- the intermittent magenta flash
    if (!bgfx::isValid(g_sceneTarget) || !bgfx::isValid(g_compositeTriangle))
    {
        return;
    }

    bgfx::setViewMode(HostCompositeView, bgfx::ViewMode::Sequential);
    bgfx::setViewFrameBuffer(HostCompositeView, g_compositeTarget);
    bgfx::setViewRect(HostCompositeView, 0, 0, BgfxWindowWidth, BgfxWindowHeight);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_PT_TRISTRIP);
    bgfx::setTexture(0, g_compositeSampler, g_sceneColour,
                     BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    bgfx::setVertexBuffer(0, g_compositeTriangle);
    bgfx::submit(HostCompositeView, g_compositeProgram);
}
}
