// Particles and decals: the PS2's particle blocks, render tables and decal blocks on the host
// (src/platform/ps2/renderer/particles.cpp and effects.cpp).
//
// A block keeps the game's ParticleRecord array at ParticleBlockHeader (0x20) and holds the host's chain where the
// PS2 kept DMA NEXT tags. Placement is Platform::Math::ParticleBlockView, in the game's own screen coordinates
// (1/16 pixel, middle 2048, Y down). The draws build vertices on the CPU: one instanced quad per particle and STQ
// quads for decals (type 0x13). AgeDecal and DrawDecals handle decal aging and drawing.
#include "host_graphics.h"
#include "draws_host.h"
#include "particle_billboard.h"

#include "game/decals.h"
#include "game/controllers.h"
#include "game/math.h"
#include "game/graphicstables.h"
#include "game/particles.h"
#include "game/renderer.h"
#include "game/view.h"

#include "shaders/fs_particle.bin.h"
#include "shaders/fs_stq.bin.h"
#include "shaders/vs_particle.bin.h"
#include "shaders/vs_stq.bin.h"

#include <cmath>
#include <cstring>
#include <vector>

extern "C"
{
    // The scene's view (game/view.h's g_RenderView, whose symbol is G_RendRel), which DrawDecals projects the decals'
    // places through exactly as the PS2's renderer reads it. Declared weak: a headless proof links no game data, and
    // a null view means the decals are drawn through the identity (which is what the proof's own view stands up).
    extern char G_RendRel[] __attribute__((weak));
}

namespace Platform::Graphics
{
namespace
{
// The game's screen coordinates: 1/16th of a pixel with the middle at 2048 (src/platform/ps2/renderer/renderer.h's
// GsScreenMiddleFloat and GsSubpixels), so the display's half width is 2048 in whole pixels or 2048 * 16 units
constexpr f32 GsScreenMiddleUnits = 2048.0f * 16.0f;
constexpr f32 GsUnitsPerPixel = 16.0f;
// The GS depth the game's clip-to-screen matrix leaves: 0 at the far plane and twice this at the near
// (src/platform/ps2/renderer/renderer.h's GsDepthRange, frame.cpp's SetUpRenderTarget)
constexpr f32 GsDepthRange = 8388607.0f;
// The clip w below which VU1 collapses a particle (src/platform/ps2/renderer/particles.cpp's ParticleNearClip)
constexpr f32 ParticleNearClip = 0.5f;
// How many steps a particle's life is broken into (Platform::ParticleRenderSteps) and how many records each kind of
// block holds (src/game/particles.cpp's BlockParticles and HexagonBlockParticles)
constexpr u32 BlockParticles = 32;
constexpr u32 HexagonBlockParticles = 12;

// The particles' and decals' programs, made lazily on first use (after BgfxInit; owned by the process like the
// shared 2D objects)
bgfx::ProgramHandle g_particleProgram = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_particleSampler = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_particleUniform = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_particleAlphaUniform = BGFX_INVALID_HANDLE;
bgfx::ProgramHandle g_stqProgram = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_stqSampler = BGFX_INVALID_HANDLE;
bgfx::VertexBufferHandle g_quadBuffer = BGFX_INVALID_HANDLE;
bool g_made = false;

// A particle an instance: the quad's four corners as the block's matrix left them (the game's screen coordinates with
// the projection's w), its premultiplied colour and the texture's rectangle. Each corner is projected on its own: the
// shape is in the particle's own units, so adding it before the matrix is what keeps the quad's size independent of
// its distance. The six quadwords are what the shader's i_data0..5 read, in order
struct HostParticleInstance
{
    f32 clip00[4]; // the quad's corner (0, 0): the shape's first corner A
    f32 clip01[4]; // (0, 1): the shape's other corner B (D - up)
    f32 clip11[4]; // (1, 1): the opposite corner D
    f32 clip10[4]; // (1, 0): A + up
    f32 colour[4];
    f32 uv[4];
};
static_assert(sizeof(HostParticleInstance) == 6 * sizeof(f32) * 4, "a particle is six quadwords");

// The unit quad's four vertices, in the TRISTRIP's order: (0, 0), (0, 1), (1, 0), (1, 1). The strip is then the
// console's own: corners A, B, A + up, D, which VU1's packets draw as the triangles (A, B, A + up) and (A + up, B, D)
// (zero_kick's class C, the vertices with ADC on the first two). The old order was the perimeter (0, 0), (0, 1), (1, 1),
// (1, 0), and a strip over the perimeter draws (A, A + up, D) and (A + up, D, B): the two overlap in a triangle, which
// adds up in the additive modes into a bright ray, and leave the bottom wedge of the quad empty
struct QuadVertex
{
    f32 x;
    f32 y;
    f32 z;
};

const QuadVertex QuadVertices[4] = {{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}};

bgfx::VertexLayout QuadLayout()
{
    bgfx::VertexLayout layout;
    layout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).end();
    return layout;
}

// The layout the decals' and the distortion's vertices come in: clip-space positions, the texture's S, T and Q, and
// the colour
struct StqVertex
{
    f32 x;
    f32 y;
    f32 z;
    f32 s;
    f32 t;
    f32 q;
    u32 colour;
};

bgfx::VertexLayout StqLayout()
{
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();
    return layout;
}

void MakeShared()
{
    if (g_made)
    {
        return;
    }
    g_made = true;
    bgfx::ShaderHandle particleVs = bgfx::createShader(bgfx::makeRef(vs_particle_glsl, sizeof(vs_particle_glsl)));
    bgfx::ShaderHandle particleFs = bgfx::createShader(bgfx::makeRef(fs_particle_glsl, sizeof(fs_particle_glsl)));
    g_particleProgram = bgfx::createProgram(particleVs, particleFs, true);
    g_particleSampler = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);
    g_particleUniform = bgfx::createUniform("u_particle", bgfx::UniformType::Vec4);
    g_particleAlphaUniform = bgfx::createUniform("u_particleAlpha", bgfx::UniformType::Vec4);
    bgfx::ShaderHandle stqVs = bgfx::createShader(bgfx::makeRef(vs_stq_glsl, sizeof(vs_stq_glsl)));
    bgfx::ShaderHandle stqFs = bgfx::createShader(bgfx::makeRef(fs_stq_glsl, sizeof(fs_stq_glsl)));
    g_stqProgram = bgfx::createProgram(stqVs, stqFs, true);
    g_stqSampler = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);

    const bgfx::Memory* memory = bgfx::copy(QuadVertices, sizeof(QuadVertices));
    g_quadBuffer = bgfx::createVertexBuffer(memory, QuadLayout());
}

// A block's host header, or null when the block isn't one of ours
HostParticleBlockHead* HeadOf(u8* block)
{
    if (block == nullptr)
    {
        return nullptr;
    }

    auto* head = reinterpret_cast<HostParticleBlockHead*>(block);
    return head->magic == HostParticleBlockMagic ? head : nullptr;
}

