// Graphics: the null platform's stubs for everything the bgfx renderer hasn't replaced yet; the
// material system (material_system.cpp) and 2D drawing (draw2d.cpp) carry the 2D functions instead.
#include "../display_sdl.h"

#include "draws_host.h"
#include "host_graphics.h"
#include "mesh_builder.h"
#include "../settings.h"
#include "game/controllers.h"
#include "game/animation.h"
#include "game/graphicstables.h"
#include "game/math.h"
#include "game/particles.h"
#include "game/place.h"
#include "game/renderer.h"
#include "game/scenery.h"
#include "game/shaderanimation.h"
#include "game/stream.h"
#include "game/view.h"
// The logic step counter the HUD reads: the port's own steps, which the picture's frames are separate from
#include "platform/hostframe.h"
#include "platform/system.h"
#include "platform/time.h"

#include <SDL3/SDL_timer.h>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>
#if defined(_WIN32)
#include <malloc.h>
#endif

namespace
{
// The frame's memory (AllocFrameMemory): a bump arena of chunks that live until the frame is drawn (the PS2's own is the
// frame buckets' DMA chain). A chunk never moves once made, so the pointers handed out stay valid. The chunks themselves
// are kept between frames: a step begins by starting the write position over at the first one, so the arena costs no
// allocator work per frame (the previous frame's bytes are not read again)
constexpr size_t FrameMemoryChunk = 1u << 20;

std::vector<std::unique_ptr<u8[]>> g_FrameMemory;
std::vector<size_t> g_FrameMemorySizes;
size_t g_FrameMemoryChunk = 0;
size_t g_FrameMemoryUsed = 0;

// One past the frame FinishFrame last presented, which is the index the next one takes (the probes' gate:
// HostFrameCounter)
u32 g_frameCounter = 0;

void ResetFrameMemory()
{
    // The chunks stay (the arena is the process's): the frame only starts writing at the first one again
    g_FrameMemoryChunk = 0;
    g_FrameMemoryUsed = 0;
}

u8* AllocFrameMemoryArena(size_t bytes)
{
    // The DMA tags the PS2's allocation must start a quadword into: keep the host's quadword aligned too
    bytes = (bytes + 0xF) & ~static_cast<size_t>(0xF);
    if (bytes == 0)
    {
        bytes = 0x10;
    }

    // The first chunk with room from the write position on. A chunk too small for the request is grown in place when
    // nothing has been written into it this frame (its old bytes belong to a frame that has already presented)
    for (;;)
    {
        if (g_FrameMemoryChunk >= g_FrameMemory.size())
        {
            const size_t size = bytes > FrameMemoryChunk ? bytes : FrameMemoryChunk;
            g_FrameMemory.push_back(std::unique_ptr<u8[]>(new u8[size]));
            g_FrameMemorySizes.push_back(size);
            g_FrameMemoryUsed = 0;
            break;
        }

        if (g_FrameMemoryUsed + bytes <= g_FrameMemorySizes[g_FrameMemoryChunk])
        {
            break;
        }

        if (g_FrameMemoryUsed == 0)
        {
            const size_t size = bytes > FrameMemoryChunk ? bytes : FrameMemoryChunk;
            g_FrameMemory[g_FrameMemoryChunk].reset(new u8[size]);
            g_FrameMemorySizes[g_FrameMemoryChunk] = size;
            break;
        }

        g_FrameMemoryChunk++;
        g_FrameMemoryUsed = 0;
    }

    u8* start = g_FrameMemory[g_FrameMemoryChunk].get() + g_FrameMemoryUsed;
    g_FrameMemoryUsed += bytes;
    return start;
}

// The resources' sizes on the PS2 (their layouts', src/platform/ps2/renderer/renderer.h)
constexpr u32 TextureResourceSize = 0x6C;
constexpr u32 MaterialResourceSize = 0x10;
constexpr u32 ModelResourceSize = 0x1C;
constexpr u32 RigidModelResourceSize = 0x20;
constexpr u32 SkinResourceSize = 0x24;
constexpr u32 BlendSkinResourceSize = 0x18;
constexpr u32 LodResourceSize = 0x24;
constexpr u32 SkyResourceSize = 0x50;

// A resource made with its ID: zeroed memory with the tables' header (no references, the ID)
void* MakeResource(u32 id, u32 size)
{
    void* memory = Platform::Graphics::HostMemory::AllocateZeroed(1, size);
    static_cast<u32*>(memory)[1] = id;
    return memory;
}

// A resource read from the stream is trusted only within these bounds: a damaged or truncated disc read must not turn
// into a multi-gigabyte allocation, a null write or a loop over a count the file made up
constexpr u32 MaxResourceSubModels = 1024;
constexpr u32 MaxResourceBlendParts = 1024;
constexpr u32 MaxResourceShapes = 255;
constexpr u32 MaxResourcePacketBytes = 0x4000000; // 64 MiB

// A submodel's or part's bytes: null when the size is not one a resource can have or the allocator fails, so the read
// skips the bytes rather than writing through nothing
void* AllocResourceBytes(u32 size)
{
    if (size == 0 || size > MaxResourcePacketBytes)
    {
        return nullptr;
    }

    return Platform::Graphics::HostMemory::Allocate(size);
}

// Past a size a signed Skip takes as one step
void SkipResourceBytes(Stream* stream, u64 bytes)
{
    while (bytes > 0)
    {
        const u32 step = bytes > 0x1000000 ? 0x1000000 : static_cast<u32>(bytes);
        stream->Skip(static_cast<s32>(step));
        bytes -= step;
    }
}
// A material the game's tables freed while something still draws through it, and the texture whose upload it still names.
//
// Every chunk unload runs ReleaseGraphicsResources (src/game/graphicstables.cpp), which frees every material and texture in
// the game's shared tables whatever references they carry, and a chunk transition runs it once per chunk unloaded. Some
// draws survive that with a pointer they took earlier: the pause screen's level title card is one -- OLEG read the picture
// once (Shape2D::Read) and keeps its material, and its own "already read" state stops it being read again. The console
// needs nothing to make that work: the freed bytes are still in the heap and the picture's pixels are still in the GS's
// memory, which the material's TEX0 address still names, so the draw keeps showing the same picture. The host frees the
// bytes for the next chunk's materials to reuse (the card then drew whichever material landed in the recycled block -- a
// rock's texture, or none at all: the white card) and destroys the upload. Keeping both reproduces the console, and keeping
// them out of circulation is what makes the pointer the game holds stay *this* material rather than a later one's.
//
// What leaves is decided by use, not age: a material is touched every time a draw resolves its texture
// (HostMaterialBoundTexture -> TouchFreedMaterial), so the entries that stay are the ones the game still draws through, and
// the ones that go are the chunk materials whose wipes nobody looks at again. The quarantine is that list, bounded at
// QuarantinedMaterials with the least recently drawn leaving first.
struct FreedMaterial
{
    void* resource;
    void* host;
    // The first shader's texture (HostMaterial's gameTexture): its upload is kept while this entry lives
    GameTexture* texture;
};

constexpr size_t QuarantinedMaterials = 8192;
// A safety valve on the uploads retained for them, in case a session strands many large textures: the oldest goes first
constexpr size_t QuarantinedTextures = 1024;

std::list<FreedMaterial> g_freedMaterials;
// The quarantined materials by their host pointer, for the draws' touch and the eviction's erase
std::unordered_map<const void*, std::list<FreedMaterial>::iterator> g_freedByHost;
// How many quarantined materials name each texture: it keeps its upload while any does
std::unordered_map<const GameTexture*, u32> g_texturePins;
// The textures whose resource died while a quarantined material still named them: their bytes and their upload are kept
std::vector<GameTexture*> g_freedTextures;

bool TexturePinned(const GameTexture* texture)
{
    const auto found = g_texturePins.find(texture);
    return found != g_texturePins.end() && found->second != 0;
}

// The material's later shaders' settings and textures (defined with their table further down): the quarantine's release is
// the last reference to them
static void RemoveMaterialLater(const void* material);

// A freed texture's upload and bytes let go: nothing that still draws names it
void ReleaseFreedTexture(GameTexture* texture)
{
    // Another quarantined material still draws with it: it keeps its place in g_freedTextures
    if (TexturePinned(texture))
    {
        return;
    }

    for (size_t at = 0; at < g_freedTextures.size(); at++)
    {
        if (g_freedTextures[at] != texture)
        {
            continue;
        }

        g_freedTextures.erase(g_freedTextures.begin() + static_cast<long>(at));
        Platform::Graphics::HostTextures().InvalidateTransfer(texture);
        Platform::Graphics::HostMemory::Free(texture);
        return;
    }

    // Still the game's (the table holds it): its own DeleteTexture invalidates it
}

// The least recently drawn material out of the quarantine: its bytes go, and its texture's upload with them when nothing
// else names it
void ReleaseOldestFreedMaterial()
{
    if (g_freedMaterials.empty())
    {
        return;
    }

    FreedMaterial freed = g_freedMaterials.front();
    g_freedMaterials.pop_front();
    g_freedByHost.erase(freed.host);
    if (freed.texture != nullptr)
    {
        const auto pin = g_texturePins.find(freed.texture);
        if (pin != g_texturePins.end() && pin->second > 1)
        {
            pin->second--;
        }
        else
        {
            g_texturePins.erase(freed.texture);
        }

        ReleaseFreedTexture(freed.texture);
    }

    // Nothing can draw through the material's bytes any more, so its later shaders' settings and textures go with it
    RemoveMaterialLater(freed.host);
    Platform::Graphics::HostMemory::Free(freed.host);
    Platform::Graphics::HostMemory::Free(freed.resource);
}
// The PS2 resources' bytes the null platform reads past instead of uploading (src/platform/ps2/renderer/resources.cpp): a
// texture's data is its size, the GS's header, 0x20 bytes of the tools' memory and its image
constexpr u32 TextureGsHeaderSize = 0x60;
constexpr u32 TextureToolsHeaderSize = 0x20;
// A shader after its type: the type's extra words, then the base's 30 setting bytes (the animation flag last), TEX1's K and L,
// the leftover bytes, the colour, the scrolls' phases and speeds, the texture's ID and the type, and the animation when the flag
// says so (shaderclasses.cpp's ReadShader)
constexpr u32 ShaderSettingFields = 30;
constexpr u32 ShaderHasAnimation = 29;
// The setting byte saying a shader samples a texture (shaderclasses.cpp's ReadShader: ShaderSettings::textured)
constexpr u32 ShaderIsTextured = 11;
// The shader type whose ShaderMakesBillboard answers yes (shaderclasses.cpp's ShaderType1BMakesBillboard): a model with
// such a shader is turned to face the camera when it is placed (resources.cpp's AnyShader, ShaderBillboardSlot)
constexpr s32 ShaderTypeBillboard = 0x1B;
// A joint's settings in an animation, and a LOD's mesh switch count
constexpr u32 JointSettingsSize = 8;
constexpr u32 LodSwitchCount = 3;
// A LOD's versions (resources.cpp)
constexpr s32 LodDistancesVersion = 0x1001;
constexpr s32 LodSquaresVersion = 0x1002;
// A blend part's shape factors, and the shift of a shape's size from quadwords to bytes
constexpr u32 ShapeFactorsSize = 12;
constexpr u32 QuadwordShift = 4;

// A shader animation's data size from its layout word and frames (game/animation.cpp's AnimationDataSize)
u32 ShaderAnimationDataSize(u32 layout, u16 frames)
{
    if (frames == 0)
    {
        return 0;
    }

    u32 joints = layout & 0x7F;
    u32 staticValues = (layout >> 11) & 0x7FF;
    u32 frameValues = (layout >> 22) & 0x3FF;
    return joints * static_cast<u32>(sizeof(JointTrackSettings)) + staticValues * sizeof(s16) + frameValues * frames * sizeof(s16);
}

// A material's shader's scroll state, as ReadMaterial keeps it for its draws (renderer.h's Shader::scrollPhases,
// scrollSpeeds and scroll: the first is the phase the mode's function advances, the second its speed, the third the
// offset the draws take)
struct HostShaderScroll
{
    f32 phases[2];
    f32 speeds[2];
    f32 scroll[2];
};

namespace
{
// A material's shaders' animations as the host keeps them: the game's ShaderAnimation (0x40 bytes, read whole by
// SkipShader and played by shaderanimation.cpp) for each shader that has one, and the colour shader 0's tracks land in
// (renderer.h's Shader::shaderColour, the animation's colour X 256 and its alpha X 127, the retail UpdateShader). The
// game's material storage is a fixed 0x70 bytes (MaterialStorage) and must not grow, so the animations live here,
// beside the material, one entry each, from ReadMaterial until DeleteMaterial lets them go. Retail keeps an animation
// per shader (AnimateMaterial calls every shader's Update slot, RestartAnimations restarts every shader), so a material
// whose animated shader is not the first still animates and still restarts
constexpr u32 HostMaterialShaderSlots = 4;
struct HostMaterialAnimation
{
    Platform::Graphics::HostMaterial* material;
    ShaderAnimation* animations[HostMaterialShaderSlots];
    // renderer.h's Shader::shaderColour, written while the first shader animates its colour
    f32 shaderColour[4];
};

std::vector<HostMaterialAnimation> g_MaterialAnimations;

// A material's later shaders, beside its storage (the game's material is a fixed 0x70 bytes with no room for them). Two
// things are each shader's own and not the first one's: its 30 setting bytes -- the GS writes TEST (ATST, AREF and AFAIL,
// the action a sample that fails the test is written with), ALPHA (ABE and the preset), ZBUF (ZTST and its write) and
// DATE from the registers the shader being drawn sends (shadersettings.cpp's PixelTest and DepthBuffer) -- and its image,
// which materials.cpp's WriteShaders uploads before that shader's packet, so pass k samples shader k's picture
struct HostMaterialLater
{
    // Shader k's settings, at index k - 1 (shader 0's are the material's own, in its shaderSettings)
    u8 settings[Platform::Graphics::HostMaterialPasses - 1][Platform::Graphics::HostShaderSettings];
    // Shader k's own scroll block (renderer.h's Shader::scrollPhases, scrollSpeeds and scroll: each shader has its own,
    // advanced by its own scroll modes and its own animation), at index k - 1
    HostShaderScroll scroll[Platform::Graphics::HostMaterialPasses - 1];
    // Shader k's own image, at index k (shader 0's is the material's gameTexture, and null here)
    GameTexture* textures[Platform::Graphics::HostMaterialPasses];
};

// By the material they belong to, from ReadMaterial until the quarantine lets the material's bytes go. Hashed, not a
// list walked per draw: every later pass of a multi-shader material looks its entry up, and a level's scenery holds
// thousands of those materials. An entry per material is what a fixed table beside the material cannot be (no room)
std::unordered_map<const Platform::Graphics::HostMaterial*, HostMaterialLater> g_MaterialLater;

HostMaterialLater* FindMaterialLater(const void* material)
{
    const auto found = g_MaterialLater.find(static_cast<const Platform::Graphics::HostMaterial*>(material));
    return found == g_MaterialLater.end() ? nullptr : &found->second;
}

// One of the material's later shaders' settings, byte for byte (a material with no such shader leaves them clear)
void MaterialLaterSettingsOf(const void* material, u32 pass, u8* settings)
{
    std::memset(settings, 0, Platform::Graphics::HostShaderSettings);
    HostMaterialLater* later = FindMaterialLater(material);
    if (later == nullptr || pass >= Platform::Graphics::HostMaterialPasses - 1)
    {
        // ShaderConstruct's default for the fields a clear block would read as "no": the one that matters is IIP, whose 0
        // means flat shading, so a later shader with no settings beside the material draws interpolated like the first one
        settings[Platform::Graphics::HostShaderSettingGouraud] = 1;
        return;
    }

    std::memcpy(settings, later->settings[pass], Platform::Graphics::HostShaderSettings);
}

// The settings and scroll block ReadMaterialShaders read for the material's later shaders, kept for the draws and for
// the frame step that advances them
void MaterialLaterStoreShader(const void* material, u32 pass, const u8* settings, const HostShaderScroll& scroll)
{
    if (material == nullptr || pass >= Platform::Graphics::HostMaterialPasses - 1)
    {
        return;
    }

    HostMaterialLater& later = g_MaterialLater[static_cast<const Platform::Graphics::HostMaterial*>(material)];
    std::memcpy(later.settings[pass], settings, Platform::Graphics::HostShaderSettings);
    later.scroll[pass] = scroll;
}

// The material's later passes' textures (pass 0 is the material's own gameTexture, and null here). Every slot is written,
// a material with no image for a pass included: the entry can belong to the address a material that died at it held (the
// quarantine keeps that one's bytes alive, so the allocator hands the same block out again), and an image left there would
// be drawn by a shader that samples none
void AddMaterialPassTextures(Platform::Graphics::HostMaterial* material, GameTexture* const* textures, u32 count)
{
    HostMaterialLater* later = FindMaterialLater(material);
    bool any = false;
    for (u32 pass = 1; pass < Platform::Graphics::HostMaterialPasses; pass++)
    {
        any = any || (pass < count && textures[pass] != nullptr);
    }

    // A single-shader material with nothing to keep by side (the common case) leaves no entry at all
    if (later == nullptr)
    {
        if (!any)
        {
            return;
        }

        later = &g_MaterialLater[material];
    }

    for (u32 pass = 1; pass < Platform::Graphics::HostMaterialPasses; pass++)
    {
        later->textures[pass] = pass < count ? textures[pass] : nullptr;
    }
}

// A later pass's texture, null when the material has none for it
GameTexture* MaterialPassTexture(const Platform::Graphics::HostMaterial* material, u32 pass)
{
    HostMaterialLater* later = FindMaterialLater(material);
    if (later == nullptr || pass >= Platform::Graphics::HostMaterialPasses)
    {
        return nullptr;
    }

    return later->textures[pass];
}

// The references those textures took go with the material, and its later shaders' settings with them
void RemoveMaterialPassTextures(const Platform::Graphics::HostMaterial* material)
{
    HostMaterialLater* later = FindMaterialLater(material);
    if (later == nullptr)
    {
        return;
    }

    for (u32 pass = 1; pass < Platform::Graphics::HostMaterialPasses; pass++)
    {
        if (later->textures[pass] != nullptr)
        {
            ReleaseTexture(later->textures[pass]);
        }
    }

    g_MaterialLater.erase(material);
}


HostMaterialAnimation* FindMaterialAnimation(const Platform::Graphics::HostMaterial* material)
{
    for (HostMaterialAnimation& entry : g_MaterialAnimations)
    {
        if (entry.material == material)
        {
            return &entry;
        }
    }

    return nullptr;
}

// The animations of a material read (ReadMaterial), its shaders', the slots without one left null
void AddMaterialAnimation(Platform::Graphics::HostMaterial* material, const ShaderAnimation* const* animations)
{
    HostMaterialAnimation entry = {};
    entry.material = material;
    for (u32 shader = 0; shader < HostMaterialShaderSlots; shader++)
    {
        entry.animations[shader] = const_cast<ShaderAnimation*>(animations[shader]);
    }

    g_MaterialAnimations.push_back(entry);
}

// A material's animations let go of (DeleteMaterial): their frames and their disk nodes with them
void RemoveMaterialAnimation(const Platform::Graphics::HostMaterial* material)
{
    for (size_t index = 0; index < g_MaterialAnimations.size(); index++)
    {
        if (g_MaterialAnimations[index].material == material)
        {
            for (u32 shader = 0; shader < HostMaterialShaderSlots; shader++)
            {
                if (g_MaterialAnimations[index].animations[shader] != nullptr)
                {
                    DestroyShaderAnimation(g_MaterialAnimations[index].animations[shader], FreeAfterDestroy);
                }
            }

            g_MaterialAnimations[index] = g_MaterialAnimations.back();
            g_MaterialAnimations.pop_back();
            return;
        }
    }
}
}

// A shader's bytes after its type: the type's extra words, then the base shader. Returns its texture's ID and, in
// *textured, whether its settings say it samples one (shaderclasses.cpp's ReadShader reads the same offsets). The
// 30 setting bytes are copied to *settings (when it isn't nullptr), which ReadMaterial keeps for its draws, the
// scroll phases and speeds to *scroll (also when it isn't nullptr), and the animation the settings name to
// *animation (again when it isn't nullptr): ReadMaterialShaders asks for the first shader's only, the rest skipped
u32 SkipShader(Stream* stream, s32 type, bool* textured, u8* settings, HostShaderScroll* scroll,
               ShaderAnimation** animation, u8* colour = nullptr)
{
    switch (type)
    {
    case 0x10:
    case 0x11:
    case 0x18:
        stream->Skip(sizeof(f32));
        break;
    case 0x17:
        stream->Skip(sizeof(s32) + 2 * sizeof(f32));
        break;
    case 0x1A:
        stream->Skip(sizeof(s32) + 4 * sizeof(f32));
        break;
    case 0x1C:
        stream->Skip(2 * sizeof(f32));
        break;
    default:
        break;
    }

    u8 read[ShaderSettingFields];
    stream->Read(read, sizeof(read), 1);
    if (settings != nullptr)
    {
        std::memcpy(settings, read, sizeof(read));
    }
    if (textured != nullptr)
    {
        *textured = read[ShaderIsTextured] != 0;
    }
    stream->Skip(2 * sizeof(s16));   // lodK and lodL
    stream->Skip(4 * sizeof(f32));   // the leftover floats
    // The shader's colour (renderer.h's Shader::shaderColour): the tint a type-0x1F material multiplies its vertex colour
    // by (the animation's own colour overrides it while it has one). The tools leave it zero on a material with no colour
    // of its own, which tints nothing
    {
        f32 floats[4];
        stream->Read(floats, sizeof(floats), 1);
        if (colour != nullptr)
        {
            const bool tinted = floats[0] != 0.0f || floats[1] != 0.0f || floats[2] != 0.0f;
            for (u32 channel = 0; channel < 4; channel++)
            {
                if (!tinted)
                {
                    colour[channel] = 255;
                    continue;
                }

                const f32 value = floats[channel] < 0.0f ? 0.0f : (floats[channel] > 1.0f ? 1.0f : floats[channel]);
                colour[channel] = static_cast<u8>(value * 255.0f + 0.5f);
            }
        }
    }
    if (scroll != nullptr)
    {
        // The phases U and V, then the speeds U and V (renderer.h's Shader: scrollPhases[2], scrollSpeeds[2])
        stream->ReadF32(&scroll->phases[0]);
        stream->ReadF32(&scroll->phases[1]);
        stream->ReadF32(&scroll->speeds[0]);
        stream->ReadF32(&scroll->speeds[1]);
    }
    else
    {
        stream->Skip(4 * sizeof(f32));
    }
    s32 textureId;
    stream->ReadS32(&textureId);
    s32 shaderType;
    stream->ReadS32(&shaderType);
    if (read[ShaderHasAnimation] != 0)
    {
        if (animation != nullptr)
        {
            // The shader's animation read whole (shaderclasses.cpp's ReadShader): its header, its data (loaded into
            // the disk manager) and a loop's length, the frames themselves found again as it is played
            auto* made = ConstructShaderAnimation(
                static_cast<ShaderAnimation*>(MemoryAllocate(static_cast<u32>(sizeof(ShaderAnimation)))));
            ReadShaderAnimation(made, stream);
            *animation = made;
        }
        else
        {
            stream->Skip(sizeof(u32));
            s32 layout;
            stream->ReadS32(&layout);
            s16 frames;
            stream->ReadS16(&frames);
            stream->Skip(static_cast<s32>(ShaderAnimationDataSize(static_cast<u32>(layout), static_cast<u16>(frames))));
        }
    }

    return static_cast<u32>(textureId);
}

// A material's later shaders let go: the references their images took and the settings kept beside the material. The
// entry's settings hold nothing the material needs once its bytes are gone, since no draw can reach it again
static void RemoveMaterialLater(const void* material)
{
    RemoveMaterialPassTextures(static_cast<const Platform::Graphics::HostMaterial*>(material));
}


// A material's shaders read past (shaderclasses.cpp's ReadMaterialShaders): the programs' key, the bucket, the name and each
// shader's type and bytes. Returns the first shader's texture ID, with *textured saying whether it samples one,
// *settings holding its 30 setting bytes and *animation its animation (both when they aren't nullptr; only the first
// shader's are kept, the host's draws reading that one)
u32 ReadMaterialShaders(Stream* stream, bool* textured, u8* settings, HostShaderScroll* scroll,
                        u32* shaderTextureIds, u32* shaderCountOut, bool* makesBillboard, s32* firstShaderType,
                        ShaderAnimation** animations, u32 animationSlots, u32* bucketOut,
                        u8 (*laterSettings)[Platform::Graphics::HostShaderSettings] = nullptr, u8* colourOut = nullptr,
                        HostShaderScroll* laterScrolls = nullptr)
{
    s64 activatedShaders;
    stream->ReadS64(&activatedShaders);
    s32 bucket;
    stream->ReadS32(&bucket);
    // The material's render bucket: the file's own (renderer.h's FrameBucket), which the draws turn into a bgfx view
    if (bucketOut != nullptr)
    {
        *bucketOut = static_cast<u32>(bucket);
    }
    s32 nameLength;
    stream->ReadS32(&nameLength);
    if (nameLength != 0)
    {
        stream->Skip(nameLength);
    }

    s32 shaderCount;
    stream->ReadS32(&shaderCount);
    u32 textureId = 0;
    bool firstTextured = false;
    bool billboard = false;
    s32 firstType = 0;
    for (s32 index = 0; index < shaderCount; index++)
    {
        s32 type;
        stream->ReadS32(&type);
            // The type-0x1B shader makes its model a billboard (shaderclasses.cpp's ShaderType1BMakesBillboard); any of a
        // material's shaders answers yes (resources.cpp's AnyShader over the shader vtables)
        if (type == ShaderTypeBillboard)
        {
            billboard = true;
        }

        bool shaderTextured = false;
        u8 shaderSettings[ShaderSettingFields];
        HostShaderScroll shaderScroll = {};
        ShaderAnimation** animationOut =
            animations != nullptr && static_cast<u32>(index) < animationSlots ? &animations[index] : nullptr;
        u32 shaderTextureId =
            SkipShader(stream, type, &shaderTextured, settings != nullptr ? shaderSettings : nullptr,
                       scroll != nullptr ? &shaderScroll : nullptr, animationOut,
                       index == 0 ? colourOut : nullptr);
        if (shaderTextureIds != nullptr && index < 4)
        {
            shaderTextureIds[index] = shaderTextureId;
        }
        // The settings and scroll block each shader carries: the GS's registers are the shader's, and so is the scroll
        // its modes and its animation advance (renderer.h's Shader::scroll), so a later shader that scrolls is not the
        // first one's
        if (index > 0 && static_cast<u32>(index) < Platform::Graphics::HostMaterialPasses)
        {
            if (laterSettings != nullptr)
            {
                std::memcpy(laterSettings[index - 1], shaderSettings, sizeof(shaderSettings));
            }
            if (laterScrolls != nullptr)
            {
                laterScrolls[index - 1] = shaderScroll;
            }
        }
        if (index == 0)
        {
            textureId = shaderTextureId;
            firstTextured = shaderTextured;
            firstType = type;
            if (settings != nullptr)
            {
                std::memcpy(settings, shaderSettings, sizeof(shaderSettings));
            }
            if (scroll != nullptr)
            {
                *scroll = shaderScroll;
            }
        }
    }

    if (shaderCountOut != nullptr)
    {
        *shaderCountOut = shaderCount < 0 ? 0 : static_cast<u32>(shaderCount);
    }
    if (textured != nullptr)
    {
        *textured = firstTextured;
    }
    if (makesBillboard != nullptr)
    {
        *makesBillboard = billboard;
    }
    if (firstShaderType != nullptr)
    {
        *firstShaderType = firstType;
    }
    return textureId;
}

// A retail texture's GS header (renderer.h's Texture) as the decoder's Ps2TextureHeader: the fields at the offsets its
// transfer is laid out in (widthPower 4, heightPower 6, levels 8, pixelFormat 9, uploadFormat 10, hasAlpha 11, function
// 13, level offsets 0x10, level buffer widths 0x2C, palette offset 0x48)
Ps2TextureHeader Ps2HeaderFromGs(const u8* gs)
{
    Ps2TextureHeader header = {};
    std::memcpy(&header.widthPower, gs + 4, sizeof(u16));
    std::memcpy(&header.heightPower, gs + 6, sizeof(u16));
    header.levels = gs[8];
    header.pixelFormat = gs[9];
    header.uploadFormat = gs[10];
    header.hasAlpha = gs[11];
    header.function = gs[13];
    std::memcpy(header.levelOffsets, gs + 0x10, sizeof(header.levelOffsets));
    std::memcpy(header.levelWidths, gs + 0x2C, sizeof(header.levelWidths));
    std::memcpy(&header.paletteOffset, gs + 0x48, sizeof(u32));
    return header;
}

// A particle page read like the PS2's ReadParticlePageData (ps2/renderer/particles.cpp): its texture and material IDs, the
// texture and material read from the stream when fromStream, else the material taken from the table; the modes' materials are
// the flat material, the page material's own for its mode
void ReadParticlePageData(ParticlePage* page, Stream* stream, bool fromStream, bool decals)
{
    // Retail's decals flag picks the page's shader (ps2/renderer/particles.cpp's MakePageShader(decals != 0)): the decal
    // page's strips go through the STQ program rather than the particle quad program, because a decal's corners are its
    // surface's two axes and the particle program's are a centre and an edge. The host needs no page shader at all for
    // that: decals are drawn by the decal renderer (particles_host.cpp's DrawDecals, which reads the page's texture and
    // the aging's edges itself), and the page's other materials below are the particle page's
    (void)decals;
    s32 textureId;
    stream->ReadS32(&textureId);
    s32 materialId;
    stream->ReadS32(&materialId);
    page->textureId = static_cast<u32>(textureId);
    page->materialId = static_cast<u32>(materialId);
    if (fromStream)
    {
        TextureFromStream(page->textureId, stream);
        page->material = MaterialFromStream(page->materialId, stream);
    }
    else
    {
        page->material = g_MaterialTable.Acquire(&page->materialId, nullptr);
    }

    Material* own = page->material != nullptr ? page->material->material : nullptr;
    page->materials[BlendPageMaterial] = own != nullptr ? own : Platform::Graphics::FlatMaterial();
    // The modes' own materials (the PS2's MakeModeMaterial + SetUpBlendedShader): one per blend mode through the
    // STQ particle program, each carrying the page's texture and its mode's blend preset
    Platform::Graphics::MakePageModeMaterials(page);
}
}

