// Headless material/texture linkage proof: a real retail chunk's textures through the game's own
// Platform::Graphics::ReadTexture (decoded into HostTextures) and its materials through ReadMaterial (each stashing
// its first textured shader's GameTexture*), then the SubmitQuad bind path (HostMaterialBoundTexture) resolving a
// material's texture to the cached handle. The decoded image's CRC32 is the golden 050c99e0: the decode is the
// TT-Lab EzSwizzle algorithm the reference port uses (the PSMCT32 write geometry, the PSMT8 read, the palette read
// from the same image and its CSM1 swap, channels scaled up). Earlier values pinned older, wrong decodes
// (66ca21b6 the raw bytes, dd72d204 the pre-EzSwizzle one whose textures came out as noise).
// bgfx runs headless on the Noop renderer: no window, no GPU; the cache, decode and bind logic run for real.
// Build: ninja texture-material-proof && ./build/host/texture_material_proof  (from the repo root)
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

// An RM2 section's item table at sectionBase: item kind's offset/size/id
bool FindItem(const uint8_t* sectionBase, size_t span, uint32_t kind, uint32_t* offset, uint32_t* size, uint32_t* id)
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
            *id = ReadU32(sectionBase + at + 8);
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

// One item's bytes as a stream, read through the game's own resource path (ReadTexture/ReadMaterial behind
// TextureFromStream/MaterialFromStream)
struct ItemStream
{
    MemoryStream stream;

    ItemStream(void* bytes, u32 size)
    {
        MemoryStream::Construct(&stream, bytes, size, 0, MemoryStream::FileAlignment);
    }
};
}