const HostParticleTable* TableOf(const u8* table)
{
    if (table == nullptr)
    {
        return nullptr;
    }

    const auto* host = reinterpret_cast<const HostParticleTable*>(table);
    return host->magic == HostParticleTableMagic ? host : nullptr;
}

// OLEG's UI particle material, the one MakeParticleMaterial is called on from the game's OLEG::SetUpParticles. Retail
// binds the FIRST page's picture to it when it builds it (materials.cpp's MakeParticleMaterial reaches into
// ParticlePageMaterial(0) and takes its shader's texture); the host reads the pages later, so the bind happens when page
// 0 arrives. Without it the gem's sparkle samples the 1x1 white texture and draws as flat coloured rectangles
HostMaterial* g_uiParticleMaterial = nullptr;
// Set while the page's own mode materials are being made: those go through MakeParticleMaterial too and must not be
// mistaken for OLEG's
bool g_inPageMaterials = false;

// OLEG's material and the first page have no fixed order (the game reads the pages when the default chunk loads and builds
// OLEG's material with it), so the picture is taken from whatever page 0 has right now, from either side of the join
void BindUiParticlePicture()
{
    if (g_uiParticleMaterial == nullptr)
    {
        return;
    }

    // ParticlePageMaterial is the page's own mode material (its host storage), not a MaterialResource
    const auto* source = reinterpret_cast<const HostMaterial*>(ParticlePageMaterial(0));
    if (source == nullptr)
    {
        return;
    }
    g_uiParticleMaterial->texture = source->texture;
    g_uiParticleMaterial->textureWidth = source->textureWidth;
    g_uiParticleMaterial->textureHeight = source->textureHeight;
    g_uiParticleMaterial->gameTexture = source->gameTexture;
    if (bgfx::isValid(HostMaterialBoundTexture(source)) || source->gameTexture != nullptr)
    {
        g_uiParticleMaterial->flags |= HostMaterialTextured;
    }
}

// The blend state a particle page's mode draws with (src/platform/ps2/renderer/particles.cpp ReadParticlePageData:
// the page material's own, additive, subtractive and cutout; both blended modes write no depth)
u64 ParticleBlendState(u32 blend)
{
    switch (blend)
    {
    case HostParticleBlendAdditive:
        // The GS's additive preset (shadersettings.cpp's InitAlphaPresets: PresetAdd = (Cs - 0) * As + Cd), so the
        // source is weighted by its alpha. bgfx's BGFX_STATE_BLEND_ADD is (ONE, ONE) = Cs + Cd, which over-brightens
        // every faded particle; the repo's own validated mapping is SRC_ALPHA/ONE (material_system.cpp's PresetBlend)
        return BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_ONE);
    case HostParticleBlendSubtractive:
        // The GS's subtractive preset: the source taken from the destination (its PresetSubtract). SUB with (ONE, ONE) is
        // src - dst, which inverts the frame instead of subtracting from it: the equation has to be REVSUB (dst - src)
        // The source is weighted by its alpha, as the PS2's PresetSubtract is (Cd - Cs * As): a faded or transparent part of
        // a quad takes nothing from the frame. With ONE the whole quad's colour came off it, which tinted the frame wherever a
        // large one stood close to the camera
        return BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_ONE) |
               BGFX_STATE_BLEND_EQUATION(BGFX_STATE_BLEND_EQUATION_REVSUB);
    case HostParticleBlendCutout:
        // Retail's BlendCutout mode is not blended at all (ReadParticlePageData's third mode shader sets blends = 0):
        // it is cut out by the GS's alpha test (SetPageAlphaTest: GEQUAL against 5), which the draw's marker state runs
        // through the material's alpha reference instead of a blend. Blending it made every cut-out particle a
        // translucent ghost
        return 0;
    default:
        // The page material's own (BlendPageMaterial): the normal alpha blend
        return BGFX_STATE_BLEND_ALPHA;
    }
}

// The particles', decals' and distortion's debug readings (the headless proof's): the vertices of the last draw and
// an FNV-1a checksum of them
struct FxDebug
{
    u32 vertices;
    u32 checksum;
};

FxDebug g_particleDebug = {0, 0};
FxDebug g_decalDebug = {0, 0};

// The last DrawParticleBlock's first record's projected place (its clip-space columns, w the projection's), what
// the headless proof of the block's placement reads
Vector4 g_particleDebugPlace = {0.0f, 0.0f, 0.0f, 1.0f};
// The same record's modulate colour (the GS's: the colour's channels over 255, the alpha over 128)
Vector4 g_particleDebugColour = {0.0f, 0.0f, 0.0f, 0.0f};

u32 HashBytes(u32 hash, const void* bytes, size_t size)
{
    auto* at = static_cast<const u8*>(bytes);
    for (size_t index = 0; index < size; index++)
    {
        hash = (hash ^ at[index]) * 16777619u;
    }

    return hash;
}

// The vector unit's float-to-integer conversions the decals' aging program uses, as PCSX2 makes them (the aging's
// outputs land in the decal block byte for byte, so the host's own casts are not enough): FTOI0 truncates towards
// zero, FTOI12 first scales by 4096 (12.4 fixed point) and truncates, both saturating at the ends of their range
// once the exponent reaches 2^31
u32 Ftoi0(f32 value)
{
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    if ((bits & 0x7F800000u) >= 0x4F000000u)
    {
        return (bits & 0x80000000u) != 0 ? 0x80000000u : 0x7FFFFFFFu;
    }

    return static_cast<u32>(static_cast<s32>(value));
}

u32 Ftoi12(f32 value)
{
    const f32 scaled = value * 4096.0f;
    u32 bits;
    std::memcpy(&bits, &scaled, sizeof(bits));
    if ((bits & 0x7F800000u) >= 0x4F000000u)
    {
        return (bits & 0x80000000u) != 0 ? 0x80000000u : 0x7FFFFFFFu;
    }

    return static_cast<u32>(static_cast<s32>(scaled));
}

// The vector unit's integer-to-float conversion of the frame's half words (the 15-bit fixed point the decal frames
// are stored in: 32768 to the unit)
f32 Itof15(s32 value)
{
    return static_cast<f32>(value) * 0x1p-15f;
}