namespace
{
// The field rate the game's logic runs at, and whether the picture is PAL. The rate is the disc's, not the display's: this
// is the PAL release, so the game's logic steps 50 times a second, and everything the game counts per frame (the movers'
// state machines, the emitters' aging, the pickup's formula) assumes that step.
// WaitVSync paces the host's loop to it and Platform::Time steps the game's clock by exactly one field, so frameTime is
// one exact field and does not inherit the host's jitter
bool g_palDisplay = false;

// The picture's next boundary (WaitVSync) and the rate it was worked out for: a rate change restarts the schedule rather
// than waiting out an interval that belonged to the rate before it
std::chrono::steady_clock::time_point g_nextPresent;
bool g_presentScheduled = false;
s32 g_presentRate = -1;

// The port's own count of what the last second held, shown in the corner of the picture and logged beside the frame counter:
// the picture's rate (the frames the display took), the logic's (the steps the game was stepped at, which the disc fixes at
// 50), and what a frame cost -- bgfx's own timers, the CPU's between two presents and the GPU's for the frame before. The two
// rates are separate on purpose (main.cpp presents at the player's rate and steps the game at the disc's), so a picture rate
// above 50 is right and a step rate under 50 is the game missing steps; the CPU time says whether a slow picture is the
// game's own work and the GPU time whether it is the picture's
struct FrameStats
{
    std::chrono::steady_clock::time_point since;
    u32 frames;
    u32 steps;
    f32 fps;
    f32 stepsPerSecond;
    f32 cpuMs;
    f32 gpuMs;
    u32 draws;
    // The frame's packets by path, and the totals they were stepped from (the counters only climb)
    u32 rigid;
    u32 skin;
    // The frame's rigid packets and the draws of an already drawn one: their ratio is the instancing headroom
    u32 packets;
    u32 repeats;
    u32 blended;
    u32 submits;
    u32 submitsTotal;
    u32 packetsTotal;
    u32 repeatsTotal;
    u32 blendedTotal;
    u32 screen;
    u32 draw2d;
    u32 rigidTotal;
    u32 skinTotal;
    u32 screenTotal;
    u32 draw2dTotal;
    bool shown;
    bool started;
};
FrameStats g_frameStats;

// The message the HUD shows. Filled once a second from the counters, and cleared and written in one go (the text stays in
// bgfx's own buffer between presents, so writing it every frame would cost a format and a blit a frame for the same words)
void ShowFrameStats(const bgfx::Stats* stats)
{
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const u32 step = HostFrame::GetCurrentStep();
    if (!g_frameStats.started)
    {
        g_frameStats.started = true;
        g_frameStats.since = now;
        g_frameStats.steps = step;
    }

    g_frameStats.frames++;
    // What the frame handed bgfx of its own: the packet draws by path, whose total against bgfx's own count is the rest (the passes,
    // the 2D batch, the particles and the decals). The counters only climb, so the step between two calls is the last frame's
    const Platform::Graphics::HostDrawCounters& counts = Platform::Graphics::HostDrawCounts();
    g_frameStats.rigid = counts.rigidDraws - g_frameStats.rigidTotal;
    g_frameStats.packets = counts.rigidPackets - g_frameStats.packetsTotal;
    g_frameStats.repeats = counts.rigidRepeats - g_frameStats.repeatsTotal;
    g_frameStats.blended = counts.rigidBlended - g_frameStats.blendedTotal;
    g_frameStats.submits = counts.rigidSubmits - g_frameStats.submitsTotal;
    g_frameStats.submitsTotal = counts.rigidSubmits;
    g_frameStats.packetsTotal = counts.rigidPackets;
    g_frameStats.repeatsTotal = counts.rigidRepeats;
    g_frameStats.blendedTotal = counts.rigidBlended;
    g_frameStats.skin = counts.skinDraws - g_frameStats.skinTotal;
    g_frameStats.screen = counts.screenDraws - g_frameStats.screenTotal;
    g_frameStats.draw2d = counts.draw2dDraws - g_frameStats.draw2dTotal;
    g_frameStats.rigidTotal = counts.rigidDraws;
    g_frameStats.skinTotal = counts.skinDraws;
    g_frameStats.screenTotal = counts.screenDraws;
    g_frameStats.draw2dTotal = counts.draw2dDraws;
    const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_frameStats.since).count();
    if (elapsed < 1000)
    {
        return;
    }

