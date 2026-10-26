// Golden texture test: decodes one real retail texture end to end.
// BH index (disc/CRASH6/CRASH.BH) -> BD bytes -> beach.rm2 graphics section
// -> texture item 0 -> DecodePs2Texture -> RGBA dump + checksum.
// Build: g++ -m32 <host flags> tools/texture_golden.cpp
//     src/platform/sdl3/graphics/texture_decoder.cpp -o /tmp/texture_golden
// Run from the repo root: /tmp/texture_golden
#include "../src/platform/sdl3/graphics/texture_decoder.h"

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

uint32_t Crc32(const std::vector<uint8_t>& bytes)
{
    uint32_t crc = 0xFFFFFFFF;
    for (uint8_t byte : bytes)
    {
        crc ^= byte;
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
    std::printf("BH entries: %zu\n", entries.size());

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
    uint32_t textureCount = ReadU32(textures + 4);
    std::printf("texture items: %u\n", textureCount);
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
    uint32_t imageQuadwords = ReadU32(transfer + at) & 0x7FFF;
    at += 16;
    const uint8_t* image = transfer + at;
    size_t imageSize = transferSize - at;
    std::printf("header: %ux%u levels %u PSM 0x%02x hasAlpha %u func %u paletteOffset 0x%x\n",
                1u << decoded.widthPower, 1u << decoded.heightPower, decoded.levels, decoded.pixelFormat,
                decoded.hasAlpha, decoded.function, decoded.paletteOffset);
    std::printf("image bytes: %zu (GIF tag says %u quadwords)\n", imageSize, imageQuadwords);

    Ps2DecodedTexture texture;
    if (!DecodePs2Texture(&decoded, image, imageSize, 0, &texture))
    {
        std::fprintf(stderr, "decode failed\n");
        return 1;
    }

    uint32_t checksum = Crc32(texture.rgba);
    std::printf("decoded: %ux%u first pixel RGBA %02x %02x %02x %02x checksum %08x\n", texture.width,
                texture.height, texture.rgba[0], texture.rgba[1], texture.rgba[2], texture.rgba[3], checksum);

    FILE* dump = std::fopen("/tmp/golden_rgba.bin", "wb");
    if (dump != nullptr)
    {
        std::fwrite(texture.rgba.data(), 1, texture.rgba.size(), dump);
        std::fclose(dump);
        std::printf("dumped %zu bytes to /tmp/golden_rgba.bin\n", texture.rgba.size());
    }
    return 0;
}