// The decals' view: the chunk the places are in (LoadDecalView's, from the camera's chunk) and the matrix of the
// chunk they came from when it is another one (the PS2's VU0 gets the same pair)
bool g_decalViewSet = false;
Matrix4x4 g_decalChunkMatrix;
Matrix4x4 g_decalFromChunkMatrix;
bool g_decalOtherChunk = false;
// The type LoadDecalType was handed: the variants the aging reads
const DecalType* g_decalType = nullptr;
// The particle a record makes at a draw (see the file's header): whether it draws at all, and the instance it makes
bool ParticleAt(const ParticleRecord& record, const HostParticleTable& table, const Matrix4x4& matrix, f32 time,
                f32* uv, HostParticleInstance* out)
{
    // The record's life at 64 steps (LifeSteps, src/game/particles.cpp): dead particles hold DeadParticle, blocks
    // never handed out hold FreeLifeScale with a spawn time far in the future, and neither can draw
    const f32 age = time - record.spawnTime;
    const f32 life = age * record.lifeScale;
    if (record.lifeScale == 0.0f || !(life >= 0.0f) || life >= static_cast<f32>(ParticleRenderSteps))
    {
        return false;
    }

    u32 step = static_cast<u32>(life);
    if (step >= ParticleRenderSteps)
    {
        step = ParticleRenderSteps - 1;
    }

    // Its place: its start, its velocity over its age and the system's gravity over the square of the age (the render
    // table's head keeps the gravity). The gravity's term is the whole of g*age*age, not half of it: retail's VU1 works
    // the same curve (TT-Lab's ParticleSimulation.PositionAt: "start + velocity x t + (0, gravity, 0) x t^2 ... the
    // game's VU1 works that out every frame"), and the game's own bounce solver solves that curve rather than a
    // half-acceleration one (PlaneTimes: t = (-v +- sqrt(v*v - 4*g*h)) / 2g). Halving it left every particle short of
    // where the game puts it
    const f32 local[3] = {record.start[0] + record.velocity[0] * age,
                          record.start[1] + record.velocity[1] * age + table.gravity * age * age,
                          record.start[2] + record.velocity[2] * age};
    // The game's matrices are row-vector ones (a point times the matrix: rows 0 to 2 are the axes, row 3 the
    // translation); the block's is to the game's screen coordinates
    f32 clip[4];
    for (u32 column = 0; column < 4; column++)
    {
        clip[column] = local[0] * matrix.m[0][column] + local[1] * matrix.m[1][column] +
                       local[2] * matrix.m[2][column] + matrix.m[3][column];
    }

    // The place is passed through as the game's screen coordinates (1/16th-pixel units, middle 2048, Y down) with the
    // GS depth the game's clip-to-screen matrix leaves (0 at the far plane, twice the depth range at the near): the
    // particle vertex shader does the one conversion (its u_particle uniform is the middle, the scales and the depth
    // range), so converting here too double-applied it (the shader re-centred an already-NDC x by 32768 ->
    // gl_Position.x ~= -8, i.e. every particle off-screen). Retail's VU1 consumes the matrix-to-screen and emits GS
    // XYZ2; the shader is the host's NDC step, depth included
    if (!(clip[3] >= ParticleNearClip))
    {
        return false;
    }

    // The quad's corners are the particle's screen-space place plus the table's shape along the screen's axes, as the
    // console's VU1 builds them. The offsets go into screen coordinates rather than the particle's own place, so the
    // quad faces the camera whatever the emitter's axes are (see particle_billboard.h); the emitter's own frame reaches
    // the particle through the block matrix, which is where a flamethrower's puffs take their direction.
    const f32* shape = table.steps[step].shape;
    // The corners in the order of corners[] below: clip00 = A, clip01 = B (the other corner, D - up), clip11 = D (the
    // opposite corner), clip10 = A + up. The console's VU1 builds the same four (vf12 = A, vf21 = B, vf22 = A + up,
    // vf16 = D), and the quad's strip and its texels follow from these slots
    const f32 offsets[4][2] = {
        {shape[0], shape[1]},
        {shape[4] - shape[2], shape[5] - shape[3]},
        {shape[4], shape[5]},
        {shape[0] + shape[2], shape[1] + shape[3]},
    };
    // The screen's units a world unit takes along the camera's right and up, and the factor that puts the shape's x on
    // the screen with the same size per unit as its y (the screen's x is squashed against its y)
    f32 across = 0.0f;
    f32 down = 0.0f;
    ParticleScreenScale(&across, &down);
    const f32 aspectFix = ShapeScreenRatio(across, down);
    f32* corners[4] = {out->clip00, out->clip01, out->clip11, out->clip10};
    for (u32 corner = 0; corner < 4; corner++)
    {
        // The shape's x runs along the screen's x (to the right) and its y upwards, and the screen's y runs down
        const f32 alongRight = offsets[corner][0] * ParticleSizeUnit * ShapeHalfExtent * aspectFix * across;
        const f32 alongUp = offsets[corner][1] * ParticleSizeUnit * ShapeHalfExtent * down;
        corners[corner][0] = clip[0] + alongRight;
        corners[corner][1] = clip[1] - alongUp;
        corners[corner][2] = clip[2];
        corners[corner][3] = clip[3];

        if (!(corners[corner][3] >= ParticleNearClip))
        {
            return false;
        }
    }

    // The colour: the table's bytes as the GS's modulate sees them. The colour's channels run 0 to 255 and the alpha
    // 0 to 128 (the GS's alpha register's range). The data's alpha keys run past that (they go up to 255), and the
    // console's shader hands the byte on and the GS holds it to what it takes: a key of 255 is fully opaque, not
    // twice as opaque as the texture's own alpha.
    //
    // The modulate is over 255 on the colour's channels and over 128 on the alpha: the smoke the console draws is the
    // grey the data holds, and over 128 it came out white
    out->colour[0] = static_cast<f32>(table.steps[step].colour[0]) / 255.0f;
    out->colour[1] = static_cast<f32>(table.steps[step].colour[1]) / 255.0f;
    out->colour[2] = static_cast<f32>(table.steps[step].colour[2]) / 255.0f;
    const u32 alpha = table.steps[step].colour[3] < 128u ? table.steps[step].colour[3] : 128u;
    out->colour[3] = static_cast<f32>(alpha) / 128.0f;

    std::memcpy(out->uv, uv, sizeof(out->uv));
    return true;
}

// The corner as the texel it points at: the console's shader puts the corner in the GS's UV register as it is, and the
// GS reads the low 14 bits of it (the pixel and its fraction in 12.4) as the texel
f32 CornerTexel(s32 corner)
{
    return static_cast<f32>(corner & 0x3FFF) / 16.0f;
}

// Four texture fractions from a table rectangle (console 12.4 fixed point: pixel and fraction in the low 14 bits of the
// GS's UV register). A size of 0 means no texture is bound, so the whole white texture is sampled.
//
// The rectangle is a small region of a page atlas, magnified into a puff. Retail pages have no mipmaps, so the host
// must sample the full-resolution page too; a mip averages neighbouring atlas cells and turns the puff into a flat
// shape.
void TableUv(const HostParticleTable& table, u32 textureWidth, u32 textureHeight, f32* uv /* u0, v0, u1, v1 */)
{
    const f32 width = textureWidth != 0 ? static_cast<f32>(textureWidth) : 1.0f;
    const f32 height = textureHeight != 0 ? static_cast<f32>(textureHeight) : 1.0f;
    uv[0] = CornerTexel(table.texture[0]) / width;
    uv[1] = CornerTexel(table.texture[1]) / height;
    uv[2] = CornerTexel(table.texture[2]) / width;
    uv[3] = CornerTexel(table.texture[3]) / height;
}

