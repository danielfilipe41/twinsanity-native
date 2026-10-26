// Headless 3D submit proof: beach.rm2 rigid model item 0 submodel 0 and the
// first parseable skin submodel through the host DrawRigidModel/DrawSkin
// (mesh_builder parse -> bgfx buffer cache -> program + light uniforms ->
// submit). bgfx runs headless on the Noop renderer: no window, no GPU; the
// cache + parse + bind + submit logic runs for real, presentation skipped.
// Prints draw-call counts (bgfx stats + host counters) and the vertex checksums.
// Build: g++ -m32 <host flags> tools/draws_proof.cpp
//     src/platform/sdl3/graphics/draws_host.cpp
//     src/platform/sdl3/graphics/mesh_builder.cpp
//     src/platform/sdl3/graphics/material_system.cpp
//     src/platform/sdl3/graphics/texture_decoder.cpp
//     src/platform/sdl3/graphics/graphics.cpp <bgfx libs> -o /tmp/draws_proof
#include "../src/platform/sdl3/graphics/draws_host.h"
#include "../src/platform/sdl3/graphics/host_graphics.h"
#include "../src/platform/sdl3/graphics/mesh_builder.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "game/scenery.h"

namespace
{
struct BhEntry
{
    std::string path;
    uint32_t start;
    uint32_t size;
};

uint32_t ReadU32(const uint8_t* at)
{
    uint32_t value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

int32_t ReadS32(const uint8_t* at)
{
    int32_t value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

std::vector<uint8_t> ReadFile(const char* path)
{
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        std::fprintf(stderr, "missing %s\n", path);
        return {};
    }

    std::fseek(file, 0, SEEK_END);
    long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size())
    {
        bytes.clear();
    }

    std::fclose(file);
    return bytes;
}

// The BH table (src/game/archive.cpp ArchiveSectionReader::Read): s32 header,
// then per file s32 path length, path bytes, s32 BD start, s32 size
bool ParseBh(const std::vector<uint8_t>& bh, std::vector<BhEntry>* entries)
{
    if (bh.size() < 4)
    {
        return false;
    }

    size_t at = 4;
    while (at + 4 <= bh.size())
    {
        int32_t length = ReadS32(bh.data() + at);
        at += 4;
        if (length < 0 || at + static_cast<size_t>(length) + 8 > bh.size())
        {
            break;
        }

        BhEntry entry;
        entry.path.assign(reinterpret_cast<const char*>(bh.data() + at), static_cast<size_t>(length));
        at += static_cast<size_t>(length);
        entry.start = static_cast<uint32_t>(ReadS32(bh.data() + at));
        at += 4;
        entry.size = static_cast<uint32_t>(ReadS32(bh.data() + at));
        at += 4;
        entries->push_back(entry);
    }

    return !entries->empty();
}

// An RM2 file: SectionHeader (u32 format, u32 itemCount, u32 size) then the
// ItemHeader table (u32 offset, s32 size, u32 id) with offsets from the
// section's own start. Finds section id's item table entry.
bool FindSection(const uint8_t* base, size_t span, uint32_t id, uint32_t* offset, uint32_t* size)
{
    if (span < 12)
    {
        return false;
    }

    uint32_t items = ReadU32(base + 4);
    size_t at = 12;
    for (uint32_t item = 0; item < items; item++)
    {
        if (at + 12 > span)
        {
            return false;
        }

        uint32_t entryOffset = ReadU32(base + at);
        int32_t entrySize = ReadS32(base + at + 4);
        uint32_t entryId = ReadU32(base + at + 8);
        at += 12;
        if (entryId == id)
        {
            *offset = entryOffset;
            *size = static_cast<uint32_t>(entrySize);
            return true;
        }
    }

    return false;
}

// A section's own header + item table at sectionBase: item kind's offset/size
bool FindItem(const uint8_t* sectionBase, size_t span, uint32_t kind, uint32_t* offset, uint32_t* size)
{
    if (span < 12)
    {
        return false;
    }

    uint32_t items = ReadU32(sectionBase + 4);
    size_t at = 12;
    for (uint32_t item = 0; item < items; item++)
    {
        if (at + 12 > span)
        {
            return false;
        }

        uint32_t entryOffset = ReadU32(sectionBase + at);
        int32_t entrySize = ReadS32(sectionBase + at + 4);
        uint32_t entryKind = ReadU32(sectionBase + at + 8);
        at += 12;
        if (entryKind == kind)
        {
            *offset = entryOffset;
            *size = static_cast<uint32_t>(entrySize);
            return true;
        }
    }

    return false;
}

// A chunk view whose ChunkMatrix slot answers the matrix as-is (the base
// ChunkView's behaviour): enough to drive the real DrawPlacedModel path
const Matrix4x4* IdentityChunkMatrix(void* self, const Matrix4x4* matrix)
{
    (void)self;
    return matrix;
}
}