    const f32 seconds = static_cast<f32>(elapsed) / 1000.0f;
    g_frameStats.fps = static_cast<f32>(g_frameStats.frames) / seconds;
    g_frameStats.stepsPerSecond = static_cast<f32>(step - g_frameStats.steps) / seconds;
    // bgfx's timers are timestamps at their own frequencies; a backend that keeps no timer answers 0 rather than a rate
    g_frameStats.cpuMs = stats->cpuTimerFreq > 0
                             ? static_cast<f32>(stats->cpuTimeFrame) * 1000.0f / static_cast<f32>(stats->cpuTimerFreq)
                             : 0.0f;
    g_frameStats.gpuMs = stats->gpuTimerFreq > 0 && stats->gpuTimeEnd > stats->gpuTimeBegin
                             ? static_cast<f32>(stats->gpuTimeEnd - stats->gpuTimeBegin) * 1000.0f /
                                   static_cast<f32>(stats->gpuTimerFreq)
                             : 0.0f;
    g_frameStats.draws = stats->numDraw;

    // The same numbers once a second in the log, for a run that is read back rather than watched (and for a report): a normal
    // run prints nothing, so the line is behind its own flag (TWINSANITY_FRAME_LOG=1), apart from the picture's (TWINSANITY_FPS)
    // The packet draws named, and whatever the total has left: the passes and the batched paths, which is where a draw count worth
    // cutting would show up
    static const bool logged = [] {
        const char* value = std::getenv("TWINSANITY_FRAME_LOG");
        return value != nullptr && std::strcmp(value, "0") != 0;
    }();
    if (logged)
    {
        const u32 packets = g_frameStats.submits + g_frameStats.skin + g_frameStats.screen + g_frameStats.draw2d;
        const u32 other = g_frameStats.draws > packets ? g_frameStats.draws - packets : 0;
        std::fprintf(stderr,
                     "host frame %u: fps %.1f steps %.0f cpu %.2f ms gpu %.2f ms draws %u (rigid %u skin %u screen %u 2d %u "
                     "passes %u) packets %u repeats %u blended %u submits %u views %u tris %d textures %d programs %d\n",
                     g_frameCounter, g_frameStats.fps, g_frameStats.stepsPerSecond, g_frameStats.cpuMs, g_frameStats.gpuMs,
                     g_frameStats.draws, g_frameStats.rigid, g_frameStats.skin, g_frameStats.screen, g_frameStats.draw2d, other,
                     g_frameStats.packets, g_frameStats.repeats, g_frameStats.blended, g_frameStats.submits, stats->numViews,
                     stats->numPrims[bgfx::Topology::TriList], stats->numTextures, stats->numPrograms);
    }

    g_frameStats.frames = 0;
    g_frameStats.steps = step;
    g_frameStats.since = now;

    if (g_frameStats.shown)
    {
        bgfx::dbgTextClear();
        bgfx::dbgTextPrintf(1, 0, 0x0f, "%.1f fps  %.0f steps/s  cpu %.2f ms  gpu %.2f ms  %u draws", g_frameStats.fps,
                            g_frameStats.stepsPerSecond, g_frameStats.cpuMs, g_frameStats.gpuMs, g_frameStats.draws);
    }
}
}

void Platform::Graphics::WaitVSync()
{
    // Retail's frame IS the vblank: the PS2 build waits for one here (ps2/graphics.cpp's sceGsSyncV) and the game's
    // per-frame work is written for that step. The host's picture has its own rate instead (settings.txt's `render`), so
    // this waits the picture's turn out, and nothing else: the game's clock moves on its own step (main.cpp takes it when
    // Platform::Time::LogicStepDue, the wall clock's 50 a second, says it is owed). A frame that overruns starts the next
    // one from now instead of being paid back in a burst (retail drops the time too: its timer keeps running)
    constexpr long long NanosecondsPerSecond = 1000000000ll;
    // The picture's rate, which the options menu changes while the game runs: Platform::Time owns it, and 0 there is unlimited
    // (nothing to wait for: the loop presents as fast as it draws)
    const s32 pictureRate = Platform::Time::DisplayRate();
    if (pictureRate <= 0)
    {
        return;
    }

    const long long step = NanosecondsPerSecond / pictureRate;
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (!g_presentScheduled || pictureRate != g_presentRate)
    {
        // The first present, or the first at a rate the row just changed to: the schedule starts from now
        g_presentScheduled = true;
        g_presentRate = pictureRate;
        g_nextPresent = now;
    }

    g_nextPresent += std::chrono::nanoseconds(step);
    std::chrono::steady_clock::time_point& next = g_nextPresent;
    if (next > now)
    {
        // SDL's precise wait: at 60 Hz or above a plain sleep wakes late by a millisecond or more, and that lateness is
        // exactly the frameTime jitter the fixed step is here to remove. Sleep the bulk of the wait, then let SDL spin
        // the last millisecond and a half
        constexpr long long SleepLimit = 1500000ll;
        long long remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(next - now).count();
        if (remaining > SleepLimit)
        {
            std::this_thread::sleep_for(std::chrono::nanoseconds(remaining - SleepLimit));
            remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(next - std::chrono::steady_clock::now()).count();
        }

        SDL_DelayPrecise(static_cast<Uint64>(remaining > 0 ? remaining : 0));
    }
    else
    {
        next = now;
    }

    // The picture's boundary only. The game's clock has its own step (main.cpp takes it when Platform::Time::LogicStepDue
    // says the game is owed one), so presenting faster than the game runs draws the frames between its steps without
    // stepping it, and presenting slower cannot make it run faster
}

// The camera's reach: the settings file's `draw_distance` line (the options screen's pop-in distance), 1 when it has none
f32 Platform::Graphics::ViewDistanceScale()
{
    const f32 scale = HostSettingsDrawDistance();
    return scale > 0.0f ? scale : 1.0f;
}

bool Platform::Graphics::IsPalDisplay()
{
    return g_palDisplay;
}

// The game calls this before it reads back what was drawn (the movies' transfer, the context's teardown) and retail
// waits for the GS and its DMA to drain here. bgfx has no chain to drain: a view's work is submitted by bgfx::frame and
// the queue is the driver's, so there is nothing for the host to wait on
void Platform::Graphics::WaitIdle()
{
}

// The view a material's draws go in: its retail bucket's (draws_host.cpp's HostViewForBucket), so a blended material
// lands in the blended bucket it belongs to, after the opaque ones and after the fog, rather than in the opaque bucket
// in submission order, which drew a transparent surface wherever its object happened to be placed
bgfx::ViewId ViewOf(const Platform::Graphics::HostMaterial* material)
{
    return Platform::Graphics::HostViewForBucket(material != nullptr ? material->bucket : 0);
}

void Platform::Graphics::StartRenderer(s32 height, bool pal)
{
    // The logic's field rate is the disc's: this is the PAL release, so 50, and the game was written for the console's
    // vblank rather than for whatever the host's display does. Deriving it from the render rate (as this did, at a flat 60)
    // ran the game 20% fast against retail: every per-frame count in the game - the movers, the emitters' aging, the
    // pickups' spin, the walk cycle's step - came round 1.2x as often as on the console. The picture keeps its PAL
    // offsets and tiling either way, so only the rate follows the region
    g_palDisplay = pal;
    // The game's logic runs at the disc's field rate (50 on this PAL disc), and the picture is presented at the player's
    // chosen rate (settings.txt's `render`, 50 when it has none). They are separate on purpose: the logic must not follow
    // the display, or the game runs 1.2x fast on a 60 Hz monitor, which is what a flat 60 did to every per-frame count the
    // game has. Below the field rate the loop could not owe a step every display frame, so the rate is raised to it (the
    // picture would otherwise starve the fixed step); a sanity ceiling keeps a typo from spinning the loop
    constexpr s32 MaxRenderRate = 1000;
    constexpr s32 UnlimitedRenderRate = 0;
    const s32 logicRate = pal ? 50 : 60;
    // 0 is unlimited: the loop presents as fast as it can draw, and vsync (when it is on) is what paces it
    s32 displayRate = HostSettingsRenderRate();
    if (displayRate == UnlimitedRenderRate || MaxRenderRate < displayRate)
    {
        displayRate = displayRate == UnlimitedRenderRate ? UnlimitedRenderRate : MaxRenderRate;
    }
    else if (displayRate < logicRate)
    {
        // Below the field rate the loop could not owe a step every display frame, so the rate is raised to it
        displayRate = logicRate;
    }

    Platform::Time::SetRates(logicRate, displayRate);
    if (displayRate == UnlimitedRenderRate)
    {
        std::fprintf(stderr, "sdl3: %d Hz logic, unlimited display%s\n", Platform::Time::FrameRate(),
                     pal ? " (PAL picture)" : "");
    }
    else
    {
        std::fprintf(stderr, "sdl3: %d Hz logic, %d Hz display%s\n", Platform::Time::FrameRate(),
                     Platform::Time::DisplayRate(), pal ? " (PAL picture)" : "");
    }

    // Retail's InitDisplay sets G_HorizontalResolution/VerticalResolution (the game's g_DisplayWidth/Height) to the
    // frame's 512-pixel width and the mode's height; the host game draws into a 960x540 scene, so they must follow
    // the render target's display size or the movie's on-screen area (and the 2D particles' aspect) collapses to
    // zero. StartFrame's screen size reaches them once the renderer's target exists; this covers the calls before it
    g_DisplayWidth = static_cast<u32>(g_ScreenWidth);
    g_DisplayHeight = height > 0 ? static_cast<u32>(height) : static_cast<u32>(g_ScreenHeight);
}

// The tail of the renderer's start-up: retail sets up the VU1 programs, the DMA chains and their buckets, the texture
// slots, the instance blocks, the default materials, the alpha presets and the screen effects here. The host makes each
// of those where it is first needed (bgfx allocates the samplers and the programs on their first draw), so the start-up
// has no second half to finish
void Platform::Graphics::FinishRendererStart()
{
}

void Platform::Graphics::MoveDisplay(const Vector2* offset)
{
    // The PS2 moves its display window out of the frame buffer, which slides the finished picture on the TV: by up to
    // 256 of the display's units across and 32 lines down at the offset's ends (src/platform/ps2/graphics.cpp's
    // DisplayMoveAcross and DisplayMoveDown, out of the 512 by 448 frame it sets the display up for). The host has no
    // display window: the picture itself moves instead, the same fraction of the frame, at the composite of the scene
    // over the backbuffer (draws_host.cpp's HostCompositeScene). MoveScreen clamps the offset to -1 and 1 before it
    // gets here
    constexpr f32 Ps2FrameWidth = 512.0f;
    constexpr f32 Ps2FrameHeight = 448.0f;
    HostSetDisplayOffset(offset->x * (256.0f / Ps2FrameWidth), offset->y * (32.0f / Ps2FrameHeight));
}

// A scrolling shader's mode (renderer.h's ShaderScroll): none, the animation's own UV offset, a phase going round
// 0 to 1, its sine, its cosine. The host reproduces the two phase modes and the animation's offset; the animated
// colour rides the same animation path
constexpr u32 HostScrollNone = 0;
constexpr u32 HostScrollAnimated = 1;
constexpr u32 HostScrollWrapped = 2;
constexpr u32 HostScrollSine = 3;
constexpr u32 HostScrollCosine = 4;

namespace
{
// A phase going round 0 to 1 (shadertypes.cpp's Wrapped, exactly: the phase advances, the whole turns come off and
// the phase's fraction is the offset). The returned value is unwrapped for the sine's sake, the phase itself wrapped
f32 ScrollWrapped(f32* phase, f32 speed, f32 seconds)
{
    f32 value = *phase + speed * seconds;
    *phase = value;
    s32 whole = TruncateToInt(value);
    if (value < static_cast<f32>(whole))
    {
        whole--;
    }

    *phase -= static_cast<f32>(whole);
    return *phase;
}

// The sine or cosine of the phase's turn (shadertypes.cpp's Waved): the phase (unwrapped) advances and the
// trigonometric value is the offset
f32 ScrollWaved(f32* phase, f32 speed, f32 seconds, bool cosine)
{
    *phase += speed * seconds;
    s32 angle;
    AngleFrom(&angle, *phase * TwoPi, AngleRadians);
    return cosine ? CosOfAngle(&angle) : SinOfAngle(&angle);
}
}

// One shader's scroll moved on by the frame's seconds, exactly as the retail shadertypes.cpp's UpdateShader does it: the
// two scrolls by their modes, then the animation's own UV offset for the animated modes. The shader's settings, phases,
// speeds and offsets are its own -- every shader of a material scrolls independently -- so they are passed in
void StepShaderScroll(const u8* settings, f32* phases, f32* speeds, f32* scroll, const ShaderAnimation* animation,
                      f32 seconds)
{
    const u32 uScroll = settings[Platform::Graphics::HostShaderSettingUScroll];
    const u32 vScroll = settings[Platform::Graphics::HostShaderSettingVScroll];
    switch (uScroll)
    {
    case HostScrollWrapped:
        scroll[0] = ScrollWrapped(&phases[0], speeds[0], seconds);
        break;
    case HostScrollSine:
        scroll[0] = ScrollWaved(&phases[0], speeds[0], seconds, false);
        break;
    case HostScrollCosine:
        // Retail's own bug, kept: the cosine mode advances the U phase and writes V (shadertypes.cpp's UpdateShader
        // has `shader->scroll[1] = Waved(&shader->scrollPhases[0], ...)`), so a material using it looks the same
        scroll[1] = ScrollWaved(&phases[0], speeds[0], seconds, true);
        break;
    default:
        break;
    }

    switch (vScroll)
    {
    case HostScrollWrapped:
        scroll[1] = ScrollWrapped(&phases[1], speeds[1], seconds);
        break;
    case HostScrollSine:
        scroll[1] = ScrollWaved(&phases[1], speeds[1], seconds, false);
        break;
    case HostScrollCosine:
        scroll[1] = ScrollWaved(&phases[1], speeds[1], seconds, true);
        break;
    default:
        break;
    }

    if (animation == nullptr)
    {
        return;
    }

    if (uScroll == HostScrollAnimated)
    {
        scroll[0] = animation->uvOffset.x;
    }

    if (vScroll == HostScrollAnimated)
    {
        scroll[1] = animation->uvOffset.y;
    }
}

