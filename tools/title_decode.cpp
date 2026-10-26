// Offline decode of a chunk's graphics section: every model/mesh/skin/blend-skin
// resource of an RM2 inside CRASH.BD, each submodel packet through the SDL3 host
// mesh builder, printing the parsed geometry so the title screen's meshes can be
// surveyed without launching the game.
// Build: g++ -m32 -malign-double -msse2 -mfpmath=sse -O2 -std=gnu++26 -fno-pie
//     -fno-exceptions -fno-rtti -Iinclude -Iinclude/host tools/title_decode.cpp
//     src/platform/sdl3/graphics/mesh_builder.cpp -o /tmp/title_decode
#include "../src/platform/sdl3/graphics/mesh_builder.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

// A subsection's own header: itemCount at +4, items at +12
uint32_t ItemCount(const uint8_t* subsection)
{
    return ReadU32(subsection + 4);
}

const uint8_t* ItemAt(const uint8_t* subsection, uint32_t index)
{
    return subsection + ReadU32(subsection + 12 + index * 12);
}

uint32_t ItemId(const uint8_t* subsection, uint32_t index)
{
    return ReadU32(subsection + 12 + index * 12 + 8);
}

uint32_t ItemSizeAt(const uint8_t* subsection, uint32_t index)
{
    return ReadU32(subsection + 12 + index * 12 + 4);
}

// ModelRead layout: s32 subModelCount, then per submodel s32 vertexCount, s32 size, packet, s32 extraSize
void SurveyModel(const uint8_t* data, uint32_t size, const char* label, uint32_t itemId)
{
    int32_t subs = ReadS32(data);
    if (subs <= 0 || size < 4)
    {
        std::printf("%s id %u: subModelCount %d (empty)\n", label, itemId, subs);
        return;
    }

    size_t at = 4;
    for (int32_t sub = 0; sub < subs && at + 8 <= size; sub++)
    {
        int32_t vertexCount = ReadS32(data + at);
        int32_t packetSize = ReadS32(data + at + 4);
        const uint8_t* packet = data + at + 8;
        if (packetSize <= 0)
        {
            std::printf("%s id %u sub %d: vertexCount %d packetSize %d (none)\n", label, itemId, sub, vertexCount,
                        packetSize);
            break;
        }

        Ps2RigidMesh mesh;
        bool ok = ParseRigidSubModel(packet, static_cast<size_t>(packetSize), static_cast<u32>(vertexCount), &mesh);
        if (ok)
        {
            // The fan probe: a mesh whose triangles mostly share one vertex renders as a fan from that vertex
            size_t fan = 0;
            if (mesh.vertexCount != 0)
            {
                std::vector<size_t> use(mesh.vertexCount, 0);
                for (size_t i : mesh.indexes)
                {
                    if (i < use.size())
                    {
                        use[i]++;
                    }
                }
                size_t best = 0;
                for (size_t count : use)
                {
                    if (count > best)
                    {
                        best = count;
                    }
                }
                fan = best;
            }
            std::printf("%s id %u sub %d: verts %u tris %zu fanmax %zu bbox [%.2f %.2f %.2f]-[%.2f %.2f %.2f] crc %08x\n",
                        label, itemId, sub, mesh.vertexCount, mesh.indexes.size() / 3, fan, mesh.boundsMin[0],
                        mesh.boundsMin[1], mesh.boundsMin[2], mesh.boundsMax[0], mesh.boundsMax[1], mesh.boundsMax[2],
                        mesh.checksum);
        }
        else
        {
            std::printf("%s id %u sub %d: PARSE FAIL vertexCount %d packetSize %d\n", label, itemId, sub, vertexCount,
                        packetSize);
        }

        at += 8 + static_cast<size_t>(packetSize);
        int32_t extraSize = ReadS32(data + at);
        at += 4;
        if (extraSize > 0)
        {
            at += static_cast<size_t>(extraSize);
        }
    }
}

