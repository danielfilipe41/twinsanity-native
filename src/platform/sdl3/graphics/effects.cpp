// Particle-side effects: the wave shader (type 0x1C, src/platform/ps2/renderer/effects.cpp) and the distortion
// hexagons (type 0x18, which copy the frame and then sample the copy; src/platform/ps2/renderer/materials.cpp).
// The wave starts at start-up but g_WaveEffect stays 0, so only the distortion draws. The screen-effects filter
// lives in screenfx.cpp.
#include "host_graphics.h"
#include "draws_host.h"
#include "particle_billboard.h"

#include "game/math.h"
#include "game/particles.h"

#include "shaders/fs_stq.bin.h"
#include "shaders/vs_stq.bin.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace Platform::Graphics
{
namespace
{
// The distortion's constants (src/platform/ps2/renderer/particles.cpp's DrawHexagonParticleBlock): the system's
// across and down the VU1 is given, and the depth the hexagons fade out to nothing at
constexpr f32 DistortionAcross = 0x1p-14f;
constexpr f32 DistortionDown = -0x1p-12f;
constexpr f32 DistortionFadeDepth = 40.0f;
// The hexagons' six corners (BuildParticleRenderTable's HexagonReach: their directions are a quarter long), drawn as
// six triangles around the centre (the console's VU1, TT-Lab's HexagonVertices)
constexpr u32 HexagonCorners = 6;
constexpr u32 HexagonVertices = 18;
// The game's screen coordinates (renderer.h's GsScreenMiddleFloat with its 16 subpixels)
constexpr f32 GsScreenMiddleUnits = 2048.0f * 16.0f;
constexpr f32 GsUnitsPerPixel = 16.0f;
// The clip w below which VU1 collapses a particle
constexpr f32 ParticleNearClip = 0.5f;
// How many steps a particle's life has (Platform::ParticleRenderSteps)
constexpr u32 ParticleSteps = 64;

// The wave shader's state (the PS2's WaveShader: sixteen rows of three phases, their sines, the speed in turns a
// second and the amplitude)
constexpr f32 FiveTurns = 0x1.F6A7A4p+4f;
constexpr f32 TenTurns = 0x1.F6A7A4p+5f;
constexpr f32 TwoPi = 6.2831855f;

struct HostWave
{
    f32 phases[HostWaveRows][3];
    f32 sines[HostWaveRows][3];
    f32 speed;
    f32 amplitude;
};

HostWave g_wave = {{}, {}, 0.0f, 0.0f};

// The distortion's program and its frame copy (the process owns them; made lazily after BgfxInit)
bgfx::ProgramHandle g_stqProgram = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_stqSampler = BGFX_INVALID_HANDLE;
bgfx::TextureHandle g_frameCopy = BGFX_INVALID_HANDLE;
u32 g_frameCopyWidth = 0;
u32 g_frameCopyHeight = 0;

// The distortion's vertices (the headless proof's reading)
u32 g_distortionVertices = 0;
u32 g_distortionChecksum = 0;

// The vertices the decals' and the distortion's program takes: clip-space positions, S, T and Q, and the colour
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
    if (bgfx::isValid(g_stqProgram))
    {
        return;
    }
    bgfx::ShaderHandle vs = bgfx::createShader(bgfx::makeRef(vs_stq_glsl, sizeof(vs_stq_glsl)));
    bgfx::ShaderHandle fs = bgfx::createShader(bgfx::makeRef(fs_stq_glsl, sizeof(fs_stq_glsl)));
    g_stqProgram = bgfx::createProgram(vs, fs, true);
    g_stqSampler = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);
}

u32 HashBytes(u32 hash, const void* bytes, size_t size)
{
    auto* at = static_cast<const u8*>(bytes);
    for (size_t index = 0; index < size; index++)
    {
        hash = (hash ^ at[index]) * 16777619u;
    }

    return hash;
}

// A block's host header (particles_host.cpp's): its magic says the block is one of ours
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

// The chain's next block (the PS2's DMA NEXT tag)
u8* NextBlock(const HostParticleBlockHead* head)
{
    return (head->tag[0] & 0x70000000) == 0x20000000 ? reinterpret_cast<u8*>(static_cast<uintptr_t>(head->tag[1]))
                                                     : nullptr;
}
}

// The wave shader (the PS2's type 0x1C: InitParticleGraphics makes it, UpdateParticleWaves moves it on)

void HostParticleWaveInit(f32 speed, f32 amplitude)
{
    std::memset(&g_wave, 0, sizeof(g_wave));
    g_wave.speed = speed;
    g_wave.amplitude = amplitude;
}

