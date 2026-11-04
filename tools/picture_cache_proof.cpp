// Headless proof of a picture read whose texture is already cached: a real .psm (Language\Titles\English\Level05.psm) through
// the game's own Shape2D::Read, which takes the picture's texture and then its material out of ONE stream.
//
// Each resource is read only when the game's tables haven't got it (src/game/resources.cpp's TextureFromStream and
// MaterialFromStream), so a texture that is already there leaves the stream sitting on its own bytes -- and the material
// behind it is then parsed out of the texture's header. That parse comes out "successful": a material with no shader, no
// texture and the file's own bytes read as its bucket (0x70007), which is cached under the picture's material id. The sprite
// that drew it shows the 1x1 white stand-in for the rest of the session -- the port's white level-title card on the pause
// screen after a chunk transition, until the game reads the picture again (a respawn or another area change). The texture's
// bytes are stepped over now, so the material always parses from its own start.
//
// bgfx runs headless on the Noop renderer: no window, no GPU; the tables, the stream, the decode and the bind run for real.
// Build: ninja picture-cache-proof && ./build/host/picture_cache_proof  (from the repo root)
#include "../src/platform/sdl3/graphics/host_graphics.h"
#include "../src/platform/sdl3/graphics/texture_decoder.h"

#include "gcc2.h"
#include "game/graphicstables.h"
#include "game/shapes.h"
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

// The BH table (src/game/archive.cpp ArchiveSectionReader::Read)
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
}

int main()
{
    const std::vector<uint8_t> bh = ReadFile("disc/CRASH6/CRASH.BH");
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

    // The level title card OLEG reads (OLEG::LoadPicture's path: PictureReader -> OLEG::ReadPicture -> Sprite::Read)
    const BhEntry* picture = nullptr;
    for (const BhEntry& entry : entries)
    {
        if (entry.path == "Language\\Titles\\English\\Level05.psm")
        {
            picture = &entry;
        }
    }
    if (picture == nullptr || picture->start + picture->size > bd.size())
    {
        std::fprintf(stderr, "Level05.psm not found\n");
        return 1;
    }

    // The game's static initializers (the graphics tables among them), then the headless bgfx the uploads need
    RunStaticConstructors();
    bgfx::Init init;
    init.type = bgfx::RendererType::Noop;
    if (!bgfx::init(init))
    {
        std::fprintf(stderr, "bgfx Noop init failed\n");
        return 1;
    }

    uint8_t* const file = bd.data() + picture->start;
    const uint32_t textureId = ReadU32(file);

    // The picture read once, as the game reads it: its texture and its material out of the one stream
    Sprite first;
    Sprite::Construct(&first);
    MemoryStream firstStream;
    MemoryStream::Construct(&firstStream, file, picture->size, 0, MemoryStream::FileAlignment);
    first.Read(&firstStream);

    auto* firstHost = reinterpret_cast<Platform::Graphics::HostMaterial*>(first.material);
    if (firstHost == nullptr || firstHost->gameTexture == nullptr)
    {
        std::fprintf(stderr, "FAIL: the picture's first read produced no texture\n");
        return 1;
    }
    std::printf("picture read once:                 bucket %u, game texture %p, shaders %u\n", firstHost->bucket,
                static_cast<void*>(firstHost->gameTexture), firstHost->shaderSettings[0]);
    if (firstHost->bucket > 26)
    {
        std::fprintf(stderr, "FAIL: even the first read parsed a bucket past the game's buckets (%u)\n", firstHost->bucket);
        return 1;
    }

    // Its texture held in the table while its material is let go -- the state a picture is in when its material was released
    // and something still holds its texture, which is when the next read meets a cached texture
    uint32_t heldTextureId = textureId;
    g_TextureTable.Acquire(&heldTextureId, nullptr);
    ReleaseMaterial(first.resource);
    first.resource = nullptr;

    // The picture read again: its material must parse from its own start, not from inside the texture's bytes
    Sprite again;
    Sprite::Construct(&again);
    MemoryStream secondStream;
    MemoryStream::Construct(&secondStream, file, picture->size, 0, MemoryStream::FileAlignment);
    again.Read(&secondStream);

    auto* host = reinterpret_cast<Platform::Graphics::HostMaterial*>(again.material);
    bgfx::TextureHandle bound = Platform::Graphics::HostMaterialBoundTexture(host);
    const bgfx::TextureHandle white = Platform::Graphics::HostWhiteTexture();
    std::printf("picture read with texture cached: bucket %u, game texture %p, binds upload %u (the white stand-in is %u)\n",
                host != nullptr ? host->bucket : 0u, host != nullptr ? static_cast<void*>(host->gameTexture) : nullptr,
                bound.idx, white.idx);

    if (host == nullptr)
    {
        std::fprintf(stderr, "FAIL: the picture's second read produced no material\n");
        return 1;
    }
    if (host->bucket > 26)
    {
        std::fprintf(stderr, "FAIL: the material parsed out of its texture's bytes (bucket %u) -- the card draws white\n",
                     host->bucket);
        return 1;
    }
    if (host->gameTexture == nullptr || !bgfx::isValid(bound) || bound.idx == white.idx)
    {
        std::fprintf(stderr, "FAIL: the material has no texture of its own to draw with -- the card draws white\n");
        return 1;
    }

    std::printf("PASS: the picture's material parses from its own start with its texture already cached (bucket %u)\n",
                host->bucket);

    // The card drawn through the chunk wipes a transition runs: the game frees every material and texture in its tables on
    // each unload (ReleaseGraphicsResources), while the sprite keeps the material it read. The draw still resolves that
    // material's own upload -- the console's freed-but-intact bytes and its texture memory -- instead of whatever material
    // lands in the recycled block (a rock's texture, or none: the white card)
    const bgfx::TextureHandle card = bound;
    for (u32 wipe = 0; wipe < 40; wipe++)
    {
        ReleaseGraphicsResources(nullptr);

        // The HUD draws the card every frame, which is what marks the material as still in use
        bgfx::TextureHandle drawn = Platform::Graphics::HostMaterialBoundTexture(host);
        if (!bgfx::isValid(drawn) || drawn.idx != card.idx)
        {
            std::fprintf(stderr, "FAIL: chunk wipe %u lost the card's texture (%u -> %u)\n", wipe, card.idx, drawn.idx);
            return 1;
        }

        bgfx::frame();
    }

    std::printf("PASS: the card still binds its own picture through 40 chunk wipes\n");
    return 0;
}