int main()
{
    std::vector<uint8_t> bh = ReadFile("disc/CRASH6/CRASH.BH");
    std::vector<uint8_t> bd = ReadFile("disc/CRASH6/CRASH.BD");
    if (bh.empty() || bd.empty())
    {
        return 1;
    }

    std::vector<BhEntry> entries;
    if (!ParseBh(bh, &entries))
    {
        std::fprintf(stderr, "BH parse failed\n");
        return 1;
    }

    const BhEntry* rm2 = nullptr;
    for (const BhEntry& entry : entries)
    {
        if (entry.path == "Levels\\Earth\\Hub\\beach.rm2")
        {
            rm2 = &entry;
        }
    }
    if (rm2 == nullptr || rm2->start + rm2->size > bd.size())
    {
        std::fprintf(stderr, "beach.rm2 not found\n");
        return 1;
    }
    const uint8_t* file = bd.data() + rm2->start;

    uint32_t graphicsOffset, graphicsSize;
    if (!FindSection(file, rm2->size, 11, &graphicsOffset, &graphicsSize))
    {
        std::fprintf(stderr, "graphics section not found\n");
        return 1;
    }
    const uint8_t* graphics = file + graphicsOffset;

    // The rigid mesh: kind 2 (models) item 0 submodel 0 (ModelRead layout:
    // s32 subModelCount, then per submodel vertexCount, packet size, packet)
    uint32_t modelsOffset, modelsSize;
    if (!FindItem(graphics, graphicsSize, 2, &modelsOffset, &modelsSize))
    {
        std::fprintf(stderr, "models subsection not found\n");
        return 1;
    }
    const uint8_t* models = graphics + modelsOffset;
    uint32_t rigidItemOffset = ReadU32(models + 12);
    const uint8_t* rigidItem = models + rigidItemOffset;
    uint32_t rigidSubModels = ReadU32(rigidItem);
    uint32_t rigidVerts = ReadU32(rigidItem + 4);
    uint32_t rigidSize = ReadU32(rigidItem + 8);
    const uint8_t* rigidPacket = rigidItem + 12;
    std::printf("rigid: subModels %u vertexCount %u packetSize %u\n", rigidSubModels, rigidVerts, rigidSize);

    // The skinned mesh: kind 4 (skins), first item whose submodel 0 parses
    // (SkinRead layout: s32 count, then per submodel material id, packet
    // size, vertex count, packet)
    uint32_t skinsOffset, skinsSize;
    if (!FindItem(graphics, graphicsSize, 4, &skinsOffset, &skinsSize))
    {
        std::fprintf(stderr, "skins subsection not found\n");
        return 1;
    }
    const uint8_t* skins = graphics + skinsOffset;
    uint32_t skinItems = ReadU32(skins + 4);
    const uint8_t* skinPacket = nullptr;
    uint32_t skinVerts = 0;
    uint32_t skinSize = 0;
    uint32_t skinItemIndex = 0;
    for (uint32_t item = 0; item < skinItems; item++)
    {
        uint32_t itemOffset = ReadU32(skins + 12 + item * 12);
        const uint8_t* data = skins + itemOffset;
        uint32_t subCount = ReadU32(data);
        if (subCount == 0)
        {
            continue;
        }

        uint32_t packetSize = ReadU32(data + 4 + 4);
        uint32_t vertexCount = ReadU32(data + 4 + 8);
        Ps2SkinMesh probe;
        if (ParseSkinSubModel(data + 4 + 12, packetSize, vertexCount, 0, &probe))
        {
            skinPacket = data + 4 + 12;
            skinVerts = vertexCount;
            skinSize = packetSize;
            skinItemIndex = item;
            break;
        }
    }
    if (skinPacket == nullptr)
    {
        std::fprintf(stderr, "no parseable skin submodel\n");
        return 1;
    }
    std::printf("skin: item %u vertexCount %u packetSize %u\n", skinItemIndex, skinVerts, skinSize);

    // bgfx headless on the Noop renderer: no window, no GPU. The mesh cache
    // parses, creates, binds and submits for real; presentation is skipped.
    bgfx::Init init;
    init.type = bgfx::RendererType::Noop;
    if (!bgfx::init(init))
    {
        std::fprintf(stderr, "bgfx Noop init failed\n");
        return 1;
    }

    // The host models around the file bytes (the packets' addresses are the
    // parse cache's keys)
    Platform::Graphics::HostRigidModelData rigidData = {};
    rigidData.subModelCount = 1;
    u32 rigidCounts[1] = {rigidVerts};
    u32 rigidSizes[1] = {rigidSize};
    const u8* rigidPackets[1] = {rigidPacket};
    rigidData.vertexCounts = rigidCounts;
    rigidData.sizes = rigidSizes;
    rigidData.packets = rigidPackets;
    Platform::Graphics::HostRigidModel rigidModel = {};
    rigidModel.model = &rigidData;
    rigidModel.count = 1;

    Platform::Graphics::HostSkin hostSkin = {};
    hostSkin.count = 1;
    u32 skinCounts[1] = {skinVerts};
    u32 skinSizes[1] = {skinSize};
    const u8* skinPackets[1] = {skinPacket};
    hostSkin.vertexCounts = skinCounts;
    hostSkin.sizes = skinSizes;
    hostSkin.packets = skinPackets;

    // Identity world + view: the mvp is a transpose of identity, so the
    // submit path (buffers, uniforms, program) runs exactly as in game
    Matrix4x4 identity = {};
    identity.m[0][0] = 1.0f;
    identity.m[1][1] = 1.0f;
    identity.m[2][2] = 1.0f;
    identity.m[3][3] = 1.0f;
    Platform::Graphics::HostSetViewMatrix(&identity);
    Vector4 lightDir = {0.0f, 0.0f, 1.0f, 0.0f};
    Vector4 lightColour = {1.0f, 1.0f, 1.0f, 1.0f};
    Vector4 ambient = {0.2f, 0.2f, 0.2f, 1.0f};
    Platform::Graphics::ModelLights lights = {&lightDir, &lightColour, &ambient};


    // One rigid + one skinned submit through the game-facing draws, then the
    // same rigid mesh placed through a chunk view (exercises DrawPlacedModel
    // and LodModelAt: the LOD holds the rigid mesh with no distance limits)
    Platform::Graphics::DrawRigidModel(reinterpret_cast<RigidModel*>(&rigidModel), &identity, lights, 1);
    Matrix4x4 joints[4] = {identity, identity, identity, identity};
    Platform::Graphics::DrawSkin(reinterpret_cast<Skin*>(&hostSkin), joints, 4, &identity, lights, 1);

    GccVTableEntry chunkVtable[8] = {};
    chunkVtable[5].function = reinterpret_cast<void*>(IdentityChunkMatrix);
    ChunkView chunkView = {};
    chunkView.toScreen = identity;
    chunkView.visibility = ChunkView::InView;
    chunkView.vtable = chunkVtable;
    Platform::Graphics::HostLod lod = {};
    Platform::Graphics::HostRigidModel* lodMeshes[1] = {&rigidModel};
    lod.meshes = lodMeshes;
    lod.count = 1;
    lod.nearest = 0;
    lod.farthest = 0xFFFFFFFF;
    RigidModel* lodMesh = Platform::Graphics::LodModelAt(reinterpret_cast<Lod*>(&lod), 100);
    if (lodMesh == nullptr)
    {
        std::fprintf(stderr, "LodModelAt returned null\n");
        bgfx::shutdown();
        return 1;
    }
    Platform::Graphics::DrawPlacedModel(lodMesh, &chunkView, &identity);

    bgfx::frame();
    const bgfx::Stats* after = bgfx::getStats();
    const Platform::Graphics::HostDrawCounters& counts = Platform::Graphics::HostDrawCounts();
    // The Noop renderer runs the whole submit path (parse, buffers, uniforms, program lookup) but rasterizes
    // nothing, so its stats stay 0 by design: the host counters are the proof's draw counts
    std::printf("bgfx draws: %u (Noop rasterizes nothing)\n", after->numDraw);
    std::printf("host draws: rigid %u skin %u\n", counts.rigidDraws, counts.skinDraws);
    std::printf("checksums: rigid %08x skin %08x\n", counts.rigidChecksum, counts.skinChecksum);
    if (counts.rigidDraws != 2 || counts.skinDraws != 1 || counts.rigidChecksum == 0 || counts.skinChecksum == 0)
    {
        std::fprintf(stderr, "draw-count proof FAILED\n");
        bgfx::shutdown();
        return 1;
    }

    std::printf("draws-proof: PASS\n");
    bgfx::shutdown();
    return 0;
}