// Where the block's records are (ParticleBlockHeader, src/game/particles.h)
ParticleRecord* RecordsOf(u8* block)
{
    return reinterpret_cast<ParticleRecord*>(block + 0x20);
}

// The chain's next block (the PS2's DMA NEXT tag), or null at its end
u8* NextBlock(const HostParticleBlockHead* head)
{
    return (head->tag[0] & 0x70000000) == 0x20000000 ? reinterpret_cast<u8*>(static_cast<uintptr_t>(head->tag[1]))
                                                     : nullptr;
}
}
}

// The particles', decals' and distortion's debug readings (the headless proof's)

u32 Platform::Graphics::HostFxDebug_ParticleVertices()
{
    return g_particleDebug.vertices;
}

u32 Platform::Graphics::HostFxDebug_ParticleChecksum()
{
    return g_particleDebug.checksum;
}

Vector4 Platform::Graphics::HostFxDebug_ParticlePlace()
{
    return g_particleDebugPlace;
}

Vector4 Platform::Graphics::HostFxDebug_ParticleColour()
{
    return g_particleDebugColour;
}

u32 Platform::Graphics::HostFxDebug_DecalVertices()
{
    return g_decalDebug.vertices;
}

u32 Platform::Graphics::HostFxDebug_DecalChecksum()
{
    return g_decalDebug.checksum;
}

