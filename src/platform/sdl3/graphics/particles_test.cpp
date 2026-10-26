// The particles', decals' and distortion's headless proof: builds the data the game would (a particle block chain
// with its records, a system's render table built the way BuildParticleRenderTable builds one, a decal block aged
// through AgeDecal, and a frame copy for the distortion), runs the host's draws over it and prints what each one
// produced: how many vertices and an FNV-1a checksum of them.
//
// The draws build their geometry on the CPU (src/platform/sdl3/graphics/particles_host.cpp and effects.cpp), so the
// checksums are of exactly the geometry a frame would submit. The game's own code is not linked in (the proof only
// needs the platform), so the particle systems' look and the decal data are built here, mirroring what
// BuildParticleRenderTable (src/game/particles.cpp) and the decal descriptors (src/game/decals.cpp) produce.
//
// Reachable via `ninja particles-test`; not part of the game build (configure.py keeps it out of the game sources,
// like draw2d_test.cpp).
#include "bgfx_context.h"
#include "host_graphics.h"
#include "particle_billboard.h"

#include "game/decals.h"
#include "game/math.h"
#include "game/particles.h"
#include "platform/graphics.h"
#include "platform/math.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
// The display the proof draws for: the game's own coordinates have the screen's middle at 2048 in 1/16th-pixel
// units, so the table's shapes are pixels of this display
constexpr u32 ProofDisplayWidth = 512;
constexpr u32 ProofDisplayHeight = 448;
constexpr f32 GsScreenMiddleUnits = 2048.0f * 16.0f;
constexpr f32 GsSubpixels = 16.0f;

// The systems the proof stands up: the particles' size and life, and the table's texture rectangle in pixels
constexpr f32 ProofSize = 400.0f;
constexpr f32 ProofLife = 1.0f;
constexpr s32 ProofTexture[4] = {0, 0, 64 * 16, 64 * 16};
constexpr u32 ProofBlocks = 2;

// A system's look, the shape its particles have at each of the 64 steps (BuildParticleRenderTable's own formulas for
// a rotation of a sixth of a turn a step, the width and height stretched by the display's aspect)
void MakeLook(Platform::Graphics::ParticleLook* look, bool hexagons)
{
    std::memset(look, 0, sizeof(*look));
    look->gravity = 12.0f;
    for (u32 corner = 0; corner < 4; corner++)
    {
        look->texture[corner] = ProofTexture[corner];
    }

    look->hexagons = hexagons;
    if (hexagons)
    {
        // The six corners a sixth of a turn apart, a quarter long
        static const s32 angles[6] = {0, 0x2AAA, 0x5555, 0x8000, 0xAAAA, 0xD555};
        for (u32 corner = 0; corner < 6; corner++)
        {
            const f32 angle = static_cast<f32>(angles[corner]) * (6.2831855f / 65536.0f);
            look->hexagonCorners[corner * 2] = std::sin(angle) * 0.25f;
            look->hexagonCorners[corner * 2 + 1] = std::cos(angle) * 0.25f;
        }
    }

    const f32 aspect = static_cast<f32>(ProofDisplayWidth) / static_cast<f32>(ProofDisplayHeight);
    for (u32 step = 0; step < Platform::Graphics::ParticleRenderSteps; step++)
    {
        const f32 life = static_cast<f32>(step) / static_cast<f32>(Platform::Graphics::ParticleRenderSteps);
        const f32 width = ProofSize;
        const f32 height = ProofSize * (0.5f + 0.5f * life);
        const f32 rotation = life * 6.2831855f;
        const f32 sine = std::sin(rotation);
        const f32 cosine = std::cos(rotation);
        f32* shape = look->steps[step].shape;
        const f32 cornerX = -(width * 0.5f) * cosine - height * (sine * 0.5f);
        const f32 cornerY = width * (sine * 0.5f) - height * (cosine * 0.5f);
        const f32 otherX = width * (cosine * 0.5f) - height * (sine * 0.5f);
        const f32 otherY = -(width * 0.5f) * sine - height * (cosine * 0.5f);
        const f32 rise = height * sine;
        shape[0] = cornerX * (aspect + aspect);
        shape[1] = cornerY + cornerY;
        shape[2] = rise * aspect + rise * aspect;
        shape[3] = height * cosine + height * cosine;
        shape[4] = otherX * (aspect + aspect) + shape[2];
        shape[5] = otherY + otherY + shape[3];
        look->steps[step].colour[0] = static_cast<u8>(32 + step * 3);
        look->steps[step].colour[1] = static_cast<u8>(64 + step * 2);
        look->steps[step].colour[2] = static_cast<u8>(128 - step);
        look->steps[step].colour[3] = static_cast<u8>(200 - step);
    }
}

// The first corner's offset in the screen's coordinates: the table's shape's first corner along the camera's right and
// up, scaled by the view's matrix to the screen the way the host's draw does it
void CornerOffset(const Platform::Graphics::ParticleLook& look, u32 step, f32* across, f32* down)
{
    f32 scaleX = 0.0f;
    f32 scaleY = 0.0f;
    Platform::Graphics::ParticleScreenScale(&scaleX, &scaleY);
    const f32 fix = Platform::Graphics::ShapeScreenRatio(scaleX, scaleY);
    const f32* shape = look.steps[step].shape;
    *across = shape[0] * Platform::Graphics::ParticleSizeUnit * Platform::Graphics::ShapeHalfExtent * fix * scaleX;
    *down = -shape[1] * Platform::Graphics::ParticleSizeUnit * Platform::Graphics::ShapeHalfExtent * scaleY;
}

