// Headless cache-hit proof: beach.rm2 texture item 0 through HostTextureCache twice.
// BH index -> BD bytes -> beach.rm2 graphics section -> texture item 0 -> transfer
// image -> GetOrUpload (miss) -> GetOrUpload (hit) -> HostSetMaterialTexture ->
// DrawSprite bind path. Prints both handles, hit flags, and the decoded checksum
// (must match the R2 golden 66ca21b6). bgfx runs headless on the Noop renderer: no
// window, no GPU; cache + decode + bind logic runs for real, presentation skipped.
// Build: g++ -m32 <host flags> tools/texture_cache_proof.cpp
//     src/platform/sdl3/graphics/texture_decoder.cpp
//     src/platform/sdl3/graphics/material_system.cpp
//     src/platform/sdl3/graphics/draw2d.cpp <bgfx libs> -o /tmp/texture_cache_proof
#include "../src/platform/sdl3/graphics/host_graphics.h"
#include "../src/platform/sdl3/graphics/texture_decoder.h"

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

uint32_t Crc32(const uint8_t* bytes, size_t size)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t index = 0; index < size; index++)
    {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; bit++)
        {
            crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
        }
    }
    return crc ^ 0xFFFFFFFF;
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

    // RM2 section 11 (graphics), subsection 0 (textures), item 0
    uint32_t graphicsOffset, graphicsSize;
    if (!FindSection(file, rm2->size, 11, &graphicsOffset, &graphicsSize))
    {
        return 1;
    }
    const uint8_t* graphics = file + graphicsOffset;
    uint32_t texturesOffset, texturesSize;
    if (!FindItem(graphics, graphicsSize, 0, &texturesOffset, &texturesSize))
    {
        return 1;
    }
    const uint8_t* textures = graphics + texturesOffset;
    uint32_t itemOffset = ReadU32(textures + 12);
    uint32_t itemSize = ReadU32(textures + 16);
    const uint8_t* item = textures + itemOffset;

    // resources.cpp TextureRead: u32 size, Texture struct, 0x20 leftover, packet
    const uint8_t* header = item + 4;
    Ps2TextureHeader decoded;
    decoded.widthPower = ReadU16(header + 4);
    decoded.heightPower = ReadU16(header + 6);
    decoded.levels = header[8];
    decoded.pixelFormat = header[9];
    decoded.uploadFormat = header[10];
    decoded.hasAlpha = header[11];
    decoded.function = header[13];
    for (int level = 0; level < 7; level++)
    {
        decoded.levelOffsets[level] = ReadU32(header + 0x10 + level * 4);
        decoded.levelWidths[level] = ReadU32(header + 0x2C + level * 4);
    }
    decoded.paletteOffset = ReadU32(header + 0x48);
    const uint8_t* transfer = item + 4 + 0x60 + 0x20;
    size_t transferSize = itemSize - (4 + 0x60 + 0x20);

    // The transfer chain: RET tag, DIRECT of the GIF tag + TRXPOS/TRXREG/TRXDIR
    // A+D writes, then the IMAGE GIF tag and the image bytes
    size_t at = 16;  // past RET
    uint32_t directWords = ReadU32(transfer + at) & 0x7FFF;
    at += 16 + static_cast<size_t>(directWords) * 16;
    at += 16;  // past the IMAGE GIF tag
    const uint8_t* image = transfer + at;
    size_t imageSize = transferSize - at;

    // bgfx headless on the Noop renderer: no window, no GPU. The cache decodes,
    // creates, and binds for real; presentation is skipped.
    bgfx::Init init;
    init.type = bgfx::RendererType::Noop;
    if (!bgfx::init(init))
    {
        std::fprintf(stderr, "bgfx Noop init failed\n");
        return 1;
    }

    Platform::Graphics::HostTextureKey key = {transfer,
                                              decoded.pixelFormat,
                                              decoded.widthPower,
                                              decoded.heightPower,
                                              decoded.levelOffsets[0],
                                              decoded.paletteOffset};
    Platform::Graphics::HostTextureUpload first =
        Platform::Graphics::HostTextures().GetOrUpload(key, &decoded, image, imageSize);
    Platform::Graphics::HostTextureUpload second =
        Platform::Graphics::HostTextures().GetOrUpload(key, &decoded, image, imageSize);
    std::printf("upload 1: handle %u hit %d\n", first.texture.idx, first.hit ? 1 : 0);
    std::printf("upload 2: handle %u hit %d\n", second.texture.idx, second.hit ? 1 : 0);
    if (!bgfx::isValid(first.texture) || first.hit || !second.hit ||
        second.texture.idx != first.texture.idx)
    {
        std::fprintf(stderr, "cache-hit proof FAILED\n");
        bgfx::shutdown();
        return 1;
    }

    // The material path: point a material at the upload, read back its bind handle
    alignas(8) uint8_t storage[Platform::Graphics::MaterialStorage];
    Platform::Graphics::ConstructMaterial(reinterpret_cast<Material*>(storage));
    auto* host = reinterpret_cast<Platform::Graphics::HostMaterial*>(storage);
    Platform::Graphics::HostSetMaterialTexture(host, first.texture);
    bgfx::TextureHandle bound = Platform::Graphics::HostMaterialTexture(host);
    std::printf("material bind: handle %u textured %d\n", bound.idx,
                (host->flags & Platform::Graphics::HostMaterialTextured) != 0 ? 1 : 0);
    if (!bgfx::isValid(bound) || bound.idx != first.texture.idx)
    {
        std::fprintf(stderr, "material bind FAILED\n");
        bgfx::shutdown();
        return 1;
    }

    // The bytes behind the handle match the R2 golden
    Ps2DecodedTexture golden;
    if (!DecodePs2Texture(&decoded, image, imageSize, 0, &golden))
    {
        std::fprintf(stderr, "decode failed\n");
        bgfx::shutdown();
        return 1;
    }
    uint32_t checksum = Crc32(golden.rgba.data(), golden.rgba.size());
    std::printf("decoded: %ux%u checksum %08x (golden 66ca21b6)\n", golden.width, golden.height, checksum);
    if (checksum != 0x66ca21b6)
    {
        std::fprintf(stderr, "checksum MISMATCH\n");
        bgfx::shutdown();
        return 1;
    }

    std::printf("texture-cache-proof: PASS (handle %u, checksum %08x)\n", first.texture.idx, checksum);
    bgfx::shutdown();
    return 0;
}
