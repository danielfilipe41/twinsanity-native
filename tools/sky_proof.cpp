// The sky headless proof: drives Platform::Graphics::DrawChunkSky over real beach geometry and checksums the
// half-size buffer it renders into and the copy it makes over the screen.
//
// No sky-dome packet is on this machine: the retail skies come from the disc's "SkyDomes\<id>" files (the ELF's
// SkyMade builds that path; they are not in CRASH.BH/BD, and every RM2's sky subsection, graphics kind 8, is
// empty), so a beach.rm2 rigid model packet stands in for a sky mesh. The geometry, the packet parser and the
// whole render path are real; only the sky's model list is synthetic.
//
// Build (from the repo root). The flags are configure.py's HOST_CXXFLAGS plus SDL3 and bgfx; the sources are the
// graphics objects sky.cpp pulls in (sky, the shared 3D submit, the 2D copy program, its material/texture
// objects, the mesh parser, the bgfx context and the host's CPU matrix helpers):
//   g++ -m32 -malign-double -msse2 -mfpmath=sse -O2 -std=gnu++26 -fno-pie -fno-exceptions -fno-rtti
//       -fno-threadsafe-statics -fno-asynchronous-unwind-tables -fno-common -fno-strict-aliasing
//       -ffunction-sections -ffp-contract=off -fno-delete-null-pointer-checks -Wall -Wno-invalid-offsetof
//       -Iinclude -Iinclude/host -I/usr/include/SDL3 -Ithird_party/bgfx/include -Ithird_party/bx/include
//       -Ithird_party/bimg/include -Ithird_party/SDL/include
//       tools/sky_proof.cpp src/platform/sdl3/graphics/sky.cpp src/platform/sdl3/graphics/draws_host.cpp
//       src/platform/sdl3/graphics/draw2d.cpp src/platform/sdl3/graphics/material_system.cpp
//       src/platform/sdl3/graphics/texture_decoder.cpp src/platform/sdl3/graphics/mesh_builder.cpp
//       src/platform/sdl3/graphics/bgfx_context.cpp src/platform/sdl3/graphics/graphics.cpp
//       src/platform/null/vumath.cpp
//       -m32 -no-pie -Lthird_party/bgfx/.build/linux32_gcc/bin -lbgfxRelease -lbimgRelease
//       -lbimg_decodeRelease -lbxRelease -lX11 -lGL -lpthread -ldl -lrt -lSDL3 -o /tmp/sky_proof
// Run from the repo root: /tmp/sky_proof
#include "../src/platform/sdl3/graphics/bgfx_context.h"
#include "../src/platform/sdl3/graphics/host_graphics.h"

#include "game/controllers.h"
#include "game/math.h"
#include "game/resources.h"
#include "game/view.h"
#include "../src/platform/sdl3/graphics/draws_host.h"

#include <bgfx/bgfx.h>
#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// game/view.h declares the retail globals with their RETAIL asm names (g_RenderTarget is G_FontRendererRel);
// sky.cpp reads the render target through it. The proof stands the singleton in
RenderTargetDescription* G_FontRendererRel = nullptr;

namespace
{
// ---- the host's Sky/RigidModel/RigidModelData (sky.cpp's HostSky; the others are draws_host.h's) ----
struct HostSky
{
    ResourceHeader header;
    u32 count;
    Platform::Graphics::HostRigidModel** models;
    Matrix4x4 toScreen;
};

constexpr u32 ScreenWidth = 960;
constexpr u32 ScreenHeight = 540;

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
    std::vector<uint8_t> bytes(size > 0 ? static_cast<size_t>(size) : 0);
    if (size > 0 && std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size())
    {
        bytes.clear();
    }
    std::fclose(file);
    return bytes;
}

// The BH table (src/game/archive.cpp ArchiveSectionReader::Read): s32 header, then per file s32 path length,
// path bytes, s32 BD start, s32 size
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

// The proof's copy-target readback: the full-size screen's colour texture blitted to a CPU-readable texture
bgfx::TextureHandle g_screenColour = BGFX_INVALID_HANDLE;
bgfx::TextureHandle g_screenReadback = BGFX_INVALID_HANDLE;
constexpr bgfx::ViewId ProofReadbackView = 31;

bool ReadScreen(u32 width, u32 height, u8* out)
{
    if (!bgfx::isValid(g_screenColour) || !bgfx::isValid(g_screenReadback) || out == nullptr)
    {
        return false;
    }
    bgfx::setViewRect(ProofReadbackView, 0, 0, static_cast<u16>(width), static_cast<u16>(height));
    bgfx::blit(ProofReadbackView, {.handle = g_screenReadback}, {.handle = g_screenColour});
    u32 ready = bgfx::read({.handle = g_screenReadback}, out);
    for (u32 spin = 0; spin < 60; spin++)
    {
        if (bgfx::frame() == ready)
        {
            return true;
        }
    }
    return false;
}
}

