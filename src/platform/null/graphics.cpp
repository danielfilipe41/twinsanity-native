// Graphics: nothing draws, frames are dropped silently.
#include "platform/graphics.h"
#include "game/graphicstables.h"
#include "game/particles.h"
#include "game/stream.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
[[noreturn]] void Abort(const char* name)
{
    std::fprintf(stderr, "null platform: Platform::Graphics::%s not implemented\n", name);
    std::abort();
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
    void* memory = std::calloc(1, size);
    static_cast<u32*>(memory)[1] = id;
    return memory;
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
    return joints * JointSettingsSize + staticValues * sizeof(s16) + frameValues * frames * sizeof(s16);
}

// A shader's bytes after its type: the type's extra words, then the base shader
void SkipShader(Stream* stream, s32 type)
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

    u8 settings[ShaderSettingFields];
    stream->Read(settings, sizeof(settings), 1);
    stream->Skip(2 * sizeof(s16));
    stream->Skip(2 * 4 * sizeof(f32));
    stream->Skip(4 * sizeof(f32));
    s32 textureId;
    stream->ReadS32(&textureId);
    s32 shaderType;
    stream->ReadS32(&shaderType);
    if (settings[ShaderHasAnimation] != 0)
    {
        stream->Skip(sizeof(u32));
        s32 layout;
        stream->ReadS32(&layout);
        s16 frames;
        stream->ReadS16(&frames);
        stream->Skip(static_cast<s32>(ShaderAnimationDataSize(static_cast<u32>(layout), static_cast<u16>(frames))));
    }
}

// A material's shaders read past (shaderclasses.cpp's ReadMaterialShaders): the programs' key, the bucket, the name and each
// shader's type and bytes
void SkipMaterialShaders(Stream* stream)
{
    s64 activatedShaders;
    stream->ReadS64(&activatedShaders);
    s32 bucket;
    stream->ReadS32(&bucket);
    s32 nameLength;
    stream->ReadS32(&nameLength);
    if (nameLength != 0)
    {
        stream->Skip(nameLength);
    }

    s32 shaderCount;
    stream->ReadS32(&shaderCount);
    for (s32 index = 0; index < shaderCount; index++)
    {
        s32 type;
        stream->ReadS32(&type);
        SkipShader(stream, type);
    }
}

// A rigid model's or mesh's data after its version: its materials' IDs and its model's ID
void SkipRigidModelData(Stream* stream)
{
    s32 count;
    stream->ReadS32(&count);
    for (s32 index = 0; index < count; index++)
    {
        s32 id;
        stream->ReadS32(&id);
    }

    s32 modelId;
    stream->ReadS32(&modelId);
}

// Meshes' IDs, taken from their table on the PS2
void SkipMeshIds(Stream* stream, u32 count)
{
    for (u32 index = 0; index < count; index++)
    {
        s32 id;
        stream->ReadS32(&id);
    }
}

// A particle page read like the PS2's ReadParticlePageData (ps2/renderer/particles.cpp): its texture and material IDs, the
// texture and material read from the stream when fromStream, else the material taken from the table; the modes' materials are
// the flat material, the page material's own for its mode
void ReadParticlePageData(ParticlePage* page, Stream* stream, bool fromStream, bool decals)
{
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
    page->materials[BlendAdditive] = Platform::Graphics::FlatMaterial();
    page->materials[BlendSubtractive] = Platform::Graphics::FlatMaterial();
    page->materials[BlendCutout] = Platform::Graphics::FlatMaterial();
}
}

void Platform::Graphics::WaitVSync()
{
}

bool Platform::Graphics::IsPalDisplay()
{
    return false;
}

void Platform::Graphics::WaitIdle()
{
}

void Platform::Graphics::StartRenderer(s32 height, bool pal)
{
    // The renderer draws nothing on null: remember the mode for queries, set up nothing
    (void)height;
    (void)pal;
}

void Platform::Graphics::FinishRendererStart()
{
}

void Platform::Graphics::MoveDisplay(const Vector2* offset)
{
    (void)offset;
}