// A material's every shader's scroll and animation for this frame's seconds (the retail AnimateMaterials -> AnimateMaterial
// -> UpdateShader path): each shader has its own scroll block, its own modes and its own animation, so a material whose
// second shader scrolls moves its second pass's UVs. Shader 0's colour is the one the draws tint with (renderer.h's
// Shader::shaderColour), which is the first shader's own
void StepMaterialScroll(Platform::Graphics::HostMaterial* material, f32 seconds)
{
    if (material == nullptr || (material->flags & Platform::Graphics::HostMaterialSettings) == 0)
    {
        return;
    }

    HostMaterialAnimation* state = FindMaterialAnimation(material);
    HostMaterialLater* later = FindMaterialLater(material);
    const u32 shaders = material->passCount != 0 ? material->passCount : 1;
    for (u32 shader = 0; shader < shaders; shader++)
    {
        ShaderAnimation* animation =
            state != nullptr && shader < HostMaterialShaderSlots ? state->animations[shader] : nullptr;
        // The animation moves on before its UV offset is read, as the retail UpdateShader reads it after advancing
        if (animation != nullptr)
        {
            AdvanceShaderAnimation(animation, seconds);
        }

        const u8* settings = material->shaderSettings;
        f32* phases = material->scrollPhases;
        f32* speeds = material->scrollSpeeds;
        f32* scroll = material->scroll;
        if (shader != 0)
        {
            if (later == nullptr || shader - 1 >= Platform::Graphics::HostMaterialPasses - 1)
            {
                continue;
            }

            settings = later->settings[shader - 1];
            phases = later->scroll[shader - 1].phases;
            speeds = later->scroll[shader - 1].speeds;
            scroll = later->scroll[shader - 1].scroll;
        }

        StepShaderScroll(settings, phases, speeds, scroll, animation, seconds);

        // The animated colour is the first shader's (renderer.h's Shader::shaderColour, which the type-0x1F draw tints with)
        if (shader != 0 || animation == nullptr || settings[Platform::Graphics::HostShaderSettingAnimatedColour] == 0)
        {
            continue;
        }

        // shadertypes.cpp's UpdateShader's ColourScale and AlphaScale: the colour's channels and its alpha in the scales
        // the game's colour byte is made from
        constexpr f32 HostAnimationColourScale = 256.0f;
        constexpr f32 HostAnimationAlphaScale = 127.0f;
        state->shaderColour[0] = animation->colour.x * HostAnimationColourScale;
        state->shaderColour[1] = animation->colour.y * HostAnimationColourScale;
        state->shaderColour[2] = animation->colour.z * HostAnimationColourScale;
        state->shaderColour[3] = animation->colour.w * HostAnimationAlphaScale;
        // The colour the draw tints with (renderer.h's Shader::colour): the animation's own, over the file's static one.
        // Three materials whose every other byte is equal -- one model drawn in several colours -- differ only here, so
        // leaving it unused drew them all in the file's default colour
        for (u32 channel = 0; channel < 3; channel++)
        {
            f32 value = state->shaderColour[channel];
            value = value < 0.0f ? 0.0f : (value > 255.0f ? 255.0f : value);
            material->colour[channel] = static_cast<u8>(value + 0.5f);
        }
    }
}

// The type-3 shader's update does nothing (shadertypes.cpp's ShaderType03Update); every other type's update is the
// base one above, the 0x1C wave included (ShaderType1CUpdate ends in the base UpdateShader)
constexpr s32 ShaderTypeNoUpdate = 3;

// A material's first shader moved on by its type (AnimateMaterial's ShaderUpdateSlot over the shader's vtable)
void StepShaderUpdate(Platform::Graphics::HostMaterial* material, f32 seconds)
{
    switch (material->shaderType)
    {
    case ShaderTypeNoUpdate:
        break;
    default:
        StepMaterialScroll(material, seconds);
        break;
    }
}

void Platform::Graphics::StepAnimations(const TimeClock* clock)
{
    if (clock == nullptr)
    {
        return;
    }
    // The retail StepAnimations: every material's shaders moved on by the clock's last advance (ps2/graphics.cpp's
    // AnimateMaterials -> AnimateMaterial -> ShaderUpdateSlot) and the particles' own wave shader moved on with them
    // (UpdateParticleWaves). The live table's materials are walked; a material a draw has not touched yet simply
    // starts its phase here
    f32 seconds = static_cast<f32>(static_cast<s32>(clock->advance)) * g_SecondsPerClockUnit;
    if (seconds == 0.0f)
    {
        return;
    }
    if (g_MaterialTable.entries != nullptr)
    {
        for (u32 index = 0; index < g_MaterialTable.count; index++)
        {
            MaterialResource* resource = g_MaterialTable.entries[index].item;
            if (resource == nullptr || resource->material == nullptr)
            {
                continue;
            }
            StepShaderUpdate(reinterpret_cast<Platform::Graphics::HostMaterial*>(resource->material), seconds);
        }
    }

    // The particles' wave shader, moved on every frame whatever the materials are (the retail UpdateParticleWaves):
    // the wave is the particles' own, not a material's, so it is not in the table above
    Platform::Graphics::HostParticleWaveUpdate(seconds);
}

void Platform::Graphics::SetUpRenderTarget(RenderTargetDescription* target)
{
    // The game's clip-to-screen matrix (src/platform/ps2/renderer/frame.cpp, FUN_0019f720's): the view's toScreen is
    // this times the camera's toClip, so leaving it zero collapses every projected vertex to the origin. The frame's
    // corner is the screen's middle less half its size (in the display's pixels) plus the frame's offset, and the
    // matrix takes clip's -1..1 to that corner + 0..width and to the GS's depth range
    if (target == nullptr)
    {
        return;
    }

    constexpr f32 ScreenMiddle = 2048.0f;
    constexpr f32 Subpixels = 16.0f;
    constexpr f32 HalfSubpixels = 8.0f;
    constexpr f32 DepthRange = 8388607.0f;

    f32 left = ScreenMiddle - static_cast<f32>(target->displayWidth) * 0.5f + static_cast<f32>(target->offsetX);
    f32 top = ScreenMiddle - static_cast<f32>(target->displayHeight) * 0.5f + static_cast<f32>(target->offsetY);
    f32 width = static_cast<f32>(target->width);
    f32 height = static_cast<f32>(target->height);

    Matrix4x4* matrix = &target->clipToScreen;
    InitIdentityMatrix(matrix);
    matrix->m[0][0] = width * HalfSubpixels;
    matrix->m[1][1] = -height * HalfSubpixels;
    matrix->m[2][2] = -DepthRange;
    matrix->m[3][0] = (left + width * 0.5f) * Subpixels;
    matrix->m[3][1] = (top + height * 0.5f) * Subpixels;
    matrix->m[3][2] = DepthRange;
    target->x = static_cast<s32>(left);
    target->y = static_cast<s32>(top);
}

// Retail adds the frame's DRAWING and DRAWINGB commands to the chain it is handed and sends the two buffers round;
// bgfx::frame submits the views that were set up this frame and does its own double buffering, so the chain is not read
void Platform::Graphics::Present(const void* commands)
{
    (void)commands;
}

// Retail's presentation from an interrupt, with the same chain and the same rounding of the two buffers. The host has no
// interrupt path: this is Present's, reachable from the same call sites
void Platform::Graphics::PresentFromInterrupt(const void* commands)
{
    (void)commands;
}

// Retail waits for what it sent to the GS to be taken here. bgfx keeps no such notion of "sent but not yet drawn": the
// frame is submitted whole, so there is nothing to wait for between the two
void Platform::Graphics::WaitSent()
{
}

void Platform::Graphics::ResetBuckets(bool movie)
{
    (void)movie;
    // The frame's memory is the PS2's DMA chain: the bump arena the frame's one-off allocations came from is let go
    // with every new frame (the pointers stay valid until the frame is drawn)
    ResetFrameMemory();
}

void Platform::Graphics::SubmitBuckets(bool fromInterrupt)
{
    (void)fromInterrupt;
    // The frame is drawn, but its memory is not let go here: the display frames between two game steps redraw the same
    // step's state, and what the step's own frame made in frame memory (the instances' joint matrices above all) has to
    // outlive the present for them to draw it again. BeginLogicFrame starts the memory over at the step after
}

void Platform::Graphics::BeginLogicFrame()
{
    // A game step's own frame begins: everything the one before it allocated is done with (its display frames have all
    // presented by now), so the frame's memory starts over here rather than under the first present
    ResetFrameMemory();
    // The options the player changed are written back here, on the game thread: the audio callback must not read the
    // game's own state (which the game thread writes) or touch the filesystem
    HostSettingsTick();
}

void* Platform::Graphics::AllocFrameMemory(u32 count, u32 size)
{
    return AllocFrameMemoryArena(static_cast<size_t>(count) * size);
}

void Platform::Graphics::FinishFrame()
{
    // The window's drawable against the swap chain's, once a frame: a window the player resized (or a window manager's mode
    // change) is followed before the frame is composited into it
    DisplaySync();
    // The overlay's quads held for batching are drawn out now: this is the frame they were queued in, and the next frame's
    // StartFrame would draw them into its own view (a frame late: a fade's first frame, or the instant black a movie or a
    // load shows, would leave the scene under it showing for that frame)
    // The rigid draws still held are the frame's last: submitted before the present, where the next frame's first draw would
    // otherwise be their neighbour
    Platform::Graphics::HostFlushRigidInstances();
    HostFlushDraw2D();
    // Present the frame: every bucket view submitted since StartFrame renders now
    g_frameCounter++;

    // The whole frame drew into the offscreen scene target (the PS2 draws everything into one frame buffer): copy
    // it over the backbuffer now, so the window shows the finished picture
    HostCompositeScene();

    // The port's own HUD and its log line, before the present it belongs to: bgfx draws the debug text with the frame it was
    // written for, over the backbuffer the composite just filled. The corner text is the debug flag's, not the picture's -- a
    // release shows the game alone -- so it appears only when TWINSANITY_FPS is set to something other than 0
    // (TWINSANITY_FPS=1); the once-a-second log line stays either way
    static const bool shown = [] {
        const char* value = std::getenv("TWINSANITY_FPS");
        return value != nullptr && std::strcmp(value, "0") != 0;
    }();
    g_frameStats.shown = shown;
    ShowFrameStats(bgfx::getStats());

    bgfx::frame();
    // The uploads of packets whose model died: the frame that could still bind them is submitted and run, so their
    // bgfx buffers can be let go now (never immediately: that was why the cache kept every entry for the session)
    HostCollectRetiredDrawBuffers();

    // A run the pause menu's close (or the window's close) asked to end ends here, between two frames: this frame is presented
    // and the uploads it held are let go, so the renderer's thread has nothing left of ours when it is shut down
    if (Platform::System::ExitRequested())
    {
        Platform::System::EndRun();
    }
}

u32 Platform::Graphics::HostFrameCounter()
{
    // The frame last presented, not the one the next present takes
    return g_frameCounter != 0 ? g_frameCounter - 1 : 0;
}

void Platform::Graphics::StartFrame(const FrameStart& frame)
{
    HostSetScreenSize(static_cast<u32>(frame.width), static_cast<u32>(frame.height));
    // The game's screen coordinates (the particles' shapes and the to-screen matrices) are measured against the
    // display, which the frame start names (the render target's screen size)
    HostSetDisplaySize(static_cast<u32>(frame.screenWidth), static_cast<u32>(frame.screenHeight));
    // Retail's G_HorizontalResolution/VerticalResolution follow the display the same way InitDisplay set them, so
    // the game's own readers of g_DisplayWidth/Height (the movie's area, the 2D particles' aspect) see the target
    if (frame.screenWidth != 0 && frame.screenHeight != 0)
    {
        g_DisplayWidth = static_cast<u32>(frame.screenWidth);
        g_DisplayHeight = static_cast<u32>(frame.screenHeight);
    }

    // bgfx's clear colour is 0xRRGGBBAA (bgfx_p.h's Clear::set takes index[0] = value >> 24 as red, and the GL
    // renderer hands index[0..3] straight to glClearColor(r, g, b, a)): the game's red, green and blue go in the top
    // three bytes and opaque alpha (the frame's, the shadow pass copies) in the low one
    const u32 rgba = static_cast<u32>(frame.red) << 24 | static_cast<u32>(frame.green) << 16 |
                     static_cast<u32>(frame.blue) << 8 | 0xFFu;
    // The scene draws into the offscreen target, not the backbuffer: the shadow pass needs the frame's depth and
    // alpha (the PS2 copies both into the half-size buffer, shadows.cpp's StartShadows), which a backbuffer cannot
    // give. FinishFrame composites it back. The clear is unconditional: the target must have a defined colour and a
    // fresh depth where the PS2 leans on its frame buffer's leftovers
    HostSceneSetView(HostOpaqueView);
    bgfx::setViewClear(HostOpaqueView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, rgba, 1.0f, 0);
}

// Retail picks which of the VU1 programs are resident before a stage that needs them (the culling set, the decal set, the
// standard set) and can wait for a microprogram to finish before swapping it out. bgfx programs are not resident in that
// sense and are never overwritten, so a stage's program set needs no announcement
void Platform::Graphics::UseHelperPrograms(u32 set, bool wait)
{
    (void)set;
    (void)wait;
}

// The chunks' culling (SetViewFrustum, ViewPlanes, LoadChunkPlanes, LoadPortalPlanes, LoadCullingView, CullLoadLink,
// CullLoadModel, CullTestBox, CullTestBoxAt, CullResult, CullMatrices, ClipBox and ClipBoxAt) lives in culling.cpp