void HostParticleWaveUpdate(f32 seconds)
{
    // The PS2's ShaderType1CUpdate: the phase is turned by the speed over the seconds (in radians), wrapped at ten
    // pi into the next twenty (five turns into ten), and the wave is its sine
    const f32 step = g_wave.speed * (seconds * TwoPi);
    for (u32 row = 0; row < HostWaveRows; row++)
    {
        for (u32 phase = 0; phase < 3; phase++)
        {
            f32 value = g_wave.phases[row][phase] + step;
            if (FiveTurns <= value)
            {
                value = value - TenTurns;
            }

            g_wave.phases[row][phase] = value;
            g_wave.sines[row][phase] = std::sin(value);
        }
    }
}

f32 HostParticleWaveAmplitude()
{
    return g_wave.amplitude;
}

void HostParticleWaveSines(f32* out)
{
    if (out == nullptr)
    {
        return;
    }

    for (u32 row = 0; row < HostWaveRows; row++)
    {
        for (u32 phase = 0; phase < 3; phase++)
        {
            out[row * 3 + phase] = g_wave.sines[row][phase];
        }
    }
}

// The distortion's frame copy (the PS2's type 0x18: the shader copies the frame first, then the hexagons sample it)

void HostDistortionFrameCopy(u32 width, u32 height, const void* rgba, u32 pitch)
{
    if (width == 0 || height == 0 || rgba == nullptr || pitch < width * 4)
    {
        return;
    }

    if (bgfx::isValid(g_frameCopy) && (width != g_frameCopyWidth || height != g_frameCopyHeight))
    {
        bgfx::destroy(g_frameCopy);
        g_frameCopy = BGFX_INVALID_HANDLE;
    }

    if (!bgfx::isValid(g_frameCopy))
    {
        g_frameCopy = bgfx::createTexture2D(static_cast<u16>(width), static_cast<u16>(height), false, 1,
                                            bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_NONE);
        if (!bgfx::isValid(g_frameCopy))
        {
            return;
        }

        g_frameCopyWidth = width;
        g_frameCopyHeight = height;
    }

    // The rows may be padded, so they go up one at a time
    auto* rows = static_cast<const u8*>(rgba);
    const u32 rowBytes = width * 4;
    for (u32 row = 0; row < height; row++)
    {
        const bgfx::Memory* memory = bgfx::copy(rows + static_cast<size_t>(row) * pitch, rowBytes);
        bgfx::updateTexture2D(g_frameCopy, 0, 0, 0, static_cast<u16>(row), static_cast<u16>(width), 1, memory);
    }
}