namespace Platform::Graphics
{
// The blocks' chains and their headers (src/platform/ps2/renderer/particles.cpp's InitParticleBlock, EndParticleBlock
// and ChainParticleBlocks)

// A block's first quadword is its DMA tag: its ID (the top three bits) says whether the chain goes on (NEXT, at the
// second word) or ends (RET)
constexpr u32 ParticleTagId = 0x70000000;
constexpr u32 ParticleNextTag = 0x20000000;
constexpr u32 ParticleReturnTag = 0x60000000;

void EndParticleBlock(u8* block)
{
    auto* words = reinterpret_cast<u32*>(block);
    words[1] = 0;
    words[0] = (words[0] & ~ParticleTagId) | ParticleReturnTag;
}

void ChainParticleBlocks(u8* const* blocks, s32 count)
{
    if (blocks == nullptr || count <= 0)
    {
        return;
    }

    EndParticleBlock(blocks[count - 1]);
    for (s32 block = count - 2; block >= 0; block--)
    {
        if (blocks[block] == nullptr)
        {
            continue;
        }

        auto* words = reinterpret_cast<u32*>(blocks[block]);
        words[0] = (words[0] & ~ParticleTagId) | ParticleNextTag;
        words[1] = static_cast<u32>(reinterpret_cast<uintptr_t>(blocks[block + 1]));
    }
}

// A block's host header: its magic, its kind and its chain ended (the game chains it when it hands it out)
void InitParticleBlock(u8* block, bool hexagons)
{
    auto* head = reinterpret_cast<HostParticleBlockHead*>(block);
    std::memset(head, 0, sizeof(*head));
    head->magic = HostParticleBlockMagic;
    head->hexagons = hexagons ? 1u : 0u;
    head->count = hexagons ? HexagonBlockParticles : BlockParticles;
    head->tag[0] = ParticleReturnTag;
}

// The render tables (InitParticleRenderTable, WriteParticleRenderTable)

void InitParticleRenderTable(u8* table)
{
    std::memset(table, 0, sizeof(HostParticleTable));
    reinterpret_cast<HostParticleTable*>(table)->magic = HostParticleTableMagic;
}

// The look into the table, in the host's own form (the PS2 sends the same values to VU1's memory)
void WriteParticleRenderTable(u8* table, const ParticleLook& look)
{
    auto* host = reinterpret_cast<HostParticleTable*>(table);
    host->magic = HostParticleTableMagic;
    host->hexagons = look.hexagons ? 1u : 0u;
    host->gravity = look.gravity;
    for (u32 corner = 0; corner < 4; corner++)
    {
        host->texture[corner] = look.texture[corner];
    }

    for (u32 corner = 0; corner < 12; corner++)
    {
        host->hexagonCorners[corner] = look.hexagonCorners[corner];
    }

    for (u32 step = 0; step < ParticleRenderSteps; step++)
    {
        for (u32 value = 0; value < 6; value++)
        {
            host->steps[step].shape[value] = look.steps[step].shape[value];
        }

        for (u32 channel = 0; channel < 4; channel++)
        {
            host->steps[step].colour[channel] = look.steps[step].colour[channel];
        }
    }
}

// The particles (DrawParticleBlock): instanced quads, one an instance, through the page material

// The UI's 2D particles' material (the PS2's MakeParticleMaterial, src/platform/ps2/renderer/materials.cpp): the flat
// material's kind in the UI's bucket, additive and textured with STQ coordinates. The first page's picture is bound
// to it once the page is uploaded (HostSetMaterialTexture); until then the white texture stands in, which the flat
// path draws as the colour alone
void* MakeParticleMaterial(Material* material)
{
    MakeShared();
    ConstructMaterial(material);
    auto* host = reinterpret_cast<HostMaterial*>(material);
    host->program = g_stqProgram.idx;
    host->flags = HostMaterialFlat | HostMaterialTextured;
    host->sampler = HostInvalidHandle;
    host->texture = HostWhiteTexture().idx;
    host->blend = HostParticleBlendAdditive;
    // The UI's bucket, as retail's own MakeParticleMaterial names it (materials.cpp: material->bucket = BucketUi): this
    // is the 2D particles' material, drawn through the 2D path, so its bucket is the UI's
    host->bucket = 24;
    // OLEG's own, not one of a page's modes: its picture is bound when page 0 is read (MakePageModeMaterials)
    if (!g_inPageMaterials)
    {
        g_uiParticleMaterial = host;
        BindUiParticlePicture();
    }
    return material;
}

// A page's per-mode materials (graphics.cpp's ReadParticlePageData calls this): the PS2's ReadParticlePageData makes
// a material for each blend mode (MakeModeMaterial + SetUpBlendedShader: additive, subtractive and cutout shaders,
// each carrying the page shader's texture and mips). The host makes one material per mode, all through the STQ
// particle program so they sample the page's picture with STQ coordinates, each with the blend preset its mode
// draws with (ParticleBlendState). page->materials keeps them (pointer storage: the game reads materials[mode])
void MakePageModeMaterials(ParticlePage* page)
{

    bgfx::TextureHandle pageTexture = BGFX_INVALID_HANDLE;
    GameTexture* gameTexture = nullptr;
    // The page's texture size: the particles' UVs are fractions of it (TableUv), and the page material carries it. Made with
    // 0 the fractions become the raw GS rectangle (in 1/16ths of a pixel), which samples a garbage texel -- every particle then
    // draws as nothing at all
    u32 pageWidth = 0;
    u32 pageHeight = 0;
    if (page->material != nullptr && page->material->material != nullptr)
    {
        auto* pageMaterial = reinterpret_cast<HostMaterial*>(page->material->material);
        gameTexture = pageMaterial->gameTexture;
        pageTexture = HostMaterialBoundTexture(pageMaterial);
        pageWidth = pageMaterial->textureWidth;
        pageHeight = pageMaterial->textureHeight;
        if (!bgfx::isValid(pageTexture))
        {
            pageTexture = HostWhiteTexture();
        }
    }

    // The page texture's size, from the upload the material's texture went through (HostTextureCache::FindSize, the same lookup
    // the font pages' glyph rectangles use). The page's material carries no size of its own, and the particles' UVs are fractions
    // of this (TableUv): with 0 the fractions become the raw GS rectangle, in 1/16ths of a pixel, and every particle samples a
    // garbage texel -- the whole effect then draws as nothing at all
    if (pageWidth == 0 || pageHeight == 0)
    {
        HostTextures().FindSize(gameTexture, &pageWidth, &pageHeight);
    }

    const HostParticleBlend modes[4] = {HostParticleBlendAdditive, HostParticleBlendSubtractive,
                                        HostParticleBlendPage, HostParticleBlendCutout};
    g_inPageMaterials = true;
    for (u32 mode = 0; mode < 4; mode++)
    {
        auto* material = static_cast<Material*>(std::calloc(1, MaterialStorage));
        MakeParticleMaterial(material);
        auto* host = reinterpret_cast<HostMaterial*>(material);
        host->blend = modes[mode];
        // The page's picture: when the upload already exists, its handle; otherwise leave the material untextured
        // rather than pinning the white fallback here. HostMaterialBoundTexture then resolves gameTexture the first
        // time a draw needs it, which is what makes a page whose texture finishes uploading after this call draw its
        // art instead of white for the rest of the session (the page materials are made once, when the page is read)
        host->texture = bgfx::isValid(pageTexture) && pageTexture.idx != HostWhiteTexture().idx ? pageTexture.idx
                                                                                              : HostInvalidHandle;
        host->textureWidth = pageWidth;
        host->textureHeight = pageHeight;
        if (bgfx::isValid(pageTexture) || gameTexture != nullptr)
        {
            host->flags |= HostMaterialTextured;
        }

        // The page's game texture: the modes' draws resolve it through HostTextures like a read material's would,
        // so the page's picture is sampled (and stays alive) whether or not it is already uploaded
        host->gameTexture = gameTexture;
        // The particles' bucket, as retail's page materials name it (particles.cpp: material->bucket = BucketParticles);
        // the draws go through HostParticlesView, so this keeps a page material that reaches the 2D path in its own bucket
        host->bucket = 22;
        page->materials[mode] = material;
    }
    g_inPageMaterials = false;

    // OLEG's 2D particles sample the first page's picture, as retail binds it when it builds their material
    if (page == &g_ParticlePages[0])
    {
        BindUiParticlePicture();
    }
}


void DrawParticleBlock(u8* block, u8* table, const Matrix4x4* matrix, Material* material, f32 time)
{
    const HostParticleTable* host = TableOf(table);
    if (host == nullptr || HeadOf(block) == nullptr || matrix == nullptr)
    {
        
        return;
    }

    // The texture the page's material samples: its stored handle, else the upload of the game texture its first
    // textured shader reads (HostMaterialBoundTexture) -- the page's picture. The white one stands in only when the
    // material samples nothing at all (the flat path's colour alone)
    auto* hostMaterial = reinterpret_cast<HostMaterial*>(material);
    bgfx::TextureHandle texture = HostMaterialBoundTexture(hostMaterial);
    if (!bgfx::isValid(texture))
    {
        texture = HostWhiteTexture();
    }

    const u32 textureWidth = hostMaterial != nullptr ? hostMaterial->textureWidth : 0;
    const u32 textureHeight = hostMaterial != nullptr ? hostMaterial->textureHeight : 0;
    f32 uv[4];
    TableUv(*host, textureWidth, textureHeight, uv);

    // Every block of the chain the game handed in, every record of each (the PS2's DMA NEXT tags walked them)
    std::vector<HostParticleInstance> instances;
    instances.reserve(BlockParticles);
    for (u8* at = block; at != nullptr; at = NextBlock(HeadOf(at)))
    {
        const HostParticleBlockHead* blockHead = HeadOf(at);
        ParticleRecord* records = RecordsOf(at);
        const u32 count = blockHead->count < BlockParticles ? blockHead->count : BlockParticles;
        for (u32 record = 0; record < count; record++)
        {
            HostParticleInstance instance;
            if (ParticleAt(records[record], *host, *matrix, time, uv, &instance))
            {
                instances.push_back(instance);
            }
        }
    }

    // What the headless proof reads: the geometry exactly as it was built
    g_particleDebug.vertices = static_cast<u32>(instances.size()) * 4;
    g_particleDebug.checksum = instances.empty() ? 0u
                                                 : HashBytes(5381u, instances.data(),
                                                             instances.size() * sizeof(HostParticleInstance));
    if (!instances.empty())
    {
        const HostParticleInstance& first = instances.front();
        g_particleDebugPlace = {first.clip00[0], first.clip00[1], first.clip00[2], first.clip00[3]};
        g_particleDebugColour = {first.colour[0], first.colour[1], first.colour[2], first.colour[3]};
    }
    else
    {
        g_particleDebugPlace = {0.0f, 0.0f, 0.0f, 1.0f};
    }
    if (instances.empty() || hostMaterial == nullptr || hostMaterial->program == HostInvalidHandle)
    {
        return;
    }

    MakeShared();
    const bool quadValid = bgfx::isValid(g_quadBuffer);
    const u32 available = bgfx::getAvailInstanceDataBuffer(static_cast<u32>(instances.size()), sizeof(HostParticleInstance));
    if (!quadValid || available == 0)
    {
        return;
    }

    bgfx::InstanceDataBuffer buffer;
    bgfx::allocInstanceDataBuffer(&buffer, static_cast<u32>(instances.size()), sizeof(HostParticleInstance));
    std::memcpy(buffer.data, instances.data(), instances.size() * sizeof(HostParticleInstance));

    // 1 / (the display's half width in 1/16th-pixel units) and the same for the height, then the middle the game's
    // coordinates are around (2048 pixels) and 1 / the GS depth range, which the shader takes the GS depth back to the
    // scene's own clip z with
    const f32 uniform[4] = {2.0f / (static_cast<f32>(HostDisplayWidth()) * GsUnitsPerPixel),
                            2.0f / (static_cast<f32>(HostDisplayHeight()) * GsUnitsPerPixel), GsScreenMiddleUnits,
                            1.0f / GsDepthRange};

    bgfx::setViewMode(HostParticlesView, bgfx::ViewMode::Sequential);
    HostSceneSetView(HostParticlesView);
    // The GS's own test is ZTST 2, whose host mirror is LEQUAL (material_system.cpp's DepthState: the two depth conventions
    // run the other way about, and GEQUAL keeps the equal case). GEQUAL here tested "farther than the scene", which rejected
    // every particle standing in front of the ground
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LEQUAL |
                   ParticleBlendState(hostMaterial->blend) | BGFX_STATE_PT_TRISTRIP);
    // The page's own filter: the GS's TEX1 takes the magnifying and the minifying filter from the page material's
    // shader, and the page's material asks for the bilinear one (the file's setting byte 23). A puff is a small region
    // of the page blown up, so this is what makes it the soft round shape the data holds: the nearest texel shows the
    // region's own texels as hard blocks (the mist's streaks, the nitro's spikes). The mip level is the nearest, the
    // console's minifying filter taking the nearest level and blending inside it
    bgfx::setTexture(0, g_particleSampler, texture,
                     BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | HostMaterialFilter(hostMaterial));
    bgfx::setVertexBuffer(0, g_quadBuffer);
    bgfx::setInstanceDataBuffer(&buffer);
    bgfx::setUniform(g_particleUniform, uniform);
    // The alpha test every one of the page's modes carries (retail's SetPageAlphaTest: ATST GEQUAL, reference 5 of
    // 128, and a sample under it is kept in neither buffer): the cutout mode is not the only one that drops what is
    // too transparent, the blended modes drop it as well. The test runs in the fragment shader (the host's own
    // fs_particle.sc), which discards the sample and leaves the frame as it was
    constexpr f32 AlphaGeq = 5.0f;
    const f32 alphaTest[4] = {AlphaGeq, AlphaGeq / 128.0f, 0.0f, 0.0f};
    bgfx::setUniform(g_particleAlphaUniform, alphaTest);
    bgfx::submit(HostParticlesView, g_particleProgram);
}