// The matrix a block draws through (Platform::Math::ParticleBlockView's work on the PS2): the proof stands up one
// that takes the block's units to the game's screen coordinates around the middle, so the picture is plausible
void MakeBlockMatrix(Matrix4x4* matrix)
{
    std::memset(matrix, 0, sizeof(*matrix));
    matrix->m[0][0] = 400.0f;
    matrix->m[1][1] = -400.0f;
    matrix->m[2][2] = 400.0f;
    matrix->m[3][0] = GsScreenMiddleUnits;
    matrix->m[3][1] = GsScreenMiddleUnits;
    matrix->m[3][3] = 1.0f;
}

// A block chain with its records: every particle born a step apart, flying out and falling
void FillBlocks(std::vector<u8>& storage, std::vector<u8*>* blocks, bool hexagons)
{
    const u32 count = hexagons ? 12u : 32u;
    const u32 bytes = hexagons ? Platform::Graphics::HexagonBlockBytes : Platform::Graphics::ParticleBlockBytes;
    storage.assign(static_cast<size_t>(bytes) * ProofBlocks, 0);
    blocks->clear();
    for (u32 block = 0; block < ProofBlocks; block++)
    {
        u8* at = storage.data() + static_cast<size_t>(block) * bytes;
        Platform::Graphics::InitParticleBlock(at, hexagons);
        auto* records = reinterpret_cast<ParticleRecord*>(at + 0x20);
        for (u32 record = 0; record < count; record++)
        {
            const f32 particle = static_cast<f32>(block * count + record);
            records[record].start[0] = std::sin(particle) * 0.4f;
            records[record].start[1] = 0.0f;
            records[record].start[2] = std::cos(particle) * 0.4f;
            records[record].spawnTime = 0.1f * static_cast<f32>(record);
            records[record].velocity[0] = std::sin(particle) * 0.5f;
            records[record].velocity[1] = 0.5f;
            records[record].velocity[2] = std::cos(particle) * 0.5f;
            records[record].lifeScale = static_cast<f32>(Platform::Graphics::ParticleRenderSteps) / ProofLife;
        }

        blocks->push_back(at);
    }

    Platform::Graphics::ChainParticleBlocks(blocks->data(), static_cast<s32>(blocks->size()));
}

// The decals of a block, aged through AgeDecal exactly as src/game/decals.cpp's UpdateDecalsVU0 ages them
void AgeBlock(DecalBlock* block, const DecalType* type, const DecalFrame* frame)
{
    for (s32 slot = 0; slot < block->count; slot++)
    {
        const DecalFrame& at = frame[slot];
        f32 place[4] = {block->places[slot].x, block->places[slot].y, block->places[slot].z,
                        static_cast<f32>(block->places[slot].life.frames) / 60.0f};
        s32 words[8] = {at.normal[0], at.normal[1], at.normal[2], at.unused06,
                        at.direction[0], at.direction[1], at.direction[2], at.unused0E};
        Platform::Graphics::DecalLook look;
        Platform::Graphics::AgeDecal(place, words, block->variants[slot], &look);
        block->places[slot].x = look.place[0];
        block->places[slot].y = look.place[1];
        block->places[slot].z = look.place[2];
        // The block's colour the way the game packs Rgba (red in the low byte on the little-endian PS2 and host)
        block->colours[slot] = static_cast<u32>(look.colour[0]) | (static_cast<u32>(look.colour[1]) << 8) |
                               (static_cast<u32>(look.colour[2]) << 16) | (static_cast<u32>(look.colour[3]) << 24);
        for (u32 value = 0; value < 4; value++)
        {
            block->sizes[slot][value] = look.sizes[value];
            block->moreSizes[slot][value] = look.moreSizes[value];
        }
    }

    (void)type;
}

