// Dumps a level's particle page texture (the 128x128 pages the particles sample) as a PNG, so the sprite
// inside a texture rectangle can be looked at.
//
// Build: g++ -m32 -Iinclude -Iinclude/host -o /tmp/opencode/pagedump tools/pagedump.cpp \
//        src/platform/sdl3/graphics/texture_decoder.cpp
// Run from the repo root: /tmp/opencode/pagedump <level.rm2 path in the BD>
#include "../src/platform/sdl3/graphics/texture_decoder.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

bool FindItem(const uint8_t* base, size_t span, uint32_t kind, uint32_t* offset, uint32_t* size)
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
        if (ReadU32(base + at + 8) == kind)
        {
            *offset = ReadU32(base + at);
            *size = ReadU32(base + at + 4);
            return true;
        }
    }
    return false;
}

void WritePng(const std::string& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba)
{
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
    {
        return;
    }
    // a minimal PNG writer: the IDAT is stored uncompressed (zlib "stored" blocks)
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (1 + static_cast<size_t>(width) * 4));
    for (uint32_t y = 0; y < height; y++)
    {
        raw.push_back(0);
        const uint8_t* row = rgba.data() + static_cast<size_t>(y) * width * 4;
        raw.insert(raw.end(), row, row + static_cast<size_t>(width) * 4);
    }

    auto chunk = [](const char* tag, const std::vector<uint8_t>& data, std::vector<uint8_t>* out) {
        uint32_t length = static_cast<uint32_t>(data.size());
        uint8_t header[4];
        std::memcpy(header, &length, 4);
        out->insert(out->end(), header, header + 4);
        size_t tagAt = out->size();
        out->insert(out->end(), tag, tag + 4);
        out->insert(out->end(), data.begin(), data.end());
        uint32_t crc = 0xFFFFFFFFu;
        auto crcByte = [&](uint8_t byte) {
            crc ^= byte;
            for (int bit = 0; bit < 8; bit++)
            {
                crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
            }
        };
        for (size_t index = tagAt; index < out->size(); index++)
        {
            crcByte((*out)[index]);
        }
        crc ^= 0xFFFFFFFFu;
        uint8_t tail[4];
        std::memcpy(tail, &crc, 4);
        out->insert(out->end(), tail, tail + 4);
    };

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    std::vector<uint8_t> ihdr;
    uint8_t buffer[13];
    std::memcpy(buffer, &width, 4);
    std::memcpy(buffer + 4, &height, 4);
    buffer[8] = 8;
    buffer[9] = 6;
    buffer[10] = 0;
    buffer[11] = 0;
    buffer[12] = 0;
    ihdr.assign(buffer, buffer + 13);
    chunk("IHDR", ihdr, &png);

    // zlib stream with stored blocks
    std::vector<uint8_t> zlib = {0x78, 0x01};
    size_t at = 0;
    while (at < raw.size())
    {
        size_t block = raw.size() - at;
        if (block > 65535)
        {
            block = 65535;
        }
        bool last = at + block >= raw.size();
        zlib.push_back(last ? 1 : 0);
        zlib.push_back(static_cast<uint8_t>(block & 0xFF));
        zlib.push_back(static_cast<uint8_t>((block >> 8) & 0xFF));
        zlib.push_back(static_cast<uint8_t>((~block) & 0xFF));
        zlib.push_back(static_cast<uint8_t>(((~block) >> 8) & 0xFF));
        zlib.insert(zlib.end(), raw.begin() + at, raw.begin() + at + block);
        at += block;
    }
    uint32_t adler = 1;
    for (uint8_t byte : raw)
    {
        adler = (adler + byte) % 65521;
    }
    adler = (adler << 16) | adler;
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        zlib.push_back(static_cast<uint8_t>((adler >> shift) & 0xFF));
    }
    chunk("IDAT", zlib, &png);
    chunk("IEND", {}, &png);

    std::fwrite(png.data(), 1, png.size(), file);
    std::fclose(file);
}
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: pagedump <level path inside the BD>\n");
        return 1;
    }
    std::vector<uint8_t> bh = ReadFile("disc/CRASH6/CRASH.BH");
    std::vector<uint8_t> bd = ReadFile("disc/CRASH6/CRASH.BD");
    std::vector<BhEntry> entries;
    if (!ParseBh(bh, &entries))
    {
        std::fprintf(stderr, "BH parse failed\n");
        return 1;
    }
    const BhEntry* level = nullptr;
    for (const BhEntry& entry : entries)
    {
        if (entry.path == argv[1])
        {
            level = &entry;
        }
    }
    if (level == nullptr)
    {
        std::fprintf(stderr, "level not found\n");
        return 1;
    }
    const uint8_t* file = bd.data() + level->start;
    uint32_t graphicsOffset = 0;
    uint32_t graphicsSize = 0;
    if (!FindSection(file, level->size, 11, &graphicsOffset, &graphicsSize))
    {
        std::fprintf(stderr, "no graphics section\n");
        return 1;
    }
    const uint8_t* graphics = file + graphicsOffset;
    uint32_t texturesOffset = 0;
    uint32_t texturesSize = 0;
    if (!FindItem(graphics, graphicsSize, 0, &texturesOffset, &texturesSize))
    {
        std::fprintf(stderr, "no textures item\n");
        return 1;
    }
    const uint8_t* textures = graphics + texturesOffset;
    uint32_t count = ReadU32(textures + 4);
    for (uint32_t index = 0; index < count; index++)
    {
        size_t at = 12 + static_cast<size_t>(index) * 12;
        if (at + 12 > texturesSize)
        {
            break;
        }
        uint32_t itemOffset = ReadU32(textures + at);
        uint32_t itemSize = ReadU32(textures + at + 4);
        const uint8_t* item = textures + itemOffset;
        const uint8_t* header = item + 4;
        uint16_t widthPower = ReadU16(header + 4);
        uint16_t heightPower = ReadU16(header + 6);
        uint32_t width = 1u << widthPower;
        uint32_t height = 1u << heightPower;
        // TS_PAGESIZE=256 dumps the 256 by 256 pages as well (the particle page's own size)
        u32 wanted = std::getenv("TS_PAGESIZE") != nullptr ? static_cast<u32>(std::atoi(std::getenv("TS_PAGESIZE"))) : 128;
        if (width != wanted || height != wanted)
        {
            continue;
        }
        Ps2TextureHeader decoded;
        decoded.widthPower = widthPower;
        decoded.heightPower = heightPower;
        decoded.levels = header[8];
        decoded.pixelFormat = header[9];
        decoded.uploadFormat = header[10];
        decoded.hasAlpha = header[11];
        decoded.function = header[13];
        for (int level2 = 0; level2 < 7; level2++)
        {
            decoded.levelOffsets[level2] = ReadU32(header + 0x10 + level2 * 4);
            decoded.levelWidths[level2] = ReadU32(header + 0x2C + level2 * 4);
        }
        decoded.paletteOffset = ReadU32(header + 0x48);

        const uint8_t* transfer = item + 4 + 0x60 + 0x20;
        size_t transferSize = itemSize - (4 + 0x60 + 0x20);

        Ps2DecodedTexture texture;
        if (!DecodePs2Texture(&decoded, transfer, transferSize, &texture))
        {
            std::fprintf(stderr, "item %u: decode failed\n", index);
            continue;
        }
        char name[64];
        std::snprintf(name, sizeof(name), "/tmp/opencode/page_%u.rgba", index);
        FILE* raw = std::fopen(name, "wb");
        if (raw != nullptr)
        {
            std::fwrite(texture.rgba.data(), 1, texture.rgba.size(), raw);
            std::fclose(raw);
        }
        std::snprintf(name, sizeof(name), "/tmp/opencode/page_%u.png", index);
        WritePng(name, texture.width, texture.height, texture.rgba);
        std::printf("item %u: %ux%u PSM 0x%02x -> %s\n", index, texture.width, texture.height, decoded.pixelFormat, name);
    }
    return 0;
}