void Platform::Graphics::StepAnimations(const TimeClock* clock)
{
    (void)clock;
}

void Platform::Graphics::SetUpRenderTarget(RenderTargetDescription* target)
{
    (void)target;
}

void Platform::Graphics::Present(const void* commands)
{
    (void)commands;
}

void Platform::Graphics::PresentFromInterrupt(const void* commands)
{
    (void)commands;
}

void Platform::Graphics::WaitSent()
{
}

void Platform::Graphics::ResetBuckets(bool movie)
{
    (void)movie;
}

void Platform::Graphics::SubmitBuckets(bool fromInterrupt)
{
    (void)fromInterrupt;
}

void* Platform::Graphics::AllocFrameMemory(u32 count, u32 size)
{
    return std::malloc(count * size);
}

void Platform::Graphics::FinishFrame()
{
}

void Platform::Graphics::FinishScene(bool effects)
{
    (void)effects;
}

void Platform::Graphics::StartFrame(const FrameStart& frame)
{
    (void)frame;
}

void Platform::Graphics::UseHelperPrograms(u32 set, bool wait)
{
    (void)set;
    (void)wait;
}

Material* Platform::Graphics::MakeFlatMaterial()
{
    // Static storage like the PS2's g_FlatMaterial: the game keeps pointers to it
    static u8 storage[MaterialStorage];
    ConstructMaterial(reinterpret_cast<Material*>(storage));
    return reinterpret_cast<Material*>(storage);
}

Material* Platform::Graphics::FlatMaterial()
{
    return MakeFlatMaterial();
}

void Platform::Graphics::ConstructMaterial(Material* material)
{
    std::memset(material, 0, MaterialStorage);
}

void Platform::Graphics::DestroyMaterial(Material* material)
{
    (void)material;
}

void* Platform::Graphics::MakeParticleMaterial(Material* material)
{
    (void)material;
    return nullptr;
}

void Platform::Graphics::MakeDistortionMaterial(Material* material)
{
    (void)material;
}

void Platform::Graphics::ChainParticleBlocks(u8* const* blocks, s32 count)
{
    (void)blocks;
    (void)count;
}

void Platform::Graphics::EndParticleBlock(u8* block)
{
    (void)block;
}

void Platform::Graphics::InitParticleBlock(u8* block, bool hexagons)
{
    (void)block;
    (void)hexagons;
}

void Platform::Graphics::InitParticleRenderTable(u8* table)
{
    (void)table;
}

void Platform::Graphics::InitParticleGraphics()
{
}

void Platform::Graphics::WriteParticleRenderTable(u8* table, const ParticleLook& look)
{
    (void)table;
    (void)look;
}

void Platform::Graphics::DrawParticleBlock(u8* block, u8* table, const Matrix4x4* matrix, Material* material, f32 time)
{
    (void)block;
    (void)table;
    (void)matrix;
    (void)material;
    (void)time;
}

void Platform::Graphics::DrawDistortionBlock(u8* block, u8* table, const Matrix4x4* matrix, Material* material, f32 time, f32 distortionX, f32 distortionY)
{
    (void)block;
    (void)table;
    (void)matrix;
    (void)material;
    (void)time;
    (void)distortionX;
    (void)distortionY;
}

void Platform::Graphics::LoadDecalView(const Matrix4x4* chunkMatrices, const Matrix4x4* fromChunk)
{
    (void)chunkMatrices;
    (void)fromChunk;
}

void Platform::Graphics::LoadDecalType(DecalType* type)
{
    (void)type;
}

void Platform::Graphics::DrawDecals(DecalData* decals)
{
    (void)decals;
}

void Platform::Graphics::AgeDecal(const f32* place, const s32* frame, s32 variant, DecalLook* look)
{
    (void)place;
    (void)frame;
    (void)variant;
    std::memset(look, 0, sizeof(*look));
}

void Platform::Graphics::SetViewFrustum(f32 near, f32 far, f32 aspect, const s32* fieldOfView)
{
    (void)near;
    (void)far;
    (void)aspect;
    (void)fieldOfView;
}

void Platform::Graphics::LoadChunkPlanes(const Matrix4x4* place)
{
    (void)place;
}