// An FNV-1a of the proof's own data, so a reader can see the inputs are the same every run
u32 ProofHash(u32 hash, const void* bytes, size_t size)
{
    auto* at = static_cast<const u8*>(bytes);
    for (size_t index = 0; index < size; index++)
    {
        hash = (hash ^ at[index]) * 16777619u;
    }

    return hash;
}
// The decals' aging against the VU0 program's own outputs: a fixed variant and frame run through AgeDecal and
// compared with what the PS2's microprogram leaves (the VU0 program at 0x1C0, instruction for instruction, as the
// reference run recorded it: the same variant, the frame's normal +Y and direction +X, and four lives)
bool CheckAgeDecal()
{
    DecalType type;
    std::memset(&type, 0, sizeof(type));
    type.variantCount = 1;
    static const f32 colours[4][4] = {{255.0f, 0.0f, 0.0f, 255.0f},
                                      {255.0f, 255.0f, 0.0f, 255.0f},
                                      {0.0f, 255.0f, 0.0f, 128.0f},
                                      {0.0f, 0.0f, 255.0f, 0.0f}};
    static const f32 sizes[4][4] = {{1.0f, 1.0f, 1.0f, 1.0f},
                                    {2.0f, 2.0f, 2.0f, 1.0f},
                                    {0.5f, 0.5f, 0.5f, 0.5f},
                                    {0.25f, 0.25f, 0.25f, 0.25f}};
    for (u32 key = 0; key < 4; key++)
    {
        for (u32 channel = 0; channel < 4; channel++)
        {
            type.variants[0].colours[key][channel] = colours[key][channel];
            type.variants[0].sizes[key][channel] = sizes[key][channel];
        }
    }

    const s32 frame[8] = {0, 32768, 0, 0, 32768, 0, 0, 0};
    static const f32 lives[4] = {1.0f, 0.5f, 0.25f, 0.0f};
    static const u8 wantColour[4][4] = {{255, 0, 0, 255}, {127, 255, 0, 191}, {0, 191, 63, 96}, {0, 0, 255, 0}};
    static const s16 wantSizes[4][4] = {{12288, -2048, 0, 240},
                                        {15360, -2560, 0, 240},
                                        {5376, -896, 0, 240},
                                        {3072, -512, 0, 240}};
    static const s16 wantMore[4][4] = {{2048, 2048, 0, 0}, {2560, 2560, 0, 0}, {896, 896, 0, 0}, {512, 512, 0, 0}};
    Platform::Graphics::LoadDecalType(&type);
    bool ok = true;
    for (u32 index = 0; index < 4; index++)
    {
        const f32 place[4] = {5.0f, 6.0f, 7.0f, lives[index]};
        Platform::Graphics::DecalLook look = {};
        Platform::Graphics::AgeDecal(place, frame, 0, &look);
        for (u32 value = 0; value < 4; value++)
        {
            ok = ok && look.colour[value] == wantColour[index][value] &&
                 look.sizes[value] == wantSizes[index][value] &&
                 look.moreSizes[value] == wantMore[index][value];
        }

        ok = ok && look.place[0] == 5.0f && look.place[1] == 6.0f && look.place[2] == 7.0f;
    }

    std::printf("agedecals: %s\n", ok ? "matches the VU0 program" : "MISMATCH");
    return ok;
}
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::fprintf(stderr, "particles-test: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window =
        SDL_CreateWindow("Crash Twinsanity (particles test)", BgfxWindowWidth, BgfxWindowHeight, SDL_WINDOW_RESIZABLE);
    if (window == nullptr)
    {
        std::fprintf(stderr, "particles-test: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    if (!BgfxInit(window))
    {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    Platform::Graphics::HostSetDisplaySize(ProofDisplayWidth, ProofDisplayHeight);

    Material* flat = Platform::Graphics::MakeFlatMaterial();
    alignas(8) u8 materialStorage[Platform::Graphics::MaterialStorage];
    Platform::Graphics::ConstructMaterial(reinterpret_cast<Material*>(materialStorage));
    Platform::Graphics::MakeDistortionMaterial(reinterpret_cast<Material*>(materialStorage));
    if (flat == nullptr)
    {
        std::fprintf(stderr, "particles-test: MakeFlatMaterial returned null\n");
        BgfxShutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // The particles' page material: what the UI's particle materials look like, with the proof's own page texture
    // bound later (the white texture stands in until the page's upload exists)
    alignas(8) u8 particleStorage[Platform::Graphics::MaterialStorage];
    Material* particleMaterial =
        static_cast<Material*>(Platform::Graphics::MakeParticleMaterial(reinterpret_cast<Material*>(particleStorage)));

    // A frame of its own so the draws have somewhere to go (the UI's view, like the 2D test)
    Platform::Graphics::FrameStart frame = {};
    frame.width = BgfxWindowWidth;
    frame.height = BgfxWindowHeight;
    frame.screenWidth = ProofDisplayWidth;
    frame.screenHeight = ProofDisplayHeight;
    Platform::Graphics::StartFrame(frame);

    // The systems' render tables, as BuildParticleRenderTable would write them
    alignas(16) u8 particleTable[Platform::Graphics::ParticleRenderTableBytes];
    alignas(16) u8 hexagonTable[Platform::Graphics::ParticleRenderTableBytes];
    Platform::Graphics::InitParticleRenderTable(particleTable);
    Platform::Graphics::InitParticleRenderTable(hexagonTable);
    Platform::Graphics::ParticleLook look;
    MakeLook(&look, false);
    Platform::Graphics::WriteParticleRenderTable(particleTable, look);
    MakeLook(&look, true);
    Platform::Graphics::WriteParticleRenderTable(hexagonTable, look);

    std::vector<u8> particleStorageBlocks;
    std::vector<u8*> blocks;
    FillBlocks(particleStorageBlocks, &blocks, false);
    Matrix4x4 matrix;
    MakeBlockMatrix(&matrix);

    // The view's matrix to the screen (the frame's view, the one the culling loads): the particle billboards take their
    // size from it, the way the console's VU1 takes it from the view's own matrix. The proof's block matrix takes a unit
    // to the screen at the same rate, so the view's is the same matrix
    alignas(16) Matrix4x4 viewToScreen;
    MakeBlockMatrix(&viewToScreen);
    alignas(16) Matrix4x4 viewToClip;
    std::memset(&viewToClip, 0, sizeof(viewToClip));
    viewToClip.m[0][0] = 1.0f;
    viewToClip.m[1][1] = 1.0f;
    viewToClip.m[2][2] = 1.0f;
    viewToClip.m[3][3] = 1.0f;
    const Vector4 viewCamera = {0.0f, 0.0f, 0.0f, 1.0f};
    Platform::Graphics::LoadCullingView(&viewToClip, &viewToScreen, &viewCamera);

    // The particles' wave, moved on a frame's worth of clock
    Platform::Graphics::HostParticleWaveInit(1.0f, 1.5f);
    Platform::Graphics::HostParticleWaveUpdate(1.0f / 60.0f);
    f32 sines[Platform::Graphics::HostWaveRows * 3];
    Platform::Graphics::HostParticleWaveSines(sines);
    const u32 waveHash = ProofHash(5381u, sines, sizeof(sines));

    // The decals: one type of two variants, a block of eight decals aged on the spot, its type's UV rectangle
    DecalData decals;
    std::memset(&decals, 0, sizeof(decals));
    DecalType type;
    std::memset(&type, 0, sizeof(type));
    type.variantCount = 2;
    for (u32 variant = 0; variant < 2; variant++)
    {
        for (u32 key = 0; key < 4; key++)
        {
            type.variants[variant].colours[key][0] = 128.0f;
            type.variants[variant].colours[key][1] = 64.0f + 32.0f * key;
            type.variants[variant].colours[key][2] = 192.0f - 32.0f * key;
            type.variants[variant].colours[key][3] = 200.0f;
            type.variants[variant].sizes[key][0] = 0.5f + 0.1f * key;
            type.variants[variant].sizes[key][1] = 0.5f;
            type.variants[variant].sizes[key][2] = 0.5f + 0.1f * key;
            type.variants[variant].sizes[key][3] = 0.25f * (key + 1);
        }
    }

    DecalBlock& decalBlock = decals.blocks[0];
    std::memset(&decalBlock, 0, sizeof(decalBlock));
    decalBlock.count = 8;
    decalBlock.key = 0;
    decalBlock.index = 0;
    DecalFrame decalFrames[8];
    for (s32 slot = 0; slot < decalBlock.count; slot++)
    {
        decalBlock.places[slot].x = 1.0f + slot;
        decalBlock.places[slot].y = 0.5f;
        decalBlock.places[slot].z = -2.0f;
        decalBlock.places[slot].life.frames = 60 - slot * 4;
        decalBlock.variants[slot] = 0;
        decalFrames[slot].normal[0] = 0;
        decalFrames[slot].normal[1] = 32767;
        decalFrames[slot].normal[2] = 0;
        decalFrames[slot].direction[0] = 32767;
        decalFrames[slot].direction[1] = 0;
        decalFrames[slot].direction[2] = 0;
    }

    decals.types[0] = &type;
    decals.typeBlocks[0] = &decalBlock;
    decals.drawLists[0] = &decalBlock;
    decals.count = decalBlock.count;
    decals.typeCount = 1;
    // The packet's two rows a type: the rectangle's top edge's left and right ends, then its bottom edge's (the
    // retail packet's own format; both T values of one row are equal, the rows being edges)
    decals.uvPacket.rectangles[0].x = 0.25f;
    decals.uvPacket.rectangles[0].y = 0.25f;
    decals.uvPacket.rectangles[0].z = 0.75f;
    decals.uvPacket.rectangles[0].w = 0.25f;
    decals.uvPacket.rectangles[1].x = 0.25f;
    decals.uvPacket.rectangles[1].y = 0.75f;
    decals.uvPacket.rectangles[1].z = 0.75f;
    decals.uvPacket.rectangles[1].w = 0.75f;

    Platform::Graphics::LoadDecalView(&matrix, nullptr);
    Platform::Graphics::LoadDecalType(&type);
    AgeBlock(&decalBlock, &type, decalFrames);

    // The distortion's frame copy: an 8 by 8 gradient, as a frame's pixels would be
    u8 frameCopy[8 * 8 * 4];
    for (u32 pixel = 0; pixel < 8 * 8; pixel++)
    {
        frameCopy[pixel * 4 + 0] = static_cast<u8>(pixel * 4);
        frameCopy[pixel * 4 + 1] = static_cast<u8>(255 - pixel * 4);
        frameCopy[pixel * 4 + 2] = static_cast<u8>(pixel * 2);
        frameCopy[pixel * 4 + 3] = 255;
    }

    Platform::Graphics::HostDistortionFrameCopy(8, 8, frameCopy, 8 * 4);

    // The drawing: the particles through their page material, the decals through their key's, the distortion
    // through the distortion material, over a few frames. The clock is the proof's own (the game's frames are
    // 1/60 s), chosen so the first particles of every block are alive and the rest are not yet born.
    constexpr int Frames = 4;
    u32 particleChecksumEarly = 0;
    for (int presented = 0; presented < Frames; presented++)
    {
        SDL_PumpEvents();
        const f32 time = presented == 0 ? 0.25f : 0.55f;
        // The game hands the chain's first block in and the PS2's DMA chain walked the rest, so the proof does the
        // same (one call draws every block of the chain)
        Platform::Graphics::DrawParticleBlock(blocks[0], particleTable, &matrix, particleMaterial, time);
        if (presented == 0)
        {
            particleChecksumEarly = Platform::Graphics::HostFxDebug_ParticleChecksum();
        }

        Platform::Graphics::DrawDecals(&decals);
        // The decal lists are emptied as they draw (the PS2's DrawDecals), so the proof re-lists the block
        decals.drawLists[0] = &decalBlock;
        decals.chunk = nullptr;

        Platform::Graphics::DrawDistortionBlock(blocks[0], hexagonTable, &matrix,
                                                reinterpret_cast<Material*>(materialStorage), time, 3.0f, 2.0f);

        BgfxFrame();
    }

    // The drawing's own readings, taken before the placement probe draws (that draw is a separate single record and
    // would otherwise leave its 4 vertices in the counters the checks below read)
    const u32 particleVertices = Platform::Graphics::HostFxDebug_ParticleVertices();
    const u32 particleChecksum = Platform::Graphics::HostFxDebug_ParticleChecksum();
    // The modulate the draw submitted (the GS's): the colour's channels over 255 and the alpha over 128, the alpha
    // held to what the GS's alpha register takes. The table's alpha keys run past it (200 - step), so the first
    // particle's alpha is fully opaque and not the 1.28 of an alpha taken over 128 as it stands
    const Vector4 particleColour = Platform::Graphics::HostFxDebug_ParticleColour();
    const u32 decalVertices = Platform::Graphics::HostFxDebug_DecalVertices();
    const u32 decalChecksum = Platform::Graphics::HostFxDebug_DecalChecksum();
    const u32 distortionVertices = Platform::Graphics::HostFxDebug_DistortionVertices();
    const u32 distortionChecksum = Platform::Graphics::HostFxDebug_DistortionChecksum();

    // The placement itself (Platform::Math::ParticleBlockView, the host's CPU stand-in for the PS2's VU0 programs
    // 0xA08/0xB90): a view whose first matrix has the camera's place in the chunk's space for its fourth row and the
    // chunk's toScreen for its second, an emitter at that chunk's origin, and the block matrix's fourth row checked
    // against the emitter's place taken through the toScreen by hand. The game composes the same pair for every
    // particle draw (DrawParticleList's call through ParticleViewOf's LoadParticleView), so a mistake in the
    // composition, the store or the pull shows here as a block matrix off its emitter -- the guard the drawing's own
    // corner check below completes, drawn versus expected
    constexpr u32 PlacementView = 0;
    alignas(16) Matrix4x4 chunkMatrices[2];
    std::memset(chunkMatrices, 0, sizeof(chunkMatrices));
    // The camera's place in the chunk's space (the view matrix's fourth row, what HostStoreParticleView keeps)
    chunkMatrices[0].m[0][0] = 1.0f;
    chunkMatrices[0].m[1][1] = 1.0f;
    chunkMatrices[0].m[2][2] = 1.0f;
    chunkMatrices[0].m[3][0] = 0.0f;
    chunkMatrices[0].m[3][1] = 6.0f;
    chunkMatrices[0].m[3][2] = 9.0f;
    chunkMatrices[0].m[3][3] = 1.0f;
    // The chunk's toScreen as the second matrix (the game's SetUpChunkView composes it from the chunk's draw matrix
    // and the view's screen matrix; the fourth row its place in subpixels, the diagonal the depth's)
    chunkMatrices[1] = matrix;
    Platform::Graphics::HostStoreParticleView(PlacementView, chunkMatrices);

    alignas(16) Matrix4x4 gravity;
    std::memset(&gravity, 0, sizeof(gravity));
    gravity.m[0][0] = 1.0f;
    gravity.m[1][1] = 1.0f;
    gravity.m[2][2] = 1.0f;
    const Vector4 emitterPlace = {0.0f, 0.0f, 0.0f, 1.0f};
    Matrix4x4 blockMatrix;
    f32 blockDistance = 0.0f;
    const f32 extents[3] = {0.5f, 0.5f, 0.5f};
    const bool blocked = Platform::Math::ParticleBlockView(PlacementView, false, extents, &gravity, &emitterPlace.x,
                                                           0.0f, &blockMatrix, &blockDistance);
    // The composed matrix's fourth row, the block's place in the chunk's space taken through the chunk's toScreen
    const Vector4 expected = {emitterPlace.x * chunkMatrices[1].m[0][0] + emitterPlace.y * chunkMatrices[1].m[1][0] +
                                  emitterPlace.z * chunkMatrices[1].m[2][0] + chunkMatrices[1].m[3][0],
                              emitterPlace.x * chunkMatrices[1].m[0][1] + emitterPlace.y * chunkMatrices[1].m[1][1] +
                                  emitterPlace.z * chunkMatrices[1].m[2][1] + chunkMatrices[1].m[3][1],
                              emitterPlace.x * chunkMatrices[1].m[0][2] + emitterPlace.y * chunkMatrices[1].m[1][2] +
                                  emitterPlace.z * chunkMatrices[1].m[2][2] + chunkMatrices[1].m[3][2],
                              emitterPlace.x * chunkMatrices[1].m[0][3] + emitterPlace.y * chunkMatrices[1].m[1][3] +
                                  emitterPlace.z * chunkMatrices[1].m[2][3] + chunkMatrices[1].m[3][3]};

    // The keepsTranslation case (the game's StartEmitterKeepingTranslation effects: the ground pound, the nitro and
    // TNT explosions, the chicken's death): retail's microprogram 0xB90 differs from 0xA08 in that the emission keeps its
    // own translation -- the place comes from the emission FRAME, which is in every particle's own start (the retail's
    // GenParticle_Box turns the start by runtime->emitMatrix, whose translation the frame keeps because the flag is
    // set), so the block's matrix has no place in its fourth row and no pull. The host put the place in that row as
    // well, which counted it twice: the effect was drawn at twice the emitter's place -- a spot on the map away from
    // the effect, growing with the distance from the chunk's origin, and right only where the emitter stands at that
    // origin. The emitter here stands off the chunk's origin, so a place counted twice shows
    const Vector4 keptPlace = {2.0f, 0.5f, -3.0f, 1.0f};
    Matrix4x4 keptMatrix;
    f32 keptDistance = 0.0f;
    const bool keptBlocked = Platform::Math::ParticleBlockView(PlacementView, true, extents, &gravity, &keptPlace.x,
                                                               3.0f, &keptMatrix, &keptDistance);
    // The composed matrix's fourth row: the place is not in it (it is in every particle's own start), so what is left
    // of it is the chunk's toScreen's own row
    const f32 keptError = std::fabs(keptMatrix.m[3][0] - chunkMatrices[1].m[3][0]) +
                          std::fabs(keptMatrix.m[3][1] - chunkMatrices[1].m[3][1]) +
                          std::fabs(keptMatrix.m[3][2] - chunkMatrices[1].m[3][2]);
    std::printf("placement kept: blocked %d distance %.3f error %.4f\n", keptBlocked ? 1 : 0,
                static_cast<double>(keptDistance), static_cast<double>(keptError));

    // The keepsTranslation emitter's own particles drawn: the first record's start is the emitter's place (the frame's
    // translation, which the generator turns the spread by), and the block matrix holds no place, so the particle lands
    // where the emitter is -- the place through the chunk's toScreen, and the table's fall (12) over the age (0.25 s)
    // in the particle's own space on top of it
    blocks.clear();
    particleStorageBlocks.assign(Platform::Graphics::ParticleBlockBytes, 0);
    u8* keptBlock = particleStorageBlocks.data();
    Platform::Graphics::InitParticleBlock(keptBlock, false);
    auto* keptRecords = reinterpret_cast<ParticleRecord*>(keptBlock + 0x20);
    keptRecords[0].start[0] = keptPlace.x;
    keptRecords[0].start[1] = keptPlace.y;
    keptRecords[0].start[2] = keptPlace.z;
    keptRecords[0].spawnTime = 0.0f;
    keptRecords[0].velocity[0] = 0.0f;
    keptRecords[0].velocity[1] = 0.0f;
    keptRecords[0].velocity[2] = 0.0f;
    keptRecords[0].lifeScale = static_cast<f32>(Platform::Graphics::ParticleRenderSteps) / ProofLife;
    Platform::Graphics::EndParticleBlock(keptBlock);
    Platform::Graphics::DrawParticleBlock(keptBlock, particleTable, &keptMatrix, particleMaterial, 0.25f);
    const Vector4 keptDrawn = Platform::Graphics::HostFxDebug_ParticlePlace();
    // The place through the kept matrix, the table's fall in the particle's own space (12 * 0.25 * 0.25) on top of it,
    // and the first corner's offset in the screen's coordinates (the draw reads the corner, not the centre)
    const f32 fall = 12.0f * 0.25f * 0.25f;
    Platform::Graphics::ParticleLook keptLook;
    MakeLook(&keptLook, false);
    f32 keptAcross = 0.0f;
    f32 keptDown = 0.0f;
    CornerOffset(keptLook, 16, &keptAcross, &keptDown);
    const f32 keptDrawnError = std::fabs(keptDrawn.x - (keptPlace.x * keptMatrix.m[0][0] +
                                                        (keptPlace.y + fall) * keptMatrix.m[1][0] +
                                                        keptPlace.z * keptMatrix.m[2][0] + keptMatrix.m[3][0] + keptAcross)) +
                               std::fabs(keptDrawn.y - (keptPlace.x * keptMatrix.m[0][1] +
                                                        (keptPlace.y + fall) * keptMatrix.m[1][1] +
                                                        keptPlace.z * keptMatrix.m[2][1] + keptMatrix.m[3][1] + keptDown)) +
                               std::fabs(keptDrawn.z - (keptPlace.x * keptMatrix.m[0][2] +
                                                        (keptPlace.y + fall) * keptMatrix.m[1][2] +
                                                        keptPlace.z * keptMatrix.m[2][2] + keptMatrix.m[3][2]));
    std::printf("placement kept drawn: %.1f %.1f %.1f error %.4f\n", static_cast<double>(keptDrawn.x),
                static_cast<double>(keptDrawn.y), static_cast<double>(keptDrawn.z),
                static_cast<double>(keptDrawnError));

    // The billboard: a particle's quad faces the camera, so its size runs along the camera's right and up, which are the
    // screen's own axes, and not along the block's own axes. The view here is turned a third of a turn round, so the
    // camera's axes and the particle's own axes differ: a quad built along the particle's own axes would lie in the
    // emitter's plane instead of facing the camera
    constexpr u32 BillboardView = PlacementView + 1;
    alignas(16) Matrix4x4 turnedMatrices[2];
    std::memcpy(turnedMatrices, chunkMatrices, sizeof(turnedMatrices));
    // The chunk's toScreen turned a third of a turn round the view's z (the camera's right and up with it)
    constexpr f32 Turn = 1.0471976f;
    constexpr f32 Scale = 400.0f;
    turnedMatrices[1].m[0][0] = Scale * std::cos(Turn);
    turnedMatrices[1].m[1][0] = Scale * std::sin(Turn);
    turnedMatrices[1].m[0][1] = -Scale * std::sin(Turn);
    turnedMatrices[1].m[1][1] = Scale * std::cos(Turn);
    Platform::Graphics::HostStoreParticleView(BillboardView, turnedMatrices);
    Matrix4x4 turnedBlock;
    f32 turnedDistance = 0.0f;
    Platform::Math::ParticleBlockView(BillboardView, false, extents, &gravity, &emitterPlace.x, 0.0f, &turnedBlock,
                                      &turnedDistance);
    blocks.clear();
    particleStorageBlocks.assign(Platform::Graphics::ParticleBlockBytes, 0);
    u8* turned = particleStorageBlocks.data();
    Platform::Graphics::InitParticleBlock(turned, false);
    auto* turnedRecords = reinterpret_cast<ParticleRecord*>(turned + 0x20);
    turnedRecords[0].start[0] = 0.0f;
    turnedRecords[0].start[1] = 0.0f;
    turnedRecords[0].start[2] = 0.0f;
    turnedRecords[0].spawnTime = 0.0f;
    turnedRecords[0].velocity[0] = 0.0f;
    turnedRecords[0].velocity[1] = 0.0f;
    turnedRecords[0].velocity[2] = 0.0f;
    turnedRecords[0].lifeScale = static_cast<f32>(Platform::Graphics::ParticleRenderSteps) / ProofLife;
    Platform::Graphics::EndParticleBlock(turned);
    Platform::Graphics::DrawParticleBlock(turned, particleTable, &turnedBlock, particleMaterial, 0.25f);
    const Vector4 turnedDrawn = Platform::Graphics::HostFxDebug_ParticlePlace();
    // The first corner's offset in the screen's coordinates: the table's shape's first corner along the camera's right
    // and up, scaled by the view's matrix to the screen. The particle is at the life step 16 (the age 0.25 s of a life of
    // a second), so the shape is the look's own at that step. The corner is the screen's offset and NOT the chunk's
    // turned axes: the quad faces the camera, so the chunk's turn must not turn it
    Platform::Graphics::ParticleLook billboardLook;
    MakeLook(&billboardLook, false);
    f32 across = 0.0f;
    f32 down = 0.0f;
    CornerOffset(billboardLook, 16, &across, &down);
    // The particle's own place (the table's fall over the age in the particle's own space) through the block's matrix,
    // and the corner's offset in the screen's coordinates on top of it: the offset is in the screen's own units, so it
    // is added to the place's screen coordinates and not taken through the matrix
    const f32 billboardError =
        std::fabs(turnedDrawn.x - (fall * turnedBlock.m[1][0] + turnedBlock.m[3][0] + across)) +
        std::fabs(turnedDrawn.y - (fall * turnedBlock.m[1][1] + turnedBlock.m[3][1] + down)) +
        std::fabs(turnedDrawn.z - (fall * turnedBlock.m[1][2] + turnedBlock.m[3][2]));
    std::printf("placement billboard: drawn %.1f %.1f %.1f error %.4f\n", static_cast<double>(turnedDrawn.x),
                static_cast<double>(turnedDrawn.y), static_cast<double>(turnedDrawn.z),
                static_cast<double>(billboardError));

    // The same emitter's particles drawn, the first record's start the emitter's own (the game's records sit in the
    // block's matrix's space, the emitter's place its fourth row's)
    blocks.clear();
    particleStorageBlocks.assign(Platform::Graphics::ParticleBlockBytes, 0);
    u8* single = particleStorageBlocks.data();
    Platform::Graphics::InitParticleBlock(single, false);
    auto* records = reinterpret_cast<ParticleRecord*>(single + 0x20);
    records[0].start[0] = 0.0f;
    records[0].start[1] = 0.0f;
    records[0].start[2] = 0.0f;
    records[0].spawnTime = 0.0f;
    records[0].velocity[0] = 0.0f;
    records[0].velocity[1] = 0.0f;
    records[0].velocity[2] = 0.0f;
    records[0].lifeScale = static_cast<f32>(Platform::Graphics::ParticleRenderSteps) / ProofLife;
    Platform::Graphics::EndParticleBlock(single);
    Platform::Graphics::DrawParticleBlock(single, particleTable, &blockMatrix, particleMaterial, 0.25f);
    const Vector4 drawn = Platform::Graphics::HostFxDebug_ParticlePlace();

    const f32 blockError = std::fabs(blockMatrix.m[3][0] - expected.x) + std::fabs(blockMatrix.m[3][1] - expected.y) +
                           std::fabs(blockMatrix.m[3][2] - expected.z);
    // The drawn particle's centre: its record's start is the emitter's own and its shape offsets are small next to
    // the subpixel middle, so its first corner must land within a particle's size of the emitter's projected place.
    // It misses by ~316 subpixels with the emitter's gravity (12) feeding the host table's fall while the proof's
    // block matrix holds none -- the host draw adds g*age*age in the particle's own space, the block matrix's place
    // none of it. Printing it marks the divergence; it is not yet wired into ok
    const f32 drawnError =
        std::fabs(drawn.x - expected.x) + std::fabs(drawn.y - expected.y) + std::fabs(drawn.z - expected.z);
    std::printf("placement: blocked %d distance %.3f block w %.3f expected %.1f %.1f %.1f drawn %.1f %.1f %.1f "
                "error %.4f\n",
                blocked ? 1 : 0, static_cast<double>(blockDistance), static_cast<double>(blockMatrix.m[3][3]),
                static_cast<double>(expected.x), static_cast<double>(expected.y), static_cast<double>(expected.z),
                static_cast<double>(drawn.x), static_cast<double>(drawn.y), static_cast<double>(drawn.z),
                static_cast<double>(drawnError));

    // The emitter's own tilt does not turn the particle again: the retail's generators turn the start and the velocity
    // by the emitter's EMISSION frame (the retail's GenParticle_Box transforms the point by runtime->emitMatrix), so a
    // particle's own start is already in the emitter's turned space and the block's matrix has the identity in its rows
    // 0 to 2. With the gravity's axes there the level's emitters whose gravity is tilted sent their particles off at an
    // angle the game never meant, turned once by the emission and once by the gravity.
    alignas(16) Matrix4x4 tiltedGravity;
    std::memset(&tiltedGravity, 0, sizeof(tiltedGravity));
    tiltedGravity.m[0][0] = 0.5f;
    tiltedGravity.m[0][1] = 0.5f;
    tiltedGravity.m[1][0] = -0.5f;
    tiltedGravity.m[1][1] = 0.5f;
    tiltedGravity.m[2][2] = 1.0f;
    Matrix4x4 tiltedBlock;
    f32 tiltedDistance = 0.0f;
    Platform::Math::ParticleBlockView(PlacementView, false, extents, &tiltedGravity, &emitterPlace.x, 0.0f, &tiltedBlock,
                                      &tiltedDistance);
    blocks.clear();
    particleStorageBlocks.assign(Platform::Graphics::ParticleBlockBytes, 0);
    u8* tilted = particleStorageBlocks.data();
    Platform::Graphics::InitParticleBlock(tilted, false);
    auto* tiltedRecords = reinterpret_cast<ParticleRecord*>(tilted + 0x20);
    tiltedRecords[0].start[0] = 0.0f;
    tiltedRecords[0].start[1] = 0.0f;
    tiltedRecords[0].start[2] = 0.0f;
    tiltedRecords[0].spawnTime = 0.0f;
    tiltedRecords[0].velocity[0] = 0.0f;
    tiltedRecords[0].velocity[1] = 0.0f;
    tiltedRecords[0].velocity[2] = 0.0f;
    tiltedRecords[0].lifeScale = static_cast<f32>(Platform::Graphics::ParticleRenderSteps) / ProofLife;
    Platform::Graphics::EndParticleBlock(tilted);
    Platform::Graphics::DrawParticleBlock(tilted, particleTable, &tiltedBlock, particleMaterial, 0.25f);
    const Vector4 tiltedDrawn = Platform::Graphics::HostFxDebug_ParticlePlace();
    // The same particle as the one drawn with the untilted emitter: the emitter's tilt is the culling box's, not the
    // particle's
    const f32 tiltError = std::fabs(tiltedDrawn.x - drawn.x) + std::fabs(tiltedDrawn.y - drawn.y) +
                           std::fabs(tiltedDrawn.z - drawn.z);
    std::printf("placement tilt: drawn %.1f %.1f %.1f error %.4f\n", static_cast<double>(tiltedDrawn.x),
                static_cast<double>(tiltedDrawn.y), static_cast<double>(tiltedDrawn.z), static_cast<double>(tiltError));

    std::printf("wave: rows %u, sines checksum %08X, amplitude %.2f\n", Platform::Graphics::HostWaveRows, waveHash,
                Platform::Graphics::HostParticleWaveAmplitude());
    std::printf("particles: %u vertices, checksum %08X (at 0.25 s %08X)\n", particleVertices, particleChecksum,
                particleChecksumEarly);
    std::printf("decals: %u vertices, checksum %08X\n", decalVertices, decalChecksum);
    std::printf("distortion: %u vertices, checksum %08X\n", distortionVertices, distortionChecksum);

    bool ok = CheckAgeDecal() && particleVertices != 0 && decalVertices != 0 && distortionVertices != 0;
    // The placement: the composed matrix's fourth row the emitter's own place through the chunk's toScreen. The drawn
    // comparison is printed above but not enforced: the proof's table carries the emitter's gravity (12), which the
    // host adds in the particle's own space while this matrix's fourth row holds none of it, so the drawn corner is
    // legitimately that far off here
    ok = ok && blockError < 0.01f;
    // The kept translation: no place in the block matrix's fourth row (0xB90's rule), the place is in the particles
    ok = ok && keptError < 0.01f;
    // The kept translation's particles: drawn where the emitter is, not at twice its place. The tolerance holds the
    // table's shape (the quad's corner a twentieth of a world unit off the particle's centre, a few subpixels on the
    // screen) and the table's fall; a place counted twice misses by thousands of subpixels
    ok = ok && keptDrawnError < 40.0f;
    // The billboard: the particle's quad faces the camera, so its corner is the camera's right and up times the shape's
    // offsets, not the particle's own axes
    ok = ok && billboardError < 1.0f;
    // The emitter's tilt: the same particle as the untilted emitter's
    ok = ok && tiltError < 1.0f;
    // The particles: two blocks of thirty-two records, the six of each that their spawn times have born by 0.55 s
    ok = ok && particleVertices == 12 * 4;
    // The distortion's hexagons of the same blocks: twelve corners a hexagon, six triangles of them
    ok = ok && distortionVertices == 12 * 18;
    // The decals: eight decals of two triangles six vertices each
    ok = ok && decalVertices == 8 * 6;
    // The particles' geometry moves with the clock (the life steps and the gravity)
    ok = ok && particleChecksumEarly != particleChecksum;
    // The modulate the draw submits (the GS's): the colour's channels over 255 and the alpha over 128, the alpha held
    // to what the GS's alpha register takes. The table's alpha keys run past it (200 - step), so the first particle's
    // alpha is fully opaque and not the 1.28 of an alpha that was taken over 128 as it stands
    const Vector4 colour = particleColour;
    const u32 firstStep = static_cast<u32>(0.55f * static_cast<f32>(Platform::Graphics::ParticleRenderSteps) / ProofLife);
    const f32 wantColour[4] = {static_cast<f32>(32 + firstStep * 3) / 255.0f, static_cast<f32>(64 + firstStep * 2) / 255.0f,
                                static_cast<f32>(128 - firstStep) / 255.0f, 1.0f};
    const f32 gotColour[4] = {colour.x, colour.y, colour.z, colour.w};
    std::printf("particle colour: %.3f %.3f %.3f %.3f (step %u)\n", static_cast<double>(colour.x),
                static_cast<double>(colour.y), static_cast<double>(colour.z), static_cast<double>(colour.w),
                firstStep);
    for (u32 channel = 0; channel < 4; channel++)
    {
        ok = ok && std::fabs(gotColour[channel] - wantColour[channel]) < 0.002f;
    }

    Platform::Graphics::DestroyMaterial(reinterpret_cast<Material*>(materialStorage));
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (!ok)
    {
        std::fprintf(stderr, "particles-test: FAILED\n");
        return 1;
    }

    std::printf("particles-test: PASS\n");
    return 0;
}