// The decals (LoadDecalView, LoadDecalType, AgeDecal, DrawDecals)

void LoadDecalView(const Matrix4x4* chunkMatrices, const Matrix4x4* fromChunk)
{
    g_decalViewSet = true;
    if (chunkMatrices != nullptr)
    {
        g_decalChunkMatrix = *chunkMatrices;
    }

    g_decalOtherChunk = fromChunk != nullptr;
    if (fromChunk != nullptr)
    {
        g_decalFromChunkMatrix = *fromChunk;
    }
}

// Retail hands the type's variants to the VU0 aging program here (ps2/renderer/particles.cpp's LoadDecalType DMAs them
// to Vu0VariantsAddress and waits for the channel). The host runs the same aging on the processor instead
// (AgeDecalKeys, which takes the variant's keys as they sit in the type), so the type only has to be kept: the pointer
// it stores is what AgeDecal reads the variants through
void LoadDecalType(DecalType* type)
{
    g_decalType = type;
}

// The body both of the aging's programs share, from the parameter on (src/platform/ps2/renderer/particles.cpp's
// AgeDecal runs the VU0 program at 0x1C0 for a decal in the camera's chunk, 0x360 for one in another; the other
// entry moves the place through that chunk's matrix first and then runs this). The comment's numbers are the VU0
// program's own instruction indices, which the host reproduces step for step: the place's w is the life left in
// seconds, the frame's eight half words are the surface's two axes at 32768 to the unit, and the variant's keys are
// four colours then four extents (each key's w its time).
void AgeDecalKeys(const f32* place, const s32* frame, const f32* variant, f32* aged, DecalLook* look)
{
    const f32* colours = variant;
    const f32* sizes = variant + 16;

    // 56: the second size key's time; 57, 59 and 63: the life as a share of it, three times the rest of it
    const f32 key1 = sizes[4 + 3];
    f32 w = place[3] * key1;
    w = 1.0f - w;
    w = w * 3.0f;
    // 64: FTOI0 (towards zero); 68: the fraction after it. The index is held to the four keys the variant has (a
    // life outside the keys' range would read past them on the PS2), and the last key HOLDS: past it there is no
    // fourth-to-fifth pair to mix, and the pair's "next" pointers would land on the variant's other block -- index 3's
    // colB is the first size key read as a colour, and its sizB is the NEXT variant's first colour key read as a size.
    // Those are 0..255 floats where a size belongs, so the quad's two axes stop being a surface frame at all and the
    // ripple flips (a roll, since the two garbage axes no longer sit at right angles) for the last of its life. Retail
    // hides that behind the last colour key's alpha of 0; reading the wrong key gave it a colour and a size instead,
    // which is the flicker at the end of the animation
    s32 index = static_cast<s32>(w);
    index = index < 0 ? 0 : (index > 3 ? 3 : index);
    const f32 frac = index >= 3 ? 0.0f : w - static_cast<f32>(index);

    const f32* colA = colours + index * 4;
    const f32* colB = colA + 4;
    const f32* sizA = sizes + index * 4;
    const f32* sizB = sizA + 4;

    // 74, 75, 76: the colour key mixed with the next, then 80: FTOI0 (the game keeps the low byte)
    for (u32 channel = 0; channel < 4; channel++)
    {
        const f32 mixed = (colA[channel] - colA[channel] * frac) + colB[channel] * frac;
        look->colour[channel] = static_cast<u8>(Ftoi0(mixed));
    }

    // 71, 72, 73: the size key's x and z mixed with the next (the keys' y is unused)
    const f32 sizeX = (sizA[0] - sizA[0] * frac) + sizB[0] * frac;
    const f32 sizeZ = (sizA[2] - sizA[2] * frac) + sizB[2] * frac;

    // 69, 70: the frame's half words as floats; 78, 79: the two edges of the decal's quad, along the surface's
    // normal by the size's x and along its direction by the size's z
    f32 alongNormal[4];
    f32 alongDirection[4];
    for (u32 channel = 0; channel < 4; channel++)
    {
        alongNormal[channel] = Itof15(frame[channel]) * sizeX;
        alongDirection[channel] = Itof15(frame[4 + channel]) * sizeZ;
    }

    // 82, 83 and 84, 87: the two edges' ends at half and three times the sizes (12.4 fixed point: 92 and 94 are the
    // FTOI12s the block's half words are the low bits of), and 107: the first set's fourth word is the marker 240
    for (u32 channel = 0; channel < 4; channel++)
    {
        look->moreSizes[channel] = static_cast<s16>(Ftoi12(alongNormal[channel] * 0.5f + alongDirection[channel] * 0.5f));
        if (channel < 3)
        {
            look->sizes[channel] = static_cast<s16>(Ftoi12(alongNormal[channel] * -0.5f + alongDirection[channel] * 3.0f));
        }
    }

    look->sizes[3] = 240;
    // 106, 107: the place itself, and its w as one (the game keeps the life in the block's own field)
    aged[0] = place[0];
    aged[1] = place[1];
    aged[2] = place[2];
    aged[3] = 1.0f;
}