void Platform::Graphics::LoadPortalPlanes(const Matrix4x4* place, const Vector4* portal)
{
    (void)place;
    (void)portal;
}

void Platform::Graphics::LoadCullingView(const Matrix4x4* toClip, const Matrix4x4* toScreen, const Vector4* camera)
{
    (void)toClip;
    (void)toScreen;
    (void)camera;
}

void Platform::Graphics::CullLoadLink(const Matrix4x4* chunk, const Matrix4x4* object)
{
    (void)chunk;
    (void)object;
}

void Platform::Graphics::CullLoadModel(const Matrix4x4* model)
{
    (void)model;
}

void Platform::Graphics::CullTestBox(const Vector4* corners)
{
    (void)corners;
}

void Platform::Graphics::CullTestBoxAt(const Box* box, const Matrix4x4* model)
{
    (void)box;
    (void)model;
}

Platform::Graphics::CullOutcome Platform::Graphics::CullResult()
{
    CullOutcome outcome;
    outcome.outside = 0;
    outcome.clipped = 0;
    outcome.distance = 0;
    return outcome;
}

void Platform::Graphics::CullMatrices(Matrix4x4* clipped, Matrix4x4* toScreen)
{
    (void)clipped;
    (void)toScreen;
}

void Platform::Graphics::ClipBox(const Matrix4x4* view, const Box* box, u32* flags)
{
    (void)view;
    (void)box;
    (void)flags;
}

const Vector4* Platform::Graphics::ViewPlanes()
{
    return nullptr;
}

void Platform::Graphics::ClipBoxAt(const Matrix4x4* view, const Box* box, u32* flags, const Matrix4x4* model)
{
    (void)view;
    (void)box;
    (void)flags;
    (void)model;
}

void Platform::Graphics::DrawPlacedModel(RigidModel* model, ChunkView* view, const Matrix4x4* world)
{
    (void)model;
    (void)view;
    (void)world;
}

RigidModel* Platform::Graphics::LodModelAt(Lod* lod, u32 distance)
{
    (void)lod;
    (void)distance;
    return nullptr;
}

s32 Platform::Graphics::LoadParticleView(const Matrix4x4* matrices, s32 index)
{
    (void)matrices;
    (void)index;
    return 0;
}

void Platform::Graphics::SetFontPage(Material* material, u32 page)
{
    (void)material;
    (void)page;
}

void Platform::Graphics::DrawSprite(Material* material, u32 colour, const Rectangle& place, const Rectangle& texture)
{
    Abort("DrawSprite");
}

void Platform::Graphics::DrawTurnedSprite(Material* material, u32 colour, const Vector2* corners, const Rectangle& texture)
{
    Abort("DrawTurnedSprite");
}

void Platform::Graphics::DrawStrip(Material* material, u32 count, const Vector2* places, const u32* colours)
{
    Abort("DrawStrip");
}

void Platform::Graphics::DrawRigidModel(RigidModel* model, const Matrix4x4* matrix, const ModelLights& lights, u32 mode)
{
    Abort("DrawRigidModel");
}

void Platform::Graphics::DrawSkin(Skin* skin, const Matrix4x4* joints, u32 jointCount, const Matrix4x4* matrix, const ModelLights& lights, u32 mode)
{
    Abort("DrawSkin");
}

void Platform::Graphics::DrawBlendSkin(BlendSkin* skin, const Matrix4x4* joints, u32 jointCount, const Matrix4x4* matrix, const ModelLights& lights, const f32* weights, const s32* shapes, s32 shapeCount, u32 mode)
{
    Abort("DrawBlendSkin");
}

void Platform::Graphics::SetUpShadowPass()
{
}

void Platform::Graphics::BeginShadows()
{
}

void Platform::Graphics::EndShadows()
{
}

void Platform::Graphics::DrawShadowMesh(RigidModel* mesh, const Matrix4x4* toScreen, const Matrix4x4* toCamera, const Matrix4x4* world)
{
    (void)mesh;
    (void)toScreen;
    (void)toCamera;
    (void)world;
}