namespace
{
// The host's 3D draws take the view's CLIP matrix, not its screen one: the PS2 bakes toScreen (clip through the
// GS's clip-to-screen matrix) into its VU1 packets because it hardcodes GS coordinates, but bgfx is fed clip space
// and does the divide and viewport itself, so toScreen's thousands-of-pixels coordinates would land far outside
// NDC (garbled geometry). toClip is the same camera math one step earlier; pushes it before every 3D draw so the
// draws follow the camera instead of the identity the matrix starts as
void HostSyncViewMatrix()
{
    if (g_RenderView != nullptr)
    {
        Platform::Graphics::HostSetViewMatrix(&g_RenderView->toClip);
    }
}

// The write mask a sample that FAILS the alpha test gets (the GS's TEST.AFAIL): 0 KEEP writes nothing (the shader
// has dropped it already), 1 FB_ONLY writes the colour and leaves the depth, 2 ZB_ONLY writes the depth alone and
// 3 RGB_ONLY writes the colour without its alpha. Returns false for KEEP, which needs no second pass
static bool FailedSampleState(const Platform::Graphics::HostMaterialState& passed, u64* outState,
                             f32* outReference)
{
    const u64 colour = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
    u64 write = 0;
    const u32 action = static_cast<u32>(passed.alphaReference[3] + 0.5f);
    switch (action)
    {
    case 1: write = colour; break;                                       // FB_ONLY: no depth
    case 2: write = BGFX_STATE_WRITE_Z; break;                           // ZB_ONLY: depth alone
    case 3: write = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_Z; break;    // RGB_ONLY: no alpha
    default: return false;
    }
    *outState = (passed.state & ~(colour | BGFX_STATE_WRITE_Z)) | write;
    *outReference = passed.alphaReference[0];
    return true;
}

// A material's later shader as the state its pass draws with: the same mapping as the first shader's, over that
// shader's own 30 setting bytes (its blend enable and preset, its depth test and write, and its alpha test with its
// AFAIL), which ReadMaterial keeps beside the material (host_graphics.h's passCount)
static Platform::Graphics::HostMaterialState LaterPassStateOf(const Platform::Graphics::HostMaterial* material,
                                                              u32 pass, u64 baseState)
{
    Platform::Graphics::HostMaterial copy = *material;
    MaterialLaterSettingsOf(material, pass, copy.shaderSettings);
    return HostMaterialStateOf(&copy, baseState, true);
}

// The program a pass submits with: the flat-shaded one when its shader cleared the GS's IIP bit, else nothing, which leaves
// the draw's own program (the rigid or skin one) in place. The stale handle is built rather than written as
// BGFX_INVALID_HANDLE, which is a braced initializer and so cannot stand in a conditional expression
static bgfx::ProgramHandle FlatProgramOr(bool flat, bgfx::ProgramHandle flatProgram)
{
    return flat ? flatProgram : bgfx::ProgramHandle{bgfx::kInvalidHandle};
}

// The scroll a pass's draw takes: that pass's own shader's offset, which is the material's own for the first shader (every
// later shader's block lives in the HostMaterialLater table, which only this file keeps)
static void SetPassScroll(const Platform::Graphics::HostMaterial* material, u32 shader)
{
    f32 scroll[2] = {0.0f, 0.0f};
    if (material != nullptr)
    {
        scroll[0] = material->scroll[0];
        scroll[1] = material->scroll[1];
        Platform::Graphics::HostMaterialShaderScroll(material, shader, scroll);
    }

    Platform::Graphics::HostSetDrawScroll(material, scroll);
}

// A submodel's draw as the passes it makes: each of its material's shaders in turn (RenderMaterial's WriteShaders writes
// every one of them over the same geometry, materials.cpp:94), and after each shader that asks for it the pass over the
// samples its alpha test rejected. Every model path draws that list in that order, so a rigid model, a skin, a blend skin
// and the scenery cannot disagree about what a material means
struct MaterialDrawPasses
{
    // Four shaders is the retail reader's most (renderer.h's MaxMaterialShaders), and each of them may add one pass over
    // its rejected samples
    Platform::Graphics::HostMaterialState states[2 * Platform::Graphics::HostMaterialPasses];
    // The shader each entry draws the material's texture of: 0 for the first shader's passes, 1..3 for a later one's,
    // which samples its own image (LaterPassTexture)
    u8 shaders[2 * Platform::Graphics::HostMaterialPasses];
    u32 count;
};

MaterialDrawPasses MaterialDrawPassesOf(const Platform::Graphics::HostMaterial* material, u64 baseState)
{
    MaterialDrawPasses passes = {};
    const u32 shaders = material != nullptr && material->passCount != 0 ? material->passCount : 1;
    for (u32 shader = 0; shader < shaders; shader++)
    {
        const Platform::Graphics::HostMaterialState state =
            shader == 0 ? HostMaterialStateOf(material, baseState, true) : LaterPassStateOf(material, shader - 1, baseState);
        passes.states[passes.count] = state;
        passes.shaders[passes.count] = static_cast<u8>(shader);
        passes.count++;

        // The samples the shader's alpha test rejected: the fragment shader drops them, which is the GS's KEEP, but the
        // other three failure actions (FB_ONLY, ZB_ONLY, RGB_ONLY) write the sample with that mask. AFAIL is a field of
        // the TEST register the shader itself sends, so every shader can ask for it, not just the first
        u64 failedState = 0;
        f32 failedReference = 0.0f;
        if (state.alphaReference[0] != 1.0f && FailedSampleState(state, &failedState, &failedReference))
        {
            Platform::Graphics::HostMaterialState failed = state;
            failed.state = failedState;
            failed.alphaReference[2] = 1.0f;
            failed.alphaReference[0] = failedReference;
            passes.states[passes.count] = failed;
            passes.shaders[passes.count] = static_cast<u8>(shader);
            passes.count++;
        }
    }

    return passes;
}

// A later shader's texture: the console uploads each later shader's own image before its packet (materials.cpp's
// WriteShaders, TextureOf(shader)), so pass k samples shader k's picture. A material whose later shader has no image
// samples the first shader's, which is what its packet left bound on the console
bgfx::TextureHandle LaterPassTexture(const Platform::Graphics::HostMaterial* material, u32 pass,
                                     bgfx::TextureHandle first)
{
    GameTexture* texture = MaterialPassTexture(material, pass);
    if (texture == nullptr)
    {
        return first;
    }

    const bgfx::TextureHandle handle = Platform::Graphics::HostTextures().Find(texture);
    return bgfx::isValid(handle) ? handle : first;
}

// The retail Billboard (src/platform/ps2/renderer/models.cpp): a model whose material makes it a billboard has its own
// rotation replaced by the side/up/back of a frame facing the camera, worked out in the model's own space. worldInChunk
// is the model's matrix in the chunk's space (the view's ChunkMatrixSlot), chunkDrawInverse the chunk's draw matrix's
// inverse (ChunkView::DrawInverseMatrix) and the camera the render view's place. Returns false when they're missing
bool HostBillboardTurn(const Matrix4x4* worldInChunk, const Matrix4x4* chunkDrawInverse, const RenderView* view,
                       Matrix4x4* turn)
{
    if (worldInChunk == nullptr || chunkDrawInverse == nullptr || view == nullptr)
    {
        return false;
    }

    Reference* reference = view->cameraObject;
    ReferencedObject* camera = reference != nullptr ? reference->object : nullptr;
    ObjectPlace* place = camera != nullptr ? camera->place : nullptr;
    if (place == nullptr)
    {
        return false;
    }

    place->SyncPosition();
    Vector4 cameraPosition = place->position;
    // The camera into the chunk's space and then into the model's own (the same two steps the retail Billboard takes)
    VuTransformPoint(chunkDrawInverse, &cameraPosition, &cameraPosition);
    Matrix4x4 inverse = *worldInChunk;
    VuInvertRigidInPlace(&inverse);
    VuTransformPoint(&inverse, &cameraPosition, &cameraPosition);

    constexpr f32 BillboardEpsilon = 0x1.5798ECp-29f;
    const Vector4 tiltedUp = {-0x1.9906CCp-5f, 0x1.FF5C28p-1f, 0.0f, 0.0f};
    const Vector4 direction = {-cameraPosition.x, -cameraPosition.y, -cameraPosition.z, 1.0f};
    Vector4 up = {0.0f, 1.0f, 0.0f, 0.0f};
    if (SquaredDistance(&up, &direction) <= DotProduct(&up, &direction) * BillboardEpsilon)
    {
        up = tiltedUp;
    }

    Vector4 side = {up.y * direction.z - up.z * direction.y, up.z * direction.x - up.x * direction.z,
                    up.x * direction.y - up.y * direction.x, 1.0f};
    f32 scale = InverseLength(&side, BillboardEpsilon);
    side.x *= scale;
    side.y *= scale;
    side.z *= scale;
    Vector4 back = {up.y * side.z - up.z * side.y, up.z * side.x - up.x * side.z, up.x * side.y - up.y * side.x, 1.0f};
    scale = InverseLength(&back, BillboardEpsilon);
    back.x *= scale;
    back.y *= scale;
    back.z *= scale;

    InitIdentityMatrix(turn);
    // Only the x, y and z of each row, which is what retail's SetRow copies: the fourth column stays the identity's zero.
    // Copying side/up/back's own w (1) into it made every vertex's w the sum of its x, y and z plus one, so the quad's
    // perspective divide ran away with it and each billboard stretched out of its place and across the sky
    for (u32 column = 0; column < 3; column++)
    {
        turn->m[0][column] = (&side.x)[column];
        turn->m[1][column] = (&up.x)[column];
        turn->m[2][column] = (&back.x)[column];
    }

    return true;
}

}

// The proof's way into the pass list and the later-shader data (host_graphics.h's note): a material built by hand, with the
// settings and scrolls ReadMaterial would have kept beside it, so what it submits can be checked without a disc. The
// pass list and the side table are a private namespace's, so these sit at file scope after them
void Platform::Graphics::HostMaterialSetLaterShader(HostMaterial* material, u32 shader, const u8* settings,
                                                   const f32* phases, const f32* speeds)
{
    if (material == nullptr || shader == 0 || shader >= Platform::Graphics::HostMaterialPasses || settings == nullptr)
    {
        return;
    }

    HostShaderScroll scroll = {};
    if (phases != nullptr)
    {
        scroll.phases[0] = phases[0];
        scroll.phases[1] = phases[1];
    }
    if (speeds != nullptr)
    {
        scroll.speeds[0] = speeds[0];
        scroll.speeds[1] = speeds[1];
    }

    MaterialLaterStoreShader(material, shader - 1, settings, scroll);
}

u32 Platform::Graphics::HostMaterialDrawPasses(const HostMaterial* material, u64 baseState, HostMaterialState* states,
                                               u8* shaders, u32 capacity)
{
    const MaterialDrawPasses passes = MaterialDrawPassesOf(material, baseState);
    if (states == nullptr || shaders == nullptr || passes.count > capacity)
    {
        return 0;
    }

    for (u32 pass = 0; pass < passes.count; pass++)
    {
        states[pass] = passes.states[pass];
        shaders[pass] = passes.shaders[pass];
    }

    return passes.count;
}

// The host material a submodel's material resource holds (the draws take their state and texture from it), nullptr
// when the resource or its material is missing
static const Platform::Graphics::HostMaterial* SubModelMaterial(MaterialResource** materials, u32 index)
{
    if (materials == nullptr || materials[index] == nullptr || materials[index]->material == nullptr)
    {
        return nullptr;
    }

    return reinterpret_cast<const Platform::Graphics::HostMaterial*>(materials[index]->material);
}

void Platform::Graphics::DrawPlacedModel(RigidModel* model, ChunkView* view, const Matrix4x4* world)
{
    // The scenery's meshes through the chunk's view: the world matrix taken into the chunk's space through the view's
    // vtable (slot 5, ChunkMatrixSlot), drawn with the view's matrix to the screen on the opaque views
    if (model == nullptr || view == nullptr || world == nullptr)
    {
        return;
    }

    auto* host = reinterpret_cast<HostRigidModel*>(model);
    if (host->model == nullptr)
    {
        return;
    }

    HostSyncViewMatrix();
    const Matrix4x4* inChunk = CallVirtual<const Matrix4x4*>(view, view->vtable, 5, world);
    // A model whose material makes it a billboard (retail SetPlacedModelRenderDMA's owner->billboard) has the turn
    // applied to its own space before the placement: the meshes face the camera wherever they stand
    bool billboard = false;
    for (u32 index = 0; index < host->count; index++)
    {
        const HostMaterial* candidate = SubModelMaterial(host->materials, index);
        if (candidate != nullptr && (candidate->flags & Platform::Graphics::HostMaterialBillboard) != 0)
        {
            billboard = true;
            break;
        }
    }

    Matrix4x4 mvp = {};
    Matrix4x4 turned = {};
    if (billboard && view->matrices != nullptr &&
        HostBillboardTurn(inChunk, &view->matrices[ChunkView::DrawInverseMatrix], view->view, &turned))
    {
        // The turn in the model's own space first, then the placement: retail's Billboard works the camera into the
        // model's space and hands the turn to the VU1 in place of the model's rotation, so the quad stays where the model
        // stands. Dropping the placement put every billboard on the level in one spot
        Matrix4x4 placed = {};
        VuMultiplyMatrices(&turned, inChunk, &placed);
        HostModelClipMatrix(&placed, &view->toClip, &mvp);
    }
    else
    {
        HostModelClipMatrix(inChunk, &view->toClip, &mvp);
    }
    // Triangle lists are the default primitive state (no flag); the depth test, its write and the blending come from
    // each submodel's material (HostMaterialStateOf), the pass's own being depth LESS with writes; the GS does not
    // backface-cull, so neither do we (reference sdl_gfx.cpp glDisable(GL_CULL_FACE))
    constexpr u64 PassState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
                              BGFX_STATE_DEPTH_TEST_LESS;

    HostRigidModelData* data = host->model;
    u32 subModels = data->subModelCount < host->count ? data->subModelCount : host->count;
    for (u32 index = 0; index < subModels; index++)
    {
        const HostMaterial* subModelMaterial = SubModelMaterial(host->materials, index);
        const bgfx::ViewId view = ViewOf(subModelMaterial);
        HostSceneSetView(view);
        const bgfx::TextureHandle subModelTexture = HostSubModelTexture(host->materials, index);
        // A material's every shader draws over its geometry (RenderMaterial's WriteShaders), and a shader whose alpha test
        // drops a sample may write it in a second pass: the scenery is most of the frame's geometry, so a material that
        // means more than its first shader has to draw it here too
        const MaterialDrawPasses passes = MaterialDrawPassesOf(subModelMaterial, PassState);
        for (u32 pass = 0; pass < passes.count; pass++)
        {
            const HostMaterialState& passState = passes.states[pass];
            const u32 shader = passes.shaders[pass];
            const bgfx::TextureHandle texture =
                shader != 0 ? LaterPassTexture(subModelMaterial, shader, subModelTexture) : subModelTexture;
            // The pass's own shader's scroll (each shader of a material has its own): a later pass that scrolls moves its
            // UVs too
            SetPassScroll(subModelMaterial, shader);
            // The pass's own shading: a shader with the GS's IIP bit cleared draws flat (HostMaterialState::flat), which is
            // the flat-qualified program and nothing else about the draw
            HostDrawRigidPacket(data->packets[index], data->sizes[index], data->vertexCounts[index], &mvp, inChunk,
                                nullptr, texture, view, passState.state, passState.alphaReference, UINT32_MAX,
                                FlatProgramOr(passState.flat, HostDraw3DFlatProgram()));
        }
    }
}

RigidModel* Platform::Graphics::LodModelAt(Lod* lod, u32 distance)
{
    // A level of detail's mesh for a squared distance (none nearer than its nearest or past its farthest; the
    // farthest -1 no limit): mirrors the PS2's LodMeshAt (src/platform/ps2/renderer/resources.cpp). A raised pop-in distance
    // (Platform::Graphics::ViewDistanceScale) ignores the farthest: the level's own data stopped the scenery's far level at a
    // distance retail's fog hid, and past it the model drew nothing and popped in on the way back. Its last mesh is what keeps
    // drawing instead, which is what a longer reach is for
    auto* host = reinterpret_cast<HostLod*>(lod);
    const bool reachExtended = ViewDistanceScale() > 1.0f;
    if (host == nullptr || host->count == 0 || distance < host->nearest || (!reachExtended && host->farthest < distance))
    {
        return nullptr;
    }

    RigidModel* mesh = reinterpret_cast<RigidModel*>(host->meshes[0]);
    s32 last = static_cast<s32>(host->count) - 1;
    if (last <= 0 || distance < host->switchDistances[0])
    {
        return mesh;
    }

    mesh = reinterpret_cast<RigidModel*>(host->meshes[1]);
    for (s32 index = 1; index < last && distance >= host->switchDistances[index]; index++)
    {
        mesh = reinterpret_cast<RigidModel*>(host->meshes[index + 1]);
    }

    return mesh;
}

s32 Platform::Graphics::LoadParticleView(const Matrix4x4* matrices, s32 index)
{
    // The chunk's view for this frame's particles, as the PS2 loads it: the chunk's matrix (its fourth row the camera's
    // place in the chunk's space) and the frame's frustum planes are kept as the index'th view (culling.cpp's
    // HostStoreParticleView), which Platform::Math::ViewDistance and ParticleBlockView read back for the particles'
    // distances and culling. The handle is that index. The PS2's contract is kept as well: the view goes in the word
    // 0x100 bytes past the matrices (the chunk's own ChunkData::particleView field, the word the game keeps it in)
    // and the handle is returned
    HostStoreParticleView(index, matrices);
    if (matrices != nullptr)
    {
        *reinterpret_cast<s32*>(const_cast<Matrix4x4*>(matrices) + 4) = index;
    }

    return index;
}

void Platform::Graphics::SetFontPage(Material* material, u32 page)
{
    // The material's first shader remembers the page of its font it draws (the retail SetFontPage). Each font page has a
    // material of its own whose texture is that page, so the host keeps the page for the glyph draws' bookkeeping
    auto* host = reinterpret_cast<HostMaterial*>(material);
    if (host != nullptr)
    {
        host->fontPage = static_cast<u8>(page);
    }
}

void Platform::Graphics::DrawRigidModel(RigidModel* model, const Matrix4x4* matrix, const ModelLights& lights, u32 mode)
{
    // An OGI's rigid models on their joints: every submodel through the
// render view's matrix with the object-space lights (mode 1 wholly in view,
// 2 clipped: the host draws everything, clipping rides the culling pass)
    (void)mode;
    if (model == nullptr || matrix == nullptr)
    {
        return;
    }

    auto* host = reinterpret_cast<HostRigidModel*>(model);
    if (host->model == nullptr)
    {
        return;
    }

    HostSyncViewMatrix();
    Matrix4x4 mvp = {};
    HostModelClipMatrix(matrix, HostViewMatrix(), &mvp);
    constexpr u64 PassState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
                              BGFX_STATE_DEPTH_TEST_LESS;
    HostRigidModelData* data = host->model;
    u32 subModels = data->subModelCount < host->count ? data->subModelCount : host->count;
    for (u32 index = 0; index < subModels; index++)
    {
        const HostMaterial* subModelMaterial = SubModelMaterial(host->materials, index);
        const MaterialDrawPasses passes = MaterialDrawPassesOf(subModelMaterial, PassState);
        // The shadow-caster mark (draws_host.cpp's MarkedForCaster): an OGI's rigid parts are the character too, so
        // their draws leave the frame's alpha at bit 7 = 0 and the shadow volumes skip them. It keeps an opaque part's
        // RGB factors intact -- forcing only the alpha half left them at enum 0, which bgfx/drivers draw black
        const bgfx::ViewId view = ViewOf(subModelMaterial);
        HostSceneSetView(view);
        const bgfx::TextureHandle subModelTexture = HostSubModelTexture(host->materials, index);
        // The material's shaders in turn (each over the same geometry, a later one with its OWN texture: a two-shader
        // material's second pass is typically additive and leaves the depth alone), each followed by the pass over the
        // samples its alpha test rejected when its failure action writes them
        for (u32 pass = 0; pass < passes.count; pass++)
        {
            const HostMaterialState& passState = passes.states[pass];
            const u32 shader = passes.shaders[pass];
            const bgfx::TextureHandle texture =
                shader != 0 ? LaterPassTexture(subModelMaterial, shader, subModelTexture) : subModelTexture;
            // The pass's own shader's scroll (each shader of a material has its own)
            SetPassScroll(subModelMaterial, shader);
            HostDrawRigidPacket(data->packets[index], data->sizes[index], data->vertexCounts[index], &mvp, matrix,
                                &lights, texture, view,
                                Platform::Graphics::MarkedForCaster(passState.state), passState.alphaReference, UINT32_MAX,
                                FlatProgramOr(passState.flat, HostDraw3DFlatProgram()));
        }
    }
}

