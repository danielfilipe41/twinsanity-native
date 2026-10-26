// Headless proof for the SDL3 rigid-mesh builder: parses the first real model packet end to end.
// BH index (disc/CRASH6/CRASH.BH) -> BD bytes -> beach.rm2 graphics section -> kind 2 (models) item 0
// -> ParseRigidSubModel -> vertex/index counts + bounding box + checksum.
// Build: g++ -m32 <host flags> tools/mesh_golden.cpp src/platform/sdl3/graphics/mesh_builder.cpp
//     -o /tmp/mesh_golden
// Run from the repo root: /tmp/mesh_golden
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

    // RM2 section 11 (graphics), subsection 2 (models), item 0
    uint32_t graphicsOffset, graphicsSize;
    if (!FindSection(file, rm2->size, 11, &graphicsOffset, &graphicsSize))
    {
        std::fprintf(stderr, "graphics section not found\n");
        return 1;
    }

    const uint8_t* graphics = file + graphicsOffset;
    uint32_t modelsOffset, modelsSize;
    if (!FindItem(graphics, graphicsSize, 2, &modelsOffset, &modelsSize))
    {
        std::fprintf(stderr, "models subsection not found\n");
        return 1;
    }

    const uint8_t* models = graphics + modelsOffset;
    uint32_t itemCount = ReadU32(models + 4);
    std::printf("model items: %u\n", itemCount);
    if (itemCount == 0)
    {
        return 1;
    }

    uint32_t itemOffset = ReadU32(models + 12);
    uint32_t itemSize = ReadU32(models + 16);
    const uint8_t* item = models + itemOffset;

    // resources.cpp ModelRead: s32 subModelCount, then per submodel vertexCount, packet size, packet
    uint32_t subModelCount = ReadU32(item);
    std::printf("item 0 size %u subModels %u\n", itemSize, subModelCount);
    if (subModelCount == 0)
    {
        return 1;
    }

    uint32_t vertexCount = ReadU32(item + 4);
    uint32_t packetSize = ReadU32(item + 8);
    const uint8_t* packet = item + 12;
    std::printf("submodel 0: vertexCount %u packetSize %u\n", vertexCount, packetSize);

    Ps2RigidMesh mesh;
    if (!ParseRigidSubModel(packet, packetSize, vertexCount, &mesh))
    {
        std::fprintf(stderr, "mesh parse failed\n");
        return 1;
    }

    std::printf("parsed: vertices %u indexes %u (%u triangles)\n", mesh.vertexCount, mesh.indexCount,
                mesh.indexCount / 3);
    std::printf("bbox: [%.4f %.4f %.4f] - [%.4f %.4f %.4f]\n", mesh.boundsMin[0], mesh.boundsMin[1],
                mesh.boundsMin[2], mesh.boundsMax[0], mesh.boundsMax[1], mesh.boundsMax[2]);
    std::printf("checksum %08x\n", mesh.checksum);
    return 0;
}