void Platform::Graphics::BeginScreenModel()
{
}

void Platform::Graphics::SetScreenModelMaterial(Material* material)
{
    (void)material;
}

void Platform::Graphics::SetScreenModelColour(u32 colour)
{
    (void)colour;
}

void Platform::Graphics::AddScreenModelVertex(const Vector4* place)
{
    (void)place;
}

ScreenModel* Platform::Graphics::EndScreenModel()
{
    return nullptr;
}

void Platform::Graphics::DeleteScreenModel(ScreenModel* model)
{
    (void)model;
}

void Platform::Graphics::DrawScreenModel(ScreenModel* model, const Matrix4x4* toScreen, const Matrix4x4* toClip, const Vector4* clip)
{
    (void)model;
    (void)toScreen;
    (void)toClip;
    (void)clip;
}

Material* Platform::Graphics::MakeSkidMaterial(bool subtracting)
{
    (void)subtracting;
    return MakeFlatMaterial();
}

void Platform::Graphics::RestartAnimations(RigidModel* model)
{
    (void)model;
}

void Platform::Graphics::RestartAnimations(Skin* skin)
{
    (void)skin;
}

void Platform::Graphics::RestartAnimations(BlendSkin* skin)
{
    (void)skin;
}

MaterialResource* Platform::Graphics::FirstMaterial(const RigidModel* model)
{
    (void)model;
    return nullptr;
}

GameTexture* Platform::Graphics::NewTexture(u32 id)
{
    return static_cast<GameTexture*>(MakeResource(id, TextureResourceSize));
}

void Platform::Graphics::DeleteTexture(GameTexture* texture)
{
    std::free(texture);
}

void Platform::Graphics::ReadTexture(GameTexture* texture, Stream* stream)
{
    (void)texture;
    u32 size;
    stream->ReadU32(&size);
    stream->Skip(static_cast<s32>(TextureGsHeaderSize + TextureToolsHeaderSize));
    stream->Skip(static_cast<s32>(size - (TextureGsHeaderSize + TextureToolsHeaderSize)));
}

MaterialResource* Platform::Graphics::NewMaterial(u32 id)
{
    return static_cast<MaterialResource*>(MakeResource(id, MaterialResourceSize));
}

void Platform::Graphics::DeleteMaterial(MaterialResource* material)
{
    std::free(material->material);
    std::free(material);
}

void Platform::Graphics::ReadMaterial(MaterialResource* material, Stream* stream)
{
    material->material = static_cast<Material*>(std::calloc(1, MaterialStorage));
    SkipMaterialShaders(stream);
}

RigidModelData* Platform::Graphics::NewModel(u32 id)
{
    return static_cast<RigidModelData*>(MakeResource(id, ModelResourceSize));
}

void Platform::Graphics::DeleteModel(RigidModelData* model)
{
    std::free(model);
}

void Platform::Graphics::ReadModel(RigidModelData* model, Stream* stream)
{
    (void)model;
    s32 subModelCount;
    stream->ReadS32(&subModelCount);
    for (s32 index = 0; index < subModelCount; index++)
    {
        s32 vertexCount;
        stream->ReadS32(&vertexCount);
        s32 size;
        stream->ReadS32(&size);
        stream->Skip(size);
        s32 extraSize;
        stream->ReadS32(&extraSize);
        if (extraSize != 0)
        {
            stream->Skip(extraSize);
        }
    }
}

RigidModel* Platform::Graphics::NewRigidModel(u32 id)
{
    return static_cast<RigidModel*>(MakeResource(id, RigidModelResourceSize));
}

void Platform::Graphics::DeleteRigidModel(RigidModel* model)
{
    std::free(model);
}

void Platform::Graphics::ReadRigidModel(RigidModel* model, Stream* stream)
{
    (void)model;
    s32 version;
    stream->ReadS32(&version);
    SkipRigidModelData(stream);
}

Skin* Platform::Graphics::NewSkin(u32 id)
{
    return static_cast<Skin*>(MakeResource(id, SkinResourceSize));
}

