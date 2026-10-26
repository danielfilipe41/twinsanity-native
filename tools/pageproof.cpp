// Headless proof of the particle pages' picture: the default chunk's (Startup\Default.rm2) particle section's three
// page records (texture id and material id), the material each page takes through the game's own
// g_MaterialTable.Acquire (the way ReadParticlePageData takes it), and the texture that material's first textured
// shader samples (the page's picture) with its size.
//
// The game reads the default chunk's pages in ReadParticleData (src/game/particles.cpp) with
// Platform::Graphics::ReadParticlePage(..., fromStream=false), which takes the material from the material table by
// id; the console's page picture is that material's first shader's texture (ps2/renderer/particles.cpp's
// FinishModeShader). The proof prints what each page binds and writes the page's texture as a PNG, so the art the
// page holds can be looked at.
//
// Build: ninja page-proof && ./build/host/page_proof [output directory]
#include "../src/platform/sdl3/graphics/host_graphics.h"
#include "../src/platform/sdl3/graphics/texture_decoder.h"

#include "gcc2.h"
#include "game/graphicstables.h"
#include "game/stream.h"

#include <bgfx/bgfx.h>

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
    uint32_t value;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

uint16_t ReadU16(const uint8_t* at)
{
    uint16_t value;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

std::vector<uint8_t> ReadFile(const std::string& path)
{
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
    {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return {};
    }
    std::fseek(file, 0, SEEK_END);
    long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (size > 0 && std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size())
    {
        bytes.clear();
    }
    std::fclose(file);
    return bytes;
}

// The BH table (src/game/archive.cpp ArchiveSectionReader::Read): s32 header, then per file s32 path length, path
// bytes, s32 BD start, s32 size
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
        std::memcpy(&length, bh.data() + at, 4);
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

// A section's item table: the item kind's offset/size/id
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

// One item's bytes as a stream, read through the game's own resource path
struct ItemStream
{
    MemoryStream stream;

    ItemStream(void* bytes, u32 size)
    {
        MemoryStream::Construct(&stream, bytes, size, 0, MemoryStream::FileAlignment);
    }
};

// A PPM of the decoded page, so the art can be looked at
bool WritePpm(const char* path, const Ps2DecodedTexture& texture)
{
    FILE* file = std::fopen(path, "wb");
    if (file == nullptr)
    {
        return false;
    }
    std::fprintf(file, "P6\n%u %u\n255\n", texture.width, texture.height);
    for (size_t pixel = 0; pixel + 3 < texture.rgba.size(); pixel += 4)
    {
        std::fwrite(&texture.rgba[pixel], 1, 3, file);
    }
    std::fclose(file);
    return true;
}
}