// SkinRead layout: s32 count, then per submodel s32 material id, s32 size, s32 vertexCount, packet
void SurveySkin(const uint8_t* data, const char* label, uint32_t itemId)
{
    int32_t count = ReadS32(data);
    if (count <= 0)
    {
        std::printf("%s id %u: count %d (empty)\n", label, itemId, count);
        return;
    }

    size_t at = 4;
    for (int32_t sub = 0; sub < count; sub++)
    {
        int32_t material = ReadS32(data + at);
        int32_t size = ReadS32(data + at + 4);
        int32_t vertexCount = ReadS32(data + at + 8);
        const uint8_t* packet = data + at + 12;
        Ps2SkinMesh mesh;
        bool ok = size > 0 && ParseSkinSubModel(packet, static_cast<size_t>(size), static_cast<u32>(vertexCount), 0, &mesh);
        if (ok)
        {
            std::printf("%s id %u sub %d: material %d verts %u tris %zu joints %u crc %08x\n", label, itemId, sub,
                        material, mesh.vertexCount, mesh.indexes.size() / 3, mesh.jointCount, mesh.checksum);
        }
        else
        {
            std::printf("%s id %u sub %d: PARSE FAIL material %d vertexCount %d packetSize %d\n", label, itemId, sub,
                        material, vertexCount, size);
        }

        at += 12 + static_cast<size_t>(size);
    }
}