void Platform::Graphics::DrawSkin(Skin* skin, const Matrix4x4* joints, u32 jointCount, const Matrix4x4* matrix, const ModelLights& lights, u32 mode)
{
    // An OGI's skin: every submodel skinned by the joint matrices (each
// joint's inverse bind pose times its matrix in the model's space)
    (void)mode;
    if (skin == nullptr || joints == nullptr || jointCount == 0 || matrix == nullptr)
    {
        return;
    }

    auto* host = reinterpret_cast<HostSkin*>(skin);
    HostSyncViewMatrix();
    Matrix4x4 mvp = {};
    HostModelClipMatrix(matrix, HostViewMatrix(), &mvp);
    constexpr u64 PassState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
                              BGFX_STATE_DEPTH_TEST_LESS;
    for (u32 index = 0; index < host->count; index++)
    {
        const HostMaterial* subModelMaterial = SubModelMaterial(host->materials, index);
        const bgfx::ViewId view = ViewOf(subModelMaterial);
        HostSceneSetView(view);
        const bgfx::TextureHandle subModelTexture = HostSubModelTexture(host->materials, index);
        // The material's first shader, its later shaders (each with its own texture, as the rigid path's: a skin's
        // material goes through RenderMaterial, which uploads every shader's own image before its packet), each followed by
        // the pass over the samples its alpha test rejected
        const MaterialDrawPasses passes = MaterialDrawPassesOf(subModelMaterial, PassState);
        for (u32 pass = 0; pass < passes.count; pass++)
        {
            const HostMaterialState& passState = passes.states[pass];
            const u32 shader = passes.shaders[pass];
            const bgfx::TextureHandle texture =
                shader != 0 ? LaterPassTexture(subModelMaterial, shader, subModelTexture) : subModelTexture;
            SetPassScroll(subModelMaterial, shader);
            HostDrawSkinPacket(host->packets[index], host->sizes[index], host->vertexCounts[index], 0, &mvp, matrix,
                               &lights, joints, jointCount, nullptr, 0, texture, view, passState.state,
                               passState.alphaReference, nullptr, nullptr,
                               FlatProgramOr(passState.flat, HostDraw3DSkinFlatProgram()));
        }
    }
}

void Platform::Graphics::DrawBlendSkin(BlendSkin* skin, const Matrix4x4* joints, u32 jointCount, const Matrix4x4* matrix, const ModelLights& lights, const f32* weights, const s32* shapes, s32 shapeCount, u32 mode)
{
    // An OGI's blend skin: every part of every submodel, the shapes the
    // caller named (shapes[i] and weights[i], a pair) baked into the parts'
    // vertexes by the draw below
    (void)mode;
    if (skin == nullptr || joints == nullptr || jointCount == 0 || matrix == nullptr)
    {
        return;
    }

    auto* host = reinterpret_cast<HostBlendSkin*>(skin);
    HostSyncViewMatrix();
    Matrix4x4 mvp = {};
    HostModelClipMatrix(matrix, HostViewMatrix(), &mvp);
    constexpr u64 PassState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
                              BGFX_STATE_DEPTH_TEST_LESS;
    u32 blends = shapeCount > 0 ? static_cast<u32>(shapeCount) : 0;
    for (u32 sub = 0; sub < host->count; sub++)
    {
        HostBlendSubModel* subModel = host->subModels[sub];
        if (subModel == nullptr)
        {
            continue;
        }

        // The submodel's material: its parts all draw with it (the PS2's blend skin shares one material a submodel)
        const HostMaterial* subModelMaterial = SubModelMaterial(host->materials, sub);
        // A blend skin's material set-up (materials.cpp's RenderBlendSkinMaterial) uploads the FIRST shader's texture for
        // every shader -- its own special case, which RenderMaterial does not share -- and writes a packet for each of
        // them, so every pass over a part samples that one image
        const bgfx::TextureHandle texture = HostSubModelTexture(host->materials, sub);
        const MaterialDrawPasses passes = MaterialDrawPassesOf(subModelMaterial, PassState);
        const bgfx::ViewId view = ViewOf(subModelMaterial);
        HostSceneSetView(view);
        for (u32 pass = 0; pass < passes.count; pass++)
        {
            const HostMaterialState& passState = passes.states[pass];
            SetPassScroll(subModelMaterial, passes.shaders[pass]);
            for (u32 part = 0; part < subModel->count; part++)
            {
                HostBlendPart* blendPart = subModel->parts[part];
                if (blendPart == nullptr)
                {
                    continue;
                }

                HostDrawSkinPacket(blendPart->packet, blendPart->packetSize, 0, host->shapeCount, &mvp, matrix, &lights,
                                   joints, jointCount, weights, blends, texture, view, passState.state,
                                   passState.alphaReference, blendPart, shapes,
                                   FlatProgramOr(passState.flat, HostDraw3DSkinFlatProgram()));
            }
        }
    }
}

namespace
{
// The screen model builder (the skid marks are the game's only models, skidmarks.cpp's LayStrip): the material the model
// being built draws with, the vertexes held -- each one's place and the colour given last, the PS2's rule (a vertex
// without a colour of its own takes the colour ScreenModelColour gave last: skidmarks.cpp sets one before every vertex)
// -- and the colour given last. The PS2 keeps the game's globals and flushes them into its packet every 38 vertexes
// (HeldVertexes); the host keeps the whole strip and freezes it into the model EndScreenModel returns, so a model may be
// as long as the game builds one
std::vector<Platform::Graphics::HostScreenModelVertex> g_screenModelVertices;
Material* g_screenModelMaterial = nullptr;
u32 g_screenModelColour = 0;
}

void Platform::Graphics::BeginScreenModel()
{
    // The builder restarted: no vertexes, no colour given, and the material the game sets next (retail's StartScreenModel
    // starts from g_ScreenModelMaterial, the keyed default material InitMaterials made; on the host a model the game
    // never gives a material draws through the pass's own state, which never happens in the game)
    g_screenModelVertices.clear();
    g_screenModelMaterial = nullptr;
    g_screenModelColour = 0;
}

void Platform::Graphics::SetScreenModelMaterial(Material* material)
{
    // None keeps the material set before (retail's ScreenModelMaterial)
    if (material != nullptr)
    {
        g_screenModelMaterial = material;
    }
}

void Platform::Graphics::SetScreenModelColour(u32 colour)
{
    // The colour the vertexes that follow take, RGBA bytes (the GS's blend reads 0x80 as one)
    g_screenModelColour = colour;
}

void Platform::Graphics::AddScreenModelVertex(const Vector4* place)
{
    if (place == nullptr)
    {
        return;
    }

    // The place and the colour given last. The place's fourth word is the PS2's no-draw flag, which this path never sets
    // (retail's AddScreenModelVertex hands ScreenModelVertex a zero), so the strip's vertexes are the three coordinates
    HostScreenModelVertex vertex;
    vertex.x = place->x;
    vertex.y = place->y;
    vertex.z = place->z;
    vertex.colour = g_screenModelColour;
    g_screenModelVertices.push_back(vertex);
}

ScreenModel* Platform::Graphics::EndScreenModel()
{
    // The strip the builder holds, frozen into a model the game keeps: its material and its vertex bytes, which the
    // draw submits. A model of no vertexes is not made -- the game never ends one without adding a strip's vertexes, and
    // retail's FinishScreenModel of none writes its RET tag through a null pointer (its own bug)
    if (g_screenModelVertices.empty())
    {
        return nullptr;
    }

    auto* model = static_cast<HostScreenModel*>(Platform::Graphics::HostMemory::AllocateZeroed(1, sizeof(HostScreenModel)));
    model->material = g_screenModelMaterial;
    model->count = static_cast<u32>(g_screenModelVertices.size());
    model->packetSize = model->count * sizeof(HostScreenModelVertex);
    model->packet = static_cast<u8*>(Platform::Graphics::HostMemory::Allocate(model->packetSize));
    std::memcpy(model->packet, g_screenModelVertices.data(), model->packetSize);
    g_screenModelVertices.clear();
    return reinterpret_cast<ScreenModel*>(model);
}

void Platform::Graphics::DeleteScreenModel(ScreenModel* model)
{
    // The model's vertex bytes and then the model (retail's DeleteScreenModel frees the packet first)
    auto* host = reinterpret_cast<HostScreenModel*>(model);
    if (host != nullptr)
    {
        Platform::Graphics::HostMemory::Free(host->packet);
        Platform::Graphics::HostMemory::Free(host);
    }
}

void Platform::Graphics::DrawScreenModel(ScreenModel* model, const Matrix4x4* toScreen, const Matrix4x4* toClip, const Vector4* clip)
{
    // The model's strip through the global objects' view (its material's bucket is BucketGlobalOpaque, the bucket the
    // skid marks' material names), after the opaque pass whose scene the marks lie on. toScreen
    // is the view's world-to-screen matrix (DrawSkidMarks scales its depth term so the marks sit against the ground they
    // were laid on) and the model-to-clip matrix the strip's places go through. toClip and clip are the planes the PS2's
    // VU1 clips the strip by; the host leaves clipping to the GPU, as the rigid and skin paths do
    (void)toClip;
    (void)clip;
    const auto* host = reinterpret_cast<const HostScreenModel*>(model);
    if (host == nullptr || host->count == 0 || toScreen == nullptr)
    {
        return;
    }

    // The pass's own state over the material's: the colour and alpha are written, the depth tested, which
    // HostMaterialStateOf folds the material's blending, alpha test and depth write onto (the marks' material writes no
    // depth, so the ground's stays)
    constexpr u64 PassState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
                              BGFX_STATE_DEPTH_TEST_LESS;
    const HostMaterialState state =
        HostMaterialStateOf(reinterpret_cast<const HostMaterial*>(host->material), PassState, true);
    // The GlobalOpaque view's own set-up: nothing else draws in it, so it is given the scene's frame buffer and the
    // frame's size here (no clear: the opaque pass cleared the frame the marks land on)
    HostSceneSetView(HostGlobalOpaqueView);
    HostDrawScreenModel(host, toScreen, HostGlobalOpaqueView, state.state, state.alphaReference,
                        FlatProgramOr(state.flat, HostDraw3DFlatProgram()));
}

Material* Platform::Graphics::MakeSkidMaterial(bool subtracting)
{
    // The skid marks' material, as retail makes it (materials.cpp's MakeSkidMaterial): shader type 1 -- its vertexes'
    // colours with no texture -- in the global objects' bucket, blended with the add preset or the subtract one, its
    // depth not written, gated by the frame buffer's alpha (the destination test, DATE). The host keeps the same GS
    // settings, which HostMaterialStateOf turns into the strip's blend and depth state, so the two materials differ in
    // their preset alone; the strip itself is drawn with the shared 3D program, whose ambient-only lighting and white
    // texture reduce it to the vertexes' colours
    constexpr u8 SkidDepthTest = 2; // the GS's ZTST gequal: ShaderConstruct's default, which the skid material keeps
    auto* material = static_cast<Material*>(Platform::Graphics::HostMemory::AllocateZeroed(1, MaterialStorage));
    ConstructMaterial(material);
    auto* host = reinterpret_cast<HostMaterial*>(material);
    host->program = HostDraw3DProgram().idx;
    host->flags = HostMaterialSettings;
    // The skid marks' bucket, as retail's MakeSkidMaterial names it (materials.cpp: BucketGlobalOpaque)
    host->bucket = 3;
    // The strip's draw binds the texture itself (the white one: the material samples none), so its sampler handle is
    // the invalid one the particle materials also carry
    host->sampler = HostInvalidHandle;
    host->texture = HostWhiteTexture().idx;
    host->blends = 1;
    host->depthTest = SkidDepthTest;
    u8* settings = host->shaderSettings;
    // The settings retail's MakeSkidMaterial and ShaderConstruct leave there (renderer.h's ShaderSettings bytes, in the
    // order ReadShader reads its fields): the shader blends with the add or the subtract preset, tests the depth with the
    // GS's gequal, writes no depth, gates on the destination's alpha and shades with the vertexes' colours. The rest of
    // the bytes are ShaderConstruct's own defaults (textured 0, the scrolls, linear) and no host draw reads them, so they
    // stay zeroed here
    settings[HostShaderSettingBlends] = 1;
    settings[HostShaderSettingPreset] = subtracting ? HostPresetSubtract : HostPresetAdd;
    settings[HostShaderSettingDepthTest] = SkidDepthTest;
    settings[HostShaderSettingGouraud] = 1;
    settings[HostShaderSettingDestinationTest] = 1;
    settings[HostShaderSettingDestinationMode] = 0;
    settings[HostShaderSettingNoDepthWrites] = 1;
    return material;
}

namespace
{
// A material's shaders' animations started again (resources.cpp's RestartAnimations(Material) restarts every shader's):
// the host keeps one per shader, so a material whose animated shader is not the first restarts too
void RestartMaterialAnimations(Material* material)
{
    HostMaterialAnimation* state =
        FindMaterialAnimation(reinterpret_cast<Platform::Graphics::HostMaterial*>(material));
    if (state == nullptr)
    {
        return;
    }

    for (u32 shader = 0; shader < HostMaterialShaderSlots; shader++)
    {
        if (state->animations[shader] != nullptr)
        {
            RestartShaderAnimation(state->animations[shader]);
        }
    }
}

// The materials of a model's or skin's submodels started again, the null ones left out
void RestartModelAnimations(MaterialResource** materials, u32 count)
{
    if (materials == nullptr)
    {
        return;
    }

    for (u32 index = 0; index < count; index++)
    {
        if (materials[index] != nullptr && materials[index]->material != nullptr)
        {
            RestartMaterialAnimations(materials[index]->material);
        }
    }
}
}

void Platform::Graphics::RestartAnimations(RigidModel* model)
{
    // A rigid model's materials' animations (resources.cpp's RigidModelRestartAnimations): the submodels the model
    // has materials for, its own count capped by the model's submodel count
    auto* host = reinterpret_cast<HostRigidModel*>(model);
    if (host == nullptr || host->model == nullptr)
    {
        return;
    }

    u32 count = host->count;
    if (host->model->subModelCount < count)
    {
        count = host->model->subModelCount;
    }

    RestartModelAnimations(host->materials, count);
}

void Platform::Graphics::RestartAnimations(Skin* skin)
{
    // A skin's materials' animations (resources.cpp's SkinRestartAnimations)
    auto* host = reinterpret_cast<HostSkin*>(skin);
    if (host == nullptr)
    {
        return;
    }

    RestartModelAnimations(host->materials, host->count);
}

void Platform::Graphics::RestartAnimations(BlendSkin* skin)
{
    // A blend skin's materials' animations (resources.cpp's BlendSkinRestartAnimations)
    auto* host = reinterpret_cast<HostBlendSkin*>(skin);
    if (host == nullptr)
    {
        return;
    }

    RestartModelAnimations(host->materials, host->count);
}

MaterialResource* Platform::Graphics::FirstMaterial(const RigidModel* model)
{
    // A rigid model's first material (nullptr when it has none): mirrors the PS2's FirstMaterial
    auto* host = reinterpret_cast<const HostRigidModel*>(model);
    return host != nullptr && host->materials != nullptr && host->count != 0 ? host->materials[0] : nullptr;
}

GameTexture* Platform::Graphics::NewTexture(u32 id)
{
    return static_cast<GameTexture*>(MakeResource(id, TextureResourceSize));
}

// A draw resolved this material: it's still in use, so it leaves the quarantine last (see the quarantine's note above)
void Platform::Graphics::TouchFreedMaterial(const void* host)
{
    const auto found = g_freedByHost.find(host);
    if (found != g_freedByHost.end())
    {
        g_freedMaterials.splice(g_freedMaterials.end(), g_freedMaterials, found->second);
    }
}

void Platform::Graphics::DeleteTexture(GameTexture* texture)
{
    if (texture == nullptr)
    {
        return;
    }

    // A material the tables freed still draws with this texture (the quarantine above): keep the upload the console's
    // texture memory would keep, and the resource's bytes, until that material leaves the quarantine
    if (TexturePinned(texture))
    {
        g_freedTextures.push_back(texture);
        while (g_freedTextures.size() > QuarantinedTextures)
        {
            // The oldest one no quarantined material still draws goes. A pinned one keeps its place rather than
            // holding the whole list up: the materials' own bound limits how many pinned textures there can be
            size_t victim = 0;
            while (victim < g_freedTextures.size() && TexturePinned(g_freedTextures[victim]))
            {
                victim++;
            }

            if (victim == g_freedTextures.size())
            {
                break;
            }

            GameTexture* released = g_freedTextures[victim];
            g_freedTextures.erase(g_freedTextures.begin() + static_cast<long>(victim));
            HostTextures().InvalidateTransfer(released);
            Platform::Graphics::HostMemory::Free(released);
        }

        return;
    }

    // The uploads the resource owns go with it
    HostTextures().InvalidateTransfer(texture);
    Platform::Graphics::HostMemory::Free(texture);
}