int main(int argc, char** argv)
{
    const std::string output = argc > 1 ? argv[1] : "/tmp/opencode";
    const std::vector<uint8_t> bh = ReadFile("disc/CRASH6/CRASH.BH");
    const std::vector<uint8_t> bd = ReadFile("disc/CRASH6/CRASH.BD");
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
        if (entry.path == "Startup\\Default.rm2")
        {
            rm2 = &entry;
        }
    }
    if (rm2 == nullptr)
    {
        std::fprintf(stderr, "Default.rm2 not found\n");
        return 1;
    }
    const uint8_t* file = bd.data() + rm2->start;

    // The file's sections: the particles' (8) and the graphics' (11)
    uint32_t particleOffset = 0;
    uint32_t particleSize = 0;
    uint32_t graphicsOffset = 0;
    uint32_t graphicsSize = 0;
    if (!FindItem(file, rm2->size, 8, &particleOffset, &particleSize) ||
        !FindItem(file, rm2->size, 11, &graphicsOffset, &graphicsSize))
    {
        std::fprintf(stderr, "no sections\n");
        return 1;
    }

    // The graphics section's textures (item kind 0) and materials (item kind 1)
    const uint8_t* graphics = file + graphicsOffset;
    uint32_t texturesOffset = 0;
    uint32_t texturesSize = 0;
    uint32_t materialsOffset = 0;
    uint32_t materialsSize = 0;
    if (!FindItem(graphics, graphicsSize, 0, &texturesOffset, &texturesSize) ||
        !FindItem(graphics, graphicsSize, 1, &materialsOffset, &materialsSize))
    {
        std::fprintf(stderr, "no graphics items\n");
        return 1;
    }
    uint8_t* textures = const_cast<uint8_t*>(graphics) + texturesOffset;
    uint8_t* materials = const_cast<uint8_t*>(graphics) + materialsOffset;

    // The game's static initializers (the graphics tables among them) and the headless bgfx the uploads need
    RunStaticConstructors();
    bgfx::Init init;
    init.type = bgfx::RendererType::Noop;
    if (!bgfx::init(init))
    {
        std::fprintf(stderr, "bgfx Noop init failed\n");
        return 1;
    }

    // Every texture through the game's ReadTexture: its bytes decoded and held in HostTextures under its
    // GameTexture*, and the item's own header read for the dump
    const uint32_t textureCount = ReadU32(textures + 4);
    for (uint32_t index = 0; index < textureCount; index++)
    {
        size_t at = 12 + static_cast<size_t>(index) * 12;
        if (at + 12 > texturesSize)
        {
            break;
        }
        uint32_t offset = ReadU32(textures + at);
        uint32_t size = ReadU32(textures + at + 4);
        uint32_t id = ReadU32(textures + at + 8);
        ItemStream item(textures + offset, size);
        TextureFromStream(id, &item.stream);
    }

    // Every material through ReadMaterial: its first textured shader's texture taken from the table
    const uint32_t materialCount = ReadU32(materials + 4);
    for (uint32_t index = 0; index < materialCount; index++)
    {
        size_t at = 12 + static_cast<size_t>(index) * 12;
        if (at + 12 > materialsSize)
        {
            break;
        }
        uint32_t offset = ReadU32(materials + at);
        uint32_t size = ReadU32(materials + at + 4);
        uint32_t id = ReadU32(materials + at + 8);
        ItemStream item(materials + offset, size);
        MaterialFromStream(id, &item.stream);
    }

    // The default chunk's particle section's three page records: the texture id and the material id
    const uint8_t* particles = file + particleOffset;
    for (uint32_t page = 0; page < 3; page++)
    {
        uint32_t textureId = ReadU32(particles + page * 8);
        uint32_t materialId = ReadU32(particles + page * 8 + 4);

        // The page's material, taken from the table the way ReadParticlePageData takes it
        MaterialResource* resource = MaterialFromStream(materialId, nullptr);
        auto* host = resource != nullptr ? reinterpret_cast<Platform::Graphics::HostMaterial*>(resource->material) : nullptr;
        std::printf("page %u: texture id %08x  material id %08x", page, textureId, materialId);
        if (host == nullptr || host->gameTexture == nullptr)
        {
            std::printf(" -> no picture\n");
            continue;
        }

        // The texture the material's shader samples: the id whose GameTexture it is
        u32 pictureId = 0;
        for (uint32_t index = 0; index < textureCount; index++)
        {
            size_t at = 12 + static_cast<size_t>(index) * 12;
            uint32_t id = ReadU32(textures + at + 8);
            TextureTable::Entry* entry = g_TextureTable.Find(id);
            if (entry != nullptr && entry->item == host->gameTexture)
            {
                pictureId = id;
                break;
            }
        }
        std::printf(" -> picture texture %08x", pictureId);

        // The material's first shader's settings bytes: the retail reads them one field at a time (the retail's
        // ReadBasePrecompShader_, shaderclasses.cpp's ReadShader), one byte a field. The "linear" one (byte 23)
        // tells the GS's TEX1 to filter the page; the animation flag is byte 29 (not byte 27, which is noDepthWrites)
        {
            const u8* settings = host->shaderSettings;
            std::printf("  settings");
            for (uint32_t byte = 0; byte < 30; byte++)
            {
                std::printf(" %02x", settings[byte]);
            }
            std::printf("  linear(23)=%d noDepthWrites(27)=%d animatedColour(28)=%d hasAnim(29)=%d",
                        (settings[Platform::Graphics::HostShaderSettingLinear] & 1) ? 1 : 0,
                        (settings[Platform::Graphics::HostShaderSettingNoDepthWrites] & 1) ? 1 : 0,
                        (settings[Platform::Graphics::HostShaderSettingAnimatedColour] & 1) ? 1 : 0,
                        (settings[Platform::Graphics::HostShaderSettingHasAnimation] & 1) ? 1 : 0);
        }

        // The picture's own bytes decoded, so the art can be looked at
        for (uint32_t index = 0; index < textureCount; index++)
        {
            size_t at = 12 + static_cast<size_t>(index) * 12;
            if (ReadU32(textures + at + 8) != pictureId)
            {
                continue;
            }
            uint32_t offset = ReadU32(textures + at);
            uint32_t size = ReadU32(textures + at + 4);
            const uint8_t* item = textures + offset;
            const uint8_t* header = item + 4;
            Ps2TextureHeader decoded;
            decoded.widthPower = ReadU16(header + 4);
            decoded.heightPower = ReadU16(header + 6);
            decoded.levels = header[8];
            decoded.pixelFormat = header[9];
            decoded.uploadFormat = header[10];
            decoded.hasAlpha = header[11];
            decoded.function = header[13];
            for (int level2 = 0; level2 < 7; level2++)
            {
                std::memcpy(&decoded.levelOffsets[level2], header + 0x10 + level2 * 4, 4);
                std::memcpy(&decoded.levelWidths[level2], header + 0x2C + level2 * 4, 4);
            }
            decoded.paletteOffset = ReadU32(header + 0x48);
            const uint8_t* transfer = item + 4 + 0x60 + 0x20;
            size_t transferSize = size - (4 + 0x60 + 0x20);
            Ps2DecodedTexture picture;
            if (DecodePs2Texture(&decoded, transfer, transferSize, &picture))
            {
                std::printf("  %ux%u", picture.width, picture.height);
                std::printf("  texfn %u hasAlpha %u levels %u psm %02x", decoded.function, decoded.hasAlpha, decoded.levels,
                            decoded.pixelFormat);
                char name[256];
                std::snprintf(name, sizeof(name), "%s/page_%u.ppm", output.c_str(), page);
                WritePpm(name, picture);
                std::printf(" -> %s", name);
            }
            break;
        }
        std::printf("\n");
    }

    bgfx::shutdown();
    return 0;
}