void Platform::Graphics::DeleteSkin(Skin* skin)
{
    std::free(skin);
}

void Platform::Graphics::ReadSkin(Skin* skin, Stream* stream)
{
    (void)skin;
    s32 count;
    stream->ReadS32(&count);
    for (s32 index = 0; index < count; index++)
    {
        s32 id;
        stream->ReadS32(&id);
        s32 size;
        stream->ReadS32(&size);
        s32 vertexCount;
        stream->ReadS32(&vertexCount);
        stream->Skip(size);
    }
}

BlendSkin* Platform::Graphics::NewBlendSkin(u32 id)
{
    return static_cast<BlendSkin*>(MakeResource(id, BlendSkinResourceSize));
}

void Platform::Graphics::DeleteBlendSkin(BlendSkin* skin)
{
    std::free(skin);
}

void Platform::Graphics::ReadBlendSkin(BlendSkin* skin, Stream* stream)
{
    (void)skin;
    s32 count;
    stream->ReadS32(&count);
    s32 shapeCount;
    stream->ReadS32(&shapeCount);
    for (s32 index = 0; index < count; index++)
    {
        s32 partCount;
        stream->ReadS32(&partCount);
        s32 id;
        stream->ReadS32(&id);
        for (s32 part = 0; part < partCount; part++)
        {
            s32 packetSize;
            stream->ReadS32(&packetSize);
            s32 unused;
            stream->ReadS32(&unused);
            stream->Skip(packetSize);
            stream->Skip(static_cast<s32>(ShapeFactorsSize));
            for (s32 shape = 0; shape < shapeCount; shape++)
            {
                s32 shapeSize;
                stream->ReadS32(&shapeSize);
                s32 shapeShapes;
                stream->ReadS32(&shapeShapes);
                stream->Skip(static_cast<s32>(static_cast<u32>(shapeSize) << QuadwordShift));
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
    std::free(mesh);
}

void Platform::Graphics::ReadMesh(RigidModel* mesh, Stream* stream)
{
    (void)mesh;
    s32 version;
    stream->ReadS32(&version);
    SkipRigidModelData(stream);
}

Lod* Platform::Graphics::NewLod(u32 id)
{
    return static_cast<Lod*>(MakeResource(id, LodResourceSize));
}

void Platform::Graphics::DeleteLod(Lod* lod)
{
    std::free(lod);
}

void Platform::Graphics::ReadLod(Lod* lod, Stream* stream)
{
    (void)lod;
    s32 version;
    stream->ReadS32(&version);
    if (version == LodDistancesVersion)
    {
        u32 count;
        stream->ReadU32(&count);
        s32 nearest;
        stream->ReadS32(&nearest);
        s32 farthest;
        stream->ReadS32(&farthest);
        for (u32 distance = 0; distance < LodSwitchCount; distance++)
        {
            s32 value;
            stream->ReadS32(&value);
        }

        s32 unused;
        stream->ReadS32(&unused);
        SkipMeshIds(stream, static_cast<u32>(static_cast<u8>(count)));
        return;
    }

    if (version == LodSquaresVersion)
    {
        s8 count;
        stream->ReadS8(&count);
        s32 nearest;
        stream->ReadS32(&nearest);
        s32 farthest;
        stream->ReadS32(&farthest);
        stream->Skip(static_cast<s32>(LodSwitchCount * sizeof(u32)));
        SkipMeshIds(stream, static_cast<u32>(static_cast<u8>(count)));
    }
}

Sky* Platform::Graphics::NewSky(u32 id)
{
    return static_cast<Sky*>(MakeResource(id, SkyResourceSize));
}

void Platform::Graphics::DeleteSky(Sky* sky)
{
    std::free(sky);
}

void Platform::Graphics::ReadSky(Sky* sky, Stream* stream)
{
    (void)sky;
    u32 unused;
    stream->ReadU32(&unused);
    s32 count;
    stream->ReadS32(&count);
    SkipMeshIds(stream, static_cast<u32>(count));
}

void Platform::Graphics::DrawChunkSky(Sky* sky, RenderView* view)
{
    Abort("DrawChunkSky");
}

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