void Platform::Graphics::ReadTexture(GameTexture* texture, Stream* stream)
{
    u32 size;
    stream->ReadU32(&size);
    constexpr u32 HeaderAndToolsSize = TextureGsHeaderSize + TextureToolsHeaderSize;
    if (size < HeaderAndToolsSize)
    {
        stream->Skip(static_cast<s32>(size));
        return;
    }

    // The GS header and the tools' memory (both skipped before), then the transfer chain the image sits in. Reading it
    // leaves the stream exactly where skipping it did
    u8 gsHeader[TextureGsHeaderSize];
    stream->Read(gsHeader, TextureGsHeaderSize, 1);
    stream->Skip(static_cast<s32>(TextureToolsHeaderSize));
    u32 transferSize = size - HeaderAndToolsSize;
    std::vector<u8> transfer(transferSize);
    stream->Read(transfer.data(), transferSize, 1);

    // Decode the image's level 0 once and keep it in the upload cache under the GameTexture* (the material's shaders
    // name this texture, and SubmitQuad finds its handle again through it). The decoder takes the whole transfer:
    // the image's size comes from its TRXREG register, not the header, and its CLUT is inside the image itself
    Ps2TextureHeader header = Ps2HeaderFromGs(gsHeader);
    HostTextureKey key = {texture,
                          header.pixelFormat,
                          header.widthPower,
                          header.heightPower,
                          header.levelOffsets[0],
                          header.paletteOffset};
    // One upload per GameTexture*: a block reused for another texture (its bytes replaced) drops the old one
    // first, so the draws' lookup by pointer never sees a stale image
    HostTextures().InvalidateTransfer(texture);
    HostTextures().GetOrUpload(key, &header, transfer.data(), transferSize);
}

bool Platform::Graphics::HostMaterialShaderScroll(const HostMaterial* material, u32 shader, f32* uv)
{
    // The offset the shader's own scroll modes and animation have moved it to, which its draws add to every UV. Shader
    // 0's lives in the material itself (the game's fixed 0x70 bytes hold it); a later shader's is kept beside it
    if (material == nullptr || uv == nullptr)
    {
        return false;
    }

    if (shader == 0)
    {
        uv[0] = material->scroll[0];
        uv[1] = material->scroll[1];
        return true;
    }

    // A shader the material does not have: nothing to report (a side entry can outlive the material its address belonged to)
    if (material->passCount == 0 || shader >= material->passCount)
    {
        return false;
    }

    HostMaterialLater* later = FindMaterialLater(material);
    if (later == nullptr || shader - 1 >= Platform::Graphics::HostMaterialPasses - 1)
    {
        return false;
    }

    uv[0] = later->scroll[shader - 1].scroll[0];
    uv[1] = later->scroll[shader - 1].scroll[1];
    return true;
}

MaterialResource* Platform::Graphics::NewMaterial(u32 id)
{
    return static_cast<MaterialResource*>(MakeResource(id, MaterialResourceSize));
}

void Platform::Graphics::DeleteMaterial(MaterialResource* material)
{
    if (material == nullptr)
    {
        return;
    }

    auto* host = reinterpret_cast<HostMaterial*>(material->material);
    GameTexture* texture = nullptr;
    if (host != nullptr)
    {
        // The shader animation goes with it (its frames and its disk node, DestroyShaderAnimation). What its later shaders
        // keep by side (their settings and their images' references) does not go yet: a draw that took this material before
        // the chunk wipe still draws through it for a couple of frames, on the console as here, and its later passes need
        // both. The quarantine's release is where they go (ReleaseOldestFreedMaterial -> RemoveMaterialLater)
        texture = host->gameTexture;
        RemoveMaterialAnimation(host);
    }

    // Kept rather than freed: a draw that took this material before the chunk wipe still draws through it, as it does on
    // the console (the quarantine note above). Its texture's upload is pinned by the same entry
    FreedMaterial freed;
    freed.resource = static_cast<void*>(material);
    freed.host = host;
    freed.texture = texture;
    g_freedMaterials.push_back(freed);
    g_freedByHost[host] = std::prev(g_freedMaterials.end());
    if (texture != nullptr)
    {
        g_texturePins[texture]++;
    }

    while (g_freedMaterials.size() > QuarantinedMaterials)
    {
        ReleaseOldestFreedMaterial();
    }

    // The reference ReadMaterial took of its shaders' texture goes with the material, as on the console -- after the entry
    // is in the quarantine, so a texture this was the last reference to finds itself pinned when the table's own DeleteTexture
    // runs and keeps its upload rather than dropping to the white stand-in
    if (texture != nullptr)
    {
        ReleaseTexture(texture);
    }
}

void Platform::Graphics::ReadMaterial(MaterialResource* material, Stream* stream)
{
    auto* host = static_cast<Material*>(Platform::Graphics::HostMemory::AllocateZeroed(1, MaterialStorage));
    material->material = host;

    // The first shader's settings (its blending, alpha test and depth test) and its texture, when it samples one,
    // kept alive for the draws (the PS2's ReadMaterialShaders takes the same reference); SubmitQuad resolves the
    // texture to its upload and the draws map the settings onto the bgfx state
    bool textured = false;
    auto* hostMaterial = reinterpret_cast<HostMaterial*>(host);
    HostShaderScroll scroll = {};
    u32 shaderTextureIds[4] = {0, 0, 0, 0};
    u32 shaderCount = 0;
    bool makesBillboard = false;
    s32 shaderType = 0;
    ShaderAnimation* animations[HostMaterialShaderSlots] = {nullptr, nullptr, nullptr, nullptr};
    u8 laterSettings[HostMaterialPasses - 1][HostShaderSettings] = {};
    HostShaderScroll laterScrolls[HostMaterialPasses - 1] = {};
    u32 textureId = ReadMaterialShaders(stream, &textured, hostMaterial->shaderSettings, &scroll, shaderTextureIds,
                                        &shaderCount, &makesBillboard, &shaderType, animations, HostMaterialShaderSlots,
                                        &hostMaterial->bucket, laterSettings, hostMaterial->colour, laterScrolls);
    // The console draws a material's every shader: a model's material (RenderMaterial -> WriteShaders) uploads each
    // shader's own texture before its packet, a blend skin's (RenderBlendSkinMaterial) the first shader's for all of
    // them. Keep what tells the later passes apart so the draws can submit them (host_graphics.h's
    // HostMaterial::passCount)
    {
        const u32 passes = shaderCount < HostMaterialPasses ? shaderCount : HostMaterialPasses;
        hostMaterial->passCount = static_cast<u8>(passes > 1 ? passes : 0);
        for (u32 pass = 1; pass < passes; pass++)
        {
            // Each later shader's own 30 setting bytes and scroll block: the GS's registers are the shader's
            // (shadersettings.cpp's PixelTest writes TEST's ATST, AREF and AFAIL, and DepthBuffer ZTST and its write, from
            // the shader being drawn), so pass k's blend, depth test, alpha test and destination test are its own and not
            // the first shader's, and so is the scroll its modes advance. The game's material storage is a fixed 0x70
            // bytes with no room for any of it (host_graphics.h's HostMaterial::passCount)
            MaterialLaterStoreShader(hostMaterial, pass - 1, laterSettings[pass - 1], laterScrolls[pass - 1]);
        }
    }
    std::memcpy(hostMaterial->scrollPhases, scroll.phases, sizeof(scroll.phases));
    std::memcpy(hostMaterial->scrollSpeeds, scroll.speeds, sizeof(scroll.speeds));
    std::memcpy(hostMaterial->scroll, scroll.scroll, sizeof(scroll.scroll));
    hostMaterial->blends = hostMaterial->shaderSettings[HostShaderSettingBlends];
    hostMaterial->depthTest = hostMaterial->shaderSettings[HostShaderSettingDepthTest];
    hostMaterial->flags |= HostMaterialSettings;
    hostMaterial->shaderType = static_cast<u8>(shaderType);
    // The file's setting byte 5 (host_graphics.h's layout comment: alphaFail)
    hostMaterial->alphaFail = hostMaterial->shaderSettings[5];
    if (makesBillboard)
    {
        hostMaterial->flags |= HostMaterialBillboard;
    }
    if (textured && textureId != 0)
    {
        hostMaterial->gameTexture = g_TextureTable.Acquire(&textureId, nullptr);
    }

    // The later shaders' textures, which the console uploads before each of their packets (materials.cpp's WriteShaders):
    // pass k samples shader k's image, and shader k+1's need not be the first's
    {
        GameTexture* passTextures[HostMaterialPasses] = {nullptr, nullptr, nullptr, nullptr};
        for (u32 pass = 1; pass < HostMaterialPasses && pass < shaderCount; pass++)
        {
            if (shaderTextureIds[pass] == 0)
            {
                continue;
            }

            u32 id = shaderTextureIds[pass];
            passTextures[pass] = g_TextureTable.Acquire(&id, nullptr);
        }

        AddMaterialPassTextures(hostMaterial, passTextures, HostMaterialPasses);
    }

    // The shaders' animations kept for the frame steps (StepMaterialScroll): their UV offsets and colours, the
    // material's storage having no room for them (MaterialStorage). Retail keeps one per shader, so a material whose
    // animated shader is not the first animates too
    bool anyAnimation = false;
    for (u32 shader = 0; shader < HostMaterialShaderSlots; shader++)
    {
        anyAnimation = anyAnimation || animations[shader] != nullptr;
    }

    if (anyAnimation)
    {
        AddMaterialAnimation(hostMaterial, animations);
    }
}

RigidModelData* Platform::Graphics::NewModel(u32 id)
{
    return static_cast<RigidModelData*>(MakeResource(id, ModelResourceSize));
}

void Platform::Graphics::DeleteModel(RigidModelData* model)
{
    if (model == nullptr)
    {
        return;
    }

    // Every submodel's bytes go (ReadModel copied each one off the stream), along with the draw cache's upload of them:
    // a packet address the allocator hands out again must not find the old model's vertices. The game's tables drop a
    // model when a chunk unloads, so leaving these behind leaked a chunk's meshes on every transition
    auto* data = reinterpret_cast<HostRigidModelData*>(model);
    if (data->packets != nullptr)
    {
        for (u32 index = 0; index < data->subModelCount; index++)
        {
            HostRetireDrawPacket(data->packets[index]);
            Platform::Graphics::HostMemory::Free(const_cast<u8*>(data->packets[index]));
        }
    }

    Platform::Graphics::HostMemory::Free(data->vertexCounts);
    Platform::Graphics::HostMemory::Free(data->sizes);
    Platform::Graphics::HostMemory::Free(data->extraSizes);
    Platform::Graphics::HostMemory::Free(const_cast<u8**>(data->packets));
    Platform::Graphics::HostMemory::Free(model);
}

void Platform::Graphics::ReadModel(RigidModelData* model, Stream* stream)
{
    // A model's submodels as the draws need them: each one's vertex count, packet bytes (copied off the stream, since the
    // host's drag-and-drop parse cache keys on the packet's address and the PS2's disk handles have no host counterpart) and
    // dropped extra. Mirrors the PS2's ModelRead (src/platform/ps2/renderer/resources.cpp): the packet's bytes are what the
    // rigid-mesh parse (mesh_builder.cpp) walks
    auto* data = reinterpret_cast<HostRigidModelData*>(model);
    s32 subModelCount;
    stream->ReadS32(&subModelCount);
    const u32 readCount = subModelCount < 0 ? 0 : static_cast<u32>(subModelCount);
    // Only the submodels the host draws are stored; the records past the cap are still consumed so the stream keeps the
    // file's own position (a damaged count must not desync everything read after it any further than it already is)
    const u32 stored = readCount < MaxResourceSubModels ? readCount : MaxResourceSubModels;
    data->subModelCount = stored;
    if (stored != 0)
    {
        data->vertexCounts = static_cast<u32*>(Platform::Graphics::HostMemory::AllocateZeroed(stored, sizeof(u32)));
        data->sizes = static_cast<u32*>(Platform::Graphics::HostMemory::AllocateZeroed(stored, sizeof(u32)));
        data->extraSizes = static_cast<u32*>(Platform::Graphics::HostMemory::AllocateZeroed(stored, sizeof(u32)));
        data->packets = static_cast<const u8**>(Platform::Graphics::HostMemory::AllocateZeroed(stored, sizeof(u8*)));
        if (data->vertexCounts == nullptr || data->sizes == nullptr || data->extraSizes == nullptr ||
            data->packets == nullptr)
        {
            // Nothing to draw into: the records are read and skipped below, and the model draws nothing
            data->subModelCount = 0;
        }
    }

    for (u32 index = 0; index < readCount; index++)
    {
        s32 vertexCount;
        stream->ReadS32(&vertexCount);
        s32 size;
        stream->ReadS32(&size);
        const bool keep = index < data->subModelCount;
        if (keep)
        {
            data->vertexCounts[index] = static_cast<u32>(vertexCount);
            data->sizes[index] = static_cast<u32>(size);
        }

        if (size > 0)
        {
            auto* packet = keep ? static_cast<u8*>(AllocResourceBytes(static_cast<u32>(size))) : nullptr;
            if (packet != nullptr)
            {
                stream->Read(packet, static_cast<u32>(size), 1);
                data->packets[index] = packet;
            }
            else
            {
                SkipResourceBytes(stream, static_cast<u32>(size));
            }
        }

        s32 extraSize;
        stream->ReadS32(&extraSize);
        if (keep)
        {
            data->extraSizes[index] = static_cast<u32>(extraSize);
        }
        if (extraSize > 0)
        {
            SkipResourceBytes(stream, static_cast<u32>(extraSize));
        }
    }
}

RigidModel* Platform::Graphics::NewRigidModel(u32 id)
{
    return static_cast<RigidModel*>(MakeResource(id, RigidModelResourceSize));
}

void Platform::Graphics::DeleteRigidModel(RigidModel* model)
{
    // The references its read took go with it (the PS2's RigidModelDestroy): its submodels' materials and the model it
    // names. Without them the game's tables never let an entry go, so the copy a level file installed stayed alive for
    // the whole session -- and the level files reuse IDs with different contents (the Earth hub's and AltEarth's
    // c975f2a2 have different submodels), so a later file's copy was never read and the earlier one was drawn instead
    auto* host = reinterpret_cast<HostRigidModel*>(model);
    for (u32 index = 0; index < host->count; index++)
    {
        ReleaseMaterial(host->materials[index]);
    }
    ReleaseModel(reinterpret_cast<RigidModelData*>(host->model));
    Platform::Graphics::HostMemory::Free(host->materials);
    Platform::Graphics::HostMemory::Free(model);
}

namespace
{
// A rigid model's or mesh's data after its version: its materials, its model (a submodel table acquired by ID), mirrored from
// the PS2's ReadRigidModelData (src/platform/ps2/renderer/resources.cpp). Shared by ReadRigidModel and ReadMesh, which read
// the same layout
void HostReadRigidModelData(Platform::Graphics::HostRigidModel* model, Stream* stream)
{
    s32 count;
    stream->ReadS32(&count);
    model->count = count < 0 ? 0 : static_cast<u32>(count);
    if (model->count != 0)
    {
        model->materials =
            static_cast<MaterialResource**>(Platform::Graphics::HostMemory::AllocateZeroed(model->count, sizeof(MaterialResource*)));
    }

    for (u32 index = 0; index < model->count; index++)
    {
        s32 id;
        stream->ReadS32(&id);
        u32 key = static_cast<u32>(id);
        model->materials[index] = g_MaterialTable.Acquire(&key, nullptr);
    }

    s32 modelId;
    stream->ReadS32(&modelId);
    model->modelId = static_cast<u32>(modelId);
    u32 key = static_cast<u32>(modelId);
    model->model = reinterpret_cast<Platform::Graphics::HostRigidModelData*>(g_ModelTable.Acquire(&key, nullptr));
}
}

void Platform::Graphics::ReadRigidModel(RigidModel* model, Stream* stream)
{
    s32 version;
    stream->ReadS32(&version);
    HostReadRigidModelData(reinterpret_cast<HostRigidModel*>(model), stream);
}

Skin* Platform::Graphics::NewSkin(u32 id)
{
    return static_cast<Skin*>(MakeResource(id, SkinResourceSize));
}

void Platform::Graphics::DeleteSkin(Skin* skin)
{
    // Its submodels' materials released (the PS2's SkinDestroy), else the material table keeps every skin's materials
    // alive for the session and a level file's later definition of one is never read
    auto* host = reinterpret_cast<HostSkin*>(skin);
    for (u32 index = 0; index < host->count; index++)
    {
        ReleaseMaterial(host->materials[index]);
    }
    Platform::Graphics::HostMemory::Free(host->materials);
    // Its submodels' bytes and the draw cache's uploads of them (DeleteModel's own reason)
    if (host->packets != nullptr)
    {
        for (u32 index = 0; index < host->count; index++)
        {
            HostRetireDrawPacket(host->packets[index]);
            Platform::Graphics::HostMemory::Free(const_cast<u8*>(host->packets[index]));
        }
    }
    Platform::Graphics::HostMemory::Free(host->vertexCounts);
    Platform::Graphics::HostMemory::Free(host->sizes);
    Platform::Graphics::HostMemory::Free(const_cast<u8**>(host->packets));
    Platform::Graphics::HostMemory::Free(skin);
}

