// Headless proof for the SDL3 skin-mesh builder: parses every real skin
// submodel end to end. BH index (disc/CRASH6/CRASH.BH) -> BD bytes ->
// beach.rm2 graphics section -> kind 4 (skins) items -> ParseSkinSubModel ->
// joint/vertex/shape counts + checksums. Also walks kind 5 (blend skins) for
// part/shape counts.
// Build: g++ -m32 <host flags> tools/skin_golden.cpp
//     src/platform/sdl3/graphics/mesh_builder.cpp -o /tmp/skin_golden
// Run from the repo root: /tmp/skin_golden
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
    uint32_t word;
    std::memcpy(&word, at, sizeof(word));
    return word;
}

int32_t ReadS32(const uint8_t* at)
{
    int32_t word;
    std::memcpy(&word, at, sizeof(word));
    return word;
}

std::vector<uint8_t> ReadFile(const char* path)
{
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        std::fprintf(stderr, "cannot open %s\n", path);
        return {};
    }

    std::fseek(file, 0, SEEK_END);
    long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    std::vector<uint8_t> bytes(size > 0 ? static_cast<size_t>(size) : 0);
    if (size > 0 && std::fread(bytes.data(), 1, static_cast<size_t>(size), file) != static_cast<size_t>(size))
    {
        std::fclose(file);
        return {};
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
        int32_t length;
        std::memcpy(&length, bh.data() + at, sizeof(length));
        at += 4;
        if (length <= 0 || at + static_cast<size_t>(length) + 8 > bh.size())
        {
            return false;
        }

        BhEntry entry;
        entry.path.assign(reinterpret_cast<const char*>(bh.data() + at), length);
        at += length;
        entry.start = ReadU32(bh.data() + at);
        entry.size = ReadU32(bh.data() + at + 4);
        at += 8;
        entries->push_back(entry);
    }

    return true;
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

    uint32_t count = ReadU32(base + 4);
    for (uint32_t index = 0; index < count; index++)
    {
        size_t at = 12 + static_cast<size_t>(index) * 12;
        if (at + 12 > span)
        {
            return false;
        }

        if (ReadU32(base + at + 8) == id)
        {
            *offset = ReadU32(base + at);
            *size = ReadU32(base + at + 4);
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

    uint32_t count = ReadU32(sectionBase + 4);
    for (uint32_t index = 0; index < count; index++)
    {
        size_t at = 12 + static_cast<size_t>(index) * 12;
        if (at + 12 > span)
        {
            return false;
        }

        if (ReadU32(sectionBase + at + 8) == kind)
        {
            *offset = ReadU32(sectionBase + at);
            *size = ReadU32(sectionBase + at + 4);
            return true;
        }
    }

    return false;
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

    std::printf("beach.rm2 at BD+%u size %u\n", rm2->start, rm2->size);
    const uint8_t* file = bd.data() + rm2->start;

    uint32_t graphicsOffset, graphicsSize;
    if (!FindSection(file, rm2->size, 11, &graphicsOffset, &graphicsSize))
    {
        std::fprintf(stderr, "graphics section not found\n");
        return 1;
    }

    const uint8_t* graphics = file + graphicsOffset;
    uint32_t skinsOffset, skinsSize;
    if (!FindItem(graphics, graphicsSize, 4, &skinsOffset, &skinsSize))
    {
        std::fprintf(stderr, "skins subsection not found\n");
        return 1;
    }

    // Skin subsection: u32 format, u32 itemCount, u32 size, then per item
    // u32 offset, s32 size, u32 id. Each item (SkinRead): s32 count, then per
    // submodel u32 material id, u32 packet size, u32 vertex count, packet.
    const uint8_t* skins = graphics + skinsOffset;
    uint32_t skinItems = ReadU32(skins + 4);
    std::printf("skin items: %u\n", skinItems);
    uint32_t totalVerts = 0;
    uint32_t maxJoints = 0;
    for (uint32_t item = 0; item < skinItems; item++)
    {
        uint32_t itemOffset = ReadU32(skins + 12 + item * 12);
        const uint8_t* data = skins + itemOffset;
        uint32_t subCount = ReadU32(data);
        size_t at = 4;
        for (uint32_t sub = 0; sub < subCount; sub++)
        {
            uint32_t packetSize = ReadU32(data + at + 4);
            uint32_t vertexCount = ReadU32(data + at + 8);
            const uint8_t* packet = data + at + 12;
            at += 12 + packetSize;
            Ps2SkinMesh mesh;
            if (!ParseSkinSubModel(packet, packetSize, vertexCount, 0, &mesh))
            {
                std::fprintf(stderr, "skin item %u sub %u parse failed (verts %u size %u)\n", item, sub,
                             vertexCount, packetSize);
                return 1;
            }

            std::printf("skin %u sub %u: joints %u verts %u indexes %u checksum %08x\n", item, sub,
                        mesh.jointCount, mesh.vertexCount, mesh.indexCount, mesh.checksum);
            totalVerts += mesh.vertexCount;
            if (mesh.jointCount > maxJoints)
            {
                maxJoints = mesh.jointCount;
            }
        }
    }

    std::printf("skins total: %u verts, max joints %u\n", totalVerts, maxJoints);

    // Blend skins (kind 5): count, shapeCount, per submodel partCount +
    // material, per part packetSize, unused, packet, 3 factors, then per shape
    // size/count + blob. Walk the structure and report part/shape counts.
    uint32_t blendOffset, blendSize;
    if (!FindItem(graphics, graphicsSize, 5, &blendOffset, &blendSize))
    {
        std::fprintf(stderr, "blend subsection not found\n");
        return 1;
    }

    const uint8_t* blend = graphics + blendOffset;
    uint32_t blendItems = ReadU32(blend + 4);
    std::printf("blend items: %u\n", blendItems);
    for (uint32_t item = 0; item < blendItems; item++)
    {
        uint32_t itemOffset = ReadU32(blend + 12 + item * 12);
        const uint8_t* data = blend + itemOffset;
        uint32_t subCount = ReadU32(data);
        uint32_t shapeCount = ReadU32(data + 4);
        size_t at = 8;
        uint32_t parts = 0;
        for (uint32_t sub = 0; sub < subCount; sub++)
        {
            uint32_t partCount = ReadU32(data + at);
            at += 8;
            for (uint32_t part = 0; part < partCount; part++)
            {
                uint32_t packetSize = ReadU32(data + at);
                at += 8 + 12 + packetSize;
                for (uint32_t shape = 0; shape < shapeCount; shape++)
                {
                    uint32_t shapeSize = ReadU32(data + at);
                    at += 8 + shapeSize * 16;
                }

                parts++;
            }
        }

        std::printf("blend %u: submodels %u shapes %u parts %u\n", item, subCount, shapeCount, parts);
    }

    return 0;
}