void AgeDecal(const f32* place, const s32* frame, s32 variant, DecalLook* look)
{
    for (u32 value = 0; value < 4; value++)
    {
        look->place[value] = 0.0f;
        look->colour[value] = 0;
        look->sizes[value] = 0;
        look->moreSizes[value] = 0;
    }

    if (g_decalType == nullptr || variant < 0 || variant >= g_decalType->variantCount ||
        variant >= static_cast<s32>(DecalType::MostVariants))
    {
        return;
    }

    // The variants as the PS2's VU0 memory has them: DecalVariant is 0x80 bytes, four colour keys then four size
    // keys, which is what LoadDecalType copies there
    const f32* data = reinterpret_cast<const f32*>(&g_decalType->variants[variant]);
    if (g_decalOtherChunk)
    {
        // The other chunk's entry (the program at 0x360, instructions 111 to 115): the place through that chunk's
        // matrix (its rows 0 to 2 with the translation in row 3, the point's w of one)
        const f32* matrix = &g_decalFromChunkMatrix.m[0][0];
        f32 moved[4];
        for (u32 column = 0; column < 3; column++)
        {
            moved[column] = place[0] * matrix[0 * 4 + column] + place[1] * matrix[1 * 4 + column] +
                            place[2] * matrix[2 * 4 + column] + matrix[3 * 4 + column];
        }

        moved[3] = place[3];
        AgeDecalKeys(moved, frame, data, look->place, look);
        return;
    }

    AgeDecalKeys(place, frame, data, look->place, look);
}

// The type a block belongs to: the game keeps a decal's type out of its block (the uv packet's rectangles are per
// type), so the block's type list is searched for it
u32 DecalTypeOf(const DecalData* decals, const DecalBlock* block)
{
    for (u32 type = 0; type < DecalData::MostTypes; type++)
    {
        for (const DecalBlock* at = decals->typeBlocks[type]; at != nullptr; at = at->next)
        {
            if (at == block)
            {
                return type;
            }
        }
    }

    return 0;
}

// A block of a key's decals drawn: one quad's two triangles a decal, through the page's material for that key (the
// PS2's DrawDecals gives VU1 the same list and draws it with decals->page.materials[key])
bool SubmitDecals(DecalData* decals, u32 key, const StqVertex* vertices, u32 count)
{
    if (count == 0 || key >= DecalData::KeyCount)
    {
        return false;
    }

    auto* hostMaterial = reinterpret_cast<HostMaterial*>(decals->page.materials[key]);
    if (hostMaterial == nullptr)
    {
        return false;
    }

    if (hostMaterial->program == HostInvalidHandle)
    {
        return false;
    }

    MakeShared();
    if (!bgfx::getAvailTransientVertexBuffer(count, StqLayout()))
    {
        return false;
    }

    bgfx::TransientVertexBuffer buffer;
    bgfx::allocTransientVertexBuffer(&buffer, count, StqLayout());
    std::memcpy(buffer.data, vertices, count * sizeof(StqVertex));

    bgfx::TextureHandle texture = HostMaterialBoundTexture(hostMaterial);
    if (!bgfx::isValid(texture))
    {
        texture = HostWhiteTexture();
    }

    bgfx::setViewMode(HostDecalsView, bgfx::ViewMode::Sequential);
    HostSceneSetView(HostDecalsView);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LEQUAL |
                   ParticleBlendState(hostMaterial->blend));
    bgfx::setTexture(0, g_stqSampler, texture,
                     BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | HostMaterialFilter(hostMaterial));
    bgfx::setVertexBuffer(0, &buffer);
    bgfx::submit(HostDecalsView, g_stqProgram);
    return true;
}