// The scene target copied into g_frameCopy (the game's own frame is the offscreen scene now): the PS2's type 0x18
// copies the frame every draw, so the hexagons sample a still of the scene rather than reading what they write.
// The copy rides the DISTORTION's own view (23): bgfx runs a view's blits before its draws, so the still the
// hexagons sample holds every pass that drew before them (blended, decals, shadows, particles), which is what
// retail's copy sees. At an earlier view the hexagons would read a copy missing those passes
constexpr bgfx::ViewId HostDistortionCopyView = 23;
void EnsureFrameCopyFromScene(u32 width, u32 height)
{
    bgfx::TextureHandle scene = Platform::Graphics::HostSceneColour();
    if (!bgfx::isValid(scene) || width == 0 || height == 0)
    {
        return;
    }

    if (bgfx::isValid(g_frameCopy) && (width != g_frameCopyWidth || height != g_frameCopyHeight))
    {
        bgfx::destroy(g_frameCopy);
        g_frameCopy = BGFX_INVALID_HANDLE;
    }

    if (!bgfx::isValid(g_frameCopy))
    {
        g_frameCopy = bgfx::createTexture2D(static_cast<u16>(width), static_cast<u16>(height), false, 1,
                                            bgfx::TextureFormat::RGBA8,
                                            BGFX_TEXTURE_BLIT_DST | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        if (!bgfx::isValid(g_frameCopy))
        {
            return;
        }

        g_frameCopyWidth = width;
        g_frameCopyHeight = height;
    }

    bgfx::setViewRect(HostDistortionCopyView, 0, 0, static_cast<u16>(width), static_cast<u16>(height));
    bgfx::blit(HostDistortionCopyView, {.handle = g_frameCopy}, {.handle = scene});
}

bgfx::TextureHandle HostDistortionFrameTexture()
{
    return g_frameCopy;
}

// The distorting hexagons (MakeDistortionMaterial, DrawDistortionBlock)

// The material the game keeps for the distorting hexagons (src/platform/ps2/renderer/materials.cpp's
// MakeDistortionMaterial): a shader of its own in the distortion's bucket that copies the frame first, blended with
// the mix preset, testing depth GREATER and writing none. On the host the copy is the frame-copy texture the
// hexagons sample (HostDistortionFrameCopy)
void MakeDistortionMaterial(Material* material)
{
    MakeShared();
    ConstructMaterial(material);
    auto* host = reinterpret_cast<HostMaterial*>(material);
    host->program = g_stqProgram.idx;
    host->flags = HostMaterialTextured;
    host->blend = HostParticleBlendPage;
    // The distortion's bucket, as retail's MakeDistortionMaterial names it (materials.cpp: BucketDistortion)
    host->bucket = 23;
    // The hexagons sample the frame copy, which the draw binds itself (the PS2's type 0x18 copies the frame into
    // the material's texture every draw): the handle stays invalid so MakeDistortionMaterial does not shadow that
    // copy with the white texture (a bound 1x1 white would pass bgfx::isValid and the draw's frame-copy fallback
    // would never run, leaving the distortion sampling flat white instead of the frame)
    host->texture = HostInvalidHandle;
    host->textureWidth = 1;
    host->textureHeight = 1;
}

// One hexagon: the particle's place in its own units, its six rim offsets (in the screen's coordinates, like the
// particle quad's) and its colour. The hexagon's size is the particle quad's own width and height at that life step,
// so it is a screen-space shape like the quad and is projected with the centre
void HexagonAt(const ParticleRecord& record, const HostParticleTable& table, const Matrix4x4& matrix, f32 time,
               f32* centre /* 3 */, f32* offsets /* 6 x 2 */, f32* colour, f32* fade)
{
    // The same life the particles' draw reads: the step and the place its start, velocity and the gravity give
    const f32 age = time - record.spawnTime;
    const f32 life = age * record.lifeScale;
    if (record.lifeScale == 0.0f || !(life >= 0.0f) || life >= static_cast<f32>(ParticleSteps))
    {
        *fade = 0.0f;
        return;
    }

    u32 step = static_cast<u32>(life);
    if (step >= ParticleSteps)
    {
        step = ParticleSteps - 1;
    }

    // The whole of g*age*age, not half of it: the particle path and the game's own bounce solver use the same curve
    const f32 local[3] = {record.start[0] + record.velocity[0] * age,
                          record.start[1] + record.velocity[1] * age + table.gravity * age * age,
                          record.start[2] + record.velocity[2] * age};
    f32 clip[4];
    for (u32 column = 0; column < 4; column++)
    {
        clip[column] = local[0] * matrix.m[0][column] + local[1] * matrix.m[1][column] +
                       local[2] * matrix.m[2][column] + matrix.m[3][column];
    }

    if (!(clip[3] >= ParticleNearClip))
    {
        *fade = 0.0f;
        return;
    }

    // The quad's width and height axes at that step, in ten thousandths of a unit (BuildParticleRenderTable's shape:
    // the corners are twice the half extents and x already carries the display's aspect)
    constexpr f32 SizeUnit = ParticleSizeUnit;
    const f32* shape = table.steps[step].shape;
    const f32 width[2] = {(shape[4] - shape[2] - shape[0]) * SizeUnit * 0.5f,
                          (shape[5] - shape[3] - shape[1]) * SizeUnit * 0.5f};
    const f32 height[2] = {shape[2] * SizeUnit * 0.5f, shape[3] * SizeUnit * 0.5f};
    // The rim's six directions, a sixth of a turn apart, along those axes (TT-Lab's ParticleDistortion.vert), turned
    // from the sprite's own 2D plane onto the camera's axes: the hexagon is a billboard like the particle quad, so its
    // rim runs along the camera's right and up, not along the block's own axes. The offsets are in the screen's
    // coordinates, like the particle quad's (see particle_billboard.h)
    //
    // The axes are the shape's own, so they are the extents the shape spells out (twice the half extents the quad is
    // built with: the distortion's hexagon is the size of the puff it distorts)
    constexpr f32 Turn = 3.14159265f / 3.0f;
    f32 across = 0.0f;
    f32 down = 0.0f;
    ParticleScreenScale(&across, &down);
    const f32 aspectFix = ShapeScreenRatio(across, down);
    for (u32 corner = 0; corner < HexagonCorners; corner++)
    {
        const f32 angle = static_cast<f32>(corner) * Turn;
        const f32 sine = std::sin(angle);
        const f32 cosine = std::cos(angle);
        const f32 alongWidth = sine * width[0] + cosine * height[0];
        const f32 alongHeight = sine * width[1] + cosine * height[1];
        offsets[corner * 2] = alongWidth * aspectFix * across;
        offsets[corner * 2 + 1] = -alongHeight * down;
    }

    std::memcpy(centre, local, sizeof(local));

    // The GS's modulate: the colour's channels run 0 to 255 and the alpha 0 to 128 (the alpha register's range, and
    // the data's alpha keys run past it, so the alpha is held to what the GS takes)
    colour[0] = static_cast<f32>(table.steps[step].colour[0]) / 255.0f;
    colour[1] = static_cast<f32>(table.steps[step].colour[1]) / 255.0f;
    colour[2] = static_cast<f32>(table.steps[step].colour[2]) / 255.0f;
    const u32 alpha = table.steps[step].colour[3] < 128u ? table.steps[step].colour[3] : 128u;
    colour[3] = static_cast<f32>(alpha) / 128.0f;

    // The hexagons fade out with the depth (the PS2 hands VU1 the depth they are gone at)
    const f32 depth = clip[3];
    f32 amount = 1.0f - depth / DistortionFadeDepth;
    *fade = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount);
}