// BlendSkinRead layout: s32 count, s32 shapeCount, then per submodel s32 partCount, s32 material, parts
void SurveyBlendSkin(const uint8_t* data, const char* label, uint32_t itemId)
{
    int32_t count = ReadS32(data);
    int32_t shapes = ReadS32(data + 4);
    if (count <= 0)
    {
        std::printf("%s id %u: count %d (empty)\n", label, itemId, count);
        return;
    }

    size_t at = 8;
    for (int32_t sub = 0; sub < count; sub++)
    {
        int32_t partCount = ReadS32(data + at);
        int32_t material = ReadS32(data + at + 4);
        at += 8;
        for (int32_t part = 0; part < partCount; part++)
        {
            int32_t packetSize = ReadS32(data + at);
            // +4 is BlendPart.unused20: retail reads it with the packet size and drops it (renderer.h), so the walk skips it
            const uint8_t* packet = data + at + 8;
            Ps2SkinMesh mesh;
            bool ok = packetSize > 0 &&
                      ParseSkinSubModel(packet, static_cast<size_t>(packetSize), 0, static_cast<u32>(shapes), &mesh);
            if (ok)
            {
                std::printf("%s id %u sub %d part %d: material %d verts %u tris %zu joints %u shapes %u crc %08x\n",
                            label, itemId, sub, part, material, mesh.vertexCount, mesh.indexes.size() / 3,
                            mesh.jointCount, mesh.shapeCount, mesh.checksum);
            }
            else
            {
                std::printf("%s id %u sub %d part %d: PARSE FAIL material %d packetSize %d\n", label, itemId, sub,
                            part, material, packetSize);
            }

            at += 8 + static_cast<size_t>(packetSize) + 12;
            for (int32_t shape = 0; shape < shapes; shape++)
            {
                int32_t shapeSize = ReadS32(data + at);
                // +4 is the shape's vertex count: VU1 unpacks that many bytes, the walk only needs the blob's size
                at += 8 + (static_cast<size_t>(shapeSize) << 4);
            }
        }
    }
}
}

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "Levels\\Earth\\Hub\\beach.rm2";
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
        if (entry.path == path)
        {
            rm2 = &entry;
        }
    }
    if (rm2 == nullptr || rm2->start + rm2->size > bd.size())
    {
        std::fprintf(stderr, "%s not found\n", path);
        return 1;
    }
    const uint8_t* file = bd.data() + rm2->start;
    std::printf("file %s size %u\n", path, rm2->size);

    uint32_t graphicsOffset = 0;
    uint32_t graphicsSize = 0;
    if (!FindSection(file, rm2->size, 11, &graphicsOffset, &graphicsSize))
    {
        std::fprintf(stderr, "graphics section not found\n");
        return 1;
    }
    const uint8_t* graphics = file + graphicsOffset;

    // The graphics section's subsections by kind
    const uint8_t* subsections[16] = {};
    uint32_t subsectionCount = ItemCount(graphics);
    for (uint32_t index = 0; index < subsectionCount; index++)
    {
        uint32_t kind = ItemId(graphics, index);
        if (kind < 16)
        {
            subsections[kind] = ItemAt(graphics, index);
        }
    }

    for (uint32_t kind = 0; kind < 12; kind++)
    {
        if (subsections[kind] != nullptr)
        {
            std::printf("kind %u: %u items (size %u)\n", kind, ItemCount(subsections[kind]),
                        ItemSizeAt(graphics, kind));
        }
    }

    const uint8_t* models = subsections[2];
    if (models != nullptr)
    {
        uint32_t count = ItemCount(models);
        std::printf("=== models (kind 2): %u items ===\n", count);
        for (uint32_t index = 0; index < count; index++)
        {
            SurveyModel(ItemAt(models, index), ItemSizeAt(models, index), "model", ItemId(models, index));
        }
    }

    const uint8_t* meshes = subsections[6];
    if (meshes != nullptr)
    {
        uint32_t count = ItemCount(meshes);
        std::printf("=== meshes (kind 6): %u items ===\n", count);
        for (uint32_t index = 0; index < count; index++)
        {
            // MeshRead: s32 version, then ReadRigidModelData: s32 materialCount, ids, s32 modelId
            const uint8_t* data = ItemAt(meshes, index) + 4;
            int32_t materialCount = ReadS32(data);
            int32_t modelId = ReadS32(data + 4 + static_cast<size_t>(materialCount) * 4);
            std::printf("mesh id %u: materials %d modelId %d\n", ItemId(meshes, index), materialCount, modelId);
        }
    }

    const uint8_t* skins = subsections[4];
    if (skins != nullptr)
    {
        uint32_t count = ItemCount(skins);
        std::printf("=== skins (kind 4): %u items ===\n", count);
        for (uint32_t index = 0; index < count; index++)
        {
            SurveySkin(ItemAt(skins, index), "skin", ItemId(skins, index));
        }
    }

    const uint8_t* blends = subsections[5];
    if (blends != nullptr)
    {
        uint32_t count = ItemCount(blends);
        std::printf("=== blendskins (kind 5): %u items ===\n", count);
        for (uint32_t index = 0; index < count; index++)
        {
            SurveyBlendSkin(ItemAt(blends, index), "blendskin", ItemId(blends, index));
        }
    }

    const uint8_t* lods = subsections[7];
    if (lods != nullptr)
    {
        uint32_t count = ItemCount(lods);
        std::printf("=== lods (kind 7): %u items ===\n", count);
        for (uint32_t index = 0; index < count; index++)
        {
            const uint8_t* data = ItemAt(lods, index);
            int32_t version = ReadS32(data);
            std::printf("lod id %u: version %04x\n", ItemId(lods, index), version);
        }
    }

    const uint8_t* skies = subsections[8];
    if (skies != nullptr)
    {
        uint32_t count = ItemCount(skies);
        std::printf("=== skies (kind 8): %u items ===\n", count);
        for (uint32_t index = 0; index < count; index++)
        {
            const uint8_t* data = ItemAt(skies, index);
            int32_t meshCount = ReadS32(data + 4);
            std::printf("sky id %u: meshes %d ids", ItemId(skies, index), meshCount);
            for (int32_t mesh = 0; mesh < meshCount; mesh++)
            {
                std::printf(" %d", ReadS32(data + 8 + mesh * 4));
            }
            std::printf("\n");
        }
    }

    return 0;
}