void DrawDecals(DecalData* decals)
{
    if (decals == nullptr || decals->count == 0)
    {
        if (decals != nullptr)
        {
            decals->chunk = nullptr;
        }

        g_decalDebug.vertices = 0;
        g_decalDebug.checksum = 0;
        return;
    }

    // The world to screen matrix the PS2's DrawDecals hands VU1 (the view's own; the identity when no view stands,
    // which is what a headless proof without game data gets)
    Matrix4x4 toScreen;
    std::memset(&toScreen, 0, sizeof(toScreen));
    for (u32 row = 0; row < 4; row++)
    {
        toScreen.m[row][row] = 1.0f;
    }

    if (G_RendRel != nullptr)
    {
        auto* view = *reinterpret_cast<RenderView**>(G_RendRel);
        if (view != nullptr)
        {
            toScreen = view->toScreen;
        }
    }

    // Every key's list, a type material at a time (the PS2's DrawDecals: the list is emptied as it is drawn)
    std::vector<StqVertex> vertices;
    vertices.reserve(decals->count * 4);
    for (u32 key = 0; key < DecalData::KeyCount; key++)
    {
        DecalBlock* block = decals->drawLists[key];
        decals->drawLists[key] = nullptr;
        for (; block != nullptr; block = block->drawNext)
        {
            const u32 type = DecalTypeOf(decals, block);
            // The packet keeps two rows a type: the rectangle's first edge (its left and right ends, in S and T) and
            // its second. The default data's type 0 is {0.25, 0.25, 0.75, 0.25} and {0.25, 0.75, 0.75, 0.75} --
            // the top and bottom edges of a square -- not one rectangle's left, top, right and bottom: reading only
            // rectangles[type] (as this did) made both rows of a type's T equal, so every decal sampled one line of
            // the page. Water's wake and footprints were the visible loss; the UVs now follow the two rows
            const Vector4* edges = decals->uvPacket.rectangles + type * 2;
            const u32 blockFirst = static_cast<u32>(vertices.size());
            const s32 count = block->count;
            for (s32 slot = 0; slot < count; slot++)
            {
                const DecalPlace& place = block->places[slot];
                // The aging's two sets of half words are the quad's two edges from the place, in the 4.12 fixed point
                // FTOI12 makes (AgeDecalKeys' sizes, a multiple of 4096: the VU0's 12 fractional bits, of which the
                // halfword keeps what fits), so they are 1/4096 of the chunk's units: the first runs along the
                // texture's S, the second along its T. Reading them as 12.4 (a multiple of 16, as this did) drew every
                // decal 256 times its size -- the water's wake as a 400 unit sheet that the near clip threw away
                const s16* size = block->sizes[slot];
                const s16* more = block->moreSizes[slot];
                const u32 colour = block->colours[slot];
                // The aging packs two linear combinations of the surface's own axes (AgeDecalKeys, mirroring the VU0
                // program): sizes = -0.5N + 3D and moreSizes = 0.5N + 0.5D, N the surface normal times the variant's
                // first size key and D its direction times the second. The second recovers both extents on its own, by
                // projecting it on the frame the block already holds:
                //     sizeX = 2 * (moreSizes . normal),   sizeZ = 2 * (moreSizes . direction)
                // and it is the safer half to read. The first set's 3D term passes the +-8 units a 12.4 half word
                // holds at a direction extent of only 2.67, and a ripple grows past that: the word wraps, the two axes
                // stop being perpendicular and a growing circle turns into a roll. That is what the life trace showed
                // -- sizes stuck at -25651 (-6.26 units, 78% of the field) while the ripple was still half alive
                constexpr f32 DecalSizeUnit = 4096.0f;
                constexpr f32 DecalFrameUnit = 32768.0f;
                const DecalFrame& frame = block->frames[slot];
                const f32 frameNormal[3] = {static_cast<f32>(frame.normal[0]) / DecalFrameUnit,
                                            static_cast<f32>(frame.normal[1]) / DecalFrameUnit,
                                            static_cast<f32>(frame.normal[2]) / DecalFrameUnit};
                const f32 frameDirection[3] = {static_cast<f32>(frame.direction[0]) / DecalFrameUnit,
                                               static_cast<f32>(frame.direction[1]) / DecalFrameUnit,
                                               static_cast<f32>(frame.direction[2]) / DecalFrameUnit};
                const f32 moreUnits[3] = {static_cast<f32>(more[0]) / DecalSizeUnit,
                                          static_cast<f32>(more[1]) / DecalSizeUnit,
                                          static_cast<f32>(more[2]) / DecalSizeUnit};
                const f32 extentX = 2.0f * (moreUnits[0] * frameNormal[0] + moreUnits[1] * frameNormal[1] +
                                            moreUnits[2] * frameNormal[2]);
                const f32 extentZ = 2.0f * (moreUnits[0] * frameDirection[0] + moreUnits[1] * frameDirection[1] +
                                            moreUnits[2] * frameDirection[2]);

                // Half an edge each way along the frame itself, so the quad is a rectangle of the surface's own axes
                // whatever the two extents are (a ripple's are equal; a footprint's need not be)
                const f32 alongU[3] = {frameNormal[0] * extentX * 0.5f, frameNormal[1] * extentX * 0.5f,
                                       frameNormal[2] * extentX * 0.5f};
                const f32 alongV[3] = {frameDirection[0] * extentZ * 0.5f, frameDirection[1] * extentZ * 0.5f,
                                       frameDirection[2] * extentZ * 0.5f};
                // Each corner is projected on its own: the decal lies in the world (its place and its two edges are
                // in the chunk's space), so its quad is a projection of four points, and the GS's STQ coordinates
                // are what kept its texture on them (the texture fractions are scaled by the clip w the divide by Q
                // in the shader undoes)
                //
                // The square's corners, round its perimeter: place (-N -D), (+N -D), (+N +D), (-N +D), each held half
                // an edge from the place. The quad is centred on the place (retail's program adds and subtracts an
                // edge either way, source/vu1 0x13's ADD/SUB pair), which is what puts a ripple on the water the
                // decal belongs to rather than beside it
                static const f32 CornerSigns[4][2] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
                StqVertex quad[4];
                bool visible = true;
                for (u32 corner = 0; corner < 4 && visible; corner++)
                {
                    const f32 useU = CornerSigns[corner][0];
                    const f32 useV = CornerSigns[corner][1];
                    const f32 local[3] = {place.x + alongU[0] * useU + alongV[0] * useV,
                                          place.y + alongU[1] * useU + alongV[1] * useV,
                                          place.z + alongU[2] * useU + alongV[2] * useV};
                    f32 clip[4];
                    for (u32 column = 0; column < 4; column++)
                    {
                        clip[column] = local[0] * toScreen.m[0][column] + local[1] * toScreen.m[1][column] +
                                       local[2] * toScreen.m[2][column] + toScreen.m[3][column];
                    }

                    visible = clip[3] >= ParticleNearClip;
                    if (!visible)
                    {
                        break;
                    }

                    // The corner in the game's screen coordinates, then bgfx's clip space (Y flips)
                    const f32 scaleX = 2.0f / (static_cast<f32>(HostDisplayWidth()) * GsUnitsPerPixel);
                    const f32 scaleY = 2.0f / (static_cast<f32>(HostDisplayHeight()) * GsUnitsPerPixel);
                    const f32 x = (clip[0] - GsScreenMiddleUnits * clip[3]) * scaleX;
                    const f32 y = (GsScreenMiddleUnits * clip[3] - clip[1]) * scaleY;
                    // The page's rectangle: the type's two rows are its top and bottom edges (S low to high at the
                    // top, then at the bottom), so its corners are (S0, T0) (S1, T0) (S1, T1) (S0, T1). The quad's
                    // four corners are the rhombus' diagonals, but the texture runs along the *edges* between them
                    // (+U to +V is one edge, +V to -U the next), so each corner takes the rectangle's own corner:
                    // putting the rectangle's mid-edges on the diagonals instead folded the page across them, which
                    // drew a round ripple as a four-pointed crystal
                    const f32 s0 = edges[0].x;
                    const f32 s1 = edges[0].z;
                    const f32 t0 = edges[0].y;
                    const f32 t1 = edges[1].y;
                    f32 u = corner == 1 || corner == 2 ? s1 : s0;
                    f32 v = corner >= 2 ? t1 : t0;

                    // The corner's depth. toScreen's z row is the GS's own (frame.cpp's clipToScreen puts -GsDepthRange
                    // on the diagonal and +GsDepthRange in the translation), so clip[2] is a GS depth in the millions
                    // and not a clip z at all. The particle shader inverts that same transform (vs_particle.sc:
                    // depth = w - gsZ / range), which is its own exact inverse: w - range*(w - z)/range = z
                    const f32 clipDepth = clip[3] - clip[2] / GsDepthRange;
                    quad[corner] = {x, y, clipDepth, u * clip[3], v * clip[3], clip[3], colour};
                }

                if (!visible)
                {
                    continue;
                }

                // The quad's two triangles (0, 1, 2) and (0, 2, 3), the order its corners are in (so neighbouring
                // decals stay apart in one key's buffer)
                static const u32 TriangleOrder[6] = {0, 1, 2, 0, 2, 3};
                for (u32 vertex = 0; vertex < 6; vertex++)
                {
                    vertices.push_back(quad[TriangleOrder[vertex]]);
                }
            }

            SubmitDecals(decals, key, vertices.data() + blockFirst, static_cast<u32>(vertices.size()) - blockFirst);
        }
    }

    g_decalDebug.vertices = static_cast<u32>(vertices.size());
    g_decalDebug.checksum =
        vertices.empty() ? 0u : HashBytes(5381u, vertices.data(), vertices.size() * sizeof(StqVertex));
}
}