int main(int argc, char** argv)
{
    const char* rm2Path = argc > 1 ? argv[1] : "Levels\\Earth\\Hub\\beach.rm2";
    std::vector<uint8_t> bh = ReadFile("disc/CRASH6/CRASH.BH");
    std::vector<uint8_t> bd = ReadFile("disc/CRASH6/CRASH.BD");
    if (bh.empty() || bd.empty())
    {
        std::fprintf(stderr, "sky-proof: disc/CRASH6/CRASH.B{H,D} missing\n");
        return 1;
    }
    std::vector<BhEntry> entries;
    if (!ParseBh(bh, &entries))
    {
        std::fprintf(stderr, "sky-proof: BH parse failed\n");
        return 1;
    }
    const BhEntry* rm2 = nullptr;
    for (const BhEntry& entry : entries)
    {
        if (entry.path == rm2Path)
        {
            rm2 = &entry;
        }
    }
    if (rm2 == nullptr || rm2->start + rm2->size > bd.size())
    {
        std::fprintf(stderr, "sky-proof: %s not found\n", rm2Path);
        return 1;
    }
    const uint8_t* file = bd.data() + rm2->start;
    uint32_t graphicsOffset = 0;
    uint32_t graphicsSize = 0;
    if (!FindSection(file, rm2->size, 11, &graphicsOffset, &graphicsSize))
    {
        std::fprintf(stderr, "sky-proof: graphics section not found\n");
        return 1;
    }
    const uint8_t* graphics = file + graphicsOffset;
    uint32_t modelsOffset = 0;
    uint32_t modelsSize = 0;
    if (!FindItem(graphics, graphicsSize, 2, &modelsOffset, &modelsSize))
    {
        std::fprintf(stderr, "sky-proof: models subsection not found\n");
        return 1;
    }
    const uint8_t* models = graphics + modelsOffset;
    if (ReadU32(models + 4) == 0)
    {
        std::fprintf(stderr, "sky-proof: no model items\n");
        return 1;
    }

    // Item 0's submodel 0 (resources.cpp ModelRead: s32 subModelCount, then per submodel vertexCount, packet
    // size, packet, extra size; submodel 0's are the fixed offsets mesh_golden reads)
    const uint8_t* item = models + ReadU32(models + 12);
    if (ReadU32(item) == 0)
    {
        std::fprintf(stderr, "sky-proof: item 0 has no submodels\n");
        return 1;
    }
    std::vector<const uint8_t*> packets = {item + 12};
    std::vector<uint32_t> vertexCounts = {ReadU32(item + 4)};
    std::vector<uint32_t> packetSizes = {ReadU32(item + 8)};
    std::vector<uint32_t> extraSizes = {0};
    if (packets.empty())
    {
        std::fprintf(stderr, "sky-proof: no submodel packets\n");
        return 1;
    }
    std::printf("sky-proof: %s graphics+%u, %zu submodels (synthetic sky models from real packets)\n", rm2Path,
                graphicsOffset, packets.size());

    // The sky's models: one HostRigidModel whose data points at the packets just collected (one per item)
    auto* modelData = new Platform::Graphics::HostRigidModelData();
    modelData->subModelCount = static_cast<u32>(packets.size());
    modelData->vertexCounts = vertexCounts.data();
    modelData->sizes = packetSizes.data();
    modelData->extraSizes = extraSizes.data();
    modelData->packets = packets.data();
    auto* model = new Platform::Graphics::HostRigidModel();
    std::memset(model, 0, sizeof(*model));
    model->model = modelData;
    model->count = static_cast<u32>(packets.size());
    Platform::Graphics::HostRigidModel* modelList[1] = {model};
    HostSky sky = {};
    sky.count = 1;
    sky.models = modelList;

    // The render target the sky sizes its buffer from and the view it projects with: the frame's size, and an
    // orthographic clip matrix covering the first packet's bounding box (mesh_golden's report)
    RenderTargetDescription target = {};
    target.width = ScreenWidth;
    target.height = ScreenHeight;
    target.displayWidth = ScreenWidth;
    target.displayHeight = ScreenHeight;
    target.offsetX = 0;
    target.offsetY = 0;
    g_RenderTarget = &target;
    RenderView view = {};
    InitIdentityMatrix(&view.toClip);
    view.toClip.m[0][0] = 2.0f / 4.3180f;
    view.toClip.m[1][1] = 2.0f / 2.2788f;
    view.toClip.m[2][2] = 0.0f;
    view.toClip.m[3][0] = 0.0f;
    view.toClip.m[3][1] = 0.0f;
    view.toClip.m[3][2] = 0.5f;
    view.cameraObject = nullptr;

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::fprintf(stderr, "sky-proof: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("Crash Twinsanity (sky proof)", ScreenWidth, ScreenHeight,
                                          SDL_WINDOW_HIDDEN);
    if (window == nullptr || !BgfxInit(window))
    {
        std::fprintf(stderr, "sky-proof: window/bgfx init failed: %s\n", SDL_GetError());
        return 1;
    }

    // The copy's target: a full-size offscreen "screen" the proof reads back (a real screen is the back buffer;
    // this stands in so the copy can be checksummed)
    g_screenColour = bgfx::createTexture2D(ScreenWidth, ScreenHeight, false, 1, bgfx::TextureFormat::RGBA8,
                                           BGFX_TEXTURE_RT | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
                                               BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    bgfx::TextureHandle screenDepth = bgfx::createTexture2D(ScreenWidth, ScreenHeight, false, 1,
                                                            bgfx::TextureFormat::D16, BGFX_TEXTURE_RT_WRITE_ONLY);
    bgfx::TextureHandle screenAttachments[2] = {g_screenColour, screenDepth};
    bgfx::FrameBufferHandle screenBuffer = bgfx::createFrameBuffer(2, screenAttachments, false);
    g_screenReadback = bgfx::createTexture2D(ScreenWidth, ScreenHeight, false, 1, bgfx::TextureFormat::RGBA8,
                                             BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK |
                                                 BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
                                                 BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    if (!bgfx::isValid(screenBuffer) || !bgfx::isValid(g_screenReadback))
    {
        std::fprintf(stderr, "sky-proof: offscreen screen creation failed\n");
        return 1;
    }
    Platform::Graphics::HostSkySetCopyTarget(screenBuffer);

    // A few frames (the first may race the buffer's creation; the last is what is read back)
    for (u32 frame = 0; frame < 3; frame++)
    {
        Platform::Graphics::DrawChunkSky(reinterpret_cast<Sky*>(&sky), &view);
        BgfxFrame();
    }

    const u32 halfWidth = ScreenWidth / 2;
    const u32 halfHeight = ScreenHeight / 2;
    std::vector<u8> buffer(static_cast<size_t>(halfWidth) * halfHeight * 4);
    std::vector<u8> screen(static_cast<size_t>(ScreenWidth) * ScreenHeight * 4);
    if (!Platform::Graphics::HostSkyReadBuffer(halfWidth, halfHeight, buffer.data()))
    {
        std::fprintf(stderr, "sky-proof: half-size buffer readback failed\n");
        return 1;
    }
    if (!ReadScreen(ScreenWidth, ScreenHeight, screen.data()))
    {
        std::fprintf(stderr, "sky-proof: screen readback failed\n");
        return 1;
    }

    // The sky's own pixels: how many of the half-size buffer's are not the clear (the geometry covered something)
    u32 covered = 0;
    for (u32 texel = 0; texel < halfWidth * halfHeight; texel++)
    {
        const u8* pixel = buffer.data() + texel * 4;
        if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0 || pixel[3] != 255)
        {
            covered++;
        }
    }
    // The copy is the buffer magnified two pixels for one (point sampling): screen pixel (2x, 2y) is buffer texel
    // (x, y). Any mismatch means the copy's geometry or sampling is wrong
    u32 mismatches = 0;
    for (u32 y = 0; y < halfHeight; y++)
    {
        for (u32 x = 0; x < halfWidth; x++)
        {
            const u8* texel = buffer.data() + (static_cast<size_t>(y) * halfWidth + x) * 4;
            const u8* pixel = screen.data() + (static_cast<size_t>(y * 2) * ScreenWidth + x * 2) * 4;
            if (texel[0] != pixel[0] || texel[1] != pixel[1] || texel[2] != pixel[2] || texel[3] != pixel[3])
            {
                mismatches++;
            }
        }
    }
    u32 bufferChecksum = Crc32(buffer.data(), buffer.size());
    u32 screenChecksum = Crc32(screen.data(), screen.size());
    std::printf("half-size buffer %ux%u: covered %u/%u pixels, checksum %08x\n", halfWidth, halfHeight, covered,
                halfWidth * halfHeight, bufferChecksum);
    std::printf("screen %ux%u: checksum %08x, copy mismatches %u/%u\n", ScreenWidth, ScreenHeight, screenChecksum,
                mismatches, halfWidth * halfHeight);
    std::printf("sky's toScreen row 3: %f %f %f (half-size buffer's middle at 2048)\n", sky.toScreen.m[3][0],
                sky.toScreen.m[3][1], sky.toScreen.m[3][2]);

    int status = 0;
    if (covered == 0)
    {
        std::fprintf(stderr, "sky-proof: nothing rendered into the half-size buffer\n");
        status = 1;
    }
    if (mismatches != 0)
    {
        std::fprintf(stderr, "sky-proof: the copy over the screen does not match the half-size buffer\n");
        status = 1;
    }
    if (status == 0)
    {
        std::printf("sky-proof: PASS (buffer %08x, screen %08x)\n", bufferChecksum, screenChecksum);
    }
    bgfx::destroy(screenBuffer);
    delete model;
    delete modelData;
    BgfxShutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return status;
}