int main(int argc, char** argv)
{
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

    // RM2 section 11 (graphics), subsection 0 (textures) and 1 (materials)
    uint32_t graphicsOffset, graphicsSize, graphicsId;
    if (!FindItem(file, rm2->size, 11, &graphicsOffset, &graphicsSize, &graphicsId))
    {
        return 1;
    }
    const uint8_t* graphics = file + graphicsOffset;
    uint32_t texturesOffset, texturesSize, texturesId;
    uint32_t materialsOffset, materialsSize, materialsId;
    if (!FindItem(graphics, graphicsSize, 0, &texturesOffset, &texturesSize, &texturesId) ||
        !FindItem(graphics, graphicsSize, 1, &materialsOffset, &materialsSize, &materialsId))
    {
        return 1;
    }
    uint8_t* textures = const_cast<uint8_t*>(graphics) + texturesOffset;
    uint8_t* materials = const_cast<uint8_t*>(graphics) + materialsOffset;

    // The game's static initializers (the graphics tables among them), then the headless bgfx the uploads need
    RunStaticConstructors();
    bgfx::Init init;
    init.type = bgfx::RendererType::Noop;
    if (!bgfx::init(init))
    {
        std::fprintf(stderr, "bgfx Noop init failed\n");
        return 1;
    }

    // Every texture through the game's ReadTexture: each decoded and held in HostTextures under its GameTexture*
    const uint32_t textureCount = ReadU32(textures + 4);
    std::vector<GameTexture*> gameTextures;
    for (uint32_t index = 0; index < textureCount; index++)
    {
        size_t at = 12 + static_cast<size_t>(index) * 12;
        uint32_t offset = ReadU32(textures + at);
        uint32_t size = ReadU32(textures + at + 4);
        uint32_t id = ReadU32(textures + at + 8);
        ItemStream item(textures + offset, size);
        gameTextures.push_back(TextureFromStream(id, &item.stream));
    }

    // Every material through ReadMaterial: its first textured shader's texture taken from the table
    const uint32_t materialCount = ReadU32(materials + 4);
    for (uint32_t index = 0; index < materialCount; index++)
    {
        size_t at = 12 + static_cast<size_t>(index) * 12;
        uint32_t offset = ReadU32(materials + at);
        uint32_t size = ReadU32(materials + at + 4);
        uint32_t id = ReadU32(materials + at + 8);
        ItemStream item(materials + offset, size);
        MaterialFromStream(id, &item.stream);
    }

    // The ReadTexture->SubmitQuad bind path: a material whose shader's texture the cache holds, bound as SubmitQuad
    // binds it
    bgfx::TextureHandle materialBound = BGFX_INVALID_HANDLE;
    for (uint32_t index = 0; index < materialCount && !bgfx::isValid(materialBound); index++)
    {
        size_t at = 12 + static_cast<size_t>(index) * 12;
        uint32_t id = ReadU32(materials + at + 8);
        MaterialResource* material = MaterialFromStream(id, nullptr);
        if (material == nullptr)
        {
            continue;
        }
        auto* host = reinterpret_cast<Platform::Graphics::HostMaterial*>(material->material);
        if (host != nullptr && host->gameTexture != nullptr)
        {
            materialBound = Platform::Graphics::HostMaterialBoundTexture(host);
            if (bgfx::isValid(materialBound))
            {
                std::printf("material %u: bind handle %u textured 1 (cache hit)\n", id, materialBound.idx);
            }
        }
    }

    // Texture 0 through ReadTexture: its cached handle and its decoded bytes' checksum (the R2 golden)
    bgfx::TextureHandle textureZero = BGFX_INVALID_HANDLE;
    if (!gameTextures.empty())
    {
        textureZero = Platform::Graphics::HostTextures().Find(gameTextures[0]);
    }

    const uint8_t* item = textures + ReadU32(textures + 12);
    const uint8_t* header = item + 4;
    Ps2TextureHeader decoded;
    decoded.widthPower = static_cast<u16>(header[4] | (header[5] << 8));
    decoded.heightPower = static_cast<u16>(header[6] | (header[7] << 8));
    decoded.levels = header[8];
    decoded.pixelFormat = header[9];
    decoded.uploadFormat = header[10];
    decoded.hasAlpha = header[11];
    decoded.function = header[13];
    for (int level = 0; level < 7; level++)
    {
        std::memcpy(&decoded.levelOffsets[level], header + 0x10 + level * 4, 4);
        std::memcpy(&decoded.levelWidths[level], header + 0x2C + level * 4, 4);
    }
    std::memcpy(&decoded.paletteOffset, header + 0x48, 4);
    std::printf("header: %ux%u levels=%u psm=%02x upload=%02x hasAlpha=%u fn=%u levelWidth0=%u paletteOffset=%u\n",
                1u << decoded.widthPower, 1u << decoded.heightPower, decoded.levels, decoded.pixelFormat,
                decoded.uploadFormat, decoded.hasAlpha, decoded.function, decoded.levelWidths[0],
                decoded.paletteOffset);
    // The packet the decoder wants: the item's bytes past the size field, the GS header and the tools' leftover
    // (the declared size counts the header and leftover, not its own four bytes)
    const uint8_t* transfer = item + 4 + 0x60 + 0x20;
    size_t transferSize = ReadU32(item) - (0x60 + 0x20);
    Ps2DecodedTexture golden;
    uint32_t checksum = 0;
    if (DecodePs2Texture(&decoded, transfer, transferSize, &golden))
    {
        checksum = Crc32(golden.rgba.data(), golden.rgba.size());
    }

    std::printf("texture 0: cached handle %u, decoded %ux%u checksum %08x (golden 050c99e0)\n",
                textureZero.idx, golden.width, golden.height, checksum);

    // The alpha bytes the decode produced: a GS palette's 0x80 is the GS's 1.0 (opaque), so a surface texture's
    // opaque texels read 0xFF once scaled (0x80 raw would be half transparency)
    if (golden.rgba.size() >= 4)
    {
        uint32_t opaque = 0;
        uint32_t half = 0;
        uint32_t total = static_cast<uint32_t>(golden.rgba.size() / 4);
        for (size_t pixel = 3; pixel < golden.rgba.size(); pixel += 4)
        {
            if (golden.rgba[pixel] == 0xFF)
            {
                opaque++;
            }
            else if (golden.rgba[pixel] == 0x80)
            {
                half++;
            }
        }
        std::printf("texture 0 alphas: %u/%u opaque (0xFF), %u at 0x80\n", opaque, total, half);

        // A PPM dump so the decode can be eyeballed (a scrambled swizzle or a wrong CLUT order is obvious by sight):
        // the caller passes an output path
        if (argc > 1)
        {
            FILE* out = std::fopen(argv[1], "wb");
            if (out != nullptr)
            {
                std::fprintf(out, "P6\n%u %u\n255\n", golden.width, golden.height);
                for (size_t pixel = 0; pixel < golden.rgba.size(); pixel += 4)
                {
                    std::fwrite(&golden.rgba[pixel], 1, 3, out);
                }
                std::fclose(out);
                std::printf("texture 0 written to %s\n", argv[1]);
            }
        }
    }

    bool ok = true;
    if (!bgfx::isValid(textureZero) || golden.width == 0)
    {
        std::fprintf(stderr, "ReadTexture did not cache texture 0\n");
        ok = false;
    }
    if (checksum != 0x050c99e0)
    {
        std::fprintf(stderr, "checksum MISMATCH\n");
        ok = false;
    }
    if (!bgfx::isValid(materialBound))
    {
        std::fprintf(stderr, "no material bound a cached texture\n");
        ok = false;
    }

    std::printf("texture-material-proof: %s (texture 0 handle %u, material bind handle %u, checksum %08x)\n",
                ok ? "PASS" : "FAIL", textureZero.idx, materialBound.idx, checksum);
    bgfx::shutdown();
    return ok ? 0 : 1;
}