void DrawDistortionBlock(u8* block, u8* table, const Matrix4x4* matrix, Material* material, f32 time, f32 distortionX,
                         f32 distortionY)
{
    // The frame copy this draws from is ready when the hexagons draw: EnsureFrameCopyFromScene below queues the scene
    // copied into g_frameCopy in this same view (HostDistortionCopyView), and bgfx runs a view's blits before its
    // draws, so the still holds every pass that drew before the distortion
    const HostParticleTable* host = TableOf(table);
    if (host == nullptr || HeadOf(block) == nullptr || matrix == nullptr)
    {
        return;
    }

    // The system's distortion as the PS2 hands it to VU1 (the across and down in the frame copy's texture units)
    const f32 across = distortionX * DistortionAcross;
    const f32 down = distortionY * DistortionDown;

    // The frame copy the hexagons sample: the material's own texture when one is bound, else the scene copied into
    // g_frameCopy (fed from the scene target, the game's frame on the host). The copy is the SCENE's own size, not the
    // display's: bgfx clamps a blit to the smaller region, and the hexagons' UVs are screen fractions over the whole
    // scene, so a 512x512 copy of a 960x540 scene would leave them sampling a corner
    auto* hostMaterial = reinterpret_cast<HostMaterial*>(material);
    EnsureFrameCopyFromScene(Platform::Graphics::HostSceneWidth(), Platform::Graphics::HostSceneHeight());
    bgfx::TextureHandle texture = HostMaterialTexture(hostMaterial);
    if (!bgfx::isValid(texture))
    {
        texture = g_frameCopy;
    }

    if (!bgfx::isValid(texture))
    {
        texture = HostWhiteTexture();
    }

    const f32 scaleX = 2.0f / (static_cast<f32>(HostDisplayWidth()) * GsUnitsPerPixel);
    const f32 scaleY = 2.0f / (static_cast<f32>(HostDisplayHeight()) * GsUnitsPerPixel);

    std::vector<StqVertex> vertices;
    vertices.reserve(12 * HexagonVertices);
    for (u8* at = block; at != nullptr; at = NextBlock(HeadOf(at)))
    {
        const HostParticleBlockHead* blockHead = HeadOf(at);
        auto* records = reinterpret_cast<ParticleRecord*>(at + 0x20);
        for (u32 record = 0; record < blockHead->count && record < 32; record++)
        {
            f32 centre[3];
            f32 offsets[HexagonCorners * 2];
            f32 colour[4];
            f32 fade = 0.0f;
            HexagonAt(records[record], *host, *matrix, time, centre, offsets, colour, &fade);
            if (fade <= 0.0f)
            {
                continue;
            }

            // The centre and each rim point are projected on their own: the rim is in the screen's coordinates, like the
            // quads', so it has to go with the centre through the divide
            f32 centreClip[4];
            for (u32 column = 0; column < 4; column++)
            {
                centreClip[column] = centre[0] * matrix->m[0][column] + centre[1] * matrix->m[1][column] +
                                     centre[2] * matrix->m[2][column] + matrix->m[3][column];
            }

            if (!(centreClip[3] >= ParticleNearClip))
            {
                continue;
            }

            const f32 baseNdcX = (centreClip[0] / centreClip[3] - GsScreenMiddleUnits) * scaleX;
            const f32 baseNdcY = (GsScreenMiddleUnits - centreClip[1] / centreClip[3]) * scaleY;
            const u32 alpha = static_cast<u32>(colour[3] * fade * 255.0f);
            const u32 rgba = (static_cast<u32>(colour[0] * 255.0f) & 0xFF) |
                             ((static_cast<u32>(colour[1] * 255.0f) & 0xFF) << 8) |
                             ((static_cast<u32>(colour[2] * 255.0f) & 0xFF) << 16) | ((alpha & 0xFF) << 24);

            const StqVertex middle = {baseNdcX, baseNdcY, 0.0f, (baseNdcX + 1.0f) * 0.5f + across,
                                      (1.0f - baseNdcY) * 0.5f + down, 1.0f, rgba};
            StqVertex rim[HexagonCorners];
            bool visible = true;
            for (u32 corner = 0; corner < HexagonCorners && visible; corner++)
            {
                // The rim's point is the centre's place in the screen's coordinates with the rim's own offset along the
                // camera's right and up
                f32 clip[4] = {centreClip[0] + offsets[corner * 2], centreClip[1] + offsets[corner * 2 + 1],
                               centreClip[2], centreClip[3]};

                visible = clip[3] >= ParticleNearClip;
                if (!visible)
                {
                    break;
                }

                const f32 ndcX = (clip[0] / clip[3] - GsScreenMiddleUnits) * scaleX;
                const f32 ndcY = (GsScreenMiddleUnits - clip[1] / clip[3]) * scaleY;
                rim[corner] = {ndcX, ndcY, 0.0f, (ndcX + 1.0f) * 0.5f + across, (1.0f - ndcY) * 0.5f + down,
                               1.0f, rgba};
            }

            if (!visible)
            {
                continue;
            }

            // Six triangles around the centre, as the console's VU1 draws them (TT-Lab's HexagonVertices is 18)
            for (u32 corner = 0; corner < HexagonCorners; corner++)
            {
                vertices.push_back(middle);
                vertices.push_back(rim[corner]);
                vertices.push_back(rim[(corner + 1) % HexagonCorners]);
            }
        }
    }

    g_distortionVertices = static_cast<u32>(vertices.size());
    g_distortionChecksum =
        vertices.empty() ? 0u : HashBytes(5381u, vertices.data(), vertices.size() * sizeof(StqVertex));

    if (vertices.empty() || hostMaterial == nullptr || hostMaterial->program == HostInvalidHandle ||
        !bgfx::getAvailTransientVertexBuffer(static_cast<u32>(vertices.size()), StqLayout()))
    {
        return;
    }

    MakeShared();
    bgfx::TransientVertexBuffer buffer;
    bgfx::allocTransientVertexBuffer(&buffer, static_cast<u32>(vertices.size()), StqLayout());
    std::memcpy(buffer.data, vertices.data(), vertices.size() * sizeof(StqVertex));

    bgfx::setViewMode(HostDistortionView, bgfx::ViewMode::Sequential);
    Platform::Graphics::HostSceneSetView(HostDistortionView);
    // Triangles are the default primitive (only the strips, lines and points have state flags). The depth test is the
    // GS's ZTST GREATER, whose host mirror is LESS (material_system.cpp's DepthState: the two depth conventions run the
    // other way about); GREATER tested "farther than the scene", and with the vertices at z 0 (nearest) it never passed
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_BLEND_ALPHA);
    // The distortion's texture is a screen copy: the reference's screen-copy settings clamp it (MakeScreenCopySettings)
    // and filter it bilinearly (the retail's screen copy writes Bilinear into the GS's TEX1)
    bgfx::setTexture(0, g_stqSampler, texture,
                     BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_MIP_POINT);
    bgfx::setVertexBuffer(0, &buffer);
    bgfx::submit(HostDistortionView, g_stqProgram);
}

u32 HostFxDebug_DistortionVertices()
{
    return g_distortionVertices;
}

u32 HostFxDebug_DistortionChecksum()
{
    return g_distortionChecksum;
}

// The particles' start-up (src/platform/ps2/renderer/particles.cpp's InitParticleGraphics): the effects' disk node
// none, the wave shader with its speed and amplitude, and the distortion's material. The wave itself is moved on
// every frame, not here: StepAnimations calls HostParticleWaveUpdate with the frame's seconds (the retail's
// UpdateParticleWaves)
void InitParticleGraphics()
{
    constexpr f32 WaveSpeed = 1.0f;
    constexpr f32 WaveAmplitude = 1.5f;
    HostParticleWaveInit(WaveSpeed, WaveAmplitude);
}
}