void Platform::Graphics::ReadSkin(Skin* skin, Stream* stream)
{
    // A skin's submodels: each one's material, packet bytes (copied) and vertex count, mirrored from the PS2's SkinRead
    // (src/platform/ps2/renderer/resources.cpp). The host's HostSkin stores packets as addresses so the parse cache can key
    // on them
    auto* host = reinterpret_cast<HostSkin*>(skin);
    s32 count;
    stream->ReadS32(&count);
    const u32 readCount = count < 0 ? 0 : static_cast<u32>(count);
    const u32 stored = readCount < MaxResourceSubModels ? readCount : MaxResourceSubModels;
    host->count = stored;
    if (stored != 0)
    {
        host->materials = static_cast<MaterialResource**>(Platform::Graphics::HostMemory::AllocateZeroed(stored, sizeof(MaterialResource*)));
        host->vertexCounts = static_cast<u32*>(Platform::Graphics::HostMemory::AllocateZeroed(stored, sizeof(u32)));
        host->sizes = static_cast<u32*>(Platform::Graphics::HostMemory::AllocateZeroed(stored, sizeof(u32)));
        host->packets = static_cast<const u8**>(Platform::Graphics::HostMemory::AllocateZeroed(stored, sizeof(u8*)));
        if (host->materials == nullptr || host->vertexCounts == nullptr || host->sizes == nullptr ||
            host->packets == nullptr)
        {
            host->count = 0;
        }
    }

    for (u32 index = 0; index < readCount; index++)
    {
        s32 id;
        stream->ReadS32(&id);
        s32 size;
        stream->ReadS32(&size);
        s32 vertexCount;
        stream->ReadS32(&vertexCount);
        const bool keep = index < host->count;
        if (keep)
        {
            u32 key = static_cast<u32>(id);
            host->materials[index] = g_MaterialTable.Acquire(&key, nullptr);
            host->sizes[index] = static_cast<u32>(size);
            host->vertexCounts[index] = static_cast<u32>(vertexCount);
            host->vertexTotal += static_cast<u32>(vertexCount);
        }

        if (size > 0)
        {
            auto* packet = keep ? static_cast<u8*>(AllocResourceBytes(static_cast<u32>(size))) : nullptr;
            if (packet != nullptr)
            {
                stream->Read(packet, static_cast<u32>(size), 1);
                host->packets[index] = packet;
            }
            else
            {
                SkipResourceBytes(stream, static_cast<u32>(size));
            }
        }
    }
}

BlendSkin* Platform::Graphics::NewBlendSkin(u32 id)
{
    return static_cast<BlendSkin*>(MakeResource(id, BlendSkinResourceSize));
}

void Platform::Graphics::DeleteBlendSkin(BlendSkin* skin)
{
    auto* host = reinterpret_cast<HostBlendSkin*>(skin);
    for (u32 index = 0; index < host->count; index++)
    {
        ReleaseMaterial(host->materials[index]);
    }
    Platform::Graphics::HostMemory::Free(host->materials);
    for (u32 index = 0; index < host->count; index++)
    {
        auto* subModel = host->subModels[index];
        if (subModel != nullptr)
        {
            for (u32 part = 0; part < subModel->count; part++)
            {
                auto* blendPart = subModel->parts[part];
                if (blendPart != nullptr)
                {
                    // Its packet and every shape's bytes were copied off the stream (ReadBlendSkin), so they go with
                    // the part. Only the table of shape addresses was freed before, which leaked the data they point at
                    HostRetireDrawPacket(blendPart->packet);
                    Platform::Graphics::HostMemory::Free(const_cast<u8*>(blendPart->packet));
                    if (blendPart->shapeAddresses != nullptr)
                    {
                        for (u32 shape = 0; shape < blendPart->shapeCount; shape++)
                        {
                            Platform::Graphics::HostMemory::Free(reinterpret_cast<u8*>(static_cast<size_t>(blendPart->shapeAddresses[shape])));
                        }
                    }
                    Platform::Graphics::HostMemory::Free(blendPart->shapeAddresses);
                    Platform::Graphics::HostMemory::Free(blendPart->shapeSizes);
                    Platform::Graphics::HostMemory::Free(blendPart->shapeCounts);
                    Platform::Graphics::HostMemory::Free(blendPart);
                }
            }
            Platform::Graphics::HostMemory::Free(subModel->parts);
            Platform::Graphics::HostMemory::Free(subModel);
        }
    }
    Platform::Graphics::HostMemory::Free(host->subModels);
    Platform::Graphics::HostMemory::Free(skin);
}

void Platform::Graphics::ReadBlendSkin(BlendSkin* skin, Stream* stream)
{
    // A blend skin's submodels and their parts, mirrored from the PS2's BlendSkinRead/BlendSubModelSetCount/ReadBlendParts/
    // ReadBlendPart (src/platform/ps2/renderer/resources.cpp): each submodel a material and its parts; each part a packet
    // (copied), its three shape factors and one shape address/size/count per shape
    auto* host = reinterpret_cast<HostBlendSkin*>(skin);
    s32 count;
    stream->ReadS32(&count);
    s32 shapeCount;
    stream->ReadS32(&shapeCount);
    const u32 readCount = count < 0 ? 0 : static_cast<u32>(count);
    const u32 stored = readCount < MaxResourceSubModels ? readCount : MaxResourceSubModels;
    host->count = stored;
    // A part's shape count is a byte (HostBlendPart::shapeCount), so a file naming more than 255 is capped
    host->shapeCount = shapeCount < 0 ? 0
                                      : (static_cast<u32>(shapeCount) < MaxResourceShapes
                                             ? static_cast<u32>(shapeCount)
                                             : MaxResourceShapes);
    if (stored != 0)
    {
        host->materials = static_cast<MaterialResource**>(Platform::Graphics::HostMemory::AllocateZeroed(stored, sizeof(MaterialResource*)));
        host->subModels = static_cast<HostBlendSubModel**>(Platform::Graphics::HostMemory::AllocateZeroed(stored, sizeof(HostBlendSubModel*)));
        if (host->materials == nullptr || host->subModels == nullptr)
        {
            host->count = 0;
        }
    }

    for (u32 index = 0; index < readCount; index++)
    {
        const bool keepModel = index < host->count;
        auto* subModel = keepModel ? static_cast<HostBlendSubModel*>(Platform::Graphics::HostMemory::AllocateZeroed(1, sizeof(HostBlendSubModel)))
                                   : nullptr;
        s32 partCount;
        stream->ReadS32(&partCount);
        s32 id;
        stream->ReadS32(&id);
        const u32 readParts = partCount < 0 ? 0 : static_cast<u32>(partCount);
        const u32 storedParts =
            readParts < MaxResourceBlendParts ? readParts : MaxResourceBlendParts;
        if (keepModel)
        {
            u32 key = static_cast<u32>(id);
            host->materials[index] = g_MaterialTable.Acquire(&key, nullptr);
            subModel->count = storedParts;
            subModel->parts = static_cast<HostBlendPart**>(Platform::Graphics::HostMemory::AllocateZeroed(storedParts, sizeof(HostBlendPart*)));
            if (subModel->parts == nullptr)
            {
                subModel->count = 0;
            }
            host->subModels[index] = subModel;
        }

        for (u32 part = 0; part < readParts; part++)
        {
            const bool wanted = keepModel && part < subModel->count;
            auto* blendPart = wanted ? static_cast<HostBlendPart*>(Platform::Graphics::HostMemory::AllocateZeroed(1, sizeof(HostBlendPart))) : nullptr;
            // A part the allocator refused is read and skipped like one past the cap (the draw finds a null part)
            const bool keep = blendPart != nullptr;
            s32 packetSize;
            stream->ReadS32(&packetSize);
            s32 unused;
            stream->ReadS32(&unused);
            if (keep)
            {
                blendPart->packetSize = static_cast<u32>(packetSize);
            }

            if (packetSize > 0)
            {
                auto* packet = keep ? static_cast<u8*>(AllocResourceBytes(static_cast<u32>(packetSize))) : nullptr;
                if (packet != nullptr)
                {
                    stream->Read(packet, static_cast<u32>(packetSize), 1);
                    blendPart->packet = packet;
                }
                else
                {
                    SkipResourceBytes(stream, static_cast<u32>(packetSize));
                }
            }

            u8 shapeFactors[ShapeFactorsSize];
            stream->Read(shapeFactors, ShapeFactorsSize, 1);
            const u8 shapes = static_cast<u8>(host->shapeCount);
            if (keep)
            {
                std::memcpy(blendPart->shapeFactors, shapeFactors, ShapeFactorsSize);
                blendPart->shapeCount = shapes;
                if (shapes != 0)
                {
                    blendPart->shapeAddresses = static_cast<u32*>(Platform::Graphics::HostMemory::AllocateZeroed(shapes, sizeof(u32)));
                    blendPart->shapeSizes = static_cast<u32*>(Platform::Graphics::HostMemory::AllocateZeroed(shapes, sizeof(u32)));
                    blendPart->shapeCounts = static_cast<u32*>(Platform::Graphics::HostMemory::AllocateZeroed(shapes, sizeof(u32)));
                    if (blendPart->shapeAddresses == nullptr || blendPart->shapeSizes == nullptr ||
                        blendPart->shapeCounts == nullptr)
                    {
                        // No table to record the shapes in: the part draws its packet alone
                        blendPart->shapeCount = 0;
                    }
                }
            }

            for (u32 shape = 0; shape < shapes; shape++)
            {
                s32 shapeSize;
                stream->ReadS32(&shapeSize);
                s32 shapeShapes;
                stream->ReadS32(&shapeShapes);
                const u64 bytes = static_cast<u64>(static_cast<u32>(shapeSize)) << QuadwordShift;
                const bool record = keep && shape < blendPart->shapeCount;
                if (record)
                {
                    blendPart->shapeSizes[shape] = static_cast<u32>(shapeSize);
                    blendPart->shapeCounts[shape] = static_cast<u32>(shapeShapes);
                }

                if (bytes != 0)
                {
                    auto* shapeBytes =
                        record ? static_cast<u8*>(AllocResourceBytes(static_cast<u32>(bytes))) : nullptr;
                    if (shapeBytes != nullptr)
                    {
                        stream->Read(shapeBytes, static_cast<u32>(bytes), 1);
                        blendPart->shapeAddresses[shape] = reinterpret_cast<u32>(shapeBytes);
                    }
                    else
                    {
                        SkipResourceBytes(stream, bytes);
                    }
                }
            }

            if (keep)
            {
                subModel->parts[part] = blendPart;
            }
        }
    }
}

RigidModel* Platform::Graphics::NewMesh(u32 id)
{
    return static_cast<RigidModel*>(MakeResource(id, RigidModelResourceSize));
}

void Platform::Graphics::DeleteMesh(RigidModel* mesh)
{
    // A mesh is a rigid model (the PS2's MeshDestroy is RigidModelDestroy): its references go the same way
    auto* host = reinterpret_cast<HostRigidModel*>(mesh);
    for (u32 index = 0; index < host->count; index++)
    {
        ReleaseMaterial(host->materials[index]);
    }
    ReleaseModel(reinterpret_cast<RigidModelData*>(host->model));
    Platform::Graphics::HostMemory::Free(host->materials);
    Platform::Graphics::HostMemory::Free(mesh);
}

void Platform::Graphics::ReadMesh(RigidModel* mesh, Stream* stream)
{
    // A mesh reads exactly like a rigid model (the PS2's MeshRead calls ReadRigidModelData too)
    s32 version;
    stream->ReadS32(&version);
    HostReadRigidModelData(reinterpret_cast<HostRigidModel*>(mesh), stream);
}

Lod* Platform::Graphics::NewLod(u32 id)
{
    return static_cast<Lod*>(MakeResource(id, LodResourceSize));
}

void Platform::Graphics::DeleteLod(Lod* lod)
{
    // Its meshes released (the PS2's LodDestroy): a LOD kept its meshes' table entries alive for the session otherwise
    auto* host = reinterpret_cast<HostLod*>(lod);
    for (u32 index = 0; index < host->count; index++)
    {
        ReleaseMesh(reinterpret_cast<RigidModel*>(host->meshes[index]));
    }
    Platform::Graphics::HostMemory::Free(host->meshes);
    Platform::Graphics::HostMemory::Free(lod);
}

namespace
{
// The meshes a LOD or sky names, taken from their table (the PS2's ReadMeshes)
void HostReadMeshes(Platform::Graphics::HostRigidModel** meshes, u32 count, Stream* stream)
{
    for (u32 index = 0; index < count; index++)
    {
        s32 id;
        stream->ReadS32(&id);
        u32 key = static_cast<u32>(id);
        meshes[index] = reinterpret_cast<Platform::Graphics::HostRigidModel*>(g_MeshTable.Acquire(&key, nullptr));
    }
}

constexpr s32 NoDistance = -1;
}

void Platform::Graphics::ReadLod(Lod* lod, Stream* stream)
{
    // A level of detail's meshes and squared distances, mirrored from the PS2's LodRead (src/platform/ps2/renderer/resources.cpp).
    // The distances are stored squared (nearest and farthest, and each mesh's switch point), which is what LodModelAt compares a
    // squared camera distance against; the farthest -1 stays -1 (no limit). The meshes are acquired by ID from the mesh table
    auto* host = reinterpret_cast<HostLod*>(lod);
    s32 version;
    stream->ReadS32(&version);
    if (version == LodDistancesVersion)
    {
        u32 count;
        stream->ReadU32(&count);
        host->count = static_cast<u8>(count);
        host->meshes = static_cast<HostRigidModel**>(Platform::Graphics::HostMemory::AllocateZeroed(host->count, sizeof(HostRigidModel*)));
        s32 nearest;
        stream->ReadS32(&nearest);
        s32 farthest;
        stream->ReadS32(&farthest);
        host->nearest = static_cast<u32>(nearest * nearest);
        host->farthest = farthest == NoDistance ? static_cast<u32>(NoDistance)
                                                : static_cast<u32>(farthest * farthest);
        for (u32 distance = 0; distance < LodSwitchCount; distance++)
        {
            s32 value;
            stream->ReadS32(&value);
            host->switchDistances[distance] = static_cast<u32>(value * value);
        }

        s32 unused;
        stream->ReadS32(&unused);
        HostReadMeshes(host->meshes, host->count, stream);
        return;
    }

    if (version == LodSquaresVersion)
    {
        s8 count;
        stream->ReadS8(&count);
        host->count = static_cast<u8>(count);
        host->meshes = static_cast<HostRigidModel**>(Platform::Graphics::HostMemory::AllocateZeroed(host->count, sizeof(HostRigidModel*)));
        s32 nearest;
        stream->ReadS32(&nearest);
        s32 farthest;
        stream->ReadS32(&farthest);
        host->nearest = static_cast<u32>(nearest);
        host->farthest = static_cast<u32>(farthest);
        stream->Read(host->switchDistances, sizeof(host->switchDistances), 1);
        HostReadMeshes(host->meshes, host->count, stream);
    }
}

Sky* Platform::Graphics::NewSky(u32 id)
{
    return static_cast<Sky*>(MakeResource(id, SkyResourceSize));
}

void Platform::Graphics::DeleteSky(Sky* sky)
{
    // Its meshes released (the PS2's SkyDestroy), for the same reason a LOD's are
    auto* host = reinterpret_cast<HostSky*>(sky);
    for (u32 index = 0; index < host->count; index++)
    {
        ReleaseMesh(reinterpret_cast<RigidModel*>(host->models[index]));
    }
    Platform::Graphics::HostMemory::Free(host->models);
    Platform::Graphics::HostMemory::Free(sky);
}

void Platform::Graphics::ReadSky(Sky* sky, Stream* stream)
{
    // A sky's meshes, taken from the mesh table like the PS2's SkyRead (src/platform/ps2/renderer/resources.cpp). HostSky
    // (sky.cpp) holds them as HostRigidModel pointers; the game's read leaves the stream at the same place it always did
    auto* host = reinterpret_cast<HostSky*>(sky);
    u32 unused;
    stream->ReadU32(&unused);
    s32 count;
    stream->ReadS32(&count);
    host->count = count < 0 ? 0 : static_cast<u32>(count);
    if (host->count == 0)
    {
        return;
    }

    host->models = static_cast<HostRigidModel**>(Platform::Graphics::HostMemory::AllocateZeroed(host->count, sizeof(HostRigidModel*)));
    HostReadMeshes(host->models, host->count, stream);
}

// DrawChunkSky lives in sky.cpp: the half-size sky buffer and its copy over the screen

void Platform::Graphics::LoadParticlePage(ParticlePage* page, const char* path, bool decals)
{
    MemoryStream file;
    MemoryStream::ConstructFromFile(&file, path, false);
    ReadParticlePageData(page, &file, true, decals);
    file.Destroy(DestroyOnly);
}

void Platform::Graphics::ReadParticlePage(ParticlePage* page, Stream* stream, bool decals)
{
    ReadParticlePageData(page, stream, false, decals);
}
